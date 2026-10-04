/* SPDX-License-Identifier: MIT
 *
 * Regression test for the bulk sm_add_many / sm_add_many_grow run-emitter
 * build (perf: O(n log n) build, not O(n^2)).
 *
 * Cross-checks the bulk builder against two independent references:
 *   1. one-at-a-time sm_add_grow (the per-element path), and
 *   2. a brute-force sorted-unique array oracle.
 * For every seeded scenario the three must agree on membership
 * (sm_equals + sm_hash), cardinality, round-trip, and sm_validate.
 *
 * Scenarios exercise sparse, dense, clustered, duplicate-heavy, and
 * existing-content-overlapping inputs across the small-set / chunk-mode
 * boundary (SM_SMALL_MAX_BITS == 1024) and the degenerate single/all-in-
 * one-chunk cases.
 */
#include <sm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state = 0x9e3779b9u;
static uint32_t
rng(void)
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return (x);
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return ((x > y) - (x < y));
}

/* Sorted-unique union of `arr` and `seed` -> the brute-force oracle. */
static size_t
oracle_build(uint64_t *out, const uint64_t *arr, size_t n,
    const uint64_t *seed, size_t sn)
{
	size_t m = 0, i;
	for (i = 0; i < n; i++)
		out[m++] = arr[i];
	for (i = 0; i < sn; i++)
		out[m++] = seed[i];
	if (m == 0)
		return (0);
	qsort(out, m, sizeof(uint64_t), cmp_u64);
	{
		size_t w = 1, r;
		for (r = 1; r < m; r++)
			if (out[r] != out[w - 1])
				out[w++] = out[r];
		return (w);
	}
}

static int failures = 0;

#define CHECK(cond, msg)                                              \
	do {                                                          \
		if (!(cond)) {                                        \
			fprintf(stderr, "FAIL [%s]: %s\n", scenario,  \
			    msg);                                     \
			failures++;                                   \
		}                                                     \
	} while (0)

/*
 * Run one scenario: build a map two ways and compare against the oracle.
 * `seed` bits are added to BOTH maps first (via sm_add_grow) so the
 * bulk path is exercised as a union into existing content.
 */
static void
run(const char *scenario, const uint64_t *arr, size_t n,
    const uint64_t *seed, size_t sn)
{
	sm_t *bulk = sm_create(64);
	sm_t *ref = sm_create(64);
	uint64_t *oracle = malloc((n + sn + 1) * sizeof(uint64_t));
	size_t on, i;

	if (bulk == NULL || ref == NULL || oracle == NULL) {
		fprintf(stderr, "FAIL [%s]: allocation\n", scenario);
		failures++;
		goto done;
	}

	/* Seed both maps with pre-existing content. */
	for (i = 0; i < sn; i++) {
		(void)sm_add_grow(&bulk, seed[i]);
		(void)sm_add_grow(&ref, seed[i]);
	}

	/* bulk path */
	CHECK(sm_add_many_grow(&bulk, arr, n), "sm_add_many_grow returned false");

	/* per-element reference path */
	for (i = 0; i < n; i++)
		(void)sm_add_grow(&ref, arr[i]);

	on = oracle_build(oracle, arr, n, seed, sn);

	CHECK(sm_validate(bulk), "bulk map fails sm_validate");
	CHECK(sm_validate(ref), "ref map fails sm_validate");
	CHECK(sm_equals(bulk, ref), "bulk != per-element");
	CHECK(sm_hash(bulk) == sm_hash(ref), "hash mismatch bulk vs ref");
	CHECK(sm_cardinality(bulk) == on, "bulk cardinality != oracle");
	CHECK(sm_cardinality(ref) == on, "ref cardinality != oracle");

	/* membership against the oracle, both directions */
	for (i = 0; i < on; i++)
		CHECK(sm_contains(bulk, oracle[i], NULL),
		    "oracle member missing from bulk");
	{
		/* round-trip: sm_to_array must reproduce the oracle exactly */
		uint64_t *back = malloc((on + 1) * sizeof(uint64_t));
		size_t bn = on + 1;
		if (back != NULL) {
			sm_to_array(bulk, back, &bn);
			CHECK(bn == on, "round-trip length != oracle");
			for (i = 0; i < bn && i < on; i++)
				CHECK(back[i] == oracle[i],
				    "round-trip value != oracle");
			free(back);
		}
	}

	/* Idempotence: adding the same array again changes nothing. */
	CHECK(sm_add_many_grow(&bulk, arr, n), "second add_many_grow false");
	CHECK(sm_cardinality(bulk) == on, "not idempotent");

done:
	free(oracle);
	sm_free(bulk);
	sm_free(ref);
}

/* Fill `arr` with `n` values in [0,span) then return n. */
static size_t
gen_sparse(uint64_t *arr, size_t n, uint64_t span)
{
	size_t i;
	for (i = 0; i < n; i++)
		arr[i] = ((uint64_t)rng() << 32 | rng()) % span;
	return (n);
}

/* Dense contiguous-ish: base + small jitter, so runs coalesce. */
static size_t
gen_dense(uint64_t *arr, size_t n, uint64_t base)
{
	size_t i;
	for (i = 0; i < n; i++)
		arr[i] = base + i; /* consecutive -> one run */
	return (n);
}

