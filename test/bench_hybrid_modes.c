/*
 * bench_hybrid_modes.c -- Benchmark hybrid bitmapset dense vs chunked modes
 *
 * Two analyses:
 *   1. Offset sweep: Same 1000-bit pattern shifted by 0..10M in 1M steps.
 *      Shows performance at and around the dense->chunked transition.
 *   2. Cutoff sweep: Vary BMS_DENSE_MAX_NWORDS from 1..65535 to find
 *      the optimal transition point.
 *
 * This file is a unity build -- it re-includes bitmapset_hybrid.c with
 * its own #defines to control the cutoff.
 *
 * Build:
 *   cc -O3 -I./include -I./test -o test/bench_hybrid_modes \
 *      test/bench_hybrid_modes.c libsparsemap.a -lm
 */

#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* ===================================================================
 * Timing
 * =================================================================== */

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Simple percentile from sorted array */
static double percentile(double *sorted, int n, double p) {
    double idx = p * (n - 1);
    int lo = (int)idx;
    int hi = lo + 1;
    if (hi >= n) hi = n - 1;
    double frac = idx - lo;
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

/* ===================================================================
 * Include the hybrid bitmapset directly (unity build)
 * We DON'T override BMS_DENSE_MAX_NWORDS here -- that's the default.
 * For the cutoff sweep, we use a runtime-configurable approach instead.
 * =================================================================== */

#ifndef BUILDING_OUTSIDE_POSTGRES
#define BUILDING_OUTSIDE_POSTGRES
#endif
#include "bitmapset.h"
#include "bitmapset.c"

/* ===================================================================
 * Benchmark helpers
 * =================================================================== */

#define WARMUP  50
#define ITERS   5000

typedef struct {
    double populate_ns;
    double contains_ns;
    double cardinality_ns;
    double union_ns;
    double intersect_ns;
    double difference_ns;
    double iterate_ns;
    size_t memory_bytes;
    bool   is_chunked;
} bench_result_t;

/* Generate a sorted array of `count` bit positions starting at `base_offset` */
static int64_t *gen_test_bits(size_t count, int64_t base_offset, const char *pattern) {
    int64_t *bits = malloc(count * sizeof(int64_t));

    if (strcmp(pattern, "dense") == 0) {
        /* Dense: bits 0..count-1, shifted by offset */
        for (size_t i = 0; i < count; i++)
            bits[i] = base_offset + (int64_t)i;
    } else if (strcmp(pattern, "sparse") == 0) {
        /* Sparse: every 10th bit */
        for (size_t i = 0; i < count; i++)
            bits[i] = base_offset + (int64_t)(i * 10);
    } else if (strcmp(pattern, "clustered") == 0) {
        /* 10 clusters of count/10 bits, gap of 1000 between clusters */
        size_t per_cluster = count / 10;
        if (per_cluster < 1) per_cluster = 1;
        size_t idx = 0;
        for (int c = 0; c < 10 && idx < count; c++) {
            int64_t cluster_start = base_offset + (int64_t)c * 1000;
            for (size_t j = 0; j < per_cluster && idx < count; j++)
                bits[idx++] = cluster_start + (int64_t)j;
        }
    } else {
        /* Default: periodic every 5th bit */
        for (size_t i = 0; i < count; i++)
            bits[i] = base_offset + (int64_t)(i * 5);
    }
    return bits;
}

static bench_result_t bench_pattern(int64_t *bits, size_t count) {
    bench_result_t result = {0};
    double *times = malloc(ITERS * sizeof(double));

    /* --- Populate benchmark --- */
    for (int i = 0; i < WARMUP; i++) {
        Bitmapset *s = NULL;
        for (size_t j = 0; j < count; j++)
            s = bms_add_member(s, bits[j]);
        bms_free(s);
    }
    for (int i = 0; i < ITERS; i++) {
        uint64_t t0 = now_ns();
        Bitmapset *s = NULL;
        for (size_t j = 0; j < count; j++)
            s = bms_add_member(s, bits[j]);
        uint64_t t1 = now_ns();
        times[i] = (double)(t1 - t0);
        bms_free(s);
    }
    qsort(times, ITERS, sizeof(double), cmp_double);
    result.populate_ns = percentile(times, ITERS, 0.50);

    /* Build the handle for query benchmarks */
    Bitmapset *handle = NULL;
    for (size_t j = 0; j < count; j++)
        handle = bms_add_member(handle, bits[j]);

    result.is_chunked = (handle != NULL && BMS_IS_CHUNKED(handle));
    if (handle != NULL) {
        if (BMS_IS_CHUNKED(handle))
            result.memory_bytes = offsetof(Bitmapset, data) +
                (size_t)BMS_ALLOC_CHUNKS(handle) * 64;
        else
            result.memory_bytes = BITMAPSET_SIZE(handle->nwords);
    }

    /* --- Contains benchmark --- */
    size_t check_count = count < 1000 ? count : 1000;
    for (int i = 0; i < WARMUP; i++) {
        volatile bool sink = false;
        for (size_t j = 0; j < check_count; j++)
            sink = bms_is_member(bits[j], handle);
        (void)sink;
    }
    for (int i = 0; i < ITERS; i++) {
        uint64_t t0 = now_ns();
        volatile bool sink = false;
        for (size_t j = 0; j < check_count; j++)
            sink = bms_is_member(bits[j], handle);
        uint64_t t1 = now_ns();
        times[i] = (double)(t1 - t0);
        (void)sink;
    }
    qsort(times, ITERS, sizeof(double), cmp_double);
    result.contains_ns = percentile(times, ITERS, 0.50);

    /* --- Cardinality benchmark --- */
    for (int i = 0; i < WARMUP; i++) {
        volatile int64_t c = bms_num_members(handle);
        (void)c;
    }
    for (int i = 0; i < ITERS; i++) {
        uint64_t t0 = now_ns();
        volatile int64_t c = bms_num_members(handle);
        uint64_t t1 = now_ns();
        times[i] = (double)(t1 - t0);
        (void)c;
    }
    qsort(times, ITERS, sizeof(double), cmp_double);
    result.cardinality_ns = percentile(times, ITERS, 0.50);

    /* --- Union benchmark (self-union) --- */
    for (int i = 0; i < WARMUP; i++) {
        Bitmapset *u = bms_union(handle, handle);
        bms_free(u);
    }
    for (int i = 0; i < ITERS; i++) {
        uint64_t t0 = now_ns();
        Bitmapset *u = bms_union(handle, handle);
        uint64_t t1 = now_ns();
        times[i] = (double)(t1 - t0);
        bms_free(u);
    }
    qsort(times, ITERS, sizeof(double), cmp_double);
    result.union_ns = percentile(times, ITERS, 0.50);

    /* --- Intersect benchmark (self-intersect) --- */
    for (int i = 0; i < WARMUP; i++) {
        Bitmapset *r = bms_intersect(handle, handle);
        bms_free(r);
    }
    for (int i = 0; i < ITERS; i++) {
        uint64_t t0 = now_ns();
        Bitmapset *r = bms_intersect(handle, handle);
        uint64_t t1 = now_ns();
        times[i] = (double)(t1 - t0);
        bms_free(r);
    }
    qsort(times, ITERS, sizeof(double), cmp_double);
    result.intersect_ns = percentile(times, ITERS, 0.50);

    /* --- Difference benchmark (self-difference) --- */
    for (int i = 0; i < WARMUP; i++) {
        Bitmapset *r = bms_difference(handle, handle);
        bms_free(r);
    }
    for (int i = 0; i < ITERS; i++) {
        uint64_t t0 = now_ns();
        Bitmapset *r = bms_difference(handle, handle);
        uint64_t t1 = now_ns();
        times[i] = (double)(t1 - t0);
        bms_free(r);
    }
    qsort(times, ITERS, sizeof(double), cmp_double);
    result.difference_ns = percentile(times, ITERS, 0.50);

    /* --- Iterate benchmark --- */
    for (int i = 0; i < WARMUP; i++) {
        int64_t x = -1;
        while ((x = bms_next_member(handle, x)) >= 0)
            ;
    }
    for (int i = 0; i < ITERS; i++) {
        uint64_t t0 = now_ns();
        int64_t x = -1;
        while ((x = bms_next_member(handle, x)) >= 0)
            ;
        uint64_t t1 = now_ns();
        times[i] = (double)(t1 - t0);
    }
    qsort(times, ITERS, sizeof(double), cmp_double);
    result.iterate_ns = percentile(times, ITERS, 0.50);

    bms_free(handle);
    free(times);
    return result;
}

/* ===================================================================
 * Analysis 1: Offset sweep
 *
 * Same 1000-bit pattern, shifted by base_offset in 1M steps.
 * Shows where dense->chunked transition happens and its cost.
 * =================================================================== */

static void run_offset_sweep(void) {
    const size_t CARD = 1000;
    const char *patterns[] = { "dense", "sparse", "clustered" };
    const int npatterns = 3;

    fprintf(stderr, "\n");
    fprintf(stderr, "================================================================\n");
    fprintf(stderr, " Analysis 1: Offset Sweep (dense -> chunked transition)\n");
    fprintf(stderr, " BMS_DENSE_MAX_BIT = %lld  (nwords=%d)\n",
            (long long)BMS_DENSE_MAX_BIT, BMS_DENSE_MAX_NWORDS);
    fprintf(stderr, " Pattern: %zu bits shifted by offset\n", CARD);
    fprintf(stderr, "================================================================\n");

    /* CSV header */
    printf("# offset_sweep\n");
    printf("pattern,offset,mode,memory_bytes,populate_ns,contains_ns,cardinality_ns,"
           "union_ns,intersect_ns,difference_ns,iterate_ns\n");

    /* Offsets: 0, 1M, 2M, ... 10M, plus steps around the transition */
    int64_t offsets[] = {
        0,
        1000000,
        2000000,
        3000000,
        3500000,     /* approaching threshold */
        4000000,     /* near threshold (4,194,239) */
        4100000,
        4190000,     /* very close */
        4194000,     /* just below */
        4194240,     /* just above BMS_DENSE_MAX_BIT */
        4200000,
        4500000,
        5000000,
        6000000,
        7000000,
        8000000,
        9000000,
        10000000,
    };
    int noffsets = sizeof(offsets) / sizeof(offsets[0]);

    for (int p = 0; p < npatterns; p++) {
        fprintf(stderr, "\n--- Pattern: %s ---\n", patterns[p]);
        fprintf(stderr, "  %-12s %-6s %6s %10s %10s %10s %10s %10s %10s %10s\n",
                "offset", "mode", "mem", "populate", "contains", "card",
                "union", "isect", "diff", "iterate");

        for (int o = 0; o < noffsets; o++) {
            int64_t *bits = gen_test_bits(CARD, offsets[o], patterns[p]);
            bench_result_t r = bench_pattern(bits, CARD);
            free(bits);

            const char *mode = r.is_chunked ? "chunk" : "dense";

            fprintf(stderr, "  %-12lld %-6s %6zu %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f\n",
                    (long long)offsets[o], mode, r.memory_bytes,
                    r.populate_ns, r.contains_ns, r.cardinality_ns,
                    r.union_ns, r.intersect_ns, r.difference_ns, r.iterate_ns);

            printf("%s,%lld,%s,%zu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
                   patterns[p], (long long)offsets[o], mode, r.memory_bytes,
                   r.populate_ns, r.contains_ns, r.cardinality_ns,
                   r.union_ns, r.intersect_ns, r.difference_ns, r.iterate_ns);
        }
    }
}

/* ===================================================================
 * Analysis 2: Cutoff sweep
 *
 * We can't change BMS_DENSE_MAX_NWORDS at runtime since it's a
 * compile-time constant used in bitmapset_hybrid.c. Instead, we
 * test the question empirically: for a given bit pattern, at what
 * point is chunked mode faster/smaller than dense mode?
 *
 * We do this by benchmarking the SAME bit values at different offsets.
 * When max_bit < BMS_DENSE_MAX_BIT, it's dense. When above, chunked.
 * The "cutoff" question becomes: for what max_bit value does chunked
 * start to win?
 *
 * We test with fine-grained offsets around the transition region to
 * map the performance curve precisely. Since the actual pattern of
 * bits is the same (just shifted), the only variable is the storage
 * mode and the memory it consumes.
 *
 * For the "what should the cutoff be" question, we need to compare
 * dense-at-this-nwords vs chunked-at-this-nwords. We can approximate
 * this by testing patterns that fill exactly N words of dense storage,
 * measuring both dense and chunked performance. Dense performance
 * scales with nwords (O(W)), chunked performance scales with chunks.
 * =================================================================== */

static void run_cutoff_sweep(void) {
    fprintf(stderr, "\n");
    fprintf(stderr, "================================================================\n");
    fprintf(stderr, " Analysis 2: Cutoff Sweep -- dense vs chunked at different sizes\n");
    fprintf(stderr, " Testing: 100 bits placed at bit positions [0..max_bit]\n");
    fprintf(stderr, " Dense nwords = ceil(max_bit/64), chunked = 1 chunk\n");
    fprintf(stderr, "================================================================\n");

    printf("# cutoff_sweep\n");
    printf("nwords,max_bit,dense_mem,chunk_mem,dense_populate_ns,chunk_populate_ns,"
           "dense_contains_ns,chunk_contains_ns,dense_union_ns,chunk_union_ns,"
           "dense_iterate_ns,chunk_iterate_ns\n");

    /*
     * Strategy: place 100 bits with the highest bit at target max_bit.
     * Run at the target offset. Below BMS_DENSE_MAX_BIT it's dense,
     * above it's chunked. We test points on both sides.
     *
     * To compare dense vs chunked at the SAME logical nwords, we use:
     * - Dense test: offset=0, max bit = nwords*64 - 1 (always < threshold)
     * - Chunked test: offset = BMS_DENSE_MAX_BIT + 1, same spread
     *
     * For small nwords the dense test measures real dense perf.
     * The chunked test always uses chunked mode (bits above threshold).
     */

    const size_t CARD = 100;  /* 100 bits */

    /* Test points: nwords from 1 to 65535 at logarithmic intervals */
    int nwords_tests[] = {
        1, 2, 4, 8, 16, 32, 64, 128, 256, 512,
        1024, 2048, 4096, 8192, 16384, 32768, 65535
    };
    int ntests = sizeof(nwords_tests) / sizeof(nwords_tests[0]);

    fprintf(stderr, "  %-8s %-12s %8s %8s | %10s %10s | %10s %10s | %10s %10s | %10s %10s\n",
            "nwords", "max_bit", "d_mem", "c_mem",
            "d_pop", "c_pop", "d_cont", "c_cont",
            "d_union", "c_union", "d_iter", "c_iter");

    for (int t = 0; t < ntests; t++) {
        int nw = nwords_tests[t];
        int64_t max_bit_dense = (int64_t)nw * 64 - 1;

        /* Dense test: 100 bits spread across [0..max_bit_dense] */
        int64_t *dense_bits = malloc(CARD * sizeof(int64_t));
        for (size_t i = 0; i < CARD; i++) {
            /* Spread bits evenly: bit 0, max_bit/99, 2*max_bit/99, ... */
            dense_bits[i] = (int64_t)((double)max_bit_dense * i / (CARD - 1));
        }

        bench_result_t dr = bench_pattern(dense_bits, CARD);

        /* Chunked test: same spread but shifted above the threshold */
        int64_t chunk_offset = BMS_DENSE_MAX_BIT + 1;
        int64_t *chunk_bits = malloc(CARD * sizeof(int64_t));
        for (size_t i = 0; i < CARD; i++) {
            chunk_bits[i] = chunk_offset + (int64_t)((double)max_bit_dense * i / (CARD - 1));
        }

        bench_result_t cr = bench_pattern(chunk_bits, CARD);

        fprintf(stderr, "  %-8d %-12lld %8zu %8zu | %10.1f %10.1f | %10.1f %10.1f | %10.1f %10.1f | %10.1f %10.1f\n",
                nw, (long long)max_bit_dense,
                dr.memory_bytes, cr.memory_bytes,
                dr.populate_ns, cr.populate_ns,
                dr.contains_ns, cr.contains_ns,
                dr.union_ns, cr.union_ns,
                dr.iterate_ns, cr.iterate_ns);

        printf("%d,%lld,%zu,%zu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
               nw, (long long)max_bit_dense,
               dr.memory_bytes, cr.memory_bytes,
               dr.populate_ns, cr.populate_ns,
               dr.contains_ns, cr.contains_ns,
               dr.union_ns, cr.union_ns,
               dr.iterate_ns, cr.iterate_ns);

        free(dense_bits);
        free(chunk_bits);
    }
}

/* ===================================================================
 * Main
 * =================================================================== */

int main(int argc, char **argv) {
    bool do_offset = true;
    bool do_cutoff = true;

    if (argc > 1) {
        if (strcmp(argv[1], "--offset") == 0) {
            do_cutoff = false;
        } else if (strcmp(argv[1], "--cutoff") == 0) {
            do_offset = false;
        } else if (strcmp(argv[1], "--help") == 0) {
            fprintf(stderr, "Usage: %s [--offset|--cutoff]\n", argv[0]);
            fprintf(stderr, "  --offset   Only run the offset sweep (dense->chunked transition)\n");
            fprintf(stderr, "  --cutoff   Only run the cutoff sweep (optimal transition point)\n");
            fprintf(stderr, "  (default)  Run both analyses\n");
            return 0;
        }
    }

    if (do_offset)
        run_offset_sweep();

    if (do_cutoff)
        run_cutoff_sweep();

    fprintf(stderr, "\nDone.\n");
    return 0;
}
