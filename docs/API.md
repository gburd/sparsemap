# sparsemap public API reference

Companion to `sm.h`.  Every public function is
documented here with semantics, allocation lineage, error returns,
and how it interacts with the lifetime contract.  See
[ARCHITECTURE.md](ARCHITECTURE.md) for the data model and
[MIGRATION.md](MIGRATION.md) for what changed since pre-v1.

## Conventions

- All functions taking a `sm_t *` accept NULL only where
  documented; otherwise NULL inputs are undefined.
- Functions that mutate the map return `SM_IDX_MAX` and set
  `errno` to `ENOSPC` when the backing buffer is full.  Grow the
  buffer with `sm_set_data_size()` and retry.
- All allocation functions return `NULL` on failure.

## Lifecycle

| Function                 | Returns         | Lineage of result      |
|--------------------------|-----------------|------------------------|
| `sm_create(size)` | `sm_t *` | `SM_OWNED_CONTIGUOUS`  |
| `sparsemap()`            | (alias)         | `SM_OWNED_CONTIGUOUS`  |
| `sm_copy(other)`  | `sm_t *` | `SM_OWNED_CONTIGUOUS`  |
| `sm_owned_copy(map)` | `sm_t *` | `SM_OWNED_CONTIGUOUS`  |
| `sm_wrap(buf,sz)` | `sm_t *` | `SM_WRAPPED`           |
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
sm_t *
sm_set_data_size(sm_t *map, uint8_t *data, size_t size);
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
sm_scan(const sm_t *m,
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

## Complete API index

<!-- BEGIN GENERATED API INDEX -->
<!-- Generated by scripts/gen_api_index.sh from sm.h.  Do not edit. -->

Every function, type and macro exported by `sm.h`, with its
one-line summary.  Signatures and full documentation live in the
header itself.

| Function | Summary |
|----------|---------|
| `sm_add_grow()` | Add a bit, growing the map's buffer geometrically if needed. |
| `sm_add_grow_cursor()` | Like sm_add_grow(), but threads a caller-owned cursor. |
| `sm_add_many()` | Add N indices from an array. |
| `sm_add_many_grow()` | Add N indices, growing the buffer as needed. |
| `sm_add_range()` | Set every bit in `[lo, hi)`. |
| `sm_add()` | Set the bit at \a idx to 1. |
| `sm_andnot()` | Synonym for sm_difference (logical AND-NOT: bits in a but not b). */ |
| `sm_and()` | Synonym for sm_intersection (logical AND). */ |
| `sm_assign()` | Set or clear the bit at \a idx. |
| `sm_capacity_remaining()` | Estimate remaining buffer capacity as a percentage. |
| `sm_cardinality()` | Count the total number of set bits (cardinality). |
| `sm_clear()` | Reset the map to empty without freeing memory. |
| `sm_compare()` | Three-way compare for ordering bitmaps. |
| `sm_contains_cached()` | Test a bit using a caller-owned 8-way MRU chunk cache. |
| `sm_contains_many()` | Test many bits in one left-to-right sweep (batched). |
| `sm_contains()` | Test whether the bit at \a idx is set. |
| `sm_copy()` | Create a deep copy of \a other. |
| `sm_create()` | Allocate a heap-managed sparsemap with an internal buffer. |
| `sm_create_from_array()` | Create a sparsemap from an array of indices. |
| `sm_create_from_range()` | Create a sparsemap containing every bit in `[lo, hi)`. |
| `sm_create_singleton()` | Create a sparsemap containing exactly the bit at `idx`. |
| `sm_deserialize()` | Deserialize a previously-serialized buffer into a fresh map. |
| `sm_difference_cardinality()` | Compute |a \ b| without allocating the difference. */ |
| `sm_difference()` | Create a new sparsemap containing bits set in \a a but not in \a b. |
| `sm_difference_inplace()` | In-place difference: `dst := dst \ src`. |
| `sm_equals()` | Test bit-set equality of two sparsemaps. |
| `sm_extract_range()` | Extract a range of bits as a new sparsemap. |
| `sm_fill_factor()` | Return the fraction of bits that are set. |
| `sm_flip_range()` | Complement every bit in `[lo, hi)`: set bits become unset and vice versa. |
| `sm_free()` | Dispose of a sparsemap, regardless of allocation lineage. |
| `sm_get_capacity()` | Return the total buffer capacity in bytes. |
| `sm_get_data()` | Return a pointer to the raw data buffer. |
| `sm_get_size()` | Return the number of buffer bytes currently in use. |
| `sm_hash()` | Stable content-based hash of the bit set. |
| `sm_init()` | Initialize a caller-allocated sm_t with a buffer. |
| `sm_intersection_cardinality()` | Compute the cardinality of (a intersect b) without allocating it. */ |
| `sm_intersection()` | Create a new sparsemap containing bits set in both \a a and \a b. |
| `sm_intersection_inplace()` | In-place intersection: `dst := dst INT src`. |
| `sm_is_empty()` | Test whether a sparsemap is empty (has no set bits). |
| `sm_is_subset()` | Test whether \a a's bits are a subset of \a b's bits. |
| `sm_is_superset()` | Test whether \a a's bits are a superset of \a b's bits. |
| `sm_jaccard_index()` | Jaccard similarity index: |a intersect b| / |a union b|. |
| `sm_locator_build()` | Build a transient sqrt(n) locator over \a map. |
| `sm_locator_contains()` | O(sqrt n) membership test; equals sm_contains(map, idx, NULL). */ |
| `sm_locator_free()` | Release a locator built by sm_locator_build (NULL-safe). */ |
| `sm_locator_rank()` | O(sqrt n) rank over inclusive [lo, hi]; equals sm_rank. |
| `sm_locator_select()` | O(sqrt n) select; equals sm_select(map, n, value). |
| `sm_maximum()` | Return the position of the last set bit (maximum). |
| `sm_membership()` | Classify a sparsemap as empty, singleton, or multi-element. |
| `sm_minimum()` | Return the position of the first set bit (minimum). |
| `sm_next_member()` | Find the lowest set bit at index > \a prev_idx. |
| `sm_nonempty_difference()` | Test whether `a \ b` has any set bits, without allocating. |
| `sm_offset()` | Create a new sparsemap with all bits shifted by \a offset. |
| `sm_open()` | Attach to an existing (serialized) sparsemap buffer. |
| `sm_open_copy()` | Allocate a fresh map and deserialize raw on-disk bytes into it. |
| `sm_or()` | Synonym for sm_union (logical OR). */ |
| `sm_overlap()` | Test whether two sparsemaps share at least one set bit. |
| `sm_owned_copy()` | Return a guaranteed-owned, guaranteed-growable copy of any sparsemap. |
| `sm_pop_first()` | Find the lowest set bit, clear it, and return it. |
| `sm_pop_last()` | Find the highest set bit, clear it, and return it. |
| `sm_prev_member()` | Find the highest set bit at index < \a prev_idx. |
| `sm_rank()` | Count matching bits in the inclusive range [\a x, \a y]. |
| `sm_remove()` | Clear the bit at \a idx (set to 0). |
| `sm_remove_range()` | Clear every bit in `[lo, hi)`. |
| `sm_scan()` | Invoke a callback for every set bit in the map. |
| `sm_select()` | Find the position of the \a n'th matching bit (0-based). |
| `sm_serialized_size()` | Compute the buffer size needed to serialize \a map. |
| `sm_serialize()` | Serialize \a map into \a out (`sm_serialized_size` bytes). |
| `sm_set_allocator()` | Set the process-wide allocator hooks. |
| `sm_set_data_size()` | Resize the data buffer. |
| `sm_shrink_to_fit()` | Realloc the data buffer down to exactly `m_data_used` bytes. |
| `sm_singleton_member()` | Return the sole member of a singleton sparsemap. |
| `sm_span()` | Find the first contiguous run of \a len bits matching \a value. |
| `sm_split()` | Split the map at \a idx, moving higher bits to \a other. |
| `sm_statistics()` | Fill an sm_stats_t with introspection data. */ |
| `sm_subset_compare()` | Classify the subset relationship between \a a and \a b. |
| `sm_to_array()` | Materialize all set bits as a uint64_t array. |
| `sm_union_cardinality()` | Compute the cardinality of (a union b) without allocating it. */ |
| `sm_union()` | Create a new sparsemap containing bits set in either \a a or \a b. |
| `sm_union_inplace()` | In-place union: `dst := dst U src`. |
| `sm_validate()` | Runtime self-check of a sparsemap's internal consistency. |
| `sm_wrap()` | Allocate a sm_t that wraps a caller-provided buffer. |
| `sm_xor_cardinality()` | XOR cardinality without allocation. |
| `sm_xor_inplace()` | In-place symmetric difference: `dst := dst XOR src`. |
| `sm_xor()` | Symmetric difference: bits set in exactly one of \a a, \a b. |

### Types and macros

| Name | Kind |
|------|------|
| `SM_ALIGNAS` | macro |
| `SM_ALIGNED` | macro |
| `sm_allocator_t` | type |
| `SM_CACHE_WAYS` | macro |
| `SM__CAT2` | macro |
| `SM__CAT` | macro |
| `SM_CURSOR_CACHED_INIT` | macro |
| `sm_cursor_cached_t` | type |
| `SM_CURSOR_INIT` | macro |
| `sm_cursor_t` | type |
| `SM_FOUND` | macro |
| `SM_IDX_MAX` | macro |
| `sm_locator_t` | type |
| `sm_membership_t` | type |
| `SM_NOT_FOUND` | macro |
| `SM__P` | macro |
| `sm_stats_t` | type |
| `sm_subset_relation_t` | type |
| `sm_t` | type |
| `SM_VERSION_MAJOR` | macro |
| `SM_VERSION_MINOR` | macro |
| `SM_VERSION_PATCH` | macro |
| `SM_VERSION_STRING` | macro |

<!-- END GENERATED API INDEX -->
