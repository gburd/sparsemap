/* SPDX-License-Identifier: MIT */
/*
 * test_smallset_query.c - rank, select, span, scan and statistics read a
 * small-set map directly (5.9.0).
 *
 * Through 5.8.5 these five functions answered a small-set map (every
 * member below 1024, stored as bare 64-bit words) by building a chunk-mode
 * copy of it, about 27 us per sm_rank at 334 members against 2 ns for
 * sm_contains.  They now work on the words.  For random small sets (0 to
 * 1023 members, empty, full 0..1023, single members, word-boundary
 * members, runs from bit 0 of every length), each function must agree
 * with:
 *
 *   1. a bit-array oracle; and
 *   2. the chunk-mode path, on a copy of the same set shifted up by 4096
 *      (two chunk windows), which is always in chunk mode: rank and select
 *      there are what the old materialized copy computed.
 *
 * sm_statistics on a small map reported the chunk breakdown of that
 * materialized copy.  The expected values are derived here from the words
 * (one chunk; RLE iff the set is a run from bit 0 that ends mid-word or is
 * longer than 128 bits; bytes_per_set_bit from that chunk's size), and
 * this file passes unchanged against v5.8.5's materializing sm_statistics,
 * which pins the derivation to the old behaviour.
 *
 * The small map is built from small-set bytes with sm_open_copy, so it
 * is in small mode whatever the set's shape (sm_add would choose the
 * chunk form for a set it stores smaller that way).
 *
 * Failures are counted per function so a run against v5.8.5 shows which
 * answers changed: select0 and unset spans past the last member (the
 * defect fixed with test_select0.c), and select1(SM_IDX_MAX), which
 * 5.8.5 wrapped through ssize_t.
 */
#define SM_EXPOSE_STRUCT 1
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NBITS 1024u
#define SHIFT 4096u

enum { F_RANK, F_SELECT1, F_SELECT0, F_SPAN1, F_SPAN0, F_SCAN, F_STATS,
	F_MODE, F_N };
static const char *fname[F_N] = { "rank", "select1", "select0", "span1",
	"span0", "scan", "statistics", "mode" };
static long failures[F_N];
static long checks;

static uint64_t rs = UINT64_C(0x9E3779B97F4A7C15);

static uint64_t
rnd(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return (rs);
}

static void
check(int f, int ok, const char *what, uint64_t a, uint64_t got,
    uint64_t want)
{
	checks++;
	if (!ok) {
		failures[f]++;
		if (failures[f] <= 5)
			fprintf(stderr, "FAIL %s %s(%llu): %llu, want %llu\n",
			    fname[f], what, (unsigned long long)a,
			    (unsigned long long)got, (unsigned long long)want);
	}
}

/* The oracle: bit i of the set, for i < NBITS. */
static unsigned char bit[NBITS];

static int
oracle_has(uint64_t x)
{
	return (x < NBITS && bit[x]);
}

static size_t
oracle_rank(uint64_t x, uint64_t y, int value)
{
	size_t c = 0;
	uint64_t i;

	if (x > y)
		return (0);
	for (i = x; i <= y && i < NBITS; i++)
		c += (size_t)(bit[i] == value);
	if (!value && y >= NBITS)
		c += (size_t)(y - (x > NBITS ? x : NBITS) + 1);
	return (c);
}

static uint64_t
oracle_select(uint64_t n, int value)
{
	uint64_t i;

	for (i = 0; i < NBITS; i++) {
		if (bit[i] == value) {
			if (n == 0)
				return (i);
			n--;
		}
	}
	if (value)
		return (SM_IDX_MAX);
	return (NBITS + n); /* n is small in every call */
}

/* First p >= idx with [p, p + len - 1] all equal to value (len 0 acts as
 * len 1).  Every position from NBITS on is unset, so an unset span that
 * has not started below NBITS starts at max(idx, NBITS).  The callers
 * keep idx + len far below 2^64. */
static uint64_t
oracle_span(uint64_t idx, size_t len, int value)
{
	uint64_t p;

	for (p = idx; p < NBITS; p++) {
		uint64_t k;
		int ok = oracle_has(p) == value;
		for (k = 1; ok && k < len; k++)
			ok = oracle_has(p + k) == value;
		if (ok)
			return (p);
	}
	if (value)
		return (SM_IDX_MAX);
	return (idx > NBITS ? idx : NBITS);
}

/* Scan collector. */
typedef struct {
	uint64_t v[NBITS + 64];
	size_t n;
	int bad_batch;
} scan_t;

static void
collect(uint64_t vec[], size_t n, void *aux)
{
	scan_t *s = aux;
	size_t i;

	if (n == 0 || n > 64)
		s->bad_batch = 1;
	for (i = 0; i < n && s->n < NBITS + 64; i++)
		s->v[s->n++] = vec[i];
}

