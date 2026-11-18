/* SPDX-License-Identifier: MIT */
/*
 * test_top_universe.c - maps with members in the top chunk of the 64-bit
 * universe, [2^64 - 2048, 2^64 - 1).
 *
 * The top chunk ends at 2^64, so through 5.8.5 every `start + capacity`
 * (or `start + length`) computed for it wrapped to 0 and the chunk looked
 * as if it ended before everything (W3).  sm_and, sm_andnot and sm_or of
 * valid sm_add-built maps then lost members or emitted chunks out of
 * order, sm_split never found a split point in it, and the locator never
 * found a member in it.  Separately, __sm_emit_run rounded an unaligned
 * run start up to the next chunk boundary, which wraps to 0 in the top
 * chunk (W1): sm_xor, sm_add_many and sm_create_from_array emitted a
 * chunk at an unaligned start that sm_validate rejects and that
 * sm_next_member skips.
 *
 * Checks, each against a sorted-array oracle:
 *   1. the minimized regression cases from the 5.8.5 investigation;
 *   2. a randomized differential over maps whose members lie in the top
 *      two chunks (and a few low ones), for every set operation, its
 *      in-place and _cardinality variants, and the predicates, with
 *      sm_validate and a full read-back of every result.
 */
#include <sm.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN  64
#define T(x)  (UINT64_MAX - (uint64_t)(x)) /* 2^64 - 1 - x */
#define TOPC  (UINT64_MAX - 2047)          /* 2^64 - 2048 */

static int failures;

#define CHECK(c, ...)                                                       \
	do {                                                                \
		if (!(c)) {                                                 \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                       \
			fprintf(stderr, "\n");                              \
			failures++;                                         \
		}                                                           \
	} while (0)

static uint64_t rs = 0x2545F4914F6CDD1DULL;

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
	const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return ((x > y) - (x < y));
}

static size_t
uniq(uint64_t *v, size_t n)
{
	size_t k, d = 0;

	qsort(v, n, sizeof(*v), cmp_u64);
	for (k = 0; k < n; k++)
		if (d == 0 || v[k] != v[d - 1])
			v[d++] = v[k];
	return (d);
}

static int
in_set(const uint64_t *v, size_t n, uint64_t x)
{
	return (bsearch(&x, v, n, sizeof(*v), cmp_u64) != NULL);
}

/* A map holding v[0..n), one sm_add per member. */
static sm_t *
by_add(const uint64_t *v, size_t n)
{
	sm_t *m = sm_create(1024);
	size_t k;

	for (k = 0; m != NULL && k < n; k++)
		if (sm_add_grow(&m, v[k]) != v[k]) {
			sm_free(m);
			return (NULL);
		}
	return (m);
}

/* A map holding [lo, hi) as one sm_add_range. */
static sm_t *
by_range(uint64_t lo, uint64_t hi)
{
	sm_t *m = sm_create(1024);

	if (m != NULL && !sm_add_range(m, lo, hi)) {
		sm_free(m);
		return (NULL);
	}
	return (m);
}

struct coll {
	uint64_t *v;
	size_t n;
	size_t cap;
	int over;
};

static void
scan_fn(uint64_t vec[], size_t n, void *aux)
{
	struct coll *c = aux;
	size_t i;

	for (i = 0; i < n; i++) {
		if (c->n == c->cap) {
			c->over = 1;
			return;
		}
		c->v[c->n++] = vec[i];
	}
}

/*
 * 0 when m holds exactly v[0..n): sm_validate, cardinality, contains on
 * every member and its neighbours, the sm_next_member and sm_prev_member
 * sequences, sm_scan, sm_to_array and minimum/maximum.  A NULL or empty
 * m matches only n == 0.  Prints `what` on the first mismatch.
 */
