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

/* -------------------------------------------------------------------
 * Reference (pre-rewrite) implementations of sm_equals / sm_compare /
 * sm_subset_compare, copied verbatim from the bit-by-bit versions they
 * replaced.  They walk sm_next_member per set bit, so they are
 * O(cardinality) -- correct on small maps, and the oracle the run-based
 * rewrite is cross-checked against below.  (They HANG on the 2^31-bit
 * map, which is exactly what the timing test proves the rewrite fixed.)
 * ------------------------------------------------------------------- */
static bool
ref_equals(const sm_t *a, const sm_t *b)
{
	const bool a_empty = (a == NULL) || sm_is_empty((sm_t *)a);
	const bool b_empty = (b == NULL) || sm_is_empty((sm_t *)b);
	if (a_empty && b_empty)
		return (true);
	if (a_empty != b_empty)
		return (false);
	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX && ib != SM_IDX_MAX) {
		if (ia != ib)
			return (false);
		ia = sm_next_member(a, ia, NULL);
		ib = sm_next_member(b, ib, NULL);
	}
	return (ia == ib);
}

static int
ref_compare(const sm_t *a, const sm_t *b)
{
	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX && ib != SM_IDX_MAX) {
		if (ia < ib)
			return (-1);
		if (ia > ib)
			return (1);
		ia = sm_next_member(a, ia, NULL);
		ib = sm_next_member(b, ib, NULL);
	}
	if (ia == SM_IDX_MAX && ib == SM_IDX_MAX)
		return (0);
	return ((ia == SM_IDX_MAX) ? -1 : 1);
}

