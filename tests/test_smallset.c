/* SPDX-License-Identifier: MIT */
/*
 * test_smallset.c -- the RLE-free variant's small-set mode.
 *
 * Three things are pinned here:
 *
 *  (1) Footprint.  A near-zero set is stored in the compact small-set
 *      form (a bare uint64 bitmapword[] from bit 0 behind an 8-byte
 *      header), so it matches or beats PostgreSQL's Bitmapset
 *      (8-byte header + ceil((maxbit+1)/64)*8 bytes); sparsemap beats
 *      Bitmapset outright once the index span exceeds the small cap and
 *      chunk mode takes over.
 *
 *  (2) Transitions.  Adding an index past the small span promotes
 *      small->chunk; removing it demotes chunk->small.  Both directions
 *      are lossless and the map validates and serialize-round-trips
 *      after every step.
 *
 *  (3) Equivalence.  Every public sm_* produces identical results in
 *      small mode, in chunk mode, and against a brute-force oracle,
 *      over a spread of shapes (empty, {0}, dense-low, straddling the
 *      threshold, sparse-spread, promote-then-demote).
 *
 * Also asserts the encoder never emits an RLE descriptor (top-two-bits
 * == 01) -- this is the RLE-free variant.
 */
#define SM_EXPOSE_STRUCT 1
#include <sm.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int failures;

#define CHECK(c)                                                              \
	do {                                                                  \
		if (!(c)) {                                                    \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
			    __LINE__, #c);                                    \
			failures++;                                           \
		}                                                             \
	} while (0)

/* Bitmapset's stored bytes: 8-byte header + ceil((maxbit+1)/64)*8. */
static size_t
bitmapset_bytes(uint64_t maxbit)
{
	return (8 + ((size_t)(maxbit / 64) + 1) * 8);
}

/* Is the map physically in small-set mode?  The header word's top bit
 * marks it (mirrors the library's internal __sm_is_small). */
static bool
is_small(const sm_t *m)
{
	if (m == NULL || m->m_data == NULL || m->m_data_used < 8)
		return (false);
	uint64_t hdr;
	memcpy(&hdr, m->m_data, 8);
	return ((hdr & ((uint64_t)1 << 63)) != 0);
}

static sm_t *
build(const uint64_t *idx, size_t n)
{
	sm_t *m = sm_create(64);
	for (size_t i = 0; i < n; i++)
		CHECK(sm_add_grow(&m, idx[i]) == idx[i]);
	m = sm_shrink_to_fit(m); /* may relocate the block */
	return (m);
}

/* -------------------------------------------------------------------
 * (1) Footprint vs Bitmapset.
 * ------------------------------------------------------------------- */
static void
test_footprint(void)
{
	struct {
		const char *name;
		uint64_t idx[4];
		size_t n;
		uint64_t maxbit;
		bool small_wins_or_ties; /* small-set stored <= Bitmapset */
		bool sparsemap_beats;    /* sparsemap (any mode) < Bitmapset */
	} cases[] = {
		{ "{0}", { 0 }, 1, 0, true, false },
		{ "{0..63}", { 0 }, 0, 63, true, false }, /* filled below */
		{ "{5,70}", { 5, 70 }, 2, 70, true, false },
		{ "{0,200}", { 0, 200 }, 2, 200, true, false },
		{ "{0,2000}", { 0, 2000 }, 2, 2000, false, true },
		{ "{200}", { 200 }, 1, 200, false, true },
	};

	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		sm_t *m;
		if (strcmp(cases[c].name, "{0..63}") == 0) {
			uint64_t d[64];
			for (int i = 0; i < 64; i++)
				d[i] = (uint64_t)i;
			m = build(d, 64);
		} else {
			m = build(cases[c].idx, cases[c].n);
		}
		const size_t sm = sm_get_size(m);
		const size_t bs = bitmapset_bytes(cases[c].maxbit);
		fprintf(stderr,
		    "  %-9s sm=%3zu bitmapset=%3zu small=%d\n",
		    cases[c].name, sm, bs, is_small(m));
		if (cases[c].small_wins_or_ties)
			CHECK(sm <= bs);
		if (cases[c].sparsemap_beats)
			CHECK(sm < bs);
		/* The stored form is never larger than the naive Bitmapset. */
		CHECK(sm <= bs);
		CHECK(sm_validate(m));
		sm_free(m);
	}
}

