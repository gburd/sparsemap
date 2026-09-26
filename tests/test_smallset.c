/* SPDX-License-Identifier: MIT */
/*
 * test_smallset.c -- small-set mode on the RLE build.
 *
 * Pins the three small-set guarantees on the RLE variant:
 *
 *  (1) Footprint.  A near-zero set is stored in the compact small-set
 *      form (a bare uint64 bitmapword[] from bit 0 behind an 8-byte
 *      header), matching or beating PostgreSQL's Bitmapset (8-byte
 *      header + ceil((maxbit+1)/64)*8 bytes).  A dense low run promotes
 *      to a single descriptor-only RLE chunk (24 bytes) via the
 *      RLE-aware promote, beating both Bitmapset and the flat small
 *      form.
 *
 *  (2) Transitions.  Adding an index past the small span promotes
 *      small->chunk; removing it demotes chunk->small.  Both directions
 *      are lossless; the map validates and serialize-round-trips after
 *      every step.
 *
 *  (3) Equivalence.  Every public sm_* produces identical results in
 *      small mode, in chunk mode, and against a brute-force oracle,
 *      over a spread of shapes.
 *
 * Also confirms the RLE-aware promote: a dense low run is stored as a
 * 24-byte RLE chunk, and small<->RLE-chunk set-algebra round-trips.
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
static int
is_small(const sm_t *m)
{
	if (m == NULL || m->m_data == NULL || m->m_data_used < 8)
		return (0);
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
	m = sm_shrink_to_fit(m);
	return (m);
}

/* (1) Footprint vs Bitmapset (RLE build). */
static void
test_footprint(void)
{
	struct {
		const char *name;
		uint64_t idx[4];
		size_t n;
		uint64_t maxbit;
		int small_wins_or_ties;
		int sparsemap_beats;
	} cases[] = {
		{ "{0}", { 0 }, 1, 0, 1, 0 },
		{ "{0..63}", { 0 }, 0, 63, 1, 0 },
		{ "{5,70}", { 5, 70 }, 2, 70, 1, 0 },
		{ "{0,200}", { 0, 200 }, 2, 200, 1, 0 },
		{ "{0,2000}", { 0, 2000 }, 2, 2000, 0, 1 },
		{ "{200}", { 200 }, 1, 200, 0, 1 },
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
		fprintf(stderr, "  %-9s sm=%3zu bitmapset=%3zu small=%d\n",
		    cases[c].name, sm, bs, is_small(m));
		if (cases[c].small_wins_or_ties)
			CHECK(sm <= bs);
		if (cases[c].sparsemap_beats)
			CHECK(sm < bs);
		CHECK(sm <= bs);
		CHECK(sm_validate(m));
		sm_free(m);
	}

	/* Dense low run {0..1023}: RLE-aware promote makes it a single
	 * 24-byte RLE chunk -- beats Bitmapset (136) and flat small (136). */
	{
		sm_t *m = sm_create(4096);
		for (uint64_t i = 0; i < 1024; i++)
			sm_add_grow(&m, i);
		m = sm_shrink_to_fit(m);
		const size_t sm = sm_get_size(m);
		fprintf(stderr, "  {0..1023} sm=%3zu bitmapset=136 flat=136\n",
		    sm);
		CHECK(sm <= 24);
		CHECK(sm < 136);
		CHECK(!is_small(m));
		CHECK(sm_cardinality(m) == 1024);
		CHECK(sm_validate(m));
		sm_free(m);
	}

	/* Dense partial-tail run {0..1000}: sparse would be 32 bytes, the
	 * RLE-aware promote makes it 24. */
	{
		sm_t *m = sm_create(4096);
		for (uint64_t i = 0; i <= 1000; i++)
			sm_add_grow(&m, i);
		m = sm_shrink_to_fit(m);
		fprintf(stderr, "  {0..1000} sm=%3zu (sparse would be 32)\n",
		    sm_get_size(m));
		CHECK(sm_get_size(m) <= 24);
		CHECK(sm_cardinality(m) == 1001);
		CHECK(sm_validate(m));
		sm_free(m);
	}
}

