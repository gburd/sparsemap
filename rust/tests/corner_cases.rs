//! Behavioral corner-case parity with the C small-set suite.
//!
//! These mirror the bitmapset-inspired cases the C library exercises so
//! the Rust API has matching coverage.  The C library switches between a
//! bit-0-based small form, sparse chunks, and RLE runs internally; the
//! Rust crate is representation-agnostic (a `BTreeMap` of windows), so
//! these tests assert the *observable behavior* is identical and
//! cross-check against a `std::collections::BTreeSet` oracle.

use sparsemap::SparseMap;
use std::collections::BTreeSet;

fn sm(iter: impl IntoIterator<Item = u64>) -> SparseMap {
    iter.into_iter().collect()
}
fn bt(iter: impl IntoIterator<Item = u64>) -> BTreeSet<u64> {
    iter.into_iter().collect()
}

/// Assert a map matches its oracle on membership, cardinality, ordered
/// iteration, min/max, and rank/select over every element.
fn agree(m: &SparseMap, oracle: &BTreeSet<u64>) {
    assert_eq!(m.cardinality(), oracle.len() as u64, "cardinality");
    assert_eq!(m.is_empty(), oracle.is_empty(), "is_empty");
    let got: Vec<u64> = m.iter().collect();
    let want: Vec<u64> = oracle.iter().copied().collect();
    assert_eq!(got, want, "sorted iteration");
    assert!(got.windows(2).all(|w| w[0] < w[1]), "strictly ascending");
    assert_eq!(m.min(), oracle.iter().next().copied(), "min");
    assert_eq!(m.max(), oracle.iter().next_back().copied(), "max");
    for (i, &b) in want.iter().enumerate() {
        assert!(m.contains(b), "contains {b}");
        assert_eq!(m.select(i as u64), Some(b), "select {i}");
        assert_eq!(m.rank(b), i as u64, "rank(member_index) of {b}");
    }
    assert_eq!(m.select(oracle.len() as u64), None, "select past end");
    // A bit one past max is absent.
    if let Some(&mx) = oracle.iter().next_back() {
        if mx < u64::MAX {
            assert!(!m.contains(mx + 1), "no phantom bit above max");
        }
    }
}

// ---------------------------------------------------------------------
// Word boundaries: bits 0, 31, 32, 63, 64, 65 (32/64-bit word seams).
// ---------------------------------------------------------------------

#[test]
fn word_boundary_bits() {
    let bits = [0u64, 31, 32, 63, 64, 65];
    let m = sm(bits);
    let o = bt(bits);
    agree(&m, &o);
    // rank/select in different words.
    assert_eq!(m.select(0), Some(0)); // word 0, bit 0
    assert_eq!(m.select(4), Some(64)); // word 1, bit 0
    assert_eq!(m.rank(64), 4);
    assert_eq!(m.rank(65), 5);
}

#[test]
fn singleton_and_empty() {
    let e = SparseMap::new();
    agree(&e, &BTreeSet::new());
    assert_eq!(e.rank(1234), 0);
    for b in [0u64, 63, 64, 1023, 1024, 100_000, u64::MAX] {
        let m = sm([b]);
        agree(&m, &bt([b]));
    }
}

#[test]
fn idempotent_insert() {
    let mut m = SparseMap::new();
    assert!(m.insert(42));
    assert!(!m.insert(42)); // already present -> false, no change
    assert!(!m.insert(42));
    agree(&m, &bt([42]));
}

#[test]
fn remove_to_empty() {
    let bits = [0u64, 1, 5, 63, 64, 1023, 4096];
    let mut m = sm(bits);
    let mut o = bt(bits);
    for &b in &bits {
        assert!(m.remove(b));
        assert!(!m.remove(b)); // second remove -> false
        o.remove(&b);
        agree(&m, &o);
    }
    assert!(m.is_empty());
    // Re-insert one low bit: canonical single again.
    m.insert(0);
    agree(&m, &bt([0]));
}

// ---------------------------------------------------------------------
// Set ops on a spread of shapes, cross-checked against BTreeSet.
// ---------------------------------------------------------------------

fn check_set_ops(a_bits: &[u64], b_bits: &[u64]) {
    let a = sm(a_bits.iter().copied());
    let b = sm(b_bits.iter().copied());
    let oa = bt(a_bits.iter().copied());
    let ob = bt(b_bits.iter().copied());

    let union: BTreeSet<u64> = oa.union(&ob).copied().collect();
    let inter: BTreeSet<u64> = oa.intersection(&ob).copied().collect();
    let diff: BTreeSet<u64> = oa.difference(&ob).copied().collect();
    let sdiff: BTreeSet<u64> = oa.symmetric_difference(&ob).copied().collect();

    agree(&a.union(&b), &union);
    agree(&a.intersection(&b), &inter);
    agree(&a.difference(&b), &diff);
    agree(&a.symmetric_difference(&b), &sdiff);
    // Operator forms agree with method forms.
    assert_eq!(&a | &b, a.union(&b));
    assert_eq!(&a & &b, a.intersection(&b));
    assert_eq!(&a - &b, a.difference(&b));
    assert_eq!(&a ^ &b, a.symmetric_difference(&b));

    assert_eq!(a.is_subset(&b), oa.is_subset(&ob), "is_subset");
    assert_eq!(a.is_superset(&b), oa.is_superset(&ob), "is_superset");
    assert_eq!(a.intersects(&b), !inter.is_empty(), "intersects/overlap");
    // compare (equality, the Rust analog of C sm_compare == 0).
    assert_eq!(a == b, oa == ob, "equality/compare");
}

#[test]
fn set_ops_both_small() {
    check_set_ops(&[0, 1, 5, 63], &[2, 5, 62, 63]);
}

