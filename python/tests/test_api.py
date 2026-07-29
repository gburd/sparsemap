import pytest
from sparsemap import SparseMap

# mirrors tests/test_large_index.c so oversized indices stay covered
LARGE_KEYS = [
    2**32 - 1,
    2**32,
    2**32 + 1,
    0x400000000000,
    0x9C4000003039,
    0xFFFFFFFFFFFFF800,
]

def test_insert_contains_remove():
    m = SparseMap()
    assert m.insert(42)
    assert not m.insert(42)
    assert 42 in m
    assert m.remove(42)
    assert not m.remove(42)
    assert 42 not in m

def test_constructor_from_iterable():
    m = SparseMap([3, 1, 4, 1, 5])
    assert list(m) == [1, 3, 4, 5]

def test_len_bool_cardinality():
    m = SparseMap()
    assert len(m) == 0 and not m
    m.insert(7)
    assert len(m) == 1 and m.cardinality() == 1 and bool(m)

def test_clear():
    m = SparseMap([1, 2, 3])
    m.clear()
    assert len(m) == 0

def test_min_max():
    assert SparseMap().min() is None
    assert SparseMap().max() is None
    m = SparseMap([5, 9, 2048])
    assert (m.min(), m.max()) == (5, 2048)

def test_rank_strictly_below():
    m = SparseMap([1, 3, 4])
    assert m.rank(0) == 0
    assert m.rank(4) == 2
    assert m.rank(5) == 3

def test_select_zero_based():
    m = SparseMap([10, 20, 30])
    assert m.select(0) == 10
    assert m.select(2) == 30
    assert m.select(3) is None

def test_insert_range_half_open():
    m = SparseMap()
    m.insert_range(10, 20)
    assert len(m) == 10
    assert 10 in m and 19 in m and 20 not in m

def test_remove_range_half_open():
    m = SparseMap()
    m.insert_range(0, 100)
    m.remove_range(10, 90)
    assert list(m) == list(range(10)) + list(range(90, 100))

def test_long_run_is_cheap():
    m = SparseMap()
    m.insert_range(0, 2_000_000)
    assert m.cardinality() == 2_000_000
    assert len(m.to_bytes()) < 100

def test_span():
    m = SparseMap([1, 2, 3, 7, 8, 9, 10])
    assert m.span(0, 4, True) == 7
    assert m.span(0, 3, False) == 4

def test_iteration_ascending():
    m = SparseMap([100, 1, 50, 2048, 4096])
    assert list(m) == [1, 50, 100, 2048, 4096]

def test_set_operators():
    a = SparseMap(range(0, 150))
    b = SparseMap(range(100, 200))
    assert len(a | b) == 200
    assert len(a & b) == 50
    assert len(a - b) == 100
    assert len(a ^ b) == 150

def test_named_set_ops_match_operators():
    a = SparseMap([1, 2, 3])
    b = SparseMap([3, 4])
    assert a.union(b) == a | b
    assert a.intersection(b) == a & b
    assert a.difference(b) == a - b
    assert a.symmetric_difference(b) == a ^ b

def test_inplace_operators():
    a = SparseMap([1, 2])
    a |= SparseMap([3])
    assert list(a) == [1, 2, 3]
    a &= SparseMap([2, 3])
    assert list(a) == [2, 3]
    a -= SparseMap([2])
    assert list(a) == [3]
    a ^= SparseMap([3, 4])
    assert list(a) == [4]

def test_inplace_with_self():
    a = SparseMap([1, 2])
    a |= a
    assert list(a) == [1, 2]
    a -= a
    assert len(a) == 0

def test_subset_superset_intersects():
    a = SparseMap([1, 2])
    b = SparseMap([1, 2, 3])
    assert a.is_subset(b) and b.is_superset(a)
    assert a.intersects(b)
    assert not a.intersects(SparseMap([9]))

def test_shifted():
    m = SparseMap([0, 10]).shifted(5)
    assert list(m) == [5, 15]
    assert list(m.shifted(-5)) == [0, 10]

def test_equality_ignores_construction_order():
    assert SparseMap([1, 2, 3]) == SparseMap([3, 2, 1])
    assert SparseMap([1, 2, 3]) != SparseMap([1, 2])

def test_dense_and_range_builds_compare_equal():
    a = SparseMap()
    a.insert_range(0, 4096)
    assert a == SparseMap(range(4096))

def test_serialize_roundtrip():
    m = SparseMap([0, 1, 2**20, 2**33])
    assert SparseMap.from_bytes(m.to_bytes()) == m

def test_from_bytes_rejects_garbage():
    with pytest.raises(ValueError):
        SparseMap.from_bytes(b"not a sparsemap")

def test_negative_index_raises():
    with pytest.raises(OverflowError):
        SparseMap().insert(-1)

def test_negative_membership_is_false():
    assert -1 not in SparseMap([1])

def test_contains_rejects_non_int():
    with pytest.raises(TypeError):
        "x" in SparseMap()

def test_large_indices():
    m = SparseMap(LARGE_KEYS)
    for k in LARGE_KEYS:
        assert k in m
    assert list(m) == sorted(LARGE_KEYS)
    assert SparseMap.from_bytes(m.to_bytes()) == m

def test_rank_select_are_inverse():
    m = SparseMap([2, 4, 8, 16, 2**40])
    for i in range(len(m)):
        pos = m.select(i)
        assert pos is not None
        assert m.rank(pos) == i

def test_repr():
    assert repr(SparseMap([1, 2])) == "SparseMap(cardinality=2)"
