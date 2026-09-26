/* SPDX-License-Identifier: MIT */
/*
 * test_chunk_transitions.c -- sparse-chunk coalesce/split corners on the
 * RLE-free build, and the two contracts that make it RLE-free:
 *
 *   (1) The encoder NEVER emits a run-length-encoded chunk.  Every dense
 *       region -- even a solid 20000-bit run -- is stored as *sparse*
 *       chunks (all-ONES payloads, descriptor bits 63:62 == 11), never
 *       an RLE descriptor (bits 63:62 == 01).  The check walks the real
 *       serialized chunk stream with the library's own codec (#include
 *       "../sm.c") and asserts no descriptor carries the RLE flag.
 *
 *   (2) A foreign stream that DOES carry an RLE chunk (written by the
 *       RLE variant) is rejected: sm_validate returns false and
 *       sm_open_copy / sm_deserialize return NULL rather than a
 *       half-parsed or crashing map (the S1 contract; sm.c validate arm
 *       "(a) RLE reader decision").
 *
 * This is the RLE-free counterpart of the RLE build's
 * test_rle_transitions.c: where that suite drives chunk<->RLE
 * transitions, here every dense edit stays sparse and the transitions
 * are sparse coalesce/split only:
 *   - a sparse chunk that becomes fully set (all-ONES, no MIXED words);
 *   - a hole poked into a dense region (split into two dense pieces);
 *   - a run of all-ONES sparse chunks spanning multiple chunk windows;
 *   - extending, merging, and splitting dense regions.
 *
 * Every case cross-checks membership against a brute-force oracle,
 * asserts sm_validate, and round-trips through serialize/deserialize.
 * Clean under plain, ASan+UBSan, and valgrind.
 */
#include "../sm.c"

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

/* RLE descriptor flag (the RLE variant's format): bits 63:62 == 01. */
#define RLE_FLAG      0x4000000000000000ULL
#define RLE_FLAG_MASK 0xC000000000000000ULL

/* Walk every chunk descriptor of `m` with the real codec and return the
 * number carrying the foreign RLE flag.  The RLE-free encoder must emit
 * zero.  Small-set maps have no chunk stream, so they count as zero. */
static int
count_rle_descriptors(const sm_t *m)
{
	if (m == NULL || __sm_is_small(m))
		return (0);
	const size_t count = __sm_get_chunk_count(m);
	uint8_t *p = __sm_get_chunk_data(m, 0);
	int rle = 0;
	for (size_t i = 0; i < count; i++) {
		p += SM_SIZEOF_OVERHEAD; /* skip chunk start */
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p);
		const __sm_bitvec_t desc = *chunk.m_data;
		if ((desc & RLE_FLAG_MASK) == RLE_FLAG)
			rle++;
		p += __sm_chunk_get_size(&chunk);
	}
	return (rle);
}

/* Cross-check the whole map against a boolean oracle over [0, hi),
 * assert validity, and round-trip through serialize/deserialize. */
static void
check_oracle(const char *name, sm_t *m, const bool *oracle, uint64_t hi)
{
	size_t card = 0;
	for (uint64_t x = 0; x < hi; x++) {
		if (sm_contains(m, x, NULL) != oracle[x]) {
			fprintf(stderr, "  MISS %s at %llu (got %d want %d)\n",
			    name, (unsigned long long)x,
			    sm_contains(m, x, NULL), (int)oracle[x]);
			CHECK(sm_contains(m, x, NULL) == oracle[x]);
			break;
		}
		if (oracle[x])
			card++;
	}
	CHECK(sm_cardinality(m) == card);
	CHECK(sm_validate(m));
	CHECK(count_rle_descriptors(m) == 0); /* never RLE on this build */

	size_t need = sm_serialized_size(m);
	uint8_t *buf = malloc(need);
	CHECK(sm_serialize(m, buf, need) == need);
	sm_t *back = sm_deserialize(buf, need);
	if (card == 0) {
		CHECK(back == NULL || sm_equals(m, back));
	} else {
		CHECK(back != NULL);
		CHECK(sm_equals(m, back));
		CHECK(count_rle_descriptors(back) == 0);
	}
	if (back)
		sm_free(back);
	free(buf);
}

