# Changelog

All notable changes to the Rust `sparsemap` crate are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com), and
the crate follows [SemVer](https://semver.org).

## [5.4.0] - 2026-07-29

Version realigned with the C `sparsemap` library, which is at 5.4.0.
The crate skips 5.2.x/5.3.0 as published versions: those C releases
were ILP32/MSVC portability fixes, an O(N) coalesce fix, and C-side
point-lookup accelerators (`sm_contains_many`, `sm_locator_*`), and
5.4.0 added `sm_add_grow_cursor`.  None change the wire format (still
version 2) or any behavior this crate can observe: the Rust port models
a map as a `BTreeMap`, so the portability/coalesce fixes don't apply
and its `contains`/`rank`/`select` are already `O(log n)` without a
locator; the ascending-append fast path is inherent to `BTreeMap`
inserts.  The bump keeps the version numbers in lockstep; there is no
source or wire change for consumers.

### Note

The C-only acceleration APIs (`sm_locator_*`, `sm_contains_many`) and
`sm_add_grow_cursor` are not surfaced in this crate; they optimize the
C byte-struct's chunk walk, which the `BTreeMap` representation does
not have.  They can be added as inherent methods later without a wire
or version change.

## [5.1.0] - 2026-06-24

Version realigned with the C `sparsemap` library, which is at 5.1.0.
There is no 5.0.0 of this crate: C 5.0.0 was an internal struct-shrink
of the C `sm_t` (per-map allocator removed, cursor externalized,
lineage folded into the capacity word) with no effect on the wire
format or on any behavior this crate can observe -- the Rust port
models a map as a `BTreeMap`, not the C byte struct, so it had nothing
to change.  Jumping straight to 5.1.0 keeps the two version numbers in
lockstep.

### Changed

- **The chunk-count header is now a full 64-bit value** (parity with C
  `sparsemap` 5.1.0).  Through 4.x it was a `u32` in the low 4 bytes of
  the 8-byte header slot, capping a serialized map at `2^32 - 1`
  (~4.29 billion) chunks; for pathologically sparse, scattered data
  (one set bit per 2048-bit chunk) that count -- not the `2^64` index
  space -- was the binding ceiling on cardinality.  Reading and writing
  the whole slot as a little-endian `u64` removes the limit.

### Compatibility

- **Wire format is unchanged at version `2`; no break.**  The header
  slot's high 4 bytes were always zero for any count `< 2^32`, so a
  5.1.0 writer emits bytes identical to 4.x for every real map, and a
  5.1.0 reader sees the same value in any 4.x buffer.  Verified: the
  checked-in C fixtures (emitted by C 4.0.0) are byte-for-byte
  identical to those emitted by C 5.1.0, and both read/write cross-FFI
  directions pass against C 5.1.0.

### Verified

- All unit, doc, and `proptest` model tests pass; `clippy -D warnings`
  and `rustfmt` clean; `no_std` build green.
- Read- and write-direction wire compatibility against C `sparsemap`
  5.1.0.

## [4.0.0] - 2026-06-04

### Fixed

- **Data loss for indices `>= 2^32`** (parity with C `sparsemap` 4.0.0):
  the wire format's chunk-start offsets are now 8 bytes (`u64`) so the
  serialized form addresses the full 64-bit universe the API has always
  advertised.  Previously, `to_bytes()` returned `EncodeError::IndexTooLarge`
  for any bit at or above `2^32`; now it succeeds for any valid index.

### Changed (breaking)

- The on-disk **wire format version** is now `2`.  Buffers written by
  the C `sparsemap` 4.0.0+ deserialize transparently; pre-4.0 buffers
  (v1, 4-byte starts) are rejected with `DecodeError::UnsupportedVersion(1)`
  and need to be re-serialized through C 4.0.0+ first.
- `SparseMap::to_bytes` is now infallible: signature changes from
  `Result<Vec<u8>, EncodeError>` to `Vec<u8>`.  Callers must drop the
  `?`/`.unwrap()`/`.expect()` they had on the result.
- `EncodeError` is removed (the `2^32` cap it expressed no longer
  exists).

### Verified

- Read-direction wire compatibility against checked-in fixtures emitted
  by C 4.0.0's `sm_serialize`.
- Write-direction wire compatibility (`ci/wire_compat.sh`): C 4.0.0
  reads byte-for-byte the buffers Rust produces, recovering identical
  bit sets across six representative shapes.
- `clippy::pedantic` and `rustfmt` clean; `no_std` build green.

## [3.0.1] - 2026-06-03

### Changed

- The crate is now **100% Rust with zero dependencies and no build
  script**.  3.0.0 carried a `cc` build-dependency and a `build.rs`
  that compiled the C library for an in-tree wire-compatibility test;
  a default `cargo add sparsemap` pulled and compiled `cc` (a no-op for
  consumers, since no C source ships in the crate).  Both are removed.
- Cross-language wire compatibility is still verified: the read
  direction (decoding C output) is now an in-crate, pure-Rust test
  against checked-in fixtures produced by the C `sm_serialize`
  (`ci/gen_fixtures.c`); the write direction (the C library reading
  Rust's `to_bytes`) runs in CI via `ci/wire_compat.sh`, which is
  excluded from the published crate.

No API or behavior changes.

## [3.0.0] - 2026-06-03

First release of the Rust port.  The version is aligned with the C
library's 3.0.0 so that "sparsemap 3.0.0" means the same thing in either
language.

### Added

- `SparseMap`: a safe, `no_std`-compatible sparse bitset over `u64`.
- Core operations: `insert`, `remove`, `contains`, `clear`,
  `cardinality`, `is_empty`, `min`, `max`, `rank`, `select`, `span`,
  `insert_range`, `remove_range`, `shifted`.
- Set algebra: `union`, `intersection`, `difference`,
  `symmetric_difference`, `intersects`, `is_subset`, `is_superset`,
  plus the `|`, `&`, `-`, `^` operators and their assigning forms.
- Iteration: `iter` (ascending), `to_vec`, and `IntoIterator` for
  `&SparseMap`.
- Standard traits: `Clone`, `Debug`, `Default`, `PartialEq`, `Eq`,
  `Hash`, `FromIterator<u64>`, `Extend<u64>`.
- `to_bytes` / `from_bytes`: serialization wire-compatible with the C
  library, with `EncodeError` / `DecodeError`.
- `forbid(unsafe_code)`; `no_std` with an `alloc` dependency and an
  optional `std` feature for `std::error::Error` impls.

### Verified

- Property tests against a `BTreeSet` oracle (stateful model, set
  operations, serialization round-trip, parse robustness).
- Cross-implementation wire-format tests: maps serialized by the C
  library deserialize here, and vice versa.
- Clean under `clippy::pedantic` and `rustfmt`.
