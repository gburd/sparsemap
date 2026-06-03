# Comparative benchmarks (this branch only)

This `bench/comparative` branch carries the comparison benchmark that
pits sparsemap against [CRoaring](https://github.com/RoaringBitmap/CRoaring)
and a plain word-array bitmapset.  It is kept off `main` so the primary
repository is not burdened with the ~1 MB vendored `roaring.c` or the
matplotlib plotting script.

`main` keeps only the dependency-free `bench/sm_microbench`
(see `bench/BENCHMARKING.md`).

## Building and running

```bash
meson setup builddir
ninja -C builddir bench/sparsemap_bench
./builddir/bench/sparsemap_bench --help
```

```
Usage: bench [OPTIONS]
  --pattern=NAME     dense, sparse, periodic, clustered, powerlaw,
                     block, alternating, worst
  --cardinality=N    override the default cardinality
  --verify           only verify correctness, do not benchmark
```

CSV goes to stdout; a human-readable summary to stderr:

```bash
./builddir/bench/sparsemap_bench > results.csv 2> summary.txt
python3 bench/plot_bench.py results.csv      # optional plots
```

## Fairness and repeatability

- Every operation is verified to produce identical results across all
  three libraries before timing (`Cardinality verified` line); a
  divergence aborts the run rather than reporting a meaningless number.
- Latencies are reported as p50 / p90 / p99 percentiles over many
  repetitions, not a single mean, so tail behavior is visible.
- Memory footprint is reported per library per pattern.
- Workloads span eight bit-distribution patterns (dense, sparse,
  periodic, clustered, power-law, block, alternating, worst case) so
  no single library's best case dominates the picture.

Pin the run for stable numbers:

```bash
sudo cpupower frequency-set -g performance
taskset -c 2 nice -n -5 ./builddir/bench/sparsemap_bench
```

## Keeping in sync with main

This branch tracks `main` and adds the comparative harness on top.
After `main` advances, rebase:

```bash
git checkout bench/comparative
git rebase main
```
