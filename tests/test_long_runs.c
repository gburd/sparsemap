/* SPDX-License-Identifier: MIT */
/*
 * test_long_runs.c - runs of 2^31 bits or more (R31).
 *
 * An RLE chunk stores its run length and capacity in 31-bit fields.
 * Through 5.8.5 two writers stored a longer value without splitting it,
 * and the field kept only the low 31 bits while sm_validate still
 * passed: __sm_emit_rle (behind sm_add_range, sm_create_from_range,
 * sm_xor, sm_extract_range, sm_add_many and the set ops' RLE paths), so
 * sm_add_range(m, 0, 2^31) gave cardinality 0; and sm_add extending an
 * RLE run one bit at a time, which grew the capacity to 2^31 - 1 and
 * then wrote length 2^31 as 0, losing the whole run.  The same
 * unaligned capacity let a later sm_add start a chunk inside the run.
 *
 * Every map here is a handful of RLE chunks, so the checks are run-level
 * rather than per bit: for each expected run [lo, hi), sm_rank over it is
 * hi - lo, the bits just outside it are clear, the gaps between runs rank
 * 0, sm_select finds each run's first bit, and the total matches.  Run
 * lengths are 2^31, 2^31 + 4096, 2^32 and 1e10, at aligned and unaligned
 * starts, through every writer that can emit a long run.
 */
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

#define P31 (UINT64_C(1) << 31)
#define BODY UINT64_C(0x7FFFF800) /* largest chunk-aligned RLE length */

static const uint64_t lens[] = { P31, P31 + 4096, UINT64_C(1) << 32,
	UINT64_C(10000000000) };
#define NLENS (sizeof(lens) / sizeof(lens[0]))

struct run {
	uint64_t lo, hi; /* [lo, hi), ascending and not touching */
};

/* Set bits in [lo, hi), or 0 for an empty range. */
static uint64_t
count(sm_t *m, uint64_t lo, uint64_t hi)
{
	return (lo >= hi ? 0 : (uint64_t)sm_rank(m, lo, hi - 1, true));
}

/*
 * 0 when m holds exactly the runs r[0..n).  Prints `what` and the first
 * mismatch otherwise.
 */
static int
runs_are(sm_t *m, const struct run *r, size_t n, const char *what)
{
	uint64_t total = 0, prev_hi = 0;
	size_t k;

	if (m == NULL) {
		fprintf(stderr, "  %s: NULL map\n", what);
		return (1);
	}
	if (!sm_validate(m)) {
		fprintf(stderr, "  %s: sm_validate false\n", what);
		return (1);
	}
	for (k = 0; k < n; k++) {
		const uint64_t len = r[k].hi - r[k].lo;

		if (count(m, prev_hi, r[k].lo) != 0) {
			fprintf(stderr, "  %s: bits before run %zu\n", what, k);
			return (1);
		}
		if (count(m, r[k].lo, r[k].hi) != len) {
			fprintf(stderr, "  %s: run %zu [%llu, %llu) ranks %llu\n",
			    what, k, (unsigned long long)r[k].lo,
			    (unsigned long long)r[k].hi,
			    (unsigned long long)count(m, r[k].lo, r[k].hi));
			return (1);
		}
		if (!sm_contains(m, r[k].lo, NULL) ||
		    !sm_contains(m, r[k].hi - 1, NULL) ||
		    sm_contains(m, r[k].hi, NULL) ||
		    (r[k].lo > 0 && sm_contains(m, r[k].lo - 1, NULL))) {
			fprintf(stderr, "  %s: run %zu edges\n", what, k);
			return (1);
		}
		if (sm_select(m, total, true) != r[k].lo ||
		    sm_select(m, total + len - 1, true) != r[k].hi - 1) {
			fprintf(stderr, "  %s: select into run %zu\n", what, k);
			return (1);
		}
		total += len;
		prev_hi = r[k].hi;
	}
	if (count(m, prev_hi, UINT64_MAX) != 0 ||
	    (uint64_t)sm_cardinality(m) != total) {
		fprintf(stderr, "  %s: card %llu want %llu\n", what,
		    (unsigned long long)sm_cardinality(m),
		    (unsigned long long)total);
		return (1);
	}
	return (0);
}

