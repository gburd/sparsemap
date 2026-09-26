/* SPDX-License-Identifier: MIT */
/*
 * test_smallset_api.c -- full public-API cross-check with every index
 * kept strictly below SM_SMALL_MAX_BITS (1024), so the map is a
 * candidate for small-set mode for the whole suite.
 *
 * Small mode is not chosen purely by "max index < 1024": the library
 * picks the small flat-word form only when it is no larger than a
 * single sparse/RLE chunk (see __sm_small_is_better in sm.c).  A set
 * with a high max bit but few members (e.g. {1,100,500}) is cheaper as
 * a sparse chunk and stays in chunk mode even though every index is
 * below 1024.  This suite therefore asserts the mode explicitly per
 * case (SMALL, CHUNK, or ANY) using the confirmed layout, and cross-
 * checks every operation against a brute-force bool[] oracle either
 * way.
 *
 * Mines the PostgreSQL Bitmapset corner-case catalog: word boundaries
 * {0,31,32,63,64,65}, singleton/empty, idempotent add, sorted insert,
 * del-to-empty, del forcing nwords shrink, member-index across words,
 * set ops with BOTH operands low, equal-sets-different-build,
 * subset/superset/overlap/disjoint, offset with word-crossing / drop,
 * and hash stability/equality.
 *
 * Runs clean under plain, ASan+UBSan, and valgrind.
 */
#define SM_EXPOSE_STRUCT 1
#include <sm.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(c)                                                              \
	do {                                                                  \
		checks++;                                                      \
		if (!(c)) {                                                    \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
			    __LINE__, #c);                                    \
			failures++;                                           \
		}                                                             \
	} while (0)

/* mode expectation for verify(): don't-care / must-be-small / must-be-chunk */
#define MODE_ANY   (-1)
#define MODE_CHUNK 0
#define MODE_SMALL 1

/* small universe -- every index used stays < 1024. */
#define U 1024u

/* Is the map physically in small-set mode?  The header word's top bit
 * marks it (mirrors the library's internal __sm_is_small). */
static int
is_small(const sm_t *m)
{
	if (m == NULL || m->m_data == NULL || m->m_data_used < 8)
		return (0);
	uint64_t hdr;
	memcpy(&hdr, m->m_data, 8);
	return ((hdr & ((uint64_t)1 << 63)) != 0);
}

/* Build a map from an index list via the grow-safe handle idiom. */
static sm_t *
build(const uint64_t *idx, size_t n)
{
	sm_t *m = sm_create(64);
	for (size_t i = 0; i < n; i++)
		CHECK(sm_add_grow(&m, idx[i]) == idx[i]);
	return (m);
}

/* ---- brute-force oracle over the small universe ---- */
struct oracle {
	bool bit[U];
};

static void
oracle_from(struct oracle *o, const uint64_t *idx, size_t n)
{
	memset(o->bit, 0, sizeof(o->bit));
	for (size_t i = 0; i < n; i++)
		if (idx[i] < U)
			o->bit[idx[i]] = true;
}

static size_t
oracle_card(const struct oracle *o)
{
	size_t c = 0;
	for (uint64_t x = 0; x < U; x++)
		if (o->bit[x])
			c++;
	return (c);
}

static void
apply_op_oracle(struct oracle *r, const struct oracle *a,
    const struct oracle *b, int op)
{
	for (uint64_t x = 0; x < U; x++) {
		switch (op) {
		case 0: r->bit[x] = a->bit[x] || b->bit[x]; break;  /* union */
		case 1: r->bit[x] = a->bit[x] && b->bit[x]; break;  /* inter */
		case 2: r->bit[x] = a->bit[x] && !b->bit[x]; break; /* diff  */
		case 3: r->bit[x] = a->bit[x] ^ b->bit[x]; break;   /* xor   */
		}
	}
}

static size_t
oracle_card_op(const struct oracle *a, const struct oracle *b, int op)
{
	struct oracle r;
	apply_op_oracle(&r, a, b, op);
	return (oracle_card(&r));
}

/* Full membership/order/rank/select/iterate cross-check of one map
 * against its oracle.  `mode` asserts small/chunk residence when != ANY. */
