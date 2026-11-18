/* SPDX-License-Identifier: MIT */
/*
 * test_select0.c - sm_select(map, n, false) across chunk gaps (5.9.0).
 *
 * Through 5.8.5 sm_select(map, n, false) counted only the unset bits
 * before the first chunk and inside chunks.  It never counted the unset
 * bits between chunks or after the last one, so on a map with gaps it
 * answered too far right (map {0, 1, 2, 10000, 10001, 50000}:
 * select0(2045) returned 8192, not 2048) or gave up (map {8192}:
 * select0(8192) returned SM_IDX_MAX, not 8193).  sm_rank(..., false)
 * was already right.  The universe is [0, 2^64 - 1), so select0 is
 * defined for every n below 2^64 - 1 - cardinality.
 *
 * For every map below (empty, small-set, sparse, gaps, RLE runs, chunk
 * boundaries, odd RLE capacities from sm_offset, a crafted chunk with
 * NONE slots, members near the top of the universe), against a sorted
 * member array:
 *   1. select0(k) equals the oracle for k over [0, max + 4096], around
 *      every member and every chunk boundary, at random k, and at the
 *      last defined k and the first undefined one;
 *   2. select0(rank0(0, x - 1)) == x for every unset x checked;
 *   3. rank0(x, y) == (y - x + 1) - rank1(x, y) == the oracle count;
 *   4. select1(k) is the k-th member and select1(card) == SM_IDX_MAX.
 */
#define SM_EXPOSE_STRUCT 1
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 2048u

static long failures;
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

/* The oracle: a sorted array of distinct members. */
typedef struct {
	uint64_t *v;
	size_t n;
} oracle_t;

static int
cmp_u64(const void *a, const void *b)
{
	const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x < y ? -1 : x > y);
}

/* Number of members <= x. */
static size_t
count_le(const oracle_t *o, uint64_t x)
{
	size_t lo = 0, hi = o->n;

	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (o->v[mid] <= x)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (lo);
}

/*
 * The k-th unset position is k + j, where j is the number of members
 * below it: the smallest j with v[j] - j > k (v[j] - j never decreases).
 */
static uint64_t
oracle_select0(const oracle_t *o, uint64_t k)
{
	size_t lo = 0, hi = o->n;

	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (o->v[mid] - mid > k)
			hi = mid;
		else
			lo = mid + 1;
	}
	if (k >= SM_IDX_MAX - lo)
		return (SM_IDX_MAX);
	return (k + lo);
}

static void
check_eq(const char *map, const char *what, uint64_t arg, uint64_t got,
    uint64_t want)
{
	checks++;
	if (got != want) {
		failures++;
		if (failures < 40)
			fprintf(stderr, "FAIL %s: %s(%llu) = %llu, want %llu\n",
			    map, what, (unsigned long long)arg,
			    (unsigned long long)got, (unsigned long long)want);
	}
}

static void
check_select0(const char *name, sm_t *m, const oracle_t *o, uint64_t k)
{
	check_eq(name, "select0", k, sm_select(m, k, false),
	    oracle_select0(o, k));
}

/* Check 2 and 3 at one unset x. */
static void
check_unset_x(const char *name, sm_t *m, const oracle_t *o, uint64_t x)
{
	const uint64_t k = (x == 0) ? 0 : x - count_le(o, x - 1);

	if (x == 0) {
		check_eq(name, "select0(rank0) at", x, sm_select(m, 0, false),
		    0);
		return;
	}
	check_eq(name, "rank0(0, x-1) at", x, sm_rank(m, 0, x - 1, false),
	    k);
	check_eq(name, "select0(rank0) at", x,
	    sm_select(m, sm_rank(m, 0, x - 1, false), false), x);
}

static void
check_rank(const char *name, sm_t *m, const oracle_t *o, uint64_t x,
    uint64_t y)
{
	const uint64_t set = count_le(o, y) - (x == 0 ? 0 : count_le(o, x - 1));
	const uint64_t width = y - x + 1;

	if (width == 0 || width > SIZE_MAX)
		return; /* the unset count does not fit a size_t */
	check_eq(name, "rank1 at x", x, sm_rank(m, x, y, true), set);
	check_eq(name, "rank0 at x", x, sm_rank(m, x, y, false),
	    width - set);
	check_eq(name, "rank0 == width - rank1 at x", x,
	    sm_rank(m, x, y, false), width - sm_rank(m, x, y, true));
}

