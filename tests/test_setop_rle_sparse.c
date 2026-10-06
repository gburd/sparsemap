/* SPDX-License-Identifier: MIT */
/*
 * test_setop_rle_sparse.c - set operations between an RLE chunk and a
 * sparse chunk that share a start, where the run ends inside the sparse
 * chunk.
 *
 * sm_union and sm_difference handle a mixed RLE/sparse overlap by
 * expanding the WHOLE sparse chunk, combining it with the run, and
 * emitting one sparse chunk.  Before 5.8.2 they then advanced the sparse
 * side only to the end of the run, so the sparse chunk's tail
 * [run end, chunk end) was emitted a second time at the same start: two
 * chunks with one start, sm_validate() == false, and sm_cardinality
 * counting the tail twice.  (Reported from pg_tre's w45 differential:
 * sm_union(sm_intersection(a, b), c) with an RLE intersection result
 * starting at 0.)
 *
 * Every operation, in both operand orders, is checked against a naive
 * bit-array model: valid, right cardinality, right members.
 */
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPAN 8192

static int failures;
static unsigned char run[SPAN], sparse[SPAN], want[SPAN];

static void
check(const char *op, uint64_t base, uint64_t len, sm_t *r)
{
	size_t n = 0, wrong = 0;
	uint64_t i;
	for (i = 0; i < SPAN; i++) {
		const bool has = r != NULL && sm_contains(r, base + i, NULL);
		n += want[i];
		wrong += has != (want[i] != 0);
	}
	if (r == NULL ? n != 0 :
	        !sm_validate(r) || sm_cardinality(r) != n || wrong != 0) {
		fprintf(stderr,
		    "FAIL %s base=%llu len=%llu: valid=%d card=%zu want=%zu "
		    "wrong=%zu\n", op, (unsigned long long)base,
		    (unsigned long long)len, r == NULL || sm_validate(r),
		    r == NULL ? 0 : sm_cardinality(r), n, wrong);
		failures++;
	}
	sm_free(r);
}

static void
one(uint64_t base, uint64_t len)
{
	static const uint64_t ids[] = { 4, 63, 64, 200, 1000, 1999, 2047,
		2048, 2100, 5000 };
	sm_t *x = sm_create(64), *y = sm_create(64), *s = sm_create(64);
	sm_t *r; /* the run [base, base + len) as one RLE chunk */
	size_t i;
	int k;

	memset(run, 0, sizeof(run));
	memset(sparse, 0, sizeof(sparse));
	/* An intersection of two runs is an RLE chunk with capacity == length,
	 * the shape that ends inside a neighbouring sparse chunk. */
	sm_add_range(x, base, base + 3000);
	sm_add_range(y, base, base + len);
	r = sm_intersection(x, y);
	for (i = 0; i < len; i++)
		run[i] = 1;
	for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		sm_add_grow(&s, base + ids[i]);
		sparse[ids[i]] = 1;
	}
	if (r == NULL || !sm_validate(r) || sm_cardinality(r) != len) {
		fprintf(stderr, "FAIL setup base=%llu len=%llu\n",
		    (unsigned long long)base, (unsigned long long)len);
		failures++;
	}

	for (k = 0; k < SPAN; k++)
		want[k] = run[k] | sparse[k];
	check("run|sparse", base, len, sm_union(r, s));
	check("sparse|run", base, len, sm_union(s, r));
	for (k = 0; k < SPAN; k++)
		want[k] = run[k] & sparse[k];
	check("run&sparse", base, len, sm_intersection(r, s));
	check("sparse&run", base, len, sm_intersection(s, r));
	for (k = 0; k < SPAN; k++)
		want[k] = run[k] & !sparse[k];
	check("run-sparse", base, len, sm_difference(r, s));
	for (k = 0; k < SPAN; k++)
		want[k] = sparse[k] & !run[k];
	check("sparse-run", base, len, sm_difference(s, r));
	for (k = 0; k < SPAN; k++)
		want[k] = run[k] ^ sparse[k];
	check("run^sparse", base, len, sm_xor(r, s));
	check("sparse^run", base, len, sm_xor(s, r));

	sm_free(r);
	sm_free(s);
	sm_free(y);
	sm_free(x);
}

int
main(void)
{
	static const uint64_t bases[] = { 0, 2048, 65536 };
	static const uint64_t lens[] = { 1, 63, 64, 65, 164, 700, 1500, 2047,
		2048, 2100 };
	size_t b, l;
	for (b = 0; b < sizeof(bases) / sizeof(bases[0]); b++)
		for (l = 0; l < sizeof(lens) / sizeof(lens[0]); l++)
			one(bases[b], lens[l]);
	if (failures != 0) {
		fprintf(stderr, "test_setop_rle_sparse: %d failures\n",
		    failures);
		return (1);
	}
	printf("test_setop_rle_sparse: OK\n");
	return (0);
}
