/* SPDX-License-Identifier: MIT */
/*
 * test_offset_overflow.c -- sm_offset must be well-defined and correct
 * for extreme |offset|, not just small shifts.
 *
 * The bug: sm_offset computed a chunk's shifted start as
 * `(ssize_t)src_start + offset`.  The top-of-function ERANGE guard
 * checks the surviving range in UNSIGNED arithmetic, so a large POSITIVE
 * offset (near SSIZE_MAX) whose result still fits the 64-bit universe
 * passes the guard -- but the signed ssize_t intermediate overflows,
 * which is undefined behaviour (UBSan: "signed integer overflow ...
 * cannot be represented in type long").  The fix computes the shifted
 * start as an unsigned magnitude + sign, so the overflow path is
 * well-defined and the shift result is exact.
 *
 * This test aborts under -fsanitize=undefined -fno-sanitize-recover on
 * the pre-fix code (the extreme-offset cases) and passes after.  It also
 * cross-checks sm_offset against a brute-force oracle for a spread of
 * offset shapes (small +/-, chunk-aligned, unaligned, and extreme).
 */
#include <sm.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(c)                                                              \
	do {                                                                  \
		if (!(c)) {                                                    \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
			    __LINE__, #c);                                    \
			failures++;                                           \
		}                                                             \
	} while (0)

/* sm_offset(map{bits}, off) must match the oracle: bit b -> b+off, kept
 * iff it stays in [0, 2^64); a positive offset that would push the max
 * bit past 2^64-1 makes the whole op ERANGE (NULL). */
static void
oracle(const uint64_t *bits, size_t n, int64_t off)
{
	sm_t *m = sm_create(64);
	for (size_t i = 0; i < n; i++)
		CHECK(sm_add_grow(&m, bits[i]) == bits[i]);
	sm_t *r = sm_offset(m, (ssize_t)off);

	uint64_t exp[1024];
	size_t en = 0;
	bool erange = false;

	if (off > 0) {
		uint64_t o = (uint64_t)off, mx = 0;
		for (size_t i = 0; i < n; i++)
			if (bits[i] > mx)
				mx = bits[i];
		if (n > 0 && mx > UINT64_MAX - o)
			erange = true; /* max bit would leave the universe */
	}
	if (!erange) {
		for (size_t i = 0; i < n; i++) {
			uint64_t b = bits[i];
			if (off >= 0) {
				exp[en++] = b + (uint64_t)off;
			} else {
				uint64_t down = (uint64_t)(-(off + 1)) + 1;
				if (b >= down)
					exp[en++] = b - down; /* else dropped */
			}
		}
	}

	if (erange) {
		CHECK(r == NULL);
	} else if (en == 0) {
		CHECK(r == NULL || sm_cardinality(r) == 0);
	} else {
		CHECK(r != NULL);
		if (r != NULL) {
			CHECK(sm_cardinality(r) == en);
			for (size_t i = 0; i < en; i++)
				CHECK(sm_contains(r, exp[i], NULL));
			CHECK(sm_validate(r));
		}
	}
	if (r != NULL)
		sm_free(r);
	sm_free(m);
}

int
main(void)
{
	failures = 0;

	/* The exact regression: a modest map, a near-SSIZE_MAX positive
	 * offset that clears the ERANGE guard.  This is the UBSan trigger. */
	{
		uint64_t b[] = { 100000, 100005, 100200 };
		oracle(b, 3, (int64_t)0x7FFFFFFFFFFFFFFFLL);
		oracle(b, 3, (int64_t)((uint64_t)1 << 62));
	}

	/* A spread of shapes and offsets, incl. extremes. */
	const uint64_t spread[] = { 0, 5, 63, 64, 100, 2047, 2048, 4096 };
	const int64_t offs[] = {
		1, -1, 63, -63, 64, -64, 2048, -2048, 100, -100,
		1000000, -1000000, 12345, -12345,
		(int64_t)((uint64_t)1 << 40), -(int64_t)((uint64_t)1 << 40),
		(int64_t)((uint64_t)1 << 62), -(int64_t)((uint64_t)1 << 62),
		(int64_t)0x7FFFFFFFFFFFFFFFLL, -(int64_t)0x7FFFFFFFFFFFFFFFLL, 0
	};
	for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
		oracle(spread, sizeof(spread) / sizeof(spread[0]), offs[i]);

	/* Dense run: unaligned extreme shifts split every chunk. */
	{
		uint64_t run[600];
		for (int i = 0; i < 600; i++)
			run[i] = (uint64_t)i;
		for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
			oracle(run, 600, offs[i]);
	}

	/* Chunk starts above SSIZE_MAX (bits near 2^63) with negative
	 * offsets: the (ssize_t)src_start cast is itself UB pre-fix. */
	{
		uint64_t hi[] = { ((uint64_t)1 << 63) + 10,
			((uint64_t)1 << 63) + 100, ((uint64_t)1 << 63) + 5000 };
		const int64_t hoff[] = { -1, -100, -5000,
			-(int64_t)((uint64_t)1 << 40), 1, 1000, 0 };
		for (size_t i = 0; i < sizeof(hoff) / sizeof(hoff[0]); i++)
			oracle(hi, 3, hoff[i]);
	}

	/* Unaligned offsets that split chunks across two output windows. */
	{
		uint64_t u[] = { 0, 1, 2, 63, 64, 65, 2047, 2048, 2049 };
		const int64_t uoff[] = { 7, 13, 63, 65, 1000, -7, -65, -1000,
			1234567, -1234567 };
		for (size_t i = 0; i < sizeof(uoff) / sizeof(uoff[0]); i++)
			oracle(u, sizeof(u) / sizeof(u[0]), uoff[i]);
	}

	if (failures) {
		fprintf(stderr, "test_offset_overflow: %d failure(s)\n",
		    failures);
		return (1);
	}
	fprintf(stderr, "test_offset_overflow: OK\n");
	return (0);
}
