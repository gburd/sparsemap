/* SPDX-License-Identifier: MIT */
/*
 * test_large_index.c - regression for the pre-4.0 data-loss bug where
 * any index >= 2^32 was silently dropped (the chunk-start offset and
 * the scan callback both truncated at 32 bits).  Exercises the full
 * 64-bit index space the public API advertises.
 */
#include <sm.h>

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c)                                                              \
	do {                                                                  \
		if (!(c)) {                                                   \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
			    __LINE__, #c);                                   \
			return (1);                                          \
		}                                                            \
	} while (0)

/* A spread of indices straddling and well past 2^32. */
static const uint64_t KEYS[] = {
	0x00000000ffffffffULL, /* 2^32 - 1 (last 32-bit value) */
	0x0000000100000000ULL, /* 2^32 exactly */
	0x0000000100000001ULL,
	0x0000400000000000ULL,
	0x00009c4000003039ULL,
	0xfffffffffffff800ULL, /* near the top of the universe */
};
#define NKEYS (sizeof(KEYS) / sizeof(KEYS[0]))

struct scan_ctx {
	uint64_t seen[NKEYS];
	size_t n;
};

static void
collect(uint64_t vec[], size_t n, void *aux)
{
	struct scan_ctx *c = aux;
	for (size_t i = 0; i < n; i++)
		if (c->n < NKEYS)
			c->seen[c->n++] = vec[i];
}

static int
u64cmp(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x < y ? -1 : x > y ? 1 : 0);
}

int
main(void)
{
	sm_t *m = sm_create(4096);
	CHECK(m != NULL);

	/* add / contains round-trip for every key (the original bug). */
	for (size_t i = 0; i < NKEYS; i++) {
		CHECK(sm_add_grow(&m, KEYS[i]) == KEYS[i]);
		CHECK(sm_contains(m, KEYS[i]));
	}
	CHECK(sm_cardinality(m) == NKEYS);

	/* extents and rank/select at 64-bit magnitudes. */
	uint64_t sorted[NKEYS];
	for (size_t i = 0; i < NKEYS; i++)
		sorted[i] = KEYS[i];
	qsort(sorted, NKEYS, sizeof(sorted[0]), u64cmp);
	CHECK(sm_minimum(m) == sorted[0]);
	CHECK(sm_maximum(m) == sorted[NKEYS - 1]);
	for (size_t i = 0; i < NKEYS; i++) {
		CHECK(sm_select(m, i, true) == sorted[i]);
		CHECK(sm_rank(m, 0, sorted[i], true) == i + 1);
	}

	/* scan must deliver the absolute 64-bit positions (the second
	 * truncation bug: the callback array used to be uint32_t[]). */
	struct scan_ctx ctx = { .n = 0 };
	sm_scan(m, collect, 0, &ctx);
	CHECK(ctx.n == NKEYS);
	for (size_t i = 0; i < NKEYS; i++)
		CHECK(ctx.seen[i] == sorted[i]);

	/* serialize round-trip preserves the 64-bit indices. */
	size_t sz = sm_serialized_size(m);
	uint8_t *buf = malloc(sz);
	CHECK(buf != NULL);
	CHECK(sm_serialize(m, buf, sz) == sz);
	sm_t *back = sm_deserialize(buf, sz);
	CHECK(back != NULL);
	CHECK(sm_equals(m, back));
	for (size_t i = 0; i < NKEYS; i++)
		CHECK(sm_contains(back, KEYS[i]));
	free(buf);
	sm_free(back);

	/* removing a >2^32 bit actually clears it. */
	CHECK(sm_remove(m, KEYS[1]) == KEYS[1]);
	CHECK(!sm_contains(m, KEYS[1]));
	CHECK(sm_cardinality(m) == NKEYS - 1);

	sm_free(m);
	printf("test_large_index: 64-bit index space OK (%zu keys)\n", NKEYS);
	return (0);
}
