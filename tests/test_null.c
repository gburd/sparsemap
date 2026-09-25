/* SPDX-License-Identifier: MIT */
/*
 * test_null.c - S5 regression: the NULL-map contract.
 *
 * A NULL map pointer is an empty, read-only map.  Every public sm_*
 * function must accept NULL without crashing: readers return the
 * empty-map value, mutators perform no action and return their
 * documented failure value with errno == EINVAL.
 *
 * Before the S5 fix, 19 of the 69 public functions segfaulted on a NULL
 * map.  This test calls every one with NULL and asserts the contract;
 * without the fix it crashes on the first offender.
 */
#include <sm.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(c)                                                              \
	do {                                                                  \
		if (!(c)) {                                                   \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
			    __LINE__, #c);                                   \
			failures++;                                          \
		}                                                            \
	} while (0)

/* A mutator on NULL must return its failure value AND set errno=EINVAL. */
#define CHECK_EINVAL(expr, want)                                              \
	do {                                                                  \
		errno = 0;                                                    \
		CHECK((expr) == (want));                                      \
		CHECK(errno == EINVAL);                                       \
	} while (0)

int
main(void)
{
	/* --- Lifecycle / buffer management (mutators / void). --- */
	sm_free(NULL);            /* documented no-op */
	sm_clear(NULL);           /* no-op */
	sm_init(NULL, NULL, 1);   /* no-op */
	sm_open(NULL, NULL, 1);   /* no-op */
	sm_statistics(NULL, NULL); /* no-op */

	errno = 0;
	CHECK(sm_copy(NULL) == NULL);
	CHECK(errno == EINVAL);
	CHECK(sm_owned_copy(NULL) == NULL);
	CHECK(sm_set_data_size(NULL, NULL, 1) == NULL);
	CHECK(sm_shrink_to_fit(NULL) == NULL);

	/* --- Read-only queries: empty-map values. --- */
	CHECK(sm_capacity_remaining(NULL) == 0.0);
	CHECK(sm_get_capacity(NULL) == 0);
	CHECK(sm_get_size(NULL) == 0);
	CHECK(sm_get_data(NULL) == NULL);
	CHECK(sm_cardinality(NULL) == 0);
	CHECK(sm_minimum(NULL) == 0);
	CHECK(sm_maximum(NULL) == 0);
	CHECK(sm_fill_factor(NULL) == 0.0);
	CHECK(sm_is_empty(NULL) == true);
	CHECK(sm_contains(NULL, 1, NULL) == false);
	CHECK(sm_contains_cached(NULL, 1, NULL) == false);
	CHECK(sm_validate(NULL) == true); /* NULL is a valid (empty) map */
	CHECK(sm_hash(NULL) == sm_hash(NULL)); /* stable, no crash */
	CHECK(sm_serialized_size(NULL) > 0);
	CHECK(sm_serialize(NULL, NULL, 1) == 0);

	/* sm_contains_many: NULL map fills results with false; NULL
	 * results/idxs is rejected. */
	{
		uint64_t idxs[3] = { 1, 2, 3 };
		bool res[3] = { true, true, true };
		sm_contains_many(NULL, idxs, res, 3);
		CHECK(res[0] == false && res[1] == false && res[2] == false);
		errno = 0;
		sm_contains_many(NULL, NULL, NULL, 3);
		CHECK(errno == EINVAL);
	}

	/* rank / select / span. */
	CHECK(sm_rank(NULL, 0, SM_IDX_MAX, true) == 0);
	CHECK(sm_select(NULL, 0, true) == SM_IDX_MAX);
	CHECK(sm_select(NULL, 5, false) == 5); /* n-th unset bit of empty */
	CHECK(sm_span(NULL, 1, 1, true) == SM_IDX_MAX);

	/* membership / iteration / singleton. */
	CHECK(sm_membership(NULL) == SM_EMPTY);
	CHECK(sm_singleton_member(NULL) == SM_IDX_MAX);
	CHECK(sm_next_member(NULL, 1, NULL) == SM_IDX_MAX);
	CHECK(sm_prev_member(NULL, 1, NULL) == SM_IDX_MAX);
	CHECK(sm_locator_build(NULL) == NULL);

	/* --- Single-bit mutators: SM_IDX_MAX + EINVAL. --- */
	CHECK_EINVAL(sm_add(NULL, 1), SM_IDX_MAX);
	CHECK_EINVAL(sm_remove(NULL, 1), SM_IDX_MAX);
	CHECK_EINVAL(sm_assign(NULL, 1, true), SM_IDX_MAX);
	CHECK_EINVAL(sm_split(NULL, 1, NULL), SM_IDX_MAX);

	/* --- Set algebra: empty operands. --- */
	CHECK(sm_union(NULL, NULL) == NULL);
	CHECK(sm_intersection(NULL, NULL) == NULL);
	CHECK(sm_difference(NULL, NULL) == NULL);
	CHECK(sm_xor(NULL, NULL) == NULL);
	CHECK(sm_or(NULL, NULL) == NULL);
	CHECK(sm_and(NULL, NULL) == NULL);
	CHECK(sm_andnot(NULL, NULL) == NULL);
	CHECK(sm_offset(NULL, 1) == NULL);
	CHECK(sm_extract_range(NULL, 1, 2) == NULL);

	/* --- Comparisons / predicates. --- */
	CHECK(sm_equals(NULL, NULL) == true);
	CHECK(sm_is_subset(NULL, NULL) == true);
	CHECK(sm_is_superset(NULL, NULL) == true);
	CHECK(sm_overlap(NULL, NULL) == false);
	CHECK(sm_nonempty_difference(NULL, NULL) == false);
	CHECK(sm_compare(NULL, NULL) == 0);
	(void)sm_subset_compare(NULL, NULL); /* must not crash */

	/* --- Cardinality / metric family. --- */
	CHECK(sm_union_cardinality(NULL, NULL) == 0);
	CHECK(sm_intersection_cardinality(NULL, NULL) == 0);
	CHECK(sm_difference_cardinality(NULL, NULL) == 0);
	CHECK(sm_xor_cardinality(NULL, NULL) == 0);
	CHECK(sm_jaccard_index(NULL, NULL) == 0.0);

	/* --- Bulk / range mutators. --- */
	CHECK(sm_add_many(NULL, NULL, 1) == false);
	CHECK(sm_add_range(NULL, 1, 2) == false);
	CHECK(sm_remove_range(NULL, 1, 2) == false);
	CHECK(sm_flip_range(NULL, 1, 2) == false);
	CHECK(sm_pop_first(NULL) == SM_IDX_MAX);
	CHECK(sm_pop_last(NULL) == SM_IDX_MAX);

	/* sm_to_array with a NULL map: query mode reports zero. */
	{
		size_t n = 0;
		sm_to_array(NULL, NULL, &n);
		CHECK(n == 0);
	}

	/* --- In-place set algebra (dst, src; returns sm_t*). --- */
	CHECK(sm_union_inplace(NULL, NULL) == NULL);
	CHECK(sm_intersection_inplace(NULL, NULL) == NULL);
	CHECK(sm_difference_inplace(NULL, NULL) == NULL);
	CHECK(sm_xor_inplace(NULL, NULL) == NULL);

	if (failures == 0)
		printf("test_null: NULL-map contract OK (69 functions)\n");
	return (failures != 0);
}
