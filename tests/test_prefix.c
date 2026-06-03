/* SPDX-License-Identifier: MIT */
/*
 * test_prefix.c - verify the SM_PREFIX symbol-renaming mechanism.
 *
 * This translation unit is compiled with -DSM_PREFIX=smtest_ and
 * linked against a private copy of sm.c built with the same define
 * (see tests/meson.build).  It exercises the renamed API end to end:
 * the public functions and type names must pick up the prefix, while
 * compile-time constants (SM_IDX_MAX, the SM_VERSION_* values, and the
 * enum constants) must stay unprefixed because they never reach the
 * linker.
 *
 * If the prefix list in sm.h ever drifts out of sync with the set of
 * exported functions, this file fails to compile or link.
 */
#include <sm.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/* Always-on check (unlike assert(), survives -DNDEBUG). */
#define CHECK(cond)                                                        \
	do {                                                               \
		if (!(cond)) {                                             \
			fprintf(stderr, "test_prefix: %s:%d: %s\n",        \
			    __FILE__, __LINE__, #cond);                    \
			return (1);                                        \
		}                                                          \
	} while (0)

int
main(void)
{
	smtest_sparsemap_t *m;
	smtest_sm_stats_t st;
	smtest_sm_membership_t mb;
	uint64_t arr[3] = { 5, 9, 100000 };

	m = smtest_sm_create(8192);
	if (m == NULL)
		return (1);

	smtest_sm_add(m, 42);
	smtest_sm_add(m, 1024);
	CHECK(smtest_sm_contains(m, 42));
	CHECK(!smtest_sm_contains(m, 43));
	CHECK(smtest_sm_cardinality(m) == 2);

	/* Enum constants are not prefixed (compile-time only). */
	mb = smtest_sm_membership(m);
	CHECK(mb == SM_MULTIPLE);

	CHECK(smtest_sm_select(m, 0, true) == 42);
	CHECK(smtest_sm_minimum(m) == 42);
	CHECK(smtest_sm_maximum(m) == 1024);

	smtest_sm_add_many(m, arr, 3);
	CHECK(smtest_sm_contains(m, 100000));

	smtest_sm_statistics(m, &st);
	CHECK(st.bits_set == smtest_sm_cardinality(m));

	/* Macros are not prefixed. */
	CHECK(SM_NOT_FOUND(smtest_sm_select(m, 999999, true)));
	CHECK(SM_IDX_MAX == UINT64_MAX);

	smtest_sm_free(m);

	printf("test_prefix: SM_PREFIX renaming OK (version %s)\n",
	    SM_VERSION_STRING);
	return (0);
}
