/* SPDX-License-Identifier: MIT */
/*
 * test_hegel.c - property-based tests for sparsemap using hegel-c.
 *
 * These complement the QCC property tests compiled into test_main:
 * hegel's server-side shrinking turns a failing random sequence into a
 * minimal reproducer, which is exactly what you want when a chunk
 * codec or set-op edge case breaks.
 *
 * hegel-c is an optional dependency (it needs the hegel server binary,
 * libcbor, and zlib), so this target is built only when meson is
 * configured with -Dhegel=enabled and the library is found.  See
 * tests/meson.build and https://github.com/gburd/hegel-c.
 *
 * The oracle is a plain bool[] over a bounded universe: every property
 * compares sparsemap against the obvious dense-array implementation of
 * the same set.
 */
#include <sm.h>

#include <hegel/generators.h>
#include <hegel/hegel.h>

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Bounded universe.  65536 bits span 32 chunks of 2048 bits each, so
 * random fills exercise multi-chunk navigation, sparse<->RLE
 * transitions, and chunk coalescing without the tests running long.
 */
#define U 65536

static sm_t *
fresh(void)
{
	sm_t *m = sm_create(4096);
	assert(m != NULL);
	return (m);
}

/* Set bit idx in the map, growing it as needed. */
static sm_t *
map_add(sm_t *m, uint64_t idx)
{
	uint64_t rc = sm_add_grow(&m, idx);
	assert(rc == idx);
	return (m);
}

/* Set bit idx in both the map (growing it) and the oracle. */
static sm_t *
oracle_add(sm_t *m, bool *oracle, uint64_t idx)
{
	m = map_add(m, idx);
	oracle[idx] = true;
	return (m);
}

/*
 * Property: a random sequence of add/remove/assign operations leaves
 * sparsemap agreeing with a dense bool[] oracle on every query
 * (contains, cardinality, minimum, maximum, and the full ascending
 * iteration order).
 */
static void
prop_model(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	bool *oracle = calloc(U, sizeof(*oracle));
	assert(oracle != NULL);
	sm_t *m = fresh();

	int nops = (int)hegel_draw_int(tc, hegel_integers(0, 400));
	for (int i = 0; i < nops; i++) {
		int kind = (int)hegel_draw_int(tc, hegel_integers(0, 3));
		uint64_t idx = (uint64_t)hegel_draw_int(tc,
		    hegel_integers(0, U - 1));
		switch (kind) {
		case 0: /* add */
			m = oracle_add(m, oracle, idx);
			break;
		case 1: /* remove */
			sm_remove(m, idx);
			oracle[idx] = false;
			break;
		case 2: { /* assign true */
			sm_t *g = sm_set_data_size(m, NULL,
			    sm_get_capacity(m) + 64);
			assert(g != NULL);
			m = g;
			sm_assign(m, idx, true);
			oracle[idx] = true;
			break;
		}
		case 3: /* add a short run (exercises RLE) */
			for (uint64_t j = idx;
			    j < idx + 64 && j < (uint64_t)U; j++)
				m = oracle_add(m, oracle, j);
			break;
		}
		assert(sm_contains(m, idx) == oracle[idx]);
	}

	/* Cardinality and extents agree. */
	size_t card = 0;
	uint64_t lo = 0, hi = 0;
	bool seen = false;
	for (uint64_t b = 0; b < U; b++) {
		if (!oracle[b])
			continue;
		card++;
		if (!seen) {
			lo = b;
			seen = true;
		}
		hi = b;
	}
	assert(sm_cardinality(m) == card);
	if (seen) {
		assert(sm_minimum(m) == lo);
		assert(sm_maximum(m) == hi);
	}

	/* Ascending iteration matches the oracle exactly. */
	uint64_t it = SM_IDX_MAX;
	for (uint64_t b = 0; b < U; b++) {
		if (!oracle[b])
			continue;
		it = sm_next_member(m, it);
		assert(it == b);
	}
	assert(sm_next_member(m, it) == SM_IDX_MAX);

	sm_free(m);
	free(oracle);
}

/*
 * Property: serialize followed by deserialize round-trips to an equal
 * bit set, regardless of the random contents.
 */
