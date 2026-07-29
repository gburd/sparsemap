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

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compatibility layer over the official hegeldev/hegel-rust hegel-c FFI
 * (see tests/hegel_compat.h).  Included after <assert.h> because it
 * redefines assert() into a shrink-friendly property-failure signal.
 * This TU owns the shim's run-state definition. */
#define HEGEL_COMPAT_IMPL
#include "hegel_compat.h"

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
		assert(sm_contains(m, idx, NULL) == oracle[idx]);
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
		it = sm_next_member(m, it, NULL);
		assert(it == b);
	}
	assert(sm_next_member(m, it, NULL) == SM_IDX_MAX);

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
		assert((u ? sm_contains(u, k, NULL) : false) == want_u);
		assert((i ? sm_contains(i, k, NULL) : false) == want_i);
		assert((d ? sm_contains(d, k, NULL) : false) == want_d);
		assert((x ? sm_contains(x, k, NULL) : false) == want_x);
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

/* ================================================================= */
/* Tier 1: rank/select duality, bidirectional iteration, cursor      */
/* ================================================================= */

/*
 * Property: sm_rank and sm_select agree with the dense oracle for
 * both polarities, and they are mutual inverses where defined.
 *
 *   - sm_rank(map, 0, k, true)  == popcount of set bits in [0, k].
 *   - sm_rank(map, 0, k, false) == count of unset bits in [0, k].
 *   - sm_select(map, n, true)   == the index of the n-th set bit.
 *   - rank/select duality: for the n-th set bit b = select(n, true),
 *     rank(0, b, true) == n + 1 (b is the (n+1)-th set bit counting
 *     from zero, inclusive of b itself).
 */
static void
prop_rank_select(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	bool *oracle = calloc(U, sizeof(*oracle));
	assert(oracle != NULL);
	sm_t *m = draw_map(tc, oracle);

	/* Spot-check sm_rank at a drawn cut point, both polarities. */
	uint64_t k = (uint64_t)hegel_draw_int(tc, hegel_integers(0, U - 1));
	size_t set_le = 0, unset_le = 0;
	for (uint64_t b = 0; b <= k; b++) {
		if (oracle[b])
			set_le++;
		else
			unset_le++;
	}
	assert(sm_rank(m, 0, k, true) == set_le);
	assert(sm_rank(m, 0, k, false) == unset_le);

	/* sm_rank over a drawn sub-range [x, y] (inclusive). */
	uint64_t x = (uint64_t)hegel_draw_int(tc, hegel_integers(0, U - 1));
	uint64_t y = (uint64_t)hegel_draw_int(tc, hegel_integers(0, U - 1));
	if (x > y) {
		uint64_t t = x;
		x = y;
		y = t;
	}
	size_t set_in = 0;
	for (uint64_t b = x; b <= y; b++)
		if (oracle[b])
			set_in++;
	assert(sm_rank(m, x, y, true) == set_in);

	/* sm_select: walk every set bit and confirm select(n) finds it,
	 * and the rank/select duality holds. */
	size_t n = 0;
	for (uint64_t b = 0; b < U; b++) {
		if (!oracle[b])
			continue;
		assert(sm_select(m, n, true) == b);
		assert(sm_rank(m, 0, b, true) == n + 1);
		n++;
	}
	/* Selecting past the last set bit yields the sentinel. */
	assert(sm_select(m, n, true) == SM_IDX_MAX);

	sm_free(m);
	free(oracle);
}

/*
 * Property: sm_prev_member is the exact reverse of the ascending
 * iteration validated by prop_model -- it visits every set bit in
 * descending order and only those bits.  The SM_IDX_MAX sentinel
 * means "start at the end".
 */
static void
prop_prev_member(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	bool *oracle = calloc(U, sizeof(*oracle));
	assert(oracle != NULL);
	sm_t *m = draw_map(tc, oracle);

	uint64_t it = SM_IDX_MAX;
	for (uint64_t bb = U; bb-- > 0;) {
		if (!oracle[bb])
			continue;
		it = sm_prev_member(m, it, NULL);
		assert(it == bb);
	}
	assert(sm_prev_member(m, it, NULL) == SM_IDX_MAX);

	sm_free(m);
	free(oracle);
}

