/*-------------------------------------------------------------------------
 *
 * bitmapset.h
 *	  PostgreSQL generic bitmap set package -- hybrid dense/chunked storage
 *
 * This implements the PostgreSQL Bitmapset API with dual-mode storage:
 *
 *   Dense mode:   Traditional word-array (compatible with original Bitmapset)
 *   Chunked mode: RLE + sparse chunk encoding for large/sparse sets
 *
 * The storage mode is encoded in the nwords field:
 *   Dense:   nwords = word count (1..BMS_DENSE_MAX_NWORDS)
 *   Chunked: nwords = (alloc_chunks << 16) | used_chunks
 *
 * Detection: BMS_IS_CHUNKED(a) tests (nwords >> 16 != 0)
 *
 * All member-index parameters use int64_t (widened from int) to support
 * chunked mode addressing.  NULL represents the empty set.
 *
 * Copyright (c) 2003-2026, PostgreSQL Global Development Group
 * Chunked storage: Copyright (c) 2024-2026, Gregory Burd <greg@burd.me>
 *
 *-------------------------------------------------------------------------
 */
#ifndef BITMAPSET_H
#define BITMAPSET_H

#ifdef BUILDING_OUTSIDE_POSTGRES
#include "postgres_compat.h"
#else
#include "postgres.h"
#endif

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
	uint8_t		data[];			/* flexible array: words[] or chunk buffer */
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
#define BMS_IS_CHUNKED(a)		((a)->nwords >> 16 != 0)
#define BMS_NWORDS(a)			((int)(a)->nwords)		/* dense only */
#define BMS_ALLOC_CHUNKS(a)		((a)->nwords >> 16)
#define BMS_USED_CHUNKS(a)		((a)->nwords & 0xFFFFu)
#define BMS_WORDS(a)			((bitmapword *)((a)->data))
#define BMS_BUF(a)				((a)->data)

/* NULL is the only representation of an empty bitmapset */
#define bms_is_empty(a)			((a) == NULL)

/*
 * Bit-twiddling functions
 *
 * When building outside PostgreSQL, use GCC builtins directly.
 * Inside PostgreSQL, use the portable pg_bitutils.h wrappers.
 */
#ifdef BUILDING_OUTSIDE_POSTGRES
#define bmw_leftmost_one_pos(w)		(63 - __builtin_clzll(w))
#define bmw_rightmost_one_pos(w)	__builtin_ctzll(w)
#define bmw_popcount(w)				__builtin_popcountll(w)
#else
#include "port/pg_bitutils.h"
#define bmw_leftmost_one_pos(w)		pg_leftmost_one_pos64(w)
#define bmw_rightmost_one_pos(w)	pg_rightmost_one_pos64(w)
#define bmw_popcount(w)				pg_popcount64(w)
#endif

/* Size calculation for dense mode */
#define BITMAPSET_SIZE(nwords)	\
	(offsetof(Bitmapset, data) + (nwords) * sizeof(bitmapword))


/*
 * Function prototypes -- all use int64_t for member indices
 */

Bitmapset *bms_copy(const Bitmapset *a);
bool bms_equal(const Bitmapset *a, const Bitmapset *b);
int bms_compare(const Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_make_singleton(int64_t x);
void bms_free(Bitmapset *a);

Bitmapset *bms_union(const Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_intersect(const Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_difference(const Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_offset_members(const Bitmapset *a, int64_t offset);
bool bms_is_subset(const Bitmapset *a, const Bitmapset *b);
BMS_Comparison bms_subset_compare(const Bitmapset *a, const Bitmapset *b);
bool bms_is_member(int64_t x, const Bitmapset *a);
int64_t bms_member_index(const Bitmapset *a, int64_t x);
bool bms_overlap(const Bitmapset *a, const Bitmapset *b);
bool bms_nonempty_difference(const Bitmapset *a, const Bitmapset *b);
int64_t bms_singleton_member(const Bitmapset *a);
bool bms_get_singleton_member(const Bitmapset *a, int64_t *member);
int64_t bms_num_members(const Bitmapset *a);

/* optimized tests when we don't need to know exact membership count */
BMS_Membership bms_membership(const Bitmapset *a);

/* these routines recycle (modify or free) their non-const inputs */
Bitmapset *bms_add_member(Bitmapset *a, int64_t x);
Bitmapset *bms_del_member(Bitmapset *a, int64_t x);
Bitmapset *bms_add_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_replace_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_add_range(Bitmapset *a, int64_t lower, int64_t upper);
Bitmapset *bms_int_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_del_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_join(Bitmapset *a, Bitmapset *b);

/* support for iterating through the integer elements of a set */
int64_t bms_next_member(const Bitmapset *a, int64_t prevbit);
int64_t bms_prev_member(const Bitmapset *a, int64_t prevbit);

/* support for hashtables using Bitmapsets as keys */
uint32_t bms_hash_value(const Bitmapset *a);

#endif							/* BITMAPSET_H */