static int
same(const sm_t *m, const uint64_t *v, size_t n, const char *what)
{
	static const int64_t near[] = { -2, -1, 1, 2 };
	static uint64_t got[16384], seen[16384];
	uint64_t i;
	size_t k, d, nr = sizeof(got) / sizeof(got[0]);
	struct coll c;

	if (n == 0) {
		if (m != NULL && !sm_is_empty(m)) {
			fprintf(stderr, "  %s: want empty, card %zu\n", what,
			    sm_cardinality((sm_t *)m));
			return (1);
		}
		return (0);
	}
	if (m == NULL) {
		fprintf(stderr, "  %s: NULL, want %zu members\n", what, n);
		return (1);
	}
	if (!sm_validate(m)) {
		fprintf(stderr, "  %s: sm_validate false\n", what);
		return (1);
	}
	if (sm_cardinality((sm_t *)m) != n) {
		fprintf(stderr, "  %s: card %zu want %zu\n", what,
		    sm_cardinality((sm_t *)m), n);
		return (1);
	}
	for (k = 0; k < n; k++) {
		if (!sm_contains(m, v[k], NULL)) {
			fprintf(stderr, "  %s: lacks 2^64-%llu\n", what,
			    (unsigned long long)(0 - v[k]));
			return (1);
		}
		for (d = 0; d < 4; d++) {
			const uint64_t y = v[k] + (uint64_t)near[d];

			if (!in_set(v, n, y) && sm_contains(m, y, NULL)) {
				fprintf(stderr, "  %s: extra %llu\n", what,
				    (unsigned long long)y);
				return (1);
			}
		}
	}
	k = 0;
	i = SM_IDX_MAX;
	while ((i = sm_next_member(m, i, NULL)) != SM_IDX_MAX) {
		if (k >= n || i != v[k]) {
			fprintf(stderr, "  %s: next_member #%zu\n", what, k);
			return (1);
		}
		k++;
	}
	if (k != n) {
		fprintf(stderr, "  %s: next_member gave %zu of %zu\n", what,
		    k, n);
		return (1);
	}
	k = n;
	i = SM_IDX_MAX;
	while ((i = sm_prev_member(m, i, NULL)) != SM_IDX_MAX) {
		if (k == 0 || i != v[k - 1]) {
			fprintf(stderr, "  %s: prev_member #%zu\n", what, k);
			return (1);
		}
		k--;
	}
	if (k != 0) {
		fprintf(stderr, "  %s: prev_member stopped at %zu\n", what, k);
		return (1);
	}
	c.v = seen;
	c.n = 0;
	c.cap = sizeof(seen) / sizeof(seen[0]);
	c.over = 0;
	sm_scan(m, scan_fn, 0, &c);
	if (c.over || c.n != n || memcmp(c.v, v, n * sizeof(*v)) != 0) {
		fprintf(stderr, "  %s: sm_scan gave %zu\n", what, c.n);
		return (1);
	}
	sm_to_array(m, got, &nr);
	if (nr != n || memcmp(got, v, n * sizeof(*v)) != 0) {
		fprintf(stderr, "  %s: sm_to_array gave %zu\n", what, nr);
		return (1);
	}
	if (sm_minimum(m) != v[0] || sm_maximum(m) != v[n - 1]) {
		fprintf(stderr, "  %s: min/max\n", what);
		return (1);
	}
	return (0);
}

/* The sorted members of a OP b: 0 or, 1 and, 2 andnot, 3 xor. */
static size_t
oracle(int op, const uint64_t *a, size_t na, const uint64_t *b, size_t nb,
    uint64_t *out)
{
	size_t i = 0, j = 0, n = 0;

	while (i < na || j < nb) {
		const uint64_t x =
		    (j >= nb || (i < na && a[i] < b[j])) ? a[i] : b[j];
		const int ia = i < na && a[i] == x, ib = j < nb && b[j] == x;
		int keep;

		i += (size_t)ia;
		j += (size_t)ib;
		keep = op == 0 ? ia || ib : op == 1 ? ia && ib :
		    op == 2 ? ia && !ib : ia != ib;
		if (keep)
			out[n++] = x;
	}
	return (n);
}

/* ---- 1. the regression cases ---------------------------------------- */