static int
is_small(const sm_t *m)
{
	uint64_t hdr;

	if (m->m_data_used < 8)
		return (0);
	memcpy(&hdr, m->m_data, sizeof(hdr));
	return ((int)(hdr >> 63));
}

/* Expected sm_statistics for the current oracle set, from the words. */
static void
expected_stats(const sm_t *m, sm_stats_t *e)
{
	uint64_t card = 0, run = 0, mixed = 0, i;
	int from_zero = 1;
	size_t bytes;

	memset(e, 0, sizeof(*e));
	e->bytes_used = m->m_data_used;
	e->bytes_capacity = sm_get_capacity(m);
	for (i = 0; i < NBITS; i++) {
		card += bit[i];
		if (bit[i] && i != run)
			from_zero = 0;
		if (bit[i] && i == run)
			run++;
	}
	for (i = 0; i < NBITS / 64; i++) {
		uint64_t j, ones = 0;
		for (j = 0; j < 64; j++)
			ones += bit[i * 64 + j];
		if (ones != 0 && ones != 64)
			mixed++;
	}
	if (card == 0)
		return;
	e->chunks_total = 1;
	e->bits_set = card;
	from_zero = from_zero && run == card;
	if (from_zero && (run % 64 != 0 || run > 128)) {
		e->chunks_rle = 1;
		e->bits_in_rle = card;
		bytes = 24;
	} else {
		e->chunks_sparse = 1;
		e->bits_in_sparse = card;
		bytes = (size_t)(24 + 8 * mixed);
		if (from_zero)
			bytes = 24; /* a whole-word run: all ONES slots */
	}
	e->bytes_per_set_bit = (double)bytes / (double)card;
}

static void
check_stats(const sm_t *m)
{
	sm_stats_t got, want;

	sm_statistics(m, &got);
	expected_stats(m, &want);
	check(F_STATS, got.chunks_total == want.chunks_total, "chunks_total",
	    0, got.chunks_total, want.chunks_total);
	check(F_STATS, got.chunks_rle == want.chunks_rle, "chunks_rle", 0,
	    got.chunks_rle, want.chunks_rle);
	check(F_STATS, got.chunks_sparse == want.chunks_sparse,
	    "chunks_sparse", 0, got.chunks_sparse, want.chunks_sparse);
	check(F_STATS, got.bytes_used == want.bytes_used, "bytes_used", 0,
	    got.bytes_used, want.bytes_used);
	check(F_STATS, got.bytes_capacity == want.bytes_capacity,
	    "bytes_capacity", 0, got.bytes_capacity, want.bytes_capacity);
	check(F_STATS, got.bits_set == want.bits_set, "bits_set", 0,
	    got.bits_set, want.bits_set);
	check(F_STATS, got.bits_in_rle == want.bits_in_rle, "bits_in_rle", 0,
	    got.bits_in_rle, want.bits_in_rle);
	check(F_STATS, got.bits_in_sparse == want.bits_in_sparse,
	    "bits_in_sparse", 0, got.bits_in_sparse, want.bits_in_sparse);
	check(F_STATS, got.bytes_per_set_bit == want.bytes_per_set_bit,
	    "bytes_per_set_bit x1e6", 0,
	    (uint64_t)(got.bytes_per_set_bit * 1e6),
	    (uint64_t)(want.bytes_per_set_bit * 1e6));
}

static void
check_scan(sm_t *m, sm_t *c, size_t skip)
{
	static scan_t a, b;
	size_t i, card = 0;
	uint64_t x;

	a.n = b.n = 0;
	a.bad_batch = b.bad_batch = 0;
	sm_scan(m, collect, skip, &a);
	sm_scan(c, collect, skip, &b);
	for (x = 0; x < NBITS; x++)
		card += bit[x];
	check(F_SCAN, !a.bad_batch, "batch size", skip, 0, 0);
	check(F_SCAN, a.n == (skip < card ? card - skip : 0), "count", skip,
	    a.n, skip < card ? card - skip : 0);
	check(F_SCAN, a.n == b.n, "count vs chunk", skip, a.n, b.n);
	for (i = 0; i < a.n && i < b.n; i++) {
		check(F_SCAN, a.v[i] + SHIFT == b.v[i], "value vs chunk", i,
		    a.v[i], b.v[i] - SHIFT);
		check(F_SCAN, oracle_has(a.v[i]) && (i == 0 ||
		    a.v[i] > a.v[i - 1]), "value vs oracle", i, a.v[i], 0);
	}
}

/* Positions worth probing: word and set edges plus random ones. */
static uint64_t
probe_point(unsigned i)
{
	static const uint64_t fixed[] = { 0, 1, 62, 63, 64, 65, 127, 128,
		511, 512, 1022, 1023, 1024, 1025, 2047, 2048, 5000 };

	if (i < sizeof(fixed) / sizeof(fixed[0]))
		return (fixed[i]);
	return (rnd() % (NBITS + 200));
}

