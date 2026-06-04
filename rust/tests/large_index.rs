//! Regression for the pre-4.0 data-loss bug where any index `>= 2^32`
//! was silently dropped (chunk-start offsets and the scan callback both
//! truncated at 32 bits).  Mirror of `tests/test_large_index.c` in the
//! upstream C library, covering the full set of public operations
//! against indices that straddle and well exceed `2^32`.
//!
//! If this test ever fails, the 4.0.0 wire format / type widening has
//! regressed.

use sparsemap::SparseMap;
use std::collections::BTreeSet;

/// A spread of indices straddling and well past `2^32`.
const KEYS: &[u64] = &[
    0x0000_0000_ffff_ffff, // 2^32 - 1 (last 32-bit value)
    0x0000_0001_0000_0000, // 2^32 exactly (the original report's case)
    0x0000_0001_0000_0001,
    0x0000_4000_0000_0000,
    0x0000_9c40_0000_3039, // mixed high/low bits
    0xffff_ffff_ffff_f800, // near the top of the universe
];

#[test]
fn add_contains_round_trip_above_2_to_32() {
    // The original report: sm_add_grow returns ok, sm_contains then
    // returns false.  Must round-trip in 4.x.
    let mut m = SparseMap::new();
    for &k in KEYS {
        assert!(m.insert(k), "insert reported already-present for {k:#x}");
        assert!(m.contains(k), "contains lost {k:#x} after insert");
    }
    assert_eq!(m.cardinality(), KEYS.len() as u64);
}

#[test]
fn min_max_rank_select_at_64_bit_magnitudes() {
    let m: SparseMap = KEYS.iter().copied().collect();
    let mut sorted: Vec<u64> = KEYS.to_vec();
    sorted.sort_unstable();

    assert_eq!(m.min(), Some(sorted[0]));
    assert_eq!(m.max(), Some(*sorted.last().unwrap()));
    for (i, &bit) in sorted.iter().enumerate() {
        assert_eq!(m.select(i as u64), Some(bit));
        // rank(idx) = number of set bits strictly below idx; bit itself
        // sits at rank position `i`.
        assert_eq!(m.rank(bit), i as u64);
    }
}

#[test]
fn iteration_yields_every_64_bit_index() {
    // The second truncation bug: sm_scan's callback array used to be
    // 32-bit; iter() is the Rust analogue and must deliver full 64-bit
    // positions in ascending order.
    let m: SparseMap = KEYS.iter().copied().collect();
    let mut sorted: Vec<u64> = KEYS.to_vec();
    sorted.sort_unstable();
    let got: Vec<u64> = m.iter().collect();
    assert_eq!(got, sorted);
}

#[test]
fn serialize_round_trip_preserves_64_bit_indices() {
    let m: SparseMap = KEYS.iter().copied().collect();
    let bytes = m.to_bytes();
    let back = SparseMap::from_bytes(&bytes).expect("4.x reads its own output");
    assert_eq!(back, m);
    let want: BTreeSet<u64> = KEYS.iter().copied().collect();
    let got: BTreeSet<u64> = back.iter().collect();
    assert_eq!(got, want);
}

#[test]
fn remove_clears_above_2_to_32() {
    let mut m: SparseMap = KEYS.iter().copied().collect();
    let target = KEYS[1]; // 2^32 exactly
    assert!(m.remove(target));
    assert!(!m.contains(target));
    assert!(!m.remove(target)); // already gone
    assert_eq!(m.cardinality(), (KEYS.len() - 1) as u64);
    // The other large keys are untouched.
    for &k in KEYS.iter().filter(|&&k| k != target) {
        assert!(m.contains(k), "remove touched the wrong bit (lost {k:#x})");
    }
}

#[test]
fn set_ops_at_64_bit_magnitudes() {
    let a: SparseMap = KEYS[..3].iter().copied().collect();
    let b: SparseMap = KEYS[2..].iter().copied().collect();

    let want_union: BTreeSet<u64> = KEYS.iter().copied().collect();
    let want_inter: BTreeSet<u64> = [KEYS[2]].into_iter().collect();
    let want_diff: BTreeSet<u64> = KEYS[..2].iter().copied().collect();

    let to_set = |m: &SparseMap| -> BTreeSet<u64> { m.iter().collect() };
    assert_eq!(to_set(&(&a | &b)), want_union);
    assert_eq!(to_set(&(&a & &b)), want_inter);
    assert_eq!(to_set(&(&a - &b)), want_diff);
}
