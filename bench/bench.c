/* SPDX-License-Identifier: MIT */
/*
 * bench.c -- Comparative benchmark: sparsemap vs CRoaring vs PostgreSQL bitmapset
 *
 * Usage:
 *   ./test/bench [OPTIONS]
 *   ./test/bench --pattern=dense --cardinality=1000
 *   ./test/bench --verify
 *   ./test/bench > results.csv 2>summary.txt
 *
 * CSV output goes to stdout; human-readable summary to stderr.
 */

#define _POSIX_C_SOURCE 200112L

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include <sm.h>
#include <roaring.h>
#include <tdigest.h>

#include "bitmapset_standalone.h"

/* ===================================================================
 * Configuration
 * =================================================================== */

#define WARMUP_ITERS  100
#define BENCH_ITERS   10000
#define TD_COMPRESSION 100.0

/* Maximum bits for bitmapset (it allocates O(max_member) memory) */
#define BMS_MAX_UNIVERSE 2000000

/* ===================================================================
 * Timing
 * =================================================================== */

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ===================================================================
 * Pattern generators
 *
 * Each returns a sorted uint32_t array of set-bit positions.
 * Caller must free() the result.
 * =================================================================== */

/* Simple xorshift32 PRNG */
static uint32_t bench_rng_state = 2463534242U;

static void bench_rng_seed(uint32_t s) {
    bench_rng_state = s ? s : 2463534242U;
}

static uint32_t bench_rng(void) {
    uint32_t x = bench_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    bench_rng_state = x;
    return x;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t va = *(const uint32_t *)a;
    uint32_t vb = *(const uint32_t *)b;
    return (va > vb) - (va < vb);
}

/* Remove duplicates from a sorted array; returns new length */
static size_t dedup_u32(uint32_t *arr, size_t n) {
    if (n <= 1) return n;
    size_t j = 1;
    for (size_t i = 1; i < n; i++) {
        if (arr[i] != arr[i - 1])
            arr[j++] = arr[i];
    }
    return j;
}

/* 1. Dense sequential: bits [0, N) */
static uint32_t *gen_dense(size_t n, size_t *out_count, uint32_t *out_universe) {
    uint32_t *bits = malloc(n * sizeof(uint32_t));
    for (size_t i = 0; i < n; i++)
        bits[i] = (uint32_t)i;
    *out_count = n;
    *out_universe = (uint32_t)n;
    return bits;
}

/* 2. Sparse random: N random bits in [0, U) */
static uint32_t *gen_sparse_random(size_t n, size_t universe,
                                    size_t *out_count, uint32_t *out_universe) {
    /* Over-generate to handle duplicates */
    size_t alloc = n + n / 2 + 64;
    uint32_t *bits = malloc(alloc * sizeof(uint32_t));
    size_t generated = 0;
    while (generated < n) {
        size_t batch = n - generated + 64;
        if (generated + batch > alloc) {
            alloc = generated + batch + 64;
            bits = realloc(bits, alloc * sizeof(uint32_t));
        }
        for (size_t i = 0; i < batch; i++)
            bits[generated + i] = bench_rng() % (uint32_t)universe;
        generated += batch;
        qsort(bits, generated, sizeof(uint32_t), cmp_u32);
        generated = dedup_u32(bits, generated);
    }
    *out_count = n;
    *out_universe = (uint32_t)universe;
    return bits;
}

/* 3. Periodic: every K-th bit in [0, N*K) */
static uint32_t *gen_periodic(size_t n, size_t period,
                               size_t *out_count, uint32_t *out_universe) {
    uint32_t *bits = malloc(n * sizeof(uint32_t));
    for (size_t i = 0; i < n; i++)
        bits[i] = (uint32_t)(i * period);
    *out_count = n;
    *out_universe = (uint32_t)(n * period);
    return bits;
}

/* 4. Clustered: C clusters of D bits, separated by gaps of G */
static uint32_t *gen_clustered(size_t clusters, size_t density, size_t gap,
                                size_t *out_count, uint32_t *out_universe) {
    size_t total = clusters * density;
    uint32_t *bits = malloc(total * sizeof(uint32_t));
    size_t idx = 0;
    uint32_t pos = 0;
    for (size_t c = 0; c < clusters; c++) {
        for (size_t d = 0; d < density; d++) {
            bits[idx++] = pos + (uint32_t)d;
        }
        pos += (uint32_t)(density + gap);
    }
    *out_count = total;
    *out_universe = pos;
    return bits;
}

