# Benchmarking sparsemap

This directory holds `sm_microbench`, a dependency-free micro-benchmark
that measures individual sparsemap operations in isolation.  It links
only against `libsparsemap`, so the numbers are stable enough to review
in a pull request and reproduce on a laptop.

The *comparative* benchmark (sparsemap vs. CRoaring vs. a plain
bitmapset) lives on the separate `bench/comparative` branch so the
primary repository stays free of the ~1 MB vendored `roaring.c` and the
plotting tooling.  See that branch's README to reproduce the comparison.

## Running

```bash
meson setup builddir
ninja -C builddir bench/sm_microbench
./builddir/bench/sm_microbench
```

For the most repeatable numbers, pin the process to one core, raise its
priority, and quiet the rest of the machine:

```bash
# Linux: performance governor, no turbo wandering, pinned + niced.
sudo cpupower frequency-set -g performance
taskset -c 2 nice -n -5 ./builddir/bench/sm_microbench
```

On a shared or virtualized host, trust the `min` column over the
`median`: the minimum sample is the one least perturbed by interrupts
and co-tenants.

## Methodology

`sm_microbench` is built around a small calibrating harness:

- **Clock.** `CLOCK_PROCESS_CPUTIME_ID` when the platform offers it
  (excludes time the kernel spent elsewhere), falling back to
  `CLOCK_MONOTONIC`.
- **Calibration.** Each measured region's inner iteration count is
  grown until one repetition lasts at least 20 ms, so timer
  granularity is negligible.
- **Repetition.** Seven independent repetitions are timed; the harness
  reports both the **minimum** per-op time (least perturbed) and the
  **median**.  A wide gap between them means the machine was noisy.
- **Warmup.** The calibration pass also primes caches and branch
  predictors before the timed repetitions.
- **No dead-code elimination.** Every operation folds a checksum into a
  `volatile` sink so the optimizer cannot delete the work.

## What is measured

The workloads span the two regimes that matter for sparsemap's encoding:

- a **random** 50,000-bit map over a 1 M-bit span (many sparse chunks,
  cursor-defeating access patterns), and
- a **dense** 200,000-bit contiguous run (mostly RLE chunks, the
  compression best case).

Operations covered: `sm_add` (sequential and random), `sm_contains`,
`sm_rank`, `sm_select`, `sm_next_member` iteration, `sm_union` /
`sm_intersection` across density combinations, and `sm_serialize`.

The dense-map query times exercise the O(1) RLE descriptor paths; the
random-map times exercise the chunk-walk cost.  Interpreting a number
in isolation is meaningless — always compare against the baseline from
the same machine and build (`buildtype=release`).
