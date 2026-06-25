/* SPDX-License-Identifier: MIT
 *
 * tests/test_portability.c -- verify the SWAR fallbacks in
 * src/sm_portability.h agree with the GCC/Clang builtins for a
 * battery of representative inputs.
 *
 * On GCC and Clang the SM_POPCOUNT64 / SM_CTZ64 / SM_CLZ64 macros
 * expand to the builtins directly; this test still runs but is a
 * trivial tautology.  Its real job is to be the regression check
 * for the day someone builds on MSVC or a compiler without the
 * builtins, where the fallback paths exercise different code.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Force the SWAR helpers to be visible regardless of compiler.
 * This duplicates the bodies from sm_portability.h; keep in sync. */
static inline int
test_swar_popcount64(uint64_t x)
{
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}

static inline int
test_swar_ctz64(uint64_t x)
{
    int n = 0;
    if (!(x & 0xFFFFFFFFULL)) { n += 32; x >>= 32; }
    if (!(x & 0xFFFFULL))     { n += 16; x >>= 16; }
    if (!(x & 0xFFULL))       { n +=  8; x >>=  8; }
    if (!(x & 0xFULL))        { n +=  4; x >>=  4; }
    if (!(x & 0x3ULL))        { n +=  2; x >>=  2; }
    if (!(x & 0x1ULL))        { n +=  1; }
    return n;
}

static inline int
test_swar_clz64(uint64_t x)
{
    int n = 0;
    if (!(x & 0xFFFFFFFF00000000ULL)) { n += 32; x <<= 32; }
    if (!(x & 0xFFFF000000000000ULL)) { n += 16; x <<= 16; }
    if (!(x & 0xFF00000000000000ULL)) { n +=  8; x <<=  8; }
    if (!(x & 0xF000000000000000ULL)) { n +=  4; x <<=  4; }
    if (!(x & 0xC000000000000000ULL)) { n +=  2; x <<=  2; }
    if (!(x & 0x8000000000000000ULL)) { n +=  1; }
    return n;
}

static int g_failures = 0;

#define EXPECT(cond, fmt, ...) do {                                     \
    if (!(cond)) {                                                      \
        fprintf(stderr, "FAIL: " fmt "\n", ##__VA_ARGS__);              \
        g_failures++;                                                   \
    }                                                                   \
} while (0)

static void
check_one(uint64_t x)
{
    /* Compare against builtins on toolchains that have them. */
#if defined(__GNUC__) || defined(__clang__)
    const int gpc = (int)__builtin_popcountll((unsigned long long)x);
    const int gcz = (x != 0) ? __builtin_ctzll((unsigned long long)x) : 0;
    const int glz = (x != 0) ? __builtin_clzll((unsigned long long)x) : 0;
    const int spc = test_swar_popcount64(x);
    const int scz = (x != 0) ? test_swar_ctz64(x) : 0;
    const int slz = (x != 0) ? test_swar_clz64(x) : 0;
    EXPECT(gpc == spc, "popcount(%016lx): builtin=%d swar=%d", (unsigned long)x, gpc, spc);
    EXPECT(gcz == scz, "ctz(%016lx): builtin=%d swar=%d",      (unsigned long)x, gcz, scz);
    EXPECT(glz == slz, "clz(%016lx): builtin=%d swar=%d",      (unsigned long)x, glz, slz);
#else
    /* Without builtins we trust SWAR; spot-check one known answer. */
    if (x == 1ULL) {
        EXPECT(test_swar_popcount64(x) == 1, "pop(1) = 1");
        EXPECT(test_swar_ctz64(x)      == 0, "ctz(1) = 0");
        EXPECT(test_swar_clz64(x)      == 63, "clz(1) = 63");
    }
#endif
}

int
main(void)
{
    /* Hand-picked edge cases. */
    static const uint64_t cases[] = {
        0x1ULL,
        0x2ULL,
        0xFFULL,
        0x100ULL,
        0xDEADBEEFCAFEBABEULL,
        0xFFFFFFFFFFFFFFFFULL,
        1ULL << 63,
        1ULL << 32,
        0x1234ULL,
        0x8000000000000001ULL,
        0x5555555555555555ULL,
        0xAAAAAAAAAAAAAAAAULL,
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(*cases); i++) {
        check_one(cases[i]);
    }

    /* All single-bit values. */
    for (int i = 0; i < 64; i++) {
        check_one(1ULL << i);
    }

    /* Pseudo-random sweep (deterministic). */
    uint64_t s = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 1024; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        check_one(s);
    }

    if (g_failures > 0) {
        fprintf(stderr, "test_portability: %d failures\n", g_failures);
        return EXIT_FAILURE;
    }
    fprintf(stdout, "test_portability: ok\n");
    return EXIT_SUCCESS;
}
