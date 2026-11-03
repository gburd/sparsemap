/* SPDX-License-Identifier: MIT */
/*
 * test_remove_tail.c - __sm_remove_data read past a full buffer (5.8.4).
 *
 * __sm_remove_data takes a chunk-data-relative offset (past the 8-byte
 * chunk-count header) but sized its memmove from m_data_used, which
 * counts that header on an sm_create / sm_clear / sm_open map.  So every
 * removal read SM_SIZEOF_OVERHEAD bytes past the used end, and past the
 * allocation when the buffer was exactly full.  (__sm_insert_data has the
 * same arithmetic, but every caller reserves that slack: SM_ENOUGH_SPACE.)
 *
 * The public path that reaches it: sm_add_range builds its result in a
 * scratch map that the emitter grows to an exact fit, then coalesces it;
 * the coalesce removes bytes from the full buffer.  The ranges below are
 * a minimised fuzz case: on 5.8.0-5.8.3 AddressSanitizer reports an
 * 8-byte heap-buffer-overflow READ in __sm_remove_data <-
 * __sm_coalesce_chunk <- __sm_coalesce_map <- sm_add_range at the last
 * range.  The bytes read landed past the new used end, so results were
 * correct; the defect is the out-of-bounds read, which only a sanitizer
 * build (CI runs one) can see.
 *
 * Checks, each against a bit-array oracle:
 *   1. the minimised sm_add_range sequence;
 *   2. the same sequence at map capacities 64..4096, so the scratch map
 *      reaches its exact-fit boundary at different points.
 */
#define SM_EXPOSE_STRUCT 1
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPAN 700000u

static const uint64_t ranges[][2] = {
	{ 6144, 13668 }, { 26841, 26890 }, { 48290, 53065 },
	{ 95133, 95556 }, { 113565, 121761 }, { 149837, 153105 },
	{ 199541, 207266 }, { 251931, 260739 }, { 313371, 315496 },
	{ 343075, 344468 }, { 376525, 384685 }, { 384717, 385705 },
	{ 405197, 410716 }, { 460493, 460949 }, { 495309, 503080 },
	{ 510937, 514344 }, { 567238, 571777 }, { 624582, 626166 },
	{ 642600, 650295 },
};
#define NRANGES (sizeof(ranges) / sizeof(ranges[0]))

static uint8_t oracle[SPAN / 8 + 1];

static int
matches(const sm_t *m, const char *what, size_t cap)
{
	uint64_t i;

	if (!sm_validate(m)) {
		fprintf(stderr, "FAIL %s cap=%zu: sm_validate\n", what, cap);
		return (0);
	}
	for (i = 0; i < SPAN; i++) {
		const int want = oracle[i >> 3] >> (i & 7) & 1;
		if ((int)sm_contains(m, i, NULL) != want) {
			fprintf(stderr, "FAIL %s cap=%zu: bit %llu want %d\n",
			    what, cap, (unsigned long long)i, want);
			return (0);
		}
	}
	return (1);
}

static int
add_range_sequence(size_t cap)
{
	sm_t *m = sm_create(cap);
	size_t k;
	uint64_t i;
	int ok;

	if (m == NULL)
		return (0);
	memset(oracle, 0, sizeof(oracle));
	for (k = 0; k < NRANGES; k++) {
		if (!sm_add_range(m, ranges[k][0], ranges[k][1])) {
			/* ENOSPC is a legitimate answer for a small map; the
			 * map must be unchanged and still valid. */
			ok = matches(m, "add_range ENOSPC", cap);
			sm_free(m);
			return (ok);
		}
		for (i = ranges[k][0]; i < ranges[k][1]; i++)
			oracle[i >> 3] |= (uint8_t)(1u << (i & 7));
	}
	ok = matches(m, "add_range", cap);
	sm_free(m);
	return (ok);
}

int
main(void)
{
	size_t cap;

	if (!add_range_sequence(1024))
		return (1);
	for (cap = 64; cap <= 4096; cap += 8)
		if (!add_range_sequence(cap))
			return (1);
	printf("test_remove_tail: ok\n");
	return (0);
}
