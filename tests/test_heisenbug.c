/* SPDX-License-Identifier: MIT
 *
 * Heisenbug reproducer for sparsemap.
 *
 * Documents and exercises the production-blocking bug filed in
 * HEISENBUG_REPORT.md by pg_tre maintainers:
 *
 *   sm_set_data_size(map, NULL, size) silently no-ops the
 *   reallocation when `map` was created via sm_wrap() (or
 *   any other lineage where m_data is not contiguous-with-struct),
 *   yet still updates m_capacity to the requested size.  The next
 *   sm_add() then writes past the actual buffer end and
 *   corrupts the libc heap.
 *
 * Two bug classes:
 *
 *   1. Direct: caller does sm_wrap + sm_set_data_size
 *      + sm_add.  Single-threaded, deterministic, ASan-visible.
 *
 *   2. Indirect: sm_union/_intersection/_difference allocate
 *      their result via sparsemap() (so the *result* is owned-
 *      contiguous and safe to grow), but if either input is wrap'd
 *      then mid-merge invariants break.  The chunks are read from
 *      the inputs; the writes hit the result.  Confirms the
 *      result-side path is sound under v1 semantics.
 *
 * Compile and run under AddressSanitizer to make the heap corruption
 * surface immediately rather than three operations downstream.  The
 * tests below treat "no crash, no ASan diagnostic, observable bits
 * correct" as success.
 *
 * Until Phase 1 of the consolidation lands, the direct-bug tests are
 * EXPECTED TO FAIL (or trip ASan).  After Phase 1, all tests must
 * pass cleanly.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sm.h>

/*
 * Local macros -- no munit dependency.  Standalone test, intended to
 * be wired into the meson test runner in Phase 3 with a TAP-style
 * adapter, and to be compilable with `cc test_heisenbug.c -lsparsemap`
 * for quick reproduction outside the build system.
 */
static int g_failures = 0;
static int g_total = 0;

#define CASE(name) static int name(void)

