/* SPDX-License-Identifier: MIT */
/*
 * test_hegel_white.c - white-box property tests for sparsemap's chunk
 * codec, using hegel-c.
 *
 * Unlike test_hegel.c (which drives the public API), this file
 * #includes ../sm.c so the properties can reach the file-static chunk
 * primitives __sm_chunk_calc_vector_size, __sm_chunk_get_position,
 * __sm_chunk_get_capacity, and the RLE descriptor helpers.  It is the
 * hegel replacement for the QCC-driven _tst_chunk_* properties that
 * used to live in sm_white.c / test.c.
 *
 * Generators build raw chunk buffers from hegel draws instead of the
 * QCC_genChunk random generator, so failing cases shrink to a minimal
 * descriptor.  The map-level QCC properties (_tst_get_chunk_offset,
 * _tst_rle_select_rank_consistency, _tst_rle_scan_completeness) are
 * covered black-box by test_hegel.c's prop_model / prop_setops, which
 * exercise the same select/rank/scan/navigation paths against a dense
 * bool[] oracle.
 *
 * hegel-c is optional; this target is built only with -Dhegel=enabled
 * (or =auto when the library is present).  See tests/meson.build.
 */
#include "../sm.c"

#include <hegel/generators.h>
#include <hegel/hegel.h>

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Reference popcount-of-MIXED-pairs over a flag byte: count how many
 * of the four 2-bit fields equal SM_PAYLOAD_MIXED (0b10).  This is the
 * independent oracle for __sm_chunk_calc_vector_size.
 */
static int
ref_calc_vector_size(uint8_t b)
{
	int count = 0;
	for (int i = 0; i < 4; i++) {
		if (((b >> (i * 2)) & 0x03) == 0x02)
			count++;
	}
	return (count);
}

/*
 * Property: __sm_chunk_calc_vector_size agrees with the reference for
 * every possible flag byte.  Drawing the full 0..255 range lets hegel
 * shrink a disagreement to the smallest offending byte.
 */
static void
prop_calc_vector_size(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	uint8_t b = (uint8_t)hegel_draw_int(tc, hegel_integers(0, 255));
	assert(__sm_chunk_calc_vector_size(b) == (size_t)ref_calc_vector_size(b));
}

/*
 * Build an RLE chunk buffer (start-offset prefix + single descriptor
 * word) the way the encoder lays one out, with a drawn run length.
 * The buffer's start-offset field is set to the run length so the
 * get_capacity property can recover it.  Caller frees.
 */
static uint8_t *
make_rle_chunk(hegel_test_case *tc)
{
	const int64_t len = hegel_draw_int(tc,
	    hegel_integers(1, SM_CHUNK_RLE_MAX_LENGTH));
	uint8_t *p = malloc(SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2);
	assert(p != NULL);
	__sm_store_idx(p, (__sm_idx_t)len);
	__sm_chunk_t chunk = {
		.m_data = (__sm_bitvec_unaligned_t *)((uintptr_t)p +
		    SM_SIZEOF_OVERHEAD)
	};
	chunk.m_data[0] = 0;
	__sm_chunk_set_rle(&chunk);
	__sm_chunk_rle_set_capacity(&chunk, SM_CHUNK_RLE_MAX_CAPACITY);
	__sm_chunk_rle_set_length(&chunk, (size_t)len);
	assert(__sm_chunk_is_rle(&chunk));
	assert(__sm_chunk_rle_get_length(&chunk) == (size_t)len);
	return (p);
}

/*
 * Build a sparse chunk buffer with `nmixed` MIXED vectors followed by
 * a drawn mix of ONES/ZEROS, with `cut` trailing NONE pairs that
 * reduce capacity.  Each stored MIXED payload word is set to a marker
 * derived from the buffer base so the get_position property can verify
 * the payload pointer arithmetic.  The start-offset field is set to
 * the chunk's resulting capacity so get_capacity can recover it.
 * Caller frees.
 */
