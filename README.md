# sparsemap

[![CI](https://codeberg.org/gregburd/sparsemap/actions/workflows/ci.yml/badge.svg)](https://codeberg.org/gregburd/sparsemap/actions)
[![Pages](https://codeberg.org/gregburd/sparsemap/actions/workflows/pages.yml/badge.svg)](https://gregburd.codeberg.page/sparsemap/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A sparse, compressed bitmap library for C.  Optimized for workloads
with long runs of consecutive set or unset bits.

## Why sparsemap

Bitmaps are great when bits are dense and the universe is small.
They get expensive when either assumption fails — a 32-bit universe
needs 512 MB just to hold one bit per integer, even if only a dozen
are set.

Sparsemap stores only the chunks that contain actual data.  In each
chunk it picks one of two encodings depending on the local pattern:

- **Sparse encoding** stores a 64-bit descriptor and only the bit
  vectors that contain a mix of set and unset bits.  Uniform vectors
  (all-zero or all-one) take zero payload.
- **RLE encoding** stores a single 64-bit descriptor for a contiguous
  run of set bits.  A 2-billion-bit run takes 8 bytes.

Best case: 16 KB of consecutive set bits in 8 bytes.  Worst case
(random bits): identical to a raw bitmap plus 8 bytes of overhead.

## When to use sparsemap

Good fit:

- PostgreSQL extensions tracking TID sets, bitmap heap scans,
  posting lists.
- Trigram / n-gram indexes where document identifiers cluster.
- Allocation bitmaps for storage engines (free-list tracking).
- Anywhere you'd reach for [CRoaring](https://github.com/RoaringBitmap/CRoaring)
  but want a smaller, simpler library and don't need 32-bit integer
  support out of the box.

Not a fit:

- Multi-threaded workloads: sparsemap is not thread-safe.  A
  lock-free / wait-free variant is in design (see
  `experiment/thread-safe`).
- 32-bit integer universes: sparsemap uses 64-bit indices.

## Quick start

```bash
nix develop                              # optional dev shell
meson setup builddir
ninja -C builddir
ninja -C builddir test
```

Or with the Makefile wrapper:

```bash
make build
make test
```

Use it from C:

```c
#include <sparsemap/sparsemap.h>

sparsemap_t *map = sm_create(4096);
sm_add(map, 42);
sm_add(map, 1024);
assert(sm_contains(map, 42));
assert(sm_cardinality(map) == 2);
sm_free(map);
```

## Documentation

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — chunk layout,
  encoding, three-tier hierarchy.
- **[docs/API.md](docs/API.md)** — every public function, lineage
  rules, error returns.
- **[docs/MIGRATION.md](docs/MIGRATION.md)** — upgrading to v1.0.0
  from earlier vendored copies.
- **[man/sparsemap.3](man/sparsemap.3)** — Unix manual page.

API docs (Doxygen) are published to
**[gregburd.codeberg.page/sparsemap/](https://gregburd.codeberg.page/sparsemap/)**.

## Build options

```bash
meson setup builddir -Ddiagnostic=true   # enable __sm_assert + invariant checks
meson setup builddir -Db_sanitize=address  # ASan
meson setup builddir -Dbuildtype=release   # production: no asserts, max optimization
```

See `meson_options.txt` for the full list.

## Consumers

Sparsemap is vendored by:

- **[pg_tre](https://github.com/pg-tre/pg_tre)** — PostgreSQL trigram
  search extension.
- **[postgres/undo](https://github.com/EnterpriseDB/postgres-undo)** —
  EnterpriseDB's PostgreSQL undo-log fork.

`contrib/pg_tre_sync.sh` and `contrib/postgres_undo_sync.sh` keep the
vendored copies in sync with upstream.

## Versioning and history

Releases follow [SemVer](https://semver.org).  See `git log` for the
full commit-by-commit history; tags `v1.0.0` through the current
release list every shipping point.  Breaking changes between
major versions are summarized in the tag annotation messages
(`git show v2.0.0`).

Major breaking changes so far:

### API stability vs ABI stability

Sparsemap promises **source-level API stability** within a major
version: function signatures, macro names, and behavior of public
`sm_*` symbols do not change in a way that breaks compiling
consumer code.

Sparsemap **does not** promise ABI stability of the
`struct sparsemap` layout.  `sizeof(struct sparsemap)` and the
offsets of its fields may change in any minor release.  Consumers
must:

- Always allocate `sparsemap_t` via `sm_create()`,
  `sm_create_with_allocator()`, or `sm_wrap()` — never embed it
  inline in another struct, never `sizeof(sparsemap_t)` for an
  on-disk format, never `memcpy(struct, ...)` it.
- Treat the type as opaque: access only via `sm_*` accessors.
- Recompile (not just relink) after upgrading sparsemap.

The **wire format** produced by `sm_serialize` and consumed by
`sm_open`/`sm_deserialize` *is* stable.  Bytes serialized by v1.0
deserialize correctly under v2.x.  This is the contract that
matters for on-disk consumers.

### Breaking changes by version

- **v1.1.0** — public API prefix renamed `sparsemap_*` → `sm_*`.
  Macro aliases for the old names were kept under
  `SM_NO_LEGACY_ALIASES`.
- **v2.0.0** — macro aliases removed.  Migrate with
  `sed -i 's/\bsparsemap_/sm_/g; s/\bSPARSEMAP_/SM_/g' your_files.c`
  (the type itself stays `sparsemap_t`).
- **v2.1.0** — added per-map `m_allocator` field to `sparsemap_t`.
  Code that duplicates the struct definition (test harnesses,
  vendored copies that don't include `<sparsemap.h>`) must keep
  it in sync.  No source-level API breakage.
- **v2.2.0** — the `sm_allocator_t` is now passed by value, not by
  pointer.  `sm_set_allocator(hooks)` and
  `sm_create_with_allocator(n, hooks)` instead of the v2.1 spellings.
  Reset the global with `(sm_allocator_t){0}` instead of `NULL`.
  Two new (optional) fields: `alloc_zero` (let your allocator deliver
  pre-zeroed memory cheaply) and `aligned_alloc` / `aligned_free`
  (reserved for future SIMD; ignored in 2.2.x).  Per-map struct grew
  by ~32 bytes; consumers that embed `struct sparsemap` directly
  (vendored test harnesses) must update the duplicate.

## Future work: SIMD

Sparsemap is scalar-only by design.  No `__builtin_popcount` chains,
no AVX intrinsics, no NEON — nothing target-specific.  The same
source compiles unchanged on x86_64, ARM, RISC-V, and anything else
with a C99 compiler.  This is deliberate: single-file vendoring and
cross-platform reproducibility outrank per-architecture peak
performance for our consumer profile (PostgreSQL extensions,
embedded indexers, undo logs).

The `aligned_alloc` / `aligned_free` slots in `sm_allocator_t` exist
so that adding SIMD later doesn't force another API break.  Two
tiers of work are plausible if a real workload ever justifies it.
Both are deferred until a downstream consumer profiles a hotspot in
a sparsemap operation.

### Tier 1 — vectorize the inner loops without changing the wire format

  - **`sm_cardinality` over MIXED runs.**  Walk chunks scalar-style
    to identify contiguous runs of MIXED bitvecs of length ≥ K
    (~4), gather them into an aligned scratch buffer, run
    AVX2/AVX-512 (or NEON) popcount, accumulate.  Falls back to the
    current scalar loop for short runs and unsupported platforms.
  - **Set ops on MIXED-MIXED chunk-pair runs.**  Same idea applied
    to `sm_union` / `sm_intersection` / `sm_xor` / `sm_difference`:
    when both inputs have aligned MIXED runs, dispatch to a
    vectorized `vpand` / `vpor` / `vpxor` loop.
  - Roughly 500 LOC of intrinsics, runtime CPU dispatch via
    `__attribute__((target("avx2")))` plus a `cpuid` probe, and one
    aligned scratch buffer per inner-loop call (uses
    `sm_allocator_t::aligned_alloc`).
  - Realistic gain: 1.5–3× on dense (mostly-MIXED) maps; near zero
    on sparse maps because the gather overhead eats the win.

### Tier 2 — wire-format extension for native SIMD layout

  - Add a fifth chunk payload type (e.g. `SM_PAYLOAD_DENSE_RUN`)
    that stores N contiguous bitvecs aligned on a 32-byte boundary,
    with a length prefix.  The encoder switches to dense-run mode
    when emitting a long MIXED run.
  - The 2-bit flag space is full (00/01/10/11 all assigned), so
    the new mode requires an escape encoding via the chunk header.
  - Removes the gather step entirely; SIMD ops run directly on the
    serialized bytes.
  - Roughly 1500 LOC, codec rewrite, deserialize-backward-compat
    work, consumer wire format changes.
  - Realistic gain: 4–6× on dense maps.

### Why neither is shipped today

Sparsemap's value proposition is "small wire format, single-file
vendoring, no SIMD assumptions".  Adding SIMD splits the code
(scalar fallback + vector fast path), introduces runtime CPU
dispatch, and forces every consumer's build system to handle
target-feature flags.  We will not pay that cost speculatively.

If and when a real workload pins `sm_cardinality` or set-op
throughput as a measured bottleneck, **Tier 1 is the right answer**
(small, contained, no wire-format change).  Tier 2 is a CRoaring-shaped
rewrite and probably the wrong tool for sparsemap's niche.  Open an
issue with profile data if you hit such a workload.

## License

MIT.  See [LICENSE](LICENSE).
