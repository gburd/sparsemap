/* SPDX-License-Identifier: MIT */
/*
 * test_locator.c - the sm_locator_t directory against the plain functions
 * and a sorted-array oracle.
 *
 * Checks:
 *   1. fresh: on many maps (empty, small-set, sparse, clustered, RLE runs,
 *      gaps, members at chunk boundaries, members near 2^64) a freshly
 *      built locator answers contains / rank / select1 exactly as the
 *      plain functions do and as the oracle does;
 *   2. select0: the n-th unset bit agrees with the sorted-array oracle on
 *      every map and with a bit-array oracle on maps below 2^24, and
 *      select0(rank0(0, x - 1)) == x for every unset probe x.  Plain
 *      sm_select(map, n, false) is wrong across chunk gaps in 5.8.5, so
 *      it is not used as an oracle;
 *   3. huge runs: RLE runs of nearly 2^31 bits, so one group of chunks
 *      spans too far for the compact per-chunk record, checked against an
 *      interval oracle;
 *   4. structural staleness: after a mutation that inserts or removes a
 *      chunk, the locator's fingerprint check notices and the queries fall
 *      back to the plain path on the live map, so they stay correct.
 *
 * A mutation that flips a bit inside an existing chunk without moving any
 * bytes is NOT detected (sm_t has no change counter); the contract in sm.h
 * says so and nothing here tests it.
 */
#include <sm.h>

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

static int
o_contains(const set_t *s, uint64_t x)
{
	size_t i = o_below(s, x);

	return (i < s->n && s->v[i] == x);
}

/* Members in [lo, hi] inclusive. */
static size_t
o_rank(const set_t *s, uint64_t lo, uint64_t hi)
{
	size_t below_hi;

	if (lo > hi)
		return (0);
	below_hi = (hi == UINT64_MAX) ? s->n : o_below(s, hi + 1);
	return (below_hi - o_below(s, lo));
}

/* The k-th (0-based) unset position in [0, 2^64 - 1), or SM_IDX_MAX.
 * With j members below it, the answer is k + j, where j is the first
 * index with v[j] - j > k (v[j] - j never decreases). */
static uint64_t
o_select0(const set_t *s, uint64_t k)
{
	size_t lo = 0, hi = s->n;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (s->v[mid] - mid <= k)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (k >= UINT64_MAX - lo)
		return (SM_IDX_MAX);
	return (k + lo);
}

/*
 * Build one map shape into m (grown as needed) and the oracle.  Shapes:
 * 0 small-set, 1 uniform sparse, 2 clustered, 3 RLE runs with gaps,
 * 4 chunk-boundary members, 5 near the top of the universe, 6 one member
 * per chunk, 7 a clustered map shifted by sm_offset (its chunks carry
 * NONE slots, so they cover less than 2048 positions), 8 a mix of 1-4.
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
	case 7: { /* sm_offset of a clustered map: chunks with NONE slots */
		set_t t = { 0 };
		sm_t *x = build(2, &t);
		const ssize_t by = (ssize_t)(rnd() % 3000) - 1000;
		sm_t *y = sm_offset(x, by);
		for (k = 0; k < t.n; k++)
			if ((int64_t)t.v[k] + by >= 0)
				set_push(s, (uint64_t)((int64_t)t.v[k] + by));
		sm_free(x);
		free(t.v);
		sm_free(m);
		if (y == NULL)
			y = sm_create(1024);
		set_normalize(s);
		return (y);
	}
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

/* Probe positions: up to 600 members (evenly spaced) and their
 * neighbours, the chunk edges around them, random positions, and the
 * ends of the universe.  Sorted. */
static uint64_t *
probes(const set_t *s, size_t *np)
{
	const size_t step = s->n > 600 ? s->n / 600 : 1;
	size_t cap = (s->n / step + 1) * 5 + 64, n = 0, i;
	uint64_t *p = malloc(cap * sizeof(uint64_t));
	uint64_t top = s->n ? s->v[s->n - 1] : 0;

	if (p == NULL)
		exit(2);
	for (i = 0; i < s->n; i += step) {
		uint64_t x = s->v[i], c = x - x % CHUNK;
		p[n++] = x;
		p[n++] = x == 0 ? 0 : x - 1;
		p[n++] = x == UINT64_MAX ? x : x + 1;
		p[n++] = c;
		p[n++] = c + CHUNK - 1;
	}
	for (i = 0; i < 40; i++)
		p[n++] = top ? rnd() % (top + 5000) : rnd() % 5000;
	p[n++] = 0;
	p[n++] = UINT64_MAX - 1;
	p[n++] = UINT64_MAX;
	qsort(p, n, sizeof(uint64_t), cmp_u64);
	*np = n;
	return (p);
}