/* (2) Promote / demote round-trips. */
static void
test_transitions(void)
{
	sm_t *m = sm_create(256);
	sm_add_grow(&m, 3);
	sm_add_grow(&m, 40);
	CHECK(is_small(m));
	CHECK(sm_validate(m));

	sm_add_grow(&m, 5000);
	CHECK(!is_small(m));
	CHECK(sm_contains(m, 3, NULL));
	CHECK(sm_contains(m, 40, NULL));
	CHECK(sm_contains(m, 5000, NULL));
	CHECK(sm_cardinality(m) == 3);
	CHECK(sm_validate(m));

	sm_remove(m, 5000);
	CHECK(is_small(m));
	CHECK(sm_contains(m, 3, NULL));
	CHECK(sm_contains(m, 40, NULL));
	CHECK(!sm_contains(m, 5000, NULL));
	CHECK(sm_cardinality(m) == 2);
	CHECK(sm_validate(m));

	uint64_t both[] = { 3, 40 };
	sm_t *fresh = build(both, 2);
	CHECK(sm_equals(m, fresh));
	CHECK(is_small(fresh));

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

	/* Demote from an RLE chunk: build a dense low run (promotes to an
	 * RLE chunk), shave off the tail so the run shrinks under the cap,
	 * and confirm it stays a correct (chunk or small) map. */
	{
		sm_t *r = sm_create(4096);
		for (uint64_t i = 0; i <= 900; i++)
			sm_add_grow(&r, i);
		CHECK(!is_small(r)); /* an RLE chunk */
		CHECK(sm_cardinality(r) == 901);
		for (uint64_t i = 900; i >= 200; i--)
			sm_remove(r, i);
		/* Now {0..199}: still a run, either small or a small RLE chunk;
		 * every query must be exact. */
		CHECK(sm_cardinality(r) == 200);
		for (uint64_t x = 0; x < 400; x++)
			CHECK(sm_contains(r, x, NULL) == (x < 200));
		CHECK(sm_minimum(r) == 0);
		CHECK(sm_maximum(r) == 199);
		CHECK(sm_validate(r));
		sm_free(r);
	}
}

/* A small-mode and a chunk-mode instance of the same logical low set
 * must answer every query identically. */
static void
test_small_vs_chunk(void)
{
	const uint64_t anchor = 9000;
	uint64_t base[] = { 0, 1, 2, 3, 5, 63, 64, 65, 66 };
	const size_t n = sizeof(base) / sizeof(base[0]);

	sm_t *small = sm_create(256);
	sm_t *chunk = sm_create(256);
	for (size_t i = 0; i < n; i++) {
		sm_add_grow(&small, base[i]);
		sm_add_grow(&chunk, base[i]);
	}
	sm_add_grow(&chunk, anchor);
	CHECK(is_small(small));
	CHECK(!is_small(chunk));

	for (uint64_t x = 0; x < 600; x++)
		CHECK(sm_contains(small, x, NULL) == sm_contains(chunk, x, NULL));

	CHECK(sm_minimum(small) == sm_minimum(chunk));
	CHECK(sm_maximum(small) == 66);
	CHECK(sm_maximum(chunk) == anchor);
	CHECK(sm_cardinality(small) + 1 == sm_cardinality(chunk));

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
	for (uint64_t x = 0; x <= 66; x++)
		CHECK(sm_rank(small, 0, x, true) == sm_rank(chunk, 0, x, true));
	for (size_t k = 0; k < n; k++)
		CHECK(sm_select(small, k, true) == sm_select(chunk, k, true));

	sm_free(small);
	sm_free(chunk);
}

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
			if (!any) { omin = x; any = true; }
			omax = x;
			card++;
		}
	}
	for (uint64_t x = 0; x < U; x++)
		CHECK(sm_contains(m, x, NULL) == oracle[x]);
	CHECK(sm_cardinality(m) == card);
	CHECK(sm_is_empty(m) == (card == 0));
	if (any) {
		CHECK(sm_minimum(m) == omin);
		CHECK(sm_maximum(m) == omax);
	}
	{
		uint64_t prev = SM_IDX_MAX;
		for (uint64_t x = 0; x < U; x++) {
			if (!oracle[x]) continue;
			CHECK(sm_next_member(m, prev, NULL) == x);
			prev = x;
		}
		CHECK(sm_next_member(m, prev, NULL) == SM_IDX_MAX);
	}
	{
		uint64_t upper = SM_IDX_MAX;
		for (uint64_t xx = U; xx-- > 0;) {
			if (!oracle[xx]) continue;
			CHECK(sm_prev_member(m, upper, NULL) == xx);
			upper = xx;
			if (xx == 0) break;
		}
	}
	{
		size_t acc = 0;
		for (uint64_t x = 0; x < U; x++) {
			CHECK(sm_rank(m, 0, x, true) == acc + (oracle[x] ? 1 : 0));
			if (oracle[x]) acc++;
		}
	}
	{
		size_t k = 0;
		for (uint64_t x = 0; x < U; x++) {
			if (oracle[x]) { CHECK(sm_select(m, k, true) == x); k++; }
		}
		CHECK(sm_select(m, card, true) == SM_IDX_MAX);
	}
	{
		uint64_t arr[U];
		size_t cnt = U;
		sm_to_array(m, arr, &cnt);
		CHECK(cnt == card);
		size_t k = 0;
		for (uint64_t x = 0; x < U; x++)
			if (oracle[x]) CHECK(arr[k++] == x);
	}
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
		if (back) sm_free(back);
		free(buf);
		sm_free(cp);
	}
	CHECK(sm_validate(m));
	fprintf(stderr, "  oracle %-16s card=%zu small=%d ok\n", name, card,
	    is_small(m));
	sm_free(m);
}

/* Set-algebra cross-check: small op small, small op chunk, small op
 * RLE-chunk, against oracle-derived expected sets. */
