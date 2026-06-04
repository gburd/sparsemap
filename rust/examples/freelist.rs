//! A worked example: tracking a free list of fixed-size slots.
//!
//! Run with `cargo run --example freelist`.

use sparsemap::SparseMap;

fn main() {
    // A storage engine with 1,000,000 slots; all free to start.
    const SLOTS: u64 = 1_000_000;
    let mut free = SparseMap::new();
    free.insert_range(0, SLOTS);
    println!(
        "free slots: {} (stored compactly as one run)",
        free.cardinality()
    );

    // Allocate the first 200,000 slots (a contiguous span) ...
    let mut allocated = SparseMap::new();
    allocated.insert_range(0, 200_000);

    // ... plus a scattering of slots elsewhere.
    for s in (500_000..500_000 + 100_000).step_by(3) {
        allocated.insert(s);
    }

    // The remaining free list is free \ allocated.
    free -= &allocated;
    println!("after allocation, free slots: {}", free.cardinality());
    println!("lowest free slot:  {:?}", free.min());
    println!("highest free slot: {:?}", free.max());

    // Find the first run of at least 4096 contiguous free slots
    // (e.g. for a large object).
    match free.span(0, 4096, true) {
        Some(start) => println!("first 4096-slot free run starts at {start}"),
        None => println!("no 4096-slot free run"),
    }

    // The 50,000th free slot, in O(log chunks) per step.
    println!("50,000th free slot: {:?}", free.select(50_000));

    // Persist the free list, then reload it.
    let bytes = free.to_bytes();
    println!("serialized free list: {} bytes", bytes.len());
    let reloaded = SparseMap::from_bytes(&bytes).expect("round-trips");
    assert_eq!(reloaded, free);
    println!("reloaded and verified equal");
}
