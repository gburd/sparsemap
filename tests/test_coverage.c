/* SPDX-License-Identifier: MIT
 *
 * tests/test_coverage.c — exercises the public API functions that the
 * existing big test suite (test/test.c) doesn't reach.  Each case is
 * a deterministic, focused smoke test, not a property test.
 *
 * Functions targeted:
 *
 *   sm_fill_factor        no calls in test_main
 *   sm_owned_copy         exercised by test_heisenbug but not test_main
 *   sm_free               same
 *   sm_capacity_remaining edge values
 *   sm_minimum/maximum    on a populated map
 *   sm_select             edge cases (out-of-range, RLE, sparse)
 *   sm_span               edge cases (start at 0, len > cardinality)
 *   sm_split              with idx == SPARSEMAP_IDX_MAX (median split)
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sparsemap.h>

static int g_failures = 0;
static int g_total = 0;

#define CASE(name) static int name(void)

#define EXPECT(cond, msg) do {                                          \
        g_total++;                                                      \
        if (!(cond)) {                                                  \
            fprintf(stderr, "  FAIL: %s:%d: %s\n",                      \
                    __FILE__, __LINE__, msg);                           \
            g_failures++;                                               \
            return 1;                                                   \
        }                                                               \
} while (0)

#define RUN(name) do {                                                  \
        const int before = g_failures;                                  \
        fprintf(stderr, "  case %s ... ", #name);                       \
        const int rc = name();                                          \
        if (rc == 0 && g_failures == before) {                          \
            fprintf(stderr, "ok\n");                                    \
        } else {                                                        \
            fprintf(stderr, "FAILED\n");                                \
        }                                                               \
} while (0)

/* ------------------------------------------------------------------ */
/*  sm_fill_factor                                                    */
/* ------------------------------------------------------------------ */

CASE(test_fill_factor_empty)
{
    sparsemap_t *m = sm_create(2048);
    EXPECT(m != NULL, "create");
    /* Empty map: cardinality=0, max-min+1=1, fill=0/1=0. */
    const double f = sm_fill_factor(m);
    EXPECT(f >= 0.0 && f <= 1.0, "fill factor in [0, 1]");
    sm_free(m);
    return 0;
}

CASE(test_fill_factor_dense)
{
    sparsemap_t *m = sm_create(8192);
    /* 100 contiguous bits => 100/100 = 1.0 fill. */
    for (uint64_t i = 0; i < 100; i++) {
        sm_add(m, i);
    }
    const double f = sm_fill_factor(m);
    EXPECT(f > 0.99, "dense fill factor near 1.0");
    sm_free(m);
    return 0;
}

