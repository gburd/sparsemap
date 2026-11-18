/* SPDX-License-Identifier: MIT */
/*
 * test_op_iter.c - lazy iteration over a OP b (5.9.0).
 *
 * sm_next_member_op and sm_scan_op walk the members of sm_and / sm_or /
 * sm_andnot / sm_xor without building the result map.  For random pairs of
 * maps of every shape (empty, NULL, small set, sparse with gaps between
 * chunks, clustered on chunk boundaries, RLE runs, identical, disjoint, top
 * of the universe) and all four ops, the member sequence from
 * sm_next_member_op (with and without cursors) and from sm_scan_op (with
 * several skip values) must equal sm_to_array of the built map, which in
 * turn must equal a sorted-array oracle.  A cost check runs AND, ANDNOT and
 * XOR over a 2^31 - 1 bit RLE run against an empty, a sparse and an
 * identical map; walking that run bit by bit would take seconds.
 */
#include <sm.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__SANITIZE_ADDRESS__) || defined(SM_COVERAGE_BUILD)
#define RUN_CEILING_MS 2000.0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) ||                                       \
    __has_feature(undefined_behavior_sanitizer)
#define RUN_CEILING_MS 2000.0
#else
#define RUN_CEILING_MS 200.0
#endif
#else
#define RUN_CEILING_MS 200.0
#endif

#define MAXN 40000

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

/* Sort and drop duplicates; returns the new count. */
static size_t
uniq(uint64_t *v, size_t n)
{
	size_t k, d = 0;

	qsort(v, n, sizeof(uint64_t), cmp_u64);
	for (k = 0; k < n; k++)
		if (d == 0 || v[k] != v[d - 1])
			v[d++] = v[k];
	return (d);
}

/* Members of a random map of the given shape into v; returns the count. */
static size_t
gen(int shape, uint64_t *v)
{
	size_t n = 0, k;
	uint64_t b, len;

	switch (shape) {
	case 0: /* empty */
		break;
	case 1: /* small set */
		for (k = 0; k < 1 + rnd() % 80; k++)
			v[n++] = rnd() % 1024;
		break;
	case 2: /* sparse, gaps between chunks */
		for (k = 0; k < 1 + rnd() % 300; k++)
			v[n++] = rnd() % 20000000;
		break;
	case 3: /* clustered on chunk boundaries */
		for (k = 0; k < 1 + rnd() % 60; k++) {
			b = (rnd() % 300) * 2048;
			v[n++] = b;
			v[n++] = b + 2047;
			v[n++] = b + 2048 + rnd() % 64;
			if (b > 0)
				v[n++] = b - 1;
		}
		break;
	case 4: /* RLE runs, some abutting chunk boundaries */
		for (k = 0; k < 1 + rnd() % 4; k++) {
			b = (rnd() % 200) * 1024 + (rnd() % 2 ? 0 : rnd() % 2048);
			len = 1 + rnd() % 6000;
			while (len-- > 0 && n < MAXN / 2)
				v[n++] = b++;
		}
		for (k = 0; k < rnd() % 30; k++)
			v[n++] = rnd() % 400000;
		break;
	default: /* top of the universe, below the UINT64_MAX sentinel */
		for (k = 0; k < 1 + rnd() % 20; k++)
			v[n++] = UINT64_MAX - 1 - rnd() % 5000;
		v[n++] = rnd() % 5000;
		break;
	}
	return (uniq(v, n));
}

/* Build a map from sorted members, one cursor-threaded add each.  Not
 * sm_add_many_grow: through 5.8.5 it drops a run that ends at UINT64_MAX
 * (the run's exclusive end wraps to 0). */
static sm_t *
build(const uint64_t *v, size_t n)
{
	sm_t *m = sm_create(256);
	sm_cursor_t c = SM_CURSOR_INIT;
	size_t k;

	for (k = 0; m != NULL && k < n; k++)
		if (sm_add_grow_cursor(&m, v[k], &c) != v[k]) {
			sm_free(m);
			return (NULL);
		}
	return (m);
}

/* True when some member lies in the last chunk below 2^64.  Through
 * 5.8.5 sm_or and sm_xor can lose members there (a = {100, 2^64 - 4001,
 * 2^64 - 4}, b = {5, 900} loses 2^64 - 4), so the built-map cross-check
 * is skipped for such pairs. */
static int
in_top_chunk(const uint64_t *v, size_t n)
{
	return (n > 0 && v[n - 1] >= UINT64_MAX - 2047);
}

