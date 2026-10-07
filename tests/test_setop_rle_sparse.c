/* SPDX-License-Identifier: MIT */
/*
 * test_setop_rle_sparse.c - set operations between an RLE chunk and a
 * sparse chunk that share a start, where the run ends inside the sparse
 * chunk.
 *
 * sm_union and sm_difference handle a mixed RLE/sparse overlap by
 * expanding the WHOLE sparse chunk, combining it with the run, and
 * emitting one sparse chunk.  Before 5.8.2 they then advanced the sparse
 * side only to the end of the run, so the sparse chunk's tail
 * [run end, chunk end) was emitted a second time at the same start: two
 * chunks with one start, sm_validate() == false, and sm_cardinality
 * counting the tail twice.  (Reported from pg_tre's w45 differential:
 * sm_union(sm_intersection(a, b), c) with an RLE intersection result
 * starting at 0.)
 *
 * Every operation, in both operand orders, is checked against a naive
 * bit-array model: valid, right cardinality, right members.
 *
 * Before 5.8.2 a run clipped by a set operation was also appended as one
 * RLE chunk at the clip point with capacity == length: an unaligned
 * chunk start (sm_validate false, K1) and an unaligned capacity that a
 * later sm_add could land inside (K2).  known_k1_k2() pins the reported
 * cases and randomized() sweeps set operations over run + sparse
 * operands, checking validity, membership, and sm_add after the op.
 */
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPAN 8192

static int failures;
static int rand_failures; /* randomized() only, reported separately */
static unsigned char run[SPAN], sparse[SPAN], want[SPAN];

static void
check(const char *op, uint64_t base, uint64_t len, sm_t *r)
{
	size_t n = 0, wrong = 0;
	uint64_t i;
	for (i = 0; i < SPAN; i++) {
		const bool has = r != NULL && sm_contains(r, base + i, NULL);
		n += want[i];
		wrong += has != (want[i] != 0);
	}
	if (r == NULL ? n != 0 :
	        !sm_validate(r) || sm_cardinality(r) != n || wrong != 0) {
		fprintf(stderr,
		    "FAIL %s base=%llu len=%llu: valid=%d card=%zu want=%zu "
		    "wrong=%zu\n", op, (unsigned long long)base,
		    (unsigned long long)len, r == NULL || sm_validate(r),
		    r == NULL ? 0 : sm_cardinality(r), n, wrong);
		failures++;
	}
	sm_free(r);
}

static void
one(uint64_t base, uint64_t len)
{
	static const uint64_t ids[] = { 4, 63, 64, 200, 1000, 1999, 2047,
		2048, 2100, 5000 };
	sm_t *x = sm_create(64), *y = sm_create(64), *s = sm_create(64);
	sm_t *r; /* the run [base, base + len) as one RLE chunk */
	size_t i;
	int k;

	memset(run, 0, sizeof(run));
	memset(sparse, 0, sizeof(sparse));
	/* An intersection of two runs is an RLE chunk with capacity == length,
	 * the shape that ends inside a neighbouring sparse chunk. */
	sm_add_range(x, base, base + 3000);
	sm_add_range(y, base, base + len);
	r = sm_intersection(x, y);
	for (i = 0; i < len; i++)
		run[i] = 1;
	for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		sm_add_grow(&s, base + ids[i]);
		sparse[ids[i]] = 1;
	}
	if (r == NULL || !sm_validate(r) || sm_cardinality(r) != len) {
		fprintf(stderr, "FAIL setup base=%llu len=%llu\n",
		    (unsigned long long)base, (unsigned long long)len);
		failures++;
	}

	for (k = 0; k < SPAN; k++)
		want[k] = run[k] | sparse[k];
	check("run|sparse", base, len, sm_union(r, s));
	check("sparse|run", base, len, sm_union(s, r));
	for (k = 0; k < SPAN; k++)
		want[k] = run[k] & sparse[k];
	check("run&sparse", base, len, sm_intersection(r, s));
	check("sparse&run", base, len, sm_intersection(s, r));
	for (k = 0; k < SPAN; k++)
		want[k] = run[k] & !sparse[k];
	check("run-sparse", base, len, sm_difference(r, s));
	for (k = 0; k < SPAN; k++)
		want[k] = sparse[k] & !run[k];
	check("sparse-run", base, len, sm_difference(s, r));
	for (k = 0; k < SPAN; k++)
		want[k] = run[k] ^ sparse[k];
	check("run^sparse", base, len, sm_xor(r, s));
	check("sparse^run", base, len, sm_xor(s, r));

	sm_free(r);
	sm_free(s);
	sm_free(y);
	sm_free(x);
}

