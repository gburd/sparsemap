# Changelog

All notable changes to the Rust `sparsemap` crate are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com), and
the crate follows [SemVer](https://semver.org).

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
