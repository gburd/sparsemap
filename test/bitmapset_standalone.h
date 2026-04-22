/*-------------------------------------------------------------------------
 *
 * bitmapset_standalone.h
 *	  Standalone version of PostgreSQL generic bitmap set package
 *
 * Adapted from PostgreSQL's bitmapset.h to compile without any
 * PostgreSQL dependencies. Uses standard C library and GCC builtins only.
 *
 * A bitmap set can represent any set of nonnegative integers, although
 * it is mainly intended for sets where the maximum value is not large,
 * say at most a few hundred.  By convention, we always represent the
 * empty set by a NULL pointer.
 *
 * Original copyright (c) 2003-2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef BITMAPSET_STANDALONE_H
#define BITMAPSET_STANDALONE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

/*
 * Data representation
 *
 * We always use 64-bit words (this standalone version targets 64-bit).
 */
#define BITS_PER_BITMAPWORD 64
typedef uint64_t bitmapword;		/* must be an unsigned type */
typedef int64_t signedbitmapword;	/* must be the matching signed type */

typedef struct Bitmapset
{
	int			nwords;			/* number of words in array */
	bitmapword	words[];		/* really [nwords] */
} Bitmapset;


/* result of bms_subset_compare */
typedef enum
{
	BMS_EQUAL,					/* sets are equal */
	BMS_SUBSET1,				/* first set is a subset of the second */
	BMS_SUBSET2,				/* second set is a subset of the first */
	BMS_DIFFERENT,				/* neither set is a subset of the other */
} BMS_Comparison;

/* result of bms_membership */
typedef enum
{
	BMS_EMPTY_SET,				/* 0 members */
	BMS_SINGLETON,				/* 1 member */
	BMS_MULTIPLE,				/* >1 member */
} BMS_Membership;

/* Bit-twiddling functions using GCC builtins for 64-bit words */
#define bmw_leftmost_one_pos(w)		(63 - __builtin_clzll(w))
#define bmw_rightmost_one_pos(w)	__builtin_ctzll(w)
#define bmw_popcount(w)				__builtin_popcountll(w)


/*
 * function prototypes
 */

Bitmapset *bms_copy(const Bitmapset *a);
bool bms_equal(const Bitmapset *a, const Bitmapset *b);
int bms_compare(const Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_make_singleton(int x);
void bms_free(Bitmapset *a);

Bitmapset *bms_union(const Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_intersect(const Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_difference(const Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_offset_members(const Bitmapset *a, int offset);
bool bms_is_subset(const Bitmapset *a, const Bitmapset *b);
BMS_Comparison bms_subset_compare(const Bitmapset *a, const Bitmapset *b);
bool bms_is_member(int x, const Bitmapset *a);
int bms_member_index(Bitmapset *a, int x);
bool bms_overlap(const Bitmapset *a, const Bitmapset *b);
bool bms_nonempty_difference(const Bitmapset *a, const Bitmapset *b);
int bms_singleton_member(const Bitmapset *a);
bool bms_get_singleton_member(const Bitmapset *a, int *member);
int bms_num_members(const Bitmapset *a);

/* optimized tests when we don't need to know exact membership count: */
BMS_Membership bms_membership(const Bitmapset *a);

/* NULL is now the only allowed representation of an empty bitmapset */
#define bms_is_empty(a)  ((a) == NULL)

/* these routines recycle (modify or free) their non-const inputs: */

Bitmapset *bms_add_member(Bitmapset *a, int x);
Bitmapset *bms_del_member(Bitmapset *a, int x);
Bitmapset *bms_add_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_replace_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_add_range(Bitmapset *a, int lower, int upper);
Bitmapset *bms_int_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_del_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_join(Bitmapset *a, Bitmapset *b);

/* support for iterating through the integer elements of a set: */
int bms_next_member(const Bitmapset *a, int prevbit);
int bms_prev_member(const Bitmapset *a, int prevbit);

/* support for hashtables using Bitmapsets as keys: */
uint32_t bms_hash_value(const Bitmapset *a);

#endif							/* BITMAPSET_STANDALONE_H */
