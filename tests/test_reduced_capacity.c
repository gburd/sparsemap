/* SPDX-License-Identifier: MIT */
/*
 * test_reduced_capacity.c - sparse chunks with NONE slots (R1).
 *
 * sm_offset emits a sparse chunk whose trailing slots are NONE, so its
 * capacity is less than the 2048-bit window it starts.  Through 5.8.5
 * __sm_get_chunk_offset stepped past a chunk once idx >= start +
 * capacity, so an index inside such a chunk's window but past its
 * capacity looked up the NEXT chunk (or the map end).  sm_add then took
 * an insert path and wrote a second chunk with the same aligned start:
 * sm_validate false, and every later reader (sm_minimum, sm_split,
 * cursor walks) misbehaved.  A sparse chunk now owns its whole window,
 * so the lookup stops on it and sm_add widens its capacity in place.
 *
 * sm_union and sm_difference had the same fault: where an RLE run
 * overlapped such a chunk they encoded the result with the sparse
 * chunk's reduced capacity and then emitted the run's remaining window
 * bits as a second chunk at the same start.
 *
 * Each case shifts a two-member map by an offset that leaves a reduced
 * chunk, runs the four set ops (both orders of difference) between it
 * and RLE runs over that chunk's window, writes into the uncovered tail
 * of the window through one writer, then splits the result there and
 * runs the set ops on the halves.  Every step is checked against a
 * byte-per-bit oracle: sm_validate, cardinality, every bit, and
 * sm_next_member order.
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
#define SPAN (8 * W) /* every member here is below SPAN */

/* The oracle: one byte per bit of [0, SPAN). */
static uint8_t want[SPAN];

/* 1 when m matches `o` on every bit of [0, SPAN). */
static int
same(sm_t *m, const uint8_t *o, const char *what)
{
	uint64_t x, prev = SM_IDX_MAX, card = 0;

	if (!sm_validate(m)) {
		CHECK(0, "%s: sm_validate", what);
		return (0);
	}
	for (x = 0; x < SPAN; x++) {
		if ((int)sm_contains(m, x, NULL) != o[x]) {
			CHECK(0, "%s: bit %llu want %d", what,
			    (unsigned long long)x, o[x]);
			return (0);
		}
		card += o[x];
	}
	if ((uint64_t)sm_cardinality(m) != card) {
		CHECK(0, "%s: cardinality %zu want %llu", what,
		    sm_cardinality(m), (unsigned long long)card);
		return (0);
	}
	/* sm_next_member walks the chunks in order. */
	for (x = 0; x < card; x++) {
		const uint64_t n = sm_next_member(m, prev, NULL);
		if (n >= SPAN || !o[n]) {
			CHECK(0, "%s: next_member after %llu = %llu", what,
			    (unsigned long long)prev, (unsigned long long)n);
			return (0);
		}
		prev = n;
	}
	return (1);
}

/*
 * A map {a, b} shifted by `off`.  Returns NULL when the shift leaves the
 * map empty.  The result is regrown so later writes have room.
 */
static sm_t *
shifted(uint64_t a, uint64_t b, long off)
{
	sm_t *src = sm_create(4096);
	sm_t *r;

	sm_add(src, a);
	sm_add(src, b);
	r = sm_offset(src, (ssize_t)off);
	sm_free(src);
	if (r == NULL)
		return (NULL);
	return (sm_set_data_size(r, NULL, 1 << 16));
}

enum how { BY_ADD, BY_ASSIGN, BY_RANGE, BY_MANY, NHOW };
static const char *const hows[] = { "sm_add", "sm_assign", "sm_add_range",
	"sm_add_many" };

