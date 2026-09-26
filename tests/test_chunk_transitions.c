/* SPDX-License-Identifier: MIT */
/*
 * test_chunk_transitions.c -- sparse-chunk coalesce/split corners on this
 * sparse-only build, and the two contracts that make it sparse-only:
 *
 *   (1) The encoder NEVER emits a descriptor whose top two bits are 01
 *       (the pattern the sibling variant used for a run-length chunk).
 *       Every dense region -- even a solid 20000-bit run -- is stored as
 *       *sparse* chunks (all-ONES payloads, descriptor bits 63:62 == 11).
 *       The check walks the real serialized chunk stream with the
 *       library's own codec (#include "../sm.c") and asserts no
 *       descriptor carries that top-bit pattern.
 *
 *   (2) A foreign stream whose descriptor top two bits are 01 (written by
 *       the sibling variant) is handled SAFELY: this build has no special
 *       chunk shape, so it reads such a descriptor as an ordinary sparse
 *       chunk.  The stream is either accepted as a well-formed sparse map
 *       (validates and round-trips) or rejected on structural grounds
 *       (bounds / chunk-aligned start / overlap / exact count) -- never a
 *       crash, over-read, or half-parsed map (the S1 contract).  Whatever
 *       sm_open_copy returns, sm_validate agrees with it.
 *
 * Where a chunk<->run transition used to matter, here every dense edit
 * stays sparse and the transitions are sparse coalesce/split only:
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

/* The sibling variant's run-length descriptor flag: bits 63:62 == 01. */
#define RUNLEN_FLAG      0x4000000000000000ULL
#define RUNLEN_FLAG_MASK 0xC000000000000000ULL

/* Walk every chunk descriptor of `m` with the real codec and return the
 * number whose top two bits are 01.  This build's encoder must emit
 * zero.  Small-set maps have no chunk stream, so they count as zero. */
