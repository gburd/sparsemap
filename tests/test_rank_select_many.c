/* SPDX-License-Identifier: MIT */
/*
 * test_rank_select_many.c - sm_rank_many and sm_select_many against
 * per-element plain calls and a sorted-array oracle.
 *
 * Checks, over empty, small-set, sparse, clustered, RLE-run,
 * chunk-boundary, near-2^64 and one-member-per-chunk maps:
 *   1. sm_rank_many(idxs)[i] == members strictly below idxs[i] (oracle)
 *      == sm_rank(map, 0, idxs[i] - 1, true) (0 for idxs[i] == 0), for
 *      sorted probes with repeats, including 0, SM_IDX_MAX and positions
 *      past the last member;
 *   2. sm_select_many(ks)[i] == the ks[i]-th member (oracle) ==
 *      sm_select(map, ks[i], true), for sorted ks with repeats, including
 *      every k past the cardinality and UINT64_MAX;
 *   3. NULL map, NULL arrays and n == 0 behave as documented.
 */
#include <sm.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, ...)                                                        \
	do {                                                                 \
		if (!(c)) {                                                  \
			fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__,       \
			    __LINE__, #c);                                  \
			fprintf(stderr, __VA_ARGS__);                       \
			fprintf(stderr, "\n");                              \
			return (1);                                         \
		}                                                           \
	} while (0)

#define CHUNK 2048u

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

/* The oracle: a sorted, de-duplicated member array. */
typedef struct {
	uint64_t *v;
	size_t n;
	size_t cap;
} set_t;

static void
set_push(set_t *s, uint64_t x)
{
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 1024;
		s->v = realloc(s->v, s->cap * sizeof(uint64_t));
		if (s->v == NULL) {
			fprintf(stderr, "out of memory\n");
			exit(2);
		}
	}
	s->v[s->n++] = x;
}

static void
set_normalize(set_t *s)
{
	size_t i, w = 0;

	if (s->n == 0)
		return;
	qsort(s->v, s->n, sizeof(uint64_t), cmp_u64);
	for (i = 0; i < s->n; i++)
		if (w == 0 || s->v[i] != s->v[w - 1])
			s->v[w++] = s->v[i];
	s->n = w;
}

/* Members strictly below x. */
static size_t
o_below(const set_t *s, uint64_t x)
{
	size_t lo = 0, hi = s->n;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (s->v[mid] < x)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (lo);
}

/*
 * Build one map shape into m (grown as needed) and the oracle.  Shapes:
 * 0 small-set, 1 uniform sparse, 2 clustered, 3 RLE runs with gaps,
 * 4 chunk-boundary members, 5 near the top of the universe, 6 one member
 * per chunk, 7 a mix of all of them.
 */
static sm_t *
build(int shape, set_t *s)
{
	sm_t *m = sm_create(1024);
	uint64_t k, n, base, len;
	int r;

	if (m == NULL)
		exit(2);
	s->n = 0;
	switch (shape) {
	case 0:
		n = rnd() % 300;
		for (k = 0; k < n; k++)
			set_push(s, rnd() % 1024);
		break;
	case 1:
		n = rnd() % 3000;
		for (k = 0; k < n; k++)
			set_push(s, rnd() % 50000000);
		break;
	case 2:
		n = rnd() % 40;
		for (k = 0; k < n; k++) {
			uint64_t c = (rnd() % 4000) * 7919, j;
			for (j = 0; j < 50; j++)
				set_push(s, c + rnd() % 300);
		}
		break;
	case 3: /* runs go in with sm_add_range, so they are RLE chunks */
		n = 1 + rnd() % 12;
		base = rnd() % 100000;
		for (k = 0; k < n; k++) {
			len = 1 + rnd() % 9000;
			for (r = 0; r < (int)len; r++)
				set_push(s, base + (uint64_t)r);
			if (!sm_add_range(m, base, base + len)) {
				m = sm_set_data_size(m, NULL,
				    sm_get_capacity(m) * 2 + 4096);
				if (m == NULL || !sm_add_range(m, base, base + len))
					exit(2);
			}
			base += len + rnd() % 20000;
		}
		set_normalize(s);
		return (m);
	case 4:
		n = rnd() % 200;
		for (k = 0; k < n; k++) {
			uint64_t c = (rnd() % 5000) * CHUNK;
			set_push(s, c);
			if (rnd() % 2)
				set_push(s, c + CHUNK - 1);
		}
		break;
	case 5:
		n = rnd() % 200;
		for (k = 0; k < n; k++)
			set_push(s, UINT64_MAX - 1 - rnd() % 100000);
		set_push(s, UINT64_MAX - 1);
		break;
	case 6:
		n = rnd() % 3000;
		for (k = 0; k < n; k++)
			set_push(s, k * 3 * CHUNK + rnd() % CHUNK);
		break;
	default:
		for (r = 0; r < 4; r++) {
			set_t t = { 0 };
			sm_t *x = build(r + 1, &t);
			uint64_t off = (uint64_t)r * 100000000;
			for (k = 0; k < t.n; k++)
				set_push(s, t.v[k] + off);
			sm_free(x);
			free(t.v);
		}
		break;
	}
	set_normalize(s);
	/* sm_add_many_grow mis-builds members near 2^64 on 5.8.5 (a separate
	 * defect), so the top-of-universe shape is added one at a time. */
	if (shape == 5) {
		for (k = 0; k < s->n; k++)
			if (sm_add_grow(&m, s->v[k]) != s->v[k])
				exit(2);
	} else if (s->n > 0 && !sm_add_many_grow(&m, s->v, s->n)) {
		fprintf(stderr, "sm_add_many_grow failed\n");
		exit(2);
	}
	return (m);
}