static void
regressions_setops(void)
{
	sm_t *a, *b, *r, *o;

	/* 1: the teammate's case, visible only in iteration. */
	{
		const uint64_t va[] = { 100, T(4000), T(3) };
		const uint64_t vb[] = { 5, 900 };
		const uint64_t want[] = { 5, 100, 900, T(4000), T(3) };

		a = by_add(va, 3);
		b = by_add(vb, 2);
		r = sm_xor(a, b);
		CHECK(same(r, want, 5, "xor 1") == 0, "case 1");
		sm_free(r);
		r = sm_or(a, b);
		CHECK(same(r, want, 5, "or 1") == 0, "case 1 or");
		sm_free(r);
		sm_free(a);
		sm_free(b);
	}
	/* 2 */
	{
		const uint64_t va[] = { 2043, T(3) }, vb[] = { 5 };
		const uint64_t want[] = { 5, 2043, T(3) };

		a = by_add(va, 2);
		b = by_add(vb, 1);
		r = sm_xor(a, b);
		CHECK(same(r, want, 3, "xor 2") == 0, "case 2");
		sm_free(r);
		sm_free(a);
		sm_free(b);
	}
	/* 3: a wire round trip of a builder-made top-chunk map. */
	{
		const uint64_t v[] = { 5, T(5) };
		size_t len;
		uint8_t *buf;

		a = sm_create_from_array(v, 2);
		CHECK(same(a, v, 2, "from_array 3") == 0, "case 3");
		len = sm_serialized_size(a);
		buf = malloc(len);
		CHECK(buf != NULL && sm_serialize(a, buf, len) == len,
		    "serialize");
		r = buf ? sm_deserialize(buf, len) : NULL;
		CHECK(same(r, v, 2, "deserialize 3") == 0, "case 3 wire");
		o = sm_open_copy(sm_get_data(a), sm_get_size(a), 0);
		CHECK(same(o, v, 2, "open_copy 3") == 0, "case 3 open");
		free(buf);
		sm_free(r);
		sm_free(o);
		sm_free(a);
	}
	/* 7, 8 */
	{
		const uint64_t v7[] = { T(276) };
		const uint64_t va[] = { T(2952), T(39) }, vb[] = { T(849) };
		const uint64_t v4[] = { T(3) }, v5[] = { 5 };

		a = by_add(v7, 1);
		r = sm_and(a, a);
		CHECK(same(r, v7, 1, "and 7") == 0, "case 7");
		sm_free(r);
		sm_free(a);
		a = by_add(va, 2);
		b = by_add(vb, 1);
		r = sm_andnot(a, b);
		CHECK(same(r, va, 2, "andnot 8") == 0, "case 8");
		sm_free(r);
		sm_free(a);
		sm_free(b);
		a = by_add(v4, 1);
		b = by_add(v5, 1);
		r = sm_andnot(a, b);
		CHECK(same(r, v4, 1, "andnot 8b") == 0, "case 8b");
		sm_free(r);
		sm_free(a);
		sm_free(b);
	}
	/* 9 */
	{
		const uint64_t va[] = { T(1139) }, vb[] = { T(2449) };
		const uint64_t want[] = { T(2449), T(1139) };
		const uint64_t v4[] = { T(3) }, v5[] = { 5 };
		const uint64_t w45[] = { 5, T(3) };

		a = by_add(va, 1);
		b = by_add(vb, 1);
		r = sm_or(a, b);
		CHECK(same(r, want, 2, "or 9") == 0, "case 9");
		sm_free(r);
		sm_free(a);
		sm_free(b);
		a = by_add(v4, 1);
		b = by_add(v5, 1);
		r = sm_or(a, b);
		CHECK(same(r, w45, 2, "or 9b") == 0, "case 9b");
		sm_free(r);
		sm_free(a);
		sm_free(b);
	}
}

/* Members [lo, hi) into v; returns the count. */
static size_t
fill(uint64_t *v, uint64_t lo, uint64_t hi)
{
	size_t n = 0;

	while (lo < hi)
		v[n++] = lo++;
	return (n);
}