/* select0 against the sorted-array oracle, the rank0 identity, and (for
 * maps below 2^24) a bit-array sweep over every unset position. */
static int
check_select0(const sm_locator_t *loc, const set_t *s, const uint64_t *p,
    size_t np, const char *what)
{
	const uint64_t top = s->n ? s->v[s->n - 1] : 0;
	size_t i;

	for (i = 0; i < np; i++) {
		const uint64_t x = p[i];
		const uint64_t k0 = x - o_below(s, x); /* unset bits below x */
		const uint64_t want = o_select0(s, k0);

		CHECK(sm_locator_select(loc, k0, false) == want,
		    "%s select0(%llu)", what, (unsigned long long)k0);
		if (!o_contains(s, x) && x != UINT64_MAX)
			CHECK(want == x, "%s select0(rank0(x)) == x at %llu",
			    what, (unsigned long long)x);
		CHECK(sm_locator_select(loc, x, false) == o_select0(s, x),
		    "%s select0(%llu) as k", what, (unsigned long long)x);
	}
	CHECK(sm_locator_select(loc, UINT64_MAX, false) == SM_IDX_MAX,
	    "%s select0(UINT64_MAX)", what);
	CHECK(sm_locator_select(loc, UINT64_MAX - 1 - s->n, false) ==
	        o_select0(s, UINT64_MAX - 1 - s->n),
	    "%s select0 near the top", what);
	if (top < (UINT64_C(1) << 24)) {
		/* Bit array of [0, top + 2048), then every unset position. */
		const uint64_t nb = top + 2048;
		uint8_t *bits = calloc(nb / 8 + 1, 1);
		uint64_t x, k = 0;

		if (bits == NULL)
			exit(2);
		for (i = 0; i < s->n; i++)
			bits[s->v[i] / 8] |= (uint8_t)(1u << (s->v[i] % 8));
		for (x = 0; x < nb; x++) {
			if (bits[x / 8] & (1u << (x % 8)))
				continue;
			if (k % 7 == 0 || x + 3000 > nb)
				CHECK(sm_locator_select(loc, k, false) == x,
				    "%s select0(%llu) bit oracle %llu", what,
				    (unsigned long long)k, (unsigned long long)x);
			k++;
		}
		free(bits);
	}
	return (0);
}

/* Fresh locator == plain == oracle for contains, rank (both values) and
 * select1 over the probe set. */
static int
check_fresh(sm_t *m, const set_t *s, const char *what)
{
	sm_locator_t *loc = sm_locator_build(m);
	size_t np, i, k;
	uint64_t *p = probes(s, &np);

	if (loc == NULL) {
		CHECK(s->n == 0, "%s: build returned NULL for a nonempty map",
		    what);
		free(p);
		return (0);
	}
	for (i = 0; i < np; i++) {
		const uint64_t x = p[i], y = p[(i * 7 + 3) % np];
		const uint64_t lo = x < y ? x : y, hi = x < y ? y : x;
		const int want = o_contains(s, x);
		const size_t r1 = o_rank(s, lo, hi);

		CHECK(sm_locator_contains(loc, x) == want, "%s contains %llu",
		    what, (unsigned long long)x);
		if (i % 32 == 0)
			CHECK(sm_contains(m, x, NULL) == want,
			    "%s plain contains %llu", what,
			    (unsigned long long)x);
		CHECK(sm_locator_rank(loc, lo, hi, true) == r1,
		    "%s rank1 [%llu, %llu]", what, (unsigned long long)lo,
		    (unsigned long long)hi);
		if (i % 32 == 0)
			CHECK(sm_rank(m, lo, hi, true) == r1, "%s plain rank1",
			    what);
		CHECK(sm_locator_rank(loc, 0, x, true) == o_rank(s, 0, x),
		    "%s rank1 [0, %llu]", what, (unsigned long long)x);
		if (i % 32 == 0)
			CHECK(sm_locator_rank(loc, lo, hi, false) ==
			        sm_rank(m, lo, hi, false),
			    "%s rank0 [%llu, %llu]", what,
			    (unsigned long long)lo, (unsigned long long)hi);
		if (hi - lo < UINT64_MAX)
			CHECK(sm_locator_rank(loc, lo, hi, false) ==
			        (size_t)(hi - lo + 1) - r1,
			    "%s rank0 oracle", what);
		CHECK(sm_locator_rank(loc, hi, lo, true) == (lo == hi ? r1 : 0),
		    "%s inverted range", what);
	}
	if (check_select0(loc, s, p, np, what) != 0)
		return (1);
	for (k = 0; k <= s->n; k += 1 + (s->n > 3000 ? rnd() % 64 : 0)) {
		const uint64_t want = k < s->n ? s->v[k] : SM_IDX_MAX;
		CHECK(sm_locator_select(loc, k, true) == want,
		    "%s select1(%zu)", what, k);
		if (k % 32 == 0)
			CHECK(sm_select(m, k, true) == want, "%s plain select1",
			    what);
	}
	CHECK(sm_locator_select(loc, s->n, true) == SM_IDX_MAX,
	    "%s select1(n)", what);
	sm_locator_free(loc);
	free(p);
	return (0);
}

