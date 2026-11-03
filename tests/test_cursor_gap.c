/* SPDX-License-Identifier: MIT */
/*
 * test_cursor_gap.c - read-cursor lookups that fall in the gap before a
 * chunk.
 *
 * A lookup that falls in the gap BEFORE a chunk caches that chunk: the
 * walk stops at the first chunk ending after idx.  Through 5.8.3 the next
 * ascending lookup in the same gap (idx < cursor start) ignored the cursor
 * and walked from chunk 0, so a pass of ascending lookups against a sparse
 * map -- one member per chunk -- cost O(lookups x chunks).  pg_fts hit it
 * as a 60-300x slower ranked scan: live document ids tested against
 * deletions scattered over a large table.  The cursor now resumes from the
 * cached chunk's left neighbour.
 *
 * Checks:
 *   1. exactness: sm_contains with a cursor equals sm_contains with NULL,
 *      for ascending passes (with repeats) and for arbitrary order (the
 *      cursor self-validates), over sparse, clustered and run-length maps;
 *   2. cost: 100,000 ascending gap lookups over 20,000 one-member chunks
 *      finish under a ceiling (about 1e9 chunk steps, seconds, on 5.8.3;
 *      about 1e5 with the fix).  Lifted under sanitizers and coverage, as in
 *      test_amplify.c.
 */
#define SM_EXPOSE_STRUCT 1
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(__SANITIZE_ADDRESS__) || defined(SM_COVERAGE_BUILD)
#define GAP_CEILING_MS 2000.0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) ||                                       \
    __has_feature(undefined_behavior_sanitizer)
#define GAP_CEILING_MS 2000.0
#else
#define GAP_CEILING_MS 250.0
#endif
#else
#define GAP_CEILING_MS 250.0
#endif

static uint64_t rs = 0x9E3779B97F4A7C15ULL;

static uint64_t
rnd(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return (rs);
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x < y ? -1 : x > y);
}

static double
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return ((double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6);
}

static int
exactness(void)
{
	int trial;

	for (trial = 0; trial < 200; trial++) {
		sm_t *m = sm_create(256);
		uint64_t span = 1000 + rnd() % 3000000, n = rnd() % 3000;
		uint64_t q, k, *v = malloc((n + 1) * sizeof(uint64_t));
		sm_cursor_t c = SM_CURSOR_INIT;

		if (m == NULL || v == NULL)
			return (1);
		for (k = 0; k < n; k++)
			v[k] = (trial % 3 == 0) ? rnd() % span
			    : (trial % 3 == 1) ? (rnd() % 64) * 4096 + rnd() % 8
			    : (k / 300) * 20000 + k % 300; /* runs: RLE chunks */
		qsort(v, n, sizeof(uint64_t), cmp_u64);
		if (n > 0 && !sm_add_many_grow(&m, v, n)) {
			fprintf(stderr, "FAIL sm_add_many_grow\n");
			return (1);
		}
		for (q = 0; q < span + 5000; q += 1 + (rnd() % 97 == 0 ? 0 : rnd() % 400))
			if (sm_contains(m, q, &c) != sm_contains(m, q, NULL)) {
				fprintf(stderr, "FAIL ascending trial %d idx %llu\n",
				    trial, (unsigned long long)q);
				return (1);
			}
		c = (sm_cursor_t)SM_CURSOR_INIT;
		for (k = 0; k < 3000; k++) {
			q = (n > 0 && k % 3 == 0) ? v[rnd() % n] + rnd() % 3 - 1
			    : rnd() % (span + 5000);
			if (sm_contains(m, q, &c) != sm_contains(m, q, NULL)) {
				fprintf(stderr, "FAIL any-order trial %d idx %llu\n",
				    trial, (unsigned long long)q);
				return (1);
			}
		}
		sm_free(m);
		free(v);
	}
	return (0);
}

static int
cost(void)
{
	const uint64_t nmem = 20000, stride = 3000;
	uint64_t i, q, hits = 0, *ids = malloc(nmem * sizeof(uint64_t));
	sm_t *m = sm_create(256);
	sm_cursor_t c = SM_CURSOR_INIT;
	double t0, ms;

	if (m == NULL || ids == NULL)
		return (1);
	for (i = 0; i < nmem; i++)
		ids[i] = i * stride + 7; /* one member per chunk */
	if (!sm_add_many_grow(&m, ids, nmem)) {
		fprintf(stderr, "FAIL sm_add_many_grow\n");
		return (1);
	}
	t0 = now_ms();
	for (q = 0; q < nmem * stride; q += 600)
		hits += sm_contains(m, q, &c);
	ms = now_ms() - t0;
	if (hits != 0) { /* q = 600j never equals 3000i + 7 */
		fprintf(stderr, "FAIL %llu unexpected hits\n",
		    (unsigned long long)hits);
		return (1);
	}
	if (ms >= GAP_CEILING_MS) {
		fprintf(stderr, "FAIL 100,000 ascending gap lookups took %.3f ms\n",
		    ms);
		return (1);
	}
	printf("ascending gap lookups: %.3f ms\n", ms);
	sm_free(m);
	free(ids);
	return (0);
}

int
main(void)
{
	if (exactness() != 0 || cost() != 0)
		return (1);
	printf("test_cursor_gap: ok\n");
	return (0);
}