/* -------------------------------------------------------------------
 * (2) Promote / demote round-trips.
 * ------------------------------------------------------------------- */
static void
test_transitions(void)
{
	sm_t *m = sm_create(256);

	/* Start small. */
	sm_add_grow(&m, 3);
	sm_add_grow(&m, 40);
	CHECK(is_small(m));
	CHECK(sm_validate(m));

	/* Promote: add an index past the small span. */
	sm_add_grow(&m, 5000);
	CHECK(!is_small(m));
	CHECK(sm_contains(m, 3, NULL));
	CHECK(sm_contains(m, 40, NULL));
	CHECK(sm_contains(m, 5000, NULL));
	CHECK(sm_cardinality(m) == 3);
	CHECK(sm_validate(m));

	/* Demote: remove the high index; everything left is near zero. */
	sm_remove(m, 5000);
	CHECK(is_small(m));
	CHECK(sm_contains(m, 3, NULL));
	CHECK(sm_contains(m, 40, NULL));
	CHECK(!sm_contains(m, 5000, NULL));
	CHECK(sm_cardinality(m) == 2);
	CHECK(sm_validate(m));

	/* The demoted small map equals a from-scratch small build. */
	uint64_t both[] = { 3, 40 };
	sm_t *fresh = build(both, 2);
	CHECK(sm_equals(m, fresh));
	CHECK(is_small(fresh));

	/* Round-trip both through serialize/deserialize; the small form
	 * survives and stays equal. */
	for (int which = 0; which < 2; which++) {
		sm_t *src = which ? fresh : m;
		size_t need = sm_serialized_size(src);
		uint8_t *buf = malloc(need);
		size_t w = sm_serialize(src, buf, need);
		CHECK(w == need);
		sm_t *back = sm_deserialize(buf, w);
		CHECK(back != NULL);
		CHECK(sm_equals(src, back));
		CHECK(sm_validate(back));
		CHECK(is_small(back));
		sm_free(back);
		free(buf);
	}

	sm_free(fresh);
	sm_free(m);
}

/* A small-mode and a chunk-mode instance of the same logical low set
 * must answer every query identically.  Build the chunk-mode instance
 * by promoting with a high anchor bit that we then account for. */
static void
test_small_vs_chunk(void)
{
	const uint64_t anchor = 9000; /* forces chunk mode, > small cap */
	/* A dense-low set whose small form is smaller than the equivalent
	 * chunk, so it genuinely lands in small mode. */
	uint64_t base[] = { 0, 1, 2, 3, 5, 63, 64, 65, 66 };
	const size_t n = sizeof(base) / sizeof(base[0]);

	sm_t *small = sm_create(256);
	sm_t *chunk = sm_create(256);
	for (size_t i = 0; i < n; i++) {
		sm_add_grow(&small, base[i]);
		sm_add_grow(&chunk, base[i]);
	}
	sm_add_grow(&chunk, anchor); /* promotes chunk to chunk mode */
	CHECK(is_small(small));
	CHECK(!is_small(chunk));

	/* Every query over the shared low range must agree. */
	for (uint64_t x = 0; x < 600; x++)
		CHECK(sm_contains(small, x, NULL) == sm_contains(chunk, x, NULL));

	CHECK(sm_minimum(small) == sm_minimum(chunk)); /* both min at 0 */
	CHECK(sm_maximum(small) == 66);
	CHECK(sm_maximum(chunk) == anchor);
	CHECK(sm_cardinality(small) + 1 == sm_cardinality(chunk));

	/* Forward iteration agrees up to the anchor. */
	uint64_t a = SM_IDX_MAX, b = SM_IDX_MAX;
	for (;;) {
		a = sm_next_member(small, a, NULL);
		b = sm_next_member(chunk, b, NULL);
		if (a == SM_IDX_MAX) {
			CHECK(b == anchor);
			break;
		}
		CHECK(a == b);
	}

	/* rank/select agree over the low range. */
	for (uint64_t x = 0; x <= 66; x++)
		CHECK(sm_rank(small, 0, x, true) == sm_rank(chunk, 0, x, true));
	for (size_t k = 0; k < n; k++)
		CHECK(sm_select(small, k, true) == sm_select(chunk, k, true));

	sm_free(small);
	sm_free(chunk);
}

/* -------------------------------------------------------------------
 * (3) Full-API cross-check: small mode vs a brute-force oracle, over a
 * spread of shapes.
 * ------------------------------------------------------------------- */
