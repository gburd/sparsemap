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

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compatibility layer over the official hegeldev hegel-c FFI; included
 * after <assert.h> since it redefines assert().  Owns the run-state. */
#define HEGEL_COMPAT_IMPL
#include "hegel_compat.h"

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
 * Build a raw RLE chunk stream (start-offset prefix + single RLE
 * descriptor word) the way the RLE variant lays one out, with a drawn
 * run length and capacity.  This build cannot represent such a chunk;
 * the property below feeds it to sm_deserialize and asserts a clean
 * rejection.  Caller frees.
 */
#define SM_RLE_FLAGS_BIT   0x4000000000000000ULL
#define SM_RLE_LEN_MASK    0x7FFFFFFFULL
#define SM_RLE_CAP_SHIFT   31

static uint8_t *
make_rle_wire(hegel_test_case *tc, size_t *out_len)
{
	const int64_t len = hegel_draw_int(tc, hegel_integers(1, 0x7FFFFFFF));
	int64_t cap = hegel_draw_int(tc, hegel_integers(len, 0x7FFFFFFF));
	/* header: chunk count = 1 */
	const size_t n = SM_SIZEOF_OVERHEAD /* count */
	    + SM_SIZEOF_OVERHEAD             /* chunk start offset */
	    + sizeof(uint64_t);              /* RLE descriptor */
	uint8_t *buf = calloc(1, n);
	assert(buf != NULL);
	uint64_t one = 1;
	memcpy(buf, &one, sizeof(one));
	__sm_idx_t start = 0;
	memcpy(buf + SM_SIZEOF_OVERHEAD, &start, SM_SIZEOF_OVERHEAD);
	const uint64_t desc = SM_RLE_FLAGS_BIT |
	    (((uint64_t)cap << SM_RLE_CAP_SHIFT) &
	        0x3FFFFFFF80000000ULL) |
	    ((uint64_t)len & SM_RLE_LEN_MASK);
	memcpy(buf + SM_SIZEOF_OVERHEAD * 2, &desc, sizeof(desc));
	*out_len = n;
	return (buf);
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
 * count of MIXED vectors seen so far.  Every chunk is sparse here.
 */
static void
prop_get_position(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	uint8_t *p = make_sparse_chunk(tc);
	__sm_chunk_t chunk = {
		.m_data = (__sm_bitvec_unaligned_t *)((uintptr_t)p +
		    SM_SIZEOF_OVERHEAD)
	};

	{
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
 * Property: a sparse chunk's recoverable capacity matches the value
 * stored in the start-offset prefix by the generator.
 */
static void
prop_get_capacity(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	uint8_t *p = make_sparse_chunk(tc);
	uint64_t want = __sm_load_idx(p);
	__sm_chunk_t chunk = {
		.m_data = (__sm_bitvec_unaligned_t *)((uintptr_t)p +
		    SM_SIZEOF_OVERHEAD)
	};

	assert(__sm_chunk_get_capacity(&chunk) == want);
	free(p);
}

/*
 * Property (RLE-free reader decision): a wire stream carrying an RLE
 * descriptor -- which this build cannot represent -- must be rejected
 * cleanly.  sm_deserialize returns NULL or a valid map, and any
 * survivor is RLE-free and passes sm_validate.  Never a crash.
 */
static void
prop_reject_rle_wire(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	size_t body_len = 0;
	uint8_t *body = make_rle_wire(tc, &body_len);

	/* Wrap in the portable header sm_deserialize expects. */
	const size_t hdr = 16;
	uint8_t *wire = calloc(1, hdr + body_len);
	assert(wire != NULL);
	const uint32_t magic = 0x30316d73u; /* "sm10" */
	memcpy(wire, &magic, 4);
	wire[4] = 2;    /* version */
	wire[5] = 0x01; /* little-endian flag */
	memcpy(wire + hdr, body, body_len);

	sm_t *m = sm_deserialize(wire, hdr + body_len);
	if (m != NULL) {
		assert(sm_validate(m));
		sm_free(m);
	}
	free(wire);
	free(body);
}

static int
run(hegel_session *s, void (*fn)(hegel_test_case *, void *), const char *name)
{
	hegel_settings settings = HEGEL_DEFAULT_SETTINGS;
	settings.max_examples = 300;
	hegel_results r = hegel_run_test(s, fn, NULL, &settings);
	int failed = r.passed ? 0 : 1;
	if (failed)
		fprintf(stderr, "hegel white-box property FAILED: %s\n", name);
	hegel_results_free(&r);
	return (failed);
}

int
main(void)
{
	/* One shared session for all properties (see test_hegel.c). */
	hegel_session *s = hegel_session_new();
	if (s == NULL) {
		fprintf(stderr, "hegel: could not start session\n");
		return (1);
	}
	int rc = 0;
	rc |= run(s, prop_calc_vector_size, "calc_vector_size");
	rc |= run(s, prop_get_position, "get_position");
	rc |= run(s, prop_get_capacity, "get_capacity");
	rc |= run(s, prop_reject_rle_wire, "reject_rle_wire");
	hegel_session_free(s);
	return (rc);
}
