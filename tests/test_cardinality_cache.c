/* SPDX-License-Identifier: MIT
 *
 * Regression test for the lazy cardinality cache (Option B: runtime-only
 * m_card_plus1 field, wire format untouched).
 *
 * The cache turns sm_cardinality() from an O(chunks) popcount walk into
 * an O(1) read by caching the count and invalidating it on every
 * membership-changing operation.  The failure mode this guards against
 * is a STALE cache: a mutation that forgets to invalidate leaves
 * sm_cardinality() returning the pre-mutation count.
 *
 * Every case here mutates the map through a public API and then asserts
 * sm_cardinality() == an INDEPENDENT recount via sm_rank(0, IDX_MAX,
 * true), which walks the map and never touches the cache.  A stale
 * cache makes the two disagree.
 *
 * Revert-to-fail: delete any __sm_card_invalidate() call in the
 * corresponding sm.c path (e.g. sm_remove) and the matching case below
 * fails -- confirmed on the box before commit.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sm.h>

static int g_failures = 0;
static int g_total = 0;

#define EXPECT(cond, msg)                                                 \
	do {                                                              \
		g_total++;                                                \
		if (!(cond)) {                                            \
			fprintf(stderr, "  FAIL: %s:%d: %s -- %s\n",      \
			    __FILE__, __LINE__, msg, #cond);              \
			g_failures++;                                     \
			return 1;                                         \
		}                                                         \
	} while (0)

#define RUN(name)                                                         \
	do {                                                              \
		const int before = g_failures;                           \
		fprintf(stderr, "  case %s ... ", #name);                 \
		(void)name();                                             \
		fprintf(stderr, g_failures == before ? "ok\n" : "FAILED\n"); \
	} while (0)

/* Independent ground-truth recount.  sm_rank walks the map and never
 * reads or writes the cache, so it cannot be fooled by a stale cache. */
static size_t
recount(sm_t *m)
{
	return (sm_rank(m, 0, SM_IDX_MAX, true));
}

/* The core check: cached count must equal the independent walk. */
#define CHECK_CACHE(m, tag)                                               \
	EXPECT(sm_cardinality(m) == recount(m), tag)

/* --- add / remove single bits, small and chunk mode --- */
static int
test_add_remove(void)
{
	sm_t *m = sm_create(64);
	uint64_t i;
	EXPECT(m != NULL, "create");
	CHECK_CACHE(m, "fresh empty");

	/* small mode */
	for (i = 0; i < 50; i++) {
		sm_add_grow(&m, i);
		CHECK_CACHE(m, "small add");
	}
	/* re-add already-set (must not change count) */
	sm_add_grow(&m, 10);
	CHECK_CACHE(m, "readd already-set");

	/* remove present + absent */
	sm_remove(m, 10);
	CHECK_CACHE(m, "remove present");
	sm_remove(m, 99999); /* absent */
	CHECK_CACHE(m, "remove absent");

	/* chunk mode: spread indices */
	for (i = 0; i < 5000; i++) {
		sm_add_grow(&m, i * 37 + 1);
		if (i % 250 == 0)
			CHECK_CACHE(m, "chunk add");
	}
	CHECK_CACHE(m, "chunk after bulk");

	for (i = 0; i < 2000; i++) {
		sm_remove(m, i * 37 + 1);
		if (i % 250 == 0)
			CHECK_CACHE(m, "chunk remove");
	}
	CHECK_CACHE(m, "chunk after bulk remove");
	sm_free(m);
	return 0;
}

/* --- ranges --- */
static int
test_ranges(void)
{
	sm_t *m = sm_create(64);
	EXPECT(m != NULL, "create");

	sm_add_range(m, 100, 5000);
	CHECK_CACHE(m, "add_range");
	/* overlapping add_range across existing bits */
	sm_add_range(m, 4000, 9000);
	CHECK_CACHE(m, "add_range overlap");

	sm_remove_range(m, 200, 300);
	CHECK_CACHE(m, "remove_range");

	sm_flip_range(m, 0, 12000);
	CHECK_CACHE(m, "flip_range");
	sm_free(m);
	return 0;
}

/* --- clear --- */
static int
test_clear(void)
{
	sm_t *m = sm_create(64);
	EXPECT(m != NULL, "create");
	sm_add_range(m, 0, 3000);
	CHECK_CACHE(m, "before clear");
	sm_clear(m);
	CHECK_CACHE(m, "after clear");
	EXPECT(sm_cardinality(m) == 0, "cleared cardinality 0");
	sm_add_grow(&m, 42);
	CHECK_CACHE(m, "add after clear");
	sm_free(m);
	return 0;
}

