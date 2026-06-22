/* SPDX-License-Identifier: MIT
 *
 * examples/ex_5.c -- exercising the v1.2 API expansion.
 *
 * Demonstrates the predicates, iteration primitives, in-place set
 * ops, range operations, and serialization added in v1.2.
 *
 * Build:
 *   cc -I include examples/ex_5.c builddir/src/libsparsemap.a -o ex_5
 *
 * (The meson build does this automatically via examples/meson.build.)
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sm.h>

static void
demo_predicates(void)
{
    printf("=== predicates and comparisons ===\n");

    sm_t *a = sm_create_from_range(0, 100);   /* {0..99} */
    sm_t *b = sm_create_from_range(50, 150);  /* {50..149} */

    printf("|a|             = %zu\n", sm_cardinality(a));
    printf("|b|             = %zu\n", sm_cardinality(b));
    printf("a empty?        = %d\n", sm_is_empty(a));
    printf("a == b?         = %d\n", sm_equals(a, b));
    printf("a subset of b?  = %d\n", sm_is_subset(a, b));
    printf("a overlap b?    = %d\n", sm_overlap(a, b));
    printf("|a U b|         = %zu  (without allocating)\n",
           sm_union_cardinality(a, b));
    printf("|a INT b|       = %zu  (without allocating)\n",
           sm_intersection_cardinality(a, b));
    printf("Jaccard(a, b)   = %.3f\n", sm_jaccard_index(a, b));

    sm_free(a);
    sm_free(b);
}

static void
demo_iteration(void)
{
    printf("\n=== forward and backward iteration ===\n");

    sm_t *m = sm_create(8192);
    const uint64_t bits[] = { 7, 64, 100, 1500, 4096, 5000 };
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        sm_add(m, bits[i]);
    }

    printf("forward:  ");
    for (uint64_t i = SM_IDX_MAX; (i = sm_next_member(m, i)) != SM_IDX_MAX; ) {
        printf("%lu ", i);
    }
    printf("\nbackward: ");
    for (uint64_t i = SM_IDX_MAX; (i = sm_prev_member(m, i)) != SM_IDX_MAX; ) {
        printf("%lu ", i);
    }
    printf("\n");

    /* Destructive iteration: pop_first drains the lowest element. */
    printf("pop_first drain: ");
    while (!sm_is_empty(m)) {
        printf("%lu ", sm_pop_first(m));
    }
    printf("\n");

    sm_free(m);
}

static void
demo_inplace_setops(void)
{
    printf("\n=== in-place set operations ===\n");

    sm_t *dst = sm_create_from_range(0, 100);
    sm_t *src = sm_create_from_range(50, 150);

    printf("before: |dst| = %zu, |src| = %zu\n",
           sm_cardinality(dst), sm_cardinality(src));

    dst = sm_union_inplace(dst, src);
    printf("after union_inplace:        |dst| = %zu (expect 150)\n",
           sm_cardinality(dst));

    dst = sm_intersection_inplace(dst, src);
    printf("after intersection_inplace: |dst| = %zu (expect 100)\n",
           sm_cardinality(dst));

    dst = sm_difference_inplace(dst, src);
    printf("after difference_inplace:   |dst| = %zu (expect 0)\n",
           sm_cardinality(dst));

    sm_free(dst);
    sm_free(src);
}

static void
demo_range_and_flip(void)
{
    printf("\n=== range manipulation ===\n");

    sm_t *m = sm_create(2048);
    sm_add_range(m, 0, 1000);
    printf("after add_range(0, 1000):    |m| = %zu\n", sm_cardinality(m));

    sm_remove_range(m, 200, 700);
    printf("after remove_range(200, 700):|m| = %zu\n", sm_cardinality(m));

    sm_flip_range(m, 0, 200);
    printf("after flip_range(0, 200):    |m| = %zu (200 set were cleared)\n",
           sm_cardinality(m));

    sm_free(m);
}

static void
demo_statistics(void)
{
    printf("\n=== statistics ===\n");

    sm_t *m = sm_create(8192);
    /* Mix dense (RLE) and sparse chunks. */
    sm_add_range(m, 0, 4096);     /* one RLE chunk worth */
    sm_add(m, 100000);
    sm_add(m, 200000);
    sm_add(m, 300000);             /* sparse chunks */

    sm_stats_t s;
    sm_statistics(m, &s);
    printf("chunks: total=%zu rle=%zu sparse=%zu\n",
           s.chunks_total, s.chunks_rle, s.chunks_sparse);
    printf("bits:   total=%lu in_rle=%lu in_sparse=%lu\n",
           s.bits_set, s.bits_in_rle, s.bits_in_sparse);
    printf("bytes:  used=%zu capacity=%zu / per_set_bit=%.4f\n",
           s.bytes_used, s.bytes_capacity, s.bytes_per_set_bit);

    sm_free(m);
}

static void
demo_serialize(void)
{
    printf("\n=== serialize / deserialize ===\n");

    sm_t *m = sm_create_from_range(0, 1000);
    sm_add(m, 99999);

    const size_t need = sm_serialized_size(m);
    uint8_t *buf = malloc(need);
    const size_t wrote = sm_serialize(m, buf, need);
    printf("serialized %zu bytes\n", wrote);

    sm_t *r = sm_deserialize(buf, wrote);
    assert(r != NULL);
    printf("deserialized: |r| = %zu, equals original? %d\n",
           sm_cardinality(r), sm_equals(m, r));

    free(buf);
    sm_free(m);
    sm_free(r);
}

int
main(void)
{
    demo_predicates();
    demo_iteration();
    demo_inplace_setops();
    demo_range_and_flip();
    demo_statistics();
    demo_serialize();
    return 0;
}
