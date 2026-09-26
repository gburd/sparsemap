import pathlib
import sys

import pytest
from sparsemap import SparseMap

# set definitions live beside the emit tool so both stay in sync
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "ci"))
import wire_emit

@pytest.mark.parametrize("name", sorted(wire_emit.SETS))
def test_python_reads_c_bytes(fixtures_dir, name):
    m = SparseMap.from_bytes((fixtures_dir / f"{name}.bin").read_bytes())
    assert list(m) == sorted(wire_emit.SETS[name])

def test_python_reads_c_empty(fixtures_dir):
    m = SparseMap.from_bytes((fixtures_dir / "empty.bin").read_bytes())
    assert len(m) == 0

@pytest.mark.parametrize("name", sorted(wire_emit.SETS))
def test_roundtrip(name):
    m = SparseMap(wire_emit.SETS[name])
    assert SparseMap.from_bytes(m.to_bytes()) == m


def test_python_decodes_c_small_mode(fixtures_dir):
    # {0} is serialized by the fixed C library in SMALL mode: the wire
    # header byte out[6] is the small-set marker, and the body's first
    # u64 has its top bit set.  The Python binding must decode it to
    # exactly {0}.
    raw = (fixtures_dir / "small_zero.bin").read_bytes()
    assert raw[6] == 1, "C did not emit small-mode for {0}"
    assert raw[16 + 7] & 0x80, "body top bit (SM_SMALL_FLAG) not set"
    m = SparseMap.from_bytes(raw)
    assert list(m) == [0]


def test_python_decodes_c_rle_chunk(fixtures_dir):
    # {2048..=4095} is a single RLE chunk not starting at 0.
    m = SparseMap.from_bytes((fixtures_dir / "rle_run_2048_4095.bin").read_bytes())
    assert list(m) == list(range(2048, 4096))
