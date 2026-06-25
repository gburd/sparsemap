# C vs Rust benchmark comparison (sparsemap 5.1.0)

Both ports at 5.1.0, release builds, same host, identical workloads
(`random_map(50000, 1<<20)`, `dense_map(0, 200000)`).  C numbers from
`bench/sm_microbench` (ns per single op).  Rust numbers from
`cargo bench` (criterion median; per-op for queries, per-build for
construction).

## Results

| workload | C | Rust | winner |
|---|---:|---:|---|
| **construction** (per op) | | | |
| insert, sequential | 37.5 ns | ~7.5 ns (30.7 us / 4096) | Rust ~5x |
| insert, random 1M span | 1466 ns | ~146 ns (597 us / 4096) | Rust ~10x |
| **query, random 50k-bit map** (per op) | | | |
| contains | 2071 ns | 16.1 ns | **Rust ~130x** |
| rank(0,x) | 92070 ns | 4921 ns | **Rust ~19x** |
| select(n) | 164386 ns | 4915 ns | **Rust ~33x** |
| next_member (per bit) | 55 ns | ~3 ns | Rust ~18x |
| **query, dense 200k RLE map** (per op) | | | |
| contains | 3.43 ns | 4.93 ns | C ~1.4x |
| rank(0,x) | 4.61 ns | 4.55 ns | tie |
| select(n) | 2.25 ns | 4.59 ns | C ~2x |
| **set ops** (per op) | | | |
| union (random,random) | 97845 ns | 38787 ns | Rust ~2.5x |
| intersection (random,random) | 116849 ns | 28064 ns | Rust ~4x |
| union (random,dense) | 22995 ns | 24641 ns | tie |
| intersection (random,dense) | 15951 ns | 3656 ns | Rust ~4x |
| **serialize** (per op) | | | |
| serialize (random) | 202857 ns | 38086 ns | Rust ~5x |
| serialize (dense) | 16.6 ns | 147.6 ns | **C ~9x** |

## Why they differ (this matters for "algorithmically identical")

The two ports are **behaviorally identical** (same answers, byte-identical
wire format, verified by cross-FFI tests) but use **different internal
data structures**, so their performance profiles diverge:

- **C**: flat byte-packed chunk stream + an *optional* caller-owned
  cursor.  A query that can't supply a monotonic cursor (random access)
  walks the chunk stream from the head: O(chunks) per lookup.  With a
  50k-bit map scattered over a 1M span that is thousands of chunks, so
  `contains`/`rank`/`select` on a random map are slow (microseconds).
- **Rust**: `BTreeMap<u64, Chunk>`.  Random `contains` is an O(log n)
  tree lookup (~16 ns); `rank`/`select` walk a bounded tree range.

So on **scattered random-access queries Rust is 20-130x faster**; on
**dense/RLE maps they are comparable** (the codec, not the locator,
dominates); C wins on **dense serialize** (a near-verbatim buffer copy
vs. Rust rebuilding the byte stream from the BTreeMap), and Rust wins
on **construction and set ops** (BTreeMap insert + run-coalescing beats
C's per-insert chunk walk; merge-join over two trees beats the flat
walk).

This is the deliberate consequence of "Option 1" parity: identical wire
format and behavior, idiomatic Rust internals.  It is not a defect --
the Rust port is simply faster on the random-query workloads its data
structure is suited to, and the C port keeps the smaller footprint
(24-byte sm_t, no per-map tree allocation) its byte-stream design buys.

A consumer choosing between them: the C library for the minimal memory
footprint and embedded/no-allocator use; the Rust crate for
random-access-query-heavy workloads in a Rust codebase.  Both produce
and read the same bytes.
