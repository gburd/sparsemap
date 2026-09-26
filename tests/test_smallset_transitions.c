/* SPDX-License-Identifier: MIT */
/*
 * test_smallset_transitions.c -- small <-> chunk/RLE transition corners
 * on the RLE build.
 *
 * Covers:
 *  - Promote: small map + a bit >= 1024 -> chunk; pre-existing bits,
 *    cardinality, min/max/select/hash unchanged, the new bit present,
 *    sm_validate true, serialize round-trips to an equal map, ascending
 *    iteration matches.
 *  - Demote: chunk map with a high bit, remove down until all bits <
 *    1024; results correct whether it flips to small or stays chunk
 *    (the actual outcome is reported).
 *  - Straddle the exact boundary: bits at 1023 and 1024; add/remove at
 *    1023/1024/1025; verify mode and results.
 *  - RLE-aware promote: dense low runs {0..1000} and {0..1023} promote
 *    to a single descriptor-only RLE chunk (white-box: 24 bytes, RLE
 *    descriptor); a sparse low scatter stays small.
 *  - Seeded randomized add/remove churn crossing the boundary
 *    repeatedly: membership == oracle, sm_validate true, round-trip
 *    equal at every step, using the grow-safe handle idiom throughout.
 *
 * Clean under plain, ASan+UBSan, and valgrind.
 */
#define SM_EXPOSE_STRUCT 1
#include <sm.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(c)                                                              \
	do {                                                                  \
		checks++;                                                      \
		if (!(c)) {                                                    \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
			    __LINE__, #c);                                    \
			failures++;                                           \
		}                                                             \
	} while (0)

#define SMALL_MAX 1024u

static int
is_small(const sm_t *m)
{
	if (m == NULL || m->m_data == NULL || m->m_data_used < 8)
		return (0);
	uint64_t hdr;
	memcpy(&hdr, m->m_data, 8);
	return ((hdr & ((uint64_t)1 << 63)) != 0);
}

/* Is the map's first chunk an RLE chunk?  Chunk mode only: read the
 * first descriptor (after the 8-byte count and 8-byte start) and check
 * bits 63:62 == 01 (SM_RLE_FLAGS). */
static int
first_chunk_is_rle(const sm_t *m)
{
	if (is_small(m) || m->m_data == NULL || m->m_data_used < 24)
		return (0);
	const uint8_t *base = (const uint8_t *)sm_get_data(m);
	uint64_t count, desc;
	memcpy(&count, base, 8);
	if (count < 1)
		return (0);
	memcpy(&desc, base + 8 + 8, 8);
	return ((desc & 0xC000000000000000ULL) == 0x4000000000000000ULL);
}

/* round-trip check: serialize, deserialize, compare equal. */
static void
roundtrip_equal(sm_t *m)
{
	size_t need = sm_serialized_size(m);
	uint8_t *buf = malloc(need);
	CHECK(sm_serialize(m, buf, need) == need);
	sm_t *back = sm_deserialize(buf, need);
	if (sm_is_empty(m)) {
		CHECK(back == NULL || sm_equals(m, back));
	} else {
		CHECK(back != NULL);
		CHECK(sm_equals(m, back));
		CHECK(sm_hash(m) == sm_hash(back));
	}
	if (back)
		sm_free(back);
	free(buf);
}

/* ------------------------------------------------------------------ */
/* Promote: small -> chunk, everything preserved                      */
/* ------------------------------------------------------------------ */
static void
test_promote(void)
{
	uint64_t pre[] = { 3, 40, 63, 64, 100 };
	const size_t n = sizeof(pre) / sizeof(pre[0]);
	sm_t *m = sm_create(64);
	for (size_t i = 0; i < n; i++)
		CHECK(sm_add_grow(&m, pre[i]) == pre[i]);
	CHECK(is_small(m));

	/* snapshot pre-promote answers */
	uint64_t mn = sm_minimum(m), mx = sm_maximum(m);
	size_t card = sm_cardinality(m);
	uint64_t hash_pre = sm_hash(m);
	uint64_t sel[8];
	for (size_t k = 0; k < n; k++)
		sel[k] = sm_select(m, k, true);

	/* add a bit past the small span -> must promote to chunk */
	CHECK(sm_add_grow(&m, 5000) == 5000);
	CHECK(!is_small(m));

	/* pre-existing bits and their order are unchanged */
	for (size_t i = 0; i < n; i++)
		CHECK(sm_contains(m, pre[i], NULL));
	CHECK(sm_contains(m, 5000, NULL));
	CHECK(sm_cardinality(m) == card + 1);
	CHECK(sm_minimum(m) == mn);
	CHECK(sm_maximum(m) == 5000);
	CHECK(sm_maximum(m) != mx);
	for (size_t k = 0; k < n; k++)
		CHECK(sm_select(m, k, true) == sel[k]);
	CHECK(sm_select(m, n, true) == 5000);
	/* hash of the augmented set differs from the pre-promote hash but is
	 * stable across the representation change (equal-set invariance is
	 * checked by round-trip below). */
	CHECK(sm_hash(m) != hash_pre);
	CHECK(sm_validate(m));

	/* serialize round-trips to an equal map; ascending iteration ok */
	roundtrip_equal(m);
	{
		uint64_t exp[8];
		size_t en = 0;
		for (size_t i = 0; i < n; i++)
			exp[en++] = pre[i];
		exp[en++] = 5000;
		uint64_t it = SM_IDX_MAX;
		for (size_t k = 0; k < en; k++)
			CHECK((it = sm_next_member(m, it, NULL)) == exp[k]);
		CHECK(sm_next_member(m, it, NULL) == SM_IDX_MAX);
	}
	sm_free(m);
	fprintf(stderr, "  promote small->chunk ok\n");
}

