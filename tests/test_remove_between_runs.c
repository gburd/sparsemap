/* SPDX-License-Identifier: MIT */
/*
 * test_remove_between_runs.c - emptying a chunk next to a run (R4).
 *
 * In the RLE variant through 5.8.5, when a clear emptied a sparse chunk,
 * __sm_map_unset removed the chunk's bytes and then coalesced with the
 * descriptor and start index of the chunk it had just removed.  Those
 * bytes then held the right neighbour, so with an RLE run on the left
 * the coalesce absorbed the right neighbour as if it were the emptied
 * chunk and the last 2048 bits of the right run were lost.
 *
 * The RLE-free variant stores a run as adjacent all-ONES sparse chunks
 * and its coalesce is a no-op, so it never had that fault.  This test
 * proves it: it passes on no-rle-v5.8.5 as well, and stays as a guard.
 * sm_remove, sm_remove_range, sm_flip_range and sm_pop_* all clear
 * through the path.
 *
 * Each case builds three neighbouring windows (left, middle, right),
 * each a run or a sparse chunk, clears some or all of the middle
 * chunk's members through one of the clearing APIs, and checks the map
 * against an interval oracle: sm_validate, cardinality, every bit of
 * a window around the change, and every bit within 2112 of each run
 * boundary (so the far end of a long run is checked too).  The long
 * runs are 40 windows, not the RLE variant's 2^31 bits: here a run
 * costs one chunk per 2048 bits, so 2^31 would be a 16 MiB map whose
 * every unaccelerated lookup walks a million chunks.
 */
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define W UINT64_C(2048)
#define P31 (UINT64_C(1) << 31)
#define LONG_RUN (40 * W)
#define MAXB 64

/*
 * The oracle: a set of disjoint half-open intervals stored as their
 * sorted boundaries.  x is a member when an odd number of boundaries
 * are <= x.
 */
struct oracle {
	uint64_t b[MAXB];
	size_t n;
};

static int
o_member(const struct oracle *o, uint64_t x)
{
	size_t i, k = 0;

	for (i = 0; i < o->n && o->b[i] <= x; i++)
		k++;
	return ((int)(k & 1));
}