/* Sorted probe positions with repeats: members and neighbours, chunk
 * edges, random positions, and the ends of the universe. */
static uint64_t *
probes(const set_t *s, size_t *np)
{
	size_t cap = s->n * 4 + 64, n = 0, i;
	uint64_t *p = malloc(cap * sizeof(uint64_t));
	uint64_t top = s->n ? s->v[s->n - 1] : 0;

	if (p == NULL)
		exit(2);
	for (i = 0; i < s->n; i++) {
		uint64_t x = s->v[i];
		p[n++] = x;
		p[n++] = x + 1;
		if (i % 3 == 0)
			p[n++] = x - x % CHUNK;
		if (i % 5 == 0)
			p[n++] = x; /* a repeat */
	}
	for (i = 0; i < 40; i++)
		p[n++] = top ? rnd() % (top + 5000) : rnd() % 5000;
	p[n++] = 0;
	p[n++] = 0;
	p[n++] = UINT64_MAX - 1;
	p[n++] = UINT64_MAX;
	qsort(p, n, sizeof(uint64_t), cmp_u64);
	*np = n;
	return (p);
}

static int
check_map(sm_t *m, const set_t *s, const char *what)
{
	size_t np, nk = 0, i;
	uint64_t *p = probes(s, &np);
	size_t *ranks = malloc(np * sizeof(size_t));
	uint64_t *ks = malloc((s->n * 2 + 64) * sizeof(uint64_t));
	uint64_t *sel = malloc((s->n * 2 + 64) * sizeof(uint64_t));

	if (ranks == NULL || ks == NULL || sel == NULL)
		exit(2);
	sm_rank_many(m, p, ranks, np);
	for (i = 0; i < np; i++) {
		const size_t want = o_below(s, p[i]);
		CHECK(ranks[i] == want, "%s rank_many(%llu) = %zu, want %zu",
		    what, (unsigned long long)p[i], ranks[i], want);
		if (i % (np / 100 + 1) == 0)
			CHECK(ranks[i] == (p[i] ? sm_rank(m, 0, p[i] - 1, true)
			                        : 0),
			    "%s rank_many vs sm_rank at %llu", what,
			    (unsigned long long)p[i]);
	}

	/* ks: every member rank (with repeats), then past the end. */
	for (i = 0; i < s->n; i += 1 + (s->n > 2000 ? rnd() % 7 : 0)) {
		ks[nk++] = i;
		if (i % 9 == 0)
			ks[nk++] = i;
	}
	ks[nk++] = s->n;
	ks[nk++] = s->n + 1;
	ks[nk++] = UINT64_MAX;
	sm_select_many(m, ks, sel, nk);
	for (i = 0; i < nk; i++) {
		const uint64_t want = ks[i] < s->n ? s->v[ks[i]] : SM_IDX_MAX;
		CHECK(sel[i] == want, "%s select_many(%llu) = %llu, want %llu",
		    what, (unsigned long long)ks[i], (unsigned long long)sel[i],
		    (unsigned long long)want);
		/* sm_select casts k to ssize_t, so k >= 2^63 goes negative on
		 * an RLE chunk (a separate 5.8.5 defect); compare below it. */
		if ((i % (nk / 100 + 1) == 0 || i + 3 >= nk) &&
		    ks[i] < (UINT64_C(1) << 63))
			CHECK(sel[i] == sm_select(m, ks[i], true),
			    "%s select_many vs sm_select at %llu", what,
			    (unsigned long long)ks[i]);
	}
	free(p);
	free(ranks);
	free(ks);
	free(sel);
	return (0);
}

static int
nulls(void)
{
	const uint64_t q[3] = { 0, 5, UINT64_MAX };
	size_t r[3] = { 7, 7, 7 };
	uint64_t o[3] = { 7, 7, 7 };
	sm_t *m = sm_create(1024);

	sm_rank_many(NULL, q, r, 3);
	CHECK(r[0] == 0 && r[1] == 0 && r[2] == 0, "rank_many(NULL map)");
	sm_select_many(NULL, q, o, 3);
	CHECK(o[0] == SM_IDX_MAX && o[2] == SM_IDX_MAX, "select_many(NULL)");
	sm_rank_many(m, q, r, 3);
	CHECK(r[0] == 0 && r[2] == 0, "rank_many(empty map)");
	sm_select_many(m, q, o, 3);
	CHECK(o[0] == SM_IDX_MAX && o[1] == SM_IDX_MAX, "select_many(empty)");
	errno = 0;
	sm_rank_many(m, NULL, r, 3);
	CHECK(errno == EINVAL, "rank_many(NULL idxs) sets EINVAL");
	errno = 0;
	sm_select_many(m, q, NULL, 3);
	CHECK(errno == EINVAL, "select_many(NULL out) sets EINVAL");
	errno = 0;
	sm_rank_many(m, NULL, NULL, 0);
	sm_select_many(m, NULL, NULL, 0);
	CHECK(errno == 0, "n == 0 is a no-op");
	sm_free(m);
	return (0);
}

int
main(void)
{
	int trial;

	if (nulls() != 0)
		return (1);
	for (trial = 0; trial < 240; trial++) {
		set_t s = { 0 };
		char what[32];
		sm_t *m = build(trial % 8, &s);

		snprintf(what, sizeof(what), "shape %d trial %d", trial % 8,
		    trial);
		if (check_map(m, &s, what) != 0)
			return (1);
		sm_free(m);
		free(s.v);
	}
	printf("test_rank_select_many: ok\n");
	return (0);
}
