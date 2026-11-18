/* SPDX-License-Identifier: MIT */
/*
 * test_range_ops.c - run-based range and relation ops (R5).
 *
 * Through 5.8.5 sm_remove_range and sm_flip_range touched every bit of
 * [lo, hi) (one sm_remove or sm_assign each), and sm_overlap and
 * sm_is_subset walked the maps member by member, so a 2^31-bit run cost
 * 2^31 steps (sm_overlap of a 2^28-bit run and a disjoint singleton
 * took 1.7 s).  sm_nonempty_difference used the run iterator but counted
 * both maps in full instead of stopping at the first witness.  They now
 * sweep runs, so cost tracks the encoded size.
 *
 * Checks:
 *   1. random maps built from short runs, single members and RLE runs
 *      (gaps, chunk boundaries, small-set mode), each op against an
 *      interval oracle: sm_remove_range and sm_flip_range over ranges
 *      that start and end inside, at the edge of, and outside runs and
 *      chunks, then sm_validate, cardinality and every bit of a window
 *      around the range; sm_overlap, sm_is_subset, sm_is_superset and
 *      sm_nonempty_difference on random pairs and on derived pairs (a
 *      map and a range cut out of it), against the oracle;
 *   2. edges: empty and NULL maps, lo >= hi, hi near 2^64, a flip of an
 *      empty map, a remove that empties the map;
 *   3. a time ceiling: each op on a 2^31-bit run (remove a middle span,
 *      flip a span that straddles its end, overlap and subset against a
 *      disjoint or nested small map) finishes in well under the
 *      ceiling, and gives the right answer.
 */
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* Sanitizer and coverage builds distort timing (see test_amplify.c). */
#if defined(__SANITIZE_ADDRESS__) || defined(SM_COVERAGE_BUILD)
#define CEILING_MS 2000.0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) ||                                       \
    __has_feature(undefined_behavior_sanitizer)
#define CEILING_MS 2000.0
#else
#define CEILING_MS 100.0
#endif
#else
#define CEILING_MS 100.0
#endif

#define W UINT64_C(2048)
#define P31 (UINT64_C(1) << 31)
#define MAXB 512

static uint64_t rs = UINT64_C(0x9E3779B97F4A7C15);

static uint64_t
rnd(void)
{
	uint64_t z = (rs += UINT64_C(0x9E3779B97F4A7C15));
	z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
	return (z ^ (z >> 31));
}

/*
 * The oracle: disjoint half-open intervals as sorted boundaries.  x is
 * a member when an odd number of boundaries are <= x.
 */
struct oracle {
	uint64_t b[MAXB];
	size_t n;
};

static int
o_member(const struct oracle *o, uint64_t x)
{
	size_t lo = 0, hi = o->n;

	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (o->b[mid] <= x)
			lo = mid + 1;
		else
			hi = mid;
	}
	return ((int)(lo & 1));
}

/*
 * Set [lo, hi) to v (v = 2 complements it).  The boundaries inside
 * (lo, hi) stay under v = 2 and go under v = 0 or 1; lo and hi become
 * boundaries wherever membership now changes there.
 */
static void
o_apply(struct oracle *o, uint64_t lo, uint64_t hi, int v)
{
	uint64_t nb[MAXB];
	size_t i = 0, n = 0;
	const int before = lo > 0 ? o_member(o, lo - 1) : 0;
	const int old_lo = o_member(o, lo);
	const int old_last = o_member(o, hi - 1);
	const int after = o_member(o, hi);
	const int new_lo = v == 2 ? !old_lo : v;
	const int new_last = v == 2 ? !old_last : v;

	while (i < o->n && o->b[i] < lo)
		nb[n++] = o->b[i++];
	if (before != new_lo)
		nb[n++] = lo;
	while (i < o->n && o->b[i] <= lo)
		i++;
	for (; i < o->n && o->b[i] < hi; i++) {
		if (v == 2)
			nb[n++] = o->b[i];
	}
	if (new_last != after)
		nb[n++] = hi;
	while (i < o->n && o->b[i] <= hi)
		i++;
	while (i < o->n)
		nb[n++] = o->b[i++];
	if (n > MAXB)
		abort();
	memcpy(o->b, nb, n * sizeof(nb[0]));
	o->n = n;
}

