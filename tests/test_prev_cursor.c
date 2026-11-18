/* SPDX-License-Identifier: MIT */
/*
 * test_prev_cursor.c - sm_prev_member with a read cursor (5.9.0).
 *
 * Through 5.8.5 sm_prev_member ignored its cursor and walked the chunk
 * list from chunk 0 on every call, so a reverse scan cost O(chunks) per
 * member.  It now resumes at the cursor's chunk once the start word stored
 * there matches the cursor, and records the chunk that held the answer.
 *
 * Checks:
 *   1. exactness: a full reverse scan with a cursor equals the sorted
 *      member list reversed, on every map shape (empty, small set, sparse,
 *      clustered, runs, members on chunk boundaries, top of the
 *      universe);
 *   2. any order: sm_prev_member and sm_next_member sharing one cursor in
 *      random order agree with the cursor-free answers;
 *   3. any_cursor_state: cursors at every offset whose start word does not
 *      match, and at every chunk start with the right start word, for
 *      every prev_offset, agree with the cursor-free answer;
 *   4. cost: a reverse scan of 500,000 members in 1,000 chunks finishes
 *      under a ceiling (5e8 chunk steps without the cursor, about 1.5e6
 *      with it).  Lifted under sanitizers and coverage.
 */
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__SANITIZE_ADDRESS__) || defined(SM_COVERAGE_BUILD)
#define PREV_CEILING_MS 2000.0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) ||                                       \
    __has_feature(undefined_behavior_sanitizer)
#define PREV_CEILING_MS 2000.0
#else
#define PREV_CEILING_MS 250.0
#endif
#else
#define PREV_CEILING_MS 250.0
#endif

static uint64_t rs = UINT64_C(0x9E3779B97F4A7C15);

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

/* Members of the given shape, sorted and without duplicates, in v[];
 * returns the count.  v must hold 20,000 entries. */
static size_t
members(int shape, uint64_t *v)
{
	size_t n = 0, k, d;
	uint64_t b;

	switch (shape) {
	case 0: /* empty */
		break;
	case 1: /* small set */
		for (k = 0; k < 60; k++)
			v[n++] = rnd() % 1024;
		break;
	case 2: /* sparse: about one member per chunk */
		for (k = 0; k < 400; k++)
			v[n++] = rnd() % 50000000;
		break;
	case 3: /* clustered on chunk boundaries */
		for (k = 0; k < 100; k++) {
			b = (rnd() % 500) * 2048;
			v[n++] = b;
			v[n++] = b + 2047;
			v[n++] = b + 2048 + rnd() % 64;
		}
		break;
	case 4: /* runs (RLE or all-ONES chunks), plus a sparse tail */
		for (b = 4096; b < 4096 + 9000; b++)
			v[n++] = b;
		for (b = 100000; b < 100000 + 2500; b++)
			v[n++] = b;
		for (k = 0; k < 50; k++)
			v[n++] = 200000 + rnd() % 100000;
		break;
	case 5: /* top of the universe, up to the last member 2^64 - 2 */
		v[n++] = 7;
		v[n++] = UINT64_MAX - 2048;
		v[n++] = UINT64_MAX - 2;
		v[n++] = UINT64_MAX - 1;
		break;
	default: /* dense chunks */
		for (k = 0; k < 6000; k++)
			v[n++] = rnd() % 20000;
		break;
	}
	qsort(v, n, sizeof(uint64_t), cmp_u64);
	for (k = d = 0; k < n; k++)
		if (d == 0 || v[k] != v[d - 1])
			v[d++] = v[k];
	return (d);
}

/* Build a map from sorted members. */
static sm_t *
build(const uint64_t *v, size_t n)
{
	sm_t *m = sm_create(256);

	if (m != NULL && n > 0 && !sm_add_many_grow(&m, v, n)) {
		sm_free(m);
		return (NULL);
	}
	return (m);
}

static int
exactness(void)
{
	uint64_t *v = malloc(20000 * sizeof(uint64_t));
	int shape, rep;

	if (v == NULL)
		return (1);
	for (rep = 0; rep < 10; rep++) {
		for (shape = 0; shape <= 6; shape++) {
			const size_t n = members(shape, v);
			sm_t *m = build(v, n);
			sm_cursor_t c = SM_CURSOR_INIT;
			uint64_t i = SM_IDX_MAX, q;
			size_t k = n;

			if (m == NULL)
				return (1);
			while ((i = sm_prev_member(m, i, &c)) != SM_IDX_MAX) {
				if (k == 0 || i != v[k - 1]) {
					fprintf(stderr, "FAIL reverse scan shape %d: "
					    "got %llu\n", shape, (unsigned long long)i);
					return (1);
				}
				k--;
			}
			if (k != 0) {
				fprintf(stderr, "FAIL reverse scan shape %d stopped "
				    "with %zu members left\n", shape, k);
				return (1);
			}
			/* Any order, one cursor shared by both directions. */
			c = (sm_cursor_t)SM_CURSOR_INIT;
			for (k = 0; k < 4000; k++) {
				q = (n > 0 && k % 2 == 0) ? v[rnd() % n] + rnd() % 3 - 1
				    : (k % 7 == 0) ? SM_IDX_MAX - rnd() % 3
				    : rnd() % 3000000;
				if (sm_prev_member(m, q, &c) !=
				        sm_prev_member(m, q, NULL) ||
				    (k % 3 == 0 && sm_next_member(m, q, &c) !=
					sm_next_member(m, q, NULL))) {
					fprintf(stderr, "FAIL any order shape %d idx "
					    "%llu\n", shape, (unsigned long long)q);
					return (1);
				}
			}
			sm_free(m);
		}
	}
	free(v);
	return (0);
}