/* 5. Power-law: dense at start, sparse toward end */
static uint32_t *gen_powerlaw(size_t n, size_t universe,
                               size_t *out_count, uint32_t *out_universe) {
    uint32_t *bits = malloc(n * sizeof(uint32_t));
    for (size_t i = 0; i < n; i++) {
        /* Power-law: x = U * (i/N)^2 */
        double frac = (double)i / (double)n;
        bits[i] = (uint32_t)(frac * frac * (double)(universe - 1));
    }
    qsort(bits, n, sizeof(uint32_t), cmp_u32);
    size_t deduped = dedup_u32(bits, n);
    *out_count = deduped;
    *out_universe = (uint32_t)universe;
    return bits;
}

/* 6. Single dense block: one contiguous block of M bits at offset */
static uint32_t *gen_single_block(size_t block_size, size_t offset,
                                   size_t *out_count, uint32_t *out_universe) {
    uint32_t *bits = malloc(block_size * sizeof(uint32_t));
    for (size_t i = 0; i < block_size; i++)
        bits[i] = (uint32_t)(offset + i);
    *out_count = block_size;
    *out_universe = (uint32_t)(offset + block_size);
    return bits;
}

/* 7. Alternating blocks: blocks of W on, W off */
static uint32_t *gen_alternating(size_t n, size_t block_width,
                                  size_t *out_count, uint32_t *out_universe) {
    uint32_t *bits = malloc(n * sizeof(uint32_t));
    size_t idx = 0;
    uint32_t pos = 0;
    while (idx < n) {
        for (size_t i = 0; i < block_width && idx < n; i++) {
            bits[idx++] = pos + (uint32_t)i;
        }
        pos += (uint32_t)(block_width * 2); /* skip the off-block */
    }
    *out_count = n;
    *out_universe = pos;
    return bits;
}

/* 8. Worst-case sparse: every other bit (0, 2, 4, ...) */
static uint32_t *gen_worst_sparse(size_t n, size_t *out_count, uint32_t *out_universe) {
    uint32_t *bits = malloc(n * sizeof(uint32_t));
    for (size_t i = 0; i < n; i++)
        bits[i] = (uint32_t)(i * 2);
    *out_count = n;
    *out_universe = (uint32_t)(n * 2);
    return bits;
}

/* ===================================================================
 * Per-operation benchmark contexts and callbacks
 *
 * Each operation has a separate callback for each library, plus a
 * small context struct to hold the pre-populated handle and any
 * operation-specific parameters.
 * =================================================================== */

/* ===================================================================
 * Pattern definition
 * =================================================================== */

typedef struct {
    const char *name;
    uint32_t *bits;
    size_t count;
    uint32_t universe;
} pattern_t;

/* ===================================================================
 * Benchmark runner
 * =================================================================== */

typedef struct {
    const char *pattern_name;
    size_t cardinality;
    uint32_t universe;
    const char *operation;
    const char *library;
    double p50_ns;
    double p90_ns;
    double p99_ns;
    double ops_per_sec;
    size_t memory_bytes;
} bench_result_t;

static void emit_csv_header(void) {
    printf("pattern,cardinality,universe,operation,library,p50_ns,p90_ns,p99_ns,ops_per_sec,memory_bytes\n");
}

static void emit_csv_row(bench_result_t *r) {
    printf("%s,%zu,%u,%s,%s,%.1f,%.1f,%.1f,%.0f,%zu\n",
           r->pattern_name, r->cardinality, r->universe,
           r->operation, r->library,
           r->p50_ns, r->p90_ns, r->p99_ns,
           r->ops_per_sec, r->memory_bytes);
}

static void emit_stderr_row(bench_result_t *r) {
    fprintf(stderr, "  %-12s %-10s %-12s  p50=%8.1f ns  p90=%8.1f ns  p99=%8.1f ns  mem=%zu\n",
            r->library, r->pattern_name, r->operation,
            r->p50_ns, r->p90_ns, r->p99_ns, r->memory_bytes);
}

/* Run a single benchmark: call `op` BENCH_ITERS times, record into t-digest */
static void run_timed(td_histogram_t *td, void (*op)(void *ctx), void *ctx, int iters) {
    /* Warmup */
    for (int i = 0; i < WARMUP_ITERS; i++)
        op(ctx);

    /* Timed runs */
    for (int i = 0; i < iters; i++) {
        uint64_t t0 = now_ns();
        op(ctx);
        uint64_t t1 = now_ns();
        td_add(td, (double)(t1 - t0), 1);
    }
}

/* --- Populate --- */
typedef struct {
    const uint32_t *bits;
    size_t count;
} populate_ctx_t;

static void op_populate_sm(void *ctx_) {
    populate_ctx_t *ctx = ctx_;
    sparsemap_t *map = sparsemap(1024 * 1024);
    for (size_t i = 0; i < ctx->count; i++) {
        uint64_t r;
        do {
            r = sm_add(map, ctx->bits[i]);
            if (SM_NOT_FOUND(r) && errno == ENOSPC) {
                size_t cap = sm_get_capacity(map);
                map = sm_set_data_size(map, NULL, cap * 2);
                errno = 0;
            }
        } while (SM_NOT_FOUND(r));
    }
    free(map);
}

