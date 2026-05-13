# Migrating to sparsemap v1.0.0

If you currently vendor a copy of `sparsemap.c` from the pre-v1
master, this document covers the breaking changes you'll see when
you sync to v1.0.0 and how to drop your local patches.

## TL;DR

- **`struct sparsemap` grew one byte** (`uint8_t m_alloc_kind` at
  the end).  If you allocated `sparsemap_t` instances with
  `palloc(sizeof(sparsemap_t))` or `calloc(1, sizeof(sparsemap_t))`,
  recompile against the v1 header and you're done — the new field
  zero-initializes correctly.
- **`sm_set_data_size` no longer silently no-ops.**  See
  [HEISENBUG_REPORT.md](../HEISENBUG_REPORT.md) for what it used to
  do; the new behavior is in [API.md](API.md).
- **`__sm_get_chunk_count` returns 0 when `m_data_used == 0`.**
  Drop any local patches that defended against the "garbage chunk
  count" bug.
- **Two new public functions:** `sm_create()` (verb-named
  alias of `sparsemap()`), `sm_free()` (lineage-aware
  disposal), `sm_owned_copy()` (universal lineage normalizer).

## Per-consumer notes

### pg_tre

You can drop these local patches:

| pg_tre patch                          | Reason                          |
|---------------------------------------|----------------------------------|
| `BUG FIX: m_data_used = 0 but ...`    | `__sm_get_chunk_count` now      |
|   in sm_intersection           | guards internally.              |
| `BUG FIX: m_data_used = 0 but ...`    | Same.                           |
|   in sm_union                  |                                  |
| `BUG FIX: m_data_used = 0 but ...`    | Same.                           |
|   in sm_maximum                |                                  |
| `BUG FIX: m_data_used = 0 but ...`    | Same.                           |
|   in __sm_rank_vec                    |                                  |

You can also simplify:

- `pg_tre_posting_materialize`: replace the manual
  `sparsemap()`-then-memcpy-then-`sm_open` dance with
  `sm_owned_copy(wrap'd_map)`.
- `materialize_merged_postings`: same simplification.
- `apply_tuple_bloom_filter`: the grow-and-recheck workaround can
  stay (it's still good defensive code), but the underlying bug it
  worked around is fixed.
- The pending-list overlay's `palloc`'d `uint64` array workaround
  can either stay or be reverted to `sm_wrap` + `sm_add`
  loop — the wrap-and-grow path is now safe.

Sync via `contrib/pg_tre_sync.sh PATH/TO/pg_tre`.

### postgres/undo

postgres/undo's vendored copy is largely API-compatible already
(it added `sm_create` and `sm_free` which v1 now
ships upstream).  The differences:

- `__sm_alloc_kind` field is new; postgres palloc'd struct
  instances will need to be re-palloced or zeroed.  `palloc0()` is
  fine.
- The fix to `__sm_get_chunk_count` matches what postgres/undo
  documented as the recommended upstream fix.

The `__sm_separate_rle_chunk` underflow bug (Phase 1 deferred bug
#3) and the off-by-4 in `__sm_insert_data` (deferred #1) are both
still present in v1.0.0; postgres/undo will see the same flaky
ASan failure that the upstream test suite sees.  These will be
fixed in v1.0.1.

Sync via `contrib/postgres_undo_sync.sh PATH/TO/postgres`.

## Disposal: when to use `sm_free` vs libc `free`

Pre-v1, every `sparsemap()` map could be disposed with libc `free()`
because struct and buffer were always one allocation.  In v1, the
wrap-and-grow promotion produces `SM_OWNED_SPLIT` maps that have a
*separately* malloc'd buffer.  Libc `free()` on those leaks the
buffer.

**Rule of thumb:** use `sm_free()` going forward.  It works
for every lineage, and you no longer have to know which lineage
your map has.

For backward compatibility, `sparsemap()` /
`sm_create()` / `sm_copy()` results can still be
disposed with libc `free()` — they're always `SM_OWNED_CONTIGUOUS`,
so a single `free()` releases everything.  Only the wrap-and-grow
case requires `sm_free()`.

## Sanitizer / hardening notes

v1 builds with meson by default; meson enables `_FORTIFY_SOURCE=2`
and `-fstack-protector-strong`, which expose two pre-existing bugs
in `__sm_separate_rle_chunk` and `__sm_insert_data`.  These are
tracked in
`.agent/notes/phase1-deferred-bugs.md` and slated for v1.0.1.

Until they're fixed, the upstream test suite builds the main test
binary with `-U_FORTIFY_SOURCE -fno-stack-protector` to mask them.
Downstream consumers using meson should either apply the same
workaround or (preferred) upgrade to v1.0.1 once it lands.
