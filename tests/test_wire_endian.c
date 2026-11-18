/* SPDX-License-Identifier: MIT */
/*
 * test_wire_endian.c - sm_deserialize reads a stream written on a host of
 * the other byte order.
 *
 * The wire format is host-endian: the magic, the cardinality hint and every
 * body word go out in the writer's byte order, and header byte 5 bit 0 says
 * which.  Through 5.8.5 the reader rejected a stream whose flag differed
 * from the host.  It now byte-swaps each 8-byte body word instead.
 *
 * The test needs no big-endian host.  It serializes a map, rewrites the
 * stream exactly as a writer of the other byte order would have produced it
 * (magic bytes reversed, flag bit flipped, cardinality and every body word
 * byte-swapped), and checks that sm_deserialize returns an sm_equals map.
 * Then it checks that corrupt foreign streams are still rejected and that
 * the writer's output is unchanged.
 */
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

static uint64_t rs = UINT64_C(0x9E3779B97F4A7C15);

static uint64_t
rnd(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return (rs);
}

static uint64_t
bswap64(uint64_t v)
{
	uint64_t r = 0;
	int i;

	for (i = 0; i < 8; i++)
		r = (r << 8) | ((v >> (8 * i)) & 0xFF);
	return (r);
}

static void
swap_word(uint8_t *p)
{
	uint64_t w;

	memcpy(&w, p, 8);
	w = bswap64(w);
	memcpy(p, &w, 8);
}

/* Rewrite a native stream in place as the other byte order writes it. */
static void
to_foreign(uint8_t *s, size_t n)
{
	size_t i;
	uint8_t t;

	t = s[0], s[0] = s[3], s[3] = t;
	t = s[1], s[1] = s[2], s[2] = t;
	s[5] ^= 0x01;
	swap_word(s + 8);
	for (i = 16; i + 8 <= n; i += 8)
		swap_word(s + i);
}

/* Build a map of the given shape.  Returns NULL for the NULL-map shape. */
static sm_t *
make(int shape)
{
	sm_t *m = sm_create(1024);
	uint64_t k, v;

	switch (shape) {
	case 0: /* empty */
		break;
	case 1: /* small set: everything below 1024 */
		for (k = 0; k < 40; k++)
			sm_add_grow(&m, rnd() % 1024);
		break;
	case 2: /* sparse: one member per chunk, gaps between chunks */
		for (k = 0; k < 300; k++)
			sm_add_grow(&m, rnd() % 100000000);
		break;
	case 3: /* clustered, members on chunk boundaries */
		for (k = 0; k < 50; k++) {
			v = (rnd() % 1000) * 2048;
			sm_add_grow(&m, v);
			sm_add_grow(&m, v + 2047);
			sm_add_grow(&m, v + 2048 + rnd() % 64);
		}
		break;
	case 4: /* RLE runs and a mixed tail */
		sm_add_range(m, 4096, 4096 + 100000);
		sm_add_range(m, 1000000, 1000000 + 3000);
		sm_add_grow(&m, 2000000);
		break;
	case 5: /* top of the 64-bit universe */
		sm_add_grow(&m, 3);
		sm_add_grow(&m, UINT64_MAX - 2);
		sm_add_grow(&m, UINT64_MAX - 1);
		break;
	default:
		sm_free(m);
		return (NULL);
	}
	return (m);
}

/* Serialize m (may be NULL) into a fresh buffer. */
static uint8_t *
serialize(const sm_t *m, size_t *np)
{
	size_t n = sm_serialized_size(m);
	uint8_t *s = malloc(n);

	if (s == NULL || sm_serialize(m, s, n) != n) {
		free(s);
		return (NULL);
	}
	*np = n;
	return (s);
}

static int
round_trip(int shape)
{
	sm_t *m = make(shape), *back;
	size_t n, n2;
	uint8_t *s = serialize(m, &n), *s2;

	CHECK(s != NULL);
	/* The writer is unchanged: a second serialize is byte-identical, and
	 * the native stream still reads back. */
	s2 = serialize(m, &n2);
	CHECK(s2 != NULL && n2 == n && memcmp(s, s2, n) == 0);
	back = sm_deserialize(s, n);
	CHECK(back != NULL && sm_equals(back, m));
	sm_free(back);

	/* The foreign-order stream reads back to the same set. */
	to_foreign(s, n);
	CHECK(memcmp(s, s2, n) != 0 || n == 0);
	back = sm_deserialize(s, n);
	CHECK(back != NULL && sm_equals(back, m) && sm_validate(back));
	CHECK(sm_cardinality(back) == (m == NULL ? 0 : sm_cardinality(m)));
	sm_free(back);

	/* Swapping back gives the native bytes again. */
	to_foreign(s, n);
	CHECK(memcmp(s, s2, n) == 0);
	free(s);
	free(s2);
	sm_free(m);
	return (0);
}

/* Foreign streams that must still be rejected. */
static int
corrupt(void)
{
	sm_t *m = make(3);
	size_t n, i;
	uint8_t *s = serialize(m, &n), *t;
	sm_t *back;

	CHECK(s != NULL);
	to_foreign(s, n);
	t = malloc(n + 8);
	CHECK(t != NULL);

	/* The flag says foreign but the magic is in native order. */
	memcpy(t, s, n);
	t[0] = s[3], t[1] = s[2], t[2] = s[1], t[3] = s[0];
	CHECK(sm_deserialize(t, n) == NULL);

	/* The flag flipped back without converting anything. */
	memcpy(t, s, n);
	t[5] ^= 0x01;
	CHECK(sm_deserialize(t, n) == NULL);

	/* A body that is not a whole number of words. */
	memcpy(t, s, n);
	t[n] = 0;
	CHECK(sm_deserialize(t, n + 1) == NULL);
	CHECK(sm_deserialize(t, n - 1) == NULL);

	/* Truncated by a word: the chunk walk no longer tiles the body. */
	CHECK(sm_deserialize(s, n - 8) == NULL);

	/* Wrong version. */
	memcpy(t, s, n);
	t[4] = 3;
	CHECK(sm_deserialize(t, n) == NULL);

	/* A corrupt chunk count in the foreign body (big-endian 2^56+1). */
	memcpy(t, s, n);
	t[16 + 7] ^= 0x01;
	CHECK(sm_deserialize(t, n) == NULL);

	/* Every single-byte flip of the foreign body either is rejected or
	 * decodes to a valid map; none may crash (run under ASan). */
	for (i = 16; i < n; i++) {
		memcpy(t, s, n);
		t[i] ^= (uint8_t)(1u << (i % 8));
		back = sm_deserialize(t, n);
		if (back != NULL) {
			CHECK(sm_validate(back));
			sm_free(back);
		}
	}
	free(t);
	free(s);
	sm_free(m);
	return (0);
}

int
main(void)
{
	int shape, rep;

	for (rep = 0; rep < 20; rep++)
		for (shape = 0; shape <= 6; shape++)
			if (round_trip(shape) != 0) {
				fprintf(stderr, "  shape %d rep %d\n", shape, rep);
				return (1);
			}
	if (corrupt() != 0)
		return (1);
	printf("test_wire_endian: ok\n");
	return (0);
}
