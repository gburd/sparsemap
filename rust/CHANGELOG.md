# Changelog

All notable changes to the Rust `sparsemap` crate are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com), and
the crate follows [SemVer](https://semver.org).

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
