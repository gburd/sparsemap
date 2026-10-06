/* SPDX-License-Identifier: MIT */
/*
 * test_open_short.c - sm_open / sm_init with a buffer shorter than the
 * 8-byte chunk-count header.
 *
 * __sm_cap rounds a size of 0..7 down to 0, yet before 5.8.2:
 *   - sm_open still read the 8-byte header (heap-buffer-overflow READ in
 *     __sm_get_chunk_count); for size 0 it left m_data_used = 8, so later
 *     readers trusted a header the caller never supplied, and
 *     sm_wrap(NULL, 0) + sm_open(m, NULL, 0) dereferenced NULL;
 *   - sm_init wrote it (sm_clear -> __sm_set_chunk_count, an 8-byte
 *     heap-buffer-overflow WRITE).
 * Such a buffer is now the empty map: m_data_used 0, nothing read or
 * written, and every reader answers "empty".
 *
 * Each buffer is malloc'd at exactly n bytes so AddressSanitizer flags any
 * access past it, and is filled with a pattern to check nothing inside it
 * is written either.  n = 8 is the smallest real map and must still work.
 */
#define SM_EXPOSE_STRUCT 1
#include <sm.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILL 0xA5

static int failures;

#define CHECK(c)                                                              \
	do {                                                                  \
		if (!(c)) {                                                   \
			fprintf(stderr, "FAIL %s:%d: %s n=%zu: %s\n",        \
			    __FILE__, __LINE__, what, n, #c);                \
			failures++;                                          \
		}                                                            \
	} while (0)

/* Everything a consumer typically does with a map it just opened. */
static void
check_empty(const char *what, sm_t *m, size_t n)
{
	if (n < 8) {
		CHECK(m->m_data_used == 0);
		CHECK(sm_get_capacity(m) == 0);
		/* The empty-map size, but it must not be recorded: there is
		 * no header to trust behind it. */
		CHECK(sm_get_size(m) == 8);
		CHECK(m->m_data_used == 0);
	}
	CHECK(sm_validate(m));
	CHECK(sm_is_empty(m));
	CHECK(sm_cardinality(m) == 0);
	CHECK(!sm_contains(m, 0, NULL));
	CHECK(!sm_contains(m, 5000, NULL));
	CHECK(sm_next_member(m, SM_IDX_MAX, NULL) == SM_IDX_MAX);
	CHECK(sm_prev_member(m, SM_IDX_MAX, NULL) == SM_IDX_MAX);
	CHECK(sm_minimum(m) == 0 && sm_maximum(m) == 0);
	{
		/* Serializes as the empty map and round-trips. */
		uint8_t out[64];
		const size_t need = sm_serialized_size(m);
		sm_t *back;
		CHECK(need <= sizeof(out));
		CHECK(sm_serialize(m, out, need) == need);
		back = sm_deserialize(out, need);
		CHECK(back != NULL && sm_cardinality(back) == 0);
		sm_free(back);
	}
	if (n < 8) {
		/* No room to add anything, and nothing is written trying. */
		errno = 0;
		CHECK(sm_add(m, 5) == SM_IDX_MAX && errno == ENOSPC);
		errno = 0;
		CHECK(sm_add(m, 1u << 20) == SM_IDX_MAX && errno == ENOSPC);
		sm_clear(m);
		CHECK(m->m_data_used == 0 && sm_cardinality(m) == 0);
	}
}

static void
check_untouched(const char *what, const uint8_t *buf, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		CHECK(buf[i] == FILL);
}

int
main(void)
{
	size_t n;

	for (n = 0; n <= 8; n++) {
		const char *what;
		uint8_t *buf = malloc(n);
		sm_t *m;
		sm_t s;
		if (n > 0 && buf == NULL)
			return (2);

		/* sm_wrap + sm_open (n == 8: an all-zero header is the
		 * valid empty map, so it is opened from zeros). */
		what = "sm_open";
		memset(buf, n < 8 ? FILL : 0, n);
		m = sm_wrap(buf, n);
		CHECK(m != NULL);
		sm_open(m, buf, n);
		check_empty(what, m, n);
		if (n < 8)
			check_untouched(what, buf, n);
		sm_free(m);

		/* sm_init on a caller-allocated struct. */
		what = "sm_init";
		memset(buf, FILL, n);
		memset(&s, 0, sizeof(s));
		sm_init(&s, buf, n);
		check_empty(what, &s, n);
		if (n < 8)
			check_untouched(what, buf, n);
		free(buf);
	}

	/* The NULL-buffer forms of the same calls. */
	{
		const char *what = "sm_open(NULL)";
		sm_t *m = sm_wrap(NULL, 0);
		sm_t s;
		n = 0;
		CHECK(m != NULL);
		sm_open(m, NULL, 0);
		check_empty(what, m, n);
		sm_free(m);

		what = "sm_init(NULL)";
		memset(&s, 0, sizeof(s));
		sm_init(&s, NULL, 0);
		check_empty(what, &s, n);
	}

	if (failures != 0) {
		fprintf(stderr, "test_open_short: %d failures\n", failures);
		return (1);
	}
	printf("test_open_short: sizes 0..8 and NULL OK\n");
	return (0);
}
