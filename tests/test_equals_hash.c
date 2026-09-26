/* SPDX-License-Identifier: MIT */
/*
 * test_equals_hash.c -- sm_equals / sm_hash / sm_compare must treat
 * logically-equal sets as equal REGARDLESS of how they were built.
 *
 * Root bug (pre-existing, also on 5.6.0 main): the maximal-run iterator
 * __sm_run_next decomposed runs PER CHUNK and did not coalesce a run
 * that ended exactly at a 2048-aligned chunk boundary with the run that
 * began at the next chunk's start.  A contiguous range stored ACROSS a
 * chunk seam -- which sm_union produces when it stitches two split
 * halves back together, and which an RLE run abutting the next chunk
 * also produces -- decomposed into TWO runs ([.,2048)+[2048,.)) whereas
 * the same set built by one sm_add_range decomposed into ONE run.
 * sm_equals and sm_hash both walk that iterator and DOCUMENT that "two
 * equal maps decompose into the identical maximal run sequence", so
 * they returned "not equal" / different hashes for equal sets --
 * violating the fundamental contract that equal sets are equal and hash
 * the same.
 *
 * The fix makes __sm_run_next yield a truly maximal, canonical run
 * decomposition (coalescing across the per-chunk seam and across chunk
 * boundaries), so every construction of the same set decomposes
 * identically.  These cases FAIL on the pre-fix library (equals==0,
 * hashes differ) and pass after.
 */
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                              \
	do {                                                                  \
		if (!(c)) {                                                    \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
			    __LINE__, #c);                                    \
			return (1);                                           \
		}                                                             \
	} while (0)

/* Assert two maps are logically equal AND hash-equal AND compare-equal
 * AND subset-EQUAL, in both argument orders. */
#define SAME(a, b)                                                            \
	do {                                                                  \
		CHECK(sm_equals((a), (b)));                                    \
		CHECK(sm_equals((b), (a)));                                    \
		CHECK(sm_hash(a) == sm_hash(b));                               \
		CHECK(sm_compare((a), (b)) == 0);                             \
		CHECK(sm_subset_compare((a), (b)) == SM_REL_EQUAL);          \
	} while (0)

/* Build [lo, hi) via individual sm_add_grow calls. */
static sm_t *
build_adds(uint64_t lo, uint64_t hi)
{
	sm_t *m = sm_create(1 << 16);
	if (m == NULL)
		return (NULL);
	for (uint64_t i = lo; i < hi; i++)
		if (sm_add_grow(&m, i) == SM_IDX_MAX) {
			sm_free(m);
			return (NULL);
		}
	return (m);
}

int
main(void)
{
	/* --- The reported case: split then union rebuilds A, and it must
	 * be equal to (and hash like) the freshly-built A. --------------- */
	{
		sm_t *A = build_adds(0, 5000);
		CHECK(A != NULL);
		sm_t *acopy = sm_copy(A);
		sm_t *o = sm_create(1 << 20);
		CHECK(acopy != NULL && o != NULL);
		CHECK(sm_split(acopy, 2000, o) != SM_IDX_MAX);
		/* left = [0,2000), o = [2000,5000): a contiguous range now
		 * lives across the 2048 chunk seam once re-stitched. */
		sm_t *U = sm_union(acopy, o);
		CHECK(U != NULL);
		CHECK(sm_cardinality(U) == sm_cardinality(A));
		SAME(U, A);
		sm_free(A);
		sm_free(acopy);
		sm_free(o);
		sm_free(U);
	}

	/* --- add_range vs individual adds vs split+union, all == [0,6000).
	 * [0,6000) spans three chunk windows, so the seam is exercised
	 * twice. --------------------------------------------------------- */
	{
		sm_t *by_adds = build_adds(0, 6000);
		sm_t *by_range = sm_create(1 << 16);
		CHECK(by_adds != NULL && by_range != NULL);
		CHECK(sm_add_range(by_range, 0, 6000));

		sm_t *left = sm_copy(by_adds);
		sm_t *right = sm_create(1 << 20);
		CHECK(left != NULL && right != NULL);
		CHECK(sm_split(left, 3000, right) != SM_IDX_MAX);
		sm_t *by_su = sm_union(left, right);
		CHECK(by_su != NULL);

		SAME(by_adds, by_range);
		SAME(by_adds, by_su);
		SAME(by_range, by_su);
		sm_free(by_adds);
		sm_free(by_range);
		sm_free(left);
		sm_free(right);
		sm_free(by_su);
	}

	/* --- A range whose END lands EXACTLY on a chunk boundary (2048),
	 * abutting the next chunk which starts at 2048.  Build it as one
	 * range, and as two ranges [0,2048)+[2048,4096); they must agree. */
	{
		sm_t *whole = sm_create(1 << 16);
		sm_t *halves = sm_create(1 << 16);
		CHECK(whole != NULL && halves != NULL);
		CHECK(sm_add_range(whole, 0, 4096));
		CHECK(sm_add_range(halves, 0, 2048));
		CHECK(sm_add_range(halves, 2048, 4096));
		SAME(whole, halves);
		sm_free(whole);
		sm_free(halves);
	}

	/* --- Set-op result equals the direct construction.  (A|B) where A,
	 * B abut at a chunk seam must equal the one contiguous range. ----- */
	{
		sm_t *a = sm_create(1 << 16);
		sm_t *b = sm_create(1 << 16);
		CHECK(a != NULL && b != NULL);
		CHECK(sm_add_range(a, 1000, 2048)); /* ends on the seam */
		CHECK(sm_add_range(b, 2048, 3500)); /* starts on the seam */
		sm_t *u = sm_union(a, b);
		sm_t *direct = sm_create(1 << 16);
		CHECK(u != NULL && direct != NULL);
		CHECK(sm_add_range(direct, 1000, 3500));
		SAME(u, direct);
		sm_free(a);
		sm_free(b);
		sm_free(u);
		sm_free(direct);
	}

	/* --- UNEQUAL sets must compare unequal and (here) hash apart. --- */
	{
		sm_t *x = build_adds(0, 5000);
		sm_t *y = build_adds(0, 5000);
		CHECK(x != NULL && y != NULL);
		CHECK(sm_remove(y, 2048) != SM_IDX_MAX); /* drop one seam bit */
		CHECK(!sm_equals(x, y));
		CHECK(sm_hash(x) != sm_hash(y));
		CHECK(sm_compare(x, y) != 0);
		CHECK(sm_subset_compare(x, y) == SM_REL_SUBSET_B); /* y ⊂ x */
		sm_free(x);
		sm_free(y);
	}

	printf("test_equals_hash: split+union == add_range, seam-coalescing "
	       "OK\n");
	return (0);
}
