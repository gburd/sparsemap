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
 * The data[] flexible array member is cast to bitmapword* in dense mode
 * (safe under -fno-strict-aliasing, which PG requires) and accessed as
 * raw bytes in chunked mode.  NULL represents the empty set.
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
	uint32_t	_padding;		/* align data to 8 bytes for bitmapword access */
	uint8_t		data[];			/* dense: cast to bitmapword*; chunked: byte buffer */
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
static inline int
BMS_NWORDS_FN(const Bitmapset *a)
{
	Assert(!BMS_IS_CHUNKED(a));
	return (int)a->nwords;
}
#define BMS_NWORDS(a)			BMS_NWORDS_FN(a)
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

#ifndef WORDNUM
#define WORDNUM(x)	((x) / BITS_PER_BITMAPWORD)
#endif

/*
 * Alignment attribute for stack-allocated Bitmapset buffers.
 */
#ifdef BUILDING_OUTSIDE_POSTGRES
#define BMS_ALIGN_ATTR __attribute__((aligned(8)))
#else
#define BMS_ALIGN_ATTR pg_attribute_aligned(8)
#endif

/*
 * BMS_LOCAL -- declare a stack-allocated dense Bitmapset.
 *
 * Usage:
 *     BMS_LOCAL(tmp, 2048);   // tmp covers bits 0..2048
 *     bms_add_member(tmp, 42);
 *     // ... use tmp as a read-only operand or in non-reallocating ops ...
 *     // Do NOT pfree(tmp) or pass to functions that may repalloc.
 *
 * The variable `name` is a Bitmapset* pointing to an aligned stack buffer.
 * Dense mode only. maxbit must be a compile-time constant for VLAs to be
 * avoided. The buffer is zeroed.
 *
 * WARNING: Functions that grow the set (bms_add_member beyond maxbit,
 * bms_union, etc.) may repalloc, which will crash on a stack pointer.
 * Only use for bounded, known-range operations.
 */
#define BMS_LOCAL(name, maxbit)                                              \
    uint8_t name##__buf[BITMAPSET_SIZE(WORDNUM(maxbit) + 1)]                \
        BMS_ALIGN_ATTR;                                                      \
    memset(name##__buf, 0, sizeof(name##__buf));                             \
    Bitmapset *name = (Bitmapset *)name##__buf;                              \
    name->nwords = (uint32_t)(WORDNUM(maxbit) + 1)

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

/* optimized tests when we don't need to know exact membership count */
BMS_Membership bms_membership(const Bitmapset *a);

/* these routines recycle (modify or free) their non-const inputs */
Bitmapset *bms_add_member(Bitmapset *a, int x);
Bitmapset *bms_del_member(Bitmapset *a, int x);
Bitmapset *bms_add_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_replace_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_add_range(Bitmapset *a, int lower, int upper);
Bitmapset *bms_int_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_del_members(Bitmapset *a, const Bitmapset *b);
Bitmapset *bms_join(Bitmapset *a, Bitmapset *b);

/* support for iterating through the integer elements of a set */
int bms_next_member(const Bitmapset *a, int prevbit);
int bms_prev_member(const Bitmapset *a, int prevbit);

/* support for hashtables using Bitmapsets as keys */
uint32_t bms_hash_value(const Bitmapset *a);

#endif							/* BITMAPSET_H */
