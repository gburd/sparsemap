//! Wire-format compatibility with the C sparsemap library (read
//! direction), as a pure-Rust test.
//!
//! The byte constants below were produced by the C library's
//! `sm_serialize` (see `ci/gen_fixtures.c`).  Deserializing them here
//! and recovering the exact bit set proves the Rust port reads the C
//! library's output.  Because the constants are checked in, this needs
//! no C compiler and no build script — the crate stays 100% Rust.
//!
//! The reverse direction (the C library reading Rust-produced bytes) is
//! exercised by `ci/wire_compat.sh` in CI, which has a C toolchain and
//! the C source; it is deliberately kept out of the published crate.

use sparsemap::SparseMap;
use std::collections::BTreeSet;

/// Deserialize C-produced `bytes`, recover the set, and confirm it
/// equals `expected`.  Also confirm Rust's own re-encoding round-trips.
fn check(bytes: &[u8], expected: &BTreeSet<u64>) {
    let m = SparseMap::from_bytes(bytes).expect("Rust must deserialize C output");
    let got: BTreeSet<u64> = m.iter().collect();
    assert_eq!(&got, expected, "Rust read of C bytes diverged");

    let re = m.to_bytes().unwrap();
    let back = SparseMap::from_bytes(&re).unwrap();
    assert_eq!(back, m, "Rust re-encode round-trip diverged");
}

fn set(iter: impl IntoIterator<Item = u64>) -> BTreeSet<u64> {
    iter.into_iter().collect()
}

// --- fixtures emitted by the C library (ci/gen_fixtures.c) ---

/// C `sm_serialize` output for the empty set (0 bits, 20 bytes).
const EMPTY: &[u8] = &[
    115, 109, 49, 48, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
];
/// C `sm_serialize` output for `{42}` (1 bit, 40 bytes).
const SINGLE: &[u8] = &[
    115, 109, 49, 48, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 4, 0, 0,
];
/// C `sm_serialize` output for `{1,2,3,2047,2048,4096,100000}` (7 bits, 108 bytes).
const SCATTERED: &[u8] = &[
    115, 109, 49, 48, 1, 1, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0,
    0, 128, 14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 0, 8, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 128, 1,
    0, 0, 0, 0, 0, 0, 0, 32, 0, 0, 0, 0, 0, 1, 0, 0, 0,
];
/// C `sm_serialize` output for `0..5000` (5000 bits, 32 bytes — RLE).
const RUN_5000: &[u8] = &[
    115, 109, 49, 48, 1, 1, 0, 0, 136, 19, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 136, 19, 0, 0,
    0, 12, 0, 64,
];
/// C `sm_serialize` output for `0..8192` (four full windows, 32 bytes — RLE).
const RUN_4WINDOWS: &[u8] = &[
    115, 109, 49, 48, 1, 1, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0, 0,
    16, 0, 64,
];
/// C `sm_serialize` output for `0..100 ∪ 10000..10050` (150 bits, 68 bytes).
const TWO_CLUSTERS: &[u8] = &[
    115, 109, 49, 48, 1, 1, 0, 0, 150, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 11, 0, 0, 0, 0,
    0, 0, 0, 255, 255, 255, 255, 15, 0, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10, 0, 0, 255, 255,
    255, 255, 255, 255, 3, 0, 0, 0, 0, 0, 0, 0,
];
/// C `sm_serialize` output for `1000..7000` (6000 bits, 52 bytes).
const OFFSET_RUN: &[u8] = &[
    115, 109, 49, 48, 1, 1, 0, 0, 112, 23, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128,
    255, 255, 255, 255, 0, 0, 0, 0, 0, 255, 255, 255, 0, 8, 0, 0, 88, 19, 0, 0, 0, 12, 0, 64,
];

#[test]
fn c_empty() {
    let m = SparseMap::from_bytes(EMPTY).expect("empty deserializes");
    assert!(m.is_empty());
}

#[test]
fn c_single() {
    check(SINGLE, &set([42]));
}

#[test]
fn c_scattered() {
    check(SCATTERED, &set([1, 2, 3, 2047, 2048, 4096, 100_000]));
}

#[test]
fn c_run_5000() {
    check(RUN_5000, &set(0..5000));
}

#[test]
fn c_run_4windows() {
    check(RUN_4WINDOWS, &set(0..8192));
}

#[test]
fn c_two_clusters() {
    check(TWO_CLUSTERS, &set((0..100).chain(10_000..10_050)));
}

#[test]
fn c_offset_run() {
    check(OFFSET_RUN, &set(1000..7000));
}
