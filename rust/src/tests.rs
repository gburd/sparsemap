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
fn shift_extreme_offsets_are_overflow_safe() {
    // The C sm_offset had a signed-overflow bug for |offset| near
    // SSIZE_MAX; the Rust shifted() uses checked u64 arithmetic per bit,
    // so extreme offsets must never panic and must drop out-of-range
    // bits individually.
    let m: SparseMap = [0, 5, 100_000, u64::MAX - 10].into_iter().collect();

    // +i64::MAX: only bits whose value + i64::MAX <= u64::MAX survive.
    let up = m.shifted(i64::MAX);
    for b in &m {
        if let Some(s) = b.checked_add(i64::MAX as u64) {
            assert!(up.contains(s)); // bits that overflow are dropped
        }
    }
    assert_eq!(up.cardinality(), [0u64, 5, 100_000].len() as u64);

    // -i64::MAX and i64::MIN: bits below zero drop, no panic.
    let _ = m.shifted(-i64::MAX);
    let dn = m.shifted(i64::MIN);
    // Only u64::MAX-10 survives i64::MIN if it is >= |i64::MIN|.
    for b in &m {
        if let Some(s) = b.checked_sub(i64::MIN.unsigned_abs()) { assert!(dn.contains(s)) }
    }

    // +2^62 on small bits: every bit shifts cleanly (no overflow).
    let up2 = m.shifted(1i64 << 62);
    assert_eq!(up2.cardinality(), 3); // u64::MAX-10 + 2^62 overflows -> dropped
    assert!(up2.contains(1u64 << 62));
    assert!(up2.contains((1u64 << 62) + 5));
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
        let bytes = m.to_bytes();
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

#[test]
fn top_of_universe_is_panic_free() {
    // Fill the top window [2^64 - 2048, 2^64) bit by bit.  Every bit but
    // u64::MAX goes in, so the window stays dense (it can never become
    // an all-ones run), and min/max/rank/select, iteration and the set
    // operations must all handle the top window without overflowing.
    let base = u64::MAX - (CHUNK_BITS - 1);
    let mut m = SparseMap::new();
    for off in 0..CHUNK_BITS {
        let idx = base + off;
        assert_eq!(m.insert(idx), idx != u64::MAX);
    }
    assert!(matches!(m.chunks.get(&base), Some(Chunk::Dense(_))));
    assert_eq!(m.cardinality(), CHUNK_BITS - 1);
    assert_eq!(m.min(), Some(base));
    assert_eq!(m.max(), Some(u64::MAX - 1));
    assert!(!m.contains(u64::MAX));
    assert_eq!(m.rank(u64::MAX), CHUNK_BITS - 1);
    assert_eq!(m.select(CHUNK_BITS - 2), Some(u64::MAX - 1));
    assert_eq!(m.select(CHUNK_BITS - 1), None);
    let bits: Vec<u64> = m.iter().collect();
    assert_eq!(bits.len() as u64, CHUNK_BITS - 1);
    assert_eq!(*bits.last().unwrap(), u64::MAX - 1);
    assert!(bits.windows(2).all(|w| w[0] < w[1]));
    // Set operations touch the run-merge and cursor-advance paths.
    let other: SparseMap = [base].into_iter().collect();
    assert_eq!((&m | &other), m);
    assert_eq!((&m & &other), other);
    assert_eq!((&m - &other).cardinality(), CHUNK_BITS - 2);
    assert_eq!((&m ^ &other).cardinality(), CHUNK_BITS - 2);
}

#[test]
fn u64_max_is_never_a_member() {
    // insert refuses u64::MAX and stores nothing, on an empty map and
    // next to members in the top window.
    let mut m = SparseMap::new();
    assert!(!m.insert(u64::MAX));
    assert!(m.is_empty());
    assert!(!m.contains(u64::MAX));
    assert!(m.insert(u64::MAX - 1));
    assert!(!m.insert(u64::MAX));
    assert!(!m.contains(u64::MAX));
    assert!(!m.remove(u64::MAX));
    assert_eq!(m.cardinality(), 1);
    assert_eq!(m.max(), Some(u64::MAX - 1));
    assert_eq!(m.rank(u64::MAX), 1);

    // insert_range is half-open, so a range ending at u64::MAX stops
    // one short of it, including the whole-window run fast path for
    // ranges that cover several top windows.
    for start in [
        u64::MAX - 1,
        u64::MAX - 2047,
        u64::MAX - 2048,
        u64::MAX - 5 * CHUNK_BITS,
    ] {
        let mut r = SparseMap::new();
        r.insert_range(start, u64::MAX);
        assert_eq!(r.cardinality(), u64::MAX - start, "range from {start}");
        assert_eq!(r.max(), Some(u64::MAX - 1));
        assert!(!r.contains(u64::MAX));
        // The top window holds 2047 members, so it is never a run.
        let top = window_base(u64::MAX);
        assert!(!matches!(r.chunks.get(&top), Some(Chunk::Run(_))));
        assert_eq!(SparseMap::from_bytes(&r.to_bytes()).as_ref(), Ok(&r));
    }

    // Extend / FromIterator go through insert and drop it too.
    let c: SparseMap = [5, u64::MAX, u64::MAX - 2].into_iter().collect();
    assert_eq!(c.to_vec(), vec![5, u64::MAX - 2]);

    // shifted drops a member that would land on u64::MAX.
    let s: SparseMap = [1, u64::MAX - 11, u64::MAX - 10].into_iter().collect();
    assert_eq!(s.shifted(10).to_vec(), vec![11, u64::MAX - 1]);

    // A union or xor of two maps that each fill the top window short of
    // u64::MAX stays short of it.
    let lo: SparseMap = (u64::MAX - CHUNK_BITS..u64::MAX - 1).collect();
    let hi: SparseMap = [u64::MAX - 1].into_iter().collect();
    let u = &lo | &hi;
    assert_eq!(u.max(), Some(u64::MAX - 1));
    assert!(!u.contains(u64::MAX));
    assert_eq!((&lo ^ &hi), u);
}

/// The `n`-th set bit of `word` by testing each bit in turn: the oracle
/// for `word_select`.
fn naive_word_select(word: u64, n: u32) -> u32 {
    let mut seen = 0;
    for b in 0..64 {
        if word & (1u64 << b) != 0 {
            if seen == n {
                return b;
            }
            seen += 1;
        }
    }
    panic!("n >= popcount");
}

#[test]
fn word_and_dense_select_match_a_bit_scan() {
    // Run with and without RUSTFLAGS="-C target-feature=+bmi2": the
    // first build checks the pdep path, the second the portable loop.
    // Words are xorshift values thinned or thickened by AND/OR so every
    // popcount from sparse to nearly full appears, plus the edge words.
    let mut s = 0x9e37_79b9_7f4a_7c15u64;
    let mut rnd = move || {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        s
    };
    let mut words = vec![1, 1 << 63, u64::MAX, u64::MAX - 1, 0x8000_0000_0000_0001];
    for _ in 0..4000 {
        let w = match rnd() % 4 {
            0 => rnd() & rnd() & rnd(),
            1 => rnd(),
            2 => rnd() | rnd() | rnd(),
            _ => rnd() & rnd() & rnd() & rnd() & rnd(),
        };
        if w != 0 {
            words.push(w);
        }
    }
    for &w in &words {
        for n in 0..w.count_ones() {
            assert_eq!(
                word_select(w, n),
                naive_word_select(w, n),
                "word {w:#x} n {n}"
            );
        }
    }

    // dense_select over whole windows, including empty words before the
    // target and a last word with only bit 63.
    for _ in 0..200 {
        let mut win = [0u64; WORDS_PER_CHUNK];
        for slot in &mut win {
            *slot = match rnd() % 3 {
                0 => 0,
                1 => rnd() & rnd(),
                _ => rnd(),
            };
        }
        win[WORDS_PER_CHUNK - 1] |= 1 << 63;
        let mut want = Vec::new();
        for (i, &x) in win.iter().enumerate() {
            for b in 0..64 {
                if x & (1u64 << b) != 0 {
                    want.push(i as u64 * 64 + b);
                }
            }
        }
        for (n, &pos) in want.iter().enumerate() {
            assert_eq!(dense_select(&win, n as u64), Some(pos));
        }
        assert_eq!(dense_select(&win, want.len() as u64), None);
    }
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