static void
verify(const char *name, sm_t *m, const struct oracle *o, int mode)
{
	size_t card = oracle_card(o);
	if (mode == MODE_SMALL)
		CHECK(is_small(m));
	else if (mode == MODE_CHUNK)
		CHECK(!is_small(m));

	for (uint64_t x = 0; x < U; x++)
		CHECK(sm_contains(m, x, NULL) == o->bit[x]);
	CHECK(sm_cardinality(m) == card);
	CHECK(sm_is_empty(m) == (card == 0));

	/* membership / singleton_member */
	sm_membership_t mem = sm_membership(m);
	if (card == 0) {
		CHECK(mem == SM_EMPTY);
		CHECK(sm_singleton_member(m) == SM_IDX_MAX);
	} else if (card == 1) {
		CHECK(mem == SM_SINGLETON);
		uint64_t only = SM_IDX_MAX;
		for (uint64_t x = 0; x < U; x++)
			if (o->bit[x])
				only = x;
		CHECK(sm_singleton_member(m) == only);
	} else {
		CHECK(mem == SM_MULTIPLE);
		CHECK(sm_singleton_member(m) == SM_IDX_MAX);
	}

	/* min / max */
	if (card) {
		uint64_t mn = SM_IDX_MAX, mx = 0;
		for (uint64_t x = 0; x < U; x++)
			if (o->bit[x]) {
				if (mn == SM_IDX_MAX)
					mn = x;
				mx = x;
			}
		CHECK(sm_minimum(m) == mn);
		CHECK(sm_maximum(m) == mx);
	}

	/* next_member ascending; member_index (rank/select) */
	{
		uint64_t prev = SM_IDX_MAX;
		size_t k = 0;
		for (uint64_t x = 0; x < U; x++) {
			if (!o->bit[x])
				continue;
			CHECK(sm_next_member(m, prev, NULL) == x);
			CHECK(sm_select(m, k, true) == x);
			CHECK(sm_rank(m, 0, x, true) == k + 1);
			prev = x;
			k++;
		}
		CHECK(sm_next_member(m, prev, NULL) == SM_IDX_MAX);
		CHECK(sm_select(m, card, true) == SM_IDX_MAX);
	}

	/* prev_member descending */
	{
		uint64_t upper = SM_IDX_MAX;
		for (uint64_t xx = U; xx-- > 0;) {
			if (!o->bit[xx])
				continue;
			CHECK(sm_prev_member(m, upper, NULL) == xx);
			upper = xx;
			if (xx == 0)
				break;
		}
	}

	/* running rank[0,x] inclusive over the whole universe */
	{
		size_t acc = 0;
		for (uint64_t x = 0; x < U; x++) {
			CHECK(sm_rank(m, 0, x, true) ==
			    acc + (o->bit[x] ? 1 : 0));
			if (o->bit[x])
				acc++;
		}
	}

	/* to_array ascending */
	{
		uint64_t arr[U];
		size_t cnt = U;
		sm_to_array(m, arr, &cnt);
		CHECK(cnt == card);
		size_t k = 0;
		for (uint64_t x = 0; x < U; x++)
			if (o->bit[x])
				CHECK(arr[k++] == x);
	}

	/* copy / hash / compare / serialize round-trip */
	{
		sm_t *cp = sm_copy(m);
		CHECK(sm_equals(m, cp));
		CHECK(sm_hash(m) == sm_hash(cp));
		CHECK(sm_compare(m, cp) == 0);
		size_t need = sm_serialized_size(m);
		uint8_t *buf = malloc(need);
		CHECK(sm_serialize(m, buf, need) == need);
		sm_t *back = sm_deserialize(buf, need);
		if (card == 0) {
			CHECK(back == NULL || sm_equals(m, back));
		} else {
			CHECK(back != NULL);
			CHECK(sm_equals(m, back));
			CHECK(sm_hash(m) == sm_hash(back));
		}
		if (back)
			sm_free(back);
		free(buf);
		sm_free(cp);
	}

	CHECK(sm_validate(m));
	fprintf(stderr, "  verify %-22s card=%3zu small=%d ok\n", name, card,
	    is_small(m));
}

/* Build from list, verify against its own oracle at the given mode. */
static void
build_verify(const char *name, const uint64_t *idx, size_t n, int mode)
{
	struct oracle o;
	oracle_from(&o, idx, n);
	sm_t *m = build(idx, n);
	verify(name, m, &o, mode);
	sm_free(m);
}