/* Set every bit of [lo, hi) to v. */
static void
o_set(struct oracle *o, uint64_t lo, uint64_t hi, int v)
{
	const int before = lo > 0 ? o_member(o, lo - 1) : 0;
	const int after = o_member(o, hi);
	uint64_t nb[MAXB];
	size_t i, n = 0;

	for (i = 0; i < o->n && o->b[i] < lo; i++)
		nb[n++] = o->b[i];
	if (before != v)
		nb[n++] = lo;
	if (v != after)
		nb[n++] = hi;
	while (i < o->n && o->b[i] <= hi)
		i++;
	for (; i < o->n; i++)
		nb[n++] = o->b[i];
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

/* 1 when every bit of [lo, hi) agrees with the oracle. */
static int
window_ok(sm_t *m, const struct oracle *o, uint64_t lo, uint64_t hi,
    const char *what)
{
	uint64_t x;

	for (x = lo; x < hi; x++) {
		const int want = o_member(o, x);
		if ((int)sm_contains(m, x, NULL) != want) {
			CHECK(0, "%s: bit %llu want %d", what,
			    (unsigned long long)x, want);
			return (0);
		}
	}
	return (1);
}

static void
check(sm_t *m, const struct oracle *o, uint64_t lo, uint64_t hi,
    const char *what)
{
	size_t i;

	CHECK(sm_validate(m), "%s: sm_validate", what);
	CHECK((uint64_t)sm_cardinality(m) == o_card(o),
	    "%s: cardinality %zu want %llu", what, sm_cardinality(m),
	    (unsigned long long)o_card(o));
	if (!window_ok(m, o, lo, hi, what))
		return;
	for (i = 0; i < o->n; i++) {
		const uint64_t b = o->b[i];
		const uint64_t a = b > 2112 ? b - 2112 : 0;
		if (!window_ok(m, o, a, b + 2112, what))
			return;
	}
}

/*
 * One neighbour window.  A run neighbour is a run of `len` bits
 * touching the middle window (ending at its start on the left,
 * starting `skip` bits past its end on the right); a sparse neighbour
 * is a few scattered bits in the adjacent window.
 */
struct side {
	int rle;
	uint64_t len;
	uint64_t skip;
};

/* The middle window's members, as offsets [lo, hi) within it. */
struct fill {
	uint64_t lo, hi;
};

static const struct fill fills[] = {
	{ 2047, 2048 }, { 0, 1 }, { 5, 70 }, { 0, 1024 }, { 1, 2048 },
	{ 0, 2047 }, { 1000, 1100 },
};
#define NFILLS (sizeof(fills) / sizeof(fills[0]))

static const struct side lefts[] = {
	{ 1, W, 0 }, { 1, 3 * W, 0 }, { 1, 3 * W - 5, 0 },
	{ 1, LONG_RUN + 2 * W, 0 }, { 0, 0, 0 },
};
#define NLEFTS (sizeof(lefts) / sizeof(lefts[0]))

static const struct side rights[] = {
	{ 1, W, 0 }, { 1, 3 * W, 0 }, { 1, 3 * W, 7 }, { 1, LONG_RUN + W, 0 },
	{ 0, 0, 0 },
};
#define NRIGHTS (sizeof(rights) / sizeof(rights[0]))

/* Add [lo, hi) to both, in the way a caller building runs would. */
static void
put(sm_t *m, struct oracle *o, uint64_t lo, uint64_t hi)
{
	uint64_t x;

	if (hi - lo >= W) {
		CHECK(sm_add_range(m, lo, hi), "sm_add_range");
	} else {
		for (x = lo; x < hi; x++)
			CHECK(sm_add(m, x) == x, "sm_add %llu",
			    (unsigned long long)x);
	}
	o_set(o, lo, hi, 1);
}

/*
 * Build left | middle | right with the middle window at `mid`.  The
 * left run ends at mid; a sparse left holds a few bits of the window
 * before mid.  Sparse windows hold bits 3, 100 and 2047 (the window's
 * edge), so the middle chunk never touches a sparse neighbour's run.
 */
static sm_t *
build(const struct side *l, const struct fill *f, const struct side *r,
    uint64_t mid, struct oracle *o)
{
	sm_t *m = sm_create(1 << 16);
	const uint64_t rs = mid + W;

	o->n = 0;
	if (l->rle) {
		put(m, o, mid - l->len, mid);
	} else {
		put(m, o, mid - W + 3, mid - W + 4);
		put(m, o, mid - W + 100, mid - W + 101);
		put(m, o, mid - 1, mid);
	}
	put(m, o, mid + f->lo, mid + f->hi);
	if (r->rle) {
		put(m, o, rs + r->skip, rs + r->len);
	} else {
		put(m, o, rs + 3, rs + 4);
		put(m, o, rs + 100, rs + 101);
		put(m, o, rs + 2047, rs + 2048);
	}
	return (m);
}

enum how { BY_REMOVE, BY_RANGE, BY_FLIP, NHOW };
static const char *const hows[] = { "sm_remove", "sm_remove_range",
	"sm_flip_range" };

/* Clear [lo, hi) of the middle window's members through `how`. */
static void
clear(sm_t *m, struct oracle *o, enum how how, uint64_t lo, uint64_t hi)
{
	uint64_t x;

	switch (how) {
	case BY_REMOVE:
		for (x = lo; x < hi; x++)
			CHECK(sm_remove(m, x) == x, "sm_remove %llu",
			    (unsigned long long)x);
		break;
	case BY_RANGE:
		CHECK(sm_remove_range(m, lo, hi), "sm_remove_range");
		break;
	case BY_FLIP:
	case NHOW:
		/* [lo, hi) is all members, so flipping it clears it. */
		CHECK(sm_flip_range(m, lo, hi), "sm_flip_range");
		break;
	}
	o_set(o, lo, hi, 0);
}

/*
 * One case: build left | middle | right, clear [lo, hi) of the middle
 * window (offsets within it) through `how`, and check before and after.
 */
static void
one_case(const struct side *l, const struct fill *f, const struct side *r,
    uint64_t lo, uint64_t hi, enum how how)
{
	const uint64_t mid = P31 + 4 * W;
	struct oracle o;
	char what[160];
	sm_t *m;

	snprintf(what, sizeof(what),
	    "left %s %llu | mid [%llu, %llu) | right %s %llu+%llu, "
	    "%s [%llu, %llu)",
	    l->rle ? "run" : "sparse", (unsigned long long)l->len,
	    (unsigned long long)f->lo, (unsigned long long)f->hi,
	    r->rle ? "run" : "sparse", (unsigned long long)r->skip,
	    (unsigned long long)r->len, hows[how], (unsigned long long)lo,
	    (unsigned long long)hi);
	m = build(l, f, r, mid, &o);
	check(m, &o, mid - 2 * W, mid + 3 * W, what);
	clear(m, &o, how, mid + lo, mid + hi);
	check(m, &o, mid - 2 * W, mid + 3 * W, what);
	sm_free(m);
}

/*
 * Every layout, fill and clearing API.  The middle chunk is emptied
 * whole, or keeps its first or last member.
 */
static void
sweep(void)
{
	size_t li, fi, ri;
	int how;

	for (li = 0; li < NLEFTS; li++) {
		for (fi = 0; fi < NFILLS; fi++) {
			for (ri = 0; ri < NRIGHTS; ri++) {
				for (how = 0; how < NHOW; how++) {
					const struct fill *f = &fills[fi];
					const enum how h = (enum how)how;

					one_case(&lefts[li], f, &rights[ri],
					    f->lo, f->hi, h);
					if (f->hi - f->lo < 2)
						continue;
					one_case(&lefts[li], f, &rights[ri],
					    f->lo + 1, f->hi, h);
					one_case(&lefts[li], f, &rights[ri],
					    f->lo, f->hi - 1, h);
				}
			}
		}
	}
}

/* The review's minimal case, at the bottom of the universe. */
static void
r4_one(void)
{
	sm_t *m = sm_create(1 << 16);

	sm_add_range(m, 0, 0x1000);
	sm_add(m, 0x17ff);
	sm_add_range(m, 0x1800, 1 << 20);
	sm_remove(m, 0x17ff);
	CHECK(sm_validate(m), "r4_one: sm_validate");
	CHECK(sm_cardinality(m) == 0x1000 + (1 << 20) - 0x1800,
	    "r4_one: cardinality %zu", sm_cardinality(m));
	CHECK(!sm_contains(m, 0x1000, NULL), "r4_one: 0x1000 set");
	CHECK(!sm_contains(m, 0x17ff, NULL), "r4_one: 0x17ff set");
	CHECK(sm_contains(m, 0xfff, NULL), "r4_one: 0xfff clear");
	CHECK(sm_contains(m, 0x1800, NULL), "r4_one: 0x1800 clear");
	CHECK(sm_contains(m, 0xfffff, NULL), "r4_one: 0xfffff clear");
	CHECK(!sm_contains(m, 0x100000, NULL), "r4_one: 0x100000 set");
	sm_free(m);
}

/* sm_pop_first empties a sparse chunk that has a run on its right. */
static void
pop_first(void)
{
	sm_t *m = sm_create(1 << 16);
	struct oracle o;

	o.n = 0;
	put(m, &o, 2 * W + 9, 2 * W + 10);
	put(m, &o, 3 * W, 6 * W);
	put(m, &o, 7 * W, 9 * W);
	CHECK(sm_pop_first(m) == 2 * W + 9, "pop_first: value");
	o_set(&o, 2 * W + 9, 2 * W + 10, 0);
	check(m, &o, 0, 10 * W, "pop_first");
	sm_free(m);
}

int
main(void)
{
	r4_one();
	pop_first();
	sweep();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return (1);
	}
	printf("test_remove_between_runs: ok\n");
	return (0);
}