static int
cursor_agrees(sm_t *m, uint64_t q, size_t off, uint64_t start, size_t po)
{
	sm_cursor_t c;

	c.offset = off;
	c.start_idx = start;
	c.prev_offset = po;
	return (sm_prev_member(m, q, &c) == sm_prev_member(m, q, NULL));
}

/* Does this build encode a whole chunk of ones as one RLE descriptor? */
static int
build_has_rle(void)
{
	sm_t *m = sm_create(1024);
	uint64_t desc = 0;

	if (m == NULL)
		return (0);
	sm_add_range(m, 1 << 20, (1 << 20) + 3 * 2048);
	memcpy(&desc, (uint8_t *)sm_get_data(m) + 16, sizeof(desc));
	sm_free(m);
	return ((desc >> 62) == 1);
}

/* Cursors the API never produces; see test_cursor_gap.c. */
static int
any_cursor_state(void)
{
	const int rle_build = build_has_rle();
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
		used = sm_get_size(m) - 8;
		for (off = 0; off < used; off += 8) {
			const uint8_t *d = (const uint8_t *)sm_get_data(m) + 8;
			uint64_t here;
			const int is_chunk = off == chunk_at;

			memcpy(&here, d + off, sizeof(here));
			if (is_chunk) {
				uint64_t desc;
				unsigned int slot;

				memcpy(&desc, d + off + 8, sizeof(desc));
				chunk_at = off + 16;
				if (!rle_build || (desc >> 62) != 1)
					for (slot = 0; slot < 32; slot++)
						if (((desc >> (2 * slot)) & 3) == 2)
							chunk_at += 8;
			}
			for (po = 0; po <= used; po += 8) {
				for (q = base - 2048; q < base + span + 4096;
				    q += 1 + rnd() % 97) {
					if (!cursor_agrees(m, q, off, here + 2048, po) ||
					    (is_chunk &&
						!cursor_agrees(m, q, off, here, po))) {
						fprintf(stderr, "FAIL any-cursor offset %zu "
						    "(%s) prev %zu idx %llu\n", off,
						    is_chunk ? "chunk start" : "mid-chunk", po,
						    (unsigned long long)q);
						return (1);
					}
				}
				if (!cursor_agrees(m, SM_IDX_MAX, off, here + 2048,
				        po) ||
				    (is_chunk &&
					!cursor_agrees(m, SM_IDX_MAX, off, here, po)))
					return (1);
			}
		}
		if (chunk_at != used) {
			fprintf(stderr, "FAIL any-cursor: chunk walk ended at %zu, "
			    "stream is %zu bytes\n", chunk_at, used);
			return (1);
		}
		sm_free(m);
	}
	return (0);
}

static int
cost(void)
{
	const uint64_t nchunks = 1000, per = 500;
	uint64_t *ids = malloc(nchunks * per * sizeof(uint64_t));
	uint64_t i, k, seen = 0;
	sm_t *m = sm_create(256);
	sm_cursor_t c = SM_CURSOR_INIT;
	double t0, ms;

	if (m == NULL || ids == NULL)
		return (1);
	for (i = 0; i < nchunks; i++)
		for (k = 0; k < per; k++)
			ids[i * per + k] = i * 10000 + 3 * k; /* mixed words */
	if (!sm_add_many_grow(&m, ids, nchunks * per))
		return (1);
	t0 = now_ms();
	i = SM_IDX_MAX;
	while ((i = sm_prev_member(m, i, &c)) != SM_IDX_MAX)
		seen++;
	ms = now_ms() - t0;
	if (seen != nchunks * per) {
		fprintf(stderr, "FAIL reverse scan saw %llu members\n",
		    (unsigned long long)seen);
		return (1);
	}
	if (ms >= PREV_CEILING_MS) {
		fprintf(stderr, "FAIL reverse scan of %llu members took %.3f ms\n",
		    (unsigned long long)seen, ms);
		return (1);
	}
	printf("reverse scan with cursor: %.3f ms\n", ms);
	sm_free(m);
	free(ids);
	return (0);
}

int
main(void)
{
	if (exactness() != 0 || any_cursor_state() != 0 || cost() != 0)
		return (1);
	printf("test_prev_cursor: ok\n");
	return (0);
}
