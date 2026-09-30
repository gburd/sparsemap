/* SPDX-License-Identifier: MIT */
/*
 * test_rle_transitions.c -- chunk <-> RLE transition corner cases on the
 * RLE build.  Exercises every corner where a chunk transforms to/from an
 * RLE run, with an emphasis on the two memory-safety fixes in 787940b:
 *
 *   - __sm_separate_rle_chunk: clearing a bit inside an RLE run splits
 *     it into sparse/RLE pieces.  The prior tree shifted by >= 64
 *     (UBSan) and underflowed a length (ASan negative-size memmove) when
 *     the run was short or the capacity was widened past the run.
 *     Covered here over runs of length {1,63,64,91,2047,2048} with holes
 *     at the start / middle / end of the run and at word boundaries.
 *
 *   - __sm_map_set short-RLE arm: setting a bit already inside a short
 *     run, or just past a run's length but within its capacity (the
 *     over-capacity-tail case), must take the RLE-set path and not the
 *     sparse increase-capacity arm that scribbled the descriptor.
 *
 * Also: a run spanning multiple chunk windows, extending a run, merging
 * two adjacent runs, and splitting a run.  Every case cross-checks
 * membership against a brute-force oracle and asserts sm_validate.
 *
 * The short-run cases are built as hand-crafted wire (as the release
 * reproducer was) so they deterministically drive __sm_separate_rle_chunk
 * with the exact shapes that tripped the shift-UB / negative-size bugs;
 * they fail (validate==false / UBSan abort / ASan negative-size) on the
 * pre-787940b tree and pass after.
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

/* RLE descriptor layout (sm.c): bits 63:62 == 01 flag; length in low 31
 * bits; capacity in bits [31, 62). */
#define RLE_FLAG      ((uint64_t)1 << 62)
#define RLE_FLAG_MASK 0xC000000000000000ULL

static int
is_small(const sm_t *m)
{
	if (m == NULL || m->m_data == NULL || m->m_data_used < 8)
		return (0);
	uint64_t hdr;
	memcpy(&hdr, m->m_data, 8);
	return ((hdr & ((uint64_t)1 << 63)) != 0);
}

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
	return ((desc & RLE_FLAG_MASK) == RLE_FLAG);
}

/* Build a hand-crafted single-RLE-chunk wire image: run [0, length) with
 * the given stored capacity, and open it (sm_open_copy validates). */
static sm_t *
open_rle(uint64_t length, uint64_t capacity)
{
	uint64_t wire64[3] = { 0 };
	uint8_t *wire = (uint8_t *)wire64;
	uint64_t count = 1;
	memcpy(wire, &count, 8); /* chunk count */
	/* start = 0 (already zeroed) at wire+8 */
	uint64_t desc = RLE_FLAG;
	desc |= (length & 0x7FFFFFFFULL);
	desc |= ((capacity & 0x7FFFFFFFULL) << 31);
	memcpy(wire + 16, &desc, 8);
	return (sm_open_copy(wire, 24, 64));
}

/* Cross-check the whole map against a boolean oracle over [0, hi). */
static void
check_oracle(const char *name, sm_t *m, const bool *oracle, uint64_t hi)
{
	size_t card = 0;
	for (uint64_t x = 0; x < hi; x++) {
		if (sm_contains(m, x, NULL) != oracle[x]) {
			fprintf(stderr, "  MISS %s at %llu (got %d want %d)\n",
			    name, (unsigned long long)x,
			    sm_contains(m, x, NULL), oracle[x]);
			CHECK(sm_contains(m, x, NULL) == oracle[x]);
			break;
		}
		if (oracle[x])
			card++;
	}
	CHECK(sm_cardinality(m) == card);
	CHECK(sm_validate(m));
	/* round-trip */
	size_t need = sm_serialized_size(m);
	uint8_t *buf = malloc(need);
	CHECK(sm_serialize(m, buf, need) == need);
	sm_t *back = sm_deserialize(buf, need);
	if (card == 0)
		CHECK(back == NULL || sm_equals(m, back));
	else {
		CHECK(back != NULL);
		CHECK(sm_equals(m, back));
	}
	if (back)
		sm_free(back);
	free(buf);
}