static void op_populate_rb(void *ctx_) {
    populate_ctx_t *ctx = ctx_;
    roaring_bitmap_t *r = roaring_bitmap_create();
    for (size_t i = 0; i < ctx->count; i++)
        roaring_bitmap_add(r, ctx->bits[i]);
    roaring_bitmap_free(r);
}

static void op_populate_bms(void *ctx_) {
    populate_ctx_t *ctx = ctx_;
    Bitmapset *b = NULL;
    for (size_t i = 0; i < ctx->count; i++)
        b = bms_add_member(b, (int)ctx->bits[i]);
    bms_free(b);
}

/* --- Contains (test all bits) --- */
typedef struct {
    void *handle;
    const uint32_t *bits;
    size_t count;
    volatile bool sink;
} contains_ctx_t;

static void op_contains_sm(void *ctx_) {
    contains_ctx_t *ctx = ctx_;
    for (size_t i = 0; i < ctx->count; i++)
        ctx->sink = sm_contains(ctx->handle, ctx->bits[i]);
}

static void op_contains_rb(void *ctx_) {
    contains_ctx_t *ctx = ctx_;
    for (size_t i = 0; i < ctx->count; i++)
        ctx->sink = roaring_bitmap_contains(ctx->handle, ctx->bits[i]);
}

static void op_contains_bms(void *ctx_) {
    contains_ctx_t *ctx = ctx_;
    for (size_t i = 0; i < ctx->count; i++)
        ctx->sink = bms_is_member((int)ctx->bits[i], ctx->handle);
}

/* --- Cardinality --- */
typedef struct {
    void *handle;
    volatile size_t sink;
} card_ctx_t;

static void op_card_sm(void *ctx_) {
    card_ctx_t *ctx = ctx_;
    ctx->sink = sm_cardinality(ctx->handle);
}

static void op_card_rb(void *ctx_) {
    card_ctx_t *ctx = ctx_;
    ctx->sink = (size_t)roaring_bitmap_get_cardinality(ctx->handle);
}

static void op_card_bms(void *ctx_) {
    card_ctx_t *ctx = ctx_;
    ctx->sink = (size_t)bms_num_members(ctx->handle);
}

/* --- Rank at midpoint --- */
typedef struct {
    void *handle;
    uint32_t mid;
    volatile size_t sink;
} rank_ctx_t;

static void op_rank_sm(void *ctx_) {
    rank_ctx_t *ctx = ctx_;
    ctx->sink = sm_rank(ctx->handle, 0, ctx->mid, true);
}

static void op_rank_rb(void *ctx_) {
    rank_ctx_t *ctx = ctx_;
    ctx->sink = (size_t)roaring_bitmap_rank(ctx->handle, ctx->mid);
}

static void op_rank_bms(void *ctx_) {
    rank_ctx_t *ctx = ctx_;
    size_t c = 0;
    int x = -1;
    while ((x = bms_next_member(ctx->handle, x)) >= 0) {
        if ((uint32_t)x <= ctx->mid) c++;
        else break;
    }
    ctx->sink = c;
}

/* --- Select (n-th bit) --- */
typedef struct {
    void *handle;
    uint32_t n;
    volatile uint32_t sink;
} select_ctx_t;

static void op_select_sm(void *ctx_) {
    select_ctx_t *ctx = ctx_;
    uint64_t r = sm_select(ctx->handle, ctx->n, true);
    ctx->sink = (uint32_t)r;
}

static void op_select_rb(void *ctx_) {
    select_ctx_t *ctx = ctx_;
    uint32_t val;
    roaring_bitmap_select(ctx->handle, ctx->n, &val);
    ctx->sink = val;
}

static void op_select_bms(void *ctx_) {
    select_ctx_t *ctx = ctx_;
    int x = -1;
    uint32_t count = 0;
    while ((x = bms_next_member(ctx->handle, x)) >= 0) {
        if (count == ctx->n) { ctx->sink = (uint32_t)x; return; }
        count++;
    }
}

/* --- Union --- */
typedef struct {
    void *a;
    void *b;
    void (*destroy)(void *);
} union_ctx_t;

static void op_union_sm(void *ctx_) {
    union_ctx_t *ctx = ctx_;
    sparsemap_t *r = sm_union(ctx->a, ctx->b);
    free(r);
}

static void op_union_rb(void *ctx_) {
    union_ctx_t *ctx = ctx_;
    roaring_bitmap_t *r = roaring_bitmap_or(ctx->a, ctx->b);
    roaring_bitmap_free(r);
}