static int
fresh(void)
{
	int trial;

	for (trial = 0; trial < 180; trial++) {
		set_t s = { 0 };
		char what[32];
		sm_t *m = build(trial % 9, &s);

		snprintf(what, sizeof(what), "shape %d trial %d", trial % 9,
		    trial);
		if (check_fresh(m, &s, what) != 0)
			return (1);
		sm_free(m);
		free(s.v);
	}
	return (0);
}

/*
 * Six RLE runs of 2^31 - 4096 bits each, a few windows apart, then single
 * members and a short run, checked against an interval oracle.  The
 * chunks sit in one group of 16 that spans far more than the 2^27 bits a
 * compact per-chunk record can express, so the group is stored with
 * absolute values (wide), and the set-bit prefixes inside it pass 2^32.  Each run stays below 2^31 bits and the runs do not touch: on
 * 5.8.5 sm_add_range truncates a longer run to its low 31 bits (a
 * separate defect), and adjacent runs coalesce into one.
 */
#define HUGE_RUN ((UINT64_C(1) << 31) - 4096)
#define HUGE_AT(i) (4096 + (uint64_t)(i) * (HUGE_RUN + 4 * CHUNK))
static int
huge_runs(void)
{
	static const uint64_t iv[][2] = {
		{ HUGE_AT(0), HUGE_AT(0) + HUGE_RUN },
		{ HUGE_AT(1), HUGE_AT(1) + HUGE_RUN },
		{ HUGE_AT(2), HUGE_AT(2) + HUGE_RUN },
		{ HUGE_AT(3), HUGE_AT(3) + HUGE_RUN },
		{ HUGE_AT(4), HUGE_AT(4) + HUGE_RUN },
		{ HUGE_AT(5), HUGE_AT(5) + HUGE_RUN },
		{ UINT64_C(1) << 36, (UINT64_C(1) << 36) + 1 },
		{ (UINT64_C(1) << 36) + 4 * CHUNK,
		    (UINT64_C(1) << 36) + 4 * CHUNK + 1 },
		{ UINT64_C(1) << 40, (UINT64_C(1) << 40) + 77 },
	};
	const size_t niv = sizeof(iv) / sizeof(iv[0]);
	uint64_t before[sizeof(iv) / sizeof(iv[0]) + 1];
	sm_t *m = sm_create(4096);
	sm_locator_t *loc;
	size_t r, t;

	CHECK(m != NULL, "create");
	before[0] = 0;
	for (r = 0; r < niv; r++) {
		CHECK(sm_add_range(m, iv[r][0], iv[r][1]), "add_range %zu", r);
		before[r + 1] = before[r] + (iv[r][1] - iv[r][0]);
	}
	loc = sm_locator_build(m);
	CHECK(loc != NULL, "locator");
	for (r = 0; r < niv; r++) {
		const uint64_t lo = iv[r][0], hi = iv[r][1];
		const uint64_t probe[6] = { lo - 1, lo, lo + 1, hi - 2, hi - 1,
			hi };
		const uint64_t gap_lo = r ? iv[r - 1][1] : 0;
		for (t = 0; t < 6; t++) {
			const uint64_t x = probe[t];
			const int in = x >= lo && x < hi;
			/* Set bits in [0, x]: whole earlier runs plus the part
			 * of this one at or below x. */
			const uint64_t upto = before[r] +
			    (x < lo ? 0 : (x >= hi ? hi - lo : x - lo + 1));
			CHECK(sm_locator_contains(loc, x) == in,
			    "huge contains %llu", (unsigned long long)x);
			CHECK(sm_locator_rank(loc, 0, x, true) == upto,
			    "huge rank %llu", (unsigned long long)x);
			CHECK(sm_locator_rank(loc, 0, x, false) == x + 1 - upto,
			    "huge rank0 %llu", (unsigned long long)x);
		}
		/* The first, a middle and the last member of the run. */
		CHECK(sm_locator_select(loc, before[r], true) == lo,
		    "huge select1 first of %zu", r);
		CHECK(sm_locator_select(loc, before[r] + (hi - lo) / 2, true) ==
		        lo + (hi - lo) / 2,
		    "huge select1 middle of %zu", r);
		CHECK(sm_locator_select(loc, before[r + 1] - 1, true) == hi - 1,
		    "huge select1 last of %zu", r);
		/* Unset bits before lo number lo - before[r]: the gap's first
		 * and last unset positions. */
		CHECK(sm_locator_select(loc, gap_lo - before[r], false) == gap_lo,
		    "huge select0 gap start %zu", r);
		CHECK(sm_locator_select(loc, lo - 1 - before[r], false) == lo - 1,
		    "huge select0 gap end %zu", r);
		CHECK(sm_locator_select(loc, lo - before[r], false) == hi,
		    "huge select0 after run %zu", r);
	}
	CHECK(sm_locator_select(loc, before[niv], true) == SM_IDX_MAX,
	    "huge select1 past the end");
	sm_locator_free(loc);
	sm_free(m);
	return (0);
}