static void
test_setops(void)
{
	uint64_t A[] = { 0, 5, 63, 200 };
	uint64_t B[] = { 5, 64, 200, 9000 };
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
	CHECK(sm_cardinality(u) == 6);
	CHECK(sm_contains(u, 0, NULL) && sm_contains(u, 64, NULL) &&
	    sm_contains(u, 9000, NULL));
	CHECK(sm_cardinality(in) == 2);
	CHECK(sm_contains(in, 5, NULL) && sm_contains(in, 200, NULL));
	CHECK(is_small(in));
	CHECK(sm_cardinality(df) == 2);
	CHECK(sm_contains(df, 0, NULL) && sm_contains(df, 63, NULL));
	CHECK(is_small(df));
	CHECK(sm_cardinality(xr) == 4);
	CHECK(sm_contains(xr, 0, NULL) && sm_contains(xr, 9000, NULL));
	for (sm_t **p = (sm_t *[]){ u, in, df, xr, NULL }; *p; p++)
		CHECK(sm_validate(*p));
	sm_free(u); sm_free(in); sm_free(df); sm_free(xr);
	sm_free(a); sm_free(b);

	/* small (sparse low) x RLE-chunk (dense low run). */
	uint64_t S[] = { 0, 5, 70 };     /* stays small: 24 < sparse 32 */
	sm_t *s = build(S, 3);
	sm_t *rle = sm_create(4096);
	for (uint64_t i = 0; i <= 800; i++)
		sm_add_grow(&rle, i);
	CHECK(is_small(s));
	CHECK(!is_small(rle));            /* an RLE chunk */
	/* intersection = {0,5,70} (all within 0..800) */
	sm_t *i2 = sm_intersection(s, rle);
	CHECK(sm_cardinality(i2) == 3);
	CHECK(sm_contains(i2, 0, NULL) && sm_contains(i2, 5, NULL) &&
	    sm_contains(i2, 70, NULL));
	CHECK(sm_validate(i2));
	/* union = {0..800} = 801 bits (s subset of rle) */
	sm_t *u2 = sm_union(s, rle);
	CHECK(sm_cardinality(u2) == 801);
	CHECK(sm_contains(u2, 800, NULL));
	CHECK(sm_validate(u2));
	/* difference s\rle = {} (s subset of rle) */
	sm_t *d2 = sm_difference(s, rle);
	CHECK(d2 == NULL || sm_cardinality(d2) == 0);
	if (d2) CHECK(sm_validate(d2));
	/* difference rle\s = {0..800} minus {0,5,70} = 798 bits */
	sm_t *d3 = sm_difference(rle, s);
	CHECK(sm_cardinality(d3) == 798);
	CHECK(!sm_contains(d3, 5, NULL) && sm_contains(d3, 6, NULL));
	CHECK(sm_validate(d3));
	sm_free(i2); sm_free(u2); if (d2) sm_free(d2); sm_free(d3);
	sm_free(s); sm_free(rle);
}

/* The RLE-aware promote actually emits a single RLE descriptor for a
 * dense low run.  A serialized small body has the small marker; a
 * promoted dense run's body has one chunk whose descriptor top-two-bits
 * are 01 (SM_RLE_FLAGS). */
static void
test_rle_promote_marker(void)
{
	sm_t *m = sm_create(4096);
	for (uint64_t i = 0; i <= 1000; i++)
		CHECK(sm_add_grow(&m, i) == i);
	CHECK(sm_validate(m));
	CHECK(!is_small(m));
	CHECK(sm_get_size(m) == 24); /* count(8)+start(8)+RLE descriptor(8) */

	const uint8_t *base = (const uint8_t *)sm_get_data(m);
	uint64_t count;
	memcpy(&count, base, 8);
	CHECK(count == 1);
	uint64_t first_desc;
	memcpy(&first_desc, base + 8 + 8, 8); /* skip count + chunk start */
	CHECK((first_desc & 0xC000000000000000ULL) == 0x4000000000000000ULL);
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
	test_rle_promote_marker();

	oracle_check("empty", NULL, 0);
	{ uint64_t s[] = { 0 }; oracle_check("{0}", s, 1); }
	{ uint64_t s[] = { 0, 1, 5, 63 }; oracle_check("{0,1,5,63}", s, 4); }
	{
		uint64_t s[64];
		for (int i = 0; i < 64; i++) s[i] = (uint64_t)i;
		oracle_check("{0..63}", s, 64);
	}
	{
		uint64_t s[128];
		for (int i = 0; i < 128; i++) s[i] = (uint64_t)i;
		oracle_check("dense-near-zero", s, 128);
	}
	{
		uint64_t s[1000];
		for (int i = 0; i < 1000; i++) s[i] = (uint64_t)i;
		oracle_check("dense-run-1000", s, 1000);
	}
	{ uint64_t s[] = { 1000, 1020, 1023, 1024, 1030 };
	  oracle_check("straddle-threshold", s, 5); }
	{ uint64_t s[] = { 0, 137, 400, 699 }; oracle_check("sparse-spread", s, 4); }
	{ uint64_t s[] = { 3, 40, 500, 500 }; oracle_check("dup-low", s, 4); }

	if (failures) {
		fprintf(stderr, "test_smallset: %d failure(s)\n", failures);
		return (1);
	}
	fprintf(stderr, "test_smallset: OK\n");
	return (0);
}