static void op_union_bms(void *ctx_) {
    union_ctx_t *ctx = ctx_;
    Bitmapset *r = bms_union(ctx->a, ctx->b);
    bms_free(r);
}

/* --- Intersection --- */
typedef struct {
    void *a;
    void *b;
} intersect_ctx_t;

static void op_intersect_sm(void *ctx_) {
    intersect_ctx_t *ctx = ctx_;
    sparsemap_t *r = sm_intersection(ctx->a, ctx->b);
    if (r) free(r);
}

static void op_intersect_rb(void *ctx_) {
    intersect_ctx_t *ctx = ctx_;
    roaring_bitmap_t *r = roaring_bitmap_and(ctx->a, ctx->b);
    roaring_bitmap_free(r);
}

static void op_intersect_bms(void *ctx_) {
    intersect_ctx_t *ctx = ctx_;
    Bitmapset *r = bms_intersect(ctx->a, ctx->b);
    bms_free(r);
}

/* --- Difference --- */
typedef struct {
    void *a;
    void *b;
} difference_ctx_t;

static void op_difference_sm(void *ctx_) {
    difference_ctx_t *ctx = ctx_;
    sparsemap_t *r = sm_difference(ctx->a, ctx->b);
    if (r) free(r);
}

static void op_difference_rb(void *ctx_) {
    difference_ctx_t *ctx = ctx_;
    roaring_bitmap_t *r = roaring_bitmap_andnot(ctx->a, ctx->b);
    roaring_bitmap_free(r);
}

static void op_difference_bms(void *ctx_) {
    difference_ctx_t *ctx = ctx_;
    Bitmapset *r = bms_difference(ctx->a, ctx->b);
    bms_free(r);
}

/* --- Iterate --- */
typedef struct {
    void *handle;
    volatile uint64_t checksum;
} iter_ctx_t;

static void sm_bench_scan_cb(uint32_t vec[], size_t n, void *aux) {
    volatile uint64_t *cs = aux;
    for (size_t i = 0; i < n; i++)
        *cs += vec[i];
}

static void op_iter_sm(void *ctx_) {
    iter_ctx_t *ctx = ctx_;
    ctx->checksum = 0;
    sm_scan(ctx->handle, sm_bench_scan_cb, 0, (void *)&ctx->checksum);
}

static void op_iter_rb(void *ctx_) {
    iter_ctx_t *ctx = ctx_;
    ctx->checksum = 0;
    roaring_uint32_iterator_t *it = roaring_iterator_create(ctx->handle);
    while (it->has_value) {
        ctx->checksum += it->current_value;
        roaring_uint32_iterator_advance(it);
    }
    roaring_uint32_iterator_free(it);
}

static void op_iter_bms(void *ctx_) {
    iter_ctx_t *ctx = ctx_;
    ctx->checksum = 0;
    int x = -1;
    while ((x = bms_next_member(ctx->handle, x)) >= 0)
        ctx->checksum += (uint32_t)x;
}

/* --- Offset --- */
typedef struct {
    void *handle;
    ssize_t offset;
} offset_ctx_t;

static void op_offset_sm(void *ctx_) {
    offset_ctx_t *ctx = ctx_;
    sparsemap_t *r = sm_offset(ctx->handle, ctx->offset);
    if (r) free(r);
}

static void op_offset_rb(void *ctx_) {
    offset_ctx_t *ctx = ctx_;
    roaring_bitmap_t *r = roaring_bitmap_add_offset(ctx->handle, (int64_t)ctx->offset);
    if (r) roaring_bitmap_free(r);
}

static void op_offset_bms(void *ctx_) {
    offset_ctx_t *ctx = ctx_;
    Bitmapset *r = bms_offset_members(ctx->handle, (int)ctx->offset);
    if (r) bms_free(r);
}

/* --- Minimum --- */
typedef struct {
    void *handle;
    volatile uint32_t sink;
} minmax_ctx_t;

static void op_min_sm(void *ctx_) {
    minmax_ctx_t *ctx = ctx_;
    ctx->sink = (uint32_t)sm_minimum(ctx->handle);
}

static void op_min_rb(void *ctx_) {
    minmax_ctx_t *ctx = ctx_;
    ctx->sink = roaring_bitmap_minimum(ctx->handle);
}

static void op_min_bms(void *ctx_) {
    minmax_ctx_t *ctx = ctx_;
    int r = bms_next_member(ctx->handle, -1);
    ctx->sink = (r >= 0) ? (uint32_t)r : 0;
}

/* --- Maximum --- */
static void op_max_sm(void *ctx_) {
    minmax_ctx_t *ctx = ctx_;
    ctx->sink = (uint32_t)sm_maximum(ctx->handle);
}

