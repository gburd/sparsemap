/* SPDX-License-Identifier: MIT
 *
 * Regression test for the 5.8.1 Defect 2 validator gap:
 * sm_validate accepted a sparse chunk whose descriptor places a
 * data-bearing slot beyond the chunk's measured capacity.
 *
 * A sparse descriptor packs 32 two-bit flags (00=ZEROS, 01=NONE,
 * 10=MIXED, 11=ONES).  __sm_chunk_get_capacity reports
 * SM_CHUNK_MAX_CAPACITY (2048) minus 64 bits for every NONE flag,
 * wherever it sits.  The slot-indexed readers (rank / cardinality /
 * select / minimum / maximum) place slot i's bits at i*64, but the
 * capacity-bounded readers (contains / next_member) stop at
 * start+capacity.  When a NONE flag sits BELOW a data-bearing
 * (ONES / MIXED) slot the two disagree: sm_cardinality counts the high
 * slot, sm_next_member / sm_contains skip it.
 *
 * The encoder never writes such a chunk -- it only emits sparse
 * descriptors whose highest data-bearing slot fits inside the reduced
 * capacity (NONE anywhere is fine as long as no ONES/MIXED slot sits at
 * or beyond capacity).  sm_validate must enforce exactly that invariant:
 * reject the shape the readers disagree on, accept every shape the
 * encoder actually produces.
 *
 * Property for ACCEPTED buffers: sm_cardinality() must equal an
 * sm_next_member() walk count.  We assert it on every buffer that
 * validate accepts here.
 */
#define SM_EXPOSE_STRUCT 1
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                              \
	do {                                                          \
		if (!(cond)) {                                        \
			fprintf(stderr, "FAIL: %s\n", msg);           \
			failures++;                                   \
		}                                                     \
	} while (0)

/* Count set bits by iterating sm_next_member. */
static uint64_t
walk_next(sm_t *m)
{
	uint64_t n = 0, i = SM_IDX_MAX;
	sm_cursor_t c = SM_CURSOR_INIT;
	while ((i = sm_next_member(m, i, &c)) != SM_IDX_MAX) {
		n++;
		if (n > (uint64_t)10000000U)
			break;
	}
	return (n);
}

/* The property every ACCEPTED buffer must satisfy. */
static void
assert_card_equals_walk(sm_t *m, const char *tag)
{
	uint64_t card = sm_cardinality(m);
	uint64_t walk = walk_next(m);
	if (card != walk) {
		fprintf(stderr,
		    "FAIL: %s: sm_cardinality=%ju != next_member walk=%ju\n",
		    tag, (uintmax_t)card, (uintmax_t)walk);
		failures++;
	}
}

/*
 * Build a one-chunk sparse buffer at start 0:
 *   flags 0..30 = ONES, flag 31 = MIXED (one payload word, low bit set).
 * Optionally flip flag `which` to NONE.  Returns the body length in
 * `*len`.  `body` must be at least 1024 bytes.
 */
static size_t
build_body(uint8_t *body, int flip, int which)
{
	uint64_t count = 1;
	uint64_t desc = 0;
	uint64_t payload = 0x1; /* flag 31 MIXED word: bit at 31*64 == 1984 */
	uint64_t s0 = 0;
	size_t off = 0;
	int f;
	memset(body, 0, 1024);
	for (f = 0; f < 31; f++)
		desc |= (uint64_t)3 << (f * 2); /* ONES */
	desc |= (uint64_t)2 << (31 * 2);	/* flag 31 MIXED */
	if (flip)
		desc = (desc & ~((uint64_t)3 << (which * 2))) |
		    ((uint64_t)1 << (which * 2)); /* -> NONE */
	memcpy(body + off, &count, 8);
	off += 8;
	memcpy(body + off, &s0, 8);
	off += 8;
	memcpy(body + off, &desc, 8);
	off += 8;
	memcpy(body + off, &payload, 8);
	off += 8;
	return (off);
}

/* Open a crafted body via sm_open_copy and sm_deserialize; it must be
 * rejected (NULL, or a valid empty map) by both.  A survivor that is not
 * valid, or that validates but disagrees on cardinality vs walk, fails. */
