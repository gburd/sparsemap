/* SPDX-License-Identifier: MIT */
/*
 * test_cursor_gap.c - read-cursor lookups that fall in the gap before a
 * chunk (5.8.4, reworked in 5.8.5).
 *
 * A lookup that falls in the gap BEFORE a chunk returns that chunk: the
 * walk stops at the first chunk ending after idx.  Through 5.8.3 the cursor
 * cached that chunk, and the next ascending lookup in the same gap
 * (idx < cursor start) ignored the cursor and walked from chunk 0, so a
 * pass of ascending lookups against a sparse map -- one member per chunk --
 * cost O(lookups x chunks).  pg_fts hit it as a 60-300x slower ranked scan.
 *
 * 5.8.4 fixed the cost by resuming from the cursor's prev_offset hint, but
 * nothing proved the hint was a chunk start: a hint pointing at a payload
 * word could pass its checks and start the walk mid-chunk, returning a
 * wrong answer (pg_fts, found with CBMC).  5.8.5 never resumes from the
 * hint.  A gap lookup caches the left neighbour, which the walk itself
 * just verified, so the next gap lookup resumes there.
 *
 * Checks:
 *   1. exactness: sm_contains with a cursor equals sm_contains with NULL,
 *      for ascending passes (with repeats) and for arbitrary order (the
 *      cursor self-validates), over sparse, clustered and run-length maps;
 *   2. hint_never_trusted: pg_fts's reproducer -- a valid cursor with
 *      prev_offset set to every 8-byte offset -- agrees with NULL;
 *   3. any_cursor_state: sm_contains and sm_next_member agree with NULL
 *      through cursors at every offset whose start word does not match,
 *      and at every chunk start with the right start word, for every
 *      prev_offset and index of small maps;
 *   4. cost: 100,000 ascending gap lookups over 20,000 one-member chunks
 *      finish under a ceiling (about 1e9 chunk steps, seconds, on 5.8.3;
 *      about 1e5 with the fix).  Lifted under sanitizers and coverage, as in
 *      test_amplify.c.
 */
#define SM_EXPOSE_STRUCT 1
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/*
 * pg_fts's reproducer.  Three adjacent one-member chunks (at byte offsets
 * 0, 24 and 48); a lookup in the third leaves the cursor there.  Then
 * prev_offset is set to every 8-byte offset and member 32773 looked up.
 * On 5.8.4 prev_offset 16 -- chunk 0's vector word -- passed the
 * neighbour checks and the walk answered false for a member.
 */
static int
hint_never_trusted(void)
{
	const uint64_t b = 32768, c = 2048, x = 32773;
	sm_t *m = sm_create(4096);
	sm_cursor_t cur = SM_CURSOR_INIT;
	size_t hint;

	if (m == NULL)
		return (1);
	sm_add(m, b + 5);
	sm_add(m, b + c + 5);
	sm_add(m, b + 2 * c + 5);
	(void)sm_contains(m, 38479, &cur);
	for (hint = 0; hint <= 64; hint += 8) {
		sm_cursor_t h = cur;
		h.prev_offset = hint;
		if (sm_contains(m, x, &h) != sm_contains(m, x, NULL)) {
			fprintf(stderr, "FAIL prev_offset %zu: sm_contains(%llu) "
			    "disagrees with the cursor-free lookup\n", hint,
			    (unsigned long long)x);
			sm_free(m);
			return (1);
		}
	}
	sm_free(m);
	return (0);
}

/*
 * Cursors the API never produces -- what a cursor kept across a mutation,
 * copied from another map, or overwritten can hold.  The walk may only
 * start at chunk 0 or at a cached offset whose start word matches the
 * recorded start.  So for every cached offset whose word does NOT match
 * (any start_idx but the word there), and for every true chunk start
 * with the right start_idx, every answer must equal the cursor-free one,
 * whatever prev_offset holds.  (A wrong offset whose payload word happens
 * to equal start_idx is not detectable without more state in the cursor;
 * sm_cursor_t documents that a cursor kept across a mutation is invalid.)
 * Small maps keep the full cross product affordable.
 */
static int
cursor_agrees(sm_t *m, uint64_t q, size_t off, uint64_t start, size_t po)
{
	sm_cursor_t c;

	c.offset = off;
	c.start_idx = start;
	c.prev_offset = po;
	if (sm_contains(m, q, &c) != sm_contains(m, q, NULL))
		return (0);
	c.offset = off;
	c.start_idx = start;
	c.prev_offset = po;
	return (sm_next_member(m, q, &c) == sm_next_member(m, q, NULL));
}

static int
any_cursor_state(void)
{
	int trial;

	for (trial = 0; trial < 40; trial++) {
		sm_t *m = sm_create(1024);
		const uint64_t base = 1 << 20, span = 6 * 2048;
		size_t used, off, po, chunk_at = 0;
		uint64_t q, k, n = 1 + rnd() % 12;

		if (m == NULL)
			return (1);
		for (k = 0; k < n; k++) {
			const uint64_t v = base + rnd() % span;
			if (rnd() % 4 == 0)
				sm_add_range(m, v, v + 1 + rnd() % 3000);
			else
				sm_add(m, v);
		}
		used = sm_get_size(m) - 8; /* chunk stream after the count */
		for (off = 0; off < used; off += 8) {
			const uint8_t *d = sm_get_data(m) + 8;
			uint64_t here;
			const int is_chunk = off == chunk_at;

			memcpy(&here, d + off, sizeof(here));
			if (is_chunk) {
				/* The next chunk: 8-byte start, 8-byte descriptor,
				 * then one word per MIXED (10) flag -- none for an
				 * RLE descriptor (top bits 01). */
				uint64_t desc;
				unsigned int slot;

				memcpy(&desc, d + off + 8, sizeof(desc));
				chunk_at = off + 16;
				if ((desc >> 62) != 1)
					for (slot = 0; slot < 32; slot++)
						if (((desc >> (2 * slot)) & 3) == 2)
							chunk_at += 8;
			}
			for (po = 0; po <= used; po += 8) {
				for (q = base - 2048; q < base + span + 2048;
				    q += 1 + rnd() % 97) {
					if (!cursor_agrees(m, q, off, here + 2048, po) ||
					    (is_chunk &&
						!cursor_agrees(m, q, off, here, po))) {
						fprintf(stderr, "FAIL any-cursor offset %zu "
						    "(%s) prev %zu idx %llu\n", off,
						    is_chunk ? "chunk start" : "mid-chunk", po,
						    (unsigned long long)q);
						sm_free(m);
						return (1);
					}
				}
			}
		}
		if (chunk_at != used) { /* the walk above must tile the stream */
			fprintf(stderr, "FAIL any-cursor: chunk walk ended at %zu, "
			    "stream is %zu bytes\n", chunk_at, used);
			sm_free(m);
			return (1);
		}
		sm_free(m);
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
	if (exactness() != 0 || hint_never_trusted() != 0 ||
	    any_cursor_state() != 0 || cost() != 0)
		return (1);
	printf("test_cursor_gap: ok\n");
	return (0);
}