static uint64_t
o_card(const struct oracle *o)
{
	uint64_t c = 0;
	size_t i;

	for (i = 0; i + 1 < o->n; i += 2)
		c += o->b[i + 1] - o->b[i];
	return (c);
}

/* a and b share a member. */
static int
o_overlap(const struct oracle *a, const struct oracle *b)
{
	size_t i, j = 0;

	for (i = 0; i + 1 < a->n; i += 2) {
		while (j + 1 < b->n && b->b[j + 1] <= a->b[i])
			j += 2;
		if (j + 1 < b->n && b->b[j] < a->b[i + 1])
			return (1);
	}
	return (0);
}

/* Every member of a is in b. */
static int
o_subset(const struct oracle *a, const struct oracle *b)
{
	size_t i, j = 0;

	for (i = 0; i + 1 < a->n; i += 2) {
		while (j + 1 < b->n && b->b[j + 1] <= a->b[i])
			j += 2;
		if (j + 1 >= b->n || b->b[j] > a->b[i] ||
		    b->b[j + 1] < a->b[i + 1])
			return (0);
	}
	return (1);
}

/* A random map over [0, 24 W): runs, members and an RLE run or two. */
static sm_t *
random_map(struct oracle *o, int small)
{
	sm_t *m = sm_create(1 << 16);
	const uint64_t span = small ? 1000 : 24 * W;
	int k, n = (int)(rnd() % 12);

	o->n = 0;
	for (k = 0; k < n; k++) {
		const uint64_t lo = rnd() % span;
		uint64_t len;
		switch (rnd() % 4) {
		case 0:
			len = 1;
			break;
		case 1:
			len = 1 + rnd() % 70;
			break;
		case 2:
			len = small ? 1 + rnd() % 300 : W + rnd() % (4 * W);
			break;
		default:
			len = small ? 64 : W * (1 + rnd() % 3) -
			        (lo % W);
			break;
		}
		if (lo + len > span)
			len = span - lo;
		CHECK(sm_add_range(m, lo, lo + len), "build add_range");
		o_apply(o, lo, lo + len, 1);
	}
	return (m);
}

/* m agrees with o: validate, cardinality, and every bit of [lo, hi). */
static int
agrees(sm_t *m, const struct oracle *o, uint64_t lo, uint64_t hi,
    const char *what)
{
	uint64_t x;

	if (!sm_validate(m)) {
		CHECK(0, "%s: sm_validate", what);
		return (0);
	}
	if ((uint64_t)sm_cardinality(m) != o_card(o)) {
		CHECK(0, "%s: cardinality %zu want %llu", what,
		    sm_cardinality(m), (unsigned long long)o_card(o));
		return (0);
	}
	for (x = lo; x < hi; x++) {
		if ((int)sm_contains(m, x, NULL) != o_member(o, x)) {
			CHECK(0, "%s: bit %llu want %d", what,
			    (unsigned long long)x, o_member(o, x));
			return (0);
		}
	}
	return (1);
}

/* A range endpoint near an existing boundary, a chunk edge, or random. */
static uint64_t
pick(const struct oracle *o, int small)
{
	const uint64_t span = small ? 1100 : 26 * W;

	switch (rnd() % 4) {
	case 0:
		if (o->n > 0) {
			const uint64_t b = o->b[rnd() % o->n];
			const uint64_t d = rnd() % 3;
			return (rnd() % 2 ? b + d : (b >= d ? b - d : 0));
		}
		return (rnd() % span);
	case 1:
		return ((rnd() % (span / W + 1)) * W);
	default:
		return (rnd() % span);
	}
}