/* ------------------------------------------------------------------ */
/* Membership / singleton / empty / word boundaries                   */
/* ------------------------------------------------------------------ */
static void
test_membership_and_boundaries(void)
{
	build_verify("empty", NULL, 0, MODE_ANY);
	build_verify("{0}", (uint64_t[]){ 0 }, 1, MODE_SMALL);
	/* {500}: high max bit, one member -> cheaper as a chunk. */
	build_verify("{500}", (uint64_t[]){ 500 }, 1, MODE_CHUNK);
	/* the six word-boundary indices around words 0 and 1 (all low) */
	build_verify("wordbdy", (uint64_t[]){ 0, 31, 32, 63, 64, 65 }, 6,
	    MODE_SMALL);
	/* spanning word 0/1 boundary */
	build_verify("{63,64}", (uint64_t[]){ 63, 64 }, 2, MODE_SMALL);
	/* member across words {100,200} stays small (3 words <= chunk) */
	build_verify("{100,200}", (uint64_t[]){ 100, 200 }, 2, MODE_SMALL);

	/* add_range(30,34) then del 32 -- hole in the middle of a word */
	{
		struct oracle o;
		oracle_from(&o, (uint64_t[]){ 30, 31, 32, 33 }, 4);
		sm_t *m = sm_create(64);
		CHECK(sm_add_range(m, 30, 34)); /* [30,34) */
		CHECK(sm_remove(m, 32) == 32);
		o.bit[32] = false;
		verify("range30-34,del32", m, &o, MODE_SMALL);
		sm_free(m);
	}
	/* del across a word boundary: {60..67}, remove {63,64} */
	{
		struct oracle o;
		sm_t *m = sm_create(64);
		CHECK(sm_add_range(m, 60, 68));
		oracle_from(&o, (uint64_t[]){ 60, 61, 62, 63, 64, 65, 66, 67 },
		    8);
		CHECK(sm_remove(m, 63) == 63);
		CHECK(sm_remove(m, 64) == 64);
		o.bit[63] = o.bit[64] = false;
		verify("delacross-wordbdy", m, &o, MODE_SMALL);
		sm_free(m);
	}
	/* is_member on empty / singleton / absent */
	{
		sm_t *e = sm_create(64);
		CHECK(!sm_contains(e, 0, NULL));
		CHECK(sm_membership(e) == SM_EMPTY);
		sm_free(e);
		sm_t *one = sm_create_singleton(0);
		CHECK(one && is_small(one));
		CHECK(sm_contains(one, 0, NULL));
		CHECK(!sm_contains(one, 1, NULL));
		CHECK(sm_membership(one) == SM_SINGLETON);
		CHECK(sm_singleton_member(one) == 0);
		sm_free(one);
	}
}

/* ------------------------------------------------------------------ */
/* member_index across different words                                */
/* ------------------------------------------------------------------ */
static void
test_member_index(void)
{
	uint64_t s[] = { 100, 200 };
	sm_t *m = build(s, 2);
	CHECK(sm_rank(m, 0, 100, true) == 1);
	CHECK(sm_rank(m, 0, 200, true) == 2);
	CHECK(sm_select(m, 0, true) == 100);
	CHECK(sm_select(m, 1, true) == 200);
	CHECK(sm_rank(m, 0, 150, true) == 1); /* index of an absent gap */
	CHECK(!sm_contains(m, 150, NULL));
	CHECK(is_small(m));
	sm_free(m);

	uint64_t t[] = { 10, 20, 30, 40, 50 };
	sm_t *n = build(t, 5);
	CHECK(sm_select(n, 0, true) == 10);
	CHECK(sm_select(n, 2, true) == 30);
	CHECK(sm_select(n, 4, true) == 50);
	sm_free(n);
}

/* ------------------------------------------------------------------ */
/* Add: idempotent, sorted-insert, growing                            */
/* ------------------------------------------------------------------ */
static void
test_add(void)
{
	struct oracle o;
	sm_t *m = sm_create(64);
	/* idempotent add of an already-present bit */
	CHECK(sm_add_grow(&m, 7) == 7);
	CHECK(sm_add_grow(&m, 7) == 7);
	CHECK(sm_cardinality(m) == 1);
	/* add that must keep sorted order (add 5 to {10}) */
	sm_clear(m);
	CHECK(sm_add_grow(&m, 10) == 10);
	CHECK(sm_add_grow(&m, 5) == 5);
	oracle_from(&o, (uint64_t[]){ 5, 10 }, 2);
	verify("sorted-insert", m, &o, MODE_SMALL);
	/* growing the set within the small span (dense-ish, i+=7 to 300) */
	sm_clear(m);
	{
		size_t n = 0;
		uint64_t idx[U];
		for (uint64_t i = 0; i < 300; i += 7) {
			CHECK(sm_add_grow(&m, i) == i);
			idx[n++] = i;
		}
		oracle_from(&o, idx, n);
		verify("growing-set", m, &o, MODE_SMALL);
	}
	sm_free(m);
}

