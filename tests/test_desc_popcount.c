/* SPDX-License-Identifier: MIT */
/*
 * test_desc_popcount.c - the chunk descriptor helpers computed by popcount
 * (5.9.0) agree with the byte-table loops they replaced.
 *
 * __sm_chunk_get_position, __sm_chunk_get_capacity and __sm_chunk_get_size
 * used to decode the thirty-two 2-bit flags of a sparse descriptor one
 * byte (through a 256-entry table) or one flag at a time.  They now count
 * MIXED and NONE flags with one masked popcount each.  This file keeps the
 * v5.8.5 loops verbatim as the oracle and compares all three helpers on:
 *
 *   1. every descriptor built from all 4^k flag patterns over the low k
 *      slots (k <= 10), with the remaining slots ZEROS, then ONES;
 *   2. two million random descriptors (uniform and flag-biased);
 *   3. the four all-same descriptors;
 *   4. every bv in 0..32 for each of those (bv >= 32 must return 0).
 *
 * It also checks __sm_word_select (the in-word select behind
 * __sm_chunk_select) against a bit-by-bit scan; the meson build compiles
 * this file a second time with -mbmi2 so both the pdep and the portable
 * form run.
 *
 * In the RLE-free variant every descriptor is sparse, including one whose
 * bits 63:62 are 01 (the RLE variant's marker; here it is just an unused
 * top slot), so the oracle covers every descriptor.
 *
 * Like test_hegel_white.c this includes ../sm.c to reach the file-static
 * helpers.
 */
#include "../sm.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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

/* The v5.8.5 table: count of MIXED (2#10) flags in a flag byte. */
static size_t
old_calc_vector_size(const uint8_t b)
{
	/* clang-format off */
	static const size_t lookup[] = {
	  0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
	  0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
	  1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
	  0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
	  0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
	  0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
	  1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
	  0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
	  1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
	  1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
	  2,  2,  3,  2,  2,  2,  3,  2,  3,  3,  4,  3,  2,  2,  3,  2,
	  1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
	  0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
	  0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
	  1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
	  0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0
	};
	/* clang-format on */
	return (lookup[b]);
}

/* v5.8.5 __sm_chunk_get_position, sparse path. */
static size_t
old_position(const __sm_bitvec_t desc, size_t bv)
{
	size_t position = 0;
	size_t num_bytes;
	size_t i;

	if (bv >= SM_FLAGS_PER_INDEX)
		return (0);
	num_bytes = bv / ((size_t)SM_FLAGS_PER_INDEX_BYTE * SM_BITS_PER_VECTOR);
	for (i = 0; i < num_bytes; i++)
		position += old_calc_vector_size(__sm_desc_flag_byte(desc, i));
	bv -= num_bytes * SM_FLAGS_PER_INDEX_BYTE;
	for (i = 0; i < bv; i++) {
		if (SM_CHUNK_GET_FLAGS(desc, i) == SM_PAYLOAD_MIXED)
			position++;
	}
	return (position);
}

/* v5.8.5 __sm_chunk_get_capacity, sparse path. */
static size_t
old_capacity(const __sm_bitvec_t desc)
{
	size_t capacity = SM_CHUNK_MAX_CAPACITY;
	size_t i;

	for (i = 0; i < sizeof(__sm_bitvec_t); i++) {
		const uint8_t b = __sm_desc_flag_byte(desc, i);
		int j;
		if (!b || b == 0xff)
			continue;
		for (j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
			if (SM_CHUNK_GET_FLAGS(b, j) == SM_PAYLOAD_NONE)
				capacity -= SM_BITS_PER_VECTOR;
		}
	}
	return (capacity);
}

/* v5.8.5 __sm_chunk_get_size, sparse path. */
static size_t
old_size(const __sm_bitvec_t desc)
{
	size_t size = sizeof(__sm_bitvec_t);
	size_t i;

	for (i = 0; i < sizeof(__sm_bitvec_t); i++)
		size += sizeof(__sm_bitvec_t) *
		    old_calc_vector_size(__sm_desc_flag_byte(desc, i));
	return (size);
}

