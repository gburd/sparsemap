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