/* ------------------------------------------------------------------ */
/* Del / shrink                                                       */
/* ------------------------------------------------------------------ */
static void
test_del(void)
{
	struct oracle o;
	/* del the only member -> empty */
	sm_t *m = sm_create_singleton(42);
	CHECK(sm_remove(m, 42) == 42);
	CHECK(sm_is_empty(m));
	CHECK(is_small(m));
	oracle_from(&o, NULL, 0);
	verify("del-to-empty", m, &o, MODE_ANY);
	sm_free(m);

	/* del forcing the stored word-count to shrink: {1,200} del 200 ->
	 * {1} now occupies only word 0.  (Was chunk -> demotes to small.) */
	uint64_t s[] = { 1, 200 };
	m = build(s, 2);
	CHECK(sm_remove(m, 200) == 200);
	oracle_from(&o, (uint64_t[]){ 1 }, 1);
	verify("nwords-shrink", m, &o, MODE_SMALL);
	sm_free(m);

	/* del non-present -> no-op (returns idx, map unchanged) */
	m = build((uint64_t[]){ 3, 9 }, 2);
	CHECK(sm_remove(m, 100) == 100);
	oracle_from(&o, (uint64_t[]){ 3, 9 }, 2);
	verify("del-absent-noop", m, &o, MODE_SMALL);
	sm_free(m);

	/* add_range / remove_range / flip_range small-mode paths */
	m = sm_create(64);
	CHECK(sm_add_range(m, 0, 100));
	CHECK(sm_remove_range(m, 40, 60)); /* clear [40,60) */
	oracle_from(&o, NULL, 0);
	for (uint64_t x = 0; x < 100; x++)
		if (x < 40 || x >= 60)
			o.bit[x] = true;
	verify("add_range-remove_range", m, &o, MODE_SMALL);
	/* flip_range: toggle [0,100) -> removed [40,60) come back, rest clear */
	CHECK(sm_flip_range(m, 0, 100));
	memset(o.bit, 0, sizeof(o.bit));
	for (uint64_t x = 40; x < 60; x++)
		o.bit[x] = true;
	verify("flip_range", m, &o, MODE_SMALL);
	sm_free(m);
}

/* ------------------------------------------------------------------ */
/* Destructive iteration: pop_first / pop_last                        */
/* ------------------------------------------------------------------ */
static void
test_pop(void)
{
	/* {3,40,63,64,500} is chunk-mode (high max, few members); the pop
	 * results must still be exact. */
	uint64_t s[] = { 3, 40, 63, 64, 500 };
	sm_t *m = build(s, 5);
	CHECK(sm_pop_first(m) == 3);
	CHECK(sm_pop_last(m) == 500);
	CHECK(sm_pop_first(m) == 40);
	CHECK(sm_pop_last(m) == 64);
	CHECK(sm_pop_first(m) == 63);
	CHECK(sm_pop_first(m) == SM_IDX_MAX);
	CHECK(sm_is_empty(m));
	sm_free(m);

	/* a confirmed-small stack: {5,10,20} */
	uint64_t t[] = { 5, 10, 20 };
	sm_t *n = build(t, 3);
	CHECK(is_small(n));
	CHECK(sm_pop_last(n) == 20);
	CHECK(sm_pop_last(n) == 10);
	CHECK(sm_pop_last(n) == 5);
	CHECK(sm_pop_last(n) == SM_IDX_MAX);
	sm_free(n);
}

/* ------------------------------------------------------------------ */
/* Set operations, BOTH operands low                                  */
/* ------------------------------------------------------------------ */
static void
check_result(const char *name, sm_t *res, const struct oracle *exp)
{
	size_t card = oracle_card(exp);
	if (card == 0) {
		CHECK(res == NULL || sm_cardinality(res) == 0);
		if (res) {
			CHECK(sm_validate(res));
			sm_free(res);
		}
		return;
	}
	CHECK(res != NULL);
	/* The result's mode is the library's choice; verify correctness. */
	verify(name, res, exp, MODE_ANY);
	sm_free(res);
}