/*
 * Property: interleaved add / remove / contains / next / prev in a
 * random order stays consistent with the oracle after every step.
 * This stresses the tail-chunk cursor far harder than prop_model's
 * mostly-ascending fill: the cursor caches the last located chunk
 * and must be invalidated by inserts and removes that shift chunk
 * bytes at or before it, so mixing the access directions is where a
 * stale-cursor bug would surface.
 */
static void
prop_cursor_interleaved(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	bool *oracle = calloc(U, sizeof(*oracle));
	assert(oracle != NULL);
	sm_t *m = fresh();

	int nops = (int)hegel_draw_int(tc, hegel_integers(0, 500));
	for (int i = 0; i < nops; i++) {
		int kind = (int)hegel_draw_int(tc, hegel_integers(0, 4));
		uint64_t idx = (uint64_t)hegel_draw_int(tc,
		    hegel_integers(0, U - 1));
		switch (kind) {
		case 0: /* add (ascending or random, cursor-relevant) */
			m = oracle_add(m, oracle, idx);
			break;
		case 1: /* remove (may shift chunks before the cursor) */
			sm_remove(m, idx);
			oracle[idx] = false;
			break;
		case 2: { /* forward probe from idx */
			uint64_t got = sm_next_member(m, idx, NULL);
			uint64_t want = SM_IDX_MAX;
			for (uint64_t b = idx + 1; b < U; b++)
				if (oracle[b]) {
					want = b;
					break;
				}
			assert(got == want);
			break;
		}
		case 3: { /* backward probe from idx */
			uint64_t got = sm_prev_member(m, idx, NULL);
			uint64_t want = SM_IDX_MAX;
			for (uint64_t b = idx; b-- > 0;)
				if (oracle[b]) {
					want = b;
					break;
				}
			assert(got == want);
			break;
		}
		case 4: /* membership probe */
			assert(sm_contains(m, idx, NULL) == oracle[idx]);
			break;
		}
	}

	sm_free(m);
	free(oracle);
}

/* ================================================================= */
/* Tier 2: range operations and sm_offset                            */
/* ================================================================= */

/*
 * Property: sm_add_range / sm_remove_range / sm_flip_range over the
 * half-open interval [lo, hi) match the obvious oracle updates, and
 * sm_flip_range is an involution (flipping the same range twice is a
 * no-op).
 */
static void
prop_range_ops(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	bool *oracle = calloc(U, sizeof(*oracle));
	assert(oracle != NULL);
	sm_t *m = draw_map(tc, oracle);

	uint64_t lo = (uint64_t)hegel_draw_int(tc, hegel_integers(0, U - 1));
	uint64_t hi = (uint64_t)hegel_draw_int(tc, hegel_integers(0, U));
	if (lo > hi) {
		uint64_t t = lo;
		lo = hi;
		hi = t;
	}

	int op = (int)hegel_draw_int(tc, hegel_integers(0, 2));
	if (op == 0) {
		/* add_range: every bit in [lo, hi) becomes set. */
		/* Capacity may need to grow; ensure room first. */
		sm_t *g = sm_set_data_size(m, NULL, sm_get_capacity(m) + U / 8);
		assert(g != NULL);
		m = g;
		sm_add_range(m, lo, hi);
		for (uint64_t b = lo; b < hi; b++)
			oracle[b] = true;
	} else if (op == 1) {
		/* remove_range: every bit in [lo, hi) becomes unset. */
		sm_remove_range(m, lo, hi);
		for (uint64_t b = lo; b < hi; b++)
			oracle[b] = false;
	} else {
		/* flip_range twice == identity. */
		sm_t *g = sm_set_data_size(m, NULL, sm_get_capacity(m) + U / 8);
		assert(g != NULL);
		m = g;
		sm_flip_range(m, lo, hi);
		for (uint64_t b = lo; b < hi; b++)
			oracle[b] = !oracle[b];
		/* check the single flip first */
		for (uint64_t b = 0; b < U; b++)
			assert(sm_contains(m, b, NULL) == oracle[b]);
		/* involution: flip the same range back */
		sm_flip_range(m, lo, hi);
		for (uint64_t b = lo; b < hi; b++)
			oracle[b] = !oracle[b];
	}

	for (uint64_t b = 0; b < U; b++)
		assert(sm_contains(m, b, NULL) == oracle[b]);

	sm_free(m);
	free(oracle);
}