static sm_t *
range_map(uint64_t lo, uint64_t hi)
{
	sm_t *m = sm_create(1024);

	if (m != NULL && !sm_add_range(m, lo, hi)) {
		sm_free(m);
		return (NULL);
	}
	return (m);
}

/* sm_add_range and sm_create_from_range at aligned and unaligned starts. */
static void
add_range(void)
{
	static const uint64_t los[] = { 0, 4096, 5 };
	size_t i, j;

	for (i = 0; i < NLENS; i++) {
		for (j = 0; j < 3; j++) {
			const struct run r = { los[j], los[j] + lens[i] };
			sm_t *m = range_map(r.lo, r.hi);

			CHECK(runs_are(m, &r, 1, "add_range") == 0,
			    "sm_add_range(%llu, +%llu)",
			    (unsigned long long)r.lo,
			    (unsigned long long)lens[i]);
			sm_free(m);
			m = sm_create_from_range(r.lo, r.hi);
			CHECK(runs_are(m, &r, 1, "create_from_range") == 0,
			    "sm_create_from_range(%llu, +%llu)",
			    (unsigned long long)r.lo,
			    (unsigned long long)lens[i]);
			sm_free(m);
		}
	}
}

/* sm_xor, sm_extract_range, sm_or, sm_and, sm_andnot, the in-place
 * union and a wire round trip over long runs. */
static void
set_ops(void)
{
	size_t i;

	for (i = 0; i < NLENS; i++) {
		const uint64_t L = lens[i], h = L / 2;
		sm_t *a = range_map(0, h), *b = range_map(h, L);
		sm_t *full = range_map(0, L), *mid = range_map(1000, L - 1000);
		sm_t *r;
		const struct run all = { 0, L };
		const struct run inner = { 1000, L - 1000 };
		const struct run ends[2] = { { 0, 1000 }, { L - 1000, L } };
		uint8_t *buf;
		size_t len;

		r = sm_xor(a, b);
		CHECK(runs_are(r, &all, 1, "xor halves") == 0, "xor %zu", i);
		sm_free(r);
		r = sm_or(a, b);
		CHECK(runs_are(r, &all, 1, "or halves") == 0, "or %zu", i);
		sm_free(r);
		r = sm_copy(a);
		r = sm_union_inplace(r, b);
		CHECK(runs_are(r, &all, 1, "union_inplace") == 0,
		    "union_inplace %zu", i);
		sm_free(r);
		r = sm_and(full, mid);
		CHECK(runs_are(r, &inner, 1, "and") == 0, "and %zu", i);
		sm_free(r);
		r = sm_andnot(full, mid);
		CHECK(runs_are(r, ends, 2, "andnot") == 0, "andnot %zu", i);
		sm_free(r);
		r = sm_xor(full, mid);
		CHECK(runs_are(r, ends, 2, "xor inner") == 0, "xor inner %zu",
		    i);
		sm_free(r);
		r = sm_extract_range(full, 1000, L - 1000);
		CHECK(runs_are(r, &inner, 1, "extract_range") == 0,
		    "extract_range %zu", i);
		sm_free(r);
		len = sm_serialized_size(full);
		buf = malloc(len);
		CHECK(buf != NULL && sm_serialize(full, buf, len) == len,
		    "serialize %zu", i);
		r = buf != NULL ? sm_deserialize(buf, len) : NULL;
		CHECK(runs_are(r, &all, 1, "deserialize") == 0,
		    "deserialize %zu", i);
		sm_free(r);
		free(buf);
		sm_free(a);
		sm_free(b);
		sm_free(full);
		sm_free(mid);
	}
}

/* sm_add_many merging new members into a long run, sm_add joining two
 * long runs (the coalesce path), sm_remove inside one, sm_offset, and
 * sm_flip_range across the seam between two RLE pieces. */