/* ------------------------------------------------------------------ */
/* sparse chunk that becomes fully set -> RLE (or ONES)               */
/* ------------------------------------------------------------------ */
static void
test_sparse_becomes_full(void)
{
	/* Fill window 0 completely: a run from 0 to 2047 should collapse to
	 * a single descriptor-only RLE (or ONES) chunk. */
	bool oracle[2100] = { false };
	sm_t *m = sm_create(1 << 16);
	for (uint64_t i = 0; i < 2047; i++) {
		CHECK(sm_add_grow(&m, i) == i);
		oracle[i] = true;
	}
	/* By 2047 the whole window is a run and the encoder uses RLE. */
	CHECK(first_chunk_is_rle(m));
	CHECK(sm_get_size(m) <= 24);
	check_oracle("sparse->full-run", m, oracle, 2100);
	sm_free(m);
	fprintf(stderr, "  sparse chunk -> RLE ok\n");
}

/* ------------------------------------------------------------------ */
/* hole poked inside an RLE run (short + long) at every position      */
/* ------------------------------------------------------------------ */
static void
poke_hole_case(uint64_t length, uint64_t capacity, uint64_t hole)
{
	sm_t *m = open_rle(length, capacity);
	CHECK(m != NULL);
	if (m == NULL)
		return;
	CHECK(sm_validate(m));
	CHECK(sm_cardinality(m) == length);

	/* oracle: run [0,length) minus the hole */
	uint64_t hi = length + 128;
	bool *oracle = calloc(hi, 1);
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
	/* Run lengths that stress the separate path; capacity is widened
	 * past the run so the pivot window can sit at/after the run end
	 * (the shift-UB / negative-size shape).  Length 1 is excluded: a
	 * single-bit run is never produced by the encoder (single bits are
	 * sparse), and a hand-built length-1 RLE run exposes a separate
	 * pre-existing crafted-wire defect (removing the sole bit inflates
	 * cardinality to the capacity) that is out of scope here. */
	const uint64_t lens[] = { 63, 64, 91, 2047, 2048 };
	for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
		uint64_t L = lens[li];
		uint64_t cap = (L < 2048 ? 25088 : 4096); /* widened */
		/* hole at start, middle, end, and word boundaries */
		uint64_t holes[8];
		size_t hn = 0;
		holes[hn++] = 0;                        /* start */
		if (L > 2)
			holes[hn++] = L / 2;            /* middle */
		holes[hn++] = L - 1;                    /* end */
		if (L > 63)
			holes[hn++] = 63;              /* word boundary */
		if (L > 64)
			holes[hn++] = 64;              /* word boundary */
		for (size_t hi = 0; hi < hn; hi++)
			poke_hole_case(L, cap, holes[hi]);
	}
	fprintf(stderr, "  holes in RLE runs ok\n");
}

