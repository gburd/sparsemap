# sparsemap

[![crates.io](https://img.shields.io/crates/v/sparsemap.svg)](https://crates.io/crates/sparsemap)
[![docs.rs](https://docs.rs/sparsemap/badge.svg)](https://docs.rs/sparsemap)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A sparse, compressed bitmap (bitset) over `u64` indices, optimized for
workloads with long runs of consecutive set bits.

This is the Rust port of the C
[sparsemap](https://codeberg.org/gregburd/sparsemap) library.  It is
**functionally identical** and **wire-compatible**: a buffer written by
either implementation deserializes in the other (verified in CI by
round-tripping through both).

```toml
[dependencies]
sparsemap = "3"
```

## Why

A plain bitmap needs one bit per element of the universe, even when only
a handful of bits are set.  `sparsemap` divides the universe into 2048-bit
windows and stores only the windows that contain data, each as one of:

- a **run** of entirely-set windows, in `O(1)` space no matter how long
  — two billion consecutive set bits is a handful of bytes; or
- a **dense** 2048-bit window, one bit per bit, for mixed regions.

Empty windows cost nothing.  The result is cheap for the two regimes
that matter — long runs and sparse clusters — and degrades gracefully to
a plain bitmap for random data.

## Example

```rust
use sparsemap::SparseMap;

let mut m = SparseMap::new();
m.insert(42);
m.insert(1024);
assert!(m.contains(42));
assert_eq!(m.cardinality(), 2);

// Build from an iterator, and use idiomatic set algebra.
let a: SparseMap = (0..100).collect();
let b: SparseMap = (50..150).collect();
assert_eq!((&a & &b).cardinality(), 50);    // intersection
assert_eq!((&a | &b).cardinality(), 150);   // union
assert_eq!((&a - &b), (0..50).collect());   // difference

// Rank / select / iterate.
let m: SparseMap = [3, 1, 4, 1, 5, 9].into_iter().collect();
assert_eq!(m.select(0), Some(1));           // first set bit
assert_eq!(m.rank(5), 3);                    // bits below 5: {1,3,4}
assert_eq!(m.iter().collect::<Vec<_>>(), vec![1, 3, 4, 5, 9]);

// Serialize to the C-compatible wire format.
let bytes = m.to_bytes();
assert_eq!(SparseMap::from_bytes(&bytes).unwrap(), m);
```

## Highlights

- **100% safe Rust.** `#![forbid(unsafe_code)]`; no `unsafe`, no raw
  pointers, no build script, and **zero dependencies** (`cargo add
  sparsemap` pulls nothing else).
- **`no_std`.** Depends only on `alloc`.  Disable the default `std`
  feature for embedded targets (you lose only the `std::error::Error`
  impls on the error types).
- **Idiomatic.** `FromIterator`, `Extend`, `IntoIterator`, `Debug`,
  `Default`, `Clone`, `PartialEq`/`Eq`/`Hash`, and the `|`, `&`, `-`,
  `^` operators (plus their `*=` assigning forms).
- **Reads the C library's format.** `from_bytes` decodes buffers written
  by the C `sm_serialize`, addressing the full 64-bit universe;
  verified in-crate against checked-in C-produced fixtures, and in CI
  the reverse direction (C reading Rust's `to_bytes`).

## Comparison with the C library

The Rust port keeps the C library's compression model and serialized
format, but uses an idiomatic `BTreeMap`-of-chunks internally instead of
a hand-managed byte buffer.  One visible consequence: random-access
`contains` / `rank` are `O(log chunks)` here versus the C library's
chunk walk, so they are often *faster* on maps with many chunks; in
exchange the per-map allocator overhead is higher.  The `bench/comparative`
material in the upstream repo quantifies both.

Two source-level differences from the C API, by design:

- the opaque handle is the value type `SparseMap` (not a `sparsemap_t`
  pointer); and
- sub-window partial runs are stored as a dense window rather than a
  length-bearing RLE descriptor, so they round-trip through the wire
  format but may serialize to slightly different (still C-readable)
  bytes.

## MSRV

Rust 1.77.

## License

MIT.