static void
expect_rejected(const uint8_t *body, size_t n, const char *tag)
{
	{
		sm_t *m = sm_open_copy(body, n + 128, 128);
		if (m != NULL) {
			CHECK(sm_validate(m),
			    "sm_open_copy returned a buffer that is invalid");
			/* If it survived at all it must be self-consistent. */
			assert_card_equals_walk(m, tag);
			sm_free(m);
		}
	}
	{
		size_t wn = 16 + n;
		uint8_t *w = calloc(1, wn);
		uint32_t magic = 0x30316d73u; /* "sm10" v2 header */
		if (w == NULL) {
			failures++;
			return;
		}
		memcpy(w, &magic, 4);
		w[4] = 2;
		w[5] = 1;
		memcpy(w + 16, body, n);
		{
			sm_t *m = sm_deserialize(w, wn);
			if (m != NULL) {
				CHECK(sm_validate(m),
				    "sm_deserialize returned an invalid buffer");
				assert_card_equals_walk(m, tag);
				sm_free(m);
			}
		}
		free(w);
	}
}

int
main(void)
{
	uint8_t body[1024];
	size_t n;
	int which;

	/* The baseline (no flip) is a legitimate chunk: all 31 ONES slots +
	 * the MIXED slot fit within capacity 2048.  It must stay ACCEPTED
	 * and self-consistent. */
	n = build_body(body, 0, 0);
	{
		sm_t *m = sm_open_copy(body, n + 128, 128);
		CHECK(m != NULL, "baseline chunk should open");
		if (m != NULL) {
			CHECK(sm_validate(m), "baseline should validate");
			CHECK(sm_cardinality(m) == 1985,
			    "baseline cardinality wrong");
			assert_card_equals_walk(m, "baseline");
			sm_free(m);
		}
	}

	/* Flip an interior flag to NONE (before the data-bearing slot 31).
	 * This is the hostile shape the readers disagree on; it must now be
	 * REJECTED by every decode entry point. */
	for (which = 0; which < 31; which++) {
		char tag[48];
		n = build_body(body, 1, which);
		snprintf(tag, sizeof tag, "flip-flag-%d-to-NONE", which);
		expect_rejected(body, n, tag);
	}

	/* An encoder-written reduced-capacity chunk with a NONE flag that
	 * does NOT push data past capacity must still VALIDATE and stay
	 * self-consistent (do not over-reject NONE itself).  Pattern taken
	 * from a real encoder output: slot 0 MIXED, slot 1 NONE, rest ZEROS
	 * (desc = 0x6).  Highest data slot (0) fits in capacity (2048-64). */
	{
		uint8_t b[1024];
		uint64_t count = 1, s0 = 0;
		uint64_t desc = ((uint64_t)2 << 0) | ((uint64_t)1 << 2); /* MIXED, NONE */
		uint64_t payload = (uint64_t)0x00000000000000FFU; /* low 8 bits */
		size_t off = 0;
		sm_t *m;
		memset(b, 0, sizeof b);
		memcpy(b + off, &count, 8);
		off += 8;
		memcpy(b + off, &s0, 8);
		off += 8;
		memcpy(b + off, &desc, 8);
		off += 8;
		memcpy(b + off, &payload, 8);
		off += 8;
		m = sm_open_copy(b, off + 128, 128);
		CHECK(m != NULL, "encoder-NONE chunk should open");
		if (m != NULL) {
			CHECK(sm_validate(m),
			    "encoder-written NONE chunk must still validate");
			CHECK(sm_cardinality(m) == 8,
			    "encoder-NONE chunk cardinality wrong");
			assert_card_equals_walk(m, "encoder-NONE");
			sm_free(m);
		}
	}

	/* Round-trip: every map the library builds must re-validate true and
	 * stay card==walk consistent, including reduced-capacity sparse
	 * chunks produced by set operations. */
	{
		sm_t *a = sm_create(16384);
		sm_t *bb = sm_create(16384);
		sm_t *r;
		size_t i;
		for (i = 0; i < 2048; i += 200)
			(void)sm_add_grow(&a, i);
		(void)sm_add_range(bb, 800, 2048);
		r = sm_intersection(a, bb);
		CHECK(r != NULL, "intersection failed");
		if (r != NULL) {
			CHECK(sm_validate(r),
			    "encoder set-op output must validate");
			assert_card_equals_walk(r, "set-op-roundtrip");
			sm_free(r);
		}
		sm_free(a);
		sm_free(bb);
	}

	if (failures == 0) {
		printf("test_validate_descriptor: all cases passed\n");
		return (0);
	}
	fprintf(stderr, "test_validate_descriptor: %d failures\n", failures);
	return (1);
}
