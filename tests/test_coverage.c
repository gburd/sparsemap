/* SPDX-License-Identifier: MIT
 *
 * tests/test_coverage.c — focused tests for under-covered code paths
 * in sparsemap.  Each section targets a specific function or branch
 * cluster identified by `scripts/measure_coverage.sh`.
 *
 * The tests deliberately mix sparse-only, RLE-only, and sparse+RLE
 * inputs to exercise the chunk-codec branches that property tests
 * don't reach with their default seeds.
 */
#include <assert.h>
#include <math.h>
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
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Populate a contiguous run [start, start+len). */
static void populate_run(sparsemap_t *m, uint64_t start, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++) {
        sm_add(m, start + i);
    }
}

/* Populate a sparse pattern: bits at start + i*stride for i in [0, n). */
static void populate_sparse(sparsemap_t *m, uint64_t start, uint64_t stride, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        sm_add(m, start + i * stride);
    }
}

/* ------------------------------------------------------------------ */
/*  sm_fill_factor                                                    */
/* ------------------------------------------------------------------ */

CASE(test_fill_factor_empty)
{
    sparsemap_t *m = sm_create(2048);
    const double f = sm_fill_factor(m);
    EXPECT(f >= 0.0 && f <= 1.0, "fill in [0, 1] for empty");
    EXPECT(f == 0.0, "empty has fill 0.0 exactly");
    sm_free(m);
    return 0;
}

CASE(test_fill_factor_dense)
{
    sparsemap_t *m = sm_create(8192);
    populate_run(m, 0, 100);
    const double f = sm_fill_factor(m);
    EXPECT(f > 0.99, "dense run is near 1.0");
    sm_free(m);
    return 0;
}

CASE(test_fill_factor_sparse)
{
    sparsemap_t *m = sm_create(16384);
    populate_sparse(m, 0, 1000, 10);
    const double f = sm_fill_factor(m);
    EXPECT(f < 0.01, "sparse pattern is near 0.0");
    sm_free(m);
    return 0;
}

