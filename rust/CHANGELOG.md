# Changelog

All notable changes to the Rust `sparsemap` crate are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com), and
the crate follows [SemVer](https://semver.org).

## [5.8.2] - 2026-10-06

Matches the C library at 5.8.2, a one-fix patch (an `sm_difference`
clipped-RLE path emitted a structurally invalid, unaligned chunk start;
membership was correct but `sm_validate` rejected it).  The Rust crate
models a map as a `BTreeMap` and is not itself affected; this release
syncs the embedded C source so the crate's C build and wire-compat
harness carry the fix.  Wire format unchanged (version 2); C<->Rust
compatibility re-verified both directions.

## [5.8.1] - 2026-10-05

Matches the C library at 5.8.1, two bug fixes in the C sparse-chunk code
(a `sm_add_many`/`sm_create_from_array` use-after-free on an owned map
that must grow, and an `sm_validate` gap on a sparse descriptor whose
data slots exceed capacity).  The Rust crate models a map as a
`BTreeMap` and is not itself affected; this release syncs the embedded C
source so the crate's C build and wire-compat harness carry the fixes.

### Changed

- Embedded C library updated to 5.8.1.  Wire format unchanged (version
  2); C<->Rust compatibility re-verified both directions.  No change to
  the Rust or Python API.

## [5.8.0] - 2026-09-30

Matches the C library at 5.8.0, a performance release for the C side
(faster `sm_add_range`, O(1) `sm_cardinality`, bulk `sm_add_many`, a
coalesce over-read fix).  The Rust crate models a map as a `BTreeMap`,
so those C hot-path changes don't alter Rust behaviour; this release
syncs the embedded C source so the crate's C build, wire-compat
harness, and any vendored C from this branch carry the fixes.

### Changed

- Embedded C library updated to 5.8.0.  Wire format unchanged (version
  2); C<->Rust wire compatibility re-verified in both directions.  The
  C `sizeof(struct sparsemap)` grew 24->32 (a runtime-only field, never
  serialized) -- this affects only C consumers that embed `sm_t` by
  value, not the Rust crate or the wire format.

## [5.7.0] - 2026-09-26

Matches the C library at 5.7.0, which adds a small-set representation
(Bitmapset-competitive near-zero footprint).  The Rust crate models a
map as a `BTreeMap`, so the small-set form is a C-serialization concern;
this release makes the Rust decoder read it.

### Added

- `from_bytes` decodes the C library's **small-set** wire form (a flat
  word array selected by the header flag) in addition to the chunk and
  RLE-chunk forms, so a map serialized by the C library at 5.7.0 --
  including a near-zero set stored in small mode -- round-trips through
  the Rust port.  The Python binding inherits this.
- Wire-compatibility fixtures covering C small-mode and RLE-chunk
  streams (both read directions), and a bitmapset-inspired corner-case
  parity suite.

### Note

The embedded C source is synced to the C 5.7.0 release, so the crate's
wire-compat harness and any C build from this branch carry the
run-iterator / `sm_split` / `sm_offset` fixes and the zero-warning
cleanup.  Wire format unchanged (version 2); C and Rust remain mutually
readable for same-endian streams.

## [5.6.0] - 2026-09-25

A security-hardening release, matching the C library at 5.6.0.  This is
the first release where the **Rust decoder itself** was hardened, not
just realigned.

### Security

- `SparseMap::from_bytes` now uses checked arithmetic on chunk starts
  and spans and rejects structurally-invalid input (RLE length beyond
  capacity, unaligned or overflowing chunk starts, overlapping spans,
  non-ascending starts) with `DecodeError::Corrupt`.  Debug builds
  previously panicked on adversarial input and release builds -- what
  the Python wheel ships -- wrapped silently, returning maps that
  iterated out of order and gave wrong set-operation results.
  `from_bytes` now never panics and never returns a corrupt map on any
  `&[u8]`; a `cargo-fuzz` target guards this (10M+ runs clean).
- The Python binding raises `ValueError` on malformed bytes rather than
  constructing a corrupt map.

### Note

The C library shares the same version.  Its 5.6.0 hardening (validated
decode at every entry, memory-safety fixes in the chunk-split and
set-path routines, O(chunks) set algebra, one NULL-map contract) is C
representation-specific; the Rust port models a map as a `BTreeMap` and
is structurally immune to those particular bugs, but the decode-safety
work above is its own.  Wire format unchanged (version 2); C and Rust
remain mutually readable for same-endian streams.

## [5.5.1] - 2026-09-07

Version realigned with the C `sparsemap` library, which is at 5.5.1.
That release makes the C helper `__sm_append_data` return `bool` with
`warn_unused_result`, so a caller that forgets to reserve capacity fails
to compile rather than silently overflowing the heap in a release build.

**Nothing to port.**  The hazard is specific to the C representation: a
flat byte buffer written through an unchecked `memcpy` helper, with the
capacity precondition recorded only by an assert that vanishes under
`NDEBUG`.  This crate stores chunks in a `BTreeMap` and grows its
payload `Vec`s, so there is no caller-supplied buffer to overflow and no
unchecked append to guard -- the same reason the 5.5.0 `sm_split` bug
did not exist here (`split_off` returns an owned map).

No source or wire change for consumers.

## [5.5.0] - 2026-09-07

Version realigned with the C `sparsemap` library, which is at 5.5.0.
That release fixes seven correctness bugs in the C chunk codec, three of
them data-loss or corruption.  **None of them affect this crate**, and
each was checked rather than assumed:

- The big-endian descriptor byte-walk, the both-RLE `sm_difference`
  fallback, the `sm_offset` duplicate-chunk-start and gap-fill defects,
  and the two coalesce bugs are all in the C representation's chunk
  codec.  This crate models a map as a `BTreeMap<u32, Chunk>` with an
  explicit enum payload, so it has no descriptor word, no byte-stream
  chunk emitter and no coalesce pass.
- The `sm_select` word-boundary off-by-one and the `SM_PAYLOAD_NONE`
  position disagreement were verified directly against this crate:
  `difference` of two RLE-length runs returns the expected 27-bit tail,
  and `select(128)` on a map holding `[0,128)` and `[500,510)` returns
  `Some(500)` rather than the C bug's `Some(128)`.
- `sm_split`'s missing bounds check has no analogue: `split_off` returns
  an owned map rather than filling a caller-provided buffer.
- `sm_locator_*` is not part of this crate's surface (see 5.4.0).

So the bump keeps the version numbers in lockstep; there is no source or
wire change for consumers.  C-to-Rust and Rust-to-C fixture exchange
passes for all six shapes at 5.5.0.

### Added

- Nothing.  The C release's new `sm_xor_inplace` is already available
  here as `BitXorAssign` (`a ^= &b`).

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