static void
prop_serialize_roundtrip(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	sm_t *m = fresh();
	int n = (int)hegel_draw_int(tc, hegel_integers(0, 300));
	for (int i = 0; i < n; i++) {
		uint64_t idx = (uint64_t)hegel_draw_int(tc,
		    hegel_integers(0, U - 1));
		m = map_add(m, idx);
	}

	size_t sz = sm_serialized_size(m);
	uint8_t *buf = malloc(sz);
	assert(buf != NULL);
	size_t wrote = sm_serialize(m, buf, sz);
	assert(wrote == sz);

	sm_t *back = sm_deserialize(buf, sz);
	/* A non-empty map must deserialize; an empty map may yield NULL. */
	if (sm_is_empty(m)) {
		if (back != NULL)
			sm_free(back);
	} else {
		assert(back != NULL);
		assert(sm_equals(m, back));
		sm_free(back);
	}
	free(buf);
	sm_free(m);
}

/* Build a sparsemap and a parallel oracle from a drawn index set. */
static sm_t *
draw_map(hegel_test_case *tc, bool *oracle)
{
	sm_t *m = fresh();
	memset(oracle, 0, U * sizeof(*oracle));
	int n = (int)hegel_draw_int(tc, hegel_integers(0, 300));
	for (int i = 0; i < n; i++) {
		uint64_t idx = (uint64_t)hegel_draw_int(tc,
		    hegel_integers(0, U - 1));
		m = oracle_add(m, oracle, idx);
	}
	return (m);
}

/*
 * Property: sm_union / sm_intersection / sm_difference / sm_xor agree
 * bit-for-bit with the oracle set operations.
 */
static void
prop_setops(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	bool *oa = calloc(U, sizeof(*oa));
	bool *ob = calloc(U, sizeof(*ob));
	assert(oa != NULL && ob != NULL);

	sm_t *a = draw_map(tc, oa);
	sm_t *b = draw_map(tc, ob);

	sm_t *u = sm_union(a, b);
	sm_t *i = sm_intersection(a, b);
	sm_t *d = sm_difference(a, b);
	sm_t *x = sm_xor(a, b);

	for (uint64_t k = 0; k < U; k++) {
		bool want_u = oa[k] || ob[k];
		bool want_i = oa[k] && ob[k];
		bool want_d = oa[k] && !ob[k];
		bool want_x = oa[k] ^ ob[k];
		assert((u ? sm_contains(u, k) : false) == want_u);
		assert((i ? sm_contains(i, k) : false) == want_i);
		assert((d ? sm_contains(d, k) : false) == want_d);
		assert((x ? sm_contains(x, k) : false) == want_x);
	}

	if (u)
		sm_free(u);
	if (i)
		sm_free(i);
	if (d)
		sm_free(d);
	if (x)
		sm_free(x);
	sm_free(a);
	sm_free(b);
	free(oa);
	free(ob);
}

/*
 * Property: deserialize never crashes on arbitrary bytes; it either
 * returns NULL or a self-consistent map that passes sm_validate.
 */
static void
prop_deserialize_no_crash(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	size_t len;
	uint8_t *data = hegel_draw_bytes(tc, hegel_binary(0, 512), &len);
	sm_t *m = sm_deserialize(data, len);
	if (m != NULL) {
		assert(sm_validate(m));
		sm_free(m);
	}
	free(data);
}

static int
run(hegel_session *s, void (*fn)(hegel_test_case *, void *), const char *name)
{
	hegel_settings settings = HEGEL_DEFAULT_SETTINGS;
	settings.max_examples = 200;
	hegel_results r = hegel_run_test(s, fn, NULL, &settings);
	int ok = r.passed ? 0 : 1;
	if (!ok)
		fprintf(stderr, "hegel property FAILED: %s\n", name);
	hegel_results_free(&r);
	return (ok);
}

int
main(void)
{
	hegel_session *s = hegel_session_new();
	int rc = 0;
	rc |= run(s, prop_model, "model");
	rc |= run(s, prop_serialize_roundtrip, "serialize_roundtrip");
	rc |= run(s, prop_setops, "setops");
	rc |= run(s, prop_deserialize_no_crash, "deserialize_no_crash");
	hegel_session_free(s);
	return (rc);
}