static void
mutations(void)
{
	size_t i;

	for (i = 0; i < NLENS; i++) {
		const uint64_t L = lens[i], a = L / 2 + 3;
		const struct run all = { 0, L };
		sm_t *m = range_map(0, L - 2), *o;
		const uint64_t tail[2] = { L - 2, L - 1 };

		CHECK(m != NULL && sm_add_many(m, tail, 2), "add_many %zu", i);
		CHECK(runs_are(m, &all, 1, "add_many") == 0, "add_many %zu",
		    i);
		sm_free(m);

		m = range_map(0, a);
		CHECK(m != NULL && sm_add_range(m, a + 1, L), "join %zu", i);
		CHECK(m != NULL && sm_add(m, a) == a, "join add %zu", i);
		CHECK(runs_are(m, &all, 1, "join") == 0, "join %zu", i);

		CHECK(sm_remove(m, a) == a, "remove %zu", i);
		{
			const struct run two[2] = { { 0, a }, { a + 1, L } };

			CHECK(runs_are(m, two, 2, "remove") == 0, "remove %zu",
			    i);
		}
		sm_free(m);

		m = range_map(4096, 4096 + L);
		o = sm_offset(m, 5);
		{
			const struct run up = { 4101, 4101 + L };

			CHECK(runs_are(o, &up, 1, "offset +5") == 0,
			    "offset +5 %zu", i);
		}
		sm_free(o);
		o = sm_offset(m, 2048);
		{
			const struct run up = { 6144, 6144 + L };

			CHECK(runs_are(o, &up, 1, "offset +2048") == 0,
			    "offset +2048 %zu", i);
		}
		sm_free(o);
		o = sm_offset(m, -5000);
		{
			const struct run down = { 0, L - 904 };

			CHECK(runs_are(o, &down, 1, "offset -5000") == 0,
			    "offset -5000 %zu", i);
		}
		sm_free(o);
		sm_free(m);

		m = range_map(0, L);
		CHECK(m != NULL && sm_flip_range(m, BODY - 10, BODY + 10),
		    "flip %zu", i);
		{
			const struct run hole[2] = { { 0, BODY - 10 },
				{ BODY + 10, L } };

			CHECK(runs_are(m, hole, 2, "flip") == 0, "flip %zu", i);
		}
		CHECK(m != NULL && sm_flip_range(m, BODY - 10, BODY + 10),
		    "flip back %zu", i);
		CHECK(runs_are(m, &all, 1, "flip back") == 0, "flip back %zu",
		    i);
		sm_free(m);
	}
}

/* sm_add growing an RLE run one bit at a time across 2^31. */
static void
add_one_at_a_time(void)
{
	sm_t *m = range_map(0, BODY);
	const struct run r = { 0, P31 + 100 };
	uint64_t i;

	for (i = BODY; m != NULL && i < P31 + 100; i++)
		if (sm_add_grow(&m, i) != i) {
			CHECK(0, "sm_add_grow(%llu)", (unsigned long long)i);
			break;
		}
	CHECK(runs_are(m, &r, 1, "add one at a time") == 0, "extend");
	sm_free(m);
}

/*
 * A run of 2^31 - 1 bits less its second-last bit, then that bit's
 * neighbour past the end.  Through 5.8.5 the remove coalesced the run
 * into one RLE chunk of capacity 2^31 - 1, which is not a multiple of
 * 2048, and the add then started a chunk at 2^31 - 2048, inside that
 * span: sm_validate false.
 */
static void
unaligned_capacity(void)
{
	const uint64_t M = UINT64_C(0x7FFFFFFF);
	const struct run r[2] = { { 0, M - 1 }, { M, M + 1 } };
	sm_t *m = range_map(0, M);

	CHECK(m != NULL && sm_remove(m, M - 1) == M - 1, "remove");
	CHECK(m != NULL && sm_add(m, M) == M, "add");
	CHECK(runs_are(m, r, 2, "unaligned capacity") == 0,
	    "unaligned capacity");
	sm_free(m);
}

int
main(void)
{
	if (SIZE_MAX < UINT64_MAX) {
		printf("test_long_runs: skipped, size_t holds no 2^32 count\n");
		return (77);
	}
	add_range();
	set_ops();
	mutations();
	add_one_at_a_time();
	unaligned_capacity();
	if (failures != 0) {
		fprintf(stderr, "test_long_runs: %d failure(s)\n", failures);
		return (1);
	}
	printf("test_long_runs: ok\n");
	return (0);
}
