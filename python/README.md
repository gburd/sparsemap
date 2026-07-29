# sparsemap (Python)

CPython bindings for [sparsemap](https://codeberg.org/gregburd/sparsemap),
a sparse, compressed, run-length-encoded bitmap over 64-bit indices.

The extension is written in Rust on top of the pure-Rust port in
`../rust` (path dependency, always in version lockstep); no C is
compiled into the wheel.  It is **wire-compatible** with the C library:
bytes from `sm_serialize` load with `SparseMap.from_bytes`, and
`to_bytes` output loads with `sm_deserialize`.  Both directions are
verified in CI (`ci/wire_compat.sh` and `tests/test_wire_compat.py`).

## Build and test

Requires a Rust toolchain and [uv](https://docs.astral.sh/uv/):

```bash
cd python
uv sync # builds the extension via maturin
uv run pytest
```

uv drives [maturin](https://maturin.rs) as the PEP 517 backend; uv's
own `uv_build` backend is Python-only and cannot compile extensions.

## Example

```python
from sparsemap import SparseMap

m = SparseMap()
m.insert_range(0, 2_000_000) # two million bits, a few bytes
assert 1_500_000 in m
assert m.rank(1_000_000) == 1_000_000
assert m.select(0) == 0

a = SparseMap(range(100))
b = SparseMap(range(50, 150))
assert len(a & b) == 50
assert (a | b) == SparseMap(range(150))

blob = m.to_bytes() # C-compatible format
assert SparseMap.from_bytes(blob) == m
```

## API

- `SparseMap(iterable=None)`; `insert`, `remove`, `clear`, `x in m`.
- `insert_range(start, end)` / `remove_range(start, end)`, half-open.
- `rank(idx)` (set bits strictly below `idx`), `select(n)` (0-based),
  `span(start, length, value=True)`.
- `min()`, `max()`, `len()`, `cardinality()` (unbounded), truthiness.
- Operators `| & - ^` and in-place forms, plus `union`,
  `intersection`, `difference`, `symmetric_difference`, `is_subset`,
  `is_superset`, `intersects`, `shifted(offset)`.
- `to_bytes()` / `SparseMap.from_bytes(data)`; ascending iteration.

Not thread-safe, like the C library; guard mutation with a lock.

## Versioning

The binding version tracks the Rust port it wraps.  MIT licensed,
like the rest of the repository.
