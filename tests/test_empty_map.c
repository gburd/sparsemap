/* SPDX-License-Identifier: MIT
 *
 * Empty-map and m_data_used==0 reproducer for sparsemap.
 *
 * Documents and exercises the "second bug" from
 * pg_tre/doc/sparsemap-bugfix-m_data_used-0.md:
 *
 *   __sm_get_chunk_count(map) reads *(uint32_t *)&map->m_data[0]
 *   regardless of map->m_data_used.  When m_data_used == 0
 *   (post-sparsemap_wrap with no subsequent clear/open, or any other
 *   path that produces a zero-used map) the read returns whatever
 *   garbage is in the first four bytes of the wrapped buffer.
 *   Functions that iterate chunks then walk past the buffer end:
 *
 *     - sparsemap_intersection   (line 2473 in pg_tre's vendored copy)
 *     - sparsemap_union          (line 2994)
 *     - sparsemap_maximum        (line 1733)
 *     - __sm_rank_vec            (line 3506)
 *
 * pg_tre carries 4 local "BUG FIX: m_data_used = 0 but garbage chunk
 * count" patches that defensively short-circuit each path.  The
 * upstream fix is to make __sm_get_chunk_count return 0 when
 * m_data_used < SM_SIZEOF_OVERHEAD (i.e. the chunk-count slot has
 * not been initialized), which makes all downstream paths correct
 * by construction.
 *
 * After Phase 1 of the consolidation, every test below must pass
 * without relying on any pg_tre-side workaround.
 */
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
            fprintf(stderr, "  FAIL: %s:%d: %s — expected %s\n",        \
                    __FILE__, __LINE__, msg, #cond);                    \
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
/*  Garbage-chunk-count: read paths must not iterate past end          */
/* ------------------------------------------------------------------ */

/*
 * Wrap a buffer that has non-zero content in its first 4 bytes and
 * never call sparsemap_clear or sparsemap_open.  m_data_used == 0,
 * so __sm_get_chunk_count must report 0 — not whatever uint32_t lives
 * at m_data[0..3].
 *
 * We can't directly observe __sm_get_chunk_count from outside the
 * library, so we exercise the four documented victim functions and
 * assert "no crash, sensible empty-map answers".
 */
CASE(test_max_on_zero_used_with_dirty_buffer)
{
    uint8_t buf[256];
    memset(buf, 0xFF, sizeof(buf)); /* every byte non-zero */
    sparsemap_t *map = sparsemap_wrap(buf, sizeof(buf));
    EXPECT(map != NULL, "wrap succeeds");

    /*
     * Per the post-fix contract, sparsemap_maximum on an
     * uninitialized-buffer map must return 0 (the documented
     * empty-map sentinel) rather than reading garbage chunk metadata.
     */
    const uint64_t mx = sparsemap_maximum(map);
    EXPECT(mx == 0, "maximum on zero-used map is 0");

    free(map);
    return 0;
}

CASE(test_rank_on_zero_used_with_dirty_buffer)
{
    uint8_t buf[256];
    memset(buf, 0xAB, sizeof(buf));
    sparsemap_t *map = sparsemap_wrap(buf, sizeof(buf));
    EXPECT(map != NULL, "wrap succeeds");

    /* Rank of "set bits" in any range of an empty map must be 0. */
    const size_t r_set = sparsemap_rank(map, 0, 1000, true);
    EXPECT(r_set == 0, "rank(set) on zero-used map is 0");

    free(map);
    return 0;
}

CASE(test_union_with_zero_used_input)
{
    uint8_t bad[256];
    memset(bad, 0x55, sizeof(bad));
    sparsemap_t *a = sparsemap_wrap(bad, sizeof(bad));

    sparsemap_t *b = sparsemap(2048);
    sparsemap_clear(b);
    sparsemap_add(b, 42);
    sparsemap_add(b, 4242);

    /*
     * Union of a zero-used map and a populated map should be
     * equivalent to the populated map — it must not iterate `a`'s
     * garbage chunk metadata.
     */
    sparsemap_t *u = sparsemap_union(a, b);
    EXPECT(u != NULL, "union returns non-NULL");
    EXPECT(sparsemap_contains(u, 42), "bit from b present in union");
    EXPECT(sparsemap_contains(u, 4242), "bit from b present in union");

    free(u);
    free(b);
    free(a);
    return 0;
}

CASE(test_intersection_with_zero_used_input)
{
    uint8_t bad[256];
    memset(bad, 0x77, sizeof(bad));
    sparsemap_t *a = sparsemap_wrap(bad, sizeof(bad));

    sparsemap_t *b = sparsemap(2048);
    sparsemap_clear(b);
    sparsemap_add(b, 42);

    /* Intersection of zero-used and anything must be empty (no crash). */
    sparsemap_t *i = sparsemap_intersection(a, b);

    /*
     * Some implementations return NULL for an empty intersection.
     * That's acceptable; what's not acceptable is a crash or a result
     * that contains `b`'s bits.
     */
    if (i != NULL) {
        EXPECT(!sparsemap_contains(i, 42),
               "intersection with zero-used must be empty");
        free(i);
    }
    free(b);
    free(a);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Post-clear / post-create empty-map invariants                      */
/* ------------------------------------------------------------------ */

/*
 * After sparsemap_create() / sparsemap() / sparsemap_init() /
 * sparsemap_clear(), the map must be in the canonical empty state:
 *   m_data_used == SM_SIZEOF_OVERHEAD
 *   chunk_count == 0
 *
 * (We can't observe these directly without including sparsemap.c's
 * private struct, but the behavioural test is "every aggregate
 * function returns the empty-map answer".)
 */
CASE(test_fresh_map_is_empty)
{
    sparsemap_t *m = sparsemap(2048);
    EXPECT(m != NULL, "fresh allocation succeeds");

    EXPECT(sparsemap_cardinality(m) == 0, "fresh cardinality is 0");
    EXPECT(sparsemap_maximum(m) == 0, "fresh maximum is 0");
    EXPECT(sparsemap_minimum(m) == 0, "fresh minimum is 0");
    EXPECT(sparsemap_rank(m, 0, UINT64_MAX, true) == 0,
           "fresh rank(set) is 0");

    free(m);
    return 0;
}

CASE(test_cleared_map_is_empty)
{
    sparsemap_t *m = sparsemap(2048);
    sparsemap_add(m, 100);
    sparsemap_add(m, 1000);
    EXPECT(sparsemap_cardinality(m) == 2, "populated cardinality 2");

    sparsemap_clear(m);
    EXPECT(sparsemap_cardinality(m) == 0, "post-clear cardinality 0");
    EXPECT(sparsemap_maximum(m) == 0, "post-clear maximum 0");
    EXPECT(!sparsemap_contains(m, 100), "post-clear bit absent");

    free(m);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Driver                                                            */
/* ------------------------------------------------------------------ */

int main(void)
{
    fprintf(stderr, "test_empty_map:\n");
    RUN(test_max_on_zero_used_with_dirty_buffer);
    RUN(test_rank_on_zero_used_with_dirty_buffer);
    RUN(test_union_with_zero_used_input);
    RUN(test_intersection_with_zero_used_input);
    RUN(test_fresh_map_is_empty);
    RUN(test_cleared_map_is_empty);
    fprintf(stderr, "  %d/%d expectations passed, %d failures\n",
            g_total - g_failures, g_total, g_failures);
    return g_failures == 0 ? 0 : 1;
}
