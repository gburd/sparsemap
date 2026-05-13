# Changelog

All notable changes to sparsemap are documented here.  Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning
follows [SemVer](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

(Phase 4 work: fix the two pre-existing ASan bugs noted in
`.agent/notes/phase1-deferred-bugs.md` before tagging v1.0.1.)

## [1.0.0] — 2026-05-13

First polished release.  Heisenbug fix from `HEISENBUG_REPORT.md`
plus full repository consolidation from a multi-branch experimental
state to a single canonical `main`.

### Added

- `sparsemap_create(size)` — verb-named allocator (postgres/undo
  compatible name).  `sparsemap()` is kept as a deprecated alias.
- `sparsemap_free(map)` — lineage-aware disposal.  Frees the struct
  for `SM_OWNED_CONTIGUOUS` and `SM_WRAPPED` lineages; frees both
  the data buffer and the struct for `SM_OWNED_SPLIT` (the lineage
  produced by growing a wrap'd map).
- `sparsemap_owned_copy(map)` — guaranteed-owned, guaranteed-growable
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

- **HEISENBUG**: `sparsemap_set_data_size(map, NULL, size)` no longer
  silently no-ops the realloc on wrap'd maps.  See
  `HEISENBUG_REPORT.md` for the original bug; see `docs/API.md` for
  the new lineage-aware behavior.
- **`__sm_get_chunk_count` empty-map bug**: returns 0 when
  `m_data_used < SM_SIZEOF_OVERHEAD` instead of reading garbage from
  uninitialized buffer bytes.  This kills four downstream
  victim-function bugs (in `sparsemap_intersection`,
  `sparsemap_union`, `sparsemap_maximum`, `__sm_rank_vec`) at the
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

- `sparsemap()` — use `sparsemap_create()` instead.  Kept as alias
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