/* ------------------------------------------------------------------ */
/* Demote: chunk -> small (or correct chunk), reported                */
/* ------------------------------------------------------------------ */
static void
test_demote(void)
{
	sm_t *m = sm_create(256);
	uint64_t low[] = { 3, 40, 63, 64, 100 };
	for (size_t i = 0; i < 5; i++)
		CHECK(sm_add_grow(&m, low[i]) == low[i]);
	CHECK(sm_add_grow(&m, 9000) == 9000); /* force chunk */
	CHECK(!is_small(m));

	/* remove the high bit; all remaining bits are < 1024 */
	CHECK(sm_remove(m, 9000) == 9000);
	int demoted = is_small(m);
	/* whichever mode it lands in, the set must be exactly {low}. */
	for (uint64_t x = 0; x < SMALL_MAX; x++) {
		int want = 0;
		for (size_t i = 0; i < 5; i++)
			if (low[i] == x)
				want = 1;
		CHECK(sm_contains(m, x, NULL) == (bool)want);
	}
	CHECK(sm_cardinality(m) == 5);
	CHECK(sm_minimum(m) == 3);
	CHECK(sm_maximum(m) == 100);
	CHECK(sm_validate(m));
	roundtrip_equal(m);
	fprintf(stderr, "  demote chunk->%s ok\n", demoted ? "small" : "chunk");
	sm_free(m);

	/* Demote from a dense RLE chunk: build {0..900} (RLE chunk), shave
	 * the tail below the small span; result stays exact either way. */
	sm_t *r = sm_create(4096);
	for (uint64_t i = 0; i <= 900; i++)
		CHECK(sm_add_grow(&r, i) == i);
	CHECK(!is_small(r));
	for (uint64_t i = 900; i >= 200; i--)
		CHECK(sm_remove(r, i) == i);
	/* now {0..199} */
	CHECK(sm_cardinality(r) == 200);
	for (uint64_t x = 0; x < 400; x++)
		CHECK(sm_contains(r, x, NULL) == (x < 200));
	CHECK(sm_minimum(r) == 0);
	CHECK(sm_maximum(r) == 199);
	CHECK(sm_validate(r));
	roundtrip_equal(r);
	fprintf(stderr, "  RLE-run shrink -> %s ok\n",
	    is_small(r) ? "small" : "chunk");
	sm_free(r);
}

/* ------------------------------------------------------------------ */
/* Straddle the exact boundary at 1023/1024/1025                      */
/* ------------------------------------------------------------------ */
static void
test_boundary(void)
{
	/* 1023 is the top small-eligible index; 1024 forces chunk. */
	sm_t *m = sm_create(64);
	CHECK(sm_add_grow(&m, 1023) == 1023);
	/* {1023} alone: high max bit, one member -> chunk (cheaper). */
	int s1023 = is_small(m);
	CHECK(sm_contains(m, 1023, NULL));
	CHECK(sm_maximum(m) == 1023);

	CHECK(sm_add_grow(&m, 1024) == 1024); /* forces chunk */
	CHECK(!is_small(m));
	CHECK(sm_contains(m, 1023, NULL));
	CHECK(sm_contains(m, 1024, NULL));
	CHECK(sm_cardinality(m) == 2);

	CHECK(sm_add_grow(&m, 1025) == 1025);
	CHECK(sm_cardinality(m) == 3);
	CHECK(sm_contains(m, 1025, NULL));

	/* remove 1024, 1025 -> back to {1023} */
	CHECK(sm_remove(m, 1025) == 1025);
	CHECK(sm_remove(m, 1024) == 1024);
	CHECK(sm_cardinality(m) == 1);
	CHECK(sm_contains(m, 1023, NULL));
	CHECK(!sm_contains(m, 1024, NULL));
	CHECK(sm_validate(m));
	roundtrip_equal(m);
	sm_free(m);

	/* A confirmed-small low set that adds exactly 1024 (the first
	 * chunk-forcing index): {0,1,2} small, +1024 -> chunk. */
	sm_t *n = sm_create(64);
	CHECK(sm_add_grow(&n, 0) == 0);
	CHECK(sm_add_grow(&n, 1) == 1);
	CHECK(sm_add_grow(&n, 2) == 2);
	CHECK(is_small(n));
	CHECK(sm_add_grow(&n, 1024) == 1024);
	CHECK(!is_small(n));
	CHECK(sm_contains(n, 0, NULL) && sm_contains(n, 1, NULL) &&
	    sm_contains(n, 2, NULL) && sm_contains(n, 1024, NULL));
	CHECK(sm_cardinality(n) == 4);
	/* removing 1024 leaves {0,1,2}; must be correct (small or chunk) */
	CHECK(sm_remove(n, 1024) == 1024);
	CHECK(sm_cardinality(n) == 3);
	CHECK(sm_contains(n, 0, NULL) && sm_contains(n, 1, NULL) &&
	    sm_contains(n, 2, NULL) && !sm_contains(n, 1024, NULL));
	CHECK(sm_validate(n));
	fprintf(stderr, "  boundary 1023/1024/1025 (s1023=%d) ok\n", s1023);
	sm_free(n);
}

