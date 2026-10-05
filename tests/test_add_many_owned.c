/* SPDX-License-Identifier: MIT
 *
 * Regression test for the 5.8.1 Defect 1 memory-safety bug:
 * sm_add_many / sm_create_from_array use-after-free + double-free + leak.
 *
 * sm_add_many takes an sm_t* (not sm_t**) and documents that it must not
 * relocate the caller's buffer.  Before the fix, __sm_add_many_core always
 * routed through __sm_replace_buffer -> sm_set_data_size, which for an
 * SM_OWNED_CONTIGUOUS map reallocs (and frees) the whole struct+buffer
 * block when the merged result outgrows the current capacity.  So on an
 * owned map that had to grow:
 *   - the map MOVED; the caller's pointer was now freed,
 *   - sm_add_many then returned false (m != map), and
 *   - the correct result lived in a leaked block.
 * sm_create_from_array made it unconditional for any input > ~1KiB: it
 * did sm_create(1024) -> sm_add_many -> on false, sm_free(stale pointer),
 * a heap-use-after-free + double-free (ASan: __sm_kind <- sm_free <-
 * sm_create_from_array).
 *
 * The fix: sm_add_many keeps its no-relocate promise -- on an owned map
 * whose result would exceed capacity it frees the scratch result and
 * returns false, leaving the caller's map UNCHANGED and valid; and
 * sm_create_from_array uses the growing variant sm_add_many_grow.
 *
 * Build this under -fsanitize=address,undefined: before the fix it aborts
 * with heap-use-after-free; after the fix it is clean.
 */
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                              \
	do {                                                          \
		if (!(cond)) {                                        \
			fprintf(stderr, "FAIL: %s\n", msg);           \
			failures++;                                   \
		}                                                     \
	} while (0)

int
main(void)
{
	/* 5000 members, stride 97 -> {0, 97, ..., 97*4999}.  Well beyond
	 * what a 1024-byte initial buffer can hold, so the bulk result must
	 * relocate.  This is the exact shape from the defect report. */
	enum { N = 5000 };
	uint64_t *arr = malloc(N * sizeof(uint64_t));
	size_t i;
	if (arr == NULL)
		return (99);
	for (i = 0; i < N; i++)
		arr[i] = (uint64_t)(97 * i);

	/* (A) sm_create_from_array: must return a valid, correct, non-NULL
	 * map with no UAF/double-free/leak. */
	{
		sm_t *m = sm_create_from_array(arr, N);
		CHECK(m != NULL, "sm_create_from_array returned NULL");
		if (m != NULL) {
			size_t present = 0;
			CHECK(sm_validate(m),
			    "sm_create_from_array result fails sm_validate");
			CHECK(sm_cardinality(m) == N,
			    "sm_create_from_array cardinality wrong");
			for (i = 0; i < N; i++)
				if (sm_contains(m, arr[i], NULL))
					present++;
			CHECK(present == N,
			    "sm_create_from_array lost members");
			sm_free(m);
		}
	}

	/* (B) sm_add_many on an owned sm_create(1024) map that must grow:
	 * per the fix, returns false, leaving the ORIGINAL pointer valid and
	 * unchanged (empty).  The pointer must not have been freed or moved. */
	{
		sm_t *m = sm_create(1024);
		sm_t *before = m;
		bool ok;
		CHECK(m != NULL, "sm_create(1024) failed");
		if (m != NULL) {
			ok = sm_add_many(m, arr, N);
			CHECK(!ok,
			    "sm_add_many should fail (cannot grow owned map)");
			/* The no-relocate promise: same object, still valid,
			 * still empty -- a careful caller keeps using it. */
			CHECK(m == before,
			    "sm_add_many relocated the owned map on false");
			CHECK(sm_validate(m),
			    "owned map invalid after false sm_add_many");
			CHECK(sm_cardinality(m) == 0,
			    "owned map not empty after false sm_add_many");
			/* And the documented retry path works on the same
			 * pointer. */
			CHECK(sm_add_many_grow(&m, arr, N),
			    "sm_add_many_grow failed after sm_add_many false");
			CHECK(sm_validate(m), "grown map invalid");
			CHECK(sm_cardinality(m) == N,
			    "grown map cardinality wrong");
			sm_free(m);
		}
	}

	/* (C) a small array that DOES fit in 1024 bytes: sm_add_many must
	 * still succeed in place and not relocate (the common fast path). */
	{
		uint64_t small[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
		sm_t *m = sm_create(1024);
		sm_t *before = m;
		CHECK(m != NULL, "sm_create(1024) failed (C)");
		if (m != NULL) {
			CHECK(sm_add_many(m, small, 8),
			    "sm_add_many of a small fitting array failed");
			CHECK(m == before,
			    "sm_add_many relocated on a fitting array");
			CHECK(sm_cardinality(m) == 8,
			    "sm_add_many fitting cardinality wrong");
			sm_free(m);
		}
	}

	free(arr);
	if (failures == 0) {
		printf("test_add_many_owned: all cases passed\n");
		return (0);
	}
	fprintf(stderr, "test_add_many_owned: %d failures\n", failures);
	return (1);
}