#define EXPECT(cond, msg) do {                                          \
        g_total++;                                                      \
        if (!(cond)) {                                                  \
            fprintf(stderr, "  FAIL: %s:%d: %s -- expected %s\n",        \
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
/*  Direct: wrap + set_data_size(NULL, big) + add                     */
/* ------------------------------------------------------------------ */

/*
 * Bug shape: caller wraps a 256-byte buffer, then asks the library to
 * grow to 4096 bytes via the (NULL, size) form, expecting the library
 * to handle reallocation.  Today the library silently no-ops the grow
 * and just sets m_capacity = 4096.  Subsequent sm_add calls
 * write past the original 256-byte buffer.
 *
 * Post-fix contract: either grow succeeds (transparently promoting
 * the wrap'd map to owned-split lineage, leaving the caller's buffer
 * untouched), or the call returns NULL and m_capacity is unchanged.
 * Silent corruption is not an option.
 */
CASE(test_wrap_then_grow_via_set_data_size_null)
{
    _Alignas(uint64_t) uint8_t small[256];
    memset(small, 0, sizeof(small));
    sm_t *map = sm_wrap(small, sizeof(small));
    EXPECT(map != NULL, "wrap allocates handle");

    /*
     * Initialize the wrapped buffer.  After this call, m_data_used
     * must be SM_SIZEOF_OVERHEAD (4) and chunk_count must be 0.
     * (See test_empty_map.c for the empty-map invariants.)
     */
    sm_clear(map);

    /*
     * Now ask the library to grow.  Today this silently sets
     * m_capacity = 4096 without reallocating.  Post-fix it must
     * either:
     *   - return non-NULL with a real 4096-byte buffer (lineage
     *     promoted to owned-split or owned-contiguous), or
     *   - return NULL with m_capacity unchanged at 256.
     */
    sm_t *grown = sm_set_data_size(map, NULL, 4096);

    if (grown == NULL) {
        /* Acceptable failure mode: caller's buffer is intact. */
        EXPECT(sm_get_capacity(map) == 256,
               "rejected grow leaves capacity unchanged");
        free(map);
        return 0;
    }

    /*
     * Successful grow.  The reported capacity must match a buffer the
     * library actually owns; if we now write 1024 bits we must not
     * corrupt the caller's `small` buffer.
     */
    EXPECT(sm_get_capacity(grown) >= 4096,
           "post-grow capacity reflects the new buffer");

    /* Write a sentinel pattern into `small` after the grow. */
    memset(small, 0xCC, sizeof(small));

    /*
     * Add 100 closely-spaced bits.  These all live in a single chunk
     * (chunks hold 2048 bits each), so 4096 bytes is far more than
     * enough.  Pre-fix, this corrupted the heap because the library
     * was writing into `small` past offset 256.  Post-fix the library
     * has its own 4096-byte buffer.
     */
    for (uint64_t i = 0; i < 100; i++) {
        const uint64_t r = sm_add(grown, i * 8);
        EXPECT(r == i * 8, "add succeeds in grown map");
    }

    /* Verify the bits we set are observable. */
    for (uint64_t i = 0; i < 100; i++) {
        EXPECT(sm_contains(grown, i * 8, NULL),
               "set bit reads back as set");
    }

    /* Caller's buffer must not have been overwritten by the library. */
    for (size_t i = 0; i < sizeof(small); i++) {
        EXPECT(small[i] == 0xCC,
               "caller's wrap buffer untouched after grow");
    }

    sm_free(grown);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Direct: wrap + set_data_size(new_buf, size) + add                 */
/* ------------------------------------------------------------------ */

/*
 * The (data, size) three-arg form is documented as "caller takes
 * responsibility for reallocating their buffer".  This case verifies
 * that a caller who does that explicitly gets a working map that can
 * fit values up to the new capacity.
 */
CASE(test_wrap_then_swap_buffer)
{
    _Alignas(uint64_t) uint8_t small[256];
    memset(small, 0, sizeof(small));
    sm_t *map = sm_wrap(small, sizeof(small));
    EXPECT(map != NULL, "wrap allocates handle");

    sm_clear(map);

    /* Caller-managed grow: copy the bits into a larger buffer. */
    _Alignas(uint64_t) uint8_t big[4096];
    memset(big, 0, sizeof(big));
    memcpy(big, small, sizeof(small));

    sm_t *grown = sm_set_data_size(map, big, sizeof(big));
    EXPECT(grown != NULL, "swap-buffer grow succeeds");
    EXPECT(sm_get_capacity(grown) == sizeof(big),
           "post-swap capacity equals new buffer size");

    /* Adding bits writes into `big`, not `small`. */
    for (uint64_t i = 0; i < 100; i++) {
        sm_add(grown, i * 8);
    }

    /*
     * `small` still holds whatever serialized empty-map state we wrote
     * via sm_clear.  At minimum, the first SM_SIZEOF_OVERHEAD
     * bytes must be zero (chunk count 0); bytes after that may be
     * anything.  Just verify the library didn't write into `small`
     * past byte 4 -- for our purposes, bytes [4, 256) should still
     * be zero from the initial memset.
     */
    int small_unchanged_past_overhead = 1;
    for (size_t i = 4; i < sizeof(small); i++) {
        if (small[i] != 0) {
            small_unchanged_past_overhead = 0;
            break;
        }
    }
    EXPECT(small_unchanged_past_overhead,
           "caller's old buffer untouched after swap");

    /*
     * Post-swap, lineage is SM_WRAPPED again (we handed the library a
     * caller-owned buffer).  Disposing with sm_free leaves
     * `big` for the caller.
     */
    sm_free(grown);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sm_owned_copy() normalizes lineage                          */
/* ------------------------------------------------------------------ */

/*
 * Whatever the lineage of the input, sm_owned_copy() returns an
 * SM_OWNED_CONTIGUOUS map: a single allocation that's safe to grow and
 * dispose with sm_free or libc free.
 */
CASE(test_owned_copy_normalizes_lineage)
{
    /* Make a wrap'd input. */
    _Alignas(uint64_t) uint8_t buf[1024];
    memset(buf, 0, sizeof(buf));
    sm_t *wrapped = sm_wrap(buf, sizeof(buf));
    sm_clear(wrapped);
    for (uint64_t i = 0; i < 50; i++) {
        sm_add(wrapped, i * 16);
    }

    /* Copy with normalized lineage. */
    sm_t *owned = sm_owned_copy(wrapped);
    EXPECT(owned != NULL, "owned_copy succeeds");

    /* Same observable state. */
    EXPECT(sm_cardinality(owned) == 50,
           "copy has same cardinality");
    for (uint64_t i = 0; i < 50; i++) {
        EXPECT(sm_contains(owned, i * 16, NULL),
               "copy contains same bits");
    }

    /* The copy can be grown. */
    sm_t *grown = sm_set_data_size(owned, NULL, 4096);
    EXPECT(grown != NULL, "owned_copy result is growable");
    EXPECT(sm_get_capacity(grown) >= 4096,
           "grown capacity reflects requested size");

    sm_free(grown);
    sm_free(wrapped);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Indirect: union with one wrap'd input growing the result          */
/* ------------------------------------------------------------------ */

/*
 * sm_union allocates its result via sparsemap() so the result
 * is owned-contiguous from the start.  The result-side grow path is
 * what __sm_ensure_capacity uses; that path must work regardless of
 * input lineages.
 */
CASE(test_union_with_wrapped_input_grows_result)
{
    /* Input A: wrap'd, populated densely.  We'll drive the result. */
    _Alignas(uint64_t) uint8_t a_buf[2048];
    memset(a_buf, 0, sizeof(a_buf));
    sm_t *a = sm_wrap(a_buf, sizeof(a_buf));
    sm_clear(a);
    for (uint64_t i = 0; i < 256; i++) {
        if (sm_add(a, i * 64) != i * 64) {
            break; /* wrap'd, no auto-grow */
        }
    }

    /* Input B: owned-contiguous, populated with disjoint bits. */
    sm_t *b = sparsemap(2048);
    EXPECT(b != NULL, "owned input allocates");
    sm_clear(b);
    for (uint64_t i = 0; i < 256; i++) {
        if (sm_add(b, 1000000 + i * 64) != 1000000 + i * 64) {
            break;
        }
    }

    /*
     * Union the disjoint inputs.  Result must be allocated owned-
     * contiguous and must contain the union of bits without
     * corrupting either input's buffer.
     */
    sm_t *u = sm_union(a, b);
    EXPECT(u != NULL, "union returns a non-NULL result");

    /* Spot-check a few bits from each side. */
    EXPECT(sm_contains(u, 0, NULL), "bit from a");
    EXPECT(sm_contains(u, 64, NULL), "bit from a");
    EXPECT(sm_contains(u, 1000000, NULL), "bit from b");
    EXPECT(sm_contains(u, 1000064, NULL), "bit from b");
    EXPECT(!sm_contains(u, 1, NULL), "unset bit");

    free(u);
    free(b);
    free(a);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Indirect: intersection / difference shaped inputs                 */
/* ------------------------------------------------------------------ */

CASE(test_intersection_difference_with_wrapped)
{
    _Alignas(uint64_t) uint8_t a_buf[2048];
    _Alignas(uint64_t) uint8_t b_buf[2048];
    memset(a_buf, 0, sizeof(a_buf));
    memset(b_buf, 0, sizeof(b_buf));
    sm_t *a = sm_wrap(a_buf, sizeof(a_buf));
    sm_t *b = sm_wrap(b_buf, sizeof(b_buf));
    sm_clear(a);
    sm_clear(b);

    /* Overlapping but not identical populations. */
    for (uint64_t i = 0; i < 100; i++) {
        sm_add(a, i * 100);
        sm_add(b, i * 100 + (i % 2));
    }

    sm_t *intr = sm_intersection(a, b);
    sm_t *diff = sm_difference(a, b);

    /* Even-index bits are in both (i*100 == i*100 + 0); odd-index aren't. */
    EXPECT(sm_contains(intr, 0, NULL), "even index in intersection");
    EXPECT(sm_contains(intr, 200, NULL), "even index in intersection");
    EXPECT(!sm_contains(intr, 100, NULL), "odd index NOT in intersection");

    EXPECT(!sm_contains(diff, 0, NULL), "even index NOT in difference");
    EXPECT(sm_contains(diff, 100, NULL), "odd index in difference");

    sm_free(intr);
    sm_free(diff);
    sm_free(a);
    sm_free(b);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Driver                                                            */
/* ------------------------------------------------------------------ */

/* Driver -- registers all tests including owned_copy normalization. */
int main(void)
{
    fprintf(stderr, "test_heisenbug:\n");
    RUN(test_wrap_then_grow_via_set_data_size_null);
    RUN(test_wrap_then_swap_buffer);
    RUN(test_owned_copy_normalizes_lineage);
    RUN(test_union_with_wrapped_input_grows_result);
    RUN(test_intersection_difference_with_wrapped);
    fprintf(stderr, "  %d/%d expectations passed, %d failures\n",
            g_total - g_failures, g_total, g_failures);
    return g_failures == 0 ? 0 : 1;
}
