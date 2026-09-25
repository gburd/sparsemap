"""from_bytes must reject hostile serialized buffers with a Python
exception rather than returning a corrupt map.

These reproduce the 2026-09 Rust decoder fuzzing findings at the binding
layer: a buffer with an overflowing / overlapping / misaligned chunk
span used to decode into a map that iterates out of order in the release
wheel.  DecodeError now maps to ValueError.
"""

import os
import pathlib
import struct

import pytest
from sparsemap import SparseMap

MAGIC = 0x30316D73
VERSION = 2
FLAG_LE = 0x01
CHUNK_BITS = 2048
RLE_FLAG = 0b01 << 62
SPAN31 = 0x7FFFFFFF


def _header(count: int) -> bytes:
    return (
        struct.pack("<I", MAGIC)
        + bytes([VERSION, FLAG_LE, 0, 0])
        + struct.pack("<Q", 0)  # cardinality hint (recomputed on read)
        + struct.pack("<Q", count)
    )


def _rle_desc(cap: int, length: int) -> int:
    return RLE_FLAG | ((cap & SPAN31) << 31) | (length & SPAN31)


def _sparse_all_ones() -> int:
    d = 0
    for i in range(32):
        d |= 0b11 << (2 * i)
    return d


def _chunk(start: int, desc: int) -> bytes:
    return struct.pack("<Q", start) + struct.pack("<Q", desc)


TOP_BASE = (2**64) - CHUNK_BITS

HOSTILE = {
    "rle_length_exceeds_capacity": _header(1)
    + _chunk(0, _rle_desc(CHUNK_BITS, CHUNK_BITS * 2)),
    "unaligned_chunk_start": _header(1)
    + _chunk(100, _rle_desc(CHUNK_BITS, CHUNK_BITS)),
    "start_plus_capacity_overflow": _header(1)
    + _chunk(TOP_BASE, _rle_desc(CHUNK_BITS * 2, CHUNK_BITS * 2)),
    "overlapping_spans": _header(2)
    + _chunk(0, _rle_desc(CHUNK_BITS * 2, CHUNK_BITS * 2))
    + _chunk(CHUNK_BITS, _sparse_all_ones()),
    "non_ascending_starts": _header(2)
    + _chunk(CHUNK_BITS, _sparse_all_ones())
    + _chunk(0, _sparse_all_ones()),
    "duplicate_starts": _header(2)
    + _chunk(CHUNK_BITS, _sparse_all_ones())
    + _chunk(CHUNK_BITS, _sparse_all_ones()),
    "zero_capacity_rle": _header(1) + _chunk(0, _rle_desc(0, 0)),
}


@pytest.mark.parametrize("name", sorted(HOSTILE))
def test_hostile_buffer_raises(name):
    with pytest.raises(ValueError):
        SparseMap.from_bytes(HOSTILE[name])


def _crash_corpus():
    # The fuzzing crash corpus lives outside the package tree and is not
    # committed; use it when available (CI/local review), else skip.
    env = os.environ.get("SPARSEMAP_RUST_CRASH_DIR")
    candidates = []
    if env:
        candidates.append(pathlib.Path(env))
    candidates.append(
        pathlib.Path(__file__).resolve().parents[3]
        / ".scratch"
        / "review-repro"
        / "rust-crash"
    )
    for d in candidates:
        if d.is_dir():
            return sorted(d.glob("crash-*"))
    return []


def test_crash_corpus_never_returns_corrupt_map():
    files = _crash_corpus()
    if not files:
        pytest.skip("rust-crash corpus not present")
    for f in files:
        data = f.read_bytes()
        try:
            m = SparseMap.from_bytes(data)
        except ValueError:
            continue  # rejected, as required
        # If it decoded, the map must be sound: ascending and stable.
        bits = list(m)
        assert bits == sorted(bits), f"{f.name}: non-ascending members"
        assert len(set(bits)) == len(bits), f"{f.name}: duplicate members"
        assert SparseMap.from_bytes(m.to_bytes()) == m, f"{f.name}: unstable round-trip"
