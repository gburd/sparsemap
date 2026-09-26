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

    let re = m.to_bytes();
    let back = SparseMap::from_bytes(&re).unwrap();
    assert_eq!(back, m, "Rust re-encode round-trip diverged");
}

fn set(iter: impl IntoIterator<Item = u64>) -> BTreeSet<u64> {
    iter.into_iter().collect()
}

// --- fixtures emitted by the C library (ci/gen_fixtures.c) ---

/// C `sm_serialize` output for the empty set (0 bits, 20 bytes).
const EMPTY: &[u8] = &[
    115, 109, 49, 48, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
];
/// C `sm_serialize` output for `{42}` (1 bit, 40 bytes).
const SINGLE: &[u8] = &[
    115, 109, 49, 48, 2, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0,
];
/// C `sm_serialize` output for `{1,2,3,2047,2048,4096,100000}` (7 bits, 108 bytes).
const SCATTERED: &[u8] = &[
    115, 109, 49, 48, 2, 1, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 2, 0, 0, 0, 0, 0, 0, 128, 14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 0, 8, 0, 0,
    0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 2, 0, 0,
    0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 128, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0, 0,
    0, 0, 1, 0, 0, 0,
];
/// C `sm_serialize` output for `0..5000` (5000 bits, 32 bytes — RLE).
const RUN_5000: &[u8] = &[
    115, 109, 49, 48, 2, 1, 0, 0, 136, 19, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 136, 19, 0, 0, 0, 12, 0, 64,
];
/// C `sm_serialize` output for `0..8192` (four full windows, 32 bytes — RLE).
const RUN_4WINDOWS: &[u8] = &[
    115, 109, 49, 48, 2, 1, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 32, 0, 0, 0, 16, 0, 64,
];
/// C `sm_serialize` output for `0..100 ∪ 10000..10050` (150 bits, 68 bytes).
const TWO_CLUSTERS: &[u8] = &[
    115, 109, 49, 48, 2, 1, 0, 0, 150, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 11, 0, 0, 0, 0, 0, 0, 0, 255, 255, 255, 255, 15, 0, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 10, 0, 0, 255, 255, 255, 255, 255, 255, 3, 0, 0, 0, 0, 0, 0, 0,
];
/// C `sm_serialize` output for `1000..7000` (6000 bits, 52 bytes).
const OFFSET_RUN: &[u8] = &[
    115, 109, 49, 48, 2, 1, 0, 0, 112, 23, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 128, 255, 255, 255, 255, 0, 0, 0, 0, 0, 255, 255, 255, 0, 8, 0, 0, 0, 0, 0,
    0, 88, 19, 0, 0, 0, 12, 0, 64,
];

// --- small-mode streams (header byte out[6] set, body top bit set) ---

/// C `sm_serialize` output for the SMALL_ZERO set (1 bits, 32 bytes, small-mode).
const SMALL_ZERO: &[u8] = &[
    115, 109, 49, 48, 2, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 128, 1, 0, 0, 0, 0,
    0, 0, 0,
];
/// C `sm_serialize` output for the SMALL_WORD0 set (4 bits, 32 bytes, small-mode).
const SMALL_WORD0: &[u8] = &[
    115, 109, 49, 48, 2, 1, 1, 0, 4, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 128, 35, 0, 0, 0, 0,
    0, 0, 128,
];
/// C `sm_serialize` output for the SMALL_FULLWORD set (64 bits, 32 bytes, small-mode).
const SMALL_FULLWORD: &[u8] = &[
    115, 109, 49, 48, 2, 1, 1, 0, 64, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 128, 255, 255, 255,
    255, 255, 255, 255, 255,
];
/// C `sm_serialize` output for the SMALL_TWOWORDS set (2 bits, 40 bytes, small-mode).
const SMALL_TWOWORDS: &[u8] = &[
    115, 109, 49, 48, 2, 1, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 128, 32, 0, 0, 0, 0,
    0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0,
];
/// C `sm_serialize` output for {3,17,88,200,511,900,1023} (7 bits, 88 bytes).
/// A sparse scatter under 1024: C keeps this in CHUNK mode (the small
/// form's word count would hit its cap), so this exercises the sparse
/// chunk decode for a set whose bits span the small/chunk boundary.
const SMALL_SCATTER: &[u8] = &[
    115, 109, 49, 48, 2, 1, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 138, 128, 0, 160, 0, 0, 0, 0, 8, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128,
];
/// C `sm_serialize` output for {0} built empty-then-single (1 bit, 32 bytes,
/// small-mode).  Byte-identical to SMALL_ZERO -- proves that a map churned
/// through insert/remove back to a single low bit serializes canonically.
const SMALL_EMPTYTHENSINGLE: &[u8] = &[
    115, 109, 49, 48, 2, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 128, 1, 0, 0, 0, 0,
    0, 0, 0,
];

