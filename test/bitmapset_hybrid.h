/*-------------------------------------------------------------------------
 *
 * bitmapset_hybrid.h
 *	  Hybrid Bitmapset: PostgreSQL Bitmapset API + Sparsemap storage
 *
 * This implements the PostgreSQL Bitmapset API with a dual-mode storage
 * backend:
 *
 *   Dense mode:  Traditional word-array (byte-identical to PostgreSQL)
 *   Chunked mode: Sparsemap encoding (RLE + sparse chunks) for large/sparse sets
 *
 * The mode is encoded in nwords:
 *   Dense:    nwords = word count (1..65535), always < 0x10000
 *   Chunked:  nwords = (alloc_chunks << 16) | used_chunks, >= 0x10000
 *
 * Detection: (nwords >> 16 != 0) -> chunked mode
 *
 * Public API uses int for member indices (matching PostgreSQL convention).
 * NULL represents the empty set.
 *
 *-------------------------------------------------------------------------
 */
#ifndef BITMAPSET_HYBRID_H
#define BITMAPSET_HYBRID_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

/*
 * Data representation
 */
#define BITS_PER_BITMAPWORD 64
typedef uint64_t bitmapword;		/* must be an unsigned type */
typedef int64_t signedbitmapword;	/* must be the matching signed type */

typedef struct Bitmapset
{
	uint32_t	nwords;			/* mode + size field */
	uint32_t	_padding;		/* align data[] to 8 bytes for bitmapword access */
	uint8_t		data[];			/* flexible array: words[] or sparsemap buffer */
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

/*
 * Mode detection and access macros
 */
#define BMS_IS_CHUNKED(a)       ((a)->nwords >> 16 != 0)
#define BMS_NWORDS(a)           ((int)(a)->nwords)       /* dense only */
#define BMS_ALLOC_CHUNKS(a)     ((a)->nwords >> 16)
#define BMS_USED_CHUNKS(a)      ((a)->nwords & 0xFFFFu)
#define BMS_WORDS(a)            ((bitmapword *)((a)->data))
#define BMS_BUF(a)              ((a)->data)

/* NULL is the only representation of an empty bitmapset */
#define bms_is_empty(a)         ((a) == NULL)

/* Bit-twiddling functions using GCC builtins for 64-bit words */
#define bmw_leftmost_one_pos(w)		(63 - __builtin_clzll(w))
#define bmw_rightmost_one_pos(w)	__builtin_ctzll(w)
#define bmw_popcount(w)				__builtin_popcountll(w)

/* Size calculation for dense mode */
#define BITMAPSET_SIZE(nwords)	\
	(offsetof(Bitmapset, data) + (nwords) * sizeof(bitmapword))

#ifndef WORDNUM
#define WORDNUM(x)	((x) / BITS_PER_BITMAPWORD)
#endif

/*
 * Function prototypes
 *
 * Public API uses int for member indices (matching PostgreSQL convention).
 * Internally, chunked-mode helpers widen to int64_t for chunk-start
 * arithmetic where needed.
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
int bms_member_index(const Bitmapset *a, int x);
bool bms_overlap(const Bitmapset *a, const Bitmapset *b);
bool bms_nonempty_difference(const Bitmapset *a, const Bitmapset *b);
int bms_singleton_member(const Bitmapset *a);
bool bms_get_singleton_member(const Bitmapset *a, int *member);
int bms_num_members(const Bitmapset *a);

BMS_Membership bms_membership(const Bitmapset *a);

/* These routines recycle (modify or free) their non-const inputs: */
Bitmapset *bms_add_member(Bitmapset *a, int x);
Bitmapset *bms_del_member(Bitmapset *a, int x);
Bitmapset *bms_add_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_replace_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_add_range(Bitmapset *a, int lower, int upper);
Bitmapset *bms_int_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_del_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_join(Bitmapset *a, Bitmapset *b);

/* Iteration */
int bms_next_member(const Bitmapset *a, int prevbit);
int bms_prev_member(const Bitmapset *a, int prevbit);

/* Hashing */
uint32_t bms_hash_value(const Bitmapset *a);

#endif							/* BITMAPSET_HYBRID_H */