/* ---- K1 / K2: clipped runs must stay chunk-aligned ---- */

static sm_t *
range_map(uint64_t lo, uint64_t hi)
{
	sm_t *m = sm_create(1024);
	sm_add_range(m, lo, hi);
	return (m);
}

/* r must hold exactly [lo, hi) minus [xlo, xhi), plus the bits in add[]. */
static void
check_diff(const char *what, sm_t *r, uint64_t lo, uint64_t hi, uint64_t xlo,
    uint64_t xhi, const uint64_t *add, size_t nadd)
{
	const uint64_t top = hi + 4096;
	size_t n = 0, wrong = 0, k;
	uint64_t i;
	for (i = 0; i < top; i++) {
		bool want_i = i >= lo && i < hi && !(i >= xlo && i < xhi);
		for (k = 0; k < nadd; k++)
			want_i = want_i || add[k] == i;
		n += want_i;
		wrong += (r != NULL && sm_contains(r, i, NULL)) != want_i;
	}
	if (r == NULL || !sm_validate(r) || sm_cardinality(r) != n ||
	    wrong != 0) {
		fprintf(stderr,
		    "FAIL %s [%llu,%llu)-[%llu,%llu) nadd=%zu: valid=%d "
		    "card=%zu want=%zu wrong=%zu\n", what,
		    (unsigned long long)lo, (unsigned long long)hi,
		    (unsigned long long)xlo, (unsigned long long)xhi, nadd,
		    r == NULL ? -1 : (int)sm_validate(r),
		    r == NULL ? 0 : sm_cardinality(r), n, wrong);
		failures++;
	}
}

static void
known_k1_k2(void)
{
	/* {a_lo, a_hi, b_lo, b_hi}; the first two are the K1 reports. */
	static const uint64_t cases[][4] = { { 10000, 20000, 10000, 13794 },
		{ 0, 5000, 0, 100 }, { 0, 10000, 4096, 8192 },
		{ 0, 2249, 0, 100 }, { 0, 16384, 0, 16357 },
		{ 2048, 3000, 2048, 2100 }, { 0, 10000, 3000, 3001 },
		{ 0, 10000, 2000, 8000 } };
	size_t c;
	for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		const uint64_t *k = cases[c];
		sm_t *a = range_map(k[0], k[1]), *b = range_map(k[2], k[3]);
		sm_t *d = sm_difference(a, b);
		uint64_t x;
		check_diff("K1 difference", d, k[0], k[1], k[2], k[3], NULL, 0);
		/* K2: an add just past the run, up to the next chunk boundary
		 * and beyond, must not land inside the run's span. */
		for (x = k[1]; d != NULL && x < k[1] + 2 * 2048; x += 97) {
			sm_t *e = sm_copy(d);
			sm_add_grow(&e, x);
			check_diff("K2 add after difference", e, k[0], k[1], k[2],
			    k[3], &x, 1);
			sm_free(e);
		}
		sm_free(d);
		sm_free(a);
		sm_free(b);
	}
}

/* ---- randomized set operations over run + sparse operands ---- */

#define RU 16384 /* universe: eight 2048-bit chunks */

static uint64_t rs;

static uint64_t
rnd(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return (rs);
}

/* Random runs (some >= 2048 bits and crossing chunk boundaries, some at
 * 0 or a chunk start) plus random sparse bits; m is the model. */
static sm_t *
gen(unsigned char *m)
{
	sm_t *s = sm_create(256);
	int k = (int)(rnd() % 4), j;
	memset(m, 0, RU);
	while (k-- > 0) {
		const unsigned t = (unsigned)(rnd() % 4);
		uint64_t lo = t == 0 ? 0 :
		    t == 1          ? (rnd() % 8) * 2048 :
		                      rnd() % RU;
		uint64_t len = rnd() % 3 ? 2048 + rnd() % 6000 : 1 + rnd() % 300;
		uint64_t hi = lo + len > RU ? RU : lo + len, i;
		while (!sm_add_range(s, lo, hi)) { /* all-or-nothing: grow, retry */
			sm_t *g = sm_set_data_size(s, NULL,
			    sm_get_capacity(s) * 2 + 4096);
			if (g == NULL)
				abort();
			s = g;
		}
		for (i = lo; i < hi; i++)
			m[i] = 1;
	}
	for (j = (int)(rnd() % 40); j > 0; j--) {
		const uint64_t x = rnd() % RU;
		sm_add_grow(&s, x);
		m[x] = 1;
	}
	return (s);
}