/*
 * Property: sm_extract_range returns a new map holding exactly the
 * source bits inside [lo, hi) and nothing else.
 */
static void
prop_extract_range(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	bool *oracle = calloc(U, sizeof(*oracle));
	assert(oracle != NULL);
	sm_t *m = draw_map(tc, oracle);

	uint64_t lo = (uint64_t)hegel_draw_int(tc, hegel_integers(0, U - 1));
	uint64_t hi = (uint64_t)hegel_draw_int(tc, hegel_integers(0, U));
	if (lo > hi) {
		uint64_t t = lo;
		lo = hi;
		hi = t;
	}

	sm_t *e = sm_extract_range(m, lo, hi);
	for (uint64_t b = 0; b < U; b++) {
		bool want = (b >= lo && b < hi) ? oracle[b] : false;
		bool got = (e != NULL) ? sm_contains(e, b, NULL) : false;
		assert(got == want);
	}
	if (e != NULL)
		sm_free(e);
	sm_free(m);
	free(oracle);
}

/*
 * Property: sm_offset shifts every bit by a signed amount, dropping
 * bits that fall below zero, and offsetting is composable:
 * offset(map, a+b) has the same membership as offset(offset(map, a), b)
 * for shifts that do not push bits out of the bounded universe.
 */
static void
prop_offset(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	bool *oracle = calloc(U, sizeof(*oracle));
	assert(oracle != NULL);
	/* Keep source bits in the lower half so positive shifts stay in U. */
	sm_t *m = fresh();
	memset(oracle, 0, U * sizeof(*oracle));
	int nbits = (int)hegel_draw_int(tc, hegel_integers(0, 200));
	for (int i = 0; i < nbits; i++) {
		uint64_t idx = (uint64_t)hegel_draw_int(tc,
		    hegel_integers(0, U / 2 - 1));
		m = oracle_add(m, oracle, idx);
	}

	/* Positive shift within bounds. */
	int64_t off = hegel_draw_int(tc, hegel_integers(0, U / 4));
	sm_t *shifted = sm_offset(m, (ssize_t)off);
	for (uint64_t b = 0; b < U; b++) {
		bool want = (b >= (uint64_t)off) ? oracle[b - (uint64_t)off]
		                                 : false;
		bool got = (shifted != NULL) ? sm_contains(shifted, b, NULL) : false;
		assert(got == want);
	}
	if (shifted != NULL)
		sm_free(shifted);
	sm_free(m);
	free(oracle);
}

/* ================================================================= */
/* Tier 3: set-op consistency identities                             */
/* ================================================================= */

/*
 * Property: the *_cardinality functions equal the cardinality of the
 * materialized result, the binary ops commute where they should, and
 * the standard inclusion/exclusion identity |A|+|B| = |A&B|+|A|B|
 * holds.  These are independent code paths (counting without
 * building the result), so the identities are real cross-checks.
 */
