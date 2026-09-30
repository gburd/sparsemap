/* SPDX-License-Identifier: MIT */
/*
 * test_split_safety.c - S3 regression: memory safety in sm_split on
 * maps that PASS validation.
 *
 * Two distinct bugs the mutating fuzzer found on valid-but-adversarial
 * maps, both in sm_split:
 *
 *   1. Source-side over-read.  The "move remaining chunks" loop trusted
 *      `count - i` for how many chunks follow the split point and walked
 *      that many from `src` via __sm_append_data, reading past the
 *      source buffer when the RLE-separation / sparse-split phases left
 *      `i` disagreeing with the bytes actually present (ASan
 *      heap-buffer-overflow READ in memcpy, sm.c:__sm_append_data).
 *
 *   2. Per-bit loop that did not terminate.  Phase (1) set in_middle
 *      whenever `start + capacity > idx`, which is also true when the
 *      chunk begins *after* idx (start > idx).  The sparse-split loop
 *      then ran `for j = idx; j < start + 2048`, i.e. from a low idx up
 *      to a high chunk start -- billions of sm_contains calls.
 *
 * The fixes: bound every chunk walk in the move by the source data end,
 * and gate in_middle on `start <= idx`.  This test replays the recorded
 * hang input and drives sm_split on maps shaped to reach the move loop;
 * without the fixes it over-reads (ASan abort) or hangs.
 */
#define SM_EXPOSE_STRUCT 1
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                              \
	do {                                                                  \
		if (!(c)) {                                                   \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
			    __LINE__, #c);                                   \
			return (1);                                          \
		}                                                            \
	} while (0)

/* The exact 25-byte fuzzer input that hung sm_split's per-bit loop:
 * one chunk whose start is well above the split point the harness uses.
 * Decoded through sm_open, then split at 4096 as the harness does. */
static const uint8_t hang_input[] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x03, 0x00, 0x00, 0x04, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x29, 0x00, 0x22,
	0x00,
};

