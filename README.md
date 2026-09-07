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

- Multi-threaded workloads: sparsemap is not thread-safe, and there
  is no plan to make it so.  Concurrent readers of an unmutated map
  are fine; any mutation needs external synchronisation.  (An
  `experiment/thread-safe` branch explored a lock-free variant in
  2024 and was not pursued; treat it as an archive, not a roadmap.)
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
#include <sparsemap/sm.h>

sm_t *map = sm_create(4096);
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

### Vendoring and symbol prefixing

The library is exactly two files, `sm.h` and `sm.c`; vendoring is a
two-file copy.  If you need two independently-vendored copies of
sparsemap to coexist in one binary, rename every public symbol by
defining `SPARSEMAP_PREFIX` before including the header:

```c
#define SPARSEMAP_PREFIX myapp_
#include <sparsemap/sm.h>

myapp_sm_t *m = myapp_sm_create(4096);   /* renamed */
myapp_sm_add(m, 42);
```

Every public function and type picks up the prefix at both declaration
and call sites (Berkeley DB `--with-uniquename` style).  Compile-time
macros (`SM_IDX_MAX`, the `SM_VERSION_*` values, enum constants) and
the serialized wire format are unaffected.

## Versioning and history

Releases follow [SemVer](https://semver.org).  **3.0.0 is the first
formal public release.**  The pre-3.0 development history (the library
grew up vendored inside other projects) is preserved on the
`archive/v2.3.0` tag for archaeology; the published history starts
clean at 3.0.0.

### API stability vs ABI stability

Sparsemap promises **source-level API stability** within a major
version: function signatures, macro names, and behavior of public
`sm_*` symbols do not change in a way that breaks compiling
consumer code.

Sparsemap **does not** promise ABI stability of the `struct sparsemap`
layout.  `sizeof(sm_t)` and the offsets of its fields may change in any
minor release.  Consumers must:

- Always allocate `sm_t` via `sm_create()` or `sm_wrap()` -- never
  embed it inline in another struct (unless you opt in with
  `SM_EXPOSE_STRUCT`, see below), never `sizeof(sm_t)` for an on-disk
  format, never `memcpy(struct, ...)` it.
- Treat the type as opaque: access only via `sm_*` accessors.
- Recompile (not just relink) after upgrading sparsemap.

The same recompile-not-relink rule applies to `sm_cursor_t`: it is a
complete, caller-stack-allocated type, and a minor release may add a
trailing field (as 5.2.0 did, for a coalescing performance hint).
Always initialize with `SM_CURSOR_INIT` and recompile after upgrading;
never persist a cursor or depend on its `sizeof`.

The same applies to `sm_cursor_cached_t` (the 5.3.0 fixed-size MRU
lookup cache): initialize with `SM_CURSOR_CACHED_INIT`, recompile after
upgrading, and reset it after any mutation.  `sm_locator_t` (the 5.3.0
transient O(sqrt n) point-lookup / rank / select index) is opaque --
build it with `sm_locator_build`, use it only through the `sm_locator_*`
functions, free it with `sm_locator_free`, and rebuild it after any
mutation (a stale locator still returns correct results, just without
the speedup).

The **wire format** produced by `sm_serialize` and consumed by
`sm_open`/`sm_deserialize` *is* stable and is preserved across the
3.x series.  This is the contract that matters for on-disk consumers.

### Embedding `sm_t` by value (`SM_EXPOSE_STRUCT`)

By default `sm_t` is an incomplete type, so the compiler rejects any
attempt to embed it by value, take its `sizeof`, or otherwise depend
on its layout.  A few consumers genuinely need the layout -- for
example to place an `sm_t` inline inside a shared-memory control
block rather than behind a pointer.  Define `SM_EXPOSE_STRUCT` before
including the header to make the full definition visible:

```c
#define SM_EXPOSE_STRUCT
#include <sparsemap/sm.h>

struct my_state {
    sm_t map;        /* embedded by value, not a pointer */
    int  generation;
};
```

This is an explicit opt-out of the ABI-opacity guarantee above: code
that embeds `sm_t` by value must be **recompiled** whenever the
struct layout changes (it may grow or shrink between minor releases).
The struct definition lives in `sm.h`
so an embedding consumer never has to copy it by hand -- doing so is
how a stale duplicate drifts out of sync with the library.  The
serialized wire format is identical whether or not the macro is set.

### Migrating from a pre-3.0 vendored copy

3.0.0 makes two source-level breaks, both mechanical:

- **The opaque type is now `sm_t`, not `sparsemap_t`.**  Migrate with
  `sed -i 's/\\bsparsemap_t\\b/sm_t/g' your_files.c`.
- **The vendoring prefix macro is `SPARSEMAP_PREFIX`, not `SM_PREFIX`.**
  Rename it if you set it.

Everything else -- the `sm_*` function names, their signatures and
behavior, and the serialized wire format -- is unchanged from the
latest pre-3.0 vendored copies.  See `docs/MIGRATION.md` for the full
checklist.

### Migrating from 4.x to 5.0

5.0.0 shrinks `sm_t` from 112 bytes to **24** (the original
`{capacity, used, data}` footprint) by removing per-map state.  Three
source-level breaks, all mechanical:

- **No per-map allocator.**  `sm_create_with_allocator()` is removed;
  the allocator is process-global via `sm_set_allocator()` only
  (CRoaring's model).  The hook struct is now a minimal
  `{ malloc, realloc, free }` triple -- the old `alloc_zero`,
  `aligned_alloc`, `aligned_free`, and `aux` fields are gone, and the
  hooks no longer take an `aux` argument.  Replace any
  `sm_create_with_allocator(n, hooks)` with
  `sm_set_allocator(hooks); m = sm_create(n);`.
- **The cursor is now caller-owned.**  `sm_contains`, `sm_next_member`,
  and `sm_prev_member` take a trailing `sm_cursor_t *cur` argument and
  are `const sm_t *`.  Pass `NULL` for no acceleration (a one-off
  lookup), or declare `sm_cursor_t c = SM_CURSOR_INIT;` and thread
  `&c` through a monotonic sweep on an unmutated map.  A cursor is
  invalidated by any mutation; using a stale one is undefined --
  reset it (or hold a lock) across writes.  `sm_rank` / `sm_select` /
  `sm_span` are unchanged (they always walk from the head).
- **Building with single `sm_add` in a loop is now O(N^2).**  The
  ascending-build acceleration moved out of the struct; bulk builders
  should call `sm_add_many()` / `sm_add_many_grow()`, which keep a
  transient internal cursor and stay O(N).

The serialized wire format is **unchanged**: 4.x bytes deserialize
under 5.0.

## SIMD: a settled decision, not a roadmap item

Sparsemap is scalar-only by design, and intends to stay that way.
No hand-written AVX or NEON intrinsics, nothing target-specific:
the same source compiles unchanged on x86_64, ARM, RISC-V, s390x
and SPARC.  Single-file vendoring and cross-platform
reproducibility outrank per-architecture peak performance for the
consumer profile (PostgreSQL extensions, embedded indexers, undo
logs).

Two measurements back this up rather than leaving it a matter of
taste:

- The compiler already vectorizes the inner loops that matter.
  Building with `-O3 -march=native` auto-vectorizes five loops in
  `sm.c` with no source change (`gcc -fopt-info-vec`).
- The payoff is small.  Against `-O2`, `-O3 -march=native` moves
  `sm_union` by roughly 6-13% on the microbenchmark (random/random
  153.2 -> 144.6 us median; random/dense 39.5 -> 34.4 us).  That is
  the same order as the gather overhead any hand-written kernel
  would have to pay back first.

So a consumer who wants vectorization can have most of the
available win today by compiling with their own `-march`, without
sparsemap growing a second code path, runtime CPU dispatch, or
target-feature flags in every downstream build system.

The allocator hooks (`sm_set_allocator`) are deliberately a minimal
`malloc`/`realloc`/`free` triple, matching CRoaring's
`roaring_init_memory_hook`.  If a future SIMD effort ever needs
aligned allocation it can add an aligned-alloc hook then; the
current API carries no speculative slots.

### What it would take, if a profile ever justified it

Kept here so the analysis does not have to be redone, not as
planned work.  **Neither tier is scheduled.**

Tier 1, no wire-format change: identify contiguous runs of MIXED
bitvecs (length >= ~4) while walking chunks, gather them into an
aligned scratch buffer, run vectorized popcount for
`sm_cardinality` or `vpand`/`vpor`/`vpxor` for the set operations,
fall back to the scalar loop otherwise.  Roughly 500 lines of
intrinsics, runtime dispatch via `__attribute__((target("avx2")))`
plus a `cpuid` probe, one aligned scratch buffer per inner-loop
call.  Realistic gain 1.5-3x on dense (mostly-MIXED) maps, near
zero on sparse maps because the gather overhead eats the win.

Tier 2, wire-format extension: add a payload type storing N
contiguous bitvecs aligned to 32 bytes with a length prefix, so
SIMD runs directly on the serialized bytes with no gather.  The
2-bit flag space is full (00/01/10/11 all assigned), so it needs an
escape encoding in the chunk header.  Roughly 1500 lines, a codec
rewrite, deserialize backward-compatibility work and a consumer
wire-format migration.  Realistic gain 4-6x on dense maps.

If a real workload ever pins `sm_cardinality` or set-op throughput
as a measured bottleneck, Tier 1 is the right answer -- small,
contained, no wire-format change.  Tier 2 is a CRoaring-shaped
rewrite and probably the wrong tool for sparsemap's niche.  Open an
issue with profile data.

## License

MIT.  See [LICENSE](LICENSE).