static void
random_cases(void)
{
	int trial;
	char what[96];

	for (trial = 0; trial < 3000; trial++) {
		const int small = trial % 5 == 0;
		struct oracle o, p;
		sm_t *m = random_map(&o, small);
		sm_t *q = random_map(&p, small);
		uint64_t lo = pick(&o, small), hi = pick(&o, small);
		const int flip = (int)(rnd() % 2);

		/* Relations of two random maps. */
		snprintf(what, sizeof(what), "trial %d relations", trial);
		CHECK((int)sm_overlap(m, q) == o_overlap(&o, &p), "%s overlap",
		    what);
		CHECK((int)sm_is_subset(m, q) == o_subset(&o, &p),
		    "%s subset", what);
		CHECK((int)sm_is_superset(m, q) == o_subset(&p, &o),
		    "%s superset", what);
		CHECK((int)sm_nonempty_difference(m, q) == !o_subset(&o, &p),
		    "%s nonempty_difference", what);

		if (lo > hi) {
			const uint64_t t = lo;
			lo = hi;
			hi = t;
		}
		if (lo == hi)
			hi++;
		snprintf(what, sizeof(what), "trial %d %s [%llu, %llu)", trial,
		    flip ? "flip" : "remove", (unsigned long long)lo,
		    (unsigned long long)hi);
		if (flip)
			CHECK(sm_flip_range(m, lo, hi), "%s", what);
		else
			CHECK(sm_remove_range(m, lo, hi), "%s", what);
		o_apply(&o, lo, hi, flip ? 2 : 0);
		agrees(m, &o, lo >= 2 * W ? lo - 2 * W : 0, hi + 2 * W, what);

		/* Relations of the result with a copy that had the range cut
		 * out or flipped back: subset both ways, overlap. */
		{
			struct oracle r = o;
			sm_t *c = sm_copy(m);
			sm_remove_range(c, lo, hi);
			o_apply(&r, lo, hi, 0);
			CHECK((int)sm_is_subset(c, m) == o_subset(&r, &o),
			    "%s cut subset", what);
			CHECK((int)sm_is_subset(m, c) == o_subset(&o, &r),
			    "%s cut superset", what);
			CHECK((int)sm_overlap(c, m) == o_overlap(&r, &o),
			    "%s cut overlap", what);
			CHECK((int)sm_nonempty_difference(m, c) ==
			        !o_subset(&o, &r),
			    "%s cut nonempty_difference", what);
			sm_free(c);
		}
		sm_free(m);
		sm_free(q);
	}
}

static void
edges(void)
{
	sm_t *e = sm_create(1024), *m = sm_create(4096);
	struct oracle o;

	CHECK(sm_remove_range(NULL, 1, 5) == false, "remove NULL");
	CHECK(sm_flip_range(NULL, 1, 5) == false, "flip NULL");
	CHECK(sm_remove_range(e, 5, 5), "remove empty range");
	CHECK(sm_flip_range(e, 7, 3), "flip inverted range");
	CHECK(!sm_overlap(NULL, e) && !sm_overlap(e, e), "overlap empty");
	CHECK(sm_is_subset(NULL, e) && sm_is_subset(e, NULL),
	    "subset empty");
	CHECK(!sm_nonempty_difference(e, NULL), "nonempty_difference empty");

	/* A flip of an empty map sets the range; a remove empties it. */
	o.n = 0;
	CHECK(sm_flip_range(e, 100, 5000), "flip empty map");
	o_apply(&o, 100, 5000, 2);
	agrees(e, &o, 0, 6000, "flip empty map");
	CHECK(sm_is_subset(e, e) && sm_overlap(e, e), "self relations");
	CHECK(sm_remove_range(e, 0, 6000), "remove all");
	o_apply(&o, 0, 6000, 0);
	agrees(e, &o, 0, 6000, "remove all");
	CHECK(sm_is_empty(e), "remove all leaves it empty");

	/* hi near the top of the universe. */
	o.n = 0;
	sm_add(m, 10);
	o_apply(&o, 10, 11, 1);
	CHECK(sm_flip_range(m, UINT64_MAX - 5000, UINT64_MAX), "flip top");
	o_apply(&o, UINT64_MAX - 5000, UINT64_MAX, 2);
	agrees(m, &o, UINT64_MAX - 6000, UINT64_MAX - 1, "flip top");
	CHECK(!sm_contains(m, UINT64_MAX, NULL), "flip top: SM_IDX_MAX");
	CHECK(sm_remove_range(m, UINT64_MAX - 4000, UINT64_MAX), "remove top");
	o_apply(&o, UINT64_MAX - 4000, UINT64_MAX, 0);
	agrees(m, &o, UINT64_MAX - 6000, UINT64_MAX - 1, "remove top");
	sm_free(e);
	sm_free(m);
}