// --- RLE-chunk streams (single RLE descriptor per <=2^31-bit span) ---

/// C `sm_serialize` output for {0..=1000} (1001 bits, 40 bytes, one RLE chunk).
const RLE_RUN_0_1000: &[u8] = &[
    115, 109, 49, 48, 2, 1, 0, 0, 233, 3, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 233, 3, 0, 0, 0, 4, 0, 64,
];
/// C `sm_serialize` output for {0..=1023} (1024 bits, 40 bytes, one RLE chunk).
const RLE_RUN_0_1023: &[u8] = &[
    115, 109, 49, 48, 2, 1, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 4, 0, 0, 0, 4, 0, 64,
];
/// C `sm_serialize` output for {0..=5000} (5001 bits, 40 bytes, spanning chunks).
const RLE_RUN_0_5000: &[u8] = &[
    115, 109, 49, 48, 2, 1, 0, 0, 137, 19, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 137, 19, 0, 0, 0, 12, 0, 64,
];
/// C `sm_serialize` output for {2048..=4095} (2048 bits, 40 bytes, RLE chunk
/// NOT starting at 0 -- proves a non-zero run start decodes at the right base).
const RLE_RUN_2048_4095: &[u8] = &[
    115, 109, 49, 48, 2, 1, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0,
    0, 0, 255, 255, 255, 255, 255, 255, 255, 255,
];

// --- mixed streams (RLE + sparse chunks in one map) ---

/// C `sm_serialize` output for {0..=1200} + {50000,50003,123456}
/// (1204 bits, 88 bytes): a dense low run promoted to a chunk plus a
/// high sparse chunk in the same map.
const MIXED_LOWRUN_HIGHSPARSE: &[u8] = &[
    115, 109, 49, 48, 2, 1, 0, 0, 180, 4, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 177, 4, 0, 0, 0, 4, 0, 64, 0, 192, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 9,
    0, 0, 0, 0, 0, 0, 224, 1, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
];
/// C `sm_serialize` output for {0,1,1023,1024,1025,2050} (6 bits, 88 bytes):
/// a set straddling the 1024 small/chunk boundary, kept in chunk mode.
const MIXED_STRADDLE_1024: &[u8] = &[
    115, 109, 49, 48, 2, 1, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 2, 0, 0, 128, 2, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 3, 0, 0, 0,
    0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0,
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

// --- small-mode ---

#[test]
fn c_small_zero() {
    check(SMALL_ZERO, &set([0]));
}

#[test]
fn c_small_word0() {
    check(SMALL_WORD0, &set([0, 1, 5, 63]));
}

#[test]
fn c_small_fullword() {
    check(SMALL_FULLWORD, &set(0..64));
}

#[test]
fn c_small_twowords() {
    check(SMALL_TWOWORDS, &set([5, 70]));
}

#[test]
fn c_small_scatter() {
    check(SMALL_SCATTER, &set([3, 17, 88, 200, 511, 900, 1023]));
}

#[test]
fn c_small_empty_then_single() {
    check(SMALL_EMPTYTHENSINGLE, &set([0]));
    // Same logical set built the two ways C can reach it decodes equal.
    assert_eq!(
        SparseMap::from_bytes(SMALL_EMPTYTHENSINGLE).unwrap(),
        SparseMap::from_bytes(SMALL_ZERO).unwrap()
    );
}

// --- RLE chunks ---

#[test]
fn c_rle_run_0_1000() {
    check(RLE_RUN_0_1000, &set(0..=1000));
}

#[test]
fn c_rle_run_0_1023() {
    check(RLE_RUN_0_1023, &set(0..=1023));
}

#[test]
fn c_rle_run_0_5000() {
    check(RLE_RUN_0_5000, &set(0..=5000));
}

#[test]
fn c_rle_run_2048_4095() {
    check(RLE_RUN_2048_4095, &set(2048..=4095));
}

// --- mixed ---

#[test]
fn c_mixed_lowrun_highsparse() {
    check(
        MIXED_LOWRUN_HIGHSPARSE,
        &set((0..=1200).chain([50000, 50003, 123456])),
    );
}

#[test]
fn c_mixed_straddle_1024() {
    check(MIXED_STRADDLE_1024, &set([0, 1, 1023, 1024, 1025, 2050]));
}
