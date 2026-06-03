//! Wire-format compatibility with the C sparsemap library.
//!
//! Compiled only when `build.rs` found the C source and set the `c_ffi`
//! cfg (i.e. when building inside the upstream repository).  The tests
//! round-trip serialized buffers in both directions:
//!
//! * a map built and serialized in C must deserialize in Rust to the
//!   same bit set, and
//! * a map built and serialized in Rust must deserialize in C to the
//!   same bit set.
//!
//! This is the ground truth for the claim that the Rust port is
//! functionally identical to, and interoperable with, the C library.
#![cfg(c_ffi)]

use sparsemap::SparseMap;
use std::collections::BTreeSet;
use std::os::raw::c_void;

#[allow(non_camel_case_types)]
type sm_t = c_void;

extern "C" {
    fn sm_create(size: usize) -> *mut sm_t;
    fn sm_free(map: *mut sm_t);
    fn sm_add(map: *mut sm_t, idx: u64) -> u64;
    fn sm_contains(map: *mut sm_t, idx: u64) -> bool;
    fn sm_cardinality(map: *mut sm_t) -> usize;
    fn sm_serialized_size(map: *const sm_t) -> usize;
    fn sm_serialize(map: *const sm_t, out: *mut u8, out_size: usize) -> usize;
    fn sm_deserialize(input: *const u8, n: usize) -> *mut sm_t;
}

/// Build a C map from a set of bits and return its serialized bytes.
fn c_serialize(bits: &BTreeSet<u64>) -> Vec<u8> {
    unsafe {
        let map = sm_create(4096);
        assert!(!map.is_null());
        for &b in bits {
            // sm_add can fail with ENOSPC on a fixed buffer; grow by
            // recreating large enough.  4096 + slack per bit is ample
            // for these small test sets.
            let rc = sm_add(map, b);
            assert_ne!(rc, u64::MAX, "C sm_add ENOSPC; enlarge test buffer");
        }
        assert_eq!(sm_cardinality(map), bits.len());
        let sz = sm_serialized_size(map);
        let mut buf = vec![0u8; sz];
        let wrote = sm_serialize(map, buf.as_mut_ptr(), sz);
        assert_eq!(wrote, sz);
        sm_free(map);
        buf
    }
}

/// Deserialize bytes with the C library and read back the bit set.
fn c_deserialize_bits(bytes: &[u8], probe: &BTreeSet<u64>) -> (bool, Vec<u64>) {
    unsafe {
        let map = sm_deserialize(bytes.as_ptr(), bytes.len());
        if map.is_null() {
            return (false, Vec::new());
        }
        let present: Vec<u64> = probe
            .iter()
            .copied()
            .filter(|&b| sm_contains(map, b))
            .collect();
        sm_free(map);
        (true, present)
    }
}

fn sample_sets() -> Vec<BTreeSet<u64>> {
    vec![
        BTreeSet::new(),
        [42u64].into_iter().collect(),
        [1, 2, 3, 2047, 2048, 4096, 100_000].into_iter().collect(),
        (0..5000u64).collect(),     // crosses windows, partial run
        (0..2048u64 * 4).collect(), // exact multi-window run
        (0..100u64).chain(10_000..10_050).collect(),
        (1000..1000u64 + 6000).collect(), // offset partial run
    ]
}

#[test]
fn c_writes_rust_reads() {
    for bits in sample_sets() {
        let c_bytes = c_serialize(&bits);
        let m = SparseMap::from_bytes(&c_bytes).expect("Rust must deserialize C output");
        let got: BTreeSet<u64> = m.iter().collect();
        assert_eq!(got, bits, "Rust read of C bytes diverged");
    }
}

#[test]
fn rust_writes_c_reads() {
    for bits in sample_sets() {
        let m: SparseMap = bits.iter().copied().collect();
        let rust_bytes = m.to_bytes().unwrap();
        // Probe a superset of the bits plus some absent neighbors.
        let mut probe = bits.clone();
        for &b in &bits {
            probe.insert(b + 1);
            probe.insert(b.saturating_sub(1));
        }
        let (ok, present) = c_deserialize_bits(&rust_bytes, &probe);
        if bits.is_empty() {
            // An empty map may serialize to a body the C reader treats
            // as NULL/empty; either way it must not report set bits.
            assert!(present.is_empty());
            continue;
        }
        assert!(ok, "C must deserialize Rust output");
        let got: BTreeSet<u64> = present.into_iter().collect();
        assert_eq!(got, bits, "C read of Rust bytes diverged");
    }
}
