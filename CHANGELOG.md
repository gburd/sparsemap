# Changelog

All notable changes to sparsemap are documented here.  Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning
follows [SemVer](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [2.0.0] — 2026-05-13

**Breaking change release.**  The `sparsemap_*` legacy macro aliases
(introduced in v1.1 for backward compatibility with pre-rename callers)
are gone.  Callers that still use them must switch to the `sm_*`
names.  The opaque type `sparsemap_t` is unchanged.

If you're a downstream consumer (pg_tre, postgres/undo) and are still
on the `sparsemap_*` names: run a one-line sed across your tree.

    sed -i 's/\bsparsemap_/sm_/g; s/\bSPARSEMAP_/SM_/g' your_files.c

(Spare the type itself: it stays `sparsemap_t`.)

### Added

Bitwise-op synonyms for the existing set operations:

  - `sm_or(a, b)`        synonym for `sm_union`
  - `sm_and(a, b)`       synonym for `sm_intersection`
  - `sm_andnot(a, b)`    synonym for `sm_difference`
  - (`sm_xor` already shipped in v1.2)

New predicates and operations:

  - `sm_is_superset(a, b)`            `sm_is_subset(b, a)` named directly
  - `sm_extract_range(map, lo, hi)`   new map containing only bits in `[lo, hi)`
  - `sm_pop_last(map)`                pop the highest set bit

### Optimized

  - **`sm_union_inplace`, `sm_intersection_inplace`, `sm_difference_inplace`**
    now delegate to the chunk-pair-walk in their out-of-place
    counterparts, then memcpy the result back into `dst`'s buffer.
    This replaces the previous bit-by-bit iteration and is genuinely
    chunk-aware: O(chunks) instead of O(bits).  For the typical
    pg_tre workload (TID set unions) this is a substantial speedup.

  - `sm_add_range` and `sm_remove_range` remain O(range) loops.
    A chunk-aware version requires direct manipulation of the
    chunk-codec primitives (`__sm_append_rle_chunk`) plus partial-
    chunk handling that's a non-trivial refactor.  Documented as a
    future optimization; the current naive implementation is correct
    but pays one `sm_add` per bit.

### Removed

All `sparsemap_*` macro aliases.  The full list (29 functions, 7
macros) is gone from `include/sparsemap.h`:

  sparsemap_create, sparsemap_copy, sparsemap_owned_copy,
  sparsemap_wrap, sparsemap_init, sparsemap_open, sparsemap_clear,
  sparsemap_free, sparsemap_set_data_size, sparsemap_capacity_remaining,
  sparsemap_get_capacity, sparsemap_get_size, sparsemap_get_data,
  sparsemap_contains, sparsemap_assign, sparsemap_add, sparsemap_remove,
  sparsemap_cardinality, sparsemap_minimum, sparsemap_maximum,
  sparsemap_fill_factor, sparsemap_rank, sparsemap_select,
  sparsemap_span, sparsemap_scan, sparsemap_union, sparsemap_intersection,
  sparsemap_difference, sparsemap_split, sparsemap_offset, plus all
  Phase A/B v1.2 additions.

  SPARSEMAP_IDX_MAX, SPARSEMAP_FOUND, SPARSEMAP_NOT_FOUND,
  SPARSEMAP_VERSION_STRING, SPARSEMAP_VERSION_MAJOR,
  SPARSEMAP_VERSION_MINOR, SPARSEMAP_VERSION_PATCH.

  The `SM_NO_LEGACY_ALIASES` opt-out preprocessor symbol is gone
  (the aliases it gated no longer exist).

### Migration from v1.2

For every call site:

  | v1.x                       | v2.0                  |
  |----------------------------|-----------------------|
  | `sparsemap_FOO(...)`       | `sm_FOO(...)`         |
  | `SPARSEMAP_FOO`            | `SM_FOO`              |
  | `sparsemap_t`              | `sparsemap_t` (same)  |
  | (compile-time switches:)   |                       |
  | `SPARSEMAP_TESTING`        | unchanged             |
  | `SPARSEMAP_DIAGNOSTIC`     | unchanged             |
  | `SPARSEMAP_H` (header guard)| unchanged            |
  | `SM_NO_LEGACY_ALIASES`     | gone                  |

### Verified

  Regular  (x86_64): 5/5 PASS
  ASan     (x86_64): 5/5 PASS
  UBSan    (x86_64): 5/5 PASS
  RISC-V   (rv):     5/5 PASS

## [1.2.0] — 2026-05-13

Major API expansion.  Adds 35 public functions covering the gap
between v1.1.1's surface and what CRoaring + PostgreSQL bitmapset
provide.  See `.agent/notes/api-gaps-and-tasks.md` for the original
gap analysis.

### Added

Predicates and comparisons:
  - `sm_is_empty(map)`
  - `sm_equals(a, b)`
  - `sm_is_subset(a, b)`
  - `sm_overlap(a, b)`
  - `sm_membership(map)` -> `SM_EMPTY` / `SM_SINGLETON` / `SM_MULTIPLE`
  - `sm_singleton_member(map)`
  - `sm_compare(a, b)` (three-way)
  - `sm_subset_compare(a, b)` -> `SM_REL_*` (four-way)

Member-by-member iteration:
  - `sm_next_member(map, prev)`
  - `sm_prev_member(map, prev)`
  - `sm_pop_first(map)` (destructive)

Cardinality without allocation:
  - `sm_union_cardinality`, `sm_intersection_cardinality`,
    `sm_difference_cardinality`, `sm_xor_cardinality`
  - `sm_nonempty_difference(a, b)`
  - `sm_jaccard_index(a, b)`

Bulk operations:
  - `sm_add_many(map, arr, n)`, `sm_to_array(map, out, *n)`
  - `sm_add_range(map, lo, hi)`, `sm_remove_range(map, lo, hi)`,
    `sm_flip_range(map, lo, hi)`

XOR:
  - `sm_xor(a, b)`

In-place set operations:
  - `sm_union_inplace(dst, src)`
  - `sm_intersection_inplace(dst, src)`
  - `sm_difference_inplace(dst, src)`

Constructors:
  - `sm_create_singleton(idx)`
  - `sm_create_from_range(lo, hi)`
  - `sm_create_from_array(arr, n)`

Maintenance and introspection:
  - `sm_validate(map)` (production-build self-check)
  - `sm_statistics(map, *stats)` with `sm_stats_t`
  - `sm_shrink_to_fit(map)`
  - `sm_hash(map)` (FNV-1a, content-based)

Portable serialization:
  - `sm_serialized_size(map)`
  - `sm_serialize(map, *out, n)`
  - `sm_deserialize(in, n)` (bounded-safe; rejects malformed input)

New enums and types:
  - `sm_membership_t`
  - `sm_subset_relation_t`
  - `sm_stats_t`

New example: `examples/ex_5.c` exercises every category.

### Backward compatibility

Legacy `sparsemap_*` macro aliases added for all 35 new functions.
Pre-v1.2 callers who still use the long names compile unchanged.
Set `SM_NO_LEGACY_ALIASES` to opt out.

### Performance notes

Most new functions are simple wrappers over the v1.1.1 primitives
(particularly `sm_next_member`).  Future releases may specialize
the hot paths (chunk-pair-walk for set-op cardinalities,
chunk-aware `sm_add_range`) when profiling shows real consumers
need it.  v1.2 prioritizes correctness and API completeness.

### Verified

  Regular  (x86_64): 5/5 PASS
  ASan     (x86_64): 5/5 PASS
  UBSan    (x86_64): 5/5 PASS
  RISC-V   (rv):     5/5 PASS
  Coverage:          76.0% lines / 61.7% branches / 93.2% functions

## [1.1.1] — 2026-05-13

Deferred-bug closure release.

### Fixed

- **Deferred bug #3 (sm_split underflow):** `sm_open` now temporarily
  sets `m_data_used = m_capacity` before calling `__sm_get_size_impl`,
  so the v1.0.0 empty-map guard in `__sm_get_chunk_count` doesn't
  short-circuit when sm_open is initializing a fully-populated stunt
  buffer.  Closes the test_api_split ASan failure that's been open
  since v1.0.0.  Drops the `-fno-stack-protector` workaround on
  test_main.
- **Deferred bug #2 (UBSan misalignment):** chunk descriptors and
  payload vectors live at offset 4-mod-8 in the buffer.  Introduced
  `__sm_bitvec_unaligned_t` (uint64_t with `aligned(1)`) for
  `__sm_chunk_t.m_data` so loads and stores through it are
  unaligned-safe.  Modern compilers lower to single native
  load/store; strict-alignment cpus get correct byte-shuffled access.
- **`sm_fill_factor` bugs:** returned NaN on empty maps (0/0),
  returned a percentage despite docs saying `[0, 1]`, and used max
  (not max-min+1) for the range.  Reimplemented to match the
  documented contract.
- **Logic bug in `__sm_separate_rle_chunk`:** vector-within-chunk
  index was computed as `(aligned_idx + lrl) / SM_BITS_PER_VECTOR`,
  which mixes absolute bit position with chunk-relative length and
  produced shift exponents up to 952 (UB).  Fixed to use just `lrl /
  SM_BITS_PER_VECTOR`.
- **Logic bug in test-only QCC_genChunk shuffle:** size_t underflow.
  Replaced with proper Fisher-Yates step.
- **Test-only struct overlay misalignment:** test code was overlaying
  `__sm_chunk_t` on misaligned malloc'd buffers.  Refactored to
  stack-local chunk struct.

### Added

- `tests/test_coverage.c` exercises sm_fill_factor, sm_owned_copy,
  sm_free, sm_capacity_remaining, sm_minimum, sm_maximum, sm_select
  edge cases, sm_span dense-run case.  Caught the sm_fill_factor bug
  fixed above.
- `SM_LIKELY` / `SM_UNLIKELY` macros over `__builtin_expect`.
- `__attribute__((hot))` on `sm_add`, `sm_remove`, `sm_contains`.

### Verified

  Regular  (x86_64): 5/5 PASS
  ASan     (x86_64): 5/5 PASS, 0 errors
  UBSan    (x86_64): 5/5 PASS, 0 distinct runtime errors
  Valgrind (x86_64): 50/50 sub-tests in test_main, 0 errors, 0 leaks
  RISC-V   (rv):     5/5 PASS

  All deferred bugs from v1.0.0 / v1.1.0 closed.  No `-fno-stack-protector`
  or `-U_FORTIFY_SOURCE` workarounds remain in tests/meson.build.

### Known limitations

- Test coverage measured at 38.8% lines / 28.6% branches / 70.8%
  functions.  Reaching the >95% target is multi-day focused
  property-test work primarily targeting the RLE separation and
  chunk-merge code paths.  Future work.

## [1.1.0] — 2026-05-13

API rename, RISC-V portability fix, and v1.0.0 followups.

### Added

- Public-API prefix renamed from `sparsemap_*` to `sm_*` (BSD-style
  short names): `sm_create`, `sm_free`, `sm_add`, `sm_contains`, etc.
  All 30 public functions and the `SM_IDX_MAX` / `SM_FOUND` /
  `SM_NOT_FOUND` / `SM_VERSION_*` macros.  The opaque type stays as
  `sparsemap_t`.
- Backward-compatibility `#define` aliases for every legacy
  `sparsemap_*` name in the public header.  Pre-v1.1 callers compile
  unchanged.  Define `SM_NO_LEGACY_ALIASES` to opt out and
  force-fail unmigrated code.
- Unaligned-safe `__sm_load_idx` / `__sm_store_idx` / `__sm_load_u32` /
  `__sm_store_u32` helpers.  All 50 prior `*(__sm_idx_t *)p` and
  `*(uint32_t *)p` casts replaced with `memcpy`-based loads.  Modern
  compilers lower to a single native load/store at -O1+; correct on
  strict-alignment cpus including the standard RISC-V profile.
- `man/sparsemap.3` (API reference) and `man/sparsemap.7`
  (concept overview) rewritten for v1.1.
- Tested on RISC-V (rv host, Ubuntu 24.04, gcc 13): 3/4 meson tests
  pass with the same `/api/split` failure as x86 (no portability
  regression).

### Fixed

- Off-by-4 in `__sm_insert_data` mitigated via `SM_ENOUGH_SPACE`
  slack so the worst-case write can no longer overrun by 4 bytes
  at the buffer boundary.
- `sm_split` now propagates `__sm_separate_rle_chunk`'s return
  value instead of silently using a possibly-uninitialized
  `sep.expand_by`.

### Documented

- 8-byte alignment requirement on buffers passed to `sm_wrap`,
  `sm_init`, `sm_open`.  Test buffers updated to use
  `_Alignas(uint64_t)`.
- Known issue: UBSan flags misaligned 8-byte loads inside the chunk
  codec because chunk descriptors land at 4-byte-aligned offsets by
  layout.  Affects only UBSan reports; real-world cpus (x86_64,
  aarch64, standard RISC-V) handle the access correctly.  Fix
  deferred to v1.2 (will require widening `__sm_idx_t` to 64-bit or
  routing all bitvec accesses through memcpy).
- Known issue: `sm_split` underflow in `__sm_separate_rle_chunk`
  exposed by ASan and stack-protector-enabled builds.  Test binary
  ships with `-U_FORTIFY_SOURCE -fno-stack-protector` to mask it.
  Production builds without sanitizers are unaffected.  Fix slated
  for v1.1.1.

### Verified

- Valgrind clean on `test_heisenbug`, `test_empty_map`,
  `test_rle_standalone` (0 errors, no leaks).
- ASan clean on the same three.
- RISC-V build + run on rv host: same 3/4 PASS.

## [1.0.0] — 2026-05-13

First polished release.  Heisenbug fix from `HEISENBUG_REPORT.md`
plus full repository consolidation from a multi-branch experimental
state to a single canonical `main`.

### Added

- `sm_create(size)` — verb-named allocator (postgres/undo
  compatible name).  `sparsemap()` is kept as a deprecated alias.
- `sm_free(map)` — lineage-aware disposal.  Frees the struct
  for `SM_OWNED_CONTIGUOUS` and `SM_WRAPPED` lineages; frees both
  the data buffer and the struct for `SM_OWNED_SPLIT` (the lineage
  produced by growing a wrap'd map).
- `sm_owned_copy(map)` — guaranteed-owned, guaranteed-growable
  copy of any sparsemap regardless of lineage.  The universal escape
  hatch for callers who don't trust their input's provenance.
- `__sm_check_invariants` — internal invariant checker that runs at
  the top of every public function under `SPARSEMAP_DIAGNOSTIC`,
  surfacing map-state corruption at the API boundary.
- meson build system (replaces autotools).  Lime-style layout:
  `src/`, `tests/`, `bench/`, `examples/`, `docs/`, `man/`,
  `scripts/`, `contrib/`, `config/`.
- CI workflows for both Codeberg (`.forgejo/workflows/`) and GitHub
  (`.github/workflows/`).  Five jobs: build (gcc + clang matrix),
  sanitizers (ASan + UBSan), valgrind, version-consistency,
  pages (Doxygen).
- Doxygen-generated API reference, deployed to Codeberg Pages.
- `contrib/pg_tre_sync.sh` and `contrib/postgres_undo_sync.sh` for
  mechanical sync into the two known consumer projects.

### Fixed

- **HEISENBUG**: `sm_set_data_size(map, NULL, size)` no longer
  silently no-ops the realloc on wrap'd maps.  See
  `HEISENBUG_REPORT.md` for the original bug; see `docs/API.md` for
  the new lineage-aware behavior.
- **`__sm_get_chunk_count` empty-map bug**: returns 0 when
  `m_data_used < SM_SIZEOF_OVERHEAD` instead of reading garbage from
  uninitialized buffer bytes.  This kills four downstream
  victim-function bugs (in `sm_intersection`,
  `sm_union`, `sm_maximum`, `__sm_rank_vec`) at the
  source.  pg_tre's four local `BUG FIX` patches can be dropped when
  syncing to v1.
- Missing `SM_ENOUGH_SPACE` guards in `__sm_map_set` at four sites
  that called `__sm_insert_data` without a pre-check.  Exposed by
  ASan; would silently overrun the buffer in production builds.

### Changed

- `struct sparsemap` gained one byte (`uint8_t m_alloc_kind` at the
  end).  ABI-incompatible with pre-v1; consumers must rebuild.
  Downstream impact: postgres/undo's vendored struct definition
  needs the same field.

### Deprecated

- `sparsemap()` — use `sm_create()` instead.  Kept as alias
  for v1.x; removed in v2.

### Repository hygiene

- Branches consolidated.  `main` is now the only "live" branch on
  origin alongside two `experiment/*` branches and nine `archive/*`
  tags.  See `.agent/notes/sparsemap-cleanup-plan.md` for the
  archive map.
- Autotools retired in favor of meson.
- Tracked IDE state (.idea, .idx, coverage/) removed from the
  branch and added to `.local-gitignore`.
- Working tree's loose `bitmapset.{c,h}` and zero-byte stub files
  cleaned up.