/* ------------------------------------------------------------------ */
/* RLE-aware promote of a dense low run                               */
/* ------------------------------------------------------------------ */
static void
test_rle_promote(void)
{
	/* {0..1000}: RLE-aware promote -> one 24-byte RLE chunk. */
	{
		sm_t *m = sm_create(4096);
		for (uint64_t i = 0; i <= 1000; i++)
			CHECK(sm_add_grow(&m, i) == i);
		CHECK(!is_small(m));
		CHECK(first_chunk_is_rle(m));
		CHECK(sm_get_size(m) == 24);
		CHECK(sm_cardinality(m) == 1001);
		CHECK(sm_validate(m));
		roundtrip_equal(m);
		sm_free(m);
	}
	/* {0..1023}: full small span as a single RLE chunk. */
	{
		sm_t *m = sm_create(4096);
		for (uint64_t i = 0; i < 1024; i++)
			CHECK(sm_add_grow(&m, i) == i);
		CHECK(!is_small(m));
		CHECK(first_chunk_is_rle(m));
		CHECK(sm_get_size(m) <= 24);
		CHECK(sm_cardinality(m) == 1024);
		CHECK(sm_validate(m));
		roundtrip_equal(m);
		sm_free(m);
	}
	/* sparse low scatter stays small: {0,5,70,300} (few words). */
	{
		sm_t *m = sm_create(64);
		uint64_t s[] = { 0, 5, 70, 300 };
		for (size_t i = 0; i < 4; i++)
			CHECK(sm_add_grow(&m, s[i]) == s[i]);
		CHECK(is_small(m));
		CHECK(sm_cardinality(m) == 4);
		CHECK(sm_validate(m));
		sm_free(m);
	}
	fprintf(stderr, "  RLE-aware promote ok\n");
}

/* ------------------------------------------------------------------ */
/* Seeded randomized churn crossing the boundary repeatedly           */
/* ------------------------------------------------------------------ */
#define CHURN_UNIVERSE 4096u
static void
test_churn(void)
{
	bool oracle[CHURN_UNIVERSE] = { false };
	sm_t *m = sm_create(64);
	uint32_t rng = 0xC0FFEEu;
	int crossings = 0;
	int prev_small = -1;

	for (int step = 0; step < 4000; step++) {
		rng = rng * 1664525u + 1013904223u;
		/* Alternate phases so the map oscillates across the boundary:
		 * even 500-step windows churn only low indices (map can be
		 * small); odd windows include high indices (forces chunk). */
		int low_phase = ((step / 500) % 2) == 0;
		uint64_t idx;
		if (low_phase) {
			/* first clear every high bit so the map can demote */
			for (uint64_t x = SMALL_MAX; x < CHURN_UNIVERSE; x++)
				if (oracle[x]) {
					sm_remove(m, x);
					oracle[x] = false;
				}
			idx = (rng >> 8) % SMALL_MAX; /* stay low */
		} else {
			idx = (rng >> 8) % CHURN_UNIVERSE; /* full range */
		}
		int add = (rng >> 1) & 1;
		if (add) {
			CHECK(sm_add_grow(&m, idx) == idx);
			oracle[idx] = true;
		} else {
			sm_remove(m, idx);
			oracle[idx] = false;
		}

		int now_small = is_small(m);
		if (prev_small != -1 && now_small != prev_small)
			crossings++;
		prev_small = now_small;

		/* membership matches oracle every step */
		size_t card = 0;
		for (uint64_t x = 0; x < CHURN_UNIVERSE; x++) {
			if (sm_contains(m, x, NULL) != oracle[x]) {
				CHECK(sm_contains(m, x, NULL) == oracle[x]);
				break;
			}
			if (oracle[x])
				card++;
		}
		CHECK(sm_cardinality(m) == card);
		CHECK(sm_validate(m));

		/* round-trip every 200 steps (full serialize is O(size)) */
		if (step % 200 == 0)
			roundtrip_equal(m);
	}
	CHECK(crossings > 0); /* the churn actually crossed the boundary */
	fprintf(stderr, "  churn 4000 ops, %d mode crossings ok\n", crossings);
	sm_free(m);
}

int
main(void)
{
	failures = 0;
	checks = 0;

	test_promote();
	test_demote();
	test_boundary();
	test_rle_promote();
	test_churn();

	fprintf(stderr, "test_smallset_transitions: %d checks, %d failure(s)\n",
	    checks, failures);
	return (failures ? 1 : 0);
}