int
main(void)
{
	/* (2) per-bit non-termination: whatever this decodes to, splitting
	 * it must terminate instantly and not read out of bounds. */
	{
		sm_t *m = sm_open_copy(hang_input, sizeof hang_input, 64);
		if (m != NULL) {
			CHECK(sm_validate(m));
			sm_t *other = sm_create(1 << 16);
			CHECK(other != NULL);
			(void)sm_split(m, 4096, other);
			sm_free(other);
			sm_free(m);
		}
	}

	/* (2) same shape, built explicitly: a chunk that starts ABOVE the
	 * split point.  Pre-fix, in_middle fired and the loop ran from a
	 * small idx up to the high chunk start. */
	{
		sm_t *m = sm_create(1 << 16);
		CHECK(m != NULL);
		/* Only bits in [1<<20, 1<<20 + 100). */
		for (uint64_t i = (1ULL << 20); i < (1ULL << 20) + 100; i++)
			CHECK(sm_add_grow(&m, i) == i);
		CHECK(sm_validate(m));
		sm_t *other = sm_create(1 << 16);
		CHECK(other != NULL);
		/* Split point far below the only chunk. */
		(void)sm_split(m, 5, other);
		/* Everything moved to `other`; the map keeps nothing < 5. */
		CHECK(!sm_contains(m, 1ULL << 20, NULL));
		CHECK(sm_contains(other, 1ULL << 20, NULL));
		CHECK(sm_cardinality(other) == 100);
		sm_free(other);
		sm_free(m);
	}

	/* (1) source-side over-read: a map with several chunks after the
	 * split point, so the move loop walks multiple chunks.  A too-small
	 * destination must give ENOSPC (documented), never over-read. */
	{
		sm_t *m = sm_create(1 << 16);
		CHECK(m != NULL);
		/* Five well-separated single-bit chunks. */
		for (int k = 0; k < 5; k++)
			CHECK(sm_add_grow(&m, (uint64_t)k * 4096 + 1) ==
			    (uint64_t)k * 4096 + 1);
		CHECK(sm_validate(m));

		/* Ample destination: the split succeeds and moves the tail. */
		sm_t *big = sm_create(1 << 16);
		CHECK(big != NULL);
		(void)sm_split(m, 4097, big);
		CHECK(sm_validate(m));
		CHECK(sm_validate(big));
		/* The union of the two halves is the original content. */
		CHECK(sm_cardinality(m) + sm_cardinality(big) == 5);
		sm_free(big);

		/* Tiny destination: must refuse cleanly, not over-read. */
		sm_t *m2 = sm_create(1 << 16);
		CHECK(m2 != NULL);
		for (int k = 0; k < 5; k++)
			CHECK(sm_add_grow(&m2, (uint64_t)k * 4096 + 1) ==
			    (uint64_t)k * 4096 + 1);
		sm_t *tiny = sm_create(8);
		CHECK(tiny != NULL);
		uint64_t rc = sm_split(m2, 4097, tiny);
		(void)rc; /* SM_IDX_MAX / ENOSPC or success; must not crash */
		sm_free(tiny);
		sm_free(m2);
		sm_free(m);
	}

	/*
	 * (3) __sm_separate_rle_chunk shift-UB / heap overflow on a mutated
	 * RLE chunk.  Two flavours the mutating fuzzer hit on maps that pass
	 * sm_open_copy + sm_validate:
	 *
	 *   (3a) A serialized RLE chunk whose run is SHORTER than one chunk
	 *        window (length <= SM_CHUNK_MAX_CAPACITY == 2048) with a
	 *        large capacity.  Toggling a bit inside the run drove the
	 *        left-aligned split to emit an inverted right chunk
	 *        (ex[1].start > ex[1].end) and corrupt the stream, later
	 *        surfacing as an AddressSanitizer negative-size-param in a
	 *        downstream memmove.  This is the exact 251-byte
	 *        reproducer's shape (start=0, length=1024, capacity=25088).
	 *        sm_offset legitimately emits RLE chunks whose run is one
	 *        full window (length == 2048), so such short-run chunks are
	 *        VALID; the left-aligned split must produce a single sparse
	 *        chunk when the whole run fits in the pivot window.
	 *
	 *   (3b) A legal RLE chunk (run spans > 1 window) whose capacity was
	 *        widened far past the run.  Setting a bit beyond the run but
	 *        within that widened capacity landed in a window entirely
	 *        past the run end, where amt_over exceeded one window and
	 *        `~0 >> amt_over/64*2` shifted by >= 64 (UBSan
	 *        "shift exponent ... too large", sm.c:2955/2960) and
	 *        first_zero = MAX_CAPACITY - amt_over underflowed.
	 *
	 * Both must decode, mutate, stay valid, and produce the right set,
	 * never trip a sanitizer.
	 */
	{
		/* (3a) short RLE: length 1024, capacity 25088.  Hand-built
		 * wire = count(1) + start(0) + RLE descriptor.  This is the
		 * serialized shape of the release-qualification reproducer.
		 * It is valid; clearing a bit inside the run must produce the
		 * run minus that bit, in a single sparse chunk, without
		 * corrupting the stream. */
		SM_ALIGNAS(uint64_t) uint8_t wire[24] = { 0 };
		const uint64_t count = 1;
		memcpy(wire, &count, 8); /* chunk count */
		/* start = 0 already zeroed */
		/* RLE descriptor: is-rle flag (bits 63:62 == 01, i.e. bit 62),
		 * length in low 31 bits, capacity in bits [31, 62).
		 * length=1024, capacity=25088. */
		uint64_t desc = (uint64_t)1 << 62; /* SM_RLE_FLAGS */
		desc |= (uint64_t)1024 & 0x7FFFFFFFULL;
		desc |= ((uint64_t)25088 & 0x7FFFFFFFULL) << 31;
		memcpy(wire + 16, &desc, 8);
		sm_t *m = sm_open_copy(wire, sizeof wire, 64);
		CHECK(m != NULL);
		CHECK(sm_validate(m));
		CHECK(sm_cardinality(m) == 1024); /* run [0, 1024) */
		/* Clear a bit inside the run (state==0 left-aligned split). */
		CHECK(sm_remove(m, 30) == 30);
		CHECK(sm_validate(m));
		CHECK(sm_contains(m, 29, NULL));
		CHECK(!sm_contains(m, 30, NULL));
		CHECK(sm_contains(m, 31, NULL));
		CHECK(sm_contains(m, 1023, NULL));
		CHECK(!sm_contains(m, 1024, NULL));
		CHECK(sm_cardinality(m) == 1023);
		/* Set a bit beyond the run but within the widened capacity
		 * (state==1); it lands in a window past the run end. */
		CHECK(sm_add_grow(&m, 20000) == 20000);
		CHECK(sm_validate(m));
		CHECK(sm_contains(m, 20000, NULL));
		CHECK(!sm_contains(m, 19999, NULL));
		CHECK(sm_cardinality(m) == 1024);
		sm_free(m);
	}
	{
		/* (3b) legal long run, widened capacity, set a bit far beyond
		 * the run.  Build a genuine RLE chunk [0, 5000) via the API,
		 * widen its capacity to 200000 in the wire, re-open, then set
		 * bit 100000 (aligned window sits entirely past the run). */
		sm_t *b = sm_create(1 << 16);
		CHECK(b != NULL);
		CHECK(sm_add_range(b, 0, 5000)); /* [0, 5000) -> RLE */
		size_t sz = sm_get_size(b);
		uint8_t *wire = malloc(sz);
		CHECK(wire != NULL);
		memcpy(wire, sm_get_data(b), sz);
		sm_free(b);
		/* widen capacity (bits [31, 62)) to 200000 */
		uint64_t desc;
		memcpy(&desc, wire + 16, 8);
		desc &= ~((uint64_t)0x7FFFFFFF << 31);
		desc |= ((uint64_t)200000 & 0x7FFFFFFFULL) << 31;
		memcpy(wire + 16, &desc, 8);
		sm_t *m = sm_open_copy(wire, sz, 64);
		free(wire);
		CHECK(m != NULL);
		CHECK(sm_validate(m));
		/* Set a bit far beyond the run but within widened capacity. */
		CHECK(sm_add_grow(&m, 100000) == 100000);
		CHECK(sm_validate(m));
		/* Correctness: the run [0,5000) plus bit 100000, nothing in
		 * between. */
		CHECK(sm_contains(m, 0, NULL));
		CHECK(sm_contains(m, 4999, NULL));
		CHECK(!sm_contains(m, 5000, NULL));
		CHECK(!sm_contains(m, 50000, NULL));
		CHECK(sm_contains(m, 100000, NULL));
		CHECK(sm_cardinality(m) == 5001);
		/* Now clear a bit inside the run (state==0 separate). */
		CHECK(sm_remove(m, 30) == 30);
		CHECK(sm_validate(m));
		CHECK(sm_contains(m, 29, NULL));
		CHECK(!sm_contains(m, 30, NULL));
		CHECK(sm_contains(m, 31, NULL));
		CHECK(sm_contains(m, 100000, NULL));
		CHECK(sm_cardinality(m) == 5000);
		sm_free(m);
	}
	{
		/* (3c) sm_add / sm_remove on a SHORT RLE run (capacity below
		 * one window) must be correct and keep the map valid, and the
		 * map must survive sm_split / sm_maximum.  Pre-fix, __sm_map_set
		 * ran its sparse-only "increase capacity" arm on the RLE chunk
		 * (capacity < SM_CHUNK_MAX_CAPACITY), scribbling over the RLE
		 * descriptor: even a NO-OP sm_add of a bit already inside the
		 * run corrupted the chunk (sm_validate -> false), and a
		 * following sm_split read past the heap in sm_maximum.
		 * sm_offset legitimately produces such short-capacity RLE
		 * chunks, so this shape is reachable in normal use.
		 *
		 * Shapes: run length 1/63/64/91/2047/2048 at a couple of bases,
		 * with capacity just above the run (short capacity) and a bit
		 * that is already set, one just past the run, and a clear
		 * inside -- all cross-checked against a brute-force oracle. */
		static const uint64_t lens[] = { 1, 63, 64, 91, 2047, 2048 };
		static const uint64_t bases[] = { 0, 2048, 2048 * 7 };
		for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
			for (size_t bi = 0; bi < sizeof bases / sizeof bases[0];
			     bi++) {
				const uint64_t len = lens[li];
				const uint64_t base = bases[bi];
				/* short capacity: run + a partial window */
				const uint64_t cap = len + 300;
				SM_ALIGNAS(uint64_t) uint8_t wire[24] = { 0 };
				const uint64_t count = 1;
				memcpy(wire, &count, 8);
				memcpy(wire + 8, &base, 8);
				uint64_t desc = (uint64_t)1 << 62; /* RLE */
				desc |= len & 0x7FFFFFFFULL;
				desc |= (cap & 0x7FFFFFFFULL) << 31;
				memcpy(wire + 16, &desc, 8);
				sm_t *m = sm_open_copy(wire, sizeof wire, 64);
				CHECK(m != NULL);
				CHECK(sm_validate(m));
				CHECK(sm_cardinality(m) == len);

				/* NO-OP: re-add a bit already inside the run. */
				const uint64_t inside = base + len / 2;
				CHECK(sm_contains(m, inside, NULL));
				sm_add_grow(&m, inside);
				CHECK(m != NULL);
				CHECK(sm_validate(m));
				CHECK(sm_cardinality(m) == len);
				CHECK(sm_contains(m, inside, NULL));

				/* SET a bit just past the run (within capacity). */
				const uint64_t past = base + len + 10;
				CHECK(past - base < cap);
				sm_add_grow(&m, past);
				CHECK(m != NULL);
				CHECK(sm_validate(m));
				CHECK(sm_contains(m, past, NULL));
				CHECK(sm_cardinality(m) == len + 1);
				/* nothing between the run end and `past` is set */
				CHECK(!sm_contains(m, base + len, NULL));
				if (past > base + len + 1)
					CHECK(!sm_contains(
					    m, base + len + 1, NULL));

				/* CLEAR a bit inside the run. */
				const uint64_t clr = base; /* first run bit */
				sm_remove(m, clr);
				CHECK(sm_validate(m));
				CHECK(!sm_contains(m, clr, NULL));
				CHECK(sm_cardinality(m) == len); /* -1 run +1 past */

				/* Brute-force oracle over the whole capacity span. */
				for (uint64_t i = base; i < base + cap; i++) {
					int want = (i > base && i < base + len)
					    || (i == past);
					CHECK(sm_contains(m, i, NULL) ==
					    (want ? true : false));
				}

				/* Must survive sm_split / sm_maximum, and
				 * round-trip. */
				sm_t *other = sm_create(1 << 16);
				CHECK(other != NULL);
				(void)sm_split(m, base + len / 2, other);
				CHECK(sm_validate(m));
				CHECK(sm_validate(other));
				sm_free(other);

				size_t ssz = sm_serialized_size(m);
				uint8_t *sb = malloc(ssz);
				CHECK(sb != NULL);
				sm_serialize(m, sb, ssz);
				sm_t *r = sm_deserialize(sb, ssz);
				free(sb);
				CHECK(r != NULL);
				CHECK(sm_validate(r));
				sm_free(r);
				sm_free(m);
			}
		}
	}

	/* -----------------------------------------------------------------
	 * BUG2 regression: sm_split of a map with a GAP between two runs
	 * used to emit an INVALID moved half.  A|B = [0,3000) + [6000,8000)
	 * stores the low dense run as an RLE chunk; splitting at 1500 lands
	 * inside that RLE run, so sm_split separates the RLE and recurses.
	 * A header double-count in the RLE separation's knit-back inserted
	 * the expansion one word too far right, half-overwriting the next
	 * chunk's start index -- the moved half had the correct cardinality
	 * but a garbage, unaligned chunk start (sm_validate == 0).  Also
	 * covers the in-gap and everything-moves partitions the same fix
	 * touched.  Pre-fix: sm_validate(o) == 0. --------------------------- */
	{
		sm_t *m = sm_create(1 << 20);
		CHECK(m != NULL);
		for (uint64_t i = 0; i < 3000; i++)
			CHECK(sm_add_grow(&m, i) == i);
		for (uint64_t i = 6000; i < 8000; i++)
			CHECK(sm_add_grow(&m, i) == i);
		CHECK(sm_validate(m));

		sm_t *o = sm_create(1 << 20);
		CHECK(o != NULL);
		CHECK(sm_split(m, 1500, o) != SM_IDX_MAX);

		/* BOTH halves valid. */
		CHECK(sm_validate(m));
		CHECK(sm_validate(o));
		/* Documented partition: m keeps [start, idx), o gets
		 * [idx, end] at the SAME absolute positions. */
		CHECK(sm_cardinality(m) == 1500);  /* [0, 1500) */
		CHECK(sm_cardinality(o) == 3500);  /* [1500,3000) + [6000,8000) */
		for (uint64_t i = 0; i < 8500; i++) {
			bool lo = i < 3000 || (i >= 6000 && i < 8000);
			bool want_m = lo && i < 1500;
			bool want_o = lo && i >= 1500;
			CHECK(sm_contains(m, i, NULL) == want_m);
			CHECK(sm_contains(o, i, NULL) == want_o);
		}
		/* Round-trip: the two halves re-union to the original. */
		sm_t *u = sm_union(m, o);
		CHECK(u != NULL);
		CHECK(sm_cardinality(u) == 5000);
		sm_free(u);
		sm_free(m);
		sm_free(o);
	}

	/* Split at a point that lands in a GAP between two chunks: the run
	 * just below the split must stay in `map`, not move to `other`. */
	{
		sm_t *m = sm_create(1 << 18);
		CHECK(m != NULL);
		/* run in chunk window 1, run in chunk window 6 */
		CHECK(sm_add_range(m, 2100, 3100));
		CHECK(sm_add_range(m, 13000, 14000));
		CHECK(sm_validate(m));
		sm_t *o = sm_create(1 << 18);
		CHECK(o != NULL);
		CHECK(sm_split(m, 5502, o) != SM_IDX_MAX); /* 5502 is in a gap */
		CHECK(sm_validate(m));
		CHECK(sm_validate(o));
		CHECK(sm_cardinality(m) == 1000); /* [2100,3100) stays */
		CHECK(sm_cardinality(o) == 1000); /* [13000,14000) moves */
		CHECK(sm_contains(m, 2100, NULL));
		CHECK(!sm_contains(o, 2100, NULL));
		CHECK(sm_contains(o, 13000, NULL));
		sm_free(m);
		sm_free(o);
	}

	/* -----------------------------------------------------------------
	 * BUG3 regression: removing the sole bit of a length-1 RLE run.
	 * A hand-crafted length-1 RLE chunk passes sm_validate; sm_remove of
	 * its only bit used to set the RLE length to 0, and a length-0 RLE
	 * descriptor reads as a FULL-CAPACITY run -- so the cardinality
	 * jumped to the stored capacity (2048) instead of dropping to 0.
	 * The fix removes the chunk outright when the last bit of a length-1
	 * run is cleared.  Pre-fix: sm_cardinality == 2048 after remove. --- */
	{
		const uint64_t cap = 2048, len = 1;
		uint64_t count = 1, start = 0;
		uint64_t desc = ((uint64_t)1 << 62) /* SM_RLE_FLAGS */
		    | (cap << 31) | len;
		uint8_t body[24];
		memcpy(body + 0, &count, 8);
		memcpy(body + 8, &start, 8);
		memcpy(body + 16, &desc, 8);

		sm_t *m = sm_open_copy(body, sizeof body, 64);
		CHECK(m != NULL);
		CHECK(sm_validate(m));
		CHECK(sm_cardinality(m) == 1);
		CHECK(sm_contains(m, 0, NULL));

		CHECK(sm_remove(m, 0) != SM_IDX_MAX);
		CHECK(sm_cardinality(m) == 0);   /* NOT 2048 */
		CHECK(!sm_contains(m, 0, NULL));
		CHECK(sm_validate(m));
		sm_free(m);
	}

	printf("test_split_safety: S3 sm_split memory safety OK\n");
	return (0);
}