/* Write [lo, hi) into m and the oracle. */
static void
write_bits(sm_t *m, enum how how, uint64_t lo, uint64_t hi)
{
	uint64_t x, arr[64];
	size_t n = 0;

	switch (how) {
	case BY_ADD:
		for (x = lo; x < hi; x++)
			CHECK(sm_add(m, x) == x, "sm_add %llu",
			    (unsigned long long)x);
		break;
	case BY_ASSIGN:
		for (x = lo; x < hi; x++)
			CHECK(sm_assign(m, x, true) == x, "sm_assign %llu",
			    (unsigned long long)x);
		break;
	case BY_RANGE:
		CHECK(sm_add_range(m, lo, hi), "sm_add_range");
		break;
	case BY_MANY:
	case NHOW:
		for (x = lo; x < hi && n < 64; x++)
			arr[n++] = x;
		hi = x;
		CHECK(sm_add_many(m, arr, n), "sm_add_many");
		break;
	}
	for (x = lo; x < hi; x++)
		want[x] = 1;
}

/* Split m at `at` and check both halves; then rejoin them. */
static void
split_and_ops(sm_t *m, uint64_t at, const char *what)
{
	static uint8_t lo_half[SPAN], hi_half[SPAN];
	sm_t *other = sm_create(1 << 16);
	sm_t *u, *x, *d;
	uint64_t i;
	char buf[260];

	for (i = 0; i < SPAN; i++) {
		lo_half[i] = i < at ? want[i] : 0;
		hi_half[i] = i >= at ? want[i] : 0;
	}
	snprintf(buf, sizeof(buf), "%s, split at %llu", what,
	    (unsigned long long)at);
	if (sm_split(m, at, other) == SM_IDX_MAX) {
		CHECK(0, "%s: sm_split failed", buf);
		sm_free(other);
		return;
	}
	same(m, lo_half, buf);
	same(other, hi_half, buf);
	if (!sm_is_empty(other)) {
		uint64_t mn = SM_IDX_MAX;
		for (i = at; i < SPAN && mn == SM_IDX_MAX; i++)
			if (hi_half[i])
				mn = i;
		CHECK(sm_minimum(other) == mn, "%s: minimum %llu want %llu",
		    buf, (unsigned long long)sm_minimum(other),
		    (unsigned long long)mn);
	}

	/* The halves are disjoint: union restores, intersection is empty,
	 * xor equals union, difference keeps the low half. */
	u = sm_union(m, other);
	x = sm_xor(m, other);
	d = sm_difference(u, other);
	CHECK(u != NULL && same(u, want, buf), "%s: union", buf);
	CHECK(x != NULL && same(x, want, buf), "%s: xor", buf);
	CHECK(sm_intersection_cardinality(m, other) == 0,
	    "%s: intersection", buf);
	if (d != NULL)
		same(d, lo_half, buf);
	else
		CHECK(sm_is_empty(m), "%s: difference empty", buf);
	sm_free(u);
	sm_free(x);
	sm_free(d);
	sm_free(other);
}

/*
 * The four set ops between m (a reduced chunk) and a run [rlo, rhi)
 * that covers its window, in both operand orders, against the oracle.
 */
static void
ops_with_run(sm_t *m, uint64_t rlo, uint64_t rhi, const char *what)
{
	static uint8_t o_or[SPAN], o_and[SPAN], o_mr[SPAN], o_rm[SPAN],
	    o_xor[SPAN];
	sm_t *run = sm_create(4096);
	sm_t *r[5];
	const uint8_t *o[5] = { o_or, o_and, o_mr, o_rm, o_xor };
	static const char *const name[5] = { "union", "intersection",
		"m - run", "run - m", "xor" };
	uint64_t i;
	int k;
	char buf[260];

	sm_add_range(run, rlo, rhi);
	for (i = 0; i < SPAN; i++) {
		const int in_r = i >= rlo && i < rhi;
		o_or[i] = (uint8_t)(want[i] | in_r);
		o_and[i] = (uint8_t)(want[i] & in_r);
		o_mr[i] = (uint8_t)(want[i] && !in_r);
		o_rm[i] = (uint8_t)(!want[i] && in_r);
		o_xor[i] = (uint8_t)(want[i] ^ in_r);
	}
	r[0] = sm_union(m, run);
	r[1] = sm_intersection(m, run);
	r[2] = sm_difference(m, run);
	r[3] = sm_difference(run, m);
	r[4] = sm_xor(m, run);
	for (k = 0; k < 5; k++) {
		snprintf(buf, sizeof(buf), "%s, %s run [%llu, %llu)", what,
		    name[k], (unsigned long long)rlo, (unsigned long long)rhi);
		if (r[k] != NULL) {
			same(r[k], o[k], buf);
		} else {
			for (i = 0; i < SPAN && !o[k][i]; i++)
				;
			CHECK(i == SPAN, "%s: NULL result", buf);
		}
		sm_free(r[k]);
	}
	sm_free(run);
}