static int
count_top01_descriptors(const sm_t *m)
{
	if (m == NULL || __sm_is_small(m))
		return (0);
	const size_t count = __sm_get_chunk_count(m);
	uint8_t *p = __sm_get_chunk_data(m, 0);
	int n = 0;
	for (size_t i = 0; i < count; i++) {
		p += SM_SIZEOF_OVERHEAD; /* skip chunk start */
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p);
		const __sm_bitvec_t desc = *chunk.m_data;
		if ((desc & RUNLEN_FLAG_MASK) == RUNLEN_FLAG)
			n++;
		p += __sm_chunk_get_size(&chunk);
	}
	return (n);
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
	CHECK(count_top01_descriptors(m) == 0); /* encoder never emits 01 */

	size_t need = sm_serialized_size(m);
	uint8_t *buf = malloc(need);
	CHECK(sm_serialize(m, buf, need) == need);
	sm_t *back = sm_deserialize(buf, need);
	if (card == 0) {
		CHECK(back == NULL || sm_equals(m, back));
	} else {
		CHECK(back != NULL);
		CHECK(sm_equals(m, back));
		CHECK(count_top01_descriptors(back) == 0);
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
	 * The sibling variant would collapse this to a descriptor-only
	 * run-length chunk; this build stores it as one all-ONES SPARSE
	 * chunk (the descriptor's four 2-bit fields are all ONES == 11, no
	 * payload words, top two bits 11 not 01). */
	const uint64_t W = SM_CHUNK_MAX_CAPACITY; /* 2048 */
	bool *oracle = calloc(W + 64, 1);
	sm_t *m = sm_create(1 << 16);
	for (uint64_t i = 0; i < W; i++) {
		CHECK(sm_add_grow(&m, i) == i);
		oracle[i] = true;
	}
	CHECK(!__sm_is_small(m));
	CHECK(count_top01_descriptors(m) == 0);
	CHECK(__sm_get_chunk_count(m) == 1); /* one full window */
	check_oracle("sparse->full-window", m, oracle, W + 64);
	free(oracle);
	sm_free(m);
	fprintf(stderr,
	    "  sparse chunk -> full all-ONES (sparse, top bits 11) ok\n");
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
	 * full all-ONES sparse chunk; none carries the 01 top-bit pattern. */
	const uint64_t N = 20000;
	bool *oracle = calloc(N + 128, 1);
	sm_t *m = sm_create(1 << 16);
	CHECK(sm_add_range(m, 0, N));
	for (uint64_t i = 0; i < N; i++)
		oracle[i] = true;
	CHECK(!__sm_is_small(m));
	CHECK(count_top01_descriptors(m) == 0);
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
		CHECK(count_top01_descriptors(m) == 0);
		CHECK(count_top01_descriptors(other) == 0);
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
	 * correctness (encoder-emits-no-01 checked via check_oracle). */
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
/* The encoder never emits a 01-top-bit descriptor, and a foreign one  */
/* is handled safely (read as sparse or rejected structurally)         */
/* ------------------------------------------------------------------ */

/* Craft a single-chunk raw chunk-stream image whose descriptor top two
 * bits are 01 (the sibling variant's run-length flag): start `start`,
 * with the given stored length/capacity fields packed as that variant
 * would.  This build has no concept of that flag; it reads the whole
 * descriptor as ordinary sparse chunk flags. */
static void
craft_top01_wire(uint8_t wire[24], uint64_t start, uint64_t length,
    uint64_t capacity)
{
	memset(wire, 0, 24);
	uint64_t count = 1;
	memcpy(wire, &count, 8);            /* chunk count */
	__sm_store_idx(wire + 8, (__sm_idx_t)start); /* chunk start */
	uint64_t desc = RUNLEN_FLAG;
	desc |= (length & 0x7FFFFFFFULL);
	desc |= ((capacity & 0x7FFFFFFFULL) << 31);
	memcpy(wire + 16, &desc, 8);        /* descriptor */
}

/* Open an untrusted image and assert the S1 safety contract: whatever
 * sm_open_copy decides, it is self-consistent -- a returned map is
 * valid, has a well-defined cardinality, and round-trips; a rejected
 * image returns NULL.  Either way there is no crash or over-read (ASan /
 * UBSan / valgrind enforce that separately).  Returns 1 if accepted. */
static int
open_is_safe(const char *name, const uint8_t *wire, size_t n)
{
	sm_t *o = sm_open_copy(wire, n, 64);
	if (o == NULL) {
		fprintf(stderr, "    %s: rejected structurally (safe)\n", name);
		return (0);
	}
	/* Accepted: it must be a well-formed sparse map that round-trips. */
	CHECK(sm_validate(o));
	size_t card = sm_cardinality(o);
	size_t need = sm_serialized_size(o);
	uint8_t *buf = malloc(need);
	CHECK(sm_serialize(o, buf, need) == need);
	sm_t *back = sm_deserialize(buf, need);
	if (card == 0) {
		CHECK(back == NULL || sm_equals(o, back));
	} else {
		CHECK(back != NULL);
		CHECK(sm_equals(o, back));
	}
	if (back)
		sm_free(back);
	free(buf);
	fprintf(stderr,
	    "    %s: read as sparse map (card=%zu), validates + round-trips (safe)\n",
	    name, card);
	sm_free(o);
	return (1);
}

static void
test_sparse_only_contract(void)
{
	/* (1) The encoder never emits a 01-top-bit descriptor for a solid
	 * 20000-bit run. */
	{
		sm_t *m = sm_create(1 << 16);
		CHECK(sm_add_range(m, 0, 20000));
		CHECK(sm_cardinality(m) == 20000);
		CHECK(!__sm_is_small(m));
		CHECK(count_top01_descriptors(m) == 0);
		/* And after a serialize round-trip the bytes still carry no 01. */
		size_t need = sm_serialized_size(m);
		uint8_t *buf = malloc(need);
		CHECK(sm_serialize(m, buf, need) == need);
		sm_t *back = sm_deserialize(buf, need);
		CHECK(back != NULL);
		CHECK(count_top01_descriptors(back) == 0);
		if (back)
			sm_free(back);
		free(buf);
		sm_free(m);
	}

	/* (2) A foreign 01-top-bit stream is handled SAFELY.  This build has
	 * no run-length concept, so such a descriptor is read as an ordinary
	 * sparse chunk: the stream is either accepted as a well-formed sparse
	 * map (validates + round-trips) or rejected on structural grounds --
	 * never a crash, over-read, or half-parsed map.  Whatever
	 * sm_open_copy returns, sm_validate agrees with it. */
	{
		uint8_t wire[24];

		/* A chunk-aligned image: read as some sparse set, accepted. */
		craft_top01_wire(wire, 0, 1000, 2048);
		(void)open_is_safe("top01 start=0 len=1000 cap=2048", wire,
		    sizeof(wire));

		/* Full-capacity fields. */
		craft_top01_wire(wire, 0, 2048, 2048);
		(void)open_is_safe("top01 start=0 len=2048 cap=2048", wire,
		    sizeof(wire));

		/* Huge length/capacity fields. */
		craft_top01_wire(wire, 0, 0x7FFFFFFF, 0x7FFFFFFF);
		(void)open_is_safe("top01 huge len/cap", wire, sizeof(wire));

		/* A non-chunk-aligned start MUST be rejected structurally: the
		 * chunk-start alignment check catches it regardless of the
		 * descriptor's top bits. */
		craft_top01_wire(wire, 100, 500, 2048);
		{
			sm_t *o = sm_open_copy(wire, sizeof(wire), 64);
			CHECK(o == NULL); /* unaligned start rejected */
			if (o)
				sm_free(o);
		}

		/* A same-bytes wrapped copy: whatever sm_validate says must match
		 * what sm_open re-derives (self-consistency), and there is no
		 * over-read on the tight buffer (enforced by ASan/valgrind). */
		craft_top01_wire(wire, 0, 1000, 2048);
		{
			sm_t *w = sm_create(sizeof(wire) + 64);
			CHECK(w != NULL);
			memcpy(sm_get_data(w), wire, sizeof(wire));
			w->m_data_used = __sm_cap(w);
			w->m_data_used = __sm_get_size_impl(w);
			int v = sm_validate(w);
			/* open_copy on the same bytes must agree with validate. */
			sm_t *o = sm_open_copy(wire, sizeof(wire), 64);
			CHECK((o != NULL) == (v != 0));
			if (o)
				sm_free(o);
			sm_free(w);
		}
	}
	fprintf(stderr,
	    "  never emits 01-top-bit descriptor; foreign 01 stream handled safely ok\n");
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
	test_sparse_only_contract();

	fprintf(stderr, "test_chunk_transitions: %d checks, %d failure(s)\n",
	    checks, failures);
	return (failures ? 1 : 0);
}
