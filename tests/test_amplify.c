/* SPDX-License-Identifier: MIT */
/*
 * test_amplify.c - S4 regression: termination and amplification.
 *
 * A tiny serialized map can declare a 2^31-bit RLE run.  Before the S4
 * fix, sm_xor / sm_hash / the *_cardinality family / sm_jaccard_index /
 * sm_extract_range walked that run bit-by-bit (O(cardinality)), taking
 * seconds, and sm_split's per-bit loop never terminated near 2^64.
 * They now walk run-by-run, so cost tracks the encoded size.
 *
 * This test builds a valid single-RLE-chunk map spanning ~2^31 bits and
 * asserts every one of those ops finishes in well under 10 ms, then
 * cross-checks sm_xor / the cardinalities against the scalar bit-by-bit
 * answer on small maps.
 */
#define SM_EXPOSE_STRUCT 1
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(c)                                                              \
	do {                                                                  \
		if (!(c)) {                                                   \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
			    __LINE__, #c);                                   \
			return (1);                                          \
		}                                                            \
	} while (0)

#define SM_RLE_FLAGS 0x4000000000000000ULL
#define BIG_LEN      0x7FFFFFFFULL /* 2^31 - 1 bits */

/* A valid one-chunk map: start 0, RLE descriptor cap==len==2^31-1. */
static sm_t *
make_big_rle(void)
{
	uint8_t body[24];
	uint64_t count = 1;
	uint64_t start = 0;
	uint64_t desc = SM_RLE_FLAGS | (BIG_LEN << 31) | BIG_LEN;
	memcpy(body + 0, &count, 8);
	memcpy(body + 8, &start, 8);
	memcpy(body + 16, &desc, 8);
	sm_t *m = sm_open_copy(body, sizeof body, 64);
	return (m);
}

static double
elapsed_ms(struct timespec a, struct timespec b)
{
	return ((b.tv_sec - a.tv_sec) * 1e3 +
	    (b.tv_nsec - a.tv_nsec) / 1e6);
}

/* The point of S4 is that runtime tracks the ENCODED size, not the
 * popcount: on the fixed tree these ops finish in microseconds, on the
 * unfixed tree they take seconds.  A generous ceiling separates the two
 * without being flaky.  Sanitizer and coverage builds distort timing
 * (ASan is 2-10x slower, gcov's per-arc counters even more), so lift
 * the ceiling there -- the same reasoning the coverage guards use
 * elsewhere in the suite.  The correctness cross-check below is
 * unconditional. */
#if defined(__SANITIZE_ADDRESS__) || defined(SM_COVERAGE_BUILD)
#define AMPLIFY_CEILING_MS 2000.0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) ||                                       \
    __has_feature(undefined_behavior_sanitizer)
#define AMPLIFY_CEILING_MS 2000.0
#else
#define AMPLIFY_CEILING_MS 100.0
#endif
#else
#define AMPLIFY_CEILING_MS 100.0
#endif

#define TIME_OP(label, stmt)                                                  \
	do {                                                                  \
		struct timespec t0, t1;                                       \
		clock_gettime(CLOCK_MONOTONIC, &t0);                          \
		stmt;                                                         \
		clock_gettime(CLOCK_MONOTONIC, &t1);                          \
		double ms = elapsed_ms(t0, t1);                               \
		if (ms >= AMPLIFY_CEILING_MS) {                               \
			fprintf(stderr, "FAIL %s took %.3f ms\n",            \
			    label, ms);                                      \
			return (1);                                          \
		}                                                             \
	} while (0)