/* --- copy, serialize/deserialize --- */
static int
test_copy_serialize(void)
{
	sm_t *m = sm_create(64);
	sm_t *c;
	sm_t *d;
	size_t need;
	uint8_t *buf;
	EXPECT(m != NULL, "create");
	sm_add_range(m, 0, 6000);
	(void)sm_cardinality(m); /* prime the source cache */

	c = sm_copy(m);
	EXPECT(c != NULL, "copy");
	CHECK_CACHE(c, "copy cardinality");
	EXPECT(sm_cardinality(c) == sm_cardinality(m), "copy == src");

	need = sm_serialized_size(m);
	buf = (uint8_t *)malloc(need);
	EXPECT(buf != NULL, "malloc");
	EXPECT(sm_serialize(m, buf, need) == need, "serialize");
	d = sm_deserialize(buf, need);
	EXPECT(d != NULL, "deserialize");
	CHECK_CACHE(d, "deserialize cardinality");
	EXPECT(sm_cardinality(d) == sm_cardinality(m), "deser == src");

	free(buf);
	sm_free(m);
	sm_free(c);
	sm_free(d);
	return 0;
}

/* --- split: both halves --- */
static int
test_split(void)
{
	sm_t *m = sm_create(1 << 16);
	sm_t *other = sm_create(1 << 16);
	size_t total;
	EXPECT(m != NULL && other != NULL, "create");
	sm_add_range(m, 0, 8000);
	total = sm_cardinality(m);
	EXPECT(sm_split(m, SM_IDX_MAX, other) != SM_IDX_MAX, "split");
	CHECK_CACHE(m, "split left half");
	CHECK_CACHE(other, "split right half");
	EXPECT(sm_cardinality(m) + sm_cardinality(other) == total,
	    "halves sum to total");
	sm_free(m);
	sm_free(other);
	return 0;
}

/* --- set ops: results and in-place --- */
static int
test_setops(void)
{
	sm_t *A = sm_create(4096);
	sm_t *B = sm_create(4096);
	sm_t *U;
	sm_t *I;
	sm_t *D;
	sm_t *X;
	sm_t *Ai;
	EXPECT(A != NULL && B != NULL, "create");
	sm_add_range(A, 0, 4000);
	sm_add_range(B, 2000, 6000);

	U = sm_union(A, B);
	I = sm_intersection(A, B);
	D = sm_difference(A, B);
	X = sm_xor(A, B);
	EXPECT(U && I && D && X, "setops built");
	CHECK_CACHE(U, "union card");
	CHECK_CACHE(I, "intersection card");
	CHECK_CACHE(D, "difference card");
	CHECK_CACHE(X, "xor card");
	EXPECT(sm_union_cardinality(A, B) == sm_cardinality(U), "union_card");
	EXPECT(sm_intersection_cardinality(A, B) == sm_cardinality(I),
	    "inter_card");
	EXPECT(sm_difference_cardinality(A, B) == sm_cardinality(D),
	    "diff_card");
	EXPECT(sm_xor_cardinality(A, B) == sm_cardinality(X), "xor_card");

	/* in-place must invalidate dst */
	Ai = sm_copy(A);
	EXPECT(Ai != NULL, "copy for inplace");
	(void)sm_cardinality(Ai); /* prime cache to the pre-op value */
	Ai = sm_union_inplace(Ai, B);
	EXPECT(Ai != NULL, "union_inplace");
	CHECK_CACHE(Ai, "union_inplace card");
	EXPECT(sm_cardinality(Ai) == sm_cardinality(U), "inplace == union");

	sm_free(A);
	sm_free(B);
	sm_free(U);
	sm_free(I);
	sm_free(D);
	sm_free(X);
	sm_free(Ai);
	return 0;
}

/* --- the direct stale-cache trap: prime, mutate, re-query ---
 * This is the minimal case a broken invalidation would fail. */
static int
test_stale_trap(void)
{
	/* Generous capacity so a clear (ONES->MIXED needs space) cannot
	 * ENOSPC -- otherwise sm_remove would legitimately leave the bit
	 * set and this would test the harness, not the cache. */
	sm_t *m = sm_create(1 << 16);
	EXPECT(m != NULL, "create");
	sm_add_range(m, 0, 1000);
	EXPECT(sm_cardinality(m) == 1000, "primed count 1000");
	/* mutate: a stale cache would still say 1000 here */
	sm_add_grow(&m, 100000);
	EXPECT(sm_cardinality(m) == 1001, "count after add == 1001");
	EXPECT(sm_cardinality(m) == recount(m), "cached == recount after add");
	EXPECT(sm_remove(m, 100000) != SM_IDX_MAX, "remove succeeds");
	EXPECT(sm_cardinality(m) == 1000, "count after remove == 1000");
	EXPECT(sm_cardinality(m) == recount(m),
	    "cached == recount after remove");
	sm_free(m);
	return 0;
}

int
main(void)
{
	fprintf(stderr, "test_cardinality_cache:\n");
	RUN(test_stale_trap);
	RUN(test_add_remove);
	RUN(test_ranges);
	RUN(test_clear);
	RUN(test_copy_serialize);
	RUN(test_split);
	RUN(test_setops);
	fprintf(stderr, "  %d/%d expectations passed, %d failures\n",
	    g_total - g_failures, g_total, g_failures);
	return (g_failures == 0 ? 0 : 1);
}
