/* SPDX-License-Identifier: MIT */
/*
 * sm_microbench.c - micro-benchmarks for individual sparsemap
 * operations.
 *
 * Unlike bench.c (the comparative benchmark vs. CRoaring, kept on a
 * separate branch), this harness measures sparsemap in isolation so
 * the numbers are stable and reviewable in CI.  It is deliberately
 * dependency-free: it links only against libsparsemap.
 *
 * Methodology, in service of repeatable numbers:
 *
 *   - CLOCK_MONOTONIC, or CLOCK_PROCESS_CPUTIME_ID when available, so
 *     wall-clock scheduling jitter is excluded.
 *   - A calibrated inner iteration count: each measured region runs
 *     long enough (>= MIN_RUN_NS) that timer granularity is noise.
 *   - REPS independent repetitions; we report the *minimum* per-op
 *     time (least perturbed by interrupts) alongside the median.
 *   - A warmup pass primes caches and branch predictors before timing.
 *   - A volatile sink defeats dead-code elimination of results.
 *
 * For the most stable numbers run pinned and at high priority:
 *
 *     taskset -c 2 nice -n -5 ./builddir/bench/sm_microbench
 *
 * and disable turbo / set the performance governor on the host.
 */
#include <sm.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REPS 7		    /* independent timed repetitions */
#define MIN_RUN_NS 20000000 /* 20 ms: grow iters until a rep lasts this long */

static volatile uint64_t g_sink;

static uint64_t
now_ns(void)
{
	struct timespec ts;
#if defined(CLOCK_PROCESS_CPUTIME_ID)
	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0)
		return ((uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec);
#endif
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec);
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x < y ? -1 : x > y ? 1 : 0);
}

/*
 * A benchmark is a function that performs `iters` units of work and
 * returns a checksum (folded into the sink).  The harness calibrates
 * `iters`, then times REPS repetitions and reports ns/op.
 */
typedef uint64_t (*bench_fn)(uint64_t iters, void *aux);

static void
run_bench(const char *name, bench_fn fn, void *aux)
{
	/* Warmup + calibrate iters so one rep lasts ~MIN_RUN_NS. */
	uint64_t iters = 1000;
	for (;;) {
		uint64_t t0 = now_ns();
		g_sink ^= fn(iters, aux);
		uint64_t dt = now_ns() - t0;
		if (dt >= MIN_RUN_NS || iters >= (1ull << 32))
			break;
		/* Scale up, guarding against a zero/near-zero measurement. */
		uint64_t scale = dt ? (MIN_RUN_NS / dt) + 1 : 8;
		iters *= (scale < 2 ? 2 : scale);
	}

	uint64_t best[REPS];
	for (int r = 0; r < REPS; r++) {
		uint64_t t0 = now_ns();
		g_sink ^= fn(iters, aux);
		best[r] = now_ns() - t0;
	}
	qsort(best, REPS, sizeof(best[0]), cmp_u64);
	double min_ns = (double)best[0] / (double)iters;
	double med_ns = (double)best[REPS / 2] / (double)iters;
	printf("  %-28s %10.2f ns/op (min)   %10.2f ns/op (median)\n", name,
	    min_ns, med_ns);
}

/* ---- dataset builders ---- */

static sparsemap_t *
build_random(uint64_t n, uint64_t span, unsigned seed)
{
	sparsemap_t *m = sm_create(1024);
	srand(seed);
	for (uint64_t i = 0; i < n; i++) {
		uint64_t idx = (uint64_t)rand() % span;
		if (sm_add_grow(&m, idx) == SM_IDX_MAX)
			break;
	}
	return (m);
}

static sparsemap_t *
build_dense(uint64_t lo, uint64_t hi)
{
	/* One long run -> mostly RLE chunks. */
	sparsemap_t *m = sm_create(1024);
	for (uint64_t i = lo; i < hi; i++) {
		if (sm_add_grow(&m, i) == SM_IDX_MAX)
			break;
	}
	return (m);
}

/* ---- benchmark bodies ---- */

static uint64_t
b_add_sequential(uint64_t iters, void *aux)
{
	(void)aux;
	uint64_t sum = 0;
	for (uint64_t r = 0; r < iters; r += 4096) {
		sparsemap_t *m = sm_create(1024);
		uint64_t lim = (iters - r < 4096) ? iters - r : 4096;
		for (uint64_t i = 0; i < lim; i++)
			sum += sm_add_grow(&m, i);
		sm_free(m);
	}
	return (sum);
}

static uint64_t
b_add_random(uint64_t iters, void *aux)
{
	(void)aux;
	uint64_t sum = 0;
	srand(12345);
	for (uint64_t r = 0; r < iters; r += 4096) {
		sparsemap_t *m = sm_create(1024);
		uint64_t lim = (iters - r < 4096) ? iters - r : 4096;
		for (uint64_t i = 0; i < lim; i++)
			sum += sm_add_grow(&m, (uint64_t)rand() % (1u << 20));
		sm_free(m);
	}
	return (sum);
}

