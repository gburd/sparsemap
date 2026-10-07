/* SPDX-License-Identifier: MIT
 *
 * Regression test for the 5.8.1 sm_difference chunk-start misalignment
 * bug (fixed in 5.8.2).
 *
 * BUG: __sm_emit_chunk_bits -- the emitter used only by sm_difference --
 * had an RLE-clip branch that, when a difference clipped an RLE run to
 * [emit_start, emit_end), appended an output chunk whose start was the
 * raw emit_start.  A chunk start must be a multiple of
 * SM_CHUNK_MAX_CAPACITY (2048).  So sm_difference could emit a map with
 * CORRECT membership that nonetheless FAILS sm_validate, and for a long
 * clipped run it emitted one over-length RLE chunk spanning many 2048
 * windows.  The other set ops (union/intersection/xor) route through the
 * shared aligned run-emitter and were never affected.
 *
 * The minimal deterministic reproducer:
 *     a = [10000, 20000), b = [10000, 13794)
 *     d = a \ b = [13794, 20000)
 *     -> ONE chunk, start = 13794 (13794 % 2048 = 1506, UNALIGNED),
 *        sm_validate(d) == false, membership correct.
 *
 * The fix routes the clipped run through __sm_emit_run (the same ordered
 * emitter the other set ops use), which splits the run at chunk
 * boundaries and anchors each output chunk at its 2048-aligned floor.
 *
 * This test asserts, for the minimal case and a spread of clipped-run
 * shapes (including runs clipped at odd points that span several
 * chunks): sm_validate() == true AND membership byte-matches a
 * brute-force oracle (bit in a, not in b).  sm_validate() is the direct
 * signal -- it returns false for a misaligned or over-length chunk, so
 * this test fails against the pre-fix sm.c and passes after.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <sm.h>

static int g_failures = 0;
static int g_total = 0;

#define EXPECT(cond, msg) do {                                          \
        g_total++;                                                      \
        if (!(cond)) {                                                  \
            fprintf(stderr, "  FAIL: %s:%d: %s -- expected %s\n",        \
                    __FILE__, __LINE__, msg, #cond);                    \
            g_failures++;                                               \
        }                                                               \
} while (0)

/* deterministic xorshift */
static uint32_t rng = 0x12345678u;
static uint32_t xrng(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/*
 * Build a map from a brute-force bool[] oracle via sm_add_grow and check
 * that sm_difference(a, b):
 *   (1) validates,
 *   (2) has membership identical to the oracle (bit in a and not in b),
 * over the whole [0, univ) window.
 */
static void
check_diff(const unsigned char *A, const unsigned char *B, uint64_t univ,
    const char *label)
{
    sm_t *ma = sm_create(1024);
    sm_t *mb = sm_create(1024);
    sm_t *d;
    uint64_t i;
    long badmem = 0;

    if (ma == NULL || mb == NULL) {
        fprintf(stderr, "  OOM building %s\n", label);
        g_failures++;
        g_total++;
        sm_free(ma);
        sm_free(mb);
        return;
    }
    for (i = 0; i < univ; i++) {
        if (A[i] && sm_add_grow(&ma, i) == SM_IDX_MAX) {
            fprintf(stderr, "  add_grow a fail %s\n", label);
            g_failures++;
            g_total++;
            sm_free(ma);
            sm_free(mb);
            return;
        }
        if (B[i] && sm_add_grow(&mb, i) == SM_IDX_MAX) {
            fprintf(stderr, "  add_grow b fail %s\n", label);
            g_failures++;
            g_total++;
            sm_free(ma);
            sm_free(mb);
            return;
        }
    }

    d = sm_difference(ma, mb);

    /* A NULL result is valid iff the oracle difference is empty. */
    if (d == NULL) {
        bool anyset = false;
        for (i = 0; i < univ; i++)
            if (A[i] && !B[i]) {
                anyset = true;
                break;
            }
        EXPECT(!anyset, label); /* NULL only legal for an empty difference */
        sm_free(ma);
        sm_free(mb);
        return;
    }

    /* Primary signal: a misaligned / over-length chunk fails validate. */
    EXPECT(sm_validate(d), label);

    /* Membership must match the oracle exactly. */
    for (i = 0; i < univ; i++) {
        bool got = sm_contains(d, i, NULL);
        bool want = (A[i] && !B[i]);
        if (got != want)
            badmem++;
    }
    EXPECT(badmem == 0, label);

    sm_free(d);
    sm_free(ma);
    sm_free(mb);
}

/* Fill a span [lo, hi) in a bool[] oracle. */
static void
set_range(unsigned char *o, uint64_t lo, uint64_t hi, uint64_t univ)
{
    uint64_t i;
    for (i = lo; i < hi && i < univ; i++)
        o[i] = 1;
}

int
main(void)
{
    enum { UNIV = 80000 };
    static unsigned char A[UNIV];
    static unsigned char B[UNIV];
    int t;

    /* --- the minimal deterministic reproducer --- */
    memset(A, 0, sizeof(A));
    memset(B, 0, sizeof(B));
    set_range(A, 10000, 20000, UNIV);
    set_range(B, 10000, 13794, UNIV);
    check_diff(A, B, UNIV, "minimal repro a=[10000,20000) b=[10000,13794)");

    /* --- long clipped run that SPANS MANY chunks (the over-length
     *     RLE-chunk variant: survivor [13794, 60000) crosses ~22 windows) --- */
    memset(A, 0, sizeof(A));
    memset(B, 0, sizeof(B));
    set_range(A, 0, 60000, UNIV);
    set_range(B, 0, 13794, UNIV);
    check_diff(A, B, UNIV, "multi-chunk tail a=[0,60000) b=[0,13794)");

    /* --- b clips the TAIL of a at an odd point (survivor is a prefix) --- */
    memset(A, 0, sizeof(A));
    memset(B, 0, sizeof(B));
    set_range(A, 4096, 50000, UNIV);
    set_range(B, 23457, 55000, UNIV); /* survivor [4096,23457) */
    check_diff(A, B, UNIV, "clip tail a=[4096,50000) b=[23457,55000)");

    /* --- b is a short run strictly INSIDE a long run of a: survivor is
     *     two clipped pieces around b, both anchored at odd points --- */
    memset(A, 0, sizeof(A));
    memset(B, 0, sizeof(B));
    set_range(A, 1000, 40000, UNIV);
    set_range(B, 15123, 15987, UNIV); /* survivor [1000,15123) U [15987,40000) */
    check_diff(A, B, UNIV, "interior b a=[1000,40000) b=[15123,15987)");

    /* --- several b runs punched into a long a run --- */
    memset(A, 0, sizeof(A));
    memset(B, 0, sizeof(B));
    set_range(A, 0, 50000, UNIV);
    set_range(B, 3001, 3500, UNIV);
    set_range(B, 17777, 19999, UNIV);
    set_range(B, 33333, 33334, UNIV);
    check_diff(A, B, UNIV, "multi-punch a=[0,50000) b=3 interior runs");

    /* --- seeded random dense overlapping ranges with offset boundaries,
     *     the shape that triggers the clipped-RLE path most often --- */
    rng = 0x12345678u;
    for (t = 0; t < 2000; t++) {
        uint64_t alo = xrng() % 10000u;
        uint64_t aln = 2049 + (xrng() % 40000u); /* > 1 chunk, mostly RLE */
        uint64_t blo = xrng() % 10000u;
        uint64_t bln = 2049 + (xrng() % 40000u);
        char lbl[80];
        memset(A, 0, sizeof(A));
        memset(B, 0, sizeof(B));
        set_range(A, alo, alo + aln, UNIV);
        set_range(B, blo, blo + bln, UNIV);
        snprintf(lbl, sizeof(lbl),
            "rand dense a=[%" PRIu64 ",%" PRIu64 ") b=[%" PRIu64 ",%" PRIu64 ")",
            alo, alo + aln, blo, blo + bln);
        check_diff(A, B, UNIV, lbl);
    }

    fprintf(stderr, "  %d/%d expectations passed, %d failures\n",
        g_total - g_failures, g_total, g_failures);
    return g_failures == 0 ? 0 : 1;
}