/* Probe an index near p: checks select0 at its oracle rank and, when it
 * is unset, the select0(rank0) identity. */
static void
probe(const char *name, sm_t *m, const oracle_t *o, uint64_t x)
{
	const size_t le = count_le(o, x);
	const int member = le > 0 && o->v[le - 1] == x;

	if (x == SM_IDX_MAX)
		return;
	if (!member) {
		check_unset_x(name, m, o, x);
		check_select0(name, m, o, x - count_le(o, x));
	}
}

static void
probe_around(const char *name, sm_t *m, const oracle_t *o, uint64_t c,
    unsigned radius)
{
	const uint64_t lo = c > radius ? c - radius : 0;
	const uint64_t hi = c > SM_IDX_MAX - 1 - radius ? SM_IDX_MAX - 1 :
	                                                  c + radius;
	uint64_t x;

	for (x = lo;; x++) {
		probe(name, m, o, x);
		if (x == hi)
			break;
	}
}

/* A bound for random probes: a little past the top member, without
 * wrapping when the top member is near 2^64. */
static uint64_t
probe_span(uint64_t top)
{
	return (top > SM_IDX_MAX - 200000 ? SM_IDX_MAX - 1 : top + 100000);
}

static void
run_checks(const char *name, sm_t *m, const oracle_t *o)
{
	const uint64_t top = o->n ? o->v[o->n - 1] : 0;
	const uint64_t span = probe_span(top);
	const uint64_t last_k = SM_IDX_MAX - 1 - (uint64_t)o->n;
	uint64_t k;
	size_t i;

	/* 1. select0 over a dense prefix (all of it for a map below 64k),
	 * at random k, and at the last defined k and the first undefined. */
	for (k = 0; k <= (top < 65536 ? top : 65536) + 4096; k++)
		check_select0(name, m, o, k);
	for (i = 0; i < 2000; i++) {
		check_select0(name, m, o, rnd() % span);
		check_select0(name, m, o, rnd());
	}
	check_select0(name, m, o, last_k);
	check_eq(name, "select0 last k", last_k, sm_select(m, last_k, false),
	    oracle_select0(o, last_k));
	check_eq(name, "select0 past last k", last_k + 1,
	    sm_select(m, last_k + 1, false), SM_IDX_MAX);
	check_eq(name, "select0", SM_IDX_MAX, sm_select(m, SM_IDX_MAX, false),
	    SM_IDX_MAX);

	/* 1, 2. Around members and the chunk boundaries next to them: all of
	 * them on a small map, an evenly spaced 300 on a large one (each
	 * probe walks the chunks). */
	for (i = 0; i < o->n; i += (o->n <= 300 ? 1 : o->n / 300)) {
		const uint64_t c = o->v[i] - o->v[i] % CHUNK;
		probe_around(name, m, o, o->v[i], 66);
		probe_around(name, m, o, c, 3);
		probe_around(name, m, o, c + CHUNK - 1, 3);
		if (c <= SM_IDX_MAX - 2 * CHUNK)
			probe_around(name, m, o, c + CHUNK, 3);
	}
	for (i = 0; i < 2000; i++)
		probe(name, m, o, rnd() % span);

	/* 3. rank over random and boundary ranges. */
	for (i = 0; i < 2000; i++) {
		uint64_t x = rnd() % span, y = rnd() % span;
		if (x > y) {
			const uint64_t t = x;
			x = y;
			y = t;
		}
		check_rank(name, m, o, x, y);
	}
	check_rank(name, m, o, 0, top);
	/* An unset count over 2^64 - 1 positions only fits a 64-bit size_t. */
	if (sizeof(size_t) >= sizeof(uint64_t))
		check_rank(name, m, o, 0, SM_IDX_MAX - 1);

	/* 4. select1. */
	for (i = 0; i < o->n; i++) {
		if (o->n > 5000 && i % 7 != 0)
			continue;
		check_eq(name, "select1", i, sm_select(m, i, true), o->v[i]);
	}
	check_eq(name, "select1 card", o->n, sm_select(m, o->n, true),
	    SM_IDX_MAX);
	check_eq(name, "select1", SM_IDX_MAX, sm_select(m, SM_IDX_MAX, true),
	    SM_IDX_MAX);
}

/* Build helpers: members accumulate in a growable array, then sorted. */
typedef struct {
	sm_t *m;
	uint64_t *v;
	size_t n, cap;
} build_t;

