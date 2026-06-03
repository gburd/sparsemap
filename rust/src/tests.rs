//! Unit tests for the core data structure.  Property tests against a
//! `BTreeSet` oracle live in `tests/`.

use super::*;
use alloc::vec;
use alloc::vec::Vec;

#[test]
fn empty() {
    let m = SparseMap::new();
    assert!(m.is_empty());
    assert_eq!(m.cardinality(), 0);
    assert_eq!(m.min(), None);
    assert_eq!(m.max(), None);
    assert!(!m.contains(0));
    assert_eq!(m.select(0), None);
    assert_eq!(m.rank(100), 0);
}

#[test]
fn insert_contains_remove() {
    let mut m = SparseMap::new();
    assert!(m.insert(42));
    assert!(!m.insert(42)); // already present
    assert!(m.contains(42));
    assert!(!m.contains(43));
    assert_eq!(m.cardinality(), 1);
    assert!(m.remove(42));
    assert!(!m.remove(42));
    assert!(m.is_empty());
}

#[test]
fn spans_many_windows() {
    let mut m = SparseMap::new();
    for i in [0u64, 2047, 2048, 4096, 1_000_000] {
        m.insert(i);
    }
    assert_eq!(m.cardinality(), 5);
    assert_eq!(m.min(), Some(0));
    assert_eq!(m.max(), Some(1_000_000));
    assert_eq!(
        m.iter().collect::<Vec<_>>(),
        vec![0, 2047, 2048, 4096, 1_000_000]
    );
}

#[test]
fn full_window_becomes_run() {
    let mut m = SparseMap::new();
    for i in 0..CHUNK_BITS {
        m.insert(i);
    }
    // A single full window is canonicalized to a run.
    assert_eq!(m.chunks.len(), 1);
    assert!(matches!(m.chunks.get(&0), Some(Chunk::Run(1))));
    assert_eq!(m.cardinality(), CHUNK_BITS);
}

#[test]
fn adjacent_runs_coalesce() {
    let mut m = SparseMap::new();
    for i in 0..(3 * CHUNK_BITS) {
        m.insert(i);
    }
    assert_eq!(m.chunks.len(), 1);
    assert!(matches!(m.chunks.get(&0), Some(Chunk::Run(3))));
}

#[test]
fn removing_from_run_splits_it() {
    let mut m = SparseMap::new();
    for i in 0..(3 * CHUNK_BITS) {
        m.insert(i);
    }
    // Clear one bit in the middle window.
    let mid = CHUNK_BITS + 100;
    assert!(m.remove(mid));
    assert!(!m.contains(mid));
    assert_eq!(m.cardinality(), 3 * CHUNK_BITS - 1);
    // Structure: Run(1) | Dense | Run(1).
    assert!(matches!(m.chunks.get(&0), Some(Chunk::Run(1))));
    assert!(matches!(m.chunks.get(&CHUNK_BITS), Some(Chunk::Dense(_))));
    assert!(matches!(
        m.chunks.get(&(2 * CHUNK_BITS)),
        Some(Chunk::Run(1))
    ));
}

#[test]
fn refilling_split_recoalesces() {
    let mut m = SparseMap::new();
    for i in 0..(3 * CHUNK_BITS) {
        m.insert(i);
    }
    let mid = CHUNK_BITS + 100;
    m.remove(mid);
    m.insert(mid);
    // Back to a single run, canonical.
    assert_eq!(m.chunks.len(), 1);
    assert!(matches!(m.chunks.get(&0), Some(Chunk::Run(3))));
}

#[test]
fn rank_select_roundtrip() {
    let bits: Vec<u64> = vec![1, 5, 64, 65, 2047, 2048, 100_000];
    let m: SparseMap = bits.iter().copied().collect();
    for (i, &b) in bits.iter().enumerate() {
        assert_eq!(m.select(i as u64), Some(b));
        assert_eq!(m.rank(b), i as u64);
    }
    assert_eq!(m.select(bits.len() as u64), None);
}

#[test]
fn rank_within_run() {
    let m: SparseMap = (0..5000u64).collect();
    assert_eq!(m.rank(0), 0);
    assert_eq!(m.rank(100), 100);
    assert_eq!(m.rank(5000), 5000);
    assert_eq!(m.rank(9999), 5000);
    assert_eq!(m.select(4999), Some(4999));
    assert_eq!(m.select(5000), None);
}