/* The small-set encoding of the oracle set: a header word with the top
 * bit set and the word count, then the words, trailing zero words
 * trimmed (sm_validate rejects them). */
static sm_t *
small_map(void)
{
	uint64_t buf[1 + NBITS / 64];
	size_t nw = 0;
	size_t i;
	sm_t *m;

	memset(buf, 0, sizeof(buf));
	for (i = 0; i < NBITS; i++) {
		if (bit[i]) {
			buf[1 + i / 64] |= UINT64_C(1) << (i % 64);
			nw = i / 64 + 1;
		}
	}
	buf[0] = (UINT64_C(1) << 63) | nw;
	m = sm_open_copy((const uint8_t *)buf, 8 + 8 * nw, 0);
	if (m == NULL) {
		fprintf(stderr, "sm_open_copy failed\n");
		exit(2);
	}
	return (m);
}

static void
check_set(void)
{
	sm_t *m = small_map();
	sm_t *c = sm_create(256);
	size_t card = 0;
	unsigned i;
	uint64_t x;

	for (x = 0; x < NBITS; x++) {
		if (!bit[x])
			continue;
		card++;
		if (sm_add_grow(&c, x + SHIFT) == SM_IDX_MAX) {
			fprintf(stderr, "sm_add_grow failed\n");
			exit(2);
		}
	}
	check(F_MODE, is_small(m) && sm_validate(m), "small mode", card,
	    (uint64_t)is_small(m), 1);
	check(F_MODE, !is_small(c), "shifted copy in chunk mode", card,
	    (uint64_t)is_small(c), 0);

	for (i = 0; i < 60; i++) {
		uint64_t a = probe_point(i), b = probe_point(i + 7 * (i & 1));
		size_t r;
		if (a > b) {
			const uint64_t t = a;
			a = b;
			b = t;
		}
		r = sm_rank(m, a, b, true);
		check(F_RANK, r == oracle_rank(a, b, 1), "rank1 lo", a, r,
		    oracle_rank(a, b, 1));
		check(F_RANK, r == sm_rank(c, a + SHIFT, b + SHIFT, true),
		    "rank1 vs chunk lo", a, r,
		    sm_rank(c, a + SHIFT, b + SHIFT, true));
		r = sm_rank(m, a, b, false);
		check(F_RANK, r == oracle_rank(a, b, 0), "rank0 lo", a, r,
		    oracle_rank(a, b, 0));
		check(F_RANK, r == sm_rank(c, a + SHIFT, b + SHIFT, false),
		    "rank0 vs chunk lo", a, r,
		    sm_rank(c, a + SHIFT, b + SHIFT, false));
	}
	check(F_RANK, sm_rank(m, 0, SM_IDX_MAX, true) == card, "rank1 all", 0,
	    sm_rank(m, 0, SM_IDX_MAX, true), card);
	check(F_RANK, sm_rank(m, 5, 4, true) == 0, "rank1 empty range", 5,
	    sm_rank(m, 5, 4, true), 0);
	if (sizeof(size_t) >= sizeof(uint64_t))
		check(F_RANK, sm_rank(m, 0, SM_IDX_MAX - 1, false) ==
		    (size_t)(SM_IDX_MAX - card), "rank0 all", 0,
		    sm_rank(m, 0, SM_IDX_MAX - 1, false), SM_IDX_MAX - card);

	for (x = 0; x <= card; x++) {
		const uint64_t s = sm_select(m, x, true);
		const uint64_t sc = sm_select(c, x, true);
		check(F_SELECT1, s == oracle_select(x, 1), "select1", x, s,
		    oracle_select(x, 1));
		check(F_SELECT1, s == (sc == SM_IDX_MAX ? sc : sc - SHIFT),
		    "select1 vs chunk", x, s, sc);
	}
	check(F_SELECT1, sm_select(m, SM_IDX_MAX, true) == SM_IDX_MAX,
	    "select1 MAX", 0, sm_select(m, SM_IDX_MAX, true), SM_IDX_MAX);
	for (x = 0; x < NBITS - card + 300; x++) {
		const uint64_t s = sm_select(m, x, false);
		const uint64_t sc = sm_select(c, x + SHIFT, false);
		check(F_SELECT0, s == oracle_select(x, 0), "select0", x, s,
		    oracle_select(x, 0));
		check(F_SELECT0, s + SHIFT == sc, "select0 vs chunk", x, s,
		    sc - SHIFT);
	}
	check(F_SELECT0, sm_select(m, SM_IDX_MAX - 1 - card, false) ==
	    SM_IDX_MAX - 1, "select0 last", 0,
	    sm_select(m, SM_IDX_MAX - 1 - card, false), SM_IDX_MAX - 1);
	check(F_SELECT0, sm_select(m, SM_IDX_MAX - card, false) == SM_IDX_MAX,
	    "select0 past last", 0, sm_select(m, SM_IDX_MAX - card, false),
	    SM_IDX_MAX);

	for (i = 0; i < 30; i++) {
		static const size_t lens[] = { 0, 1, 2, 3, 63, 64, 65, 200 };
		const uint64_t idx = probe_point(i);
		const size_t len = i < 8 ? lens[i] : (size_t)(rnd() % 130);
		uint64_t s = sm_span(m, idx, len, true);
		uint64_t sc = sm_span(c, idx + SHIFT, len, true);
		check(F_SPAN1, s == oracle_span(idx, len, 1), "span1", idx, s,
		    oracle_span(idx, len, 1));
		check(F_SPAN1, s == (sc == SM_IDX_MAX ? sc : sc - SHIFT),
		    "span1 vs chunk", idx, s, sc);
		s = sm_span(m, idx, len, false);
		sc = sm_span(c, idx + SHIFT, len, false);
		check(F_SPAN0, s == oracle_span(idx, len, 0), "span0", idx, s,
		    oracle_span(idx, len, 0));
		check(F_SPAN0, s == (sc == SM_IDX_MAX ? sc : sc - SHIFT),
		    "span0 vs chunk", idx, s, sc);
	}
	/* 10 + (2^64 - 1) - 1 passes the top of the universe.  Through
	 * 5.8.5 this looped over 2^64 unset bits; SM_TEST_NO_HANG skips it
	 * to see the other differences on an old build. */
	if (sizeof(size_t) >= sizeof(uint64_t) &&
	    getenv("SM_TEST_NO_HANG") == NULL) {
		check(F_SPAN0, sm_span(m, 10, SIZE_MAX, false) == SM_IDX_MAX,
		    "span0 longer than the universe", 10,
		    sm_span(m, 10, SIZE_MAX, false), SM_IDX_MAX);
		check(F_SPAN0, sm_span(c, 10 + SHIFT, SIZE_MAX, false) ==
		    SM_IDX_MAX, "span0 longer than the universe (chunk)", 10,
		    sm_span(c, 10 + SHIFT, SIZE_MAX, false), SM_IDX_MAX);
	}
	check(F_SPAN0, sm_span(m, SM_IDX_MAX - 10, 11, false) ==
	    SM_IDX_MAX - 10, "span0 to the top", 0,
	    sm_span(m, SM_IDX_MAX - 10, 11, false), SM_IDX_MAX - 10);

	check_scan(m, c, 0);
	check_scan(m, c, 1);
	check_scan(m, c, card / 2);
	check_scan(m, c, card);
	check_scan(m, c, card + 5);
	check_scan(m, c, (size_t)(rnd() % (card + 1)));

	check_stats(m);
	sm_free(m);
	sm_free(c);
}