static void op_max_rb(void *ctx_) {
    minmax_ctx_t *ctx = ctx_;
    ctx->sink = roaring_bitmap_maximum(ctx->handle);
}

static void op_max_bms(void *ctx_) {
    minmax_ctx_t *ctx = ctx_;
    int last = -1, x = -1;
    while ((x = bms_next_member(ctx->handle, x)) >= 0)
        last = x;
    ctx->sink = (last >= 0) ? (uint32_t)last : 0;
}

/* ===================================================================
 * Main benchmark orchestration
 * =================================================================== */

static void bench_one_pattern(pattern_t *pat, bool skip_bms, bool verify_only) {
    const uint32_t *bits = pat->bits;
    size_t count = pat->count;

    fprintf(stderr, "\nPattern: %-20s  cardinality=%zu  universe=%u\n",
            pat->name, count, pat->universe);

    /* --- Build pre-populated handles for query benchmarks --- */
    sparsemap_t *sm_handle = sparsemap(1024 * 1024);
    for (size_t i = 0; i < count; i++) {
        uint64_t r;
        do {
            r = sm_add(sm_handle, bits[i]);
            if (SM_NOT_FOUND(r) && errno == ENOSPC) {
                size_t cap = sm_get_capacity(sm_handle);
                sm_handle = sm_set_data_size(sm_handle, NULL, cap * 2);
                errno = 0;
            }
        } while (SM_NOT_FOUND(r));
    }

    roaring_bitmap_t *rb_handle = roaring_bitmap_create();
    for (size_t i = 0; i < count; i++)
        roaring_bitmap_add(rb_handle, bits[i]);

    Bitmapset *bms_handle = NULL;
    if (!skip_bms) {
        for (size_t i = 0; i < count; i++)
            bms_handle = bms_add_member(bms_handle, (int)bits[i]);
    }

    /* --- Verify cross-library correctness --- */
    size_t sm_card = sm_cardinality(sm_handle);
    size_t rb_card = (size_t)roaring_bitmap_get_cardinality(rb_handle);
    size_t bms_card = skip_bms ? sm_card : (size_t)bms_num_members(bms_handle);

    if (sm_card != rb_card || sm_card != bms_card) {
        fprintf(stderr, "  VERIFY FAIL: cardinality mismatch sm=%zu rb=%zu bms=%zu\n",
                sm_card, rb_card, bms_card);
    } else {
        fprintf(stderr, "  Cardinality verified: %zu across all libraries\n", sm_card);
    }

    /* Verify contains for a sample of bits */
    for (size_t i = 0; i < count && i < 100; i++) {
        bool sm_has = sm_contains(sm_handle, bits[i]);
        bool rb_has = roaring_bitmap_contains(rb_handle, bits[i]);
        bool bms_has = skip_bms ? sm_has : bms_is_member((int)bits[i], bms_handle);
        if (!sm_has || !rb_has || !bms_has) {
            fprintf(stderr, "  VERIFY FAIL: bit %u missing in sm=%d rb=%d bms=%d\n",
                    bits[i], sm_has, rb_has, bms_has);
            break;
        }
    }

    if (verify_only) {
        fprintf(stderr, "  Verification complete.\n");
        goto cleanup;
    }

    /* --- Memory --- */
    size_t sm_mem = sm_get_size(sm_handle) + 32; /* opaque struct overhead */
    size_t rb_mem = roaring_bitmap_size_in_bytes(rb_handle);
    size_t bms_mem = skip_bms ? 0 :
        (offsetof(Bitmapset, words) + (size_t)bms_handle->nwords * sizeof(bitmapword));

    fprintf(stderr, "  Memory: sparsemap=%zu  croaring=%zu  bitmapset=%zu\n",
            sm_mem, rb_mem, bms_mem);

    /* Midpoint for rank */
    uint32_t midpoint = count > 0 ? bits[count / 2] : 0;
    /* n for select */
    uint32_t sel_n = count > 0 ? (uint32_t)(count / 2) : 0;

    /* -----------------------------------------------------------
     * Run benchmarks for each operation x library
     * ----------------------------------------------------------- */

    struct {
        const char *op_name;
        void (*sm_op)(void *);
        void (*rb_op)(void *);
        void (*bms_op)(void *);
        void *sm_ctx;
        void *rb_ctx;
        void *bms_ctx;
        size_t sm_mem_val;
        size_t rb_mem_val;
        size_t bms_mem_val;
    } benchmarks[13];
    int num_benchmarks = 0;

    /* Populate */
    populate_ctx_t pop_ctx = { .bits = bits, .count = count };
    benchmarks[num_benchmarks++] = (__typeof__(benchmarks[0])){
        "populate",
        op_populate_sm, op_populate_rb, op_populate_bms,
        &pop_ctx, &pop_ctx, &pop_ctx,
        sm_mem, rb_mem, bms_mem
    };

    /* Contains */
    contains_ctx_t cont_sm = { .handle = sm_handle, .bits = bits, .count = count < 1000 ? count : 1000 };
    contains_ctx_t cont_rb = { .handle = rb_handle, .bits = bits, .count = cont_sm.count };
    contains_ctx_t cont_bms = { .handle = bms_handle, .bits = bits, .count = cont_sm.count };
    benchmarks[num_benchmarks++] = (__typeof__(benchmarks[0])){
        "contains",
        op_contains_sm, op_contains_rb, op_contains_bms,
        &cont_sm, &cont_rb, &cont_bms,
        sm_mem, rb_mem, bms_mem
    };

    /* Cardinality */
    card_ctx_t card_sm = { .handle = sm_handle };
    card_ctx_t card_rb = { .handle = rb_handle };
    card_ctx_t card_bms = { .handle = bms_handle };
    benchmarks[num_benchmarks++] = (__typeof__(benchmarks[0])){
        "cardinality",
        op_card_sm, op_card_rb, op_card_bms,
        &card_sm, &card_rb, &card_bms,
        sm_mem, rb_mem, bms_mem
    };

    /* Rank */
    rank_ctx_t rank_sm_c = { .handle = sm_handle, .mid = midpoint };
    rank_ctx_t rank_rb_c = { .handle = rb_handle, .mid = midpoint };
    rank_ctx_t rank_bms_c = { .handle = bms_handle, .mid = midpoint };
    benchmarks[num_benchmarks++] = (__typeof__(benchmarks[0])){
        "rank",
        op_rank_sm, op_rank_rb, op_rank_bms,
        &rank_sm_c, &rank_rb_c, &rank_bms_c,
        sm_mem, rb_mem, bms_mem
    };

    /* Select */
    select_ctx_t sel_sm = { .handle = sm_handle, .n = sel_n };
    select_ctx_t sel_rb = { .handle = rb_handle, .n = sel_n };
    select_ctx_t sel_bms = { .handle = bms_handle, .n = sel_n };
    benchmarks[num_benchmarks++] = (__typeof__(benchmarks[0])){
        "select",
        op_select_sm, op_select_rb, op_select_bms,
        &sel_sm, &sel_rb, &sel_bms,
        sm_mem, rb_mem, bms_mem
    };

    /* Union */
    union_ctx_t union_sm_c = { .a = sm_handle, .b = sm_handle };
    union_ctx_t union_rb_c = { .a = rb_handle, .b = rb_handle };
    union_ctx_t union_bms_c = { .a = bms_handle, .b = bms_handle };
    benchmarks[num_benchmarks++] = (__typeof__(benchmarks[0])){
        "union",
        op_union_sm, op_union_rb, op_union_bms,
        &union_sm_c, &union_rb_c, &union_bms_c,
        sm_mem, rb_mem, bms_mem
    };

    /* Intersection */
    intersect_ctx_t isect_sm_c = { .a = sm_handle, .b = sm_handle };
    intersect_ctx_t isect_rb_c = { .a = rb_handle, .b = rb_handle };
    intersect_ctx_t isect_bms_c = { .a = bms_handle, .b = bms_handle };
    benchmarks[num_benchmarks++] = (__typeof__(benchmarks[0])){
        "intersection",
        op_intersect_sm, op_intersect_rb, op_intersect_bms,
        &isect_sm_c, &isect_rb_c, &isect_bms_c,
        sm_mem, rb_mem, bms_mem
    };

    /* Difference */
    difference_ctx_t diff_sm_c = { .a = sm_handle, .b = sm_handle };
    difference_ctx_t diff_rb_c = { .a = rb_handle, .b = rb_handle };
    difference_ctx_t diff_bms_c = { .a = bms_handle, .b = bms_handle };
    benchmarks[num_benchmarks++] = (__typeof__(benchmarks[0])){
        "difference",
        op_difference_sm, op_difference_rb, op_difference_bms,
        &diff_sm_c, &diff_rb_c, &diff_bms_c,
        sm_mem, rb_mem, bms_mem
    };

    /* Iterate */
    iter_ctx_t iter_sm_c = { .handle = sm_handle };
    iter_ctx_t iter_rb_c = { .handle = rb_handle };
    iter_ctx_t iter_bms_c = { .handle = bms_handle };
    benchmarks[num_benchmarks++] = (__typeof__(benchmarks[0])){
        "iterate",
        op_iter_sm, op_iter_rb, op_iter_bms,
        &iter_sm_c, &iter_rb_c, &iter_bms_c,
        sm_mem, rb_mem, bms_mem
    };

    /* Offset (+64) */
    offset_ctx_t off_sm = { .handle = sm_handle, .offset = 64 };
    offset_ctx_t off_rb = { .handle = rb_handle, .offset = 64 };
    offset_ctx_t off_bms = { .handle = bms_handle, .offset = 64 };
    benchmarks[num_benchmarks++] = (__typeof__(benchmarks[0])){
        "offset",
        op_offset_sm, op_offset_rb, op_offset_bms,
        &off_sm, &off_rb, &off_bms,
        sm_mem, rb_mem, bms_mem
    };

    /* Minimum */
    minmax_ctx_t min_sm = { .handle = sm_handle };
    minmax_ctx_t min_rb = { .handle = rb_handle };
    minmax_ctx_t min_bms = { .handle = bms_handle };
    benchmarks[num_benchmarks++] = (__typeof__(benchmarks[0])){
        "minimum",
        op_min_sm, op_min_rb, op_min_bms,
        &min_sm, &min_rb, &min_bms,
        sm_mem, rb_mem, bms_mem
    };

    /* Maximum */
    minmax_ctx_t max_sm = { .handle = sm_handle };
    minmax_ctx_t max_rb = { .handle = rb_handle };
    minmax_ctx_t max_bms = { .handle = bms_handle };
    benchmarks[num_benchmarks++] = (__typeof__(benchmarks[0])){
        "maximum",
        op_max_sm, op_max_rb, op_max_bms,
        &max_sm, &max_rb, &max_bms,
        sm_mem, rb_mem, bms_mem
    };

    /* --- Run all benchmarks --- */
    for (int b = 0; b < num_benchmarks; b++) {
        /* Reduce iterations for expensive operations on large inputs */
        int iters = BENCH_ITERS;
        if (count > 50000 &&
            (strcmp(benchmarks[b].op_name, "populate") == 0 ||
             strcmp(benchmarks[b].op_name, "iterate") == 0 ||
             strcmp(benchmarks[b].op_name, "offset") == 0 ||
             strcmp(benchmarks[b].op_name, "union") == 0 ||
             strcmp(benchmarks[b].op_name, "intersection") == 0 ||
             strcmp(benchmarks[b].op_name, "difference") == 0 ||
             strcmp(benchmarks[b].op_name, "contains") == 0)) {
            iters = 100;
        } else if (count > 10000 &&
                   (strcmp(benchmarks[b].op_name, "populate") == 0 ||
                    strcmp(benchmarks[b].op_name, "intersection") == 0 ||
                    strcmp(benchmarks[b].op_name, "difference") == 0 ||
                    strcmp(benchmarks[b].op_name, "offset") == 0)) {
            iters = 1000;
        }

        struct {
            const char *lib_name;
            void (*op)(void *);
            void *ctx;
            size_t mem;
            bool skip;
        } libs[3] = {
            { "sparsemap", benchmarks[b].sm_op, benchmarks[b].sm_ctx, benchmarks[b].sm_mem_val, false },
            { "croaring",  benchmarks[b].rb_op, benchmarks[b].rb_ctx, benchmarks[b].rb_mem_val, false },
            { "bitmapset", benchmarks[b].bms_op, benchmarks[b].bms_ctx, benchmarks[b].bms_mem_val, skip_bms },
        };

        for (int l = 0; l < 3; l++) {
            if (libs[l].skip) continue;

            td_histogram_t *td = td_new(TD_COMPRESSION);

            run_timed(td, libs[l].op, libs[l].ctx, iters);

            double p50 = td_quantile(td, 0.50);
            double p90 = td_quantile(td, 0.90);
            double p99 = td_quantile(td, 0.99);
            double ops = (p50 > 0) ? 1e9 / p50 : 0;

            bench_result_t result = {
                .pattern_name = pat->name,
                .cardinality = count,
                .universe = pat->universe,
                .operation = benchmarks[b].op_name,
                .library = libs[l].lib_name,
                .p50_ns = p50,
                .p90_ns = p90,
                .p99_ns = p99,
                .ops_per_sec = ops,
                .memory_bytes = libs[l].mem,
            };

            emit_csv_row(&result);
            emit_stderr_row(&result);

            td_free(td);
        }
    }

cleanup:
    free(sm_handle);
    roaring_bitmap_free(rb_handle);
    if (bms_handle) bms_free(bms_handle);
}