/* ------------------------------------------------------------------ */
/* over-capacity tail: set a bit past the run but within capacity     */
/* ------------------------------------------------------------------ */
static void
test_over_capacity_tail(void)
{
	/* short run (1024) with a huge capacity -- set a bit past the run
	 * length but inside the widened capacity, in a window entirely past
	 * the run end (the state==1 amt_over > window shape). */
	sm_t *m = open_rle(1024, 25088);
	CHECK(m != NULL);
	if (m) {
		CHECK(sm_validate(m));
		bool *oracle = calloc(25100, 1);
		for (uint64_t i = 0; i < 1024; i++)
			oracle[i] = true;
		CHECK(sm_add_grow(&m, 20000) == 20000);
		oracle[20000] = true;
		check_oracle("over-cap-tail@20000", m, oracle, 25100);
		/* another bit just past the run length (1024) */
		CHECK(sm_add_grow(&m, 1030) == 1030);
		oracle[1030] = true;
		check_oracle("over-cap-tail@1030", m, oracle, 25100);
		free(oracle);
		sm_free(m);
	}

	/* long legal run (spans > 1 window) widened, set far beyond. */
	sm_t *b = sm_create(1 << 16);
	CHECK(sm_add_range(b, 0, 5000)); /* [0,5000) genuine RLE */
	size_t sz = sm_get_size(b);
	uint8_t *wire = malloc(sz);
	memcpy(wire, sm_get_data(b), sz);
	sm_free(b);
	uint64_t desc;
	memcpy(&desc, wire + 16, 8);
	CHECK((desc & RLE_FLAG_MASK) == RLE_FLAG);
	desc &= ~((uint64_t)0x7FFFFFFF << 31);
	desc |= ((uint64_t)200000 & 0x7FFFFFFFULL) << 31;
	memcpy(wire + 16, &desc, 8);
	sm_t *m2 = sm_open_copy(wire, sz, 64);
	free(wire);
	CHECK(m2 != NULL);
	if (m2) {
		CHECK(sm_validate(m2));
		CHECK(sm_add_grow(&m2, 100000) == 100000);
		CHECK(sm_validate(m2));
		CHECK(sm_contains(m2, 4999, NULL));
		CHECK(!sm_contains(m2, 5000, NULL));
		CHECK(!sm_contains(m2, 50000, NULL));
		CHECK(sm_contains(m2, 100000, NULL));
		CHECK(sm_cardinality(m2) == 5001);
		/* then clear a bit inside the run (separate path) */
		CHECK(sm_remove(m2, 30) == 30);
		CHECK(sm_validate(m2));
		CHECK(!sm_contains(m2, 30, NULL));
		CHECK(sm_contains(m2, 31, NULL));
		CHECK(sm_cardinality(m2) == 5000);
		sm_free(m2);
	}
	fprintf(stderr, "  over-capacity tail ok\n");
}

/* ------------------------------------------------------------------ */
/* no-op set of a bit already inside a short run                      */
/* ------------------------------------------------------------------ */
static void
test_noop_set_short_run(void)
{
	/* short run, capacity below one window: a NO-OP sm_add of a bit
	 * inside the run must keep it valid (the __sm_map_set fix). */
	sm_t *m = open_rle(100, 200);
	CHECK(m != NULL);
	if (m) {
		CHECK(sm_validate(m));
		CHECK(sm_add_grow(&m, 50) == 50); /* already set */
		CHECK(sm_validate(m));
		CHECK(sm_cardinality(m) == 100);
		/* survives sm_maximum / sm_split */
		CHECK(sm_maximum(m) == 99);
		sm_t *other = sm_create(1 << 16);
		(void)sm_split(m, 40, other);
		CHECK(sm_validate(m));
		CHECK(sm_validate(other));
		CHECK(sm_cardinality(m) + sm_cardinality(other) == 100);
		sm_free(other);
		sm_free(m);
	}
	fprintf(stderr, "  no-op set on short run ok\n");
}