static void
prop_setop_identities(hegel_test_case *tc, void *ctx)
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

	size_t cu = u ? sm_cardinality(u) : 0;
	size_t ci = i ? sm_cardinality(i) : 0;
	size_t cd = d ? sm_cardinality(d) : 0;
	size_t cx = x ? sm_cardinality(x) : 0;

	/* cardinality-without-materialization matches. */
	assert(sm_union_cardinality(a, b) == cu);
	assert(sm_intersection_cardinality(a, b) == ci);
	assert(sm_difference_cardinality(a, b) == cd);
	assert(sm_xor_cardinality(a, b) == cx);

	/* inclusion-exclusion: |A|+|B| == |A&B| + |A|B|. */
	assert(sm_cardinality(a) + sm_cardinality(b) == ci + cu);
	/* xor == union minus intersection. */
	assert(cx == cu - ci);
	/* difference == |A| - |A&B|. */
	assert(cd == sm_cardinality(a) - ci);

	/* commutativity of union / intersection / xor. */
	sm_t *u2 = sm_union(b, a);
	sm_t *i2 = sm_intersection(b, a);
	sm_t *x2 = sm_xor(b, a);
	assert((u && u2) ? sm_equals(u, u2) : (u == u2));
	assert((i && i2) ? sm_equals(i, i2) : (i == i2));
	assert((x && x2) ? sm_equals(x, x2) : (x == x2));

	/* intersection is a subset of each operand. */
	if (i != NULL) {
		assert(sm_is_subset(i, a));
		assert(sm_is_subset(i, b));
	}

	if (u) sm_free(u);
	if (i) sm_free(i);
	if (d) sm_free(d);
	if (x) sm_free(x);
	if (u2) sm_free(u2);
	if (i2) sm_free(i2);
	if (x2) sm_free(x2);
	sm_free(a);
	sm_free(b);
	free(oa);
	free(ob);
}

/*
 * Property: the in-place set ops produce the same set as their pure
 * counterparts.  The in-place variants have separate
 * buffer-management code (chunk-pair walk + memcpy into one operand),
 * so equivalence to the pure version is a strong oracle.
 */
static void
prop_setop_inplace(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	bool *oa = calloc(U, sizeof(*oa));
	bool *ob = calloc(U, sizeof(*ob));
	assert(oa != NULL && ob != NULL);
	sm_t *a = draw_map(tc, oa);
	sm_t *b = draw_map(tc, ob);

	int op = (int)hegel_draw_int(tc, hegel_integers(0, 2));

	/* pure reference */
	sm_t *pure = (op == 0) ? sm_union(a, b)
	           : (op == 1) ? sm_intersection(a, b)
	                       : sm_difference(a, b);

	/* in-place on a copy of a */
	sm_t *acopy = sm_copy(a);
	assert(acopy != NULL);
	sm_t *got = (op == 0) ? sm_union_inplace(acopy, b)
	          : (op == 1) ? sm_intersection_inplace(acopy, b)
	                      : sm_difference_inplace(acopy, b);

	for (uint64_t k = 0; k < U; k++) {
		bool wp = (pure != NULL) ? sm_contains(pure, k, NULL) : false;
		bool wg = (got != NULL) ? sm_contains(got, k, NULL) : false;
		assert(wp == wg);
	}

	if (pure)
		sm_free(pure);
	/* got aliases acopy (possibly reallocated); free exactly once. */
	if (got)
		sm_free(got);
	else
		sm_free(acopy);
	sm_free(a);
	sm_free(b);
	free(oa);
	free(ob);
}

/* ================================================================= */
/* Tier 4: hash/compare, lineage round-trips, constructors           */
/* ================================================================= */

/*
 * Property: equal maps hash equally and compare equal; unequal maps
 * compare non-equal.  (Hash collisions on unequal maps are allowed,
 * so we only assert the equals=>same-hash direction.)
 */
static void
prop_hash_compare(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	bool *oa = calloc(U, sizeof(*oa));
	assert(oa != NULL);
	sm_t *a = draw_map(tc, oa);
	sm_t *b = sm_copy(a);
	assert(b != NULL);

	/* identical content: equal, same hash, compare 0. */
	assert(sm_equals(a, b));
	assert(sm_hash(a) == sm_hash(b));
	assert(sm_compare(a, b) == 0);

	/* perturb b by one bit and require inequality. */
	uint64_t idx = (uint64_t)hegel_draw_int(tc, hegel_integers(0, U - 1));
	if (sm_contains(b, idx, NULL)) {
		sm_remove(b, idx);
	} else {
		sm_t *g = sm_set_data_size(b, NULL, sm_get_capacity(b) + 64);
		assert(g != NULL);
		b = g;
		sm_add(b, idx);
	}
	assert(!sm_equals(a, b));
	assert(sm_compare(a, b) != 0);

	sm_free(a);
	sm_free(b);
	free(oa);
}