#[test]
fn set_operations() {
    let a: SparseMap = (0..100).collect();
    let b: SparseMap = (50..150).collect();
    assert_eq!((&a | &b), (0..150).collect());
    assert_eq!((&a & &b), (50..100).collect());
    assert_eq!((&a - &b), (0..50).collect());
    let xor: SparseMap = (0..50).chain(100..150).collect();
    assert_eq!((&a ^ &b), xor);
    assert!(a.intersects(&b));
    assert!(!a.intersects(&(200..300).collect()));
}

#[test]
fn set_ops_on_runs_stay_runs() {
    let a: SparseMap = (0..10 * CHUNK_BITS).collect();
    let b: SparseMap = (5 * CHUNK_BITS..15 * CHUNK_BITS).collect();
    let u = &a | &b;
    assert_eq!(u, (0..15 * CHUNK_BITS).collect());
    // The union of two long runs is a single run.
    assert_eq!(u.chunks.len(), 1);
    assert!(matches!(u.chunks.get(&0), Some(Chunk::Run(15))));
}

#[test]
fn subset_superset() {
    let a: SparseMap = [1, 2, 3].into_iter().collect();
    let b: SparseMap = (0..10).collect();
    assert!(a.is_subset(&b));
    assert!(b.is_superset(&a));
    assert!(!b.is_subset(&a));
}

#[test]
fn shift() {
    let m: SparseMap = [10, 20, 30].into_iter().collect();
    assert_eq!(m.shifted(5), [15, 25, 35].into_iter().collect());
    assert_eq!(m.shifted(-10), [0, 10, 20].into_iter().collect());
    // Bits shifted below zero are dropped.
    assert_eq!(m.shifted(-15), [5, 15].into_iter().collect());
}

#[test]
fn insert_range_uses_runs() {
    let mut m = SparseMap::new();
    m.insert_range(0, 5 * CHUNK_BITS);
    assert_eq!(m.cardinality(), 5 * CHUNK_BITS);
    assert_eq!(m, (0..5 * CHUNK_BITS).collect());
}

#[test]
fn equality_is_structural_canonical() {
    // Built two different ways, same bits => equal and same hash.
    let mut a = SparseMap::new();
    a.insert_range(0, 2 * CHUNK_BITS);
    let b: SparseMap = (0..2 * CHUNK_BITS).collect();
    assert_eq!(a, b);

    use core::hash::{Hash, Hasher};
    let mut ha = FnvHasher::default();
    let mut hb = FnvHasher::default();
    a.hash(&mut ha);
    b.hash(&mut hb);
    assert_eq!(ha.finish(), hb.finish());
}

#[test]
fn serialize_roundtrip() {
    let cases: Vec<SparseMap> = vec![
        SparseMap::new(),
        [42].into_iter().collect(),
        (0..5000u64).collect(),
        [1, 2, 3, 2048, 4096, 1_000_000].into_iter().collect(),
    ];
    for m in cases {
        let bytes = m.to_bytes().unwrap();
        let back = SparseMap::from_bytes(&bytes).unwrap();
        assert_eq!(m, back, "roundtrip mismatch");
    }
}

#[test]
fn deserialize_rejects_garbage() {
    assert_eq!(SparseMap::from_bytes(&[]), Err(DecodeError::TooShort));
    assert_eq!(
        SparseMap::from_bytes(&[0u8; 32]),
        Err(DecodeError::BadMagic)
    );
}

// A tiny deterministic FNV-1a hasher so the equality/hash test needs no
// std-provided Hasher.
#[derive(Default)]
struct FnvHasher(u64);

impl core::hash::Hasher for FnvHasher {
    fn finish(&self) -> u64 {
        self.0
    }
    fn write(&mut self, bytes: &[u8]) {
        let mut h = if self.0 == 0 {
            0xcbf2_9ce4_8422_2325
        } else {
            self.0
        };
        for &b in bytes {
            h ^= u64::from(b);
            h = h.wrapping_mul(0x0000_0100_0000_01b3);
        }
        self.0 = h;
    }
}