static void
test_setops(void)
{
	uint64_t A[] = { 0, 1, 2, 5, 63, 64, 300 }; /* small */
	uint64_t B[] = { 2, 5, 64, 65, 300, 700 };  /* chunk (max 700, sparse) */
	struct oracle oa, ob, oe;
	oracle_from(&oa, A, sizeof(A) / sizeof(A[0]));
	oracle_from(&ob, B, sizeof(B) / sizeof(B[0]));
	sm_t *a = build(A, sizeof(A) / sizeof(A[0]));
	sm_t *b = build(B, sizeof(B) / sizeof(B[0]));
	CHECK(is_small(a)); /* A stays small; B may not */

	apply_op_oracle(&oe, &oa, &ob, 0);
	check_result("union", sm_union(a, b), &oe);
	apply_op_oracle(&oe, &oa, &ob, 1);
	check_result("intersect", sm_intersection(a, b), &oe);
	apply_op_oracle(&oe, &oa, &ob, 2);
	check_result("difference", sm_difference(a, b), &oe);
	apply_op_oracle(&oe, &oa, &ob, 3);
	check_result("xor", sm_xor(a, b), &oe);

	CHECK(sm_union_cardinality(a, b) == oracle_card_op(&oa, &ob, 0));
	CHECK(sm_intersection_cardinality(a, b) == oracle_card_op(&oa, &ob, 1));
	CHECK(sm_difference_cardinality(a, b) == oracle_card_op(&oa, &ob, 2));
	CHECK(sm_xor_cardinality(a, b) == oracle_card_op(&oa, &ob, 3));
	CHECK(sm_nonempty_difference(a, b) ==
	    (oracle_card_op(&oa, &ob, 2) > 0));

	sm_free(a);
	sm_free(b);

	/* Two confirmed-small operands: {1,2,5,63} op {2,5,64} */
	{
		uint64_t P[] = { 1, 2, 5, 63 };
		uint64_t Q[] = { 2, 5, 64 };
		struct oracle op, oq, orr;
		oracle_from(&op, P, 4);
		oracle_from(&oq, Q, 3);
		sm_t *p = build(P, 4), *q = build(Q, 3);
		CHECK(is_small(p) && is_small(q));
		apply_op_oracle(&orr, &op, &oq, 0);
		check_result("small-union", sm_union(p, q), &orr);
		apply_op_oracle(&orr, &op, &oq, 1);
		check_result("small-inter", sm_intersection(p, q), &orr);
		apply_op_oracle(&orr, &op, &oq, 2);
		check_result("small-diff", sm_difference(p, q), &orr);
		apply_op_oracle(&orr, &op, &oq, 3);
		check_result("small-xor", sm_xor(p, q), &orr);
		sm_free(p);
		sm_free(q);
	}

	/* disjoint intersect -> empty */
	{
		sm_t *x = build((uint64_t[]){ 1, 2 }, 2);
		sm_t *y = build((uint64_t[]){ 100, 300 }, 2);
		sm_t *r = sm_intersection(x, y);
		CHECK(r == NULL || sm_cardinality(r) == 0);
		if (r)
			sm_free(r);
		sm_free(x);
		sm_free(y);
	}
	/* difference to empty (subset \ superset) */
	{
		sm_t *x = build((uint64_t[]){ 1, 2, 3, 4, 5 }, 5);
		sm_t *y = build((uint64_t[]){ 1, 3 }, 2);
		sm_t *r = sm_difference(y, x); /* {1,3} \ {1..5} = {} */
		CHECK(r == NULL || sm_cardinality(r) == 0);
		if (r)
			sm_free(r);
		sm_free(x);
		sm_free(y);
	}
	/* identical-set difference -> empty */
	{
		sm_t *x = build((uint64_t[]){ 7, 70, 700 }, 3);
		sm_t *y = sm_copy(x);
		sm_t *r = sm_difference(x, y);
		CHECK(r == NULL || sm_cardinality(r) == 0);
		if (r)
			sm_free(r);
		sm_free(x);
		sm_free(y);
	}
	/* overlapping-range union: [0,15) U [10,20) */
	{
		sm_t *x = sm_create(64), *y = sm_create(64);
		CHECK(sm_add_range(x, 0, 15));
		CHECK(sm_add_range(y, 10, 20));
		struct oracle ex, ey, er;
		oracle_from(&ex, NULL, 0);
		for (uint64_t i = 0; i < 15; i++)
			ex.bit[i] = true;
		oracle_from(&ey, NULL, 0);
		for (uint64_t i = 10; i < 20; i++)
			ey.bit[i] = true;
		apply_op_oracle(&er, &ex, &ey, 0);
		check_result("rangeUnion", sm_union(x, y), &er);
		sm_free(x);
		sm_free(y);
	}
	/* [0,100) diff [50,150) = [0,50) */
	{
		sm_t *x = sm_create(64), *y = sm_create(64);
		CHECK(sm_add_range(x, 0, 100));
		CHECK(sm_add_range(y, 50, 150));
		struct oracle ex, ey, er;
		oracle_from(&ex, NULL, 0);
		for (uint64_t i = 0; i < 100; i++)
			ex.bit[i] = true;
		oracle_from(&ey, NULL, 0);
		for (uint64_t i = 50; i < 150; i++)
			ey.bit[i] = true;
		apply_op_oracle(&er, &ex, &ey, 2);
		check_result("rangeDiff", sm_difference(x, y), &er);
		sm_free(x);
		sm_free(y);
	}

	/* or/and/andnot aliases match union/intersection/difference */
	{
		sm_t *x = build((uint64_t[]){ 1, 2, 3 }, 3);
		sm_t *y = build((uint64_t[]){ 2, 3, 4 }, 3);
		sm_t *o1 = sm_or(x, y), *o2 = sm_union(x, y);
		sm_t *a1 = sm_and(x, y), *a2 = sm_intersection(x, y);
		sm_t *n1 = sm_andnot(x, y), *n2 = sm_difference(x, y);
		CHECK(sm_equals(o1, o2));
		CHECK(sm_equals(a1, a2));
		CHECK(sm_equals(n1, n2));
		sm_free(o1); sm_free(o2); sm_free(a1); sm_free(a2);
		sm_free(n1); sm_free(n2);
		sm_free(x); sm_free(y);
	}

	/* in-place variants: dst := dst OP src */
	{
		sm_t *dst = build((uint64_t[]){ 1, 2, 3 }, 3);
		sm_t *src = build((uint64_t[]){ 3, 4, 5 }, 3);
		dst = sm_union_inplace(dst, src);
		struct oracle e;
		oracle_from(&e, (uint64_t[]){ 1, 2, 3, 4, 5 }, 5);
		verify("union_inplace", dst, &e, MODE_ANY);
		dst = sm_intersection_inplace(dst, src); /* {3,4,5} */
		oracle_from(&e, (uint64_t[]){ 3, 4, 5 }, 3);
		verify("intersection_inplace", dst, &e, MODE_ANY);
		dst = sm_difference_inplace(dst, src); /* {} */
		CHECK(sm_is_empty(dst));
		sm_free(dst);
		sm_free(src);
	}
}