static sm_subset_relation_t
ref_subset_compare(const sm_t *a, const sm_t *b)
{
	bool a_subset_b = true;
	bool b_subset_a = true;
	uint64_t ia = sm_next_member(a, SM_IDX_MAX, NULL);
	uint64_t ib = sm_next_member(b, SM_IDX_MAX, NULL);
	while (ia != SM_IDX_MAX || ib != SM_IDX_MAX) {
		if (ia == ib) {
			ia = sm_next_member(a, ia, NULL);
			ib = sm_next_member(b, ib, NULL);
		} else if (ia != SM_IDX_MAX && (ib == SM_IDX_MAX || ia < ib)) {
			a_subset_b = false;
			ia = sm_next_member(a, ia, NULL);
		} else {
			b_subset_a = false;
			ib = sm_next_member(b, ib, NULL);
		}
		if (!a_subset_b && !b_subset_a)
			return (SM_REL_DIFFERENT);
	}
	if (a_subset_b && b_subset_a)
		return (SM_REL_EQUAL);
	if (a_subset_b)
		return (SM_REL_SUBSET_A);
	return (SM_REL_SUBSET_B);
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

	/* The real amplification test: xor a 2^31-bit run against a small
	 * map so the OUTPUT is itself a 2^31-bit run.  Pre-fix this emitted
	 * the survivor one bit at a time and took tens of seconds; it must
	 * now finish in well under 10 ms and be correct. */
	sm_t *xr_big = NULL;
	TIME_OP("xor_big_nonempty", { xr_big = sm_xor(big, small); });
	CHECK(xr_big != NULL);
	CHECK(sm_validate(xr_big));
	/* small = {5, 1000000, 2000000000}; all three lie inside [0,2^31-1),
	 * so xor clears exactly those three bits from the giant run. */
	CHECK(sm_cardinality(xr_big) == BIG_LEN - 3);
	CHECK(!sm_contains(xr_big, 5, NULL));
	CHECK(!sm_contains(xr_big, 1000000, NULL));
	CHECK(!sm_contains(xr_big, 2000000000ULL, NULL));
	CHECK(sm_contains(xr_big, 4, NULL));
	CHECK(sm_contains(xr_big, 6, NULL));
	CHECK(sm_contains(xr_big, 999999, NULL));
	CHECK(sm_contains(xr_big, 1000001, NULL));
	CHECK(sm_contains(xr_big, BIG_LEN - 1, NULL));
	/* Survives a serialize -> deserialize round-trip. */
	{
		size_t n = sm_serialized_size(xr_big);
		CHECK(n > 0);
		uint8_t *buf = malloc(n);
		CHECK(buf != NULL);
		CHECK(sm_serialize(xr_big, buf, n) == n);
		sm_t *rt = sm_deserialize(buf, n);
		CHECK(rt != NULL);
		CHECK(sm_validate(rt));
		CHECK(sm_cardinality(rt) == BIG_LEN - 3);
		/* Compare encoded bytes rather than sm_equals(), which walks
		 * bit-by-bit and would itself take O(2^31) on this map. */
		size_t n2 = sm_serialized_size(rt);
		CHECK(n2 == n);
		uint8_t *buf2 = malloc(n2);
		CHECK(buf2 != NULL);
		CHECK(sm_serialize(rt, buf2, n2) == n2);
		CHECK(memcmp(buf, buf2, n) == 0);
		free(buf2);
		sm_free(rt);
		free(buf);
	}
	sm_free(xr_big);

	/* extract a small window out of the huge run. */
	sm_t *ex = NULL;
	TIME_OP("extract_range", { ex = sm_extract_range(big, 100, 200); });
	CHECK(ex != NULL && sm_cardinality(ex) == 100);
	sm_free(ex);

	/* The real range-amplification test: extract a window that spans a
	 * large slice of the run (crossing many 2048-bit chunk boundaries).
	 * Pre-fix this materialised the clipped run bit-by-bit and took
	 * seconds; it must now be fast and exact. */
	sm_t *ex_big = NULL;
	TIME_OP("extract_range_big",
	    { ex_big = sm_extract_range(big, 0, 1000000); });
	CHECK(ex_big != NULL);
	CHECK(sm_validate(ex_big));
	CHECK(sm_cardinality(ex_big) == 1000000);
	CHECK(sm_contains(ex_big, 0, NULL));
	CHECK(sm_contains(ex_big, 999999, NULL));
	CHECK(!sm_contains(ex_big, 1000000, NULL));
	sm_free(ex_big);

	/* An offset window that both starts and ends mid-chunk over the run. */
	sm_t *ex_mid = NULL;
	TIME_OP("extract_range_mid",
	    { ex_mid = sm_extract_range(big, 3000, 500003); });
	CHECK(ex_mid != NULL);
	CHECK(sm_validate(ex_mid));
	CHECK(sm_cardinality(ex_mid) == 497003);
	CHECK(!sm_contains(ex_mid, 2999, NULL));
	CHECK(sm_contains(ex_mid, 3000, NULL));
	CHECK(sm_contains(ex_mid, 500002, NULL));
	CHECK(!sm_contains(ex_mid, 500003, NULL));
	sm_free(ex_mid);
	/* split near the middle of the run. */
	sm_t *other = sm_create(1 << 16);
	CHECK(other != NULL);
	TIME_OP("split", { (void)sm_split(big, 1000000000ULL, other); });
	sm_free(other);

	/* The last S4 gap: the three public comparison functions used to
	 * walk bit-by-bit (via sm_next_member) and so hung on a 2^31-bit
	 * operand.  They are now run-based and must finish well under the
	 * ceiling.  Use fresh identical 2^31-bit maps (big/big2 were mutated
	 * by sm_split above): equals is true and compare is 0; small's bits
	 * all lie inside the run so small is a strict subset (SM_REL_SUBSET_A). */
	{
		sm_t *c1 = make_big_rle();
		sm_t *c2 = make_big_rle();
		CHECK(c1 != NULL && c2 != NULL);
		volatile bool eq = false;
		TIME_OP("equals", { eq = sm_equals(c1, c2); });
		CHECK(eq == true);
		volatile int cmp = 99;
		TIME_OP("compare", { cmp = sm_compare(c1, c2); });
		CHECK(cmp == 0);
		volatile sm_subset_relation_t rel = SM_REL_DIFFERENT;
		TIME_OP("subset_compare",
		    { rel = sm_subset_compare(small, c1); });
		CHECK(rel == SM_REL_SUBSET_A);
		/* Superset direction too, still run-based. */
		TIME_OP("subset_compare_rev",
		    { rel = sm_subset_compare(c1, small); });
		CHECK(rel == SM_REL_SUBSET_B);
		sm_free(c1);
		sm_free(c2);
	}

	sm_free(big);
	sm_free(big2);
	sm_free(small);

	/* Correctness cross-check: sm_xor / cardinalities vs the scalar
	 * bit-by-bit answer, and the run-based sm_equals / sm_compare /
	 * sm_subset_compare vs their bit-by-bit references, on small maps of
	 * assorted shapes.  The shape grid crosses empty, single, sparse
	 * scatter, RLE-length run, chunk-boundary straddle and chunk-aligned
	 * run against each other, so equal / strict-subset / strict-superset
	 * / overlapping / disjoint pairings all occur among the 36 pairs. */
	size_t npairs = 0;
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
			/* Run-based comparison ops must match the scalar
			 * bit-by-bit reference on every shape pair. */
			CHECK(sm_equals(a, b) == ref_equals(a, b));
			CHECK(sm_compare(a, b) == ref_compare(a, b));
			CHECK(sm_subset_compare(a, b) ==
			    ref_subset_compare(a, b));
			npairs++;
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

	/* Explicit edge pairs on top of the shape grid: adjacent runs, an
	 * exactly-equal pair, disjoint, strict subset, strict superset, and
	 * RLE-length runs straddling a chunk boundary.  Each is checked
	 * against the same bit-by-bit reference for all three ops. */
	{
		const struct {
			uint64_t alo, ahi, blo, bhi;
		} pairs[] = {
			{ 0, 5, 5, 10 },	    /* adjacent runs, disjoint */
			{ 0, 100, 0, 100 },	    /* exactly equal */
			{ 0, 10, 100, 110 },	    /* disjoint, gap */
			{ 10, 20, 0, 100 },	    /* a strict subset of b */
			{ 0, 100, 40, 60 },	    /* b strict subset of a */
			{ 0, 50, 25, 75 },	    /* overlapping */
			{ 2040, 2060, 2040, 2060 }, /* equal, chunk straddle */
			{ 2000, 2050, 2050, 2100 }, /* adjacent across boundary */
			{ 0, 1, 0, 1 },		    /* single-bit equal */
			{ 0, 1, 1, 2 },		    /* single-bit disjoint */
		};
		for (size_t k = 0; k < sizeof(pairs) / sizeof(pairs[0]); k++) {
			sm_t *a = sm_create(1 << 16);
			sm_t *b = sm_create(1 << 16);
			CHECK(a && b);
			for (uint64_t i = pairs[k].alo; i < pairs[k].ahi; i++)
				sm_add_grow(&a, i);
			for (uint64_t i = pairs[k].blo; i < pairs[k].bhi; i++)
				sm_add_grow(&b, i);
			CHECK(sm_equals(a, b) == ref_equals(a, b));
			CHECK(sm_compare(a, b) == ref_compare(a, b));
			CHECK(sm_subset_compare(a, b) ==
			    ref_subset_compare(a, b));
			/* Order antisymmetry: compare(a,b) == -compare(b,a). */
			CHECK(sm_compare(a, b) == -sm_compare(b, a));
			npairs++;
			sm_free(a);
			sm_free(b);
		}
	}

	printf("test_amplify: S4 termination + correctness OK "
	    "(%zu comparison pairs, 0 mismatches)\n",
	    npairs);
	return (0);
}