/* Oracle: the sorted members of va OP vb. */
static size_t
oracle(int op, const uint64_t *va, size_t na, const uint64_t *vb, size_t nb,
    uint64_t *out)
{
	size_t i = 0, j = 0, n = 0;

	while (i < na || j < nb) {
		uint64_t x;
		int in_a, in_b, keep;

		if (j >= nb || (i < na && va[i] < vb[j]))
			x = va[i];
		else
			x = vb[j];
		in_a = i < na && va[i] == x;
		in_b = j < nb && vb[j] == x;
		i += (size_t)in_a;
		j += (size_t)in_b;
		keep = op == SM_OP_AND ? in_a && in_b
		    : op == SM_OP_OR ? in_a || in_b
		    : op == SM_OP_ANDNOT ? in_a && !in_b
		    : in_a != in_b;
		if (keep)
			out[n++] = x;
	}
	return (n);
}

struct collect {
	uint64_t *v;
	size_t n;
	size_t cap;
	int bad;
};

static void
collect_fn(uint64_t vec[], size_t n, void *aux)
{
	struct collect *c = aux;
	size_t i;

	if (n == 0 || n > 64)
		c->bad = 1;
	for (i = 0; i < n; i++) {
		if (c->n >= c->cap) {
			c->bad = 1;
			return;
		}
		c->v[c->n++] = vec[i];
	}
}

static sm_t *
built_op(int op, const sm_t *a, const sm_t *b)
{
	switch (op) {
	case SM_OP_AND:
		return (sm_and(a, b));
	case SM_OP_OR:
		return (sm_or(a, b));
	case SM_OP_ANDNOT:
		return (sm_andnot(a, b));
	default:
		return (sm_xor(a, b));
	}
}

static int
check_pair(const sm_t *a, const sm_t *b, const uint64_t *va, size_t na,
    const uint64_t *vb, size_t nb, uint64_t *want, uint64_t *got)
{
	static const size_t skips[] = { 0, 1, 63, 64, 65, 1000 };
	int op;

	for (op = SM_OP_AND; op <= SM_OP_XOR; op++) {
		const size_t nw = oracle(op, va, na, vb, nb, want);
		sm_t *r = built_op(op, a, b);
		size_t nr = MAXN * 4, k, s;
		sm_cursor_t ca = SM_CURSOR_INIT, cb = SM_CURSOR_INIT;
		uint64_t i;
		int pass;

		/* The built map agrees with the oracle (sanity of the oracle),
		 * except where the built operation is known to lose bits. */
		sm_to_array(r, got, &nr);
		if (r == NULL)
			nr = 0;
		if (!in_top_chunk(va, na) && !in_top_chunk(vb, nb) &&
		    (nr != nw || memcmp(got, want, nw * sizeof(uint64_t)) != 0)) {
			fprintf(stderr, "FAIL op %d: built map != oracle\n", op);
			return (1);
		}
		sm_free(r);

		/* sm_next_member_op, without and then with cursors. */
		for (pass = 0; pass < 2; pass++) {
			k = 0;
			i = SM_IDX_MAX;
			while ((i = sm_next_member_op(a, b, (sm_op_t)op, i,
			            pass ? &ca : NULL, pass ? &cb : NULL)) !=
			    SM_IDX_MAX) {
				if (k >= nw || i != want[k]) {
					fprintf(stderr, "FAIL op %d pass %d: member "
					    "%zu is %llu, want %llu\n", op, pass, k,
					    (unsigned long long)i,
					    (unsigned long long)(k < nw ? want[k] : 0));
					return (1);
				}
				k++;
			}
			if (k != nw) {
				fprintf(stderr, "FAIL op %d pass %d: %zu of %zu "
				    "members\n", op, pass, k, nw);
				return (1);
			}
		}
		/* Arbitrary bounds, cursors kept across non-monotone calls. */
		for (k = 0; k < 200; k++) {
			const uint64_t q = (nw > 0 && k % 2) ? want[rnd() % nw] -
			    rnd() % 2 : rnd() % 30000000;
			size_t lo = 0, hi = nw;
			uint64_t expect;

			while (lo < hi) { /* first want[] > q */
				const size_t mid = lo + (hi - lo) / 2;
				if (want[mid] <= q)
					lo = mid + 1;
				else
					hi = mid;
			}
			expect = lo < nw ? want[lo] : SM_IDX_MAX;
			if (q == SM_IDX_MAX)
				expect = nw > 0 ? want[0] : SM_IDX_MAX;
			if (sm_next_member_op(a, b, (sm_op_t)op, q, &ca, &cb) !=
			    expect) {
				fprintf(stderr, "FAIL op %d bound %llu\n", op,
				    (unsigned long long)q);
				return (1);
			}
		}
		/* sm_scan_op with several skips. */
		for (s = 0; s < sizeof(skips) / sizeof(skips[0]); s++) {
			struct collect c = { got, 0, MAXN * 4, 0 };
			const size_t from = skips[s] < nw ? skips[s] : nw;

			sm_scan_op(a, b, (sm_op_t)op, collect_fn, skips[s], &c);
			if (c.bad || c.n != nw - from ||
			    memcmp(got, want + from,
				(nw - from) * sizeof(uint64_t)) != 0) {
				fprintf(stderr, "FAIL sm_scan_op op %d skip %zu: "
				    "%zu members, want %zu\n", op, skips[s], c.n,
				    nw - from);
				return (1);
			}
		}
	}
	return (0);
}

