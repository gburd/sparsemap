# sparsemap public API reference

Companion to `sm.h`.  Every public function is
documented here with semantics, allocation lineage, error returns,
and how it interacts with the lifetime contract.  See
[ARCHITECTURE.md](ARCHITECTURE.md) for the data model and
[MIGRATION.md](MIGRATION.md) for what changed since pre-v1.

## Conventions

- All functions taking a `sparsemap_t *` accept NULL only where
  documented; otherwise NULL inputs are undefined.
- Functions that mutate the map return `SM_IDX_MAX` and set
  `errno` to `ENOSPC` when the backing buffer is full.  Grow the
  buffer with `sm_set_data_size()` and retry.
- All allocation functions return `NULL` on failure.

## Lifecycle

| Function                 | Returns         | Lineage of result      |
|--------------------------|-----------------|------------------------|
| `sm_create(size)` | `sparsemap_t *` | `SM_OWNED_CONTIGUOUS`  |
| `sparsemap()`            | (alias)         | `SM_OWNED_CONTIGUOUS`  |
| `sm_copy(other)`  | `sparsemap_t *` | `SM_OWNED_CONTIGUOUS`  |
| `sm_owned_copy(map)` | `sparsemap_t *` | `SM_OWNED_CONTIGUOUS`  |
| `sm_wrap(buf,sz)` | `sparsemap_t *` | `SM_WRAPPED`           |
| `sm_init(map,buf,sz)` | `void`      | `SM_WRAPPED`           |
| `sm_open(map,buf,sz)` | `void`      | `SM_WRAPPED`           |
| `sm_free(map)`    | `void`          | (disposes any lineage) |
| `sm_clear(map)`   | `void`          | (preserves lineage)    |

`sparsemap()` is a deprecated alias of `sm_create()`; new
code should use the verb-named version.  Both will accept callers
identically through v1.x; the alias is removed in v2.

`sm_owned_copy()` is the "I don't trust this map's lineage"
escape hatch.  Pass any sparsemap (yours, library-returned, or
deserialized) and get back a self-contained copy that's safe to grow
and dispose.

## Resize contract

```c
sparsemap_t *
sm_set_data_size(sparsemap_t *map, uint8_t *data, size_t size);
```

Two calling forms.  See [HEISENBUG_REPORT.md](../HEISENBUG_REPORT.md)
for the bug this contract resolves.

### `data == NULL`: library-managed resize

Always succeeds (returning a possibly-relocated map pointer) or
returns `NULL` on allocation failure.  **Never** silently no-ops.

| Lineage                | Behavior                                        |
|------------------------|-------------------------------------------------|
| `SM_OWNED_CONTIGUOUS`  | `realloc()` the single struct+buffer block.    |
|                        | Caller MUST update all references to the new   |
|                        | pointer.                                        |
| `SM_OWNED_SPLIT`       | `realloc(m_data, size)`; struct stays put.     |
| `SM_WRAPPED`, shrink   | Update `m_capacity` in place; caller's buffer  |
|                        | is unchanged.  Lineage stays `SM_WRAPPED`.     |
| `SM_WRAPPED`, grow     | Allocate new library-owned buffer, memcpy the  |
|                        | `m_data_used` prefix, redirect `m_data`.       |
|                        | **Lineage transitions to `SM_OWNED_SPLIT`**.   |
|                        | Caller's original buffer is untouched and      |
|                        | still theirs to free.  The promoted map MUST   |
|                        | be disposed with `sm_free()` — libc     |
|                        | `free()` leaks the new buffer.                 |

### `data != NULL`: caller-supplied buffer

The map is re-pointed to `data`.  `m_capacity` is updated.  Lineage
transitions to `SM_WRAPPED`: the library will not realloc or free
`data` on the caller's behalf.  The caller is responsible for
copying any existing bits before the call (the library only updates
the pointer).

## Single-bit operations

| Function                         | Description                              |
|----------------------------------|------------------------------------------|
| `sm_contains(map, idx)`   | Test bit; returns `bool`.                |
| `sm_assign(map, idx, v)`  | Set or clear, depending on `v`.          |
| `sm_add(map, idx)`        | Set bit to 1.                            |
| `sm_remove(map, idx)`     | Clear bit (set to 0).                    |

`add` / `remove` / `assign` may trigger chunk transitions:

- A long run of `add` calls on consecutive indices may transition a
  sparse chunk to RLE.
- A `remove` inside an RLE run separates the chunk into pieces.
- Adjacent chunks that form a contiguous run are coalesced into a
  single RLE chunk.

All three return `SM_IDX_MAX` with `errno=ENOSPC` if the
buffer is full.

## Aggregate queries

| Function                    | Description                                |
|-----------------------------|--------------------------------------------|
| `sm_cardinality(m)`  | Number of set bits.  Equivalent to        |
|                             | `rank(m, 0, SM_IDX_MAX, true)`.    |
| `sm_minimum(m)`      | Lowest set bit, or 0 if empty.             |
| `sm_maximum(m)`      | Highest set bit, or 0 if empty.            |
| `sm_fill_factor(m)`  | cardinality / (max - min + 1).             |

## Rank / select / span

| Function                              | Description                       |
|---------------------------------------|-----------------------------------|
| `sm_rank(m, x, y, value)`      | Count bits matching `value` in    |
|                                       | inclusive range `[x, y]`.         |
| `sm_select(m, n, value)`       | Index of the n-th matching bit    |
|                                       | (0-based).  RLE chunks O(1).      |
| `sm_span(m, start, len, value)`| First run of ≥`len` consecutive   |
|                                       | matching bits at or after `start`.|

`select` returns `SM_IDX_MAX` if fewer than `n + 1` matches
exist; `span` does the same if no run of the requested length exists.

## Iteration

```c
void
sm_scan(const sparsemap_t *m,
               void (*scanner)(uint32_t vec[], size_t n, void *aux),
               size_t skip, void *aux);
```

Invokes `scanner` with batches of up to 64 set-bit indices in
ascending order.  `skip` is the number of set bits to ignore before
the first callback.  `aux` is passed through verbatim.

## Bulk operations

| Function                            | Description                       |
|-------------------------------------|-----------------------------------|
| `sm_union(a, b)`             | Logical OR.  Returns a new map.   |
| `sm_intersection(a, b)`      | Logical AND.                      |
| `sm_difference(a, b)`        | a AND NOT b.                      |
| `sm_split(map, idx, other)`  | Move bits at/after `idx` from    |
|                                     | `map` into `other`.               |
| `sm_offset(map, offset)`     | Return a new map with all bits   |
|                                     | shifted by `offset` (signed).     |

All result-returning bulk operations produce `SM_OWNED_CONTIGUOUS`
maps; the result is safe to grow and dispose with `sm_free()`
or libc `free()`.

## Compile-time options

| Macro                   | Effect                                          |
|-------------------------|-------------------------------------------------|
| `SPARSEMAP_TESTING`     | Exposes `_tst_*` helpers and the `QCC_*`        |
|                         | property-test generators used by `tests/test`.  |
| `SPARSEMAP_DIAGNOSTIC`  | Enables `__sm_assert`, `__sm_diag()` debug       |
|                         | output, and the `__sm_check_invariants` runs at|
|                         | the top of every public function.               |

In meson, both are set with `-Ddiagnostic=true` (sets
`SPARSEMAP_DIAGNOSTIC`) or by adding `-DSPARSEMAP_TESTING` to
`CFLAGS`.  Production builds disable both.