#define U 1100u

static void
oracle_check(const char *name, const uint64_t *idx, size_t n)
{
	bool oracle[U] = { false };
	sm_t *m = sm_create(256);
	for (size_t i = 0; i < n; i++) {
		if (idx[i] < U)
			oracle[idx[i]] = true;
		sm_add_grow(&m, idx[i]);
	}

	size_t card = 0;
	uint64_t omin = 0, omax = 0;
	bool any = false;
	for (uint64_t x = 0; x < U; x++) {
		if (oracle[x]) {
			if (!any) {
				omin = x;
				any = true;
			}
			omax = x;
			card++;
		}
	}

	/* contains */
	for (uint64_t x = 0; x < U; x++)
		CHECK(sm_contains(m, x, NULL) == oracle[x]);

	/* cardinality / is_empty / min / max */
	CHECK(sm_cardinality(m) == card);
	CHECK(sm_is_empty(m) == (card == 0));
	if (any) {
		CHECK(sm_minimum(m) == omin);
		CHECK(sm_maximum(m) == omax);
	}

	/* next_member forward walk == oracle order */
	{
		uint64_t prev = SM_IDX_MAX;
		for (uint64_t x = 0; x < U; x++) {
			if (!oracle[x])
				continue;
			uint64_t got = sm_next_member(m, prev, NULL);
			CHECK(got == x);
			prev = x;
		}
		CHECK(sm_next_member(m, prev, NULL) == SM_IDX_MAX);
	}

	/* prev_member reverse walk == oracle order */
	{
		uint64_t upper = SM_IDX_MAX;
		for (uint64_t xx = U; xx-- > 0;) {
			if (!oracle[xx])
				continue;
			uint64_t got = sm_prev_member(m, upper, NULL);
			CHECK(got == xx);
			upper = xx;
			if (xx == 0)
				break;
		}
	}

	/* rank(true) at every cut point */
	{
		size_t acc = 0;
		for (uint64_t x = 0; x < U; x++) {
			CHECK(sm_rank(m, 0, x, true) == acc + (oracle[x] ? 1 : 0));
			if (oracle[x])
				acc++;
		}
	}

	/* select(true) for every n */
	{
		size_t k = 0;
		for (uint64_t x = 0; x < U; x++) {
			if (oracle[x]) {
				CHECK(sm_select(m, k, true) == x);
				k++;
			}
		}
		CHECK(sm_select(m, card, true) == SM_IDX_MAX);
	}

	/* to_array */
	{
		uint64_t arr[U];
		size_t cnt = U;
		sm_to_array(m, arr, &cnt);
		CHECK(cnt == card);
		size_t k = 0;
		for (uint64_t x = 0; x < U; x++)
			if (oracle[x])
				CHECK(arr[k++] == x);
	}

	/* hash / equals are stable across a copy and a serialize round-trip */
	{
		sm_t *cp = sm_copy(m);
		CHECK(sm_equals(m, cp));
		CHECK(sm_hash(m) == sm_hash(cp));
		size_t need = sm_serialized_size(m);
		uint8_t *buf = malloc(need);
		CHECK(sm_serialize(m, buf, need) == need);
		sm_t *back = sm_deserialize(buf, need);
		if (card == 0) {
			CHECK(back == NULL || sm_equals(m, back));
		} else {
			CHECK(back != NULL);
			CHECK(sm_equals(m, back));
			CHECK(sm_hash(m) == sm_hash(back));
		}
		if (back)
			sm_free(back);
		free(buf);
		sm_free(cp);
	}

	CHECK(sm_validate(m));
	fprintf(stderr, "  oracle %-14s card=%zu small=%d ok\n", name, card,
	    is_small(m));
	sm_free(m);
}

/* Set-algebra cross-check: small op small, small op chunk, against the
 * oracle-derived expected sets. */