static int
pairs(void)
{
	uint64_t *va = malloc(MAXN * 2 * sizeof(uint64_t));
	uint64_t *vb = malloc(MAXN * 2 * sizeof(uint64_t));
	uint64_t *want = malloc(MAXN * 4 * sizeof(uint64_t));
	uint64_t *got = malloc(MAXN * 4 * sizeof(uint64_t));
	int trial;

	if (va == NULL || vb == NULL || want == NULL || got == NULL)
		return (1);
	for (trial = 0; trial < 300; trial++) {
		const int sa = (int)(rnd() % 6), sb = (int)(rnd() % 6);
		size_t na = gen(sa, va), nb, n2, k;
		sm_t *a, *b;

		switch (trial % 5) {
		case 0: /* identical */
			memcpy(vb, va, na * sizeof(uint64_t));
			nb = na;
			break;
		case 1: /* disjoint: b is a random map with a's members removed */
			nb = gen(sb, vb);
			for (k = n2 = 0; k < nb; k++)
				if (bsearch(&vb[k], va, na, sizeof(uint64_t),
					cmp_u64) == NULL)
					vb[n2++] = vb[k];
			nb = n2;
			break;
		case 2: /* b overlaps a partly */
			for (k = nb = 0; k < na; k++)
				if (rnd() % 2)
					vb[nb++] = va[k];
			nb = uniq(vb, nb + gen(sb, vb + nb));
			break;
		default:
			nb = gen(sb, vb);
			break;
		}
		a = build(va, na);
		b = build(vb, nb);
		if (a == NULL || b == NULL)
			return (1);
		if (check_pair(a, b, va, na, vb, nb, want, got) != 0 ||
		    (trial % 10 == 0 &&
			(check_pair(NULL, b, va, 0, vb, nb, want, got) != 0 ||
			    check_pair(a, NULL, va, na, vb, 0, want, got) != 0))) {
			fprintf(stderr, "  trial %d shapes %d %d sizes %zu %zu\n",
			    trial, sa, sb, na, nb);
			return (1);
		}
		sm_free(a);
		sm_free(b);
	}
	free(va);
	free(vb);
	free(want);
	free(got);
	return (0);
}

static void
count_fn(uint64_t vec[], size_t n, void *aux)
{
	(void)vec;
	*(uint64_t *)aux += n;
}

/* A map holding exactly [0, 2^31 - 1), one RLE chunk. */
static sm_t *
make_big_run(void)
{
	sm_t *m = sm_create(1024);

	if (m != NULL && !sm_add_range(m, 0, UINT64_C(0x7FFFFFFF))) {
		sm_free(m);
		return (NULL);
	}
	return (m);
}