/* ===================================================================
 * CLI and main
 * =================================================================== */

static void usage(void) {
    fprintf(stderr,
        "Usage: bench [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --pattern=NAME     Run only this pattern (dense, sparse, periodic,\n"
        "                     clustered, powerlaw, block, alternating, worst)\n"
        "  --cardinality=N    Override default cardinality\n"
        "  --verify           Only verify correctness, don't benchmark\n"
        "  --help             Show this help\n"
        "\n"
        "CSV output goes to stdout, human-readable summary to stderr.\n"
    );
}

int main(int argc, char **argv) {
    const char *filter_pattern = NULL;
    size_t override_cardinality = 0;
    bool verify_only = false;

    static struct option long_options[] = {
        { "pattern",     required_argument, NULL, 'p' },
        { "cardinality", required_argument, NULL, 'c' },
        { "verify",      no_argument,       NULL, 'v' },
        { "help",        no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:c:vh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'p': filter_pattern = optarg; break;
        case 'c': override_cardinality = (size_t)atol(optarg); break;
        case 'v': verify_only = true; break;
        case 'h': usage(); return 0;
        default:  usage(); return 1;
        }
    }

    bench_rng_seed(42);  /* Deterministic for reproducibility */

    emit_csv_header();

    /* Default cardinalities to test */
    size_t default_card = override_cardinality ? override_cardinality : 10000;

    /* Build patterns */
    #define MAX_PATTERNS 8
    pattern_t patterns[MAX_PATTERNS];
    int npat = 0;

    size_t cnt;
    uint32_t uni;

    /* 1. Dense sequential */
    if (!filter_pattern || strcmp(filter_pattern, "dense") == 0) {
        patterns[npat].name = "dense";
        patterns[npat].bits = gen_dense(default_card, &cnt, &uni);
        patterns[npat].count = cnt;
        patterns[npat].universe = uni;
        npat++;
    }

    /* 2. Sparse random */
    if (!filter_pattern || strcmp(filter_pattern, "sparse") == 0) {
        patterns[npat].name = "sparse";
        patterns[npat].bits = gen_sparse_random(default_card, default_card * 10, &cnt, &uni);
        patterns[npat].count = cnt;
        patterns[npat].universe = uni;
        npat++;
    }

    /* 3. Periodic (every 8th bit) */
    if (!filter_pattern || strcmp(filter_pattern, "periodic") == 0) {
        patterns[npat].name = "periodic";
        patterns[npat].bits = gen_periodic(default_card, 8, &cnt, &uni);
        patterns[npat].count = cnt;
        patterns[npat].universe = uni;
        npat++;
    }

    /* 4. Clustered (100 clusters of 100 bits, gaps of 500) */
    if (!filter_pattern || strcmp(filter_pattern, "clustered") == 0) {
        size_t clusters = default_card / 100;
        if (clusters < 1) clusters = 1;
        patterns[npat].name = "clustered";
        patterns[npat].bits = gen_clustered(clusters, 100, 500, &cnt, &uni);
        patterns[npat].count = cnt;
        patterns[npat].universe = uni;
        npat++;
    }

    /* 5. Power-law */
    if (!filter_pattern || strcmp(filter_pattern, "powerlaw") == 0) {
        patterns[npat].name = "powerlaw";
        patterns[npat].bits = gen_powerlaw(default_card, default_card * 10, &cnt, &uni);
        patterns[npat].count = cnt;
        patterns[npat].universe = uni;
        npat++;
    }

    /* 6. Single dense block */
    if (!filter_pattern || strcmp(filter_pattern, "block") == 0) {
        patterns[npat].name = "block";
        patterns[npat].bits = gen_single_block(default_card, 1000, &cnt, &uni);
        patterns[npat].count = cnt;
        patterns[npat].universe = uni;
        npat++;
    }

    /* 7. Alternating blocks (64 on, 64 off) */
    if (!filter_pattern || strcmp(filter_pattern, "alternating") == 0) {
        patterns[npat].name = "alternating";
        patterns[npat].bits = gen_alternating(default_card, 64, &cnt, &uni);
        patterns[npat].count = cnt;
        patterns[npat].universe = uni;
        npat++;
    }

    /* 8. Worst-case sparse (every other bit) */
    if (!filter_pattern || strcmp(filter_pattern, "worst") == 0) {
        patterns[npat].name = "worst";
        patterns[npat].bits = gen_worst_sparse(default_card, &cnt, &uni);
        patterns[npat].count = cnt;
        patterns[npat].universe = uni;
        npat++;
    }

    /* Run benchmarks */
    for (int i = 0; i < npat; i++) {
        /* Skip bitmapset for large universes (it allocates O(max) memory) */
        bool skip_bms = (patterns[i].universe > BMS_MAX_UNIVERSE);
        if (skip_bms) {
            fprintf(stderr, "\nSkipping bitmapset for pattern '%s' (universe=%u > %d)\n",
                    patterns[i].name, patterns[i].universe, BMS_MAX_UNIVERSE);
        }

        bench_one_pattern(&patterns[i], skip_bms, verify_only);
        free(patterns[i].bits);
    }

    fprintf(stderr, "\nBenchmark complete.\n");
    return 0;
}