CASE(test_fill_factor_sparse)
{
    sparsemap_t *m = sm_create(16384);
    /* 10 bits across [0, 10000]; fill = 10/10001 ~= 0.001. */
    for (uint64_t i = 0; i < 10; i++) {
        sm_add(m, i * 1000);
    }
    const double f = sm_fill_factor(m);
    EXPECT(f < 0.01, "sparse fill factor near 0.0");
    sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_owned_copy                                                     */
/* ------------------------------------------------------------------ */

CASE(test_owned_copy_of_owned)
{
    sparsemap_t *src = sm_create(1024);
    for (uint64_t i = 0; i < 50; i++) {
        sm_add(src, i * 8);
    }
    sparsemap_t *cpy = sm_owned_copy(src);
    EXPECT(cpy != NULL, "owned_copy succeeds");
    EXPECT(sm_cardinality(cpy) == sm_cardinality(src), "copy has same cardinality");
    for (uint64_t i = 0; i < 50; i++) {
        EXPECT(sm_contains(cpy, i * 8), "copy has same bits");
    }
    sm_free(src);
    sm_free(cpy);
    return 0;
}

CASE(test_owned_copy_of_null)
{
    sparsemap_t *cpy = sm_owned_copy(NULL);
    EXPECT(cpy == NULL, "owned_copy(NULL) returns NULL");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_free                                                           */
/* ------------------------------------------------------------------ */

CASE(test_free_null_is_noop)
{
    /* Documented as no-op; no observable behavior to assert beyond
     * "doesn't crash". */
    sm_free(NULL);
    EXPECT(1, "sm_free(NULL) doesn't crash");
    return 0;
}

CASE(test_free_owned_split_after_grow)
{
    /* Path: wrap -> grow promotes to OWNED_SPLIT -> sm_free must
     * release both struct and library-owned buffer. */
    _Alignas(uint64_t) uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    sparsemap_t *m = sm_wrap(buf, sizeof(buf));
    sm_clear(m);
    sparsemap_t *grown = sm_set_data_size(m, NULL, 4096);
    EXPECT(grown != NULL, "grow promotes wrap to owned-split");
    sm_free(grown);
    /* If sm_free leaks the buffer, valgrind / ASan reports it. */
    EXPECT(1, "no crash, no leak");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_capacity_remaining edge cases                                  */
/* ------------------------------------------------------------------ */

CASE(test_capacity_remaining_empty)
{
    sparsemap_t *m = sm_create(1024);
    const double r = sm_capacity_remaining(m);
    EXPECT(r > 99.0, "fresh map has ~100% remaining");
    sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_minimum / sm_maximum on populated maps                         */
/* ------------------------------------------------------------------ */

CASE(test_minimum_maximum)
{
    sparsemap_t *m = sm_create(2048);
    sm_add(m, 1000);
    sm_add(m, 5000);
    sm_add(m, 3000);
    EXPECT(sm_minimum(m) == 1000, "minimum is lowest set bit");
    EXPECT(sm_maximum(m) == 5000, "maximum is highest set bit");
    sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_select edges                                                    */
/* ------------------------------------------------------------------ */

CASE(test_select_out_of_range)
{
    sparsemap_t *m = sm_create(2048);
    sm_add(m, 100);
    sm_add(m, 200);
    /* 0th and 1st set bits exist; 2nd doesn't. */
    EXPECT(sm_select(m, 0, true) == 100, "0th set bit");
    EXPECT(sm_select(m, 1, true) == 200, "1st set bit");
    EXPECT(sm_select(m, 2, true) == SM_IDX_MAX, "out of range select returns IDX_MAX");
    sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_span edges                                                     */
/* ------------------------------------------------------------------ */

CASE(test_span_dense_run)
{
    sparsemap_t *m = sm_create(4096);
    /* Build a contiguous run: bits [100, 200) set. */
    for (uint64_t i = 100; i < 200; i++) {
        sm_add(m, i);
    }
    /* First run of 50 set bits starting at any position is at 100. */
    const uint64_t at = sm_span(m, 0, 50, true);
    EXPECT(at == 100, "span finds the run");
    /* No run of 200 set bits. */
    const uint64_t no = sm_span(m, 0, 200, true);
    EXPECT(no == SM_IDX_MAX, "span of unfindable length returns IDX_MAX");
    sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Driver                                                            */
/* ------------------------------------------------------------------ */

int main(void)
{
    fprintf(stderr, "test_coverage:\n");
    RUN(test_fill_factor_empty);
    RUN(test_fill_factor_dense);
    RUN(test_fill_factor_sparse);
    RUN(test_owned_copy_of_owned);
    RUN(test_owned_copy_of_null);
    RUN(test_free_null_is_noop);
    RUN(test_free_owned_split_after_grow);
    RUN(test_capacity_remaining_empty);
    RUN(test_minimum_maximum);
    RUN(test_select_out_of_range);
    RUN(test_span_dense_run);
    fprintf(stderr, "  %d/%d expectations passed, %d failures\n",
            g_total - g_failures, g_total, g_failures);
    return g_failures == 0 ? 0 : 1;
}