static void
b_push(build_t *b, uint64_t x)
{
	if (b->n == b->cap) {
		b->cap = b->cap ? b->cap * 2 : 1024;
		b->v = realloc(b->v, b->cap * sizeof(uint64_t));
		if (b->v == NULL) {
			perror("realloc");
			exit(2);
		}
	}
	b->v[b->n++] = x;
}

static void
b_add(build_t *b, uint64_t x)
{
	if (sm_add_grow(&b->m, x) == SM_IDX_MAX) {
		fprintf(stderr, "sm_add_grow(%llu) failed\n",
		    (unsigned long long)x);
		exit(2);
	}
	b_push(b, x);
}

/* Add [lo, hi) with sm_add_range, growing on ENOSPC. */
static void
b_range(build_t *b, uint64_t lo, uint64_t hi)
{
	uint64_t x;

	while (!sm_add_range(b->m, lo, hi)) {
		b->m = sm_set_data_size(b->m, NULL,
		    sm_get_capacity(b->m) * 2 + 4096);
		if (b->m == NULL) {
			perror("sm_set_data_size");
			exit(2);
		}
	}
	for (x = lo; x < hi; x++)
		b_push(b, x);
}

static void
b_add_all(build_t *b, const uint64_t *xs, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		b_add(b, xs[i]);
}

static void
b_begin(build_t *b)
{
	memset(b, 0, sizeof(*b));
	b->m = sm_create(1024);
}

/* Sort and dedupe the oracle, then check it against the map. */
static void
b_finish(build_t *b, const char *name)
{
	oracle_t o;
	size_t i, w = 0;

	qsort(b->v, b->n, sizeof(uint64_t), cmp_u64);
	for (i = 0; i < b->n; i++)
		if (w == 0 || b->v[w - 1] != b->v[i])
			b->v[w++] = b->v[i];
	o.v = b->v;
	o.n = w;
	if (getenv("SM_TEST_VERBOSE") != NULL)
		fprintf(stderr, "map %s: %zu members\n", name, o.n);
	if (sm_cardinality(b->m) != o.n) {
		failures++;
		fprintf(stderr, "FAIL %s: cardinality %zu, oracle %zu\n", name,
		    sm_cardinality(b->m), o.n);
	}
	run_checks(name, b->m, &o);
	sm_free(b->m);
	free(b->v);
}

/* A chunk with NONE slots: slot 0 MIXED (bits 0 and 2), slots 1-19 ZEROS,
 * slots 20-30 NONE (capacity 1344), slot 31 ZEROS; then a member at 4096.
 * The NONE positions are unset. */
static void
none_slot_map(void)
{
	uint64_t w[7];
	uint64_t desc = 0;
	unsigned s;
	sm_t *m;
	oracle_t o;
	uint64_t members[] = { 0, 2, 4096 };

	desc |= UINT64_C(2); /* slot 0 MIXED */
	for (s = 20; s <= 30; s++)
		desc |= (uint64_t)1 << (2 * s);
	w[0] = 2; /* chunk count */
	w[1] = 0; /* chunk 0 start */
	w[2] = desc;
	w[3] = UINT64_C(5); /* payload: bits 0 and 2 */
	w[4] = 4096; /* chunk 1 start */
	w[5] = UINT64_C(2); /* slot 0 MIXED */
	w[6] = UINT64_C(1); /* payload: bit 0 */
	m = sm_open_copy((const uint8_t *)w, sizeof(w), 0);
	if (m == NULL || !sm_validate(m) || sm_cardinality(m) != 3) {
		failures++;
		fprintf(stderr, "FAIL none_slot: crafted map not accepted\n");
		sm_free(m);
		return;
	}
	o.v = members;
	o.n = 3;
	run_checks("none_slot", m, &o);
	sm_free(m);
}

/* Members copied from a map built by sm_offset (odd RLE capacities). */
static void
offset_map(int64_t shift)
{
	build_t b;
	sm_t *src = sm_create(1024);
	uint64_t x;
	char name[64];

	while (!sm_add_range(src, 1000, 9000))
		src = sm_set_data_size(src, NULL, sm_get_capacity(src) * 2);
	sm_add_grow(&src, 20000);
	b_begin(&b);
	sm_free(b.m);
	b.m = sm_offset(src, (ssize_t)shift);
	for (x = sm_next_member(b.m, SM_IDX_MAX, NULL); x != SM_IDX_MAX;
	     x = sm_next_member(b.m, x, NULL))
		b_push(&b, x);
	snprintf(name, sizeof(name), "offset(%lld)", (long long)shift);
	b_finish(&b, name);
	sm_free(src);
}