/*
 * Property: the allocation-lineage round-trips preserve the bit set.
 * A map serialized then re-opened via sm_open into a caller buffer,
 * an sm_owned_copy, and an sm_copy must all equal the original and
 * survive a subsequent grow + add.  The lineage state machine
 * (SM_WRAPPED / SM_OWNED_SPLIT / SM_OWNED_CONTIGUOUS) is where the
 * original heisenbug lived, so exercising the transitions under grow
 * is high value.
 */
static void
prop_lineage_roundtrip(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	bool *oracle = calloc(U, sizeof(*oracle));
	assert(oracle != NULL);
	sm_t *m = draw_map(tc, oracle);

	/* sm_copy and sm_owned_copy both mirror the original. */
	sm_t *c = sm_copy(m);
	sm_t *oc = sm_owned_copy(m);
	assert(c != NULL && oc != NULL);
	assert(sm_equals(m, c));
	assert(sm_equals(m, oc));

	/* sm_open_copy deserializes raw bytes into a fresh owned map
	 * (sm_t is opaque, so we cannot stack-allocate one for sm_open;
	 * sm_open_copy is the purpose-built helper and exercises the same
	 * deserialize-into-owned-buffer path). */
	size_t sz = sm_get_size(m);
	uint8_t *buf = malloc(sz);
	assert(buf != NULL);
	memcpy(buf, sm_get_data(m), sz);
	sm_t *opened = sm_open_copy(buf, sz, 256);
	if (sm_is_empty(m)) {
		if (opened != NULL)
			assert(sm_equals(m, opened));
	} else {
		assert(opened != NULL);
		assert(sm_equals(m, opened));
	}

	/* Grow the owned copy and add a fresh bit: lineage must hold. */
	uint64_t newbit = (uint64_t)hegel_draw_int(tc, hegel_integers(0, U - 1));
	uint64_t rc = sm_add_grow(&oc, newbit);
	assert(rc == newbit);
	assert(sm_contains(oc, newbit, NULL));

	free(buf);
	if (opened != NULL)
		sm_free(opened);
	sm_free(c);
	sm_free(oc);
	sm_free(m);
	free(oracle);
}

/*
 * Property: the convenience constructors agree with the oracle, and
 * sm_to_array round-trips sm_create_from_array.
 */
static void
prop_constructors(hegel_test_case *tc, void *ctx)
{
	(void)ctx;

	/* singleton */
	uint64_t s = (uint64_t)hegel_draw_int(tc, hegel_integers(0, U - 1));
	sm_t *sg = sm_create_singleton(s);
	assert(sg != NULL);
	assert(sm_membership(sg) == SM_SINGLETON);
	assert(sm_singleton_member(sg) == s);
	assert(sm_cardinality(sg) == 1);
	assert(sm_contains(sg, s, NULL));
	sm_free(sg);

	/* from_range [lo, hi) */
	uint64_t lo = (uint64_t)hegel_draw_int(tc, hegel_integers(0, U - 1));
	uint64_t hi = (uint64_t)hegel_draw_int(tc, hegel_integers(0, U));
	if (lo > hi) {
		uint64_t t = lo;
		lo = hi;
		hi = t;
	}
	sm_t *r = sm_create_from_range(lo, hi);
	for (uint64_t b = 0; b < U; b++) {
		bool want = (b >= lo && b < hi);
		bool got = (r != NULL) ? sm_contains(r, b, NULL) : false;
		assert(got == want);
	}
	if (r != NULL)
		sm_free(r);

	/* from_array, then to_array round-trip (sorted, deduplicated
	 * ascending output). */
	int cnt = (int)hegel_draw_int(tc, hegel_integers(0, 64));
	bool *seen = calloc(U, sizeof(*seen));
	uint64_t *in = malloc((cnt ? cnt : 1) * sizeof(uint64_t));
	assert(seen != NULL && in != NULL);
	int nin = 0;
	for (int j = 0; j < cnt; j++) {
		uint64_t v = (uint64_t)hegel_draw_int(tc, hegel_integers(0, U - 1));
		in[nin++] = v;
		seen[v] = true;
	}
	sm_t *fa = sm_create_from_array(in, (size_t)nin);
	assert(fa != NULL);
	size_t want_card = 0;
	for (uint64_t b = 0; b < U; b++) {
		assert(sm_contains(fa, b, NULL) == seen[b]);
		if (seen[b])
			want_card++;
	}
	assert(sm_cardinality(fa) == want_card);

	/* to_array yields the set bits in ascending order. */
	size_t out_n = want_card;
	uint64_t *out = malloc((out_n ? out_n : 1) * sizeof(uint64_t));
	assert(out != NULL);
	sm_to_array(fa, out, &out_n);
	assert(out_n == want_card);
	size_t oi = 0;
	for (uint64_t b = 0; b < U; b++) {
		if (seen[b])
			assert(out[oi++] == b);
	}

	free(out);
	free(in);
	free(seen);
	sm_free(fa);
}