/* ------------------------------------------------------------------ */
/* A sparse chunk that becomes fully set stays sparse (all-ONES)      */
/* ------------------------------------------------------------------ */
static void
test_sparse_becomes_full(void)
{
	/* Fill a single chunk window [0, SM_CHUNK_MAX_CAPACITY) completely.
	 * The RLE build would collapse this to a descriptor-only RLE chunk;
	 * the RLE-free build stores it as one all-ONES SPARSE chunk (the
	 * descriptor's four 2-bit fields are all ONES == 11, no payload
	 * words, no RLE flag). */
	const uint64_t W = SM_CHUNK_MAX_CAPACITY; /* 2048 */
	bool *oracle = calloc(W + 64, 1);
	sm_t *m = sm_create(1 << 16);
	for (uint64_t i = 0; i < W; i++) {
		CHECK(sm_add_grow(&m, i) == i);
		oracle[i] = true;
	}
	CHECK(!__sm_is_small(m));
	CHECK(count_rle_descriptors(m) == 0);
	CHECK(__sm_get_chunk_count(m) == 1); /* one full window */
	check_oracle("sparse->full-window", m, oracle, W + 64);
	free(oracle);
	sm_free(m);
	fprintf(stderr, "  sparse chunk -> full all-ONES (sparse, not RLE) ok\n");
}

/* ------------------------------------------------------------------ */
/* Poke a hole into a dense region (splits the run in two)            */
/* ------------------------------------------------------------------ */
static void
poke_hole_case(uint64_t length, uint64_t hole)
{
	uint64_t hi = length + 128;
	bool *oracle = calloc(hi, 1);
	sm_t *m = sm_create(1 << 16);
	CHECK(sm_add_range(m, 0, length));
	for (uint64_t i = 0; i < length; i++)
		oracle[i] = true;

	CHECK(sm_remove(m, hole) == hole);
	oracle[hole] = false;

	char name[64];
	snprintf(name, sizeof(name), "hole len=%llu@%llu",
	    (unsigned long long)length, (unsigned long long)hole);
	check_oracle(name, m, oracle, hi);
	free(oracle);
	sm_free(m);
}

static void
test_holes(void)
{
	/* Dense-region lengths that straddle word (64) and chunk-window
	 * (2048) boundaries; a hole at the start, middle, end, and at the
	 * word boundaries exercises the sparse mixed/ones payload split. */
	const uint64_t lens[] = { 63, 64, 91, 2047, 2048, 5000 };
	for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
		uint64_t L = lens[li];
		uint64_t holes[8];
		size_t hn = 0;
		holes[hn++] = 0;         /* start */
		if (L > 2)
			holes[hn++] = L / 2; /* middle */
		holes[hn++] = L - 1;         /* end */
		if (L > 63)
			holes[hn++] = 63;    /* word boundary */
		if (L > 64)
			holes[hn++] = 64;    /* word boundary */
		if (L > 2048)
			holes[hn++] = 2048;  /* chunk-window boundary */
		for (size_t h = 0; h < hn; h++)
			poke_hole_case(L, holes[h]);
	}
	fprintf(stderr, "  holes in dense regions ok\n");
}

/* ------------------------------------------------------------------ */
/* Runs of all-ONES sparse chunks spanning multiple chunk windows     */
/* ------------------------------------------------------------------ */
static void
test_multi_window_all_ones(void)
{
	/* A solid 20000-bit run spans ~10 chunk windows.  Every window is a
	 * full all-ONES sparse chunk; none is RLE. */
	const uint64_t N = 20000;
	bool *oracle = calloc(N + 128, 1);
	sm_t *m = sm_create(1 << 16);
	CHECK(sm_add_range(m, 0, N));
	for (uint64_t i = 0; i < N; i++)
		oracle[i] = true;
	CHECK(!__sm_is_small(m));
	CHECK(count_rle_descriptors(m) == 0);
	CHECK(__sm_get_chunk_count(m) >= 2); /* genuinely multi-window */
	check_oracle("multi-window[0,20000)", m, oracle, N + 128);
	free(oracle);
	sm_free(m);
	fprintf(stderr, "  multi-window all-ONES sparse run ok\n");
}