int
main(void)
{
	static const uint64_t repro[] = { 0, 1, 2, 10000, 10001, 50000 };
	static const uint64_t small_pair[] = { 5, 700 };
	static const uint64_t small_edges[] = { 63, 64, 127, 128, 1023 };
	static const uint64_t top_members[] = { 7, UINT64_C(1) << 63,
		SM_IDX_MAX - CHUNK, SM_IDX_MAX - 2, SM_IDX_MAX - 1 };
	build_t b;
	uint64_t x;
	int r;

	/* Empty maps. */
	{
		oracle_t o = { NULL, 0 };
		sm_t *m = sm_create(64);
		run_checks("empty", m, &o);
		sm_free(m);
		check_eq("NULL", "select0", 12345, sm_select(NULL, 12345, false),
		    12345);
	}

	/* The two reproducers from the 5.8.5 review. */
	b_begin(&b);
	b_add_all(&b, repro, sizeof(repro) / sizeof(repro[0]));
	check_eq("repro", "select0", 2045, sm_select(b.m, 2045, false), 2048);
	b_finish(&b, "repro {0,1,2,10000,10001,50000}");
	b_begin(&b);
	b_add(&b, 8192);
	check_eq("repro", "select0", 8192, sm_select(b.m, 8192, false), 8193);
	b_finish(&b, "repro {8192}");

	/* Small-set maps. */
	b_begin(&b);
	b_add_all(&b, small_pair, 2);
	b_finish(&b, "small {5,700}");
	b_begin(&b);
	b_add_all(&b, small_edges, sizeof(small_edges) / sizeof(small_edges[0]));
	b_finish(&b, "small word boundaries");
	b_begin(&b);
	for (x = 0; x < 1024; x++)
		b_add(&b, x);
	b_finish(&b, "small full 0..1023");
	for (r = 0; r < 20; r++) {
		b_begin(&b);
		for (x = 0; x < 1024; x++)
			if (rnd() % 7 == 0)
				b_add(&b, x);
		b_finish(&b, "small random");
	}

	/* Chunk mode: chunk boundaries, k * 2048 and k * 2048 + 2047. */
	b_begin(&b);
	for (x = 0; x < 12; x++) {
		b_add(&b, x * CHUNK);
		b_add(&b, x * CHUNK + 2047);
	}
	b_finish(&b, "boundaries");

	/* Sparse random members with gaps of many chunks. */
	for (r = 0; r < 4; r++) {
		b_begin(&b);
		for (x = 0; x < 3000; x++)
			b_add(&b, rnd() % 2000000);
		b_finish(&b, "sparse random");
	}

	/* RLE runs, one crossing chunk boundaries, with gaps between. */
	b_begin(&b);
	b_range(&b, 0, 10000);
	b_finish(&b, "rle [0,10000)");
	b_begin(&b);
	b_range(&b, 4096, 4096 + 3 * CHUNK + 17);
	b_range(&b, 40000, 47000);
	b_add(&b, 47001);
	b_range(&b, 100000, 100000 + 2 * CHUNK);
	b_add(&b, 500000);
	b_finish(&b, "rle runs with gaps");

	/* Clustered: short runs and singles mixed. */
	b_begin(&b);
	for (r = 0; r < 60; r++) {
		const uint64_t base = rnd() % 400000;
		if (r % 3 == 0)
			b_range(&b, base, base + rnd() % 5000);
		else
			for (x = base; x < base + 300; x += 1 + rnd() % 9)
				b_add(&b, x);
	}
	b_finish(&b, "clustered");

	/* Top of the universe: the last chunk ends at 2^64. */
	b_begin(&b);
	b_add_all(&b, top_members, sizeof(top_members) / sizeof(top_members[0]));
	b_finish(&b, "top of universe");

	none_slot_map();
	offset_map(100);
	offset_map(-777);
	offset_map(3000);

	if (failures != 0) {
		fprintf(stderr, "test_select0: %ld of %ld checks failed\n",
		    failures, checks);
		return (1);
	}
	printf("test_select0: ok (%ld checks)\n", checks);
	return (0);
}
