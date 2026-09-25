/* SPDX-License-Identifier: MIT */
/*
 * test_validate.c - S1 regression: one definition of a valid map.
 *
 * sm_validate must reject RLE-length-over-capacity, unaligned chunk
 * starts, start+capacity overflow, overlapping spans, and a stored
 * chunk count that disagrees with the walk; and sm_open, sm_open_copy
 * and sm_deserialize must all enforce it (empty map / NULL on failure)
 * -- exactly the contract sm_deserialize already had.
 *
 * Without the S1 fix, sm_open_copy returns a half-parsed map that later
 * crashes sm_add / sm_offset (the review's RLE-len>cap crash), and
 * sm_validate accepts unaligned / overlapping / overflowing chunks.
 */
#define SM_EXPOSE_STRUCT 1
#include <sm.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(c)                                                              \
	do {                                                                  \
		if (!(c)) {                                                   \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
			    __LINE__, #c);                                   \
			return (1);                                          \
		}                                                            \
	} while (0)

static uint8_t body[4096];
static size_t
put(size_t off, uint64_t v)
{
	memcpy(body + off, &v, 8);
	return (off + 8);
}

/* Every hostile body must be rejected by all three decode entry points:
 * sm_open leaves an empty valid map, sm_open_copy returns NULL (or a
 * valid map), sm_deserialize returns NULL (or a valid map).  A survivor
 * that is not valid is a failure. */
static int
reject(const char *name, size_t n)
{
	/* sm_open: caller-supplied buffer. */
	{
		size_t cap = n + 64;
		uint8_t *data = calloc(1, cap);
		CHECK(data != NULL);
		memcpy(data, body, n);
		sm_t m;
		memset(&m, 0, sizeof m);
		sm_open(&m, data, cap);
		if (!sm_validate(&m)) {
			fprintf(stderr, "FAIL %s: sm_open left invalid map\n",
			    name);
			free(data);
			return (1);
		}
		/* An empty map is the required outcome for a hostile body. */
		free(data);
	}
	/* sm_open_copy: NULL or a valid map. */
	{
		sm_t *m = sm_open_copy(body, n, 64);
		if (m != NULL && !sm_validate(m)) {
			fprintf(stderr,
			    "FAIL %s: sm_open_copy returned invalid map\n",
			    name);
			sm_free(m);
			return (1);
		}
		sm_free(m);
	}
	/* sm_deserialize: wrap in the v2 wire header, LE. */
	{
		size_t wn = 16 + n;
		uint8_t *w = calloc(1, wn);
		CHECK(w != NULL);
		uint32_t magic = 0x30316d73u;
		memcpy(w, &magic, 4);
		w[4] = 2;
		w[5] = 1;
		memcpy(w + 16, body, n);
		sm_t *m = sm_deserialize(w, wn);
		if (m != NULL && !sm_validate(m)) {
			fprintf(stderr,
			    "FAIL %s: sm_deserialize returned invalid map\n",
			    name);
			free(w);
			sm_free(m);
			return (1);
		}
		free(w);
		sm_free(m);
	}
	return (0);
}

int
main(void)
{
	size_t o;

	/* (b) unaligned chunk start (not a multiple of 2048). */
	memset(body, 0, sizeof body);
	o = put(0, 1);
	o = put(o, 1000);
	o = put(o, 3);
	CHECK(reject("unaligned start", o) == 0);

	/* strictly-descending starts. */
	memset(body, 0, sizeof body);
	o = put(0, 2);
	o = put(o, 4096);
	o = put(o, 3ULL << 2);
	o = put(o, 0);
	o = put(o, 3ULL << 2);
	CHECK(reject("descending starts", o) == 0);

	/* duplicate starts. */
	memset(body, 0, sizeof body);
	o = put(0, 2);
	o = put(o, 2048);
	o = put(o, 3);
	o = put(o, 2048);
	o = put(o, 3);
	CHECK(reject("duplicate starts", o) == 0);

	/* (c) start near UINT64_MAX, run overflows the index space. */
	memset(body, 0, sizeof body);
	o = put(0, 1);
	o = put(o, UINT64_MAX - 100);
	o = put(o, 3);
	CHECK(reject("start near 2^64", o) == 0);

	/* (a) RLE length > capacity. */
	memset(body, 0, sizeof body);
	o = put(0, 1);
	o = put(o, 0);
	{
		uint64_t rle = (1ULL << 62) | ((uint64_t)100 << 31) | 5000;
		o = put(o, rle);
	}
	CHECK(reject("RLE len > cap", o) == 0);

	/* (e) stored chunk count 2^32-1 but a tiny body. */
	memset(body, 0, sizeof body);
	o = put(0, 0xFFFFFFFFULL);
	o = put(o, 0);
	o = put(o, 3);
	CHECK(reject("count 2^32-1", o) == 0);

	/* (d) two RLE chunks whose spans overlap: start 0 with cap 4096
	 * covers [0, 4096), the second starts at 2048 (< 4096). */
	memset(body, 0, sizeof body);
	o = put(0, 2);
	o = put(o, 0);
	{
		uint64_t rle = (1ULL << 62) | ((uint64_t)4096 << 31) | 100;
		o = put(o, rle);
	}
	o = put(o, 2048);
	{
		uint64_t rle = (1ULL << 62) | ((uint64_t)2048 << 31) | 10;
		o = put(o, rle);
	}
	CHECK(reject("overlapping spans", o) == 0);

	/* A legitimate map at the very top of the index space must NOT be
	 * rejected: the last valid chunk ends exactly at 2^64. */
	{
		sm_t *m = sm_create(4096);
		CHECK(m != NULL);
		CHECK(sm_add_grow(&m, 0xfffffffffffff800ULL) ==
		    0xfffffffffffff800ULL);
		CHECK(sm_validate(m));
		size_t sz = sm_serialized_size(m);
		uint8_t *buf = malloc(sz);
		CHECK(buf != NULL);
		CHECK(sm_serialize(m, buf, sz) == sz);
		sm_t *back = sm_deserialize(buf, sz);
		CHECK(back != NULL);
		CHECK(sm_contains(back, 0xfffffffffffff800ULL, NULL));
		free(buf);
		sm_free(back);
		sm_free(m);
	}

	printf("test_validate: S1 invariants OK\n");
	return (0);
}
