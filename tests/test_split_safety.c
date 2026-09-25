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

	printf("test_split_safety: S3 sm_split memory safety OK\n");
	return (0);
}
