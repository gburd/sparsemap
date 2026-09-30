/* SPDX-License-Identifier: MIT
 *
 * Regression + oracle cross-check for the chunk-aware sm_add_range()
 * fast path (opt/add-range).
 *
 * The old sm_add_range() was a per-bit loop:
 *     for (i = lo; i < hi; i++) sm_add(map, i);
 * O(hi - lo), ~14 ns/bit -- a [0, 1e6) add cost ~14 ms even though the
 * result is a short run of all-ONES sparse chunks.  The fast path emits
 * the run directly through the same ordered emitter the set-ops use, in
 * O(existing_chunks + run_chunks).
 *
 * Every case below checks the fast-path result against BOTH:
 *   (a) a brute-force bool[] oracle over the affected span, and
 *   (b) a REFERENCE map built with a copy of the OLD per-bit loop
 *       (ref_add_range, using sm_add_grow so the reference can grow).
 * If the fast path and the old loop ever disagree (sm_equals /
 * sm_hash / sm_cardinality) the test fails -- so it genuinely
 * exercises the new logic and would catch a fast-path bug.
 *
 * It also asserts sm_validate(), serialize->deserialize round-trips,
 * ascending iteration, and (for [0, N) runs) that the fast path footprint
 * matches the old per-bit loop byte-for-byte -- this RLE-free variant
 * stores a run as all-ONES sparse chunks, so [0, N) does not collapse to a
 * constant size, but the two builders must still agree.
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

/* -- deterministic RNG (xorshift) ---------------------------------- */
static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
static uint64_t xrng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* -- reference: a copy of the OLD per-bit sm_add_range loop --------- */
static void ref_add_range(sm_t **mp, uint64_t lo, uint64_t hi)
{
    uint64_t i;
    for (i = lo; i < hi; i++) {
        if (sm_add_grow(mp, i) == SM_IDX_MAX) {
            fprintf(stderr, "  ref: add_grow ENOSPC at %" PRIu64 "\n", i);
            abort();
        }
    }
}

/*
 * Apply a sequence of ranges through the fast path (into `fast`) and the
 * old loop (into `ref`), then cross-check everything over [span_lo, span_hi).
 */
static void check_seq(const uint64_t *slo, const uint64_t *shi, int nseed,
                      uint64_t span_lo, uint64_t span_hi, const char *label)
{
    int i;
    uint64_t k, idx, prev = 0;
    bool first = true;
    /* Generously sized so ENOSPC never masks a correctness bug. */
    sm_t *fast = sm_create(1u << 20);
    sm_t *ref = sm_create(64);

    for (i = 0; i < nseed; i++) {
        g_total++;
        if (!sm_add_range(fast, slo[i], shi[i])) {
            fprintf(stderr, "  FAIL [%s]: fast add_range [%" PRIu64 ",%"
                    PRIu64 ") returned false\n", label, slo[i], shi[i]);
            g_failures++;
            goto done;
        }
        ref_add_range(&ref, slo[i], shi[i]);
    }

    /* 1. structural validity */
    EXPECT(sm_validate(fast), label);
    /* 2. same map as the OLD loop (equals + hash) */
    EXPECT(sm_equals(fast, ref), label);
    EXPECT(sm_hash(fast) == sm_hash(ref), label);
    /* 3. cardinality */
    EXPECT(sm_cardinality(fast) == sm_cardinality(ref), label);

    /* 4. brute-force bool[] oracle over the affected span */
    if (span_hi > span_lo && span_hi - span_lo <= 8000000ULL) {
        size_t n = (size_t)(span_hi - span_lo);
        unsigned char *bits = (unsigned char *)calloc(n, 1);
        int mism = 0;
        for (i = 0; i < nseed; i++) {
            uint64_t a = slo[i] > span_lo ? slo[i] : span_lo;
            uint64_t b = shi[i] < span_hi ? shi[i] : span_hi;
            for (k = a; k < b; k++)
                bits[k - span_lo] = 1;
        }
        for (k = span_lo; k < span_hi; k++) {
            bool want = bits[k - span_lo] != 0;
            bool got = sm_contains(fast, k, NULL);
            if (want != got) {
                if (mism++ == 0)
                    fprintf(stderr, "  FAIL [%s]: oracle @%" PRIu64
                            " want=%d got=%d\n", label, k, want, got);
            }
        }
        g_total++;
        if (mism) g_failures++;
        free(bits);
    }

    /* 5. serialize -> deserialize round-trip */
    {
        size_t sz = sm_serialized_size(fast);
        uint8_t *buf = (uint8_t *)malloc(sz ? sz : 1);
        size_t w = sm_serialize(fast, buf, sz);
        sm_t *rt = sm_deserialize(buf, w);
        EXPECT(rt != NULL && sm_equals(rt, fast), label);
        if (rt) sm_free(rt);
        free(buf);
    }

    /* 6. ascending iteration */
    idx = SM_IDX_MAX;
    g_total++;
    while ((idx = sm_next_member(fast, idx, NULL)) != SM_IDX_MAX) {
        if (!first && idx <= prev) {
            fprintf(stderr, "  FAIL [%s]: iteration not ascending @%"
                    PRIu64 "\n", label, idx);
            g_failures++;
            break;
        }
        prev = idx;
        first = false;
    }

done:
    sm_free(fast);
    sm_free(ref);
}

