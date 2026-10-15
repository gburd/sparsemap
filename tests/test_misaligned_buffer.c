/* SPDX-License-Identifier: MIT */
/*
 * test_misaligned_buffer.c - sm_wrap / sm_init / sm_open over a caller
 * buffer that is NOT 8-byte aligned.
 *
 * sm.h documents that a misaligned caller buffer works (with a speed
 * cost on some cpus).  Before 5.8.3 the small-set word array was read
 * and written through a plain `uint64_t *`, which is undefined
 * behaviour on such a buffer: UBSan reports it, and a strict-alignment
 * cpu that does not emulate misaligned access would trap.  Every
 * offset 0..7 must give the same answers as an aligned buffer, through
 * small-set mode, the promote into chunk mode, and an sm_open of the
 * serialized body.  Build with -fsanitize=alignment to make it strict.
 */
#define SM_EXPOSE_STRUCT
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BUFSZ 65536
static _Alignas(16) uint8_t raw_a[BUFSZ + 16];
static _Alignas(16) uint8_t raw_b[BUFSZ + 16];

static int failures;

#define CHECK(c, msg)                                                  \
	do {                                                           \
		if (!(c)) {                                            \
			fprintf(stderr, "FAIL off=%d: %s\n", off, msg);\
			failures++;                                    \
		}                                                      \
	} while (0)

static void
run(int off, size_t want_small, size_t want_chunk)
{
	uint8_t *a = raw_a + off;
	uint8_t *b = raw_b + off;
	sm_t m, m2;
	uint64_t i;
	size_t n;

	sm_init(&m, a, BUFSZ);
	/* Small-set mode: every index below 1024. */
	for (i = 0; i < 900; i += 3)
		CHECK(sm_add(&m, i) == i, "small add");
	CHECK(sm_cardinality(&m) == want_small, "small cardinality");
	CHECK(sm_contains(&m, 300, NULL), "small contains hit");
	CHECK(!sm_contains(&m, 301, NULL), "small contains miss");
	CHECK(sm_minimum(&m) == 0 && sm_maximum(&m) == 897, "small min/max");
	CHECK(sm_next_member(&m, 450, NULL) == 453, "small next_member");
	CHECK(sm_validate(&m), "small validate");
	/* Remove the top bits so the trailing words are trimmed. */
	CHECK(sm_remove(&m, 897) == 897, "small remove");
	CHECK(sm_maximum(&m) == 894, "small max after remove");
	CHECK(sm_add(&m, 897) == 897, "small re-add");

	/* Promote into chunk mode. */
	for (i = 5000; i < 9000; i += 7)
		CHECK(sm_add(&m, i) == i, "chunk add");
	CHECK(sm_cardinality(&m) == want_chunk, "chunk cardinality");
	CHECK(sm_validate(&m), "chunk validate");

	/* Reopen the serialized body over the second misaligned buffer. */
	n = sm_get_size(&m);
	memcpy(b, sm_get_data(&m), n);
	sm_open(&m2, b, BUFSZ);
	CHECK(sm_get_size(&m2) == n, "reopened size");
	CHECK(sm_equals(&m, &m2), "reopened equals");
	CHECK(sm_cardinality(&m2) == want_chunk, "reopened cardinality");

	/* Demote back to small mode and read it through sm_open again. */
	for (i = 5000; i < 9000; i += 7)
		(void)sm_remove(&m, i);
	CHECK(sm_cardinality(&m) == want_small, "demoted cardinality");
	n = sm_get_size(&m);
	memcpy(b, sm_get_data(&m), n);
	sm_open(&m2, b, BUFSZ);
	CHECK(sm_equals(&m, &m2), "demoted reopen equals");
	CHECK(sm_contains(&m2, 300, NULL), "demoted reopen contains");
}

int
main(void)
{
	int off;
	size_t want_small = 300;              /* 0, 3, ..., 897 */
	size_t want_chunk = want_small + 572; /* 5000, 5007, ..., 8997 */

	for (off = 0; off < 8; off++)
		run(off, want_small, want_chunk);
	if (failures != 0) {
		fprintf(stderr, "test_misaligned_buffer: %d failures\n",
		    failures);
		return (1);
	}
	printf("test_misaligned_buffer: OK (offsets 0..7)\n");
	return (0);
}
