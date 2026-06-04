//! (dev target: relax pedantic lints that fire on benchmark closures
//! and criterion-generated items)
#![allow(clippy::pedantic, missing_docs)]

//! Criterion micro-benchmarks for `sparsemap`.
//!
//! Run with `cargo bench`.  These mirror the C `sm_microbench` workloads
//! (a sparse random map and a dense run map) so the two ports can be
//! compared apples-to-apples.

use criterion::{black_box, criterion_group, criterion_main, BenchmarkId, Criterion};
use sparsemap::SparseMap;

fn random_map(n: u64, span: u64, seed: u64) -> SparseMap {
    let mut s = seed;
    let mut next = move || {
        // SplitMix64.
        s = s.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = s;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        z ^ (z >> 31)
    };
    let mut m = SparseMap::new();
    for _ in 0..n {
        m.insert(next() % span);
    }
    m
}

fn dense_map(lo: u64, hi: u64) -> SparseMap {
    let mut m = SparseMap::new();
    m.insert_range(lo, hi);
    m
}

fn bench_construction(c: &mut Criterion) {
    let mut g = c.benchmark_group("construct");
    g.bench_function("insert_sequential_4096", |b| {
        b.iter(|| {
            let mut m = SparseMap::new();
            for i in 0..4096u64 {
                m.insert(black_box(i));
            }
            m
        });
    });
    g.bench_function("insert_random_4096", |b| {
        b.iter(|| black_box(random_map(4096, 1 << 20, 1)));
    });
    g.finish();
}

fn bench_queries(c: &mut Criterion) {
    let rnd = random_map(50_000, 1 << 20, 1);
    let dense = dense_map(0, 200_000);
    let mut g = c.benchmark_group("query");
    for (name, m) in [("random", &rnd), ("dense", &dense)] {
        let hi = m.max().unwrap_or(0) + 1;
        let card = m.cardinality();
        g.bench_with_input(BenchmarkId::new("contains", name), m, |b, m| {
            let mut i = 0u64;
            b.iter(|| {
                i = i.wrapping_add(2_654_435_761) % hi;
                black_box(m.contains(black_box(i)))
            });
        });
        g.bench_with_input(BenchmarkId::new("rank", name), m, |b, m| {
            let mut i = 0u64;
            b.iter(|| {
                i = i.wrapping_add(40_503) % hi;
                black_box(m.rank(black_box(i)))
            });
        });
        g.bench_with_input(BenchmarkId::new("select", name), m, |b, m| {
            let mut i = 0u64;
            b.iter(|| {
                i = i.wrapping_add(2_654_435_761) % card.max(1);
                black_box(m.select(black_box(i)))
            });
        });
        g.bench_with_input(BenchmarkId::new("iterate", name), m, |b, m| {
            b.iter(|| black_box(m.iter().count()));
        });
    }
    g.finish();
}

fn bench_setops(c: &mut Criterion) {
    let a = random_map(50_000, 1 << 20, 1);
    let b = random_map(50_000, 1 << 20, 2);
    let d = dense_map(0, 200_000);
    let mut g = c.benchmark_group("setop");
    g.bench_function("union_rand_rand", |bn| bn.iter(|| black_box(&a | &b)));
    g.bench_function("intersection_rand_rand", |bn| {
        bn.iter(|| black_box(&a & &b))
    });
    g.bench_function("union_rand_dense", |bn| bn.iter(|| black_box(&a | &d)));
    g.bench_function("intersection_rand_dense", |bn| {
        bn.iter(|| black_box(&a & &d))
    });
    g.finish();
}

fn bench_serialize(c: &mut Criterion) {
    let rnd = random_map(50_000, 1 << 20, 1);
    let dense = dense_map(0, 200_000);
    let mut g = c.benchmark_group("serialize");
    g.bench_function("random", |b| b.iter(|| black_box(rnd.to_bytes())));
    g.bench_function("dense", |b| b.iter(|| black_box(dense.to_bytes())));
    g.finish();
}

criterion_group!(
    benches,
    bench_construction,
    bench_queries,
    bench_setops,
    bench_serialize
);
criterion_main!(benches);
