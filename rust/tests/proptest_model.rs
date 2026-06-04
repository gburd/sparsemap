#![allow(clippy::pedantic)]
//! Property tests: every operation is checked against a `BTreeSet<u64>`
//! oracle.  The oracle is the obvious dense implementation of the same
//! set; any divergence is a bug in `sparsemap`.

use proptest::prelude::*;
use sparsemap::SparseMap;
use std::collections::BTreeSet;

/// Universe wide enough to span ~25 windows, so runs, dense windows,
/// splits, and coalescing all get exercised.
const U: u64 = 50_000;

#[derive(Debug, Clone)]
enum Op {
    Insert(u64),
    Remove(u64),
    InsertRange(u64, u64),
    RemoveRange(u64, u64),
}

fn op_strategy() -> impl Strategy<Value = Op> {
    // Ranges are bounded in width so the dense oracle stays cheap while
    // still spanning several 2048-bit windows.
    prop_oneof![
        (0..U).prop_map(Op::Insert),
        (0..U).prop_map(Op::Remove),
        (0..U, 1u64..6000).prop_map(|(a, w)| Op::InsertRange(a, (a + w).min(U))),
        (0..U, 1u64..6000).prop_map(|(a, w)| Op::RemoveRange(a, (a + w).min(U))),
    ]
}

fn apply(m: &mut SparseMap, model: &mut BTreeSet<u64>, op: &Op) {
    match *op {
        Op::Insert(i) => {
            assert_eq!(m.insert(i), model.insert(i));
        }
        Op::Remove(i) => {
            assert_eq!(m.remove(i), model.remove(&i));
        }
        Op::InsertRange(a, b) => {
            m.insert_range(a, b);
            for i in a..b {
                model.insert(i);
            }
        }
        Op::RemoveRange(a, b) => {
            m.remove_range(a, b);
            for i in a..b {
                model.remove(&i);
            }
        }
    }
}

fn check(m: &SparseMap, model: &BTreeSet<u64>) {
    assert_eq!(m.cardinality(), model.len() as u64, "cardinality");
    assert_eq!(m.is_empty(), model.is_empty(), "is_empty");
    assert_eq!(m.min(), model.iter().next().copied(), "min");
    assert_eq!(m.max(), model.iter().next_back().copied(), "max");
    assert!(
        m.iter().eq(model.iter().copied()),
        "iteration order disagrees"
    );
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(256))]

    /// Stateful model test: a random sequence of mutations keeps the map
    /// in lock-step with the oracle on every query.
    #[test]
    fn model(ops in proptest::collection::vec(op_strategy(), 0..120)) {
        let mut m = SparseMap::new();
        let mut model = BTreeSet::new();
        for op in &ops {
            apply(&mut m, &mut model, op);
            // Cheap invariant after every op.
            prop_assert_eq!(m.cardinality(), model.len() as u64);
        }
        check(&m, &model);

        // rank / select against the oracle, sampled so the test stays
        // fast even when InsertRange produced a huge cardinality.
        let sorted: Vec<u64> = model.iter().copied().collect();
        let stride = (sorted.len() / 64).max(1);
        for (n, &bit) in sorted.iter().enumerate().step_by(stride) {
            prop_assert_eq!(m.select(n as u64), Some(bit));
            prop_assert_eq!(m.rank(bit), n as u64);
        }
        prop_assert_eq!(m.select(sorted.len() as u64), None);
    }

    /// `contains` agrees with the oracle across the whole universe edge
    /// (including just past `max`).
    #[test]
    fn contains_matches(bits in proptest::collection::hash_set(0..U, 0..300)) {
        let m: SparseMap = bits.iter().copied().collect();
        for i in 0..U + 2 {
            prop_assert_eq!(m.contains(i), bits.contains(&i));
        }
    }

    /// Set operations equal the oracle's set operations.
    #[test]
    fn set_ops(
        sa in proptest::collection::hash_set(0..U, 0..300),
        sb in proptest::collection::hash_set(0..U, 0..300),
    ) {
        let a: SparseMap = sa.iter().copied().collect();
        let b: SparseMap = sb.iter().copied().collect();
        let oa: BTreeSet<u64> = sa.iter().copied().collect();
        let ob: BTreeSet<u64> = sb.iter().copied().collect();

        let want = |s: BTreeSet<u64>| -> SparseMap { s.into_iter().collect() };

        prop_assert_eq!(&a | &b, want(oa.union(&ob).copied().collect()));
        prop_assert_eq!(&a & &b, want(oa.intersection(&ob).copied().collect()));
        prop_assert_eq!(&a - &b, want(oa.difference(&ob).copied().collect()));
        prop_assert_eq!(
            &a ^ &b,
            want(oa.symmetric_difference(&ob).copied().collect())
        );
        prop_assert_eq!(a.intersects(&b), !oa.is_disjoint(&ob));
        prop_assert_eq!(a.is_subset(&b), oa.is_subset(&ob));
    }

    /// Serialize/deserialize round-trips to an equal set.
    #[test]
    fn serialize_roundtrip(bits in proptest::collection::hash_set(0..U, 0..400)) {
        let m: SparseMap = bits.iter().copied().collect();
        let bytes = m.to_bytes();
        let back = SparseMap::from_bytes(&bytes).unwrap();
        prop_assert_eq!(m, back);
    }

    /// Deserializing arbitrary bytes never panics.
    #[test]
    fn deserialize_never_panics(bytes in proptest::collection::vec(any::<u8>(), 0..512)) {
        let _ = SparseMap::from_bytes(&bytes);
    }

    /// Shifting by `k` then by `-k` is the identity for non-negative
    /// shifts (no bits fall off the bottom).
    #[test]
    fn shift_roundtrip(
        bits in proptest::collection::hash_set(0..U, 0..200),
        k in 0..1000u64,
    ) {
        let m: SparseMap = bits.iter().copied().collect();
        let there_and_back = m.shifted(k as i64).shifted(-(k as i64));
        prop_assert_eq!(m, there_and_back);
    }
}