/* ------------------------------------------------------------------ */
/* equal: same logical set, different internal build                  */
/* ------------------------------------------------------------------ */
static void
test_equal_and_compare(void)
{
	/* empty == empty */
	sm_t *e1 = sm_create(64), *e2 = sm_create(64);
	CHECK(sm_equals(e1, e2));
	CHECK(sm_compare(e1, e2) == 0);
	CHECK(sm_hash(e1) == sm_hash(e2));
	sm_add_grow(&e2, 5);
	CHECK(!sm_equals(e1, e2)); /* empty != nonempty */
	sm_free(e1);
	sm_free(e2);

	/* SAME logical set built three ways over a low sparse spread; all
	 * three stay small.  Must be equal, compare 0, hash equal. */
	uint64_t idx[] = { 3, 7, 40, 63, 64, 200 };
	sm_t *asc = sm_create(64);
	for (size_t i = 0; i < 6; i++)
		sm_add_grow(&asc, idx[i]);
	sm_t *desc = sm_create(64);
	for (size_t i = 6; i-- > 0;)
		sm_add_grow(&desc, idx[i]);
	sm_t *fa = sm_create_from_array(idx, 6);
	CHECK(sm_equals(asc, desc));
	CHECK(sm_equals(asc, fa));
	CHECK(sm_compare(asc, desc) == 0);
	CHECK(sm_hash(asc) == sm_hash(desc));
	CHECK(sm_hash(asc) == sm_hash(fa));
	CHECK(is_small(asc) && is_small(desc) && is_small(fa));
	sm_free(asc);
	sm_free(desc);
	sm_free(fa);

	/* Equal logical set, one built small and one built to be chunk:
	 * {5,64} directly (small) vs the same set derived by removing a
	 * high anchor from {5,64,900} (leaves {5,64}, may stay chunk).
	 * They must compare equal regardless of representation. */
	sm_t *small = build((uint64_t[]){ 5, 64 }, 2);
	sm_t *viachunk = build((uint64_t[]){ 5, 64, 900 }, 3);
	CHECK(sm_remove(viachunk, 900) == 900);
	CHECK(sm_equals(small, viachunk));
	CHECK(sm_compare(small, viachunk) == 0);
	CHECK(sm_hash(small) == sm_hash(viachunk));
	sm_free(small);
	sm_free(viachunk);

	/* compare orders lexicographically: {1} < {2} < {1,2} */
	sm_t *s1 = build((uint64_t[]){ 1 }, 1);
	sm_t *s2 = build((uint64_t[]){ 2 }, 1);
	sm_t *s12 = build((uint64_t[]){ 1, 2 }, 2);
	CHECK(sm_compare(s1, s2) < 0);
	CHECK(sm_compare(s2, s1) > 0);
	CHECK(sm_compare(s1, s12) < 0);
	sm_free(s1);
	sm_free(s2);
	sm_free(s12);
}