/* 10, 11, 12: an RLE run in the top chunks, and sm_split in the top. */
static void
regressions_rle(void)
{
	static uint64_t v[8192], w[8192];
	const uint64_t lo = UINT64_MAX - 8191;
	sm_t *R, *Q, *r, *o;
	sm_locator_t *L;
	size_t n, k;

	/* R = [2^64 - 8192, 2^64 - 1), the largest run below SM_IDX_MAX. */
	n = fill(v, lo, UINT64_MAX);
	R = by_range(lo, UINT64_MAX);
	CHECK(same(R, v, n, "R") == 0, "R itself");
	r = sm_and(R, R);
	CHECK(same(r, v, n, "and R R") == 0, "case 10 and");
	sm_free(r);
	r = sm_or(R, R);
	CHECK(same(r, v, n, "or R R") == 0, "case 10 or");
	sm_free(r);
	/* Q = R less its top member: R andnot Q and R xor Q are that one. */
	Q = by_range(lo, UINT64_MAX - 1);
	r = sm_andnot(R, Q);
	CHECK(same(r, &v[n - 1], 1, "andnot R Q") == 0, "case 10 andnot");
	sm_free(r);
	r = sm_xor(R, Q);
	CHECK(same(r, &v[n - 1], 1, "xor R Q") == 0, "case 10 xor");
	sm_free(r);
	sm_free(Q);
	/* sm_remove inside the run and at its top member. */
	r = sm_copy(R);
	sm_remove(r, T(10));
	for (k = 0; k < n && v[k] != T(10); k++)
		;
	memcpy(w, v, k * sizeof(*v));
	memcpy(w + k, v + k + 1, (n - k - 1) * sizeof(*v));
	CHECK(same(r, w, n - 1, "remove R") == 0, "case 10 remove");
	sm_free(r);
	r = sm_copy(R);
	sm_remove(r, T(1));
	CHECK(same(r, v, n - 1, "remove R top") == 0, "case 10 remove top");
	sm_free(r);
	/* sm_split inside the top chunk. */
	r = sm_copy(R);
	o = sm_create(1024);
	sm_split(r, T(100), o);
	for (k = 0; k < n && v[k] < T(100); k++)
		;
	CHECK(same(r, v, k, "split R left") == 0, "case 10 split left");
	CHECK(same(o, v + k, n - k, "split R right") == 0,
	    "case 10 split right");
	sm_free(r);
	sm_free(o);
	/* The locator in the top chunk. */
	L = sm_locator_build(R);
	CHECK(L != NULL && sm_locator_contains(L, T(5)), "case 10 locator");
	CHECK(L != NULL && !sm_locator_contains(L, UINT64_MAX),
	    "case 10 locator SM_IDX_MAX");
	sm_locator_free(L);
	/* Overlap with a sparse chunk inside the run's span (V1's input). */
	{
		const uint64_t vb[] = { T(5000) };

		o = by_add(vb, 1);
		r = sm_or(R, o);
		CHECK(same(r, v, n, "or R {2^64-5001}") == 0, "case 10 or V1");
		sm_free(r);
		sm_free(o);
	}
	sm_free(R);

	/* 11: a one-window run [2^64 - 2048, 2^64 - 2) less one member. */
	n = fill(v, TOPC, T(1));
	R = by_range(TOPC, T(1));
	sm_remove(R, T(100));
	for (k = 0; k < n && v[k] != T(100); k++)
		;
	memmove(v + k, v + k + 1, (n - k - 1) * sizeof(*v));
	CHECK(same(R, v, n - 1, "one-window remove") == 0, "case 11");
	sm_free(R);

	/* 12 */
	{
		const uint64_t vs[] = { T(40), T(39), T(5) };

		r = by_add(vs, 3);
		o = sm_create(1024);
		sm_split(r, T(39), o);
		CHECK(same(r, vs, 1, "split 12 left") == 0, "case 12 left");
		CHECK(same(o, vs + 1, 2, "split 12 right") == 0,
		    "case 12 right");
		sm_free(r);
		sm_free(o);
	}
	/* The locator over sparse top chunks. */
	{
		const uint64_t vl[] = { 7, T(3000), T(40), T(39), T(5) };

		r = by_add(vl, 5);
		L = sm_locator_build(r);
		for (k = 0; k < 5; k++)
			CHECK(L != NULL && sm_locator_contains(L, vl[k]),
			    "locator sparse top %zu", k);
		sm_locator_free(L);
		sm_free(r);
	}
}

/* ---- 2. randomized differential in the top two chunks --------------- */

/* Up to MAXN / 2 members: a few low, some in the chunk below the top,
 * some in the top chunk (below SM_IDX_MAX), sometimes short runs. */
static size_t
gen(uint64_t *v)
{
	size_t n = 0, k;
	const size_t want = rnd() % 8;

	for (k = 0; k < want; k++) {
		uint64_t x, len;

		switch (rnd() % 4) {
		case 0:
			x = rnd() % 5000;
			break;
		case 1:
			x = TOPC - 1 - rnd() % 2048;
			break;
		default:
			x = TOPC + rnd() % 2047;
			break;
		}
		len = rnd() % 4 == 0 ? 1 + rnd() % 6 : 1;
		while (len-- > 0 && n < MAXN / 2 && x != UINT64_MAX)
			v[n++] = x++;
	}
	return (uniq(v, n));
}