static void one(uint64_t lo, uint64_t hi, uint64_t slo, uint64_t shi,
                const char *l)
{
    uint64_t a[1], b[1];
    a[0] = lo; b[0] = hi;
    check_seq(a, b, 1, slo, shi, l);
}

int main(void)
{
    int t;

    fprintf(stderr, "test_add_range:\n");

    /* --- degenerate / empty --- */
    one(5, 5, 0, 10, "empty lo==hi");
    one(10, 5, 0, 20, "lo>hi (no-op)");

    /* --- tiny --- */
    one(0, 1, 0, 4, "single bit 0");
    one(7, 8, 0, 16, "single bit mid");

    /* --- word aligned --- */
    one(0, 64, 0, 128, "word 0");
    one(64, 128, 0, 256, "word 1");
    one(0, 256, 0, 320, "four words");

    /* --- unaligned --- */
    one(3, 130, 0, 200, "unaligned small");
    one(1, 2047, 0, 2100, "unaligned near chunk");

    /* --- crossing 1 chunk boundary --- */
    one(2000, 2100, 1900, 2200, "cross 1 chunk boundary");
    one(0, 2048, 0, 2100, "exactly one chunk");
    one(0, 2049, 0, 2200, "one chunk + 1");

    /* --- crossing several chunk boundaries --- */
    one(0, 10000, 0, 10100, "several chunks");
    one(1500, 9000, 1400, 9100, "several chunks unaligned");

    /* --- thousands of chunk boundaries --- */
    one(0, 4000000ULL, 0, 4000000ULL, "thousands of chunk boundaries");

    /* --- straddle the small/chunk (1024) boundary --- */
    one(0, 1023, 0, 1100, "up to 1023 (small)");
    one(0, 1024, 0, 1100, "up to 1024");
    one(1000, 1050, 0, 1100, "straddle 1024");
    one(500, 2000, 0, 2100, "straddle 1024 into chunk");

    /* --- entirely inside small-set mode --- */
    one(0, 100, 0, 512, "inside small mode");
    one(200, 400, 0, 512, "inside small mode mid");

    /* --- footprint: [0,N) must match the old per-bit loop map exactly.
     * This RLE-free variant stores a run as all-ONES sparse chunks, so the
     * footprint grows with N (no RLE collapse); the invariant is that the
     * fast path produces byte-identical bytes to the old loop, plus the
     * right cardinality and a valid map. --- */
    {
        const uint64_t Ns[] = { 1000ULL, 10000ULL, 1000000ULL, 4000000ULL };
        int i;
        for (i = 0; i < 4; i++) {
            sm_t *m = sm_create(1u << 22);
            sm_t *ref = sm_create(64);
            g_total++;
            if (!sm_add_range(m, 0, Ns[i])) {
                fprintf(stderr, "  FAIL: footprint add_range[0,%" PRIu64
                        ") false\n", Ns[i]);
                g_failures++;
            } else {
                ref_add_range(&ref, 0, Ns[i]);
                EXPECT(sm_get_size(m) == sm_get_size(ref),
                       "footprint == old loop size");
                EXPECT(sm_equals(m, ref), "footprint == old loop bytes");
                EXPECT(sm_hash(m) == sm_hash(ref), "footprint hash");
                EXPECT(sm_cardinality(m) == (size_t)Ns[i], "footprint card");
                EXPECT(sm_validate(m), "footprint validate");
            }
            sm_free(m);
            sm_free(ref);
        }
    }

    /* --- overlap / abut / idempotent (union semantics) --- */
    {
        uint64_t lo[2], hi[2];
        lo[0] = 100; hi[0] = 200; lo[1] = 100; hi[1] = 200;
        check_seq(lo, hi, 2, 0, 300, "idempotent double add");
        lo[0] = 0; hi[0] = 100; lo[1] = 100; hi[1] = 200;
        check_seq(lo, hi, 2, 0, 300, "abutting -> coalesce");
        lo[0] = 0; hi[0] = 100; lo[1] = 50; hi[1] = 150;
        check_seq(lo, hi, 2, 0, 200, "overlapping");
        lo[0] = 0; hi[0] = 2048; lo[1] = 2048; hi[1] = 4096;
        check_seq(lo, hi, 2, 0, 4200, "abut across chunk boundary");
    }
    {
        /* Range OR'd over pre-existing scattered set bits. */
        uint64_t lo[3], hi[3];
        lo[0] = 5;   hi[0] = 6;
        lo[1] = 3000; hi[1] = 3001;
        lo[2] = 100; hi[2] = 4000;
        check_seq(lo, hi, 3, 0, 4100, "range OR over scattered bits");
    }
    {
        /* small mode, then a range that promotes past 1024. */
        uint64_t lo[2], hi[2];
        lo[0] = 0;   hi[0] = 100;
        lo[1] = 900; hi[1] = 1200;
        check_seq(lo, hi, 2, 0, 1300, "small then promote");
    }
    {
        /* big base range, then extend far. */
        uint64_t lo[2], hi[2];
        lo[0] = 0;      hi[0] = 500000;
        lo[1] = 400000; hi[1] = 1000000;
        check_seq(lo, hi, 2, 399000, 1000010, "big base + overlapping extend");
    }

    /* --- seeded random single ranges --- */
    for (t = 0; t < 3000; t++) {
        uint64_t base = xrng() % 60000ULL;
        uint64_t len = 1 + (xrng() % 25000ULL);
        uint64_t lo = base, hi = base + len;
        uint64_t slo = lo > 200 ? lo - 200 : 0;
        uint64_t shi = hi + 200;
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "rand[%" PRIu64 ",%" PRIu64 ")", lo, hi);
        one(lo, hi, slo, shi, lbl);
    }

    /* --- seeded random multi-range sequences with overlaps --- */
    for (t = 0; t < 800; t++) {
        uint64_t lo[6], hi[6];
        int ns = 2 + (int)(xrng() % 4);
        int i;
        uint64_t maxhi = 0;
        for (i = 0; i < ns; i++) {
            uint64_t b = xrng() % 9000ULL;
            uint64_t l = 1 + (xrng() % 5000ULL);
            lo[i] = b;
            hi[i] = b + l;
            if (hi[i] > maxhi) maxhi = hi[i];
        }
        check_seq(lo, hi, ns, 0, maxhi + 50, "rand multi-range");
    }

    /* --- large 64-bit indices (strength: 64-bit domain) --- */
    one(1000000000000ULL, 1000000000000ULL + 5000ULL,
        1000000000000ULL - 100ULL, 1000000000000ULL + 5100ULL,
        "high 64-bit range");

    fprintf(stderr, "  %d/%d expectations passed, %d failures\n",
            g_total - g_failures, g_total, g_failures);
    return g_failures == 0 ? 0 : 1;
}