static uint64_t
b_contains_hit(uint64_t iters, void *aux)
{
	sparsemap_t *m = aux;
	uint64_t hi = sm_maximum(m), sum = 0;
	for (uint64_t i = 0; i < iters; i++)
		sum += sm_contains(m, (i * 2654435761u) % (hi + 1)) ? 1 : 0;
	return (sum);
}

static uint64_t
b_next_member(uint64_t iters, void *aux)
{
	sparsemap_t *m = aux;
	uint64_t sum = 0, done = 0;
	while (done < iters) {
		uint64_t i = SM_IDX_MAX;
		while ((i = sm_next_member(m, i)) != SM_IDX_MAX) {
			sum += i;
			if (++done >= iters)
				break;
		}
		if (sm_is_empty(m))
			break;
	}
	return (sum);
}

static uint64_t
b_rank(uint64_t iters, void *aux)
{
	sparsemap_t *m = aux;
	uint64_t hi = sm_maximum(m), sum = 0;
	for (uint64_t i = 0; i < iters; i++)
		sum += sm_rank(m, 0, (i * 40503u) % (hi + 1), true);
	return (sum);
}

static uint64_t
b_select(uint64_t iters, void *aux)
{
	sparsemap_t *m = aux;
	uint64_t card = sm_cardinality(m), sum = 0;
	if (card == 0)
		return (0);
	for (uint64_t i = 0; i < iters; i++)
		sum += sm_select(m, (i * 2654435761u) % card, true);
	return (sum);
}

struct pair {
	const sparsemap_t *a, *b;
};

static uint64_t
b_union(uint64_t iters, void *aux)
{
	struct pair *p = aux;
	uint64_t sum = 0;
	for (uint64_t i = 0; i < iters; i++) {
		sparsemap_t *r = sm_union(p->a, p->b);
		if (r) {
			sum += sm_get_size(r);
			sm_free(r);
		}
	}
	return (sum);
}

static uint64_t
b_intersection(uint64_t iters, void *aux)
{
	struct pair *p = aux;
	uint64_t sum = 0;
	for (uint64_t i = 0; i < iters; i++) {
		sparsemap_t *r = sm_intersection(p->a, p->b);
		if (r) {
			sum += sm_get_size(r);
			sm_free(r);
		}
	}
	return (sum);
}

static uint64_t
b_serialize(uint64_t iters, void *aux)
{
	sparsemap_t *m = aux;
	size_t sz = sm_serialized_size(m);
	uint8_t *buf = malloc(sz);
	uint64_t sum = 0;
	for (uint64_t i = 0; i < iters; i++)
		sum += sm_serialize(m, buf, sz);
	free(buf);
	return (sum);
}

int
main(void)
{
	printf("sparsemap micro-benchmarks (version %s)\n", SM_VERSION_STRING);
	printf("lower is better; min is the least-perturbed sample\n\n");

	sparsemap_t *rnd = build_random(50000, 1u << 20, 1);
	sparsemap_t *dense = build_dense(0, 200000);
	sparsemap_t *rnd2 = build_random(50000, 1u << 20, 2);
	struct pair rr = { rnd, rnd2 };
	struct pair rd = { rnd, dense };

	printf("construction:\n");
	run_bench("add sequential", b_add_sequential, NULL);
	run_bench("add random (1M span)", b_add_random, NULL);

	printf("\nqueries on a 50k-bit random map:\n");
	run_bench("contains", b_contains_hit, rnd);
	run_bench("rank(0, x)", b_rank, rnd);
	run_bench("select(n)", b_select, rnd);
	run_bench("next_member iterate", b_next_member, rnd);

	printf("\nqueries on a 200k-bit dense (RLE) map:\n");
	run_bench("contains", b_contains_hit, dense);
	run_bench("rank(0, x)", b_rank, dense);
	run_bench("select(n)", b_select, dense);
	run_bench("next_member iterate", b_next_member, dense);

	printf("\nset operations:\n");
	run_bench("union (random,random)", b_union, &rr);
	run_bench("intersection (random,random)", b_intersection, &rr);
	run_bench("union (random,dense)", b_union, &rd);
	run_bench("intersection (random,dense)", b_intersection, &rd);

	printf("\nserialization:\n");
	run_bench("serialize (random)", b_serialize, rnd);
	run_bench("serialize (dense)", b_serialize, dense);

	sm_free(rnd);
	sm_free(rnd2);
	sm_free(dense);
	/* Keep the optimizer honest. */
	if (g_sink == 0x1234567)
		printf("(sink=%" PRIu64 ")\n", (uint64_t)g_sink);
	return (0);
}