static uint8_t *
make_sparse_chunk(hegel_test_case *tc)
{
	const int nmixed = (int)hegel_draw_int(tc,
	    hegel_integers(0, SM_FLAGS_PER_INDEX - 1));
	const int cut = (int)hegel_draw_int(tc,
	    hegel_integers(0, SM_FLAGS_PER_INDEX - nmixed - 1));

	uint8_t *p = malloc(SM_SIZEOF_OVERHEAD +
	    sizeof(__sm_bitvec_t) * (nmixed + 1));
	assert(p != NULL);
	__sm_store_idx(p, (__sm_idx_t)(SM_CHUNK_MAX_CAPACITY -
	    (cut * SM_BITS_PER_VECTOR)));

	__sm_chunk_t chunk = {
		.m_data = (__sm_bitvec_unaligned_t *)((uintptr_t)p +
		    SM_SIZEOF_OVERHEAD)
	};
	__sm_bitvec_unaligned_t *desc = chunk.m_data;
	*desc = 0;

	for (int i = 0; i < nmixed; i++) {
		SM_CHUNK_SET_FLAGS(*desc, i, SM_PAYLOAD_MIXED);
		/* Marker recomputable from the buffer base in the property. */
		chunk.m_data[1 + i] = (uintptr_t)p + i;
	}
	/* Fill the middle with a drawn ONES/ZEROS pattern. */
	for (int i = nmixed; i < SM_FLAGS_PER_INDEX - cut; i++) {
		if (SM_CHUNK_GET_FLAGS(*desc, i) != SM_PAYLOAD_MIXED &&
		    hegel_draw_bool(tc, hegel_booleans())) {
			SM_CHUNK_SET_FLAGS(*desc, i, SM_PAYLOAD_ONES);
		}
	}
	/* Reduce capacity: shift in `cut` trailing NONE pairs. */
	*desc <<= cut * 2;
	for (int i = 0; i < cut; i++)
		SM_CHUNK_SET_FLAGS(*desc, i, SM_PAYLOAD_NONE);

	assert(__sm_chunk_is_rle(&chunk) == false);
	return (p);
}

/*
 * Property: __sm_chunk_get_position returns the running count of
 * MIXED vectors before index i, so that m_data[1 + position] points
 * at the payload word for a MIXED vector (verified against the marker
 * planted by make_sparse_chunk), and for non-MIXED vectors equals the
 * count of MIXED vectors seen so far.  RLE chunks always report 0.
 */
static void
prop_get_position(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	bool rle = hegel_draw_bool(tc, hegel_booleans());
	uint8_t *p = rle ? make_rle_chunk(tc) : make_sparse_chunk(tc);
	__sm_chunk_t chunk = {
		.m_data = (__sm_bitvec_unaligned_t *)((uintptr_t)p +
		    SM_SIZEOF_OVERHEAD)
	};

	if (__sm_chunk_is_rle(&chunk)) {
		for (size_t i = 0; i < SM_FLAGS_PER_INDEX; i++)
			assert(__sm_chunk_get_position(&chunk, i) == 0);
	} else {
		size_t mixed = 0;
		for (size_t i = 0; i < SM_FLAGS_PER_INDEX; i++) {
			size_t pos = __sm_chunk_get_position(&chunk, i);
			switch (SM_CHUNK_GET_FLAGS(*chunk.m_data, i)) {
			case SM_PAYLOAD_MIXED:
				assert(chunk.m_data[1 + pos] ==
				    (uintptr_t)p + pos);
				mixed++;
				break;
			case SM_PAYLOAD_ONES:
			case SM_PAYLOAD_ZEROS:
				assert(pos == mixed);
				break;
			default:
				break;
			}
		}
	}
	free(p);
}

/*
 * Property: the chunk's recoverable capacity matches the value stored
 * in the start-offset prefix by the generator -- run length for RLE
 * chunks, bit capacity for sparse chunks.
 */
static void
prop_get_capacity(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	bool rle = hegel_draw_bool(tc, hegel_booleans());
	uint8_t *p = rle ? make_rle_chunk(tc) : make_sparse_chunk(tc);
	uint64_t want = __sm_load_idx(p);
	__sm_chunk_t chunk = {
		.m_data = (__sm_bitvec_unaligned_t *)((uintptr_t)p +
		    SM_SIZEOF_OVERHEAD)
	};

	if (__sm_chunk_is_rle(&chunk))
		assert(__sm_chunk_rle_get_length(&chunk) == want);
	else
		assert(__sm_chunk_get_capacity(&chunk) == want);
	free(p);
}

static int
run(void (*fn)(hegel_test_case *, void *), const char *name)
{
	hegel_session *s = hegel_session_new();
	if (s == NULL) {
		fprintf(stderr, "hegel: could not start session for %s\n", name);
		return (1);
	}
	hegel_settings settings = HEGEL_DEFAULT_SETTINGS;
	settings.max_examples = 300;
	hegel_results r = hegel_run_test(s, fn, NULL, &settings);
	int ok = r.passed ? 0 : 1;
	if (!ok)
		fprintf(stderr, "hegel white-box property FAILED: %s\n", name);
	hegel_results_free(&r);
	hegel_session_free(s);
	return (ok);
}

int
main(void)
{
	int rc = 0;
	rc |= run(prop_calc_vector_size, "calc_vector_size");
	rc |= run(prop_get_position, "get_position");
	rc |= run(prop_get_capacity, "get_capacity");
	return (rc);
}