int
main(void)
{
	long total = 0;
	unsigned r, L;
	uint64_t x;
	int f;

	/* Empty, full, single members, word boundaries. */
	memset(bit, 0, sizeof(bit));
	check_set();
	memset(bit, 1, sizeof(bit));
	check_set();
	for (x = 0; x < NBITS; x += (x < 130 ? 1 : 61)) {
		memset(bit, 0, sizeof(bit));
		bit[x] = 1;
		check_set();
	}
	memset(bit, 0, sizeof(bit));
	for (x = 63; x < NBITS; x += 64) {
		bit[x] = 1;
		bit[x + 1 < NBITS ? x + 1 : x] = 1;
	}
	check_set();

	/* Runs from bit 0 of every length: the RLE / sparse boundary. */
	for (L = 1; L <= NBITS; L += (L < 260 ? 1 : 37)) {
		memset(bit, 0, sizeof(bit));
		memset(bit, 1, L);
		check_set();
	}

	/* Random sets of varied density, including a run not from 0. */
	for (r = 0; r < 400; r++) {
		const unsigned density = 1 + (unsigned)(rnd() % 100);
		const uint64_t top = 1 + rnd() % NBITS;
		memset(bit, 0, sizeof(bit));
		for (x = 0; x < top; x++)
			bit[x] = (unsigned char)(rnd() % 100 < density);
		if (r % 10 == 0)
			memset(bit + 100, 1, 300);
		check_set();
	}

	for (f = 0; f < F_N; f++) {
		if (failures[f] != 0)
			fprintf(stderr, "test_smallset_query: %s: %ld failures\n",
			    fname[f], failures[f]);
		total += failures[f];
	}
	if (total != 0) {
		fprintf(stderr, "test_smallset_query: %ld of %ld checks failed\n",
		    total, checks);
		return (1);
	}
	printf("test_smallset_query: ok (%ld checks)\n", checks);
	return (0);
}
