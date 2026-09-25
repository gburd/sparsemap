#![no_main]
use libfuzzer_sys::fuzz_target;
use sparsemap::SparseMap;

// Any panic is a finding: `from_bytes` documents that arbitrary bytes
// are safe to feed in.  `overflow-checks = true` in the fuzz profile
// turns the silent wrap-around of a release build into a detectable
// panic, so an unchecked add/mul on a chunk span shows up here.
//
// A decode that returns `Ok` must produce a *sound* map: its members
// iterate strictly ascending, and it re-serializes to a buffer that
// decodes back to the same map.  Those assertions catch the
// release-mode wrong-answer bugs (non-ascending members, unstable
// round-trip) that a plain "did it panic?" target would miss.
fuzz_target!(|data: &[u8]| {
    if let Ok(m) = SparseMap::from_bytes(data) {
        let _ = m.len();

        // Members must be strictly ascending (bounded sample).
        let bits: Vec<u64> = m.iter().take(4096).collect();
        for w in bits.windows(2) {
            assert!(w[0] < w[1], "decoded map iterates non-ascending");
        }

        // Round-trip must be stable.
        let back = m.to_bytes();
        match SparseMap::from_bytes(&back) {
            Ok(re) => assert!(re == m, "decoded map is not round-trip stable"),
            Err(e) => panic!("re-serialized valid map failed to decode: {e:?}"),
        }

        // Exercise the mutate + set-operation paths on the decoded map.
        let mut c = m.clone();
        c.insert(5);
        c.remove(100);
        let _ = &m | &c;
        let _ = &m & &c;
        let _ = &m ^ &c;
        let _ = &m - &c;
    }
});