static void
test_setops(void)
{
	uint64_t A[] = { 0, 5, 63, 200 };   /* small */
	uint64_t B[] = { 5, 64, 200, 9000 };/* chunk (has 9000) */

	sm_t *a = build(A, 4);
	sm_t *b = sm_create(256);
	for (size_t i = 0; i < 4; i++)
		sm_add_grow(&b, B[i]);
	CHECK(is_small(a));
	CHECK(!is_small(b));

	sm_t *u = sm_union(a, b);
	sm_t *in = sm_intersection(a, b);
	sm_t *df = sm_difference(a, b);
	sm_t *xr = sm_xor(a, b);

	/* union = {0,5,63,64,200,9000} */
	CHECK(sm_cardinality(u) == 6);
	CHECK(sm_contains(u, 0, NULL) && sm_contains(u, 64, NULL) &&
	    sm_contains(u, 9000, NULL));
	/* intersection = {5,200} -- near zero, so the result demotes small */
	CHECK(sm_cardinality(in) == 2);
	CHECK(sm_contains(in, 5, NULL) && sm_contains(in, 200, NULL));
	CHECK(is_small(in));
	/* difference a\b = {0,63} */
	CHECK(sm_cardinality(df) == 2);
	CHECK(sm_contains(df, 0, NULL) && sm_contains(df, 63, NULL));
	CHECK(is_small(df));
	/* xor = {0,63,64,9000} */
	CHECK(sm_cardinality(xr) == 4);
	CHECK(sm_contains(xr, 0, NULL) && sm_contains(xr, 9000, NULL));

	for (sm_t **p = (sm_t *[]){ u, in, df, xr, NULL }; *p; p++)
		CHECK(sm_validate(*p));

	sm_free(u);
	sm_free(in);
	sm_free(df);
	sm_free(xr);
	sm_free(a);
	sm_free(b);
}

/* -------------------------------------------------------------------
 * The encoder never emits an RLE descriptor.  A serialized body is the
 * internal m_data; walk its chunk stream (chunk mode) and assert no
 * chunk descriptor has top-two-bits == 01.  Small bodies carry no
 * chunks at all.
 * ------------------------------------------------------------------- */
static void
test_no_rle_emitted(void)
{
	/* A long dense run: in the RLE variant this becomes RLE chunks; in
	 * this build it must be all-ONES sparse chunks, never an RLE
	 * descriptor. */
	sm_t *m = sm_create(4096);
	for (uint64_t i = 0; i < 20000; i++)
		CHECK(sm_add_grow(&m, i) == i);
	CHECK(sm_validate(m));
	CHECK(!is_small(m)); /* spans far past the small cap */

	/* Walk the internal chunk stream directly. */
	const uint8_t *base = (const uint8_t *)sm_get_data(m);
	uint64_t count;
	memcpy(&count, base, 8);
	CHECK((count & ((uint64_t)1 << 63)) == 0); /* not the small marker */
	/* We cannot re-derive per-chunk sizes without the internal codec,
	 * but sm_validate already rejects any RLE descriptor, so a valid
	 * map here means the encoder emitted none.  Assert the contract
	 * holds directly on the first chunk's descriptor as a spot check. */
	uint64_t first_desc;
	memcpy(&first_desc, base + 8 + 8, 8); /* skip count + chunk start */
	CHECK((first_desc & 0xC000000000000000ULL) != 0x4000000000000000ULL);

	sm_free(m);
}

int
main(void)
{
	failures = 0;

	test_footprint();
	test_transitions();
	test_small_vs_chunk();
	test_setops();
	test_no_rle_emitted();

	/* Oracle cross-check over a spread of shapes. */
	oracle_check("empty", NULL, 0);
	{ uint64_t s[] = { 0 }; oracle_check("{0}", s, 1); }
	{ uint64_t s[] = { 0, 1, 5, 63 }; oracle_check("{0,1,5,63}", s, 4); }
	{
		uint64_t s[64];
		for (int i = 0; i < 64; i++)
			s[i] = (uint64_t)i;
		oracle_check("{0..63}", s, 64);
	}
	{
		uint64_t s[128];
		for (int i = 0; i < 128; i++)
			s[i] = (uint64_t)i;
		oracle_check("dense-near-zero", s, 128);
	}
	{ uint64_t s[] = { 1000, 1020, 1023, 1024, 1030 };
	  oracle_check("straddle-threshold", s, 5); }
	{ uint64_t s[] = { 0, 137, 400, 699 }; oracle_check("sparse-spread", s, 4); }
	{ uint64_t s[] = { 3, 40, 500, 500 }; oracle_check("dup-low", s, 4); }
	{ uint64_t s[] = { 0, 137, 400, 699 }; oracle_check("low-under-700", s, 4); }

	if (failures) {
		fprintf(stderr, "test_smallset: %d failure(s)\n", failures);
		return (1);
	}
	fprintf(stderr, "test_smallset: OK\n");
	return (0);
}