/* ------------------------------------------------------------------ */
/* subset / superset / overlap / disjoint                             */
/* ------------------------------------------------------------------ */
static void
test_subset_overlap(void)
{
	sm_t *sub = build((uint64_t[]){ 2, 5, 64 }, 3);
	sm_t *sup = build((uint64_t[]){ 2, 5, 6, 64, 300 }, 5);
	sm_t *eq = sm_copy(sub);
	sm_t *dis = build((uint64_t[]){ 700, 800 }, 2);
	sm_t *part = build((uint64_t[]){ 5, 700 }, 2);

	CHECK(sm_is_subset(sub, sup));
	CHECK(!sm_is_subset(sup, sub));
	CHECK(sm_is_superset(sup, sub));
	CHECK(sm_is_subset(sub, eq)); /* equal counts as subset */
	CHECK(sm_subset_compare(sub, sup) == SM_REL_SUBSET_A);
	CHECK(sm_subset_compare(sup, sub) == SM_REL_SUBSET_B);
	CHECK(sm_subset_compare(sub, eq) == SM_REL_EQUAL);
	CHECK(sm_subset_compare(sub, dis) == SM_REL_DIFFERENT);

	CHECK(sm_overlap(sub, sup));
	CHECK(!sm_overlap(sub, dis));
	CHECK(sm_overlap(sub, part)); /* share 5 */

	/* jaccard: sub INT part = {5} (1), union = {2,5,64,700} (4) -> 0.25 */
	double j = sm_jaccard_index(sub, part);
	CHECK(j > 0.24 && j < 0.26);

	sm_free(sub);
	sm_free(sup);
	sm_free(eq);
	sm_free(dis);
	sm_free(part);
}

/* ------------------------------------------------------------------ */
/* offset: word-crossing, whole-word, below-0 drop                    */
/* ------------------------------------------------------------------ */
static void
offset_oracle(struct oracle *r, const struct oracle *s, int64_t off)
{
	memset(r->bit, 0, sizeof(r->bit));
	for (uint64_t x = 0; x < U; x++) {
		if (!s->bit[x])
			continue;
		int64_t nx = (int64_t)x + off;
		if (nx >= 0 && nx < (int64_t)U)
			r->bit[nx] = true;
	}
}

static void
check_offset(sm_t *m, const struct oracle *base, int64_t off)
{
	struct oracle exp;
	offset_oracle(&exp, base, off);
	sm_t *r = sm_offset(m, (ssize_t)off);
	size_t card = oracle_card(&exp);
	if (card == 0) {
		CHECK(r == NULL || sm_cardinality(r) == 0);
		if (r)
			sm_free(r);
		return;
	}
	CHECK(r != NULL);
	if (r) {
		for (uint64_t x = 0; x < U; x++)
			CHECK(sm_contains(r, x, NULL) == exp.bit[x]);
		CHECK(sm_cardinality(r) == card);
		CHECK(sm_validate(r));
		sm_free(r);
	}
}

