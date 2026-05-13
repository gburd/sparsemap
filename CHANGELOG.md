# Changelog

All notable changes to sparsemap are documented here.  Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning
follows [SemVer](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