/*
 * The source member `a` lands at a + off.  When a sits at the top of
 * its window and the shift carries it into the next one, or a negative
 * shift pulls a window's start below the output window's, the output
 * chunk keeps a reduced capacity.  Write `len` bits at offset `pos`
 * within that output window, clipped to the window.
 */
static void
one_case(uint64_t a, long off, enum how how, uint64_t pos, uint64_t len)
{
	const uint64_t b = 6 * W + 77;
	const uint64_t at = a + (uint64_t)off;
	const uint64_t win = at / W * W;
	const uint64_t lo = win + pos;
	const uint64_t hi = lo + len < win + W ? lo + len : win + W;
	char what[200];
	sm_t *m = shifted(a, b, off);

	memset(want, 0, sizeof(want));
	want[at] = 1;
	want[b + (uint64_t)off] = 1;
	snprintf(what, sizeof(what), "a=%llu off=%ld %s pos=%llu len=%llu",
	    (unsigned long long)a, off, hows[how], (unsigned long long)pos,
	    (unsigned long long)len);
	if (m == NULL) {
		CHECK(0, "%s: sm_offset", what);
		return;
	}
	if (same(m, want, what)) {
		if (how == BY_ADD && pos == 2047 && len == 1) {
			ops_with_run(m, win - W, win + 2 * W, what);
			ops_with_run(m, win, win + W, what);
			ops_with_run(m, win + 7, win + W + 9, what);
			ops_with_run(m, win + 1500, win + 2 * W, what);
		}
		write_bits(m, how, lo, hi);
		if (same(m, want, what))
			split_and_ops(m, lo, what);
	}
	sm_free(m);
}

/* The review's minimal case. */
static void
r1_offset_redcap(void)
{
	sm_t *m = shifted(6080, 20000, 64);

	CHECK(m != NULL, "r1: sm_offset");
	if (m == NULL)
		return;
	sm_add(m, 6144 + 1000);
	CHECK(sm_validate(m), "r1: sm_validate");
	CHECK(sm_cardinality(m) == 3, "r1: cardinality %zu",
	    sm_cardinality(m));
	CHECK(sm_minimum(m) == 6144, "r1: minimum %llu",
	    (unsigned long long)sm_minimum(m));
	CHECK(sm_next_member(m, 6144, NULL) == 7144, "r1: next");
	sm_free(m);
}

int
main(void)
{
	/* 6143 is a window's last bit; 7144 - 1000 is a window's first. */
	static const uint64_t as[] = { 3 * W - 1, 3 * W + 1000 };
	static const long offs[] = { 1, 63, 64, 65, 200, 1000, -1000, 2047,
		-1 };
	static const uint64_t lens[] = { 1, 70, 2048 };
	size_t ai, oi, pi, li;
	int how;

	r1_offset_redcap();
	for (ai = 0; ai < sizeof(as) / sizeof(as[0]); ai++) {
		for (oi = 0; oi < sizeof(offs) / sizeof(offs[0]); oi++) {
			/* The slot just past the shifted member, then fixed
			 * offsets past every reduced capacity built here. */
			const uint64_t at = as[ai] + (uint64_t)offs[oi];
			const uint64_t poss[] = { at % W / 64 * 64 + 64,
				at % W / 64 * 64 + 65, 1100, 1600, 2047 };
			const size_t np = sizeof(poss) / sizeof(poss[0]);
			const size_t nl = sizeof(lens) / sizeof(lens[0]);
			for (pi = 0; pi < np; pi++)
				for (li = 0; li < nl; li++)
					for (how = 0; how < NHOW; how++)
						one_case(as[ai], offs[oi],
						    (enum how)how, poss[pi],
						    lens[li]);
		}
	}
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return (1);
	}
	printf("test_reduced_capacity: ok\n");
	return (0);
}
