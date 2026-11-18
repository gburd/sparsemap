/* SPDX-License-Identifier: MIT */
/*
 * test_frozen.c - the read-only Elias-Fano copy (sm_freeze, 5.9.0).
 *
 * For maps of every shape (empty, NULL, one member, small set, sparse with
 * gaps, clustered on chunk boundaries, RLE runs of up to a few million
 * bits, dense, the top of the 64-bit universe up to 2^64 - 2), every
 * query on the frozen copy equals the sm_t answer and a sorted-array
 * oracle: cardinality, contains, rank, select, next and prev at members,
 * neighbours, chunk boundaries, out-of-range indices and SM_IDX_MAX.  The
 * thawed map is sm_equals to the source.  Serialize then deserialize gives
 * the same answers; truncated, padded and header-flipped streams and
 * random bytes are rejected without crashing (run under ASan).  Finally it
 * prints bytes per member for sm_t and the frozen copy at densities 1e-4,
 * 1e-3 and 1e-2 of a 1e8 universe.
 */
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                              \
	do {                                                                  \
		if (!(c)) {                                                   \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
			    __LINE__, #c);                                   \
			return (1);                                          \
		}                                                            \
	} while (0)

#define MAXN 3000000

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

static size_t
gen(int shape, uint64_t *v)
{
	size_t n = 0, k;
	uint64_t b;

	switch (shape) {
	case 0: /* empty */
		break;
	case 1: /* one member */
		v[n++] = rnd() % 5000000;
		break;
	case 2: /* small set */
		for (k = 0; k < 1 + rnd() % 200; k++)
			v[n++] = rnd() % 1024;
		break;
	case 3: /* sparse with gaps */
		for (k = 0; k < 1 + rnd() % 3000; k++)
			v[n++] = rnd() % 100000000;
		break;
	case 4: /* clustered on chunk boundaries */
		for (k = 0; k < 1 + rnd() % 300; k++) {
			b = (rnd() % 5000) * 2048;
			v[n++] = b;
			v[n++] = b + 2047;
			v[n++] = b + 2048;
		}
		break;
	case 5: /* RLE runs, up to a few million bits */
		for (b = 4096; b < 4096 + 1000000 + rnd() % 1000000; b++)
			v[n++] = b;
		for (k = 0; k < 100; k++)
			v[n++] = 5000000 + rnd() % 1000000;
		break;
	case 6: /* dense */
		for (k = 0; k < 20000; k++)
			v[n++] = rnd() % 30000;
		break;
	case 7: /* top of the universe, up to the last member 2^64 - 2 */
		v[n++] = 0;
		for (k = 0; k < 1 + rnd() % 50; k++)
			v[n++] = UINT64_MAX - 1 - rnd() % 5000;
		v[n++] = UINT64_MAX - 1;
		break;
	default: /* the last member alone: u = 2^64 - 1, L = 63 */
		v[n++] = UINT64_MAX - 1;
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

/* Oracle: index of the first v[] >= x. */
static size_t
lower_bound(const uint64_t *v, size_t n, uint64_t x)
{
	size_t lo = 0, hi = n;

	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (v[mid] < x)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (lo);
}

/* Every query at x against the oracle and, where meaningful, the map. */
static int
query(const sm_frozen_t *f, sm_t *m, const uint64_t *v, size_t n, uint64_t x)
{
	const size_t lb = lower_bound(v, n, x);
	const int in = lb < n && v[lb] == x;
	size_t up;
	uint64_t want;

	CHECK(sm_frozen_contains(f, x) == in);
	CHECK(sm_contains(m, x, NULL) == in);
	/* next: the first member > x, with SM_IDX_MAX meaning "from start". */
	if (x == SM_IDX_MAX)
		want = n > 0 ? v[0] : SM_IDX_MAX;
	else {
		up = lb + (size_t)in;
		want = up < n ? v[up] : SM_IDX_MAX;
	}
	CHECK(sm_frozen_next_member(f, x) == want);
	if (want != UINT64_MAX)
		CHECK(sm_next_member(m, x, NULL) == want);
	/* prev: the last member < x; SM_IDX_MAX is the bound UINT64_MAX. */
	want = lb > 0 ? v[lb - 1] : SM_IDX_MAX;
	CHECK(sm_frozen_prev_member(f, x) == want);
	CHECK(sm_prev_member(m, x, NULL) == want);
	/* rank over [0, x] and [x, top]. */
	up = lb + (size_t)in;
	CHECK(sm_frozen_rank(f, 0, x) == up);
	CHECK(sm_frozen_rank(f, x, UINT64_MAX) == n - lb);
	CHECK(sm_rank(m, 0, x, true) == up);
	return (0);
}

static int
check(const sm_frozen_t *f, sm_t *m, const uint64_t *v, size_t n)
{
	size_t k;
	sm_t *t;

	CHECK(sm_frozen_cardinality(f) == n);
	CHECK(sm_frozen_select(f, n) == SM_IDX_MAX);
	CHECK(sm_frozen_select(f, SM_IDX_MAX) == SM_IDX_MAX);
	CHECK(sm_frozen_rank(f, 5, 4) == 0);
	CHECK(sm_frozen_rank(f, 0, UINT64_MAX) == n);
	for (k = 0; k < n; k += 1 + (n > 20000 ? rnd() % 500 : 0)) {
		CHECK(sm_frozen_select(f, k) == v[k]);
		CHECK(sm_select(m, k, true) == v[k]);
		if (query(f, m, v, n, v[k]) != 0 ||
		    (v[k] > 0 && query(f, m, v, n, v[k] - 1) != 0) ||
		    query(f, m, v, n, v[k] + 1) != 0) {
			fprintf(stderr, "  at member %zu = %llu\n", k,
			    (unsigned long long)v[k]);
			return (1);
		}
		CHECK(sm_frozen_rank(f, v[k], v[k]) == 1);
	}
	for (k = 0; k < 2000; k++) {
		const uint64_t x = (k % 4 == 0) ? (rnd() % 60000) * 2048 +
		    (rnd() % 2 ? 0 : 2047) : (k % 4 == 1) ? rnd()
		    : rnd() % 120000000;
		if (query(f, m, v, n, x) != 0) {
			fprintf(stderr, "  at %llu\n", (unsigned long long)x);
			return (1);
		}
	}
	CHECK(query(f, m, v, n, 0) == 0);
	CHECK(query(f, m, v, n, SM_IDX_MAX) == 0);
	t = sm_thaw(f);
	CHECK(t != NULL && sm_equals(t, m));
	sm_free(t);
	return (0);
}

static int
serial(const sm_frozen_t *f, sm_t *m, const uint64_t *v, size_t n)
{
	const size_t sz = sm_frozen_serialized_size(f);
	uint8_t *buf = malloc(sz + 16);
	sm_frozen_t *g;
	size_t i;

	CHECK(buf != NULL && sz >= 32);
	CHECK(sm_frozen_serialize(f, buf, sz - 1) == 0);
	CHECK(sm_frozen_serialize(f, buf, sz) == sz);
	g = sm_frozen_deserialize(buf, sz);
	CHECK(g != NULL);
	CHECK(check(g, m, v, n) == 0);
	sm_frozen_free(g);

	/* Truncated, padded, and every header field flipped. */
	CHECK(sm_frozen_deserialize(buf, sz - 8) == NULL);
	CHECK(sm_frozen_deserialize(buf, sz - 1) == NULL);
	memset(buf + sz, 0, 16);
	CHECK(sm_frozen_deserialize(buf, sz + 8) == NULL);
	CHECK(sm_frozen_deserialize(NULL, sz) == NULL);
	for (i = 0; i < 32; i++) {
		const uint8_t save = buf[i];
		buf[i] ^= (uint8_t)(1u << (i % 8));
		g = sm_frozen_deserialize(buf, sz);
		/* A flipped count or maximum that still matches the body would
		 * have to re-encode the same members; it cannot. */
		CHECK(g == NULL);
		buf[i] = save;
	}
	/* Body bit flips: rejected, or a valid structure that answers
	 * consistently (a flipped low bit of the last bucket moves a member). */
	for (i = 32; i < sz && i < 32 + 512; i++) {
		const uint8_t save = buf[i];
		buf[i] ^= (uint8_t)(1u << (rnd() % 8));
		g = sm_frozen_deserialize(buf, sz);
		if (g != NULL) {
			const size_t c = sm_frozen_cardinality(g);
			CHECK(c == n);
			CHECK(c == 0 ||
			    sm_frozen_contains(g, sm_frozen_select(g, c - 1)));
			sm_frozen_free(g);
		}
		buf[i] = save;
	}
	free(buf);
	return (0);
}

static int
shapes(void)
{
	uint64_t *v = malloc(MAXN * sizeof(uint64_t));
	int shape, rep;

	CHECK(v != NULL);
	for (rep = 0; rep < 4; rep++) {
		for (shape = 0; shape <= 8; shape++) {
			const size_t n = gen(shape, v);
			sm_t *m = build(v, n);
			sm_frozen_t *f = sm_freeze(m);

			CHECK(m != NULL && f != NULL);
			if (check(f, m, v, n) != 0 || serial(f, m, v, n) != 0) {
				fprintf(stderr, "  shape %d rep %d n %zu\n", shape,
				    rep, n);
				return (1);
			}
			sm_frozen_free(f);
			sm_free(m);
		}
	}
	/* NULL map and NULL frozen map are the empty set. */
	{
		sm_frozen_t *f = sm_freeze(NULL);
		sm_t *t;

		CHECK(f != NULL && sm_frozen_cardinality(f) == 0);
		CHECK(sm_frozen_cardinality(NULL) == 0);
		CHECK(!sm_frozen_contains(NULL, 3));
		CHECK(sm_frozen_next_member(NULL, SM_IDX_MAX) == SM_IDX_MAX);
		CHECK(sm_frozen_prev_member(NULL, SM_IDX_MAX) == SM_IDX_MAX);
		CHECK(sm_frozen_select(NULL, 0) == SM_IDX_MAX);
		CHECK(sm_frozen_rank(NULL, 0, 9) == 0);
		CHECK(sm_frozen_serialized_size(NULL) == 0);
		t = sm_thaw(NULL);
		CHECK(t != NULL && sm_is_empty(t));
		sm_free(t);
		sm_frozen_free(f);
		sm_frozen_free(NULL);
	}
	free(v);
	return (0);
}

static int
fuzz(void)
{
	uint8_t buf[512];
	int k;
	size_t i, n;
	sm_frozen_t *f;

	for (k = 0; k < 20000; k++) {
		n = 8 * (rnd() % 64);
		for (i = 0; i < n; i++)
			buf[i] = (uint8_t)rnd();
		if (n >= 32 && k % 2 == 0) {
			/* A plausible header so the body checks run. */
			const uint64_t magic = UINT64_C(0x3130667a6f72666d);
			uint64_t cnt = rnd() % 40, max = rnd() % 4000, ver;
			uint32_t l = 0;
			if (cnt > 0) {
				uint64_t q = (max + 1) / cnt;
				while (q > 1) {
					q >>= 1;
					l++;
				}
			} else
				max = 0;
			ver = 1 | ((uint64_t)l << 32);
			for (i = 0; i < 8; i++) {
				buf[i] = (uint8_t)(magic >> (8 * i));
				buf[8 + i] = (uint8_t)(ver >> (8 * i));
				buf[16 + i] = (uint8_t)(cnt >> (8 * i));
				buf[24 + i] = (uint8_t)(max >> (8 * i));
			}
		}
		f = sm_frozen_deserialize(buf, n);
		if (f != NULL) {
			const size_t c = sm_frozen_cardinality(f);
			uint64_t x = SM_IDX_MAX, seen = 0;
			while ((x = sm_frozen_next_member(f, x)) != SM_IDX_MAX)
				seen++;
			CHECK(seen == c);
			CHECK(c == 0 || sm_frozen_contains(f, sm_frozen_select(f, 0)));
			sm_frozen_free(f);
		}
	}
	return (0);
}

/* Bytes per member, sm_t versus frozen, on a 1e8 universe. */
static int
report(void)
{
	static const double dens[] = { 1e-4, 1e-3, 1e-2 };
	uint64_t *v = malloc(1000000 * sizeof(uint64_t));
	size_t d, k, n;

	CHECK(v != NULL);
	for (d = 0; d < 3; d++) {
		sm_t *m;
		sm_frozen_t *f;

		n = (size_t)(1e8 * dens[d]);
		for (k = 0; k < n; k++)
			v[k] = rnd() % 100000000;
		n = uniq(v, n);
		m = build(v, n);
		f = sm_freeze(m);
		CHECK(m != NULL && f != NULL);
		printf("density %g: %zu members, sm_t %.2f B/member, frozen "
		    "%.2f B/member (serialized %.2f)\n", dens[d], n,
		    (double)sm_get_size(m) / (double)n,
		    (double)sm_frozen_size(f) / (double)n,
		    (double)sm_frozen_serialized_size(f) / (double)n);
		CHECK(sm_frozen_size(f) < sm_get_size(m));
		sm_frozen_free(f);
		sm_free(m);
	}
	free(v);
	return (0);
}

int
main(void)
{
	if (shapes() != 0 || fuzz() != 0 || report() != 0)
		return (1);
	printf("test_frozen: ok\n");
	return (0);
}