static void
test_offset(void)
{
	/* {1,3,5} by +1/0/-1 */
	{
		struct oracle o;
		uint64_t s[] = { 1, 3, 5 };
		oracle_from(&o, s, 3);
		sm_t *m = build(s, 3);
		check_offset(m, &o, +1);
		check_offset(m, &o, 0);
		check_offset(m, &o, -1);
		sm_free(m);
	}
	/* {31,32,63,64} by +1/0/-1 -- word-boundary crossing */
	{
		struct oracle o;
		uint64_t s[] = { 31, 32, 63, 64 };
		oracle_from(&o, s, 4);
		sm_t *m = build(s, 4);
		check_offset(m, &o, +1);
		check_offset(m, &o, 0);
		check_offset(m, &o, -1);
		sm_free(m);
	}
	/* {1,2,3} by +64 -- whole-word shift */
	{
		struct oracle o;
		uint64_t s[] = { 1, 2, 3 };
		oracle_from(&o, s, 3);
		sm_t *m = build(s, 3);
		check_offset(m, &o, +64);
		sm_free(m);
	}
	/* {65,66,67} by -64 */
	{
		struct oracle o;
		uint64_t s[] = { 65, 66, 67 };
		oracle_from(&o, s, 3);
		sm_t *m = build(s, 3);
		check_offset(m, &o, -64);
		sm_free(m);
	}
	/* members going below 0 silently drop: {1,2,3} by -2 -> {0,1} */
	{
		struct oracle o;
		uint64_t s[] = { 1, 2, 3 };
		oracle_from(&o, s, 3);
		sm_t *m = build(s, 3);
		check_offset(m, &o, -2);
		sm_free(m);
	}
	/* all bits drop below 0 -> NULL */
	{
		sm_t *m = build((uint64_t[]){ 1, 2 }, 2);
		sm_t *r = sm_offset(m, -100);
		CHECK(r == NULL);
		sm_free(m);
	}
	/* positive overflow is unreachable from a small map (max bit < 1024
	 * + max ssize_t offset stays < 2^64); the ERANGE path is exercised
	 * in test_offset_overflow.c and the transition suite. */
}

/* ------------------------------------------------------------------ */
/* hash: equal sets hash equal, differ usually, empty stable          */
/* ------------------------------------------------------------------ */
static void
test_hash(void)
{
	sm_t *e1 = sm_create(64), *e2 = sm_create(64);
	CHECK(sm_hash(e1) == sm_hash(e2)); /* empty stable */
	sm_free(e1);
	sm_free(e2);

	sm_t *a = build((uint64_t[]){ 1, 100, 500 }, 3);
	sm_t *b = build((uint64_t[]){ 500, 100, 1 }, 3); /* same set */
	sm_t *c = build((uint64_t[]){ 1, 100, 501 }, 3); /* differs by one */
	CHECK(sm_hash(a) == sm_hash(b));
	CHECK(sm_hash(a) != sm_hash(c));
	sm_free(a);
	sm_free(b);
	sm_free(c);
}

/* ------------------------------------------------------------------ */
/* Constructors: singleton / from_range / from_array                  */
/* ------------------------------------------------------------------ */
static void
test_constructors(void)
{
	sm_t *s = sm_create_singleton(63);
	CHECK(s && is_small(s));
	CHECK(sm_cardinality(s) == 1 && sm_contains(s, 63, NULL));
	sm_free(s);

	sm_t *r = sm_create_from_range(10, 20); /* [10,20) */
	CHECK(r && is_small(r));
	struct oracle o;
	oracle_from(&o, NULL, 0);
	for (uint64_t x = 10; x < 20; x++)
		o.bit[x] = true;
	verify("from_range", r, &o, MODE_SMALL);
	sm_free(r);

	/* {2,4,...512}: sparse spread with a high max bit -> chunk mode. */
	uint64_t arr[] = { 2, 4, 8, 16, 32, 64, 128, 256, 512 };
	sm_t *fa = sm_create_from_array(arr, 9);
	CHECK(fa != NULL);
	oracle_from(&o, arr, 9);
	verify("from_array", fa, &o, MODE_ANY);
	sm_free(fa);
}

int
main(void)
{
	failures = 0;
	checks = 0;

	test_membership_and_boundaries();
	test_member_index();
	test_add();
	test_del();
	test_pop();
	test_setops();
	test_equal_and_compare();
	test_subset_overlap();
	test_offset();
	test_hash();
	test_constructors();

	fprintf(stderr, "test_smallset_api: %d checks, %d failure(s)\n", checks,
	    failures);
	return (failures ? 1 : 0);
}