/*
 * A locator built, then the map mutated by a change that inserts or
 * removes a chunk.  The fingerprint differs, so every query must answer
 * for the live map.  The map is sized so that sm_add never grows (and so
 * never moves) the map: the locator holds a pointer to it.
 */
static int
structural(void)
{
	int trial;

	for (trial = 0; trial < 60; trial++) {
		sm_t *m = sm_create(1 << 20);
		sm_locator_t *loc;
		uint64_t k, x, n = 50 + rnd() % 500;

		if (m == NULL)
			return (1);
		for (k = 0; k < n; k++)
			sm_add(m, k * 5 * CHUNK + rnd() % CHUNK);
		if (trial % 4 == 3)
			sm_add_range(m, n * 5 * CHUNK, n * 5 * CHUNK + 9000);
		loc = sm_locator_build(m);
		CHECK(loc != NULL, "build");
		switch (trial % 3) {
		case 0: /* a new chunk in the middle */
			x = (rnd() % n) * 5 * CHUNK + 2 * CHUNK + 17;
			CHECK(sm_add(m, x) == x, "add");
			break;
		case 1: /* a new chunk at the end */
			x = (n + 100) * 5 * CHUNK;
			CHECK(sm_add(m, x) == x, "add");
			break;
		default: /* remove a chunk's only member */
			x = sm_select(m, rnd() % n, true);
			CHECK(sm_remove(m, x) == x, "remove");
			break;
		}
		for (k = 0; k < 400; k++) {
			const uint64_t q = (k % 2) ? x + k / 2 - 100
			                           : rnd() % ((n + 101) * 5 * CHUNK);
			CHECK(sm_locator_contains(loc, q) ==
			        sm_contains(m, q, NULL),
			    "stale contains %llu", (unsigned long long)q);
			CHECK(sm_locator_rank(loc, 0, q, true) ==
			        sm_rank(m, 0, q, true),
			    "stale rank %llu", (unsigned long long)q);
		}
		for (k = 0; k <= n; k += 1 + rnd() % 8)
			CHECK(sm_locator_select(loc, k, true) ==
			        sm_select(m, k, true),
			    "stale select %llu", (unsigned long long)k);
		sm_locator_free(loc);
		sm_free(m);
	}
	return (0);
}

static int
nulls(void)
{
	sm_t *m = sm_create(1024);

	CHECK(sm_locator_build(NULL) == NULL, "build(NULL)");
	CHECK(sm_locator_build(m) == NULL, "build(empty)");
	CHECK(!sm_locator_contains(NULL, 5), "contains(NULL)");
	CHECK(sm_locator_rank(NULL, 0, 9, true) == 0, "rank(NULL)");
	CHECK(sm_locator_select(NULL, 0, true) == SM_IDX_MAX, "select(NULL)");
	CHECK(sm_locator_select(NULL, 0, false) == SM_IDX_MAX,
	    "select0(NULL)");

	sm_locator_free(NULL);
	sm_free(m);
	return (0);
}

int
main(void)
{
	if (nulls() != 0 || fresh() != 0 || huge_runs() != 0 ||
	    structural() != 0)
		return (1);
	printf("test_locator: ok\n");
	return (0);
}
