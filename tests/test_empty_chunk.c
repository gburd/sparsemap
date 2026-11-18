/* SPDX-License-Identifier: MIT */
/*
 * test_empty_chunk.c - no map holds a chunk without a member (R6).
 *
 * Through 5.8.5 sm_split always added the middle chunk to `other`, even
 * when that chunk had no member at or above the split index, so `other`
 * kept an empty chunk (descriptor 0).  sm_validate accepted it and
 * sm_minimum(other) returned that chunk's edge (0) instead of the first
 * member.  sm_split now drops the empty copy, and sm_validate rejects an
 * empty sparse chunk and a length-0 RLE chunk.
 *
 * A 5.8.5 stream can hold such a chunk, so the readers (sm_open,
 * sm_open_copy, sm_deserialize) drop empty chunks before validating:
 * they hold no member, so the set is unchanged and the stream still
 * reads.
 *
 * Checks:
 *   1. the review's case, then a sweep of splits inside a chunk's window
 *      past its last member, against a sorted-array oracle, with
 *      sm_validate, cardinality, sm_minimum, sm_maximum and full
 *      iteration on both halves;
 *   2. sm_validate rejects a crafted empty sparse chunk and a crafted
 *      length-0 RLE chunk;
 *   3. each reader accepts a body with an empty chunk first, between two
 *      chunks, and last, and yields exactly the other chunks' members.
 */
#define SM_EXPOSE_STRUCT 1
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(c, ...)                                                       \
	do {                                                                \
		if (!(c)) {                                                 \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                       \
			fprintf(stderr, "\n");                              \
			failures++;                                         \
		}                                                           \
	} while (0)

/* 1 when m holds exactly v[0..n), ascending. */
static int
holds(sm_t *m, const uint64_t *v, size_t n, const char *what)
{
	uint64_t x = SM_IDX_MAX;
	size_t i;

	if (!sm_validate(m)) {
		CHECK(0, "%s: sm_validate", what);
		return (0);
	}
	if (sm_cardinality(m) != n) {
		CHECK(0, "%s: cardinality %zu want %zu", what,
		    sm_cardinality(m), n);
		return (0);
	}
	if (n > 0 && (sm_minimum(m) != v[0] || sm_maximum(m) != v[n - 1])) {
		CHECK(0, "%s: min %llu max %llu want %llu %llu", what,
		    (unsigned long long)sm_minimum(m),
		    (unsigned long long)sm_maximum(m), (unsigned long long)v[0],
		    (unsigned long long)v[n - 1]);
		return (0);
	}
	for (i = 0; i < n; i++) {
		x = sm_next_member(m, x, NULL);
		if (x != v[i]) {
			CHECK(0, "%s: member %zu is %llu want %llu", what, i,
			    (unsigned long long)x, (unsigned long long)v[i]);
			return (0);
		}
	}
	CHECK(sm_next_member(m, x, NULL) == SM_IDX_MAX, "%s: extra member",
	    what);
	return (1);
}

/* Split {a, b, c} at `at` and check both halves. */
static void
split_case(uint64_t a, uint64_t b, uint64_t c, uint64_t at)
{
	const uint64_t v[3] = { a, b, c };
	sm_t *m = sm_create(4096), *o = sm_create(4096);
	size_t lo = 0;
	char what[96];

	sm_add(m, a);
	sm_add(m, b);
	sm_add(m, c);
	while (lo < 3 && v[lo] < at)
		lo++;
	snprintf(what, sizeof(what), "{%llu, %llu, %llu} split at %llu",
	    (unsigned long long)a, (unsigned long long)b,
	    (unsigned long long)c, (unsigned long long)at);
	CHECK(sm_split(m, at, o) == at, "%s: return", what);
	holds(m, v, lo, what);
	holds(o, v + lo, 3 - lo, what);
	sm_free(m);
	sm_free(o);
}