/*
 * Property: run-oriented construction.  prop_model draws individual
 * scattered bits, which rarely build the contiguous runs that become
 * RLE chunks and almost never produce the set-gap-set-within-one-window
 * shape that broke __sm_separate_rle_chunk (multi-run cardinality/rank
 * undercount and serialize crash, fixed post-5.2.0).  This property
 * instead draws a sequence of [start, start+len) runs separated by gaps
 * -- deliberately overlapping windows, straddling 2048-bit boundaries,
 * and leaving intra-window gaps -- then checks cardinality, rank over
 * random sub-ranges, serialize round-trip, and per-bit contains against
 * the dense bool[] oracle.  This is the shape that exercises the RLE
 * build / coalesce / separate paths hardest.
 */
static void
prop_runs_model(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	bool *oracle = calloc(U, sizeof(*oracle));
	assert(oracle != NULL);
	sm_t *m = fresh();

	/* 1..12 runs, each 1..4096 bits, gaps 0..2048 (so runs land in the
	 * same window, adjacent windows, or straddle boundaries). */
	int nruns = (int)hegel_draw_int(tc, hegel_integers(1, 12));
	uint64_t pos = (uint64_t)hegel_draw_int(tc, hegel_integers(0, 2048));
	for (int r = 0; r < nruns; r++) {
		uint64_t len = (uint64_t)hegel_draw_int(tc,
		    hegel_integers(1, 4096));
		for (uint64_t i = 0; i < len && pos + i < U; i++) {
			m = oracle_add(m, oracle, pos + i);
		}
		pos += len;
		uint64_t gap = (uint64_t)hegel_draw_int(tc,
		    hegel_integers(0, 2048));
		pos += gap;
		if (pos >= U)
			break;
	}

	/* Optionally clear a random sub-range (exercises separate on removes). */
	if (hegel_draw_int(tc, hegel_integers(0, 1))) {
		uint64_t clo = (uint64_t)hegel_draw_int(tc,
		    hegel_integers(0, U - 1));
		uint64_t chi = (uint64_t)hegel_draw_int(tc,
		    hegel_integers(0, U - 1));
		if (clo > chi) {
			uint64_t t = clo;
			clo = chi;
			chi = t;
		}
		for (uint64_t b = clo; b <= chi; b++) {
			sm_remove(m, b);
			oracle[b] = false;
		}
	}

	/* Cardinality + per-bit contains against the oracle. */
	size_t want_card = 0;
	for (uint64_t b = 0; b < U; b++) {
		assert(sm_contains(m, b, NULL) == oracle[b]);
		if (oracle[b])
			want_card++;
	}
	assert(sm_cardinality(m) == want_card);

	/* rank over several random inclusive sub-ranges. */
	for (int q = 0; q < 8; q++) {
		uint64_t lo = (uint64_t)hegel_draw_int(tc,
		    hegel_integers(0, U - 1));
		uint64_t hi = (uint64_t)hegel_draw_int(tc,
		    hegel_integers(0, U - 1));
		if (lo > hi) {
			uint64_t t = lo;
			lo = hi;
			hi = t;
		}
		size_t set = 0;
		for (uint64_t b = lo; b <= hi; b++)
			if (oracle[b])
				set++;
		assert(sm_rank(m, lo, hi, true) == set);
	}

	/* serialize round-trip must preserve the exact set (this crashed
	 * pre-fix on the corrupt multi-run stream). */
	size_t n = sm_serialized_size(m);
	uint8_t *buf = malloc(n ? n : 1);
	assert(buf != NULL);
	sm_serialize(m, buf, n);
	sm_t *e = sm_deserialize(buf, n);
	assert(e != NULL);
	assert(sm_cardinality(e) == want_card);
	for (uint64_t b = 0; b < U; b++)
		assert(sm_contains(e, b, NULL) == oracle[b]);

	free(buf);
	sm_free(e);
	sm_free(m);
	free(oracle);
}