static int
long_runs(void)
{
	const uint64_t big = UINT64_C(0x7FFFFFFF);
	sm_t *run = make_big_run(), *empty = sm_create(64);
	sm_t *sparse = sm_create(1024), *other = make_big_run();
	sm_cursor_t ca = SM_CURSOR_INIT, cb = SM_CURSOR_INIT;
	uint64_t i, n, k;
	double t0, ms;

	if (run == NULL || empty == NULL || sparse == NULL || other == NULL)
		return (1);
	for (k = 0; k < 100; k++)
		sm_add_grow(&sparse, k * (big / 100));
	/* The tail of [0, 2^31 - 1) that is not in [0, 2^31 - 101). */
	sm_remove_range(other, big - 100, big);
	t0 = now_ms();

	/* ANDNOT against an empty map: the first member, and the last. */
	if (sm_next_member_op(run, empty, SM_OP_ANDNOT, SM_IDX_MAX, &ca,
		&cb) != 0 ||
	    sm_next_member_op(run, empty, SM_OP_ANDNOT, big - 2, NULL, NULL) !=
		big - 1)
		return (1);
	/* ANDNOT against a sparse map: 100 holes each skipped in one step. */
	ca = (sm_cursor_t)SM_CURSOR_INIT;
	cb = (sm_cursor_t)SM_CURSOR_INIT;
	i = SM_IDX_MAX;
	for (k = 0; k < 1000; k++)
		i = sm_next_member_op(run, sparse, SM_OP_ANDNOT, i, &ca, &cb);
	if (i != 1000) /* 0 is in sparse, so the 1000th is 1000 */
		return (1);
	/* ANDNOT against itself minus a tail: only the tail is left. */
	if (sm_next_member_op(run, other, SM_OP_ANDNOT, SM_IDX_MAX, NULL,
		NULL) != big - 100)
		return (1);
	/* XOR of the run with itself minus a tail: the 100-bit tail. */
	ca = (sm_cursor_t)SM_CURSOR_INIT;
	cb = (sm_cursor_t)SM_CURSOR_INIT;
	i = SM_IDX_MAX;
	n = 0;
	while ((i = sm_next_member_op(run, other, SM_OP_XOR, i, &ca, &cb)) !=
	    SM_IDX_MAX)
		n++;
	if (n != 100)
		return (1);
	/* XOR of identical runs is empty. */
	if (sm_next_member_op(other, other, SM_OP_XOR, SM_IDX_MAX, NULL,
		NULL) != SM_IDX_MAX)
		return (1);
	/* AND of the run with a sparse map: exactly sparse's members. */
	ca = (sm_cursor_t)SM_CURSOR_INIT;
	cb = (sm_cursor_t)SM_CURSOR_INIT;
	i = SM_IDX_MAX;
	n = 0;
	while ((i = sm_next_member_op(run, sparse, SM_OP_AND, i, &ca, &cb)) !=
	    SM_IDX_MAX)
		n++;
	if (n != 100)
		return (1);
	/* sm_scan_op skipping almost all of the run, then the last 10. */
	n = 0;
	sm_scan_op(run, sparse, SM_OP_ANDNOT, count_fn, (size_t)big - 110,
	    &n);
	if (n != 10)
		return (1);
	n = 0;
	sm_scan_op(run, other, SM_OP_XOR, count_fn, 0, &n);
	if (n != 100)
		return (1);
	ms = now_ms() - t0;
	if (ms >= RUN_CEILING_MS) {
		fprintf(stderr, "FAIL long-run ops took %.3f ms\n", ms);
		return (1);
	}
	printf("long-run ops: %.3f ms\n", ms);
	sm_free(run);
	sm_free(empty);
	sm_free(sparse);
	sm_free(other);
	return (0);
}

/* The last member, 2^64 - 2, in both maps or in one: sm_scan_op and
 * sm_next_member_op report it like any other.  UINT64_MAX is SM_IDX_MAX
 * and cannot be added. */
static int
top_of_universe(void)
{
	sm_t *a = sm_create(256), *b = sm_create(256);
	uint64_t n = 0;

	if (a == NULL || b == NULL)
		return (1);
	errno = 0;
	if (sm_add_grow(&a, UINT64_MAX) != SM_IDX_MAX || errno != EINVAL)
		return (1);
	sm_add_grow(&a, UINT64_MAX - 1);
	sm_add_grow(&a, UINT64_MAX - 2);
	sm_add_grow(&b, UINT64_MAX - 2);
	sm_scan_op(a, b, SM_OP_XOR, count_fn, 0, &n);
	if (n != 1)
		return (1);
	n = 0;
	sm_scan_op(a, b, SM_OP_OR, count_fn, 0, &n);
	if (n != 2)
		return (1);
	if (sm_next_member_op(a, b, SM_OP_AND, SM_IDX_MAX, NULL, NULL) !=
	    UINT64_MAX - 2)
		return (1);
	if (sm_next_member_op(a, b, SM_OP_XOR, UINT64_MAX - 2, NULL, NULL) !=
	    UINT64_MAX - 1)
		return (1);
	errno = 0;
	if (sm_next_member_op(a, b, (sm_op_t)7, SM_IDX_MAX, NULL, NULL) !=
		SM_IDX_MAX ||
	    errno != EINVAL)
		return (1);
	sm_free(a);
	sm_free(b);
	return (0);
}

int
main(void)
{
	if (pairs() != 0) {
		fprintf(stderr, "FAIL pairs\n");
		return (1);
	}
	if (long_runs() != 0) {
		fprintf(stderr, "FAIL long_runs\n");
		return (1);
	}
	if (top_of_universe() != 0) {
		fprintf(stderr, "FAIL top_of_universe\n");
		return (1);
	}
	printf("test_op_iter: ok\n");
	return (0);
}