/* ------------------------------------------------------------------ */
/* Extend, merge, and split dense regions                             */
/* ------------------------------------------------------------------ */
static void
test_extend_merge_split(void)
{
	/* Extend a dense region: [0,5000) then add [5000,5500). */
	{
		bool *oracle = calloc(6000, 1);
		sm_t *m = sm_create(1 << 16);
		CHECK(sm_add_range(m, 0, 5000));
		for (uint64_t i = 0; i < 5000; i++)
			oracle[i] = true;
		check_oracle("dense[0,5000)", m, oracle, 6000);

		CHECK(sm_add_range(m, 5000, 5500));
		for (uint64_t i = 5000; i < 5500; i++)
			oracle[i] = true;
		check_oracle("extend-dense", m, oracle, 6000);
		free(oracle);
		sm_free(m);
	}

	/* Merge two adjacent dense regions: [0,1000) and [1000,2000) meet. */
	{
		bool *oracle = calloc(3000, 1);
		sm_t *m = sm_create(1 << 16);
		CHECK(sm_add_range(m, 0, 1000));
		CHECK(sm_add_range(m, 1000, 2000));
		for (uint64_t i = 0; i < 2000; i++)
			oracle[i] = true;
		check_oracle("merge-adjacent", m, oracle, 3000);
		free(oracle);
		sm_free(m);
	}

	/* Split a dense region: [0,4000) then remove [1500,2500) -> two
	 * dense pieces; then refill and re-merge. */
	{
		bool *oracle = calloc(5000, 1);
		sm_t *m = sm_create(1 << 16);
		CHECK(sm_add_range(m, 0, 4000));
		for (uint64_t i = 0; i < 4000; i++)
			oracle[i] = true;
		CHECK(sm_remove_range(m, 1500, 2500));
		for (uint64_t i = 1500; i < 2500; i++)
			oracle[i] = false;
		check_oracle("split-dense-hole", m, oracle, 5000);

		CHECK(sm_add_range(m, 1500, 2500));
		for (uint64_t i = 1500; i < 2500; i++)
			oracle[i] = true;
		check_oracle("refill-merge", m, oracle, 5000);
		free(oracle);
		sm_free(m);
	}

	/* sm_split within a single dense region: the moved half has no
	 * internal gap, so it is a clean, valid partition.  Verified by a
	 * direct membership scan; both halves stay sparse. */
	{
		sm_t *m = sm_create(1 << 16);
		CHECK(sm_add_range(m, 0, 5000));
		sm_t *other = sm_create(1 << 16);
		(void)sm_split(m, 2000, other);
		CHECK(sm_validate(m));
		CHECK(sm_validate(other));
		CHECK(count_rle_descriptors(m) == 0);
		CHECK(count_rle_descriptors(other) == 0);
		CHECK(sm_cardinality(m) == 2000);
		CHECK(sm_cardinality(other) == 3000);
		CHECK(sm_maximum(m) == 1999);
		CHECK(sm_minimum(other) == 2000);
		for (uint64_t x = 0; x < 5100; x++) {
			int inm = sm_contains(m, x, NULL);
			int ino = sm_contains(other, x, NULL);
			CHECK(inm == (x < 2000));
			CHECK(ino == (x >= 2000 && x < 5000));
			CHECK(!(inm && ino)); /* disjoint */
		}
		sm_free(other);
		sm_free(m);
	}

	/* Two dense regions separated by a gap: read + round-trip
	 * correctness (no.-rle-emitted included via check_oracle). */
	{
		bool *oracle = calloc(9000, 1);
		sm_t *m = sm_create(1 << 16);
		CHECK(sm_add_range(m, 0, 3000));
		CHECK(sm_add_range(m, 6000, 8000));
		for (uint64_t i = 0; i < 3000; i++)
			oracle[i] = true;
		for (uint64_t i = 6000; i < 8000; i++)
			oracle[i] = true;
		check_oracle("two-dense-regions", m, oracle, 9000);
		free(oracle);
		sm_free(m);
	}
	fprintf(stderr, "  extend / merge / split dense regions ok\n");
}