static void
check_rand(const char *what, long it, sm_t *r, const unsigned char *w)
{
	static uint64_t got[RU], ref[RU];
	size_t n = 0, gn = RU, wrong = 0, i;
	for (i = 0; i < RU; i++)
		if (w[i])
			ref[n++] = i;
	if (r == NULL) {
		if (n != 0) {
			if (rand_failures++ < 20)
				fprintf(stderr,
				    "FAIL rand it=%ld %s: NULL, want %zu\n", it,
				    what, n);
			failures++;
		}
		return;
	}
	sm_to_array(r, got, &gn); /* exact membership vs the sorted model */
	for (i = 0; i < 256; i++) {
		const uint64_t x = rnd() % (RU + 2048);
		wrong += sm_contains(r, x, NULL) != (x < RU && w[x]);
	}
	if (!sm_validate(r) || sm_cardinality(r) != n || gn != n ||
	    memcmp(got, ref, n * sizeof(uint64_t)) != 0 || wrong != 0) {
		if (rand_failures++ < 20)
			fprintf(stderr,
			    "FAIL rand it=%ld %s: valid=%d card=%zu want=%zu "
			    "wrong=%zu\n", it, what, (int)sm_validate(r),
			    sm_cardinality(r), n, wrong);
		failures++;
	}
}

static sm_t *
op(int o, const sm_t *x, const sm_t *y)
{
	return (o == 0 ? sm_union(x, y) :
	        o == 1 ? sm_intersection(x, y) :
	        o == 2 ? sm_difference(x, y) :
	                 sm_xor(x, y));
}

static void
model(int o, const unsigned char *x, const unsigned char *y, unsigned char *w)
{
	size_t i;
	for (i = 0; i < RU; i++)
		w[i] = o == 0 ? (x[i] | y[i]) :
		    o == 1    ? (x[i] & y[i]) :
		    o == 2    ? (x[i] & !y[i]) :
		                (x[i] ^ y[i]);
}

static void
randomized(uint64_t seed, long iters)
{
	static const char *const nm[4] = { "union", "intersection",
		"difference", "xor" };
	static unsigned char A[RU], B[RU], C[RU], W[RU], W2[RU];
	long it;
	rs = seed | 1;
	for (it = 0; it < iters; it++) {
		sm_t *a = gen(A), *b = gen(B), *c = gen(C);
		int o, sw;
		for (o = 0; o < 4; o++) {
			for (sw = 0; sw < 2; sw++) {
				sm_t *r = sw ? op(o, b, a) : op(o, a, b), *r2;
				const int o2 = (int)(rnd() % 4);
				int k;
				char what[64];
				model(o, sw ? B : A, sw ? A : B, W);
				snprintf(what, sizeof(what), "%s%s", nm[o],
				    sw ? " (b,a)" : "");
				check_rand(what, it, r, W);
				/* the result as an operand */
				r2 = op(o2, r, c);
				model(o2, W, C, W2);
				snprintf(what, sizeof(what), "%s then %s", nm[o],
				    nm[o2]);
				check_rand(what, it, r2, W2);
				sm_free(r2);
				/* sm_add after the op */
				if (r == NULL)
					r = sm_create(64);
				for (k = 0; k < 3; k++) {
					const uint64_t x = rnd() % RU;
					sm_add_grow(&r, x);
					W[x] = 1;
				}
				snprintf(what, sizeof(what), "%s + add", nm[o]);
				check_rand(what, it, r, W);
				sm_free(r);
			}
		}
		sm_free(a);
		sm_free(b);
		sm_free(c);
	}
}

int
main(void)
{
	static const uint64_t seeds[] = { 1, 0x5eed5eedULL, 0xdeadbeef12345ULL };
	static const uint64_t bases[] = { 0, 2048, 65536 };
	static const uint64_t lens[] = { 1, 63, 64, 65, 164, 700, 1500, 2047,
		2048, 2100 };
	size_t b, l;
	for (b = 0; b < sizeof(bases) / sizeof(bases[0]); b++)
		for (l = 0; l < sizeof(lens) / sizeof(lens[0]); l++)
			one(bases[b], lens[l]);
	known_k1_k2();
	for (b = 0; b < sizeof(seeds) / sizeof(seeds[0]); b++)
		randomized(seeds[b], 4000);
	if (rand_failures != 0)
		fprintf(stderr, "test_setop_rle_sparse: randomized: %d failing "
		    "checks\n", rand_failures);
	if (failures != 0) {
		fprintf(stderr, "test_setop_rle_sparse: %d failures\n",
		    failures);
		return (1);
	}
	printf("test_setop_rle_sparse: OK\n");
	return (0);
}