/* Clustered: a handful of dense blocks scattered across a wide span. */
static size_t
gen_clustered(uint64_t *arr, size_t n)
{
	size_t i = 0;
	while (i < n) {
		uint64_t base = ((uint64_t)rng() << 20) % 100000000ull;
		size_t blk = 1 + (rng() % 200);
		size_t j;
		for (j = 0; j < blk && i < n; j++)
			arr[i++] = base + j;
	}
	return (n);
}

int
main(void)
{
	enum { N = 4000 };
	uint64_t *arr = malloc(N * sizeof(uint64_t));
	uint64_t *seed = malloc(N * sizeof(uint64_t));
	if (arr == NULL || seed == NULL)
		return (99);

	/* empty input */
	arr[0] = 0; /* silence -Wmaybe-uninitialized; n==0 reads nothing */
	run("empty", arr, 0, NULL, 0);

	/*
	 * Minimal n>=2 and chunk-boundary pairs.  These are the smallest
	 * inputs that enter the bulk coalesce/union/emit path, and are
	 * exactly the shapes a downstream port (PostgreSQL sbm) found an
	 * infinite loop on while random/large scenarios masked it.  The
	 * run() helper's "bulk == per-element sm_add_grow == oracle"
	 * cross-check is the property that catches a non-terminating or
	 * wrong emitter here; a hang shows up as the test never returning.
	 * SM_CHUNK_MAX_CAPACITY is 2048, so 2047/2048 straddle a chunk
	 * boundary and 2048/2049 open on one.
	 */
	{
		uint64_t two[2] = { 1, 2 };		/* same word */
		uint64_t gap[2] = { 1, 2048 };		/* different chunks */
		uint64_t cross[2] = { 2047, 2048 };	/* straddle chunk boundary */
		uint64_t aligned[2] = { 2048, 2049 };	/* open on a boundary */
		uint64_t three[3] = { 1, 2, 3 };	/* consecutive -> one run */
		uint64_t dup2[2] = { 42, 42 };		/* duplicate pair */
		uint64_t wordedge[3] = { 63, 64, 65 };	/* 64-bit word boundary */
		run("pair-same-word", two, 2, NULL, 0);
		run("pair-two-chunks", gap, 2, NULL, 0);
		run("pair-straddle-chunk", cross, 2, NULL, 0);
		run("pair-chunk-aligned", aligned, 2, NULL, 0);
		run("triple-consecutive", three, 3, NULL, 0);
		run("pair-duplicate", dup2, 2, NULL, 0);
		run("triple-word-edge", wordedge, 3, NULL, 0);
	}

	/* single element small / chunk / at the 1024 boundary */
	arr[0] = 7;
	run("single-small", arr, 1, NULL, 0);
	arr[0] = 1024;
	run("single-boundary", arr, 1, NULL, 0);
	arr[0] = 5000000;
	run("single-chunk", arr, 1, NULL, 0);

	/* duplicates only */
	{
		size_t i;
		for (i = 0; i < 500; i++)
			arr[i] = 42;
		run("all-duplicates", arr, 500, NULL, 0);
	}

	/* small-mode: all < 1024, stays small */
	run("small-mode", arr, gen_sparse(arr, 300, 1024), NULL, 0);

	/* straddle 1024: some below, some above the small-set boundary */
	{
		size_t i;
		for (i = 0; i < 400; i++)
			arr[i] = (i & 1) ? (900 + i) : (i % 1000);
		run("straddle-1024", arr, 400, NULL, 0);
	}

	/* sparse across a wide span (chunk mode, one bit per window) */
	run("sparse-1e6", arr, gen_sparse(arr, N, 1000000ull), NULL, 0);
	run("sparse-1e9", arr, gen_sparse(arr, N, 1000000000ull), NULL, 0);

	/* dense: a single long run (all-in-one-chunk and multi-chunk) */
	run("dense-in-chunk", arr, gen_dense(arr, 1500, 0), NULL, 0);
	run("dense-multichunk", arr, gen_dense(arr, N, 500000), NULL, 0);

	/* clustered dense blocks */
	run("clustered", arr, gen_clustered(arr, N), NULL, 0);

	/* overlapping existing content: seed then bulk-add, some overlap */
	{
		size_t sn = gen_sparse(seed, 800, 2000000ull);
		size_t an = gen_sparse(arr, N, 2000000ull);
		run("overlap-existing", arr, an, seed, sn);
	}

	/* overlap where the seed is a dense run and the bulk fills gaps */
	{
		size_t sn = gen_dense(seed, 1000, 3000);
		size_t an;
		size_t i;
		for (i = 0; i < 1000; i++)
			arr[i] = 3000 + 2 * i; /* every other, some overlap */
		an = 1000;
		run("overlap-run", arr, an, seed, sn);
	}

	/* seed dense small-mode, bulk pushes it into chunk mode */
	{
		size_t sn = gen_dense(seed, 500, 0);      /* < 1024 mostly */
		size_t an = gen_dense(arr, 500, 5000000); /* far away */
		run("promote-from-small", arr, an, seed, sn);
	}

	free(arr);
	free(seed);

	if (failures == 0) {
		printf("test_add_many: all scenarios passed\n");
		return (0);
	}
	fprintf(stderr, "test_add_many: %d failures\n", failures);
	return (1);
}