/*
 * Property: the point-lookup / rank / select accelerators (Ideas 3, 4,
 * 5) agree with both the dense bool[] oracle and the plain sm_contains
 * / sm_rank / sm_select path on a random map.  Also exercises the
 * staleness fallback: after a mutation the locator (not rebuilt) must
 * still return correct results.
 */
static void
prop_accel(hegel_test_case *tc, void *ctx)
{
	(void)ctx;
	bool *oracle = calloc(U, sizeof(*oracle));
	assert(oracle != NULL);
	sm_t *m = draw_map(tc, oracle);

	/* Cardinality + ascending set-bit list from the oracle. */
	size_t card = 0;
	for (uint64_t b = 0; b < U; b++)
		if (oracle[b])
			card++;
	uint64_t *bits = malloc((card ? card : 1) * sizeof(uint64_t));
	assert(bits != NULL);
	size_t bi = 0;
	for (uint64_t b = 0; b < U; b++)
		if (oracle[b])
			bits[bi++] = b;

	/* Build a sorted probe list: every set bit +/- 1 and evenly-spaced
	 * gaps, all ascending. */
	size_t maxp = card * 3 + 128;
	uint64_t *probes = malloc(maxp * sizeof(uint64_t));
	assert(probes != NULL);
	size_t np = 0;
	for (size_t i = 0; i < card; i++) {
		if (bits[i] > 0)
			probes[np++] = bits[i] - 1;
		probes[np++] = bits[i];
		if (bits[i] + 1 < U)
			probes[np++] = bits[i] + 1;
	}
	for (int k = 0; k < 128; k++)
		probes[np++] = (uint64_t)k * (U / 128);
	/* insertion of gap probes may leave duplicates/order breaks; sort. */
	for (size_t i = 1; i < np; i++) {
		uint64_t v = probes[i];
		size_t j = i;
		while (j > 0 && probes[j - 1] > v) {
			probes[j] = probes[j - 1];
			j--;
		}
		probes[j] = v;
	}
	size_t w = 0;
	for (size_t i = 0; i < np; i++)
		if (w == 0 || probes[i] != probes[w - 1])
			probes[w++] = probes[i];
	np = w;

	/* Idea 5: batch contains == oracle == plain contains. */
	bool *many = malloc((np ? np : 1) * sizeof(bool));
	assert(many != NULL);
	sm_contains_many(m, probes, many, np);
	for (size_t i = 0; i < np; i++) {
		bool want = probes[i] < U ? oracle[probes[i]] : false;
		assert(many[i] == want);
		assert(many[i] == sm_contains(m, probes[i], NULL));
	}

	/* Idea 3: cached contains in scrambled order == oracle. */
	sm_cursor_cached_t cache = SM_CURSOR_CACHED_INIT;
	for (size_t i = 0; i < np; i++) {
		size_t j = (i * 2654435761u) % (np ? np : 1);
		bool want = probes[j] < U ? oracle[probes[j]] : false;
		assert(sm_contains_cached(m, probes[j], &cache) == want);
	}

	/* Idea 4: locator contains / rank(true) / select(true). */
	sm_locator_t *loc = sm_locator_build(m);
	if (loc == NULL) {
		/* Empty map. */
		assert(card == 0);
	} else {
		for (size_t i = 0; i < np; i++) {
			bool want = probes[i] < U ? oracle[probes[i]] : false;
			assert(sm_locator_contains(loc, probes[i]) == want);
		}
		for (size_t i = 0; i < np; i++) {
			uint64_t x = probes[i];
			assert(sm_locator_rank(loc, 0, x, true)
			    == sm_rank(m, 0, x, true));
			assert(sm_locator_rank(loc, 0, x, false)
			    == sm_rank(m, 0, x, false));
		}
		for (size_t n = 0; n < card; n++)
			assert(sm_locator_select(loc, n, true)
			    == sm_select(m, n, true));
		assert(sm_locator_select(loc, card, true)
		    == sm_select(m, card, true));

		/* Staleness: mutate without rebuilding; stay correct.
		 * Guarded out under SPARSEMAP_DIAGNOSTIC where a stale query
		 * asserts by contract. */
#ifndef SPARSEMAP_DIAGNOSTIC
		if (card > 0) {
			uint64_t hole = bits[card / 2];
			sm_remove(m, hole);
			for (size_t i = 0; i < np; i++) {
				assert(sm_locator_contains(loc, probes[i])
				    == sm_contains(m, probes[i], NULL));
			}
			assert(sm_locator_rank(loc, 0, hole, true)
			    == sm_rank(m, 0, hole, true));
		}
#endif
		sm_locator_free(loc);
	}

	free(many);
	free(probes);
	free(bits);
	sm_free(m);
	free(oracle);
}