/* A map holding v through one of the builders. */
static sm_t *
build(int path, const uint64_t *v, size_t n)
{
	sm_t *m;
	sm_cursor_t c = SM_CURSOR_INIT;
	size_t k;

	switch (path) {
	case 0:
		return (by_add(v, n));
	case 1:
		m = sm_create(256);
		if (n > 0 && !sm_add_many_grow(&m, v, n)) {
			sm_free(m);
			return (NULL);
		}
		return (m);
	case 2:
		return (n == 0 ? sm_create(256) : sm_create_from_array(v, n));
	default:
		m = sm_create(256);
		for (k = 0; m != NULL && k < n; k++)
			if (sm_add_grow_cursor(&m, v[k], &c) != v[k]) {
				sm_free(m);
				return (NULL);
			}
		return (m);
	}
}

static void
setops_pair(const uint64_t *va, size_t na, const uint64_t *vb, size_t nb,
    int pa, int pb)
{
	static const char *on[4] = { "or", "and", "andnot", "xor" };
	uint64_t w[2 * MAXN];
	sm_t *a = build(pa, va, na), *b = build(pb, vb, nb), *r, *d;
	int op;

	CHECK(same(a, va, na, "build a") == 0, "build path %d", pa);
	CHECK(same(b, vb, nb, "build b") == 0, "build path %d", pb);
	for (op = 0; op < 4; op++) {
		const size_t nw = oracle(op, va, na, vb, nb, w);

		r = op == 0 ? sm_or(a, b) : op == 1 ? sm_and(a, b) :
		    op == 2 ? sm_andnot(a, b) : sm_xor(a, b);
		CHECK(same(r, w, nw, on[op]) == 0, "sm_%s", on[op]);
		sm_free(r);
		d = sm_copy(a);
		d = op == 0 ? sm_union_inplace(d, b) :
		    op == 1 ? sm_intersection_inplace(d, b) :
		    op == 2 ? sm_difference_inplace(d, b) :
		    sm_xor_inplace(d, b);
		CHECK(same(d, w, nw, on[op]) == 0, "sm_%s inplace", on[op]);
		sm_free(d);
	}
	CHECK(sm_union_cardinality(a, b) == oracle(0, va, na, vb, nb, w),
	    "union_cardinality");
	CHECK(sm_intersection_cardinality(a, b) ==
	        oracle(1, va, na, vb, nb, w),
	    "intersection_cardinality");
	CHECK(sm_difference_cardinality(a, b) == oracle(2, va, na, vb, nb, w),
	    "difference_cardinality");
	CHECK(sm_xor_cardinality(a, b) == oracle(3, va, na, vb, nb, w),
	    "xor_cardinality");
	CHECK(sm_nonempty_difference(a, b) ==
	        (oracle(2, va, na, vb, nb, w) > 0),
	    "nonempty_difference");
	CHECK(sm_is_subset(a, b) == (oracle(2, va, na, vb, nb, w) == 0),
	    "is_subset");
	CHECK(sm_overlap(a, b) == (oracle(1, va, na, vb, nb, w) > 0),
	    "overlap");
	{
		const size_t ni = oracle(1, va, na, vb, nb, w);
		const size_t nu = oracle(0, va, na, vb, nb, w);
		const double want = nu == 0 ? 0.0 : (double)ni / (double)nu;
		const int eq = na == nb && memcmp(va, vb, na * 8) == 0;

		CHECK(sm_jaccard_index(a, b) == want, "jaccard");
		CHECK(sm_equals(a, b) == eq, "equals");
		CHECK(!eq || sm_hash(a) == sm_hash(b), "hash");
		CHECK((sm_compare(a, b) == 0) == eq, "compare");
	}
	r = sm_extract_range(a, 0, UINT64_MAX);
	CHECK(same(r, va, na, "extract_range") == 0, "extract_range");
	sm_free(r);
	sm_free(a);
	sm_free(b);
}

static void
differential(long trials)
{
	uint64_t va[MAXN], vb[MAXN];
	long t;

	for (t = 0; t < trials && failures < 20; t++) {
		const size_t na = gen(va), nb = gen(vb);

		setops_pair(va, na, vb, nb, (int)(rnd() % 4), (int)(rnd() % 4));
	}
}

int
main(void)
{
	regressions_setops();
	regressions_rle();
	differential(3000);
	if (failures != 0) {
		fprintf(stderr, "test_top_universe: %d failure(s)\n", failures);
		return (1);
	}
	printf("test_top_universe: ok\n");
	return (0);
}