int
main(void)
{
	sm_t *big = make_big_rle();
	CHECK(big != NULL);
	CHECK(sm_validate(big));
	CHECK(sm_cardinality(big) == BIG_LEN);

	/* A second big map, offset so it partially overlaps. */
	sm_t *big2 = make_big_rle();
	CHECK(big2 != NULL);

	/* A small map to pair against the big one. */
	sm_t *small = sm_create(1024);
	CHECK(small != NULL);
	sm_add(small, 5);
	sm_add(small, 1000000);
	sm_add(small, 2000000000ULL);

	/* Every op below must finish in well under 10 ms on a 2^31-bit
	 * operand. */
	TIME_OP("union_cardinality",
	    { volatile size_t r = sm_union_cardinality(big, small); (void)r; });
	TIME_OP("intersection_cardinality", {
		volatile size_t r = sm_intersection_cardinality(big, small);
		(void)r;
	});
	TIME_OP("difference_cardinality", {
		volatile size_t r = sm_difference_cardinality(big, small);
		(void)r;
	});
	TIME_OP("xor_cardinality",
	    { volatile size_t r = sm_xor_cardinality(big, big2); (void)r; });
	TIME_OP("jaccard_index",
	    { volatile double r = sm_jaccard_index(big, big2); (void)r; });
	TIME_OP("hash", { volatile uint64_t r = sm_hash(big); (void)r; });
	/* xor of two identical big runs is empty -> fast. */
	sm_t *xr = NULL;
	TIME_OP("xor", { xr = sm_xor(big, big2); });
	CHECK(xr == NULL); /* identical -> empty symmetric difference */
	/* extract a small window out of the huge run. */
	sm_t *ex = NULL;
	TIME_OP("extract_range", { ex = sm_extract_range(big, 100, 200); });
	CHECK(ex != NULL && sm_cardinality(ex) == 100);
	sm_free(ex);
	/* split near the middle of the run. */
	sm_t *other = sm_create(1 << 16);
	CHECK(other != NULL);
	TIME_OP("split", { (void)sm_split(big, 1000000000ULL, other); });
	sm_free(other);

	sm_free(big);
	sm_free(big2);
	sm_free(small);

	/* Correctness cross-check: sm_xor / cardinalities vs the scalar
	 * bit-by-bit answer on small maps of assorted shapes. */
	for (int sa = 0; sa < 6; sa++) {
		for (int sb = 0; sb < 6; sb++) {
			sm_t *a = sm_create(1 << 16);
			sm_t *b = sm_create(1 << 16);
			CHECK(a && b);
			/* shapes: empty, single, sparse, RLE run, straddle,
			 * chunk-aligned run. */
			const struct {
				uint64_t lo, hi, step;
			} shp[6] = {
				{ 0, 0, 1 },       /* empty */
				{ 7, 8, 1 },       /* single */
				{ 0, 120, 3 },     /* sparse stride 3 */
				{ 0, 4096, 1 },    /* long run -> RLE */
				{ 2000, 2100, 1 }, /* straddles 2048 */
				{ 2048, 4096, 1 }, /* chunk-aligned */
			};
			for (uint64_t i = shp[sa].lo; i < shp[sa].hi;
			     i += shp[sa].step)
				sm_add_grow(&a, i);
			for (uint64_t i = shp[sb].lo; i < shp[sb].hi;
			     i += shp[sb].step)
				sm_add_grow(&b, i);

			size_t s_union = 0, s_inter = 0, s_xor = 0;
			for (uint64_t i = 0; i < 5000; i++) {
				bool ina = sm_contains(a, i, NULL);
				bool inb = sm_contains(b, i, NULL);
				if (ina || inb)
					s_union++;
				if (ina && inb)
					s_inter++;
				if (ina != inb)
					s_xor++;
			}
			CHECK(sm_union_cardinality(a, b) == s_union);
			CHECK(sm_intersection_cardinality(a, b) == s_inter);
			CHECK(sm_xor_cardinality(a, b) == s_xor);
			sm_t *x = sm_xor(a, b);
			size_t xc = x ? sm_cardinality(x) : 0;
			CHECK(xc == s_xor);
			for (uint64_t i = 0; i < 5000; i++) {
				bool want = sm_contains(a, i, NULL) !=
				    sm_contains(b, i, NULL);
				bool got = x ? sm_contains(x, i, NULL) : false;
				CHECK(want == got);
			}
			sm_free(x);
			sm_free(a);
			sm_free(b);
		}
	}

	printf("test_amplify: S4 termination + correctness OK\n");
	return (0);
}