static void
splits(void)
{
	static const uint64_t bases[] = { 0, 2048, 4096, UINT64_C(1) << 40 };
	static const uint64_t offs[] = { 1, 63, 64, 65, 1000, 2047 };
	size_t bi, oi;

	/* The review's case. */
	split_case(4100, 4110, 13096, 4111);
	for (bi = 0; bi < sizeof(bases) / sizeof(bases[0]); bi++) {
		for (oi = 0; oi < sizeof(offs) / sizeof(offs[0]); oi++) {
			const uint64_t w = bases[bi];
			/* Split past the window's last member, inside the
			 * window: `other` gets no bit of this chunk. */
			split_case(w + 4, w + 14, w + 9000, w + 14 + offs[oi]);
			/* Split inside the window with a member above. */
			if (14 + offs[oi] < 2047)
				split_case(w + 4, w + 2047, w + 9000,
				    w + 14 + offs[oi]);
		}
	}
}

/* A chunk-mode body: count, then (start, descriptor[, payload]). */
static uint64_t body[16];

static size_t
mk(int empty_at, int rle)
{
	/* Members 4096 + 5 and 12288 + 7, plus an empty chunk at 8192
	 * placed first, between, or last (empty_at 0, 1, 2). */
	const uint64_t empty_start = empty_at == 0 ? 0 :
	    empty_at == 1 ? 8192 : 20480;
	/* RLE: flag 01 in bits 63:62, capacity 2048 in bits 61:31, and a
	 * length-0 run. */
	const uint64_t empty_desc =
	    rle ? (UINT64_C(1) << 62) | (UINT64_C(2048) << 31) : 0;
	size_t n = 1;

	body[0] = 3;
	if (empty_at == 0) {
		body[n++] = empty_start;
		body[n++] = empty_desc;
	}
	body[n++] = 4096;
	body[n++] = 2; /* slot 0 MIXED */
	body[n++] = UINT64_C(1) << 5;
	if (empty_at == 1) {
		body[n++] = empty_start;
		body[n++] = empty_desc;
	}
	body[n++] = 12288;
	body[n++] = 2;
	body[n++] = UINT64_C(1) << 7;
	if (empty_at == 2) {
		body[n++] = empty_start;
		body[n++] = empty_desc;
	}
	return (n * sizeof(uint64_t));
}

static void
readers(void)
{
	static const uint64_t v[2] = { 4096 + 5, 12288 + 7 };
	int at, rle;

	for (rle = 0; rle < 2; rle++) {
		for (at = 0; at < 3; at++) {
			const size_t n = mk(at, rle);
			uint8_t *buf = calloc(1, n + 64);
			uint8_t *wire = calloc(1, n + 16);
			const uint32_t magic = 0x30316d73u;
			sm_t om, *m;
			char what[64];

			if (buf == NULL || wire == NULL)
				exit(2);
			snprintf(what, sizeof(what), "%s empty chunk at %d",
			    rle ? "RLE" : "sparse", at);

			/* The stream as written, with its empty chunk, does
			 * not validate. */
			sm_init(&om, buf, n + 64);
			memcpy(buf, body, n);
			om.m_data_used = n;
			CHECK(!sm_validate(&om), "%s: validate accepts it",
			    what);

			sm_open(&om, buf, n + 64);
			holds(&om, v, 2, what);

			m = sm_open_copy((const uint8_t *)body, n, 64);
			CHECK(m != NULL, "%s: sm_open_copy NULL", what);
			if (m != NULL)
				holds(m, v, 2, what);
			sm_free(m);

			memcpy(wire, &magic, 4);
			wire[4] = 2;
			wire[5] = 1;
			memcpy(wire + 16, body, n);
			m = sm_deserialize(wire, n + 16);
			CHECK(m != NULL, "%s: sm_deserialize NULL", what);
			if (m != NULL)
				holds(m, v, 2, what);
			sm_free(m);
			free(buf);
			free(wire);
		}
	}
}

int
main(void)
{
	splits();
	readers();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return (1);
	}
	printf("test_empty_chunk: ok\n");
	return (0);
}