static double
ms_since(struct timespec t0)
{
	struct timespec t1;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	return ((double)(t1.tv_sec - t0.tv_sec) * 1e3 +
	    (double)(t1.tv_nsec - t0.tv_nsec) / 1e6);
}

#define TIMED(label, stmt)                                                    \
	do {                                                                  \
		struct timespec t0;                                           \
		double ms;                                                    \
		clock_gettime(CLOCK_MONOTONIC, &t0);                          \
		stmt;                                                         \
		ms = ms_since(t0);                                            \
		CHECK(ms < CEILING_MS, "%s took %.3f ms", label, ms);        \
	} while (0)

/* Each op on a 2^31-bit run at 4096. */
static void
long_runs(void)
{
	const uint64_t lo = 4096, hi = 4096 + P31;
	sm_t *m = sm_create(4096), *one = sm_create(1024), *in = sm_create(4096);
	sm_t *c;
	bool r = false;

	CHECK(sm_add_range(m, lo, hi), "build run");
	sm_add(one, 3);
	sm_add(in, lo + 77);
	sm_add(in, hi - 1);

	TIMED("overlap disjoint", r = sm_overlap(m, one));
	CHECK(!r, "overlap disjoint");
	TIMED("overlap nested", r = sm_overlap(m, in));
	CHECK(r, "overlap nested");
	TIMED("subset nested", r = sm_is_subset(in, m));
	CHECK(r, "subset nested");
	TIMED("subset run in small", r = sm_is_subset(m, in));
	CHECK(!r, "subset run in small");
	TIMED("subset self", r = sm_is_subset(m, m));
	CHECK(r, "subset self");
	TIMED("nonempty_difference", r = sm_nonempty_difference(m, in));
	CHECK(r, "nonempty_difference");

	c = sm_copy(m);
	TIMED("remove_range middle", r = sm_remove_range(c, lo + 1000, hi - 1000));
	CHECK(r && sm_validate(c) && sm_cardinality(c) == 2000,
	    "remove_range middle: card %zu", sm_cardinality(c));
	CHECK(sm_contains(c, lo + 999, NULL) && !sm_contains(c, lo + 1000, NULL) &&
	        !sm_contains(c, hi - 1001, NULL) && sm_contains(c, hi - 1000, NULL),
	    "remove_range middle: edges");
	sm_free(c);

	c = sm_copy(m);
	TIMED("flip_range straddle", r = sm_flip_range(c, hi - 5, hi + P31));
	CHECK(r && sm_validate(c) &&
	        (uint64_t)sm_cardinality(c) == P31 - 5 + P31,
	    "flip_range straddle: card %zu", sm_cardinality(c));
	CHECK(sm_contains(c, hi - 6, NULL) && !sm_contains(c, hi - 5, NULL) &&
	        !sm_contains(c, hi - 1, NULL) && sm_contains(c, hi, NULL) &&
	        sm_contains(c, hi + P31 - 1, NULL) &&
	        !sm_contains(c, hi + P31, NULL),
	    "flip_range straddle: edges");
	sm_free(c);

	sm_free(m);
	sm_free(one);
	sm_free(in);
}

int
main(void)
{
	if (SIZE_MAX < UINT64_MAX) {
		printf("test_range_ops: skipped, size_t holds no 2^32 count\n");
		return (77);
	}
	edges();
	random_cases();
	long_runs();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return (1);
	}
	printf("test_range_ops: ok\n");
	return (0);
}