/* ------------------------------------------------------------------ */
/* The encoder never emits RLE, and validate rejects a foreign RLE    */
/* ------------------------------------------------------------------ */

/* Craft a single-chunk wire image carrying an RLE descriptor: run
 * [0, length) with the given stored capacity. */
static void
craft_rle_wire(uint8_t wire[24], uint64_t length, uint64_t capacity)
{
	memset(wire, 0, 24);
	uint64_t count = 1;
	memcpy(wire, &count, 8);      /* chunk count */
	/* chunk start = 0 at wire+8 (already zeroed) */
	uint64_t desc = RLE_FLAG;
	desc |= (length & 0x7FFFFFFFULL);
	desc |= ((capacity & 0x7FFFFFFFULL) << 31);
	memcpy(wire + 16, &desc, 8);  /* RLE descriptor */
}

static void
test_no_rle_contract(void)
{
	/* (1) The encoder never emits RLE for a solid 20000-bit run. */
	{
		sm_t *m = sm_create(1 << 16);
		CHECK(sm_add_range(m, 0, 20000));
		CHECK(sm_cardinality(m) == 20000);
		CHECK(!__sm_is_small(m));
		CHECK(count_rle_descriptors(m) == 0);
		/* And after a serialize round-trip the bytes are still RLE-free. */
		size_t need = sm_serialized_size(m);
		uint8_t *buf = malloc(need);
		CHECK(sm_serialize(m, buf, need) == need);
		sm_t *back = sm_deserialize(buf, need);
		CHECK(back != NULL);
		CHECK(count_rle_descriptors(back) == 0);
		if (back)
			sm_free(back);
		free(buf);
		sm_free(m);
	}

	/* (2) A foreign RLE-flagged stream is rejected. */
	{
		uint8_t wire[24];
		/* legal-looking RLE run [0,1000), capacity 2048. */
		craft_rle_wire(wire, 1000, 2048);

		/* sm_open_copy validates untrusted bytes and must return NULL. */
		sm_t *o = sm_open_copy(wire, sizeof(wire), 64);
		CHECK(o == NULL);
		if (o)
			sm_free(o);

		/* sm_validate on a wrapped copy of the same bytes must be false. */
		sm_t *w = sm_create(sizeof(wire) + 64);
		CHECK(w != NULL);
		memcpy(sm_get_data(w), wire, sizeof(wire));
		w->m_data_used = __sm_cap(w);
		w->m_data_used = __sm_get_size_impl(w);
		CHECK(!sm_validate(w));
		sm_free(w);

		/* A second shape: a full-capacity RLE run. */
		craft_rle_wire(wire, 2048, 2048);
		sm_t *o2 = sm_open_copy(wire, sizeof(wire), 64);
		CHECK(o2 == NULL);
		if (o2)
			sm_free(o2);
	}
	fprintf(stderr, "  never emits RLE; rejects foreign RLE stream ok\n");
}

int
main(void)
{
	failures = 0;
	checks = 0;

	test_sparse_becomes_full();
	test_holes();
	test_multi_window_all_ones();
	test_extend_merge_split();
	test_no_rle_contract();

	fprintf(stderr, "test_chunk_transitions: %d checks, %d failure(s)\n",
	    checks, failures);
	return (failures ? 1 : 0);
}