static int
run(hegel_session *s, void (*fn)(hegel_test_case *, void *), const char *name)
{
	hegel_settings settings = HEGEL_DEFAULT_SETTINGS;
	settings.max_examples = 200;
	hegel_results r = hegel_run_test(s, fn, NULL, &settings);
	int failed = r.passed ? 0 : 1;
	if (failed)
		fprintf(stderr, "hegel property FAILED: %s\n", name);
	hegel_results_free(&r);
	return (failed);
}

int
main(void)
{
	/*
	 * One shared session for every property.  Each session spawns a
	 * Hegel server subprocess; spawning sixteen of them in sequence
	 * (a session per property) exhausts the hegel-c 0.x client and
	 * aborts mid-run.  A single session runs every property cleanly.
	 */
	hegel_session *s = hegel_session_new();
	if (s == NULL) {
		fprintf(stderr, "hegel: could not start session\n");
		return (1);
	}
	int rc = 0;
	/* Existing black-box properties. */
	rc |= run(s, prop_model, "model");
	rc |= run(s, prop_serialize_roundtrip, "serialize_roundtrip");
	rc |= run(s, prop_setops, "setops");
	rc |= run(s, prop_deserialize_no_crash, "deserialize_no_crash");
	/* Tier 1: rank/select, bidirectional iteration, cursor. */
	rc |= run(s, prop_rank_select, "rank_select");
	rc |= run(s, prop_prev_member, "prev_member");
	rc |= run(s, prop_cursor_interleaved, "cursor_interleaved");
	/* Tier 2: range ops + offset. */
	rc |= run(s, prop_range_ops, "range_ops");
	rc |= run(s, prop_extract_range, "extract_range");
	rc |= run(s, prop_offset, "offset");
	/* Tier 3: set-op identities + in-place equivalence. */
	rc |= run(s, prop_setop_identities, "setop_identities");
	rc |= run(s, prop_setop_inplace, "setop_inplace");
	/* Tier 4: hash/compare, lineage, constructors. */
	rc |= run(s, prop_hash_compare, "hash_compare");
	rc |= run(s, prop_lineage_roundtrip, "lineage_roundtrip");
	rc |= run(s, prop_constructors, "constructors");
	/* Run-oriented model: exercises RLE build / coalesce / separate on
	 * runs, gaps, and set-gap-set-within-a-window shapes. */
	rc |= run(s, prop_runs_model, "runs_model");
	/* Point-lookup / rank / select accelerators (Ideas 3, 4, 5). */
	rc |= run(s, prop_accel, "accel");
	hegel_session_free(s);
	return (rc);
}