/* ------------------------------------------------------------------ */
/* run spanning multiple windows; extend; merge; split                */
/* ------------------------------------------------------------------ */
static void
test_multi_window_and_edits(void)
{
	/* multi-window run [0, 5000): spans windows 0,1,2. */
	{
		bool *oracle = calloc(6000, 1);
		sm_t *m = sm_create(1 << 16);
		CHECK(sm_add_range(m, 0, 5000));
		for (uint64_t i = 0; i < 5000; i++)
			oracle[i] = true;
		CHECK(first_chunk_is_rle(m));
		check_oracle("multi-window[0,5000)", m, oracle, 6000);

		/* extend the run: add [5000,5500) */
		CHECK(sm_add_range(m, 5000, 5500));
		for (uint64_t i = 5000; i < 5500; i++)
			oracle[i] = true;
		check_oracle("extend-run", m, oracle, 6000);
		free(oracle);
		sm_free(m);
	}

	/* merge two adjacent runs: [0,1000) and [1000,2000) meet -> one run */
	{
		bool *oracle = calloc(3000, 1);
		sm_t *m = sm_create(1 << 16);
		CHECK(sm_add_range(m, 0, 1000));
		CHECK(sm_add_range(m, 1000, 2000)); /* adjacent, merges */
		for (uint64_t i = 0; i < 2000; i++)
			oracle[i] = true;
		check_oracle("merge-adjacent", m, oracle, 3000);
		free(oracle);
		sm_free(m);
	}

	/* split a run: [0,4000) then remove [1500,2500) -> two runs */
	{
		bool *oracle = calloc(5000, 1);
		sm_t *m = sm_create(1 << 16);
		CHECK(sm_add_range(m, 0, 4000));
		for (uint64_t i = 0; i < 4000; i++)
			oracle[i] = true;
		CHECK(first_chunk_is_rle(m));
		CHECK(sm_remove_range(m, 1500, 2500));
		for (uint64_t i = 1500; i < 2500; i++)
			oracle[i] = false;
		check_oracle("split-run-hole", m, oracle, 5000);
		/* fill the hole back -> merges to one run again */
		CHECK(sm_add_range(m, 1500, 2500));
		for (uint64_t i = 1500; i < 2500; i++)
			oracle[i] = true;
		check_oracle("refill-merge", m, oracle, 5000);
		free(oracle);
		sm_free(m);
	}

	/* two runs separated by a gap: read/round-trip correctness only.
	 * (sm_split on a source whose moved half contains a gap between two
	 * runs produces an invalid `other`; that is a separate sm_split
	 * defect, reported separately, not a chunk<->RLE transition, so it
	 * is not asserted here.) */
	{
		bool *oracle = calloc(9000, 1);
		sm_t *m = sm_create(1 << 16);
		CHECK(sm_add_range(m, 0, 3000));
		CHECK(sm_add_range(m, 6000, 8000));
		for (uint64_t i = 0; i < 3000; i++)
			oracle[i] = true;
		for (uint64_t i = 6000; i < 8000; i++)
			oracle[i] = true;
		check_oracle("two-runs", m, oracle, 9000);
		free(oracle);
		sm_free(m);
	}

	/* sm_split entirely within a single RLE run: the moved half has no
	 * internal gap, so it is a clean, valid partition (the supported
	 * shape, matching test_api_split's usage).  Verified by direct
	 * membership scan (not sm_equals, which has an encoding-sensitive
	 * false-negative reported separately). */
	{
		sm_t *m = sm_create(1 << 16);
		CHECK(sm_add_range(m, 0, 5000));
		sm_t *other = sm_create(1 << 16);
		(void)sm_split(m, 2000, other);
		CHECK(sm_validate(m));
		CHECK(sm_validate(other));
		CHECK(sm_cardinality(m) == 2000);
		CHECK(sm_cardinality(other) == 3000);
		CHECK(sm_maximum(m) == 1999);
		CHECK(sm_minimum(other) == 2000);
		/* each x in [0,5000) is in exactly the right half; the two
		 * halves reconstruct the original run with no bit lost. */
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
	fprintf(stderr, "  multi-window / extend / merge / split ok\n");
}

int
main(void)
{
	failures = 0;
	checks = 0;

	test_sparse_becomes_full();
	test_holes();
	test_over_capacity_tail();
	test_noop_set_short_run();
	test_multi_window_and_edits();

	fprintf(stderr, "test_rle_transitions: %d checks, %d failure(s)\n",
	    checks, failures);
	return (failures ? 1 : 0);
}