/* Compare all three helpers on one descriptor, for every bv in 0..32. */
static void
check_desc(__sm_bitvec_t desc)
{
	__sm_bitvec_t word = desc;
	__sm_chunk_t chunk;
	size_t bv;

	chunk.m_data = &word;
	checks++;
	if (__sm_chunk_get_capacity(&chunk) != old_capacity(desc)) {
		failures++;
		fprintf(stderr, "FAIL capacity desc=%016llx: %zu want %zu\n",
		    (unsigned long long)desc, __sm_chunk_get_capacity(&chunk),
		    old_capacity(desc));
	}
	if (__sm_chunk_get_size(&chunk) != old_size(desc)) {
		failures++;
		fprintf(stderr, "FAIL size desc=%016llx: %zu want %zu\n",
		    (unsigned long long)desc, __sm_chunk_get_size(&chunk),
		    old_size(desc));
	}
	for (bv = 0; bv <= SM_FLAGS_PER_INDEX; bv++) {
		if (__sm_chunk_get_position(&chunk, bv) !=
		    old_position(desc, bv)) {
			failures++;
			fprintf(stderr,
			    "FAIL position desc=%016llx bv=%zu: %zu want %zu\n",
			    (unsigned long long)desc, bv,
			    __sm_chunk_get_position(&chunk, bv),
			    old_position(desc, bv));
		}
	}
}

/* All 4^k patterns over the low k slots, upper slots filled with `fill`. */
static void
exhaustive_low_slots(unsigned k, unsigned fill)
{
	const uint64_t n = UINT64_C(1) << (2 * k);
	__sm_bitvec_t upper = 0;
	unsigned s;
	uint64_t p;

	for (s = k; s < SM_FLAGS_PER_INDEX; s++)
		SM_CHUNK_SET_FLAGS(upper, s, fill);
	for (p = 0; p < n; p++)
		check_desc(upper | (__sm_bitvec_t)p);
}

/* A random descriptor whose flags are drawn from a biased distribution. */
static __sm_bitvec_t
biased_desc(void)
{
	__sm_bitvec_t d = 0;
	const unsigned favourite = (unsigned)(rnd() & 3);
	unsigned s;

	for (s = 0; s < SM_FLAGS_PER_INDEX; s++) {
		const unsigned f = (rnd() % 4 == 0) ? (unsigned)(rnd() & 3)
		                                     : favourite;
		SM_CHUNK_SET_FLAGS(d, s, f);
	}
	return (d);
}

/* __sm_word_select against a bit-by-bit scan, for every n < popcount. */
static void
check_word_select(__sm_bitvec_t w)
{
	size_t n = 0;
	size_t b;

	for (b = 0; b < 64; b++) {
		if (((w >> b) & 1) == 0)
			continue;
		checks++;
		if (__sm_word_select(w, n) != b) {
			failures++;
			fprintf(stderr, "FAIL word_select w=%016llx n=%zu: %zu "
			    "want %zu\n", (unsigned long long)w, n,
			    __sm_word_select(w, n), b);
		}
		n++;
	}
}

int
main(void)
{
	unsigned k;
	unsigned f;
	long i;

#if defined(SM_TEST_REQUIRE_BMI2)
	/* Built with -mbmi2: running it on a cpu without BMI2 would trap. */
	if (!__builtin_cpu_supports("bmi2")) {
		printf("test_desc_popcount: skipped, no BMI2\n");
		return (77);
	}
#endif
	/* 1. Exhaustive low slots.  k = 10 is 2^20 patterns per fill. */
	for (k = 1; k <= 10; k++) {
		exhaustive_low_slots(k, SM_PAYLOAD_ZEROS);
		exhaustive_low_slots(k, SM_PAYLOAD_ONES);
	}
	/* The same over the HIGH slots, including slot 31. */
	for (i = 0; i < (1L << 20); i++)
		check_desc((__sm_bitvec_t)i << 44);

	/* 2. Random descriptors. */
	for (i = 0; i < 1000000; i++) {
		check_desc(rnd());
		check_desc(biased_desc());
	}

	/* 3. All-same descriptors. */
	for (f = 0; f < 4; f++) {
		__sm_bitvec_t d = 0;
		unsigned s;
		for (s = 0; s < SM_FLAGS_PER_INDEX; s++)
			SM_CHUNK_SET_FLAGS(d, s, f);
		check_desc(d);
	}

	/* The in-word select on edge words and random words. */
	check_word_select(1);
	check_word_select(UINT64_C(1) << 63);
	check_word_select(~(__sm_bitvec_t)0);
	check_word_select(UINT64_C(0x8000000000000001));
	for (i = 0; i < 200000; i++) {
		check_word_select(rnd());
		check_word_select(rnd() & rnd() & rnd());
		check_word_select(rnd() | rnd() | rnd());
	}

	if (failures != 0) {
		fprintf(stderr, "test_desc_popcount: %ld of %ld checks failed\n",
		    failures, checks);
		return (1);
	}
#if defined(SM_HAVE_PDEP64)
	printf("test_desc_popcount: ok (%ld checks, pdep)\n", checks);
#else
	printf("test_desc_popcount: ok (%ld checks, portable)\n", checks);
#endif
	return (0);
}