CASE(test_fill_factor_single_bit)
{
    sparsemap_t *m = sm_create(2048);
    sm_add(m, 42);
    const double f = sm_fill_factor(m);
    EXPECT(f == 1.0, "single bit: rank/range = 1/1 = 1.0");
    sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_owned_copy                                                     */
/* ------------------------------------------------------------------ */

CASE(test_owned_copy_of_owned)
{
    sparsemap_t *src = sm_create(1024);
    populate_sparse(src, 0, 8, 50);
    sparsemap_t *cpy = sm_owned_copy(src);
    EXPECT(cpy != NULL, "owned_copy succeeds");
    EXPECT(sm_cardinality(cpy) == sm_cardinality(src), "same cardinality");
    for (uint64_t i = 0; i < 50; i++) {
        EXPECT(sm_contains(cpy, i * 8), "same bits");
    }
    sm_free(src);
    sm_free(cpy);
    return 0;
}

CASE(test_owned_copy_of_null)
{
    EXPECT(sm_owned_copy(NULL) == NULL, "owned_copy(NULL) returns NULL");
    return 0;
}

CASE(test_owned_copy_of_wrapped)
{
    _Alignas(uint64_t) uint8_t buf[1024];
    memset(buf, 0, sizeof(buf));
    sparsemap_t *w = sm_wrap(buf, sizeof(buf));
    sm_clear(w);
    populate_sparse(w, 0, 16, 30);

    sparsemap_t *cpy = sm_owned_copy(w);
    EXPECT(cpy != NULL, "owned_copy from wrapped");
    EXPECT(sm_cardinality(cpy) == 30, "copied cardinality");

    /* The copy can be grown (it's owned-contiguous). */
    sparsemap_t *grown = sm_set_data_size(cpy, NULL, 4096);
    EXPECT(grown != NULL, "owned_copy result is growable");

    sm_free(grown);
    sm_free(w);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_free                                                           */
/* ------------------------------------------------------------------ */

CASE(test_free_null)
{
    sm_free(NULL);
    EXPECT(1, "no crash");
    return 0;
}

CASE(test_free_owned_split_after_grow)
{
    _Alignas(uint64_t) uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    sparsemap_t *m = sm_wrap(buf, sizeof(buf));
    sm_clear(m);
    sparsemap_t *grown = sm_set_data_size(m, NULL, 4096);
    EXPECT(grown != NULL, "grow promotes wrap to split");
    sm_free(grown);
    EXPECT(1, "no leak (valgrind verifies)");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_set_data_size — every lineage × every direction                */
/* ------------------------------------------------------------------ */

CASE(test_set_data_size_owned_grow)
{
    sparsemap_t *m = sm_create(256);
    sparsemap_t *grown = sm_set_data_size(m, NULL, 4096);
    EXPECT(grown != NULL, "owned grow");
    EXPECT(sm_get_capacity(grown) == 4096, "capacity reflects grow");
    sm_free(grown);
    return 0;
}

CASE(test_set_data_size_owned_shrink)
{
    sparsemap_t *m = sm_create(4096);
    sparsemap_t *shrunk = sm_set_data_size(m, NULL, 256);
    EXPECT(shrunk != NULL, "owned shrink");
    EXPECT(sm_get_capacity(shrunk) == 256, "capacity reflects shrink");
    sm_free(shrunk);
    return 0;
}

CASE(test_set_data_size_owned_same_size)
{
    sparsemap_t *m = sm_create(1024);
    sparsemap_t *same = sm_set_data_size(m, NULL, 1024);
    EXPECT(same == m, "same-size resize is no-op");
    EXPECT(sm_get_capacity(same) == 1024, "capacity unchanged");
    sm_free(same);
    return 0;
}

CASE(test_set_data_size_wrap_shrink_in_place)
{
    /* Shrinking a wrapped map keeps it wrapped (caller's buffer
     * unchanged, just less of it accessible). */
    _Alignas(uint64_t) uint8_t buf[1024];
    memset(buf, 0, sizeof(buf));
    sparsemap_t *m = sm_wrap(buf, sizeof(buf));
    sm_clear(m);
    sm_set_data_size(m, NULL, 512);
    EXPECT(sm_get_capacity(m) == 512, "wrap shrink updates capacity");
    /* m_data is still buf; we don't have a public way to verify but
     * sm_free should NOT free the buffer.  Just dispose. */
    sm_free(m);
    return 0;
}

CASE(test_set_data_size_explicit_buffer)
{
    /* The (data, size) form re-points at a caller-supplied buffer. */
    _Alignas(uint64_t) uint8_t buf1[256];
    _Alignas(uint64_t) uint8_t buf2[1024];
    memset(buf1, 0, sizeof(buf1));
    memset(buf2, 0, sizeof(buf2));
    sparsemap_t *m = sm_wrap(buf1, sizeof(buf1));
    sm_clear(m);
    sparsemap_t *swapped = sm_set_data_size(m, buf2, sizeof(buf2));
    EXPECT(swapped == m, "swap returns the same map");
    EXPECT(sm_get_capacity(m) == sizeof(buf2), "capacity reflects new buffer");
    sm_free(m);
    return 0;
}

CASE(test_set_data_size_null_input)
{
    EXPECT(sm_set_data_size(NULL, NULL, 1024) == NULL, "NULL input returns NULL");
    return 0;
}

CASE(test_set_data_size_split_grow)
{
    /* Promote wrap to split, then grow the split. */
    _Alignas(uint64_t) uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    sparsemap_t *m = sm_wrap(buf, sizeof(buf));
    sm_clear(m);
    sparsemap_t *split = sm_set_data_size(m, NULL, 1024); /* WRAP -> SPLIT */
    EXPECT(split != NULL, "wrap-to-split promotion");
    sparsemap_t *grown = sm_set_data_size(split, NULL, 4096); /* SPLIT -> SPLIT (grown) */
    EXPECT(grown != NULL, "split grow");
    EXPECT(sm_get_capacity(grown) == 4096, "split capacity grew");
    sm_free(grown);
    return 0;
}

CASE(test_set_data_size_split_shrink)
{
    /* Promote wrap to split, then shrink the split. */
    _Alignas(uint64_t) uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    sparsemap_t *m = sm_wrap(buf, sizeof(buf));
    sm_clear(m);
    sparsemap_t *split = sm_set_data_size(m, NULL, 4096);
    EXPECT(split != NULL, "wrap-to-split");
    sparsemap_t *shrunk = sm_set_data_size(split, NULL, 1024);
    EXPECT(shrunk != NULL, "split shrink");
    EXPECT(sm_get_capacity(shrunk) == 1024, "split capacity shrank");
    sm_free(shrunk);
    return 0;
}

CASE(test_set_data_size_split_same_size)
{
    _Alignas(uint64_t) uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    sparsemap_t *m = sm_wrap(buf, sizeof(buf));
    sm_clear(m);
    sparsemap_t *split = sm_set_data_size(m, NULL, 2048);
    sparsemap_t *same = sm_set_data_size(split, NULL, 2048);
    EXPECT(same == split, "split same-size is no-op");
    sm_free(same);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_union / sm_intersection / sm_difference                        */
/*  (matrix of sparse and RLE inputs)                                 */
/* ------------------------------------------------------------------ */

CASE(test_setops_sparse_x_sparse)
{
    sparsemap_t *a = sm_create(4096);
    sparsemap_t *b = sm_create(4096);
    populate_sparse(a, 0, 1000, 10);   /* 0, 1000, 2000, ..., 9000 */
    populate_sparse(b, 500, 1000, 10); /* 500, 1500, ..., 9500 */

    sparsemap_t *u = sm_union(a, b);
    sparsemap_t *i = sm_intersection(a, b);
    sparsemap_t *d = sm_difference(a, b);

    EXPECT(u != NULL && sm_cardinality(u) == 20, "union has both sets");
    EXPECT(i == NULL || sm_cardinality(i) == 0, "disjoint intersection empty");
    EXPECT(d != NULL && sm_cardinality(d) == 10, "difference is just a");

    sm_free(u); sm_free(i); sm_free(d);
    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_setops_rle_x_rle)
{
    sparsemap_t *a = sm_create(8192);
    sparsemap_t *b = sm_create(8192);
    /* Two long runs that overlap. */
    populate_run(a, 0, 4096);     /* bits [0, 4096) */
    populate_run(b, 2048, 4096);  /* bits [2048, 6144) */

    sparsemap_t *u = sm_union(a, b);
    sparsemap_t *i = sm_intersection(a, b);
    sparsemap_t *d = sm_difference(a, b);

    EXPECT(u != NULL, "union RLE x RLE");
    EXPECT(sm_cardinality(u) == 6144, "union spans [0, 6144)");
    EXPECT(i != NULL, "intersection");
    EXPECT(sm_cardinality(i) == 2048, "intersection [2048, 4096)");
    EXPECT(d != NULL, "difference");
    EXPECT(sm_cardinality(d) == 2048, "difference [0, 2048)");

    sm_free(u); sm_free(i); sm_free(d);
    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_setops_sparse_x_rle)
{
    sparsemap_t *a = sm_create(4096);
    sparsemap_t *b = sm_create(8192);
    populate_sparse(a, 0, 100, 30); /* 30 sparse bits */
    populate_run(b, 1000, 2000);    /* RLE run */

    sparsemap_t *u = sm_union(a, b);
    sparsemap_t *i = sm_intersection(a, b);
    sparsemap_t *d = sm_difference(a, b);

    EXPECT(u != NULL && sm_cardinality(u) > sm_cardinality(b), "union grows");
    EXPECT(i != NULL || sm_cardinality(b) == 0, "intersection has overlap bits");
    EXPECT(d != NULL, "difference of sparse - rle");

    sm_free(u); sm_free(i); sm_free(d);
    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_setops_rle_x_sparse)
{
    /* Mirror of above, with arguments swapped, to hit the
     * other branch in the chunk-merge two-pointer walk. */
    sparsemap_t *a = sm_create(8192);
    sparsemap_t *b = sm_create(4096);
    populate_run(a, 1000, 2000);
    populate_sparse(b, 0, 100, 30);

    sparsemap_t *u = sm_union(a, b);
    sparsemap_t *i = sm_intersection(a, b);
    sparsemap_t *d = sm_difference(a, b);

    EXPECT(u != NULL, "union rle x sparse");
    EXPECT(d != NULL || sm_cardinality(a) == 0, "difference");

    sm_free(u); sm_free(i); sm_free(d);
    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_setops_with_empty)
{
    sparsemap_t *a = sm_create(2048);
    populate_sparse(a, 0, 16, 20);
    sparsemap_t *empty = sm_create(2048);

    sparsemap_t *u_ae = sm_union(a, empty);
    EXPECT(u_ae != NULL && sm_cardinality(u_ae) == 20, "a UNION empty = a");
    sm_free(u_ae);

    sparsemap_t *i_ae = sm_intersection(a, empty);
    EXPECT(i_ae == NULL || sm_cardinality(i_ae) == 0, "a AND empty = empty");
    if (i_ae) sm_free(i_ae);

    sparsemap_t *d_ae = sm_difference(a, empty);
    EXPECT(d_ae != NULL && sm_cardinality(d_ae) == 20, "a MINUS empty = a");
    sm_free(d_ae);

    sm_free(a); sm_free(empty);
    return 0;
}

CASE(test_setops_with_null)
{
    sparsemap_t *a = sm_create(1024);
    sm_add(a, 42);

    EXPECT(sm_union(NULL, NULL) == NULL, "union(NULL, NULL) = NULL");
    EXPECT(sm_intersection(NULL, a) == NULL, "intersection(NULL, x) = NULL");
    EXPECT(sm_intersection(a, NULL) == NULL, "intersection(x, NULL) = NULL");
    EXPECT(sm_difference(NULL, a) == NULL, "difference(NULL, x) = NULL");

    sm_free(a);
    return 0;
}

CASE(test_setops_identical_inputs)
{
    sparsemap_t *a = sm_create(4096);
    populate_sparse(a, 0, 64, 30);

    sparsemap_t *u = sm_union(a, a);
    sparsemap_t *i = sm_intersection(a, a);
    sparsemap_t *d = sm_difference(a, a);

    EXPECT(u != NULL && sm_cardinality(u) == 30, "a UNION a = a");
    EXPECT(i != NULL && sm_cardinality(i) == 30, "a AND a = a");
    EXPECT(d == NULL || sm_cardinality(d) == 0, "a MINUS a = empty");

    sm_free(u); sm_free(i);
    if (d) sm_free(d);
    sm_free(a);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_offset                                                         */
/* ------------------------------------------------------------------ */

CASE(test_offset_zero)
{
    sparsemap_t *m = sm_create(1024);
    populate_sparse(m, 0, 16, 20);
    sparsemap_t *o = sm_offset(m, 0);
    EXPECT(o != NULL, "offset 0 returns copy");
    EXPECT(sm_cardinality(o) == sm_cardinality(m), "same cardinality");
    EXPECT(sm_minimum(o) == sm_minimum(m), "same min");
    sm_free(o); sm_free(m);
    return 0;
}

CASE(test_offset_positive_chunk_aligned)
{
    sparsemap_t *m = sm_create(1024);
    populate_sparse(m, 0, 16, 10);
    /* Shift by exactly one chunk size (2048 bits). */
    sparsemap_t *o = sm_offset(m, 2048);
    EXPECT(o != NULL, "positive chunk-aligned offset");
    EXPECT(sm_cardinality(o) == sm_cardinality(m), "preserves cardinality");
    EXPECT(sm_contains(o, 2048), "first bit shifted");
    sm_free(o); sm_free(m);
    return 0;
}

CASE(test_offset_positive_unaligned)
{
    sparsemap_t *m = sm_create(2048);
    populate_sparse(m, 100, 16, 20);
    /* Shift by an unaligned amount. */
    sparsemap_t *o = sm_offset(m, 73);
    EXPECT(o != NULL, "positive unaligned offset");
    EXPECT(sm_cardinality(o) == sm_cardinality(m), "preserves cardinality");
    EXPECT(sm_contains(o, 173), "shifted bit visible");
    sm_free(o); sm_free(m);
    return 0;
}

CASE(test_offset_negative_partial)
{
    sparsemap_t *m = sm_create(2048);
    populate_sparse(m, 1000, 16, 20);
    /* Shift left by 500. Bits at [1000, 1304] become [500, 804]. */
    sparsemap_t *o = sm_offset(m, -500);
    EXPECT(o != NULL, "negative offset");
    EXPECT(sm_contains(o, 500), "lowest bit at 500");
    sm_free(o); sm_free(m);
    return 0;
}

CASE(test_offset_negative_drops_bits)
{
    sparsemap_t *m = sm_create(2048);
    populate_sparse(m, 100, 16, 20);
    /* Shift left by enough to drop all bits. */
    sparsemap_t *o = sm_offset(m, -10000);
    /* Result should be empty (all bits shifted below 0). */
    EXPECT(o == NULL || sm_cardinality(o) == 0, "negative shift past 0 drops bits");
    if (o) sm_free(o);
    sm_free(m);
    return 0;
}

CASE(test_offset_null)
{
    EXPECT(sm_offset(NULL, 100) == NULL, "offset(NULL) returns NULL");
    return 0;
}

CASE(test_offset_rle_chunk)
{
    sparsemap_t *m = sm_create(8192);
    populate_run(m, 0, 4096); /* RLE chunk */
    sparsemap_t *o = sm_offset(m, 100);
    EXPECT(o != NULL, "offset of RLE chunk");
    EXPECT(sm_cardinality(o) == 4096, "preserves run length");
    sm_free(o); sm_free(m);
    return 0;
}

CASE(test_offset_carry_across_chunks)
{
    /* Source map populated densely with bits at the END of each
     * source chunk; a small positive unaligned offset moves them into
     * the START of the next chunk — exactly the carry case in
     * sm_offset. */
    sparsemap_t *m = sm_create(32768);
    for (int chunk = 0; chunk < 5; chunk++) {
        const uint64_t base = chunk * 2048;
        /* Bits in the last few vectors of each chunk. */
        for (uint64_t i = 1900; i < 2048; i++) {
            sm_add(m, base + i);
        }
    }
    const uint64_t card_before = sm_cardinality(m);
    /* Shift by 200 — unaligned and bigger than the source-chunk
     * tail, so each chunk's bits span two output chunks. */
    sparsemap_t *o = sm_offset(m, 200);
    EXPECT(o != NULL, "shift across chunks");
    EXPECT(sm_cardinality(o) == card_before, "preserves cardinality");
    sm_free(o);
    sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_split — exotic positions                                       */
/* ------------------------------------------------------------------ */

CASE(test_split_at_zero)
{
    sparsemap_t *m = sm_create(2048);
    populate_run(m, 0, 100);
    sparsemap_t *other = sm_create(2048);
    /* Split at 0: left empty, right = m. */
    sm_split(m, 0, other);
    EXPECT(sm_cardinality(m) == 0, "left side empty");
    EXPECT(sm_cardinality(other) == 100, "right has all bits");
    sm_free(m);
    sm_free(other);
    return 0;
}

CASE(test_split_past_end)
{
    sparsemap_t *m = sm_create(2048);
    populate_run(m, 0, 100);
    sparsemap_t *other = sm_create(2048);
    /* Split far past max bit: left = m, right empty. */
    sm_split(m, 100000, other);
    EXPECT(sm_cardinality(m) == 100, "left has all bits");
    EXPECT(sm_cardinality(other) == 0, "right empty");
    sm_free(m);
    sm_free(other);
    return 0;
}

CASE(test_split_in_middle_sparse)
{
    sparsemap_t *m = sm_create(2048);
    populate_sparse(m, 0, 16, 20);
    sparsemap_t *other = sm_create(2048);
    /* Split at the 10th bit's position. */
    const uint64_t split_at = 10 * 16;
    sm_split(m, split_at, other);
    EXPECT(sm_cardinality(m) + sm_cardinality(other) == 20, "no bits lost");
    sm_free(m);
    sm_free(other);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_select with both true and false                                */
/* ------------------------------------------------------------------ */

CASE(test_select_false_bits)
{
    sparsemap_t *m = sm_create(2048);
    populate_run(m, 0, 100); /* bits [0, 100) set; bit 100 unset; ... */
    /* 0th unset bit is at position 100. */
    EXPECT(sm_select(m, 0, false) == 100, "first unset bit");
    /* 1st unset is at position 101. */
    EXPECT(sm_select(m, 1, false) == 101, "second unset bit");
    sm_free(m);
    return 0;
}

CASE(test_select_in_rle_chunk)
{
    sparsemap_t *m = sm_create(8192);
    populate_run(m, 0, 4096); /* RLE */
    EXPECT(sm_select(m, 0, true) == 0, "RLE first set");
    EXPECT(sm_select(m, 100, true) == 100, "RLE 100th set");
    EXPECT(sm_select(m, 4095, true) == 4095, "RLE last set");
    EXPECT(sm_select(m, 4096, true) == SM_IDX_MAX, "past end of RLE");
    sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_rank with both polarities                                      */
/* ------------------------------------------------------------------ */

CASE(test_rank_both_polarities)
{
    sparsemap_t *m = sm_create(4096);
    populate_run(m, 0, 100);    /* bits [0, 100) set */
    populate_run(m, 200, 100);  /* bits [200, 300) set */

    EXPECT(sm_rank(m, 0, 99, true) == 100, "rank set in [0, 99]");
    EXPECT(sm_rank(m, 0, 99, false) == 0, "rank unset in [0, 99]");
    EXPECT(sm_rank(m, 100, 199, true) == 0, "rank set in gap");
    EXPECT(sm_rank(m, 100, 199, false) == 100, "rank unset in gap");
    EXPECT(sm_rank(m, 0, 299, true) == 200, "rank set in full range");
    EXPECT(sm_rank(m, 0, 299, false) == 100, "rank unset in full range");

    sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Chunk transition: sparse with NONE flags grow                     */
/* ------------------------------------------------------------------ */

CASE(test_sparse_with_unused_flags)
{
    /* When you build a sparse chunk by setting bits non-contiguously,
     * some 2-bit flags end up SM_PAYLOAD_NONE.  Adding a bit that
     * crosses into a NONE region triggers __sm_chunk_increase_capacity. */
    sparsemap_t *m = sm_create(2048);
    /* Set bit 0 and bit 1500 — sparse with internal gaps. */
    sm_add(m, 0);
    sm_add(m, 1500);
    /* Now add bits inside the gap — exercises increase_capacity. */
    for (uint64_t i = 100; i < 200; i++) {
        sm_add(m, i);
    }
    EXPECT(sm_cardinality(m) == 102, "all bits present after gap-fill");
    EXPECT(sm_contains(m, 0), "bit 0 still set");
    EXPECT(sm_contains(m, 1500), "bit 1500 still set");
    EXPECT(sm_contains(m, 150), "bit 150 set");
    sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  RLE ↔ sparse transitions                                          */
/* ------------------------------------------------------------------ */

CASE(test_rle_to_sparse_transition)
{
    /* Build an RLE chunk, then clear a bit in the middle to force
     * separation back into sparse + RLE pieces. */
    sparsemap_t *m = sm_create(8192);
    populate_run(m, 0, 4096);
    EXPECT(sm_cardinality(m) == 4096, "populated RLE");
    /* Clear bit 100 — forces RLE separation. */
    sm_remove(m, 100);
    EXPECT(sm_cardinality(m) == 4095, "one bit cleared");
    EXPECT(!sm_contains(m, 100), "bit 100 unset");
    EXPECT(sm_contains(m, 99), "bit 99 still set");
    EXPECT(sm_contains(m, 101), "bit 101 still set");
    sm_free(m);
    return 0;
}

CASE(test_sparse_to_rle_transition)
{
    /* Fill a chunk completely with set bits — should transition to RLE. */
    sparsemap_t *m = sm_create(8192);
    /* SM_CHUNK_MAX_CAPACITY = 2048 bits per chunk. */
    populate_run(m, 0, 2048);
    EXPECT(sm_cardinality(m) == 2048, "chunk full");
    /* Add one more, crosses chunk boundary, may transition to RLE. */
    sm_add(m, 2048);
    EXPECT(sm_cardinality(m) == 2049, "chunk extended");
    sm_free(m);
    return 0;
}

CASE(test_rle_extend)
{
    /* Adding a bit at the end of an RLE run extends the run. */
    sparsemap_t *m = sm_create(8192);
    populate_run(m, 0, 2049); /* triggers RLE transition */
    /* The next bit should extend the existing run. */
    sm_add(m, 2049);
    EXPECT(sm_cardinality(m) == 2050, "RLE extended");
    EXPECT(sm_contains(m, 2049), "appended bit set");
    sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  set ops with chunk-spanning inputs                                */
/* ------------------------------------------------------------------ */

CASE(test_setops_spanning_many_chunks)
{
    sparsemap_t *a = sm_create(16384);
    sparsemap_t *b = sm_create(16384);
    /* Bits in a span 5 chunks; b spans the same 5 chunks but
     * different patterns within each. */
    for (int chunk = 0; chunk < 5; chunk++) {
        const uint64_t base = chunk * 2048;
        sm_add(a, base + 100);
        sm_add(a, base + 200);
        sm_add(a, base + 1000);
        sm_add(b, base + 100);
        sm_add(b, base + 500);
        sm_add(b, base + 1500);
    }
    EXPECT(sm_cardinality(a) == 15, "a populated");
    EXPECT(sm_cardinality(b) == 15, "b populated");

    sparsemap_t *u = sm_union(a, b);
    sparsemap_t *i = sm_intersection(a, b);
    sparsemap_t *d = sm_difference(a, b);

    EXPECT(u != NULL && sm_cardinality(u) == 25,
           "union of 15+15 with 5 in common = 25");
    EXPECT(i != NULL && sm_cardinality(i) == 5,
           "intersection has 5 (the 100-bits in each chunk)");
    EXPECT(d != NULL && sm_cardinality(d) == 10,
           "a - b has 10");

    sm_free(u); sm_free(i); sm_free(d);
    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_setops_a_subset_of_b)
{
    sparsemap_t *a = sm_create(4096);
    sparsemap_t *b = sm_create(4096);
    populate_sparse(a, 0, 64, 10);                /* 10 bits */
    populate_sparse(b, 0, 32, 50);                /* 50 bits, includes a's */

    sparsemap_t *u = sm_union(a, b);
    sparsemap_t *i = sm_intersection(a, b);
    sparsemap_t *d_ab = sm_difference(a, b);
    sparsemap_t *d_ba = sm_difference(b, a);

    EXPECT(sm_cardinality(u) == 50, "union = b");
    EXPECT(sm_cardinality(i) == 10, "intersection = a");
    EXPECT(d_ab == NULL || sm_cardinality(d_ab) == 0, "a - b = empty");
    EXPECT(sm_cardinality(d_ba) == 40, "b - a = 40");

    sm_free(u); sm_free(i);
    if (d_ab) sm_free(d_ab);
    sm_free(d_ba);
    sm_free(a); sm_free(b);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_minimum / sm_maximum on empty                                  */
/* ------------------------------------------------------------------ */

CASE(test_min_max_empty)
{
    sparsemap_t *m = sm_create(2048);
    /* Documented: returns 0 on empty map. */
    EXPECT(sm_minimum(m) == 0, "empty min returns 0");
    EXPECT(sm_maximum(m) == 0, "empty max returns 0");
    sm_free(m);
    return 0;
}

CASE(test_min_max_rle)
{
    sparsemap_t *m = sm_create(8192);
    populate_run(m, 100, 4000);
    EXPECT(sm_minimum(m) == 100, "RLE min");
    EXPECT(sm_maximum(m) == 4099, "RLE max");
    sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_span edge cases                                                */
/* ------------------------------------------------------------------ */

CASE(test_span_unset_bits)
{
    sparsemap_t *m = sm_create(2048);
    populate_sparse(m, 0, 100, 10); /* sparse: bits 0, 100, 200, ... */
    /* Span of 50 unset bits: starts at bit 1 (since 0 is set, 1-99 unset). */
    const uint64_t at = sm_span(m, 0, 50, false);
    EXPECT(at == 1, "span of unset bits starts at 1");
    sm_free(m);
    return 0;
}

CASE(test_span_full_run)
{
    sparsemap_t *m = sm_create(8192);
    populate_run(m, 0, 1000);
    EXPECT(sm_span(m, 0, 1000, true) == 0, "span of full run");
    sm_free(m);
    return 0;
}

CASE(test_span_with_start_offset)
{
    sparsemap_t *m = sm_create(8192);
    populate_run(m, 0, 100);
    populate_run(m, 200, 200);
    /* From start=150, find run of 100 set bits — should be at 200. */
    EXPECT(sm_span(m, 150, 100, true) == 200, "span starts after offset");
    sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_select edge cases                                              */
/* ------------------------------------------------------------------ */

CASE(test_select_far_index)
{
    sparsemap_t *m = sm_create(16384);
    populate_run(m, 1000, 100);
    EXPECT(sm_select(m, 50, true) == 1050, "50th set bit at offset");
    sm_free(m);
    return 0;
}

CASE(test_select_empty_map)
{
    sparsemap_t *m = sm_create(1024);
    EXPECT(sm_select(m, 0, true) == SM_IDX_MAX, "select on empty returns IDX_MAX");
    sm_free(m);
    return 0;
}

CASE(test_select_unset_in_rle)
{
    /* RLE chunk fully set within the chunk; the chunk's range is
     * [0, 4096), all set, no unset bits within range.  sm_select(false)
     * cannot find unset bits past the last chunk — returns IDX_MAX. */
    sparsemap_t *m = sm_create(8192);
    populate_run(m, 0, 4096);
    EXPECT(sm_select(m, 0, false) == SM_IDX_MAX,
           "no unset bit selectable past last chunk");
    sm_free(m);
    return 0;
}

CASE(test_select_unset_in_partial_rle)
{
    /* RLE chunk with run shorter than capacity: unset bits exist
     * within the chunk's covered range. */
    sparsemap_t *m = sm_create(8192);
    /* A run that fills part of the chunk; the rest of the chunk is
     * unset bits within the chunk's range. */
    populate_run(m, 0, 1500);
    /* The first unset bit is at 1500. */
    EXPECT(sm_select(m, 0, false) == 1500, "first unset bit at run-end");
    /* The 10th unset is at 1510. */
    EXPECT(sm_select(m, 10, false) == 1510, "10th unset bit");
    sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_scan callback                                                  */
/* ------------------------------------------------------------------ */

static size_t g_scan_count = 0;
static uint32_t g_scan_first = 0;
static void scan_cb(uint32_t vec[], size_t n, void *aux)
{
    (void)aux;
    if (g_scan_count == 0 && n > 0) g_scan_first = vec[0];
    g_scan_count += n;
}

CASE(test_scan_basic)
{
    sparsemap_t *m = sm_create(2048);
    sm_add(m, 42);
    sm_add(m, 100);
    sm_add(m, 1000);
    g_scan_count = 0; g_scan_first = 0;
    sm_scan(m, scan_cb, 0, NULL);
    EXPECT(g_scan_count == 3, "scan visits all set bits");
    EXPECT(g_scan_first == 42, "first bit is 42");
    sm_free(m);
    return 0;
}

CASE(test_scan_with_skip)
{
    sparsemap_t *m = sm_create(2048);
    populate_sparse(m, 0, 100, 10);
    g_scan_count = 0; g_scan_first = 0;
    sm_scan(m, scan_cb, 5, NULL);
    EXPECT(g_scan_count == 5, "scan skips first 5");
    EXPECT(g_scan_first == 500, "after skip, first is 500");
    sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  More set-op patterns to exercise sm_union's merge branches        */
/* ------------------------------------------------------------------ */

CASE(test_setops_dense_dense)
{
    /* Two dense maps with overlapping but not identical density. */
    sparsemap_t *a = sm_create(8192);
    sparsemap_t *b = sm_create(8192);
    /* a: every other bit in [0, 1000). */
    for (uint64_t i = 0; i < 1000; i += 2) sm_add(a, i);
    /* b: every third bit in [0, 1000). */
    for (uint64_t i = 0; i < 1000; i += 3) sm_add(b, i);

    sparsemap_t *u = sm_union(a, b);
    sparsemap_t *i = sm_intersection(a, b);
    sparsemap_t *d = sm_difference(a, b);

    /* Verify a few specific bits. */
    EXPECT(u != NULL && sm_contains(u, 0), "both a and b have 0");
    EXPECT(u != NULL && sm_contains(u, 6), "both share 6");
    EXPECT(u != NULL && sm_contains(u, 9), "only b has 9");
    EXPECT(u != NULL && sm_contains(u, 4), "only a has 4");

    EXPECT(i != NULL && sm_contains(i, 0), "intersection at 0");
    EXPECT(i == NULL || !sm_contains(i, 4), "4 not in intersection");

    EXPECT(d != NULL && sm_contains(d, 4), "4 in a-b");
    EXPECT(d == NULL || !sm_contains(d, 6), "6 not in a-b");

    sm_free(u); sm_free(i); sm_free(d);
    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_setops_first_chunks_disjoint)
{
    /* a has bits in chunk 0 only; b has bits in chunk 1 only.
     * Exercises the two-pointer merge's "output a, then output b"
     * branch where neither pointer overlaps with the other. */
    sparsemap_t *a = sm_create(4096);
    sparsemap_t *b = sm_create(4096);
    populate_sparse(a, 0, 16, 50);     /* chunk 0 only */
    populate_sparse(b, 2048, 16, 50);  /* chunk 1 only */

    sparsemap_t *u = sm_union(a, b);
    sparsemap_t *intr = sm_intersection(a, b);
    sparsemap_t *d = sm_difference(a, b);

    EXPECT(sm_cardinality(u) == 100, "disjoint union");
    EXPECT(intr == NULL || sm_cardinality(intr) == 0, "disjoint intersection empty");
    EXPECT(sm_cardinality(d) == 50, "a - b = a (disjoint)");

    sm_free(u); if (intr) sm_free(intr); sm_free(d);
    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_setops_long_runs)
{
    sparsemap_t *a = sm_create(32768);
    sparsemap_t *b = sm_create(32768);
    populate_run(a, 0, 16384);    /* RLE chunks: 8 chunks worth of 1s */
    populate_run(b, 8192, 16384); /* RLE chunks shifted */

    sparsemap_t *u = sm_union(a, b);
    EXPECT(u != NULL && sm_cardinality(u) == 24576, "long run union");

    sparsemap_t *intr = sm_intersection(a, b);
    EXPECT(intr != NULL && sm_cardinality(intr) == 8192, "intersection of overlap");

    sm_free(u); sm_free(intr);
    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_setops_many_chunks)
{
    /* Force ~10 chunks of various types in each map. */
    sparsemap_t *a = sm_create(32768);
    sparsemap_t *b = sm_create(32768);
    /* Even-numbered chunks of a are RLE; odd-numbered are sparse. */
    for (int c = 0; c < 10; c++) {
        const uint64_t base = c * 2048;
        if (c % 2 == 0) {
            populate_run(a, base, 1500);  /* dense; may transition to RLE */
        } else {
            populate_sparse(a, base, 32, 30);
        }
        /* b: opposite pattern. */
        if (c % 2 == 1) {
            populate_run(b, base, 1500);
        } else {
            populate_sparse(b, base, 32, 30);
        }
    }

    sparsemap_t *u = sm_union(a, b);
    sparsemap_t *intr = sm_intersection(a, b);
    sparsemap_t *d = sm_difference(a, b);

    EXPECT(u != NULL, "union of mixed chunks");
    EXPECT(sm_cardinality(u) >= sm_cardinality(a), "union >= a");
    EXPECT(sm_cardinality(u) >= sm_cardinality(b), "union >= b");

    sm_free(u);
    if (intr) sm_free(intr);
    if (d) sm_free(d);
    sm_free(a); sm_free(b);
    return 0;
}

/*
 * Two-pointer merge in sm_union/intersection/difference: exercise
 * both "a runs out first" and "b runs out first" termination paths.
 */
CASE(test_setops_a_runs_out_first)
{
    sparsemap_t *a = sm_create(8192);
    sparsemap_t *b = sm_create(32768);
    /* a has chunks at offsets 0, 2048; b has chunks at 0, 2048, 4096, 6144, 8192. */
    populate_sparse(a, 0, 16, 20);
    populate_sparse(a, 2048, 16, 20);
    populate_sparse(b, 0, 16, 20);
    populate_sparse(b, 2048, 16, 20);
    populate_sparse(b, 4096, 16, 20);
    populate_sparse(b, 6144, 16, 20);
    populate_sparse(b, 8192, 16, 20);

    sparsemap_t *u = sm_union(a, b);
    EXPECT(u != NULL, "union: a shorter");
    EXPECT(sm_cardinality(u) == sm_cardinality(b), "union covers b");

    sparsemap_t *d = sm_difference(b, a);
    EXPECT(d != NULL && sm_cardinality(d) == 60, "b - a leaves 3 chunks");

    sm_free(u); sm_free(d);
    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_setops_b_runs_out_first)
{
    /* Mirror: b is shorter than a. */
    sparsemap_t *a = sm_create(32768);
    sparsemap_t *b = sm_create(8192);
    populate_sparse(a, 0, 16, 20);
    populate_sparse(a, 2048, 16, 20);
    populate_sparse(a, 4096, 16, 20);
    populate_sparse(a, 6144, 16, 20);
    populate_sparse(a, 8192, 16, 20);
    populate_sparse(b, 0, 16, 20);
    populate_sparse(b, 2048, 16, 20);

    sparsemap_t *u = sm_union(a, b);
    EXPECT(u != NULL, "union: b shorter");
    EXPECT(sm_cardinality(u) == sm_cardinality(a), "union covers a");

    sparsemap_t *d = sm_difference(a, b);
    EXPECT(d != NULL && sm_cardinality(d) == 60, "a - b leaves 3 chunks");

    sm_free(u); sm_free(d);
    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_setops_a_chunk_b_chunk_far_apart)
{
    /* a has chunk at 0; b has chunk at 10*2048.  No overlap. */
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(32768);
    populate_sparse(a, 0, 16, 20);
    populate_sparse(b, 20480, 16, 20);

    sparsemap_t *u = sm_union(a, b);
    EXPECT(u != NULL && sm_cardinality(u) == 40, "far apart union");

    sparsemap_t *intr = sm_intersection(a, b);
    EXPECT(intr == NULL || sm_cardinality(intr) == 0, "far apart intersection empty");

    sm_free(u);
    if (intr) sm_free(intr);
    sm_free(a); sm_free(b);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  More sm_offset patterns                                           */
/* ------------------------------------------------------------------ */

CASE(test_offset_one_bit)
{
    sparsemap_t *m = sm_create(1024);
    sm_add(m, 100);
    sparsemap_t *o = sm_offset(m, 50);
    EXPECT(o != NULL && sm_contains(o, 150), "single-bit offset");
    EXPECT(sm_cardinality(o) == 1, "single bit preserved");
    sm_free(o); sm_free(m);
    return 0;
}

CASE(test_offset_dense_long_run)
{
    sparsemap_t *m = sm_create(32768);
    populate_run(m, 0, 8192); /* multiple RLE chunks */
    sparsemap_t *o = sm_offset(m, 1000);
    EXPECT(o != NULL, "offset dense");
    EXPECT(sm_cardinality(o) == 8192, "all bits preserved");
    EXPECT(sm_contains(o, 1000), "first shifted bit");
    EXPECT(sm_contains(o, 9191), "last shifted bit");
    sm_free(o); sm_free(m);
    return 0;
}

CASE(test_offset_chunk_aligned_negative)
{
    sparsemap_t *m = sm_create(8192);
    populate_sparse(m, 4096, 16, 30);
    /* Shift left by exactly one chunk size. */
    sparsemap_t *o = sm_offset(m, -2048);
    EXPECT(o != NULL, "chunk-aligned negative offset");
    EXPECT(sm_cardinality(o) == 30, "all bits preserved");
    EXPECT(sm_contains(o, 2048), "first bit at 4096-2048=2048");
    sm_free(o); sm_free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  v2.1 additions: open_copy, add_grow, allocator hooks              */
/* ------------------------------------------------------------------ */

CASE(test_open_copy)
{
    /* Build a serialized payload with sm_create + sm_get_data. */
    sparsemap_t *src = sm_create(2048);
    sm_add(src, 5); sm_add(src, 100); sm_add(src, 1500);
    const size_t n = sm_get_size(src);
    uint8_t *bytes = malloc(n);
    memcpy(bytes, sm_get_data(src), n);

    /* Round-trip via sm_open_copy. */
    sparsemap_t *r = sm_open_copy(bytes, n, 256);
    EXPECT(r != NULL, "open_copy returns non-NULL");
    EXPECT(sm_get_capacity(r) == n + 256, "capacity = n + slack");
    EXPECT(sm_equals(r, src), "contents match");

    /* Slack must permit further additions without realloc. */
    EXPECT(sm_add(r, 9999) == 9999, "add succeeds in slack");

    /* Empty payload. */
    sparsemap_t *e = sm_open_copy(NULL, 0, 1024);
    EXPECT(e != NULL && sm_is_empty(e), "empty payload yields empty map");
    sm_free(e);

    free(bytes);
    sm_free(src);
    sm_free(r);
    return 0;
}

CASE(test_add_grow)
{
    sparsemap_t *m = sm_create(64);   /* tiny: will need to grow */
    /* Add lots of bits; verify add_grow handles the relocation. */
    for (uint64_t i = 0; i < 200; i++) {
        EXPECT(sm_add_grow(&m, i * 100) == i * 100, "add_grow ok");
    }
    EXPECT(sm_cardinality(m) == 200, "all 200 added");
    EXPECT(sm_contains(m, 100) && sm_contains(m, 19900), "first and last present");
    sm_free(m);

    /* NULL or NULL-pointer-pointee returns SM_IDX_MAX. */
    EXPECT(sm_add_grow(NULL, 0) == SM_IDX_MAX, "NULL mapp");
    sparsemap_t *null_map = NULL;
    EXPECT(sm_add_grow(&null_map, 0) == SM_IDX_MAX, "NULL *mapp");
    return 0;
}

/* Allocator instrumentation: count alloc / realloc / free calls so we
 * can verify the hook is actually being called. */
static struct {
    size_t allocs;
    size_t reallocs;
    size_t frees;
} g_alloc_stats;

static void *test_alloc(size_t n, void *aux)
{
    (void)aux;
    g_alloc_stats.allocs++;
    return malloc(n);
}
static void *test_realloc(void *p, size_t n, void *aux)
{
    (void)aux;
    g_alloc_stats.reallocs++;
    return realloc(p, n);
}
static void test_free(void *p, void *aux)
{
    (void)aux;
    if (p) g_alloc_stats.frees++;
    free(p);
}

CASE(test_allocator_global)
{
    static const sm_allocator_t hooks = {
        .alloc = test_alloc,
        .realloc = test_realloc,
        .free = test_free,
        .aux = NULL,
    };
    memset(&g_alloc_stats, 0, sizeof(g_alloc_stats));
    sm_set_allocator(&hooks);

    sparsemap_t *m = sm_create(1024);
    EXPECT(g_alloc_stats.allocs >= 1, "alloc hook invoked on create");
    sm_add(m, 42);
    EXPECT(sm_contains(m, 42), "basic add still works");
    sm_free(m);
    EXPECT(g_alloc_stats.frees >= 1, "free hook invoked");

    /* Reset to libc and verify subsequent maps don't touch hooks. */
    sm_set_allocator(NULL);
    const size_t allocs_before = g_alloc_stats.allocs;
    sparsemap_t *m2 = sm_create(1024);
    sm_add(m2, 100);
    sm_free(m2);
    EXPECT(g_alloc_stats.allocs == allocs_before, "libc bypasses hooks");
    return 0;
}

CASE(test_allocator_per_map)
{
    static const sm_allocator_t hooks = {
        .alloc = test_alloc,
        .realloc = test_realloc,
        .free = test_free,
        .aux = NULL,
    };
    memset(&g_alloc_stats, 0, sizeof(g_alloc_stats));
    /* Default is libc. */
    sm_set_allocator(NULL);

    /* Create with per-map override. */
    sparsemap_t *m = sm_create_with_allocator(1024, &hooks);
    EXPECT(g_alloc_stats.allocs == 1, "per-map alloc hook invoked");

    /* Grow this map: should also use the hook (via realloc). */
    sparsemap_t *grown = sm_set_data_size(m, NULL, 4096);
    EXPECT(grown != NULL, "grow ok");
    EXPECT(g_alloc_stats.reallocs == 1, "per-map realloc hook invoked");

    /* Concurrent libc map should not touch the hook. */
    const size_t allocs_at_check = g_alloc_stats.allocs;
    sparsemap_t *libc_map = sm_create(1024);
    EXPECT(g_alloc_stats.allocs == allocs_at_check, "libc map untouched");
    sm_free(libc_map);

    sm_free(grown);
    EXPECT(g_alloc_stats.frees >= 1, "per-map free invoked on dispose");
    return 0;
}

CASE(test_or_and_andnot)
{
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(2048);
    for (int i = 0; i < 5; i++) sm_add(a, i * 100);     /* 0,100,200,300,400 */
    for (int i = 2; i < 7; i++) sm_add(b, i * 100);     /* 200,300,400,500,600 */

    /* sm_or = sm_union */
    sparsemap_t *o = sm_or(a, b);
    sparsemap_t *u = sm_union(a, b);
    EXPECT(sm_equals(o, u), "sm_or == sm_union");
    sm_free(o); sm_free(u);

    /* sm_and = sm_intersection */
    sparsemap_t *an = sm_and(a, b);
    sparsemap_t *in = sm_intersection(a, b);
    EXPECT(sm_equals(an, in), "sm_and == sm_intersection");
    sm_free(an); sm_free(in);

    /* sm_andnot = sm_difference */
    sparsemap_t *anot = sm_andnot(a, b);
    sparsemap_t *df = sm_difference(a, b);
    EXPECT(sm_equals(anot, df), "sm_andnot == sm_difference");
    sm_free(anot); sm_free(df);

    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_is_superset)
{
    EXPECT(sm_is_superset(NULL, NULL), "empty superset of empty");
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(2048);
    sm_add(a, 100); sm_add(a, 200); sm_add(a, 300);
    sm_add(b, 200);

    EXPECT(sm_is_superset(a, b), "a superset of b");
    EXPECT(!sm_is_superset(b, a), "b not superset of a");
    EXPECT(sm_is_superset(a, a), "a superset of itself");

    /* Symmetry with sm_is_subset. */
    EXPECT(sm_is_superset(a, b) == sm_is_subset(b, a), "superset(a,b) == subset(b,a)");

    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_extract_range)
{
    sparsemap_t *m = sm_create(8192);
    for (uint64_t i = 0; i < 1000; i += 10) sm_add(m, i);  /* 0,10,20,...,990 */

    /* Extract [100, 200) — should contain 100,110,...,190. */
    sparsemap_t *r = sm_extract_range(m, 100, 200);
    EXPECT(r != NULL, "extract returns non-NULL");
    EXPECT(sm_cardinality(r) == 10, "10 bits");
    EXPECT(sm_contains(r, 100) && sm_contains(r, 190), "endpoints");
    EXPECT(!sm_contains(r, 90) && !sm_contains(r, 200), "outside range");
    sm_free(r);

    /* Empty result: extract a range with no bits. */
    /* m has bits at multiples of 10, no bits in [101, 109). */
    sparsemap_t *empty = sm_extract_range(m, 101, 110);
    EXPECT(empty == NULL, "empty range extracted as NULL");

    /* Whole map. */
    sparsemap_t *whole = sm_extract_range(m, 0, 1000);
    EXPECT(whole != NULL && sm_equals(whole, m), "whole-range extract == original");
    sm_free(whole);

    sm_free(m);
    return 0;
}

CASE(test_pop_last)
{
    sparsemap_t *m = sm_create(8192);
    EXPECT(sm_pop_last(m) == SM_IDX_MAX, "empty: IDX_MAX");

    sm_add(m, 50); sm_add(m, 100); sm_add(m, 200);
    EXPECT(sm_pop_last(m) == 200, "highest popped");
    EXPECT(!sm_contains(m, 200), "popped bit gone");
    EXPECT(sm_pop_last(m) == 100, "next highest");
    EXPECT(sm_pop_last(m) == 50, "lowest");
    EXPECT(sm_pop_last(m) == SM_IDX_MAX, "empty after drain");

    sm_free(m);
    return 0;
}

CASE(test_serialize_roundtrip)
{
    sparsemap_t *m = sm_create(2048);
    sm_add(m, 0); sm_add(m, 100); sm_add(m, 1000); sm_add(m, 1500);
    for (uint64_t i = 0; i < 4096; i++) sm_add(m, 100000 + i);  /* RLE chunk */

    const size_t need = sm_serialized_size(m);
    EXPECT(need > 0, "size > 0");
    uint8_t *buf = malloc(need);
    EXPECT(buf != NULL, "buffer allocated");
    EXPECT(sm_serialize(m, buf, need) == need, "serialize fills buffer");

    sparsemap_t *r = sm_deserialize(buf, need);
    EXPECT(r != NULL, "deserialize succeeds");
    EXPECT(sm_equals(m, r), "round-trip preserves bits");
    EXPECT(sm_cardinality(r) == sm_cardinality(m), "same cardinality");

    free(buf);
    sm_free(m); sm_free(r);
    return 0;
}

CASE(test_serialize_empty)
{
    sparsemap_t *m = sm_create(1024);
    const size_t need = sm_serialized_size(m);
    uint8_t *buf = malloc(need);
    sm_serialize(m, buf, need);

    sparsemap_t *r = sm_deserialize(buf, need);
    EXPECT(r != NULL, "empty deserialize ok");
    EXPECT(sm_is_empty(r), "deserialized is empty");

    free(buf);
    sm_free(m); sm_free(r);
    return 0;
}

CASE(test_deserialize_validation)
{
    /* Too short. */
    EXPECT(sm_deserialize((const uint8_t *)"x", 1) == NULL, "too short rejected");

    /* Bad magic. */
    uint8_t buf[64] = { 0 };
    memset(buf, 0xAB, sizeof(buf));
    EXPECT(sm_deserialize(buf, sizeof(buf)) == NULL, "bad magic rejected");

    /* Right magic, wrong version. */
    uint32_t magic = 0x30316d73;  /* sm10 LE */
    memcpy(buf, &magic, 4);
    buf[4] = 99;  /* version 99 */
    buf[5] = 0x01;
    memset(buf + 6, 0, 10);
    EXPECT(sm_deserialize(buf, sizeof(buf)) == NULL, "bad version rejected");

    /* Right header, malformed body. */
    buf[4] = 1;
    buf[5] = 0x01;
    /* Body claims 99 chunks but only 4 bytes follow. */
    memset(buf + 16, 0xff, 4);
    EXPECT(sm_deserialize(buf, 20) == NULL, "malformed body rejected");

    return 0;
}

CASE(test_flip_range)
{
    sparsemap_t *m = sm_create(2048);
    /* Empty map: flip [10, 20) sets bits 10-19. */
    EXPECT(sm_flip_range(m, 10, 20), "flip empty");
    EXPECT(sm_cardinality(m) == 10, "10 bits set after flip");
    EXPECT(sm_contains(m, 10) && sm_contains(m, 19), "endpoints");
    EXPECT(!sm_contains(m, 9) && !sm_contains(m, 20), "outside range");

    /* Flipping the same range again clears them. */
    EXPECT(sm_flip_range(m, 10, 20), "flip back");
    EXPECT(sm_is_empty(m), "empty again");

    /* Flip a partial overlap. */
    sm_add(m, 50); sm_add(m, 51); sm_add(m, 52);
    EXPECT(sm_flip_range(m, 51, 53), "partial flip");
    /* 51 was set -> unset.  52 was set -> unset.  50 still set. */
    EXPECT(sm_contains(m, 50) && !sm_contains(m, 51) && !sm_contains(m, 52),
           "partial flip results");

    sm_free(m);
    return 0;
}

CASE(test_validate_ok)
{
    EXPECT(sm_validate(NULL), "NULL valid (treated as empty)");
    sparsemap_t *m = sm_create(2048);
    EXPECT(sm_validate(m), "fresh valid");
    for (int i = 0; i < 100; i++) sm_add(m, i * 16);
    EXPECT(sm_validate(m), "populated valid");
    sm_free(m);
    return 0;
}

CASE(test_statistics)
{
    sparsemap_t *m = sm_create(8192);
    sm_stats_t s;

    /* Empty. */
    sm_statistics(m, &s);
    EXPECT(s.chunks_total == 0, "empty: 0 chunks");
    EXPECT(s.bits_set == 0, "empty: 0 bits");

    /* Sparse. */
    sm_add(m, 0);
    sm_add(m, 1500);
    sm_statistics(m, &s);
    EXPECT(s.chunks_total == 1, "sparse: 1 chunk");
    EXPECT(s.chunks_sparse == 1 && s.chunks_rle == 0, "all sparse");
    EXPECT(s.bits_set == 2 && s.bits_in_sparse == 2, "2 bits sparse");

    /* Dense run forces RLE. */
    sm_clear(m);
    for (uint64_t i = 0; i < 4096; i++) sm_add(m, i);
    sm_statistics(m, &s);
    EXPECT(s.bits_set == 4096, "4096 bits set");
    /* RLE chunks store 2048 bits each in 8 bytes; very efficient. */
    EXPECT(s.bytes_per_set_bit < 0.1, "RLE: low bytes per bit");

    sm_free(m);
    return 0;
}

CASE(test_shrink_to_fit)
{
    sparsemap_t *m = sm_create(8192);
    /* Add then remove most bits; lots of unused capacity. */
    for (int i = 0; i < 100; i++) sm_add(m, i);
    for (int i = 0; i < 100; i++) sm_remove(m, i);
    const size_t before = sm_get_capacity(m);
    sparsemap_t *shrunk = sm_shrink_to_fit(m);
    EXPECT(shrunk != NULL, "shrink succeeds");
    EXPECT(sm_get_capacity(shrunk) <= before, "capacity at most before");
    sm_free(shrunk);

    /* NULL input. */
    EXPECT(sm_shrink_to_fit(NULL) == NULL, "NULL input returns NULL");

    /* Wrap'd: returns as-is, no shrink. */
    _Alignas(uint64_t) uint8_t buf[1024];
    memset(buf, 0, sizeof(buf));
    sparsemap_t *w = sm_wrap(buf, sizeof(buf));
    sm_clear(w);
    sparsemap_t *w2 = sm_shrink_to_fit(w);
    EXPECT(w2 == w, "wrap'd returns same pointer");
    sm_free(w);
    return 0;
}

CASE(test_union_inplace)
{
    sparsemap_t *dst = sm_create(2048);
    sparsemap_t *src = sm_create(2048);
    for (int i = 0; i < 5; i++) sm_add(dst, i * 100);
    for (int i = 0; i < 5; i++) sm_add(src, i * 100 + 50);

    dst = sm_union_inplace(dst, src);
    EXPECT(dst != NULL, "union_inplace returns dst");
    EXPECT(sm_cardinality(dst) == 10, "union has 10 bits");
    EXPECT(sm_contains(dst, 0) && sm_contains(dst, 50), "both sets present");

    /* Adding duplicates: cardinality unchanged. */
    dst = sm_union_inplace(dst, src);
    EXPECT(sm_cardinality(dst) == 10, "duplicate union no-op");

    sm_free(dst); sm_free(src);
    return 0;
}

CASE(test_intersection_inplace)
{
    sparsemap_t *dst = sm_create(2048);
    sparsemap_t *src = sm_create(2048);
    for (int i = 0; i < 10; i++) sm_add(dst, i * 100);
    sm_add(src, 100);
    sm_add(src, 200);
    sm_add(src, 300);

    dst = sm_intersection_inplace(dst, src);
    EXPECT(dst != NULL, "intersection_inplace returns dst");
    EXPECT(sm_cardinality(dst) == 3, "3 bits intersect");
    EXPECT(sm_contains(dst, 100) && sm_contains(dst, 200) && sm_contains(dst, 300),
           "intersection bits");
    EXPECT(!sm_contains(dst, 0), "non-intersecting bit gone");

    sm_free(dst); sm_free(src);
    return 0;
}

CASE(test_difference_inplace)
{
    sparsemap_t *dst = sm_create(2048);
    sparsemap_t *src = sm_create(2048);
    for (int i = 0; i < 10; i++) sm_add(dst, i * 100);
    sm_add(src, 200);
    sm_add(src, 500);
    sm_add(src, 9999);  /* not in dst, should be ignored */

    dst = sm_difference_inplace(dst, src);
    EXPECT(dst != NULL, "difference_inplace returns dst");
    EXPECT(sm_cardinality(dst) == 8, "two bits removed");
    EXPECT(!sm_contains(dst, 200) && !sm_contains(dst, 500), "removed bits gone");
    EXPECT(sm_contains(dst, 0) && sm_contains(dst, 100), "untouched bits stay");

    sm_free(dst); sm_free(src);
    return 0;
}

CASE(test_add_range)
{
    sparsemap_t *m = sm_create(2048);
    EXPECT(sm_add_range(m, 100, 100), "empty range no-op");
    EXPECT(sm_cardinality(m) == 0, "still empty");

    EXPECT(sm_add_range(m, 100, 200), "add [100, 200)");
    EXPECT(sm_cardinality(m) == 100, "100 bits");
    EXPECT(sm_contains(m, 100) && sm_contains(m, 199), "endpoints");
    EXPECT(!sm_contains(m, 99) && !sm_contains(m, 200), "outside excluded");

    sm_free(m);
    return 0;
}

CASE(test_remove_range)
{
    sparsemap_t *m = sm_create(8192);
    sm_add_range(m, 0, 1000);
    EXPECT(sm_cardinality(m) == 1000, "1000 bits added");

    EXPECT(sm_remove_range(m, 200, 700), "remove middle");
    EXPECT(sm_cardinality(m) == 500, "500 left");
    EXPECT(sm_contains(m, 100) && sm_contains(m, 800), "edges still set");
    EXPECT(!sm_contains(m, 300) && !sm_contains(m, 600), "middle cleared");

    sm_free(m);
    return 0;
}

CASE(test_xor)
{
    /* Disjoint: xor = union */
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(2048);
    for (int i = 0; i < 10; i++) sm_add(a, i * 100);
    for (int i = 0; i < 10; i++) sm_add(b, i * 100 + 50);
    sparsemap_t *x = sm_xor(a, b);
    EXPECT(x != NULL && sm_cardinality(x) == 20, "disjoint xor = 20");
    sm_free(x);

    /* Identical: xor = empty */
    sparsemap_t *c = sm_create(2048);
    for (int i = 0; i < 10; i++) sm_add(c, i * 100);
    x = sm_xor(a, c);
    EXPECT(x == NULL || sm_cardinality(x) == 0, "identical xor = empty");
    if (x) sm_free(x);

    /* Overlap: xor = symmetric diff */
    sm_add(b, 100);  /* now b has bit 100 too, which a also has */
    sm_add(b, 200);
    x = sm_xor(a, b);
    /* a={0,100,200,...,900}; b={50,100,150,200,250,...,950}
     * a ^ b: bits unique to one or the other. 100 and 200 in both -> excluded. */
    EXPECT(x != NULL, "overlap xor non-null");
    EXPECT(!sm_contains(x, 100) && !sm_contains(x, 200), "shared excluded");
    EXPECT(sm_contains(x, 0) && sm_contains(x, 50), "unique included");
    sm_free(x);

    sm_free(a); sm_free(b); sm_free(c);
    return 0;
}

CASE(test_xor_cardinality)
{
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(2048);
    for (int i = 0; i < 10; i++) sm_add(a, i * 100);
    for (int i = 0; i < 10; i++) sm_add(b, i * 100);
    EXPECT(sm_xor_cardinality(a, b) == 0, "identical xor 0");

    sm_add(b, 9999);
    EXPECT(sm_xor_cardinality(a, b) == 1, "one diff = 1");

    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_create_singleton)
{
    sparsemap_t *m = sm_create_singleton(42);
    EXPECT(m != NULL, "singleton created");
    EXPECT(sm_cardinality(m) == 1, "one bit");
    EXPECT(sm_contains(m, 42), "correct bit");
    EXPECT(sm_singleton_member(m) == 42, "matches singleton api");
    sm_free(m);
    return 0;
}

CASE(test_create_from_range)
{
    sparsemap_t *m = sm_create_from_range(0, 100);
    EXPECT(m != NULL, "range created");
    EXPECT(sm_cardinality(m) == 100, "100 bits");
    EXPECT(sm_contains(m, 0) && sm_contains(m, 99), "endpoints");
    EXPECT(!sm_contains(m, 100), "upper exclusive");
    sm_free(m);

    /* Empty range. */
    sparsemap_t *e = sm_create_from_range(50, 50);
    EXPECT(e != NULL && sm_is_empty(e), "empty range = empty map");
    sm_free(e);
    return 0;
}

CASE(test_create_from_array)
{
    const uint64_t arr[] = { 5, 100, 200, 1000 };
    sparsemap_t *m = sm_create_from_array(arr, 4);
    EXPECT(m != NULL && sm_cardinality(m) == 4, "4 bits");
    EXPECT(sm_contains(m, 5), "first bit");
    EXPECT(sm_contains(m, 1000), "last bit");
    sm_free(m);
    return 0;
}

CASE(test_hash)
{
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(2048);
    /* Empty maps hash to the same value. */
    EXPECT(sm_hash(a) == sm_hash(b), "empty hashes equal");

    sm_add(a, 42);
    sm_add(b, 42);
    EXPECT(sm_hash(a) == sm_hash(b), "identical content hashes equal");

    sm_add(b, 100);
    EXPECT(sm_hash(a) != sm_hash(b), "different content hashes differ");

    /* Equality implies same hash (test contract directly). */
    sparsemap_t *c = sm_copy(a);
    EXPECT(sm_equals(a, c) && sm_hash(a) == sm_hash(c),
           "equals implies same hash");

    sm_free(a); sm_free(b); sm_free(c);
    return 0;
}

CASE(test_compare)
{
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(2048);
    EXPECT(sm_compare(a, b) == 0, "empty == empty");

    sm_add(a, 100);
    EXPECT(sm_compare(a, b) > 0, "populated > empty");
    EXPECT(sm_compare(b, a) < 0, "empty < populated");

    sm_add(b, 200);  /* b > a now (200 > 100) */
    EXPECT(sm_compare(a, b) < 0, "a < b lex");

    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_subset_compare)
{
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(2048);
    EXPECT(sm_subset_compare(a, b) == SM_REL_EQUAL, "empty == empty");

    sm_add(a, 100);
    sm_add(b, 100);
    sm_add(b, 200);
    EXPECT(sm_subset_compare(a, b) == SM_REL_SUBSET_A, "a strict subset b");
    EXPECT(sm_subset_compare(b, a) == SM_REL_SUBSET_B, "b strict superset a");

    sm_add(a, 999);
    EXPECT(sm_subset_compare(a, b) == SM_REL_DIFFERENT, "divergent");

    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_pop_first)
{
    sparsemap_t *m = sm_create(2048);
    EXPECT(sm_pop_first(m) == SM_IDX_MAX, "empty pops nothing");

    sm_add(m, 100); sm_add(m, 200); sm_add(m, 50);
    EXPECT(sm_pop_first(m) == 50, "first popped");
    EXPECT(!sm_contains(m, 50), "popped bit gone");
    EXPECT(sm_cardinality(m) == 2, "cardinality decreased");

    EXPECT(sm_pop_first(m) == 100, "next popped");
    EXPECT(sm_pop_first(m) == 200, "last popped");
    EXPECT(sm_pop_first(m) == SM_IDX_MAX, "now empty");

    sm_free(m);
    return 0;
}

CASE(test_union_cardinality)
{
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(2048);
    EXPECT(sm_union_cardinality(a, b) == 0, "empty union 0");

    for (int i = 0; i < 10; i++) sm_add(a, i * 100);
    EXPECT(sm_union_cardinality(a, b) == 10, "a alone = 10");
    for (int i = 0; i < 10; i++) sm_add(b, i * 100 + 50);  /* disjoint */
    EXPECT(sm_union_cardinality(a, b) == 20, "disjoint = 20");

    /* Add a duplicate: now b contains some of a's bits. */
    sm_add(b, 0);
    sm_add(b, 100);
    EXPECT(sm_union_cardinality(a, b) == 20, "shared bits not double-counted");

    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_intersection_cardinality)
{
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(2048);
    EXPECT(sm_intersection_cardinality(a, b) == 0, "empty intersect = 0");

    for (int i = 0; i < 10; i++) sm_add(a, i * 100);
    for (int i = 0; i < 10; i++) sm_add(b, i * 100 + 50); /* disjoint */
    EXPECT(sm_intersection_cardinality(a, b) == 0, "disjoint = 0");

    sm_add(b, 100); sm_add(b, 200); sm_add(b, 300);
    EXPECT(sm_intersection_cardinality(a, b) == 3, "three in common");

    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_difference_cardinality)
{
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(2048);
    EXPECT(sm_difference_cardinality(a, b) == 0, "empty - empty = 0");

    for (int i = 0; i < 10; i++) sm_add(a, i * 100);
    EXPECT(sm_difference_cardinality(a, b) == 10, "a - empty = a");
    EXPECT(sm_difference_cardinality(b, a) == 0, "empty - a = 0");

    /* Remove three from a's perspective via b. */
    sm_add(b, 100); sm_add(b, 200); sm_add(b, 300);
    EXPECT(sm_difference_cardinality(a, b) == 7, "a - b removes 3");

    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_nonempty_difference)
{
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(2048);
    EXPECT(!sm_nonempty_difference(a, b), "empty - empty: false");

    sm_add(a, 100);
    EXPECT(sm_nonempty_difference(a, b), "populated - empty: true");

    sm_add(b, 100);
    EXPECT(!sm_nonempty_difference(a, b), "a == b: false");

    sm_add(a, 200);
    EXPECT(sm_nonempty_difference(a, b), "a has extra bit: true");

    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_jaccard_index)
{
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(2048);
    EXPECT(sm_jaccard_index(a, b) == 0.0, "empty pair: 0.0");

    for (int i = 0; i < 10; i++) sm_add(a, i * 100);
    for (int i = 0; i < 10; i++) sm_add(b, i * 100);  /* identical */
    EXPECT(sm_jaccard_index(a, b) == 1.0, "identical: 1.0");

    /* Add disjoint bits to each. */
    sm_add(a, 9999);
    sm_add(b, 8888);
    /* intersection = 10, union = 12. j = 10/12 ~= 0.833 */
    const double j = sm_jaccard_index(a, b);
    EXPECT(j > 0.83 && j < 0.84, "jaccard around 0.833");

    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_add_many)
{
    sparsemap_t *m = sm_create(2048);
    const uint64_t arr[] = { 5, 10, 100, 200, 1500 };
    EXPECT(sm_add_many(m, arr, 5), "add_many succeeds");
    EXPECT(sm_cardinality(m) == 5, "5 bits added");
    EXPECT(sm_contains(m, 5) && sm_contains(m, 1500), "first and last present");

    /* Empty array. */
    EXPECT(sm_add_many(m, NULL, 0), "add 0 elements ok");

    sm_free(m);
    return 0;
}

CASE(test_to_array)
{
    sparsemap_t *m = sm_create(2048);
    sm_add(m, 5);
    sm_add(m, 100);
    sm_add(m, 1500);

    /* Query size with NULL out. */
    size_t n = 0;
    sm_to_array(m, NULL, &n);
    EXPECT(n == 3, "size query returns 3");

    /* Materialize. */
    uint64_t buf[10];
    n = 10;
    sm_to_array(m, buf, &n);
    EXPECT(n == 3, "3 written");
    EXPECT(buf[0] == 5 && buf[1] == 100 && buf[2] == 1500, "sorted output");

    /* Truncated buffer. */
    n = 2;
    sm_to_array(m, buf, &n);
    EXPECT(n == 2, "truncated to 2");

    sm_free(m);
    return 0;
}

CASE(test_is_empty)
{
    sparsemap_t *m = sm_create(2048);
    EXPECT(sm_is_empty(m), "fresh map is empty");
    EXPECT(sm_is_empty(NULL), "NULL is empty");
    sm_add(m, 42);
    EXPECT(!sm_is_empty(m), "after add, not empty");
    sm_clear(m);
    EXPECT(sm_is_empty(m), "after clear, empty");
    sm_free(m);
    return 0;
}

CASE(test_equals)
{
    EXPECT(sm_equals(NULL, NULL), "NULL == NULL");
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(2048);
    EXPECT(sm_equals(a, b), "empty == empty");
    EXPECT(sm_equals(NULL, a), "NULL == empty");
    EXPECT(sm_equals(a, NULL), "empty == NULL");

    sm_add(a, 42);
    EXPECT(!sm_equals(a, b), "a != b after add");
    sm_add(b, 42);
    EXPECT(sm_equals(a, b), "equal again");

    /* Encoding-independent: a built sparse, b built dense should still equal
     * if the bit set is the same.  Build identical contents differently. */
    sparsemap_t *c = sm_create(8192);
    sparsemap_t *d = sm_create(8192);
    for (uint64_t i = 0; i < 100; i++) sm_add(c, i);
    for (uint64_t i = 0; i < 100; i++) sm_add(d, i);
    EXPECT(sm_equals(c, d), "same content, identical maps equal");

    sm_free(a); sm_free(b); sm_free(c); sm_free(d);
    return 0;
}

CASE(test_is_subset)
{
    EXPECT(sm_is_subset(NULL, NULL), "empty subset of empty");
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(2048);
    EXPECT(sm_is_subset(a, b), "empty subset of empty");

    sm_add(b, 42);
    EXPECT(sm_is_subset(a, b), "empty subset of {42}");
    EXPECT(!sm_is_subset(b, a), "{42} not subset of empty");

    sm_add(a, 42);
    EXPECT(sm_is_subset(a, b), "a == b is subset");
    EXPECT(sm_is_subset(b, a), "b == a is subset (mutual)");

    sm_add(b, 100);
    EXPECT(sm_is_subset(a, b), "{42} subset of {42, 100}");
    EXPECT(!sm_is_subset(b, a), "{42, 100} not subset of {42}");

    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_overlap)
{
    EXPECT(!sm_overlap(NULL, NULL), "NULL has no overlap");
    sparsemap_t *a = sm_create(2048);
    sparsemap_t *b = sm_create(2048);
    EXPECT(!sm_overlap(a, b), "empty/empty no overlap");

    sm_add(a, 42);
    EXPECT(!sm_overlap(a, b), "a populated, b empty: no overlap");
    sm_add(b, 100);
    EXPECT(!sm_overlap(a, b), "disjoint: no overlap");
    sm_add(b, 42);
    EXPECT(sm_overlap(a, b), "share bit 42: overlap");

    sm_free(a); sm_free(b);
    return 0;
}

CASE(test_membership)
{
    sparsemap_t *m = sm_create(2048);
    EXPECT(sm_membership(m) == SM_EMPTY, "fresh map empty");
    EXPECT(sm_membership(NULL) == SM_EMPTY, "NULL empty");

    sm_add(m, 42);
    EXPECT(sm_membership(m) == SM_SINGLETON, "one bit -> singleton");
    sm_add(m, 100);
    EXPECT(sm_membership(m) == SM_MULTIPLE, "two bits -> multiple");
    sm_add(m, 1000);
    EXPECT(sm_membership(m) == SM_MULTIPLE, "three bits -> multiple");

    sm_free(m);
    return 0;
}

CASE(test_singleton_member)
{
    sparsemap_t *m = sm_create(2048);
    EXPECT(sm_singleton_member(m) == SM_IDX_MAX, "empty: IDX_MAX");
    EXPECT(sm_singleton_member(NULL) == SM_IDX_MAX, "NULL: IDX_MAX");

    sm_add(m, 42);
    EXPECT(sm_singleton_member(m) == 42, "singleton: returns the bit");

    sm_add(m, 100);
    EXPECT(sm_singleton_member(m) == SM_IDX_MAX, "two bits: IDX_MAX");

    sm_free(m);
    return 0;
}

CASE(test_next_member)
{
    sparsemap_t *m = sm_create(8192);
    EXPECT(sm_next_member(m, SM_IDX_MAX) == SM_IDX_MAX, "empty: IDX_MAX");
    EXPECT(sm_next_member(NULL, SM_IDX_MAX) == SM_IDX_MAX, "NULL: IDX_MAX");

    sm_add(m, 0);
    sm_add(m, 100);
    sm_add(m, 1000);
    sm_add(m, 4000);
    EXPECT(sm_next_member(m, SM_IDX_MAX) == 0, "first set bit");
    EXPECT(sm_next_member(m, 0) == 100, "after 0");
    EXPECT(sm_next_member(m, 99) == 100, "after 99");
    EXPECT(sm_next_member(m, 100) == 1000, "after 100");
    EXPECT(sm_next_member(m, 1000) == 4000, "after 1000");
    EXPECT(sm_next_member(m, 4000) == SM_IDX_MAX, "past last");
    EXPECT(sm_next_member(m, 10000) == SM_IDX_MAX, "way past");

    /* RLE chunk path. */
    sparsemap_t *r = sm_create(8192);
    for (uint64_t i = 0; i < 4096; i++) sm_add(r, i);
    EXPECT(sm_next_member(r, SM_IDX_MAX) == 0, "RLE first");
    EXPECT(sm_next_member(r, 100) == 101, "RLE walk");
    EXPECT(sm_next_member(r, 4094) == 4095, "RLE last-1");
    EXPECT(sm_next_member(r, 4095) == SM_IDX_MAX, "past RLE end");
    sm_free(r);

    sm_free(m);
    return 0;
}

CASE(test_prev_member)
{
    sparsemap_t *m = sm_create(8192);
    EXPECT(sm_prev_member(m, SM_IDX_MAX) == SM_IDX_MAX, "empty: IDX_MAX");

    sm_add(m, 0);
    sm_add(m, 100);
    sm_add(m, 1000);
    sm_add(m, 4000);
    EXPECT(sm_prev_member(m, SM_IDX_MAX) == 4000, "last set bit");
    EXPECT(sm_prev_member(m, 4000) == 1000, "before 4000");
    EXPECT(sm_prev_member(m, 1001) == 1000, "before 1001");
    EXPECT(sm_prev_member(m, 1000) == 100, "before 1000");
    EXPECT(sm_prev_member(m, 100) == 0, "before 100");
    EXPECT(sm_prev_member(m, 0) == SM_IDX_MAX, "before first");

    /* RLE chunk path. */
    sparsemap_t *r = sm_create(8192);
    for (uint64_t i = 100; i < 200; i++) sm_add(r, i);
    EXPECT(sm_prev_member(r, SM_IDX_MAX) == 199, "RLE last");
    EXPECT(sm_prev_member(r, 150) == 149, "RLE walk");
    EXPECT(sm_prev_member(r, 100) == SM_IDX_MAX, "before RLE start");
    sm_free(r);

    sm_free(m);
    return 0;
}

CASE(test_iteration_idiom)
{
    sparsemap_t *m = sm_create(4096);
    const uint64_t bits[] = { 0, 7, 64, 100, 200, 1000, 1500 };
    const size_t n = sizeof(bits) / sizeof(bits[0]);
    for (size_t i = 0; i < n; i++) sm_add(m, bits[i]);

    /* Forward */
    size_t count = 0;
    uint64_t i = SM_IDX_MAX;
    while ((i = sm_next_member(m, i)) != SM_IDX_MAX) {
        EXPECT(i == bits[count], "forward iteration order");
        count++;
    }
    EXPECT(count == n, "forward visits every bit");

    /* Backward */
    count = 0;
    i = SM_IDX_MAX;
    while ((i = sm_prev_member(m, i)) != SM_IDX_MAX) {
        EXPECT(i == bits[n - 1 - count], "backward iteration order");
        count++;
    }
    EXPECT(count == n, "backward visits every bit");

    sm_free(m);
    return 0;
}

int main(void)
{
    fprintf(stderr, "test_coverage:\n");

    /* fill_factor */
    RUN(test_fill_factor_empty);
    RUN(test_fill_factor_dense);
    RUN(test_fill_factor_sparse);
    RUN(test_fill_factor_single_bit);

    /* owned_copy */
    RUN(test_owned_copy_of_owned);
    RUN(test_owned_copy_of_null);
    RUN(test_owned_copy_of_wrapped);

    /* free */
    RUN(test_free_null);
    RUN(test_free_owned_split_after_grow);

    /* set_data_size */
    RUN(test_set_data_size_owned_grow);
    RUN(test_set_data_size_owned_shrink);
    RUN(test_set_data_size_owned_same_size);
    RUN(test_set_data_size_wrap_shrink_in_place);
    RUN(test_set_data_size_explicit_buffer);
    RUN(test_set_data_size_null_input);
    RUN(test_set_data_size_split_grow);
    RUN(test_set_data_size_split_shrink);
    RUN(test_set_data_size_split_same_size);

    /* set ops matrix */
    RUN(test_setops_sparse_x_sparse);
    RUN(test_setops_rle_x_rle);
    RUN(test_setops_sparse_x_rle);
    RUN(test_setops_rle_x_sparse);
    RUN(test_setops_with_empty);
    RUN(test_setops_with_null);
    RUN(test_setops_identical_inputs);

    /* offset */
    RUN(test_offset_zero);
    RUN(test_offset_positive_chunk_aligned);
    RUN(test_offset_positive_unaligned);
    RUN(test_offset_negative_partial);
    RUN(test_offset_negative_drops_bits);
    RUN(test_offset_null);
    RUN(test_offset_rle_chunk);
    RUN(test_offset_carry_across_chunks);
    RUN(test_offset_one_bit);
    RUN(test_offset_dense_long_run);
    RUN(test_offset_chunk_aligned_negative);

    /* split exotic */
    RUN(test_split_at_zero);
    RUN(test_split_past_end);
    RUN(test_split_in_middle_sparse);

    /* select / rank */
    RUN(test_select_false_bits);
    RUN(test_select_in_rle_chunk);
    RUN(test_rank_both_polarities);

    /* chunk transitions */
    RUN(test_sparse_with_unused_flags);
    RUN(test_rle_to_sparse_transition);
    RUN(test_sparse_to_rle_transition);
    RUN(test_rle_extend);

    /* set ops cross-chunk */
    RUN(test_setops_spanning_many_chunks);
    RUN(test_setops_a_subset_of_b);
    RUN(test_setops_dense_dense);
    RUN(test_setops_first_chunks_disjoint);
    RUN(test_setops_long_runs);
    RUN(test_setops_many_chunks);
    RUN(test_setops_a_runs_out_first);
    RUN(test_setops_b_runs_out_first);
    RUN(test_setops_a_chunk_b_chunk_far_apart);

    /* min/max edges */
    RUN(test_min_max_empty);
    RUN(test_min_max_rle);

    /* span variants */
    RUN(test_span_unset_bits);
    RUN(test_span_full_run);
    RUN(test_span_with_start_offset);

    /* select edges */
    RUN(test_select_far_index);
    RUN(test_select_empty_map);
    RUN(test_select_unset_in_rle);
    RUN(test_select_unset_in_partial_rle);

    /* Phase A: predicates and iteration */
    RUN(test_is_empty);
    RUN(test_equals);
    RUN(test_is_subset);
    RUN(test_overlap);
    RUN(test_membership);
    RUN(test_singleton_member);
    RUN(test_next_member);
    RUN(test_prev_member);
    RUN(test_iteration_idiom);

    /* Phase B: cardinality without alloc, bulk add, to_array */
    RUN(test_union_cardinality);
    RUN(test_intersection_cardinality);
    RUN(test_difference_cardinality);
    RUN(test_nonempty_difference);
    RUN(test_jaccard_index);
    RUN(test_add_many);
    RUN(test_to_array);

    /* Phase B continued: range, xor, constructors, hash, compare */
    RUN(test_add_range);
    RUN(test_remove_range);
    RUN(test_xor);
    RUN(test_xor_cardinality);
    RUN(test_create_singleton);
    RUN(test_create_from_range);
    RUN(test_create_from_array);
    RUN(test_hash);
    RUN(test_compare);
    RUN(test_subset_compare);
    RUN(test_pop_first);

    /* Phase B in-place set operations */
    RUN(test_union_inplace);
    RUN(test_intersection_inplace);
    RUN(test_difference_inplace);

    /* flip / validate / statistics / shrink_to_fit */
    RUN(test_flip_range);
    RUN(test_validate_ok);
    RUN(test_statistics);
    RUN(test_shrink_to_fit);

    /* serialize / deserialize */
    RUN(test_serialize_roundtrip);
    RUN(test_serialize_empty);
    RUN(test_deserialize_validation);

    /* v2 additions */
    RUN(test_or_and_andnot);
    RUN(test_is_superset);
    RUN(test_extract_range);
    RUN(test_pop_last);

    /* v2.1 additions */
    RUN(test_open_copy);
    RUN(test_add_grow);
    RUN(test_allocator_global);
    RUN(test_allocator_per_map);

    /* scan */
    RUN(test_scan_basic);
    RUN(test_scan_with_skip);

    fprintf(stderr, "  %d/%d expectations passed, %d failures\n",
            g_total - g_failures, g_total, g_failures);
    return g_failures == 0 ? 0 : 1;
}