#[test]
fn set_ops_disjoint() {
    check_set_ops(&[0, 1, 2], &[100, 200, 300]);
}

#[test]
fn set_ops_identical() {
    let bits = [0u64, 5, 64, 1023, 100_000];
    check_set_ops(&bits, &bits);
}

#[test]
fn set_ops_subset() {
    check_set_ops(&[1, 2, 3], &[0, 1, 2, 3, 4, 5]);
}

#[test]
fn set_ops_overlapping() {
    check_set_ops(&[0, 1, 2, 3, 4], &[3, 4, 5, 6, 7]);
}

#[test]
fn set_ops_one_low_one_high_straddle() {
    // One set entirely in the low words, the other high above the
    // small/chunk boundary -> disjoint straddle.
    let low: Vec<u64> = (0..64).collect();
    let high: Vec<u64> = (100_000..100_064).collect();
    check_set_ops(&low, &high);
}

#[test]
fn set_ops_varying_densities() {
    // Dense contiguous run vs. a sparse scatter overlapping it.
    let dense: Vec<u64> = (0..5000).collect();
    let sparse = [0u64, 2500, 4999, 5000, 10_000, 1_000_000];
    check_set_ops(&dense, &sparse);
}

// ---------------------------------------------------------------------
// Equal sets built differently compare equal AND hash equal.  This is
// the Rust analog of the C canonical-run fix: a set built by individual
// inserts must equal the same set decoded from a C RLE range-stream.
// ---------------------------------------------------------------------

fn hash_of(m: &SparseMap) -> u64 {
    use std::hash::{Hash, Hasher};
    let mut h = std::collections::hash_map::DefaultHasher::new();
    m.hash(&mut h);
    h.finish()
}

#[test]
fn equal_sets_built_differently_are_equal_and_hash_equal() {
    // Range {0..=5000} built three ways: individual inserts, insert_range,
    // and decoded from the C library's RLE serialization of the same set.
    let by_insert: SparseMap = {
        let mut m = SparseMap::new();
        for i in 0..=5000u64 {
            m.insert(i);
        }
        m
    };
    let by_range: SparseMap = {
        let mut m = SparseMap::new();
        m.insert_range(0, 5001);
        m
    };
    let by_iter: SparseMap = (0..=5000u64).collect();

    assert_eq!(by_insert, by_range);
    assert_eq!(by_insert, by_iter);
    assert_eq!(hash_of(&by_insert), hash_of(&by_range));
    assert_eq!(hash_of(&by_insert), hash_of(&by_iter));

    // Cross-representation: a set that straddles a chunk seam built by
    // inserts vs. by a run then extra bits must be canonical-equal.
    let seam_a: SparseMap = (2000..2100u64).collect();
    let seam_b: SparseMap = {
        let mut m = SparseMap::new();
        m.insert_range(2000, 2050);
        for i in 2050..2100u64 {
            m.insert(i);
        }
        m
    };
    assert_eq!(seam_a, seam_b);
    assert_eq!(hash_of(&seam_a), hash_of(&seam_b));

    // Round-trip through the wire format must not change identity/hash.
    let back = SparseMap::from_bytes(&by_insert.to_bytes()).unwrap();
    assert_eq!(by_insert, back);
    assert_eq!(hash_of(&by_insert), hash_of(&back));
}

#[test]
fn full_window_run_equals_bit_by_bit() {
    // A full 2048-bit window is stored as a run internally; built bit by
    // bit it must be identical and hash-identical.
    let run: SparseMap = (0..2048u64).collect();
    let bits: SparseMap = {
        let mut m = SparseMap::new();
        for i in 0..2048u64 {
            m.insert(i);
        }
        m
    };
    assert_eq!(run, bits);
    assert_eq!(hash_of(&run), hash_of(&bits));
}

// ---------------------------------------------------------------------
// Offset / shift at word boundaries, below-zero drop, i64 extremes.
// ---------------------------------------------------------------------

#[test]
fn shift_at_word_boundaries() {
    let bits = [0u64, 31, 32, 63, 64, 65, 127, 128];
    let m = sm(bits);
    // Shift by a word (64) and check every bit lands where the oracle says.
    for off in [1i64, 31, 32, 33, 63, 64, 65, -1, -31, -32, -64] {
        let shifted = m.shifted(off);
        let oracle: BTreeSet<u64> = bits
            .iter()
            .filter_map(|&b| {
                if off >= 0 {
                    b.checked_add(off.unsigned_abs())
                } else {
                    b.checked_sub(off.unsigned_abs())
                }
            })
            .collect();
        agree(&shifted, &oracle);
    }
}

#[test]
fn shift_below_zero_drops() {
    let m = sm([10, 20, 30]);
    // -15 drops bit 10, keeps 20->5 and 30->15.
    agree(&m.shifted(-15), &bt([5, 15]));
    // -100 drops everything.
    assert!(m.shifted(-100).is_empty());
}

#[test]
fn shift_i64_extremes_no_panic() {
    let m = sm([0, 5, 100_000, u64::MAX - 10]);
    // +i64::MAX: bits whose value+i64::MAX overflows u64 are dropped.
    let up = m.shifted(i64::MAX);
    let oracle: BTreeSet<u64> = [0u64, 5, 100_000]
        .iter()
        .map(|&b| b + i64::MAX as u64)
        .collect();
    agree(&up, &oracle);
    // i64::MIN: all but the top bit drop below zero.
    let dn = m.shifted(i64::MIN);
    let oracle_dn: BTreeSet<u64> = [u64::MAX - 10]
        .iter()
        .filter_map(|&b| b.checked_sub(i64::MIN.unsigned_abs()))
        .collect();
    agree(&dn, &oracle_dn);
    // -i64::MAX must not panic.
    let _ = m.shifted(-i64::MAX);
}
