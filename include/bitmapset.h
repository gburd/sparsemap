/*-------------------------------------------------------------------------
 *
 * bitmapset.h
 *	  PostgreSQL generic bitmap set package
 *
 * A bitmap set can represent any set of non-negative integers in a
 * space-effient manner.  The range of the set is from 1 to UINT64_MAX.
 * By convention, we always represent the empty set by a NULL pointer.
 *
 * Copyright (c) 2003-2025, PostgreSQL Global Development Group
 *
 * src/include/nodes/bitmapset.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BITMAPSET_H
#define BITMAPSET_H

//include "nodes/nodes.h"
#include "../src/postgres.h"

/*
 * Forward decl to save including pg_list.h
 */
struct List;

/*
 * Data representation
 *
 * This is an implementation for a sparse, compressed bitmap. It is resizable
 * and mutable, with reasonable performance for random access modifications
 * and lookups.
 *
 * A Bitmapset consists of multiple chunks stored sequentially in memory.
 * Each chunk encodes a contiguous range of up to BMS_CHUNK_MAX_CAPACITY bits
 * (typically 2048 bits on 64-bit systems) using compression to minimize
 * storage for uniform bit patterns.
 *
 * Memory Layout:
 * - Header: 4-byte chunk count
 * - Chunk data: Variable-length compressed chunks stored sequentially
 *
 * Each chunk consists of:
 * - 4-byte starting bit offset (chunk_off_t)
 * - Flag bitvector (bms_bitvec_t): 2-bit flags describing each sub-range
 * - Payload vectors: Only stored for "mixed" sub-ranges
 *
 * Flag Encoding (2 bits per sub-range):
 * - 0b00 (BMS_PAYLOAD_ZEROS): All bits are 0 (no payload stored)
 * - 0b01 (BMS_PAYLOAD_NONE):  Sub-range unused (reduces chunk capacity)
 * - 0b10 (BMS_PAYLOAD_MIXED): Mixed 0s and 1s (payload vector stored)
 * - 0b11 (BMS_PAYLOAD_ONES):  All bits are 1 (no payload stored)
 *
 * Compression Efficiency:
 * - Minimum size: 12 bytes (4-byte offset + 8-byte flags) for uniform patterns
 * - Maximum size: 268 bytes when all sub-ranges are mixed
 * - Empty ranges: Represented as gaps between chunks (0 bytes)
 * - Sparse bitmaps achieve significant space savings
 *
 * Three-Tier Architecture:
 *
 * Tier 0 (Bit Level): Individual bits stored in bms_bitvec_t vectors
 *   - 64-bit vectors on 64-bit platforms, 32-bit on 32-bit platforms
 *   - Direct bit manipulation for mixed payload ranges
 *
 * Tier 1 (Chunk Level): Groups of vectors with compressed representation
 *   - Each chunk covers up to BMS_CHUNK_MAX_CAPACITY contiguous bits
 *   - Flag bitvector describes BMS_FLAGS_PER_INDEX sub-ranges
 *   - Only mixed sub-ranges require storage of actual bit vectors
 *   - Uniform sub-ranges (all 0s or all 1s) compressed into flags
 *
 * Tier 2 (Bitmap Level): Collection of chunks with gap handling
 *   - Chunks stored in ascending bit-offset order
 *   - Gaps between chunks implicitly represent unset bits
 *   - Dynamic memory management for chunk insertion/removal
 *   - Automatic chunk merging and splitting for optimal storage
 *
 * Performance Characteristics:
 * - Random access: O(log chunks + constant) for bit operations
 * - Sequential access: Optimized chunk-level iteration
 * - Memory usage: Proportional to set bit density and fragmentation
 * - Sparse patterns: Excellent compression (gaps cost nothing)
 * - Dense patterns: Competitive with traditional bitmap representations
 *
 * Below is a simplified representation.
 *
 *     00 11 22 33
 *     ^-- descriptor for bms_bitvec_t 1
 *        ^-- descriptor for bms_bitvec_t 2
 *           ^-- descriptor for bms_bitvec_t 3
 *              ^-- descriptor for bms_bitvec_t 4
 *
 *    The flags can have one of the following values:
 *
 *     00   The bms_bitvec_t is all zero -> no additional vectors required
 *     11   The bms_bitvec_t is all one -> no additional vectors required
 *     10   The bms_bitvec_t contains a bitmap -> no additional vectors required
 *     01   The bms_bitvec_t is not used, used to reduce capacity
 *
 *    The serialized size of a chunk in memory therefore is at least one
 *    bms_bitvec_t for the flags, and (optionally) additional bms_bitvec_t if
 *    they are required.
 */
typedef struct Bitmapset
{
  //	pg_node_attr(custom_copy_equal, special_read_write, no_query_jumble)

	NodeTag		type;

	size_t		size;			/* size of data in bytes */
	size_t		used;			/* amount of data used in bytes */
	uint8	   *data;			/* pointer to the chunks that describe the
								 * bitmap */
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

extern Bitmapset *bms(Bitmapset *map, uint8 *data, size_t bytes);
extern void bms_free(Bitmapset *a);

/* Returns the total number of set bits in the bitmap */
extern uint64 bms_num_members(const Bitmapset *a);

/* Returns the data size in bytes. */
extern size_t bms_size(const Bitmapset *map);

/* Returns the amount of data in use in bytes. */
extern size_t bms_used(const Bitmapset *map);

/*
 * Returns true if the Bitmapset is empty. */
extern bool bms_is_empty(const Bitmapset *a);

extern Bitmapset *bms_add_member(Bitmapset *a, uint64 x);
extern Bitmapset *bms_del_member(Bitmapset *a, uint64 x);

extern double bms_density(const Bitmapset *a);
extern double bms_get_utilization(const Bitmapset *map);

/* Advanced query functions */
extern int	bms_rank(const Bitmapset *map, bms_idx_t from, bms_idx_t to, bool value);
extern int	bms_select(const Bitmapset *map, bms_idx_t nth, bool value);
extern size_t bms_find_span(Bitmapset *map, size_t offset, size_t length, bool value);

/* Split operations */
extern int	bms_split(Bitmapset *source_map, bms_idx_t pivot, Bitmapset *dest_map);

extern Bitmapset *bms_copy(const Bitmapset *a);
extern bool bms_equal(const Bitmapset *a, const Bitmapset *b);
extern int	bms_compare(const Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_make_singleton(int x);

extern Bitmapset *bms_union(const Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_intersect(const Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_difference(const Bitmapset *a, const Bitmapset *b);
extern bool bms_is_subset(const Bitmapset *a, const Bitmapset *b);
extern BMS_Comparison bms_subset_compare(const Bitmapset *a, const Bitmapset *b);
extern bool bms_is_member(bms_idx_t x, const Bitmapset *a);
extern int	bms_member_index(Bitmapset *a, bms_idx_t x);
extern bool bms_overlap(const Bitmapset *a, const Bitmapset *b);
extern bool bms_overlap_list(const Bitmapset *a, const struct List *b);
extern bool bms_nonempty_difference(const Bitmapset *a, const Bitmapset *b);
extern int	bms_singleton_member(const Bitmapset *a);
extern bool bms_get_singleton_member(const Bitmapset *a, int *member);

/* optimized tests when we don't need to know the exact membership count: */
extern BMS_Membership bms_membership(const Bitmapset *a);

/* these routines recycle (modify or free) their non-const inputs: */
extern Bitmapset *bms_add_member(Bitmapset *a, bms_idx_t x);
extern Bitmapset *bms_del_member(Bitmapset *a, bms_idx_t x);
extern Bitmapset *bms_add_members(Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_replace_members(Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_add_range(Bitmapset *a, bms_idx_t lower, bms_idx_t upper);
extern Bitmapset *bms_int_members(Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_del_members(Bitmapset *a, const Bitmapset *b);
extern Bitmapset *bms_join(Bitmapset *a, Bitmapset *b);

/* support for iterating through the integer elements of a set: */
extern void bms_scan(const Bitmapset *map, void (*scanner) (bms_idx_t bit_indices[], size_t count, void *aux_data), size_t skip_count, void *aux_data);
extern int	bms_first_member(const Bitmapset *a);
extern int	bms_last_member(const Bitmapset *a);
extern int	bms_next_member(const Bitmapset *a, int prevbit);
extern int	bms_prev_member(const Bitmapset *a, int prevbit);

/* support for hashtables using Bitmapsets as keys: */
extern uint32 bms_hash_value(const Bitmapset *a);
extern uint32 bitmap_hash(const void *key, Size keysize);
extern int	bitmap_match(const void *key1, const void *key2, Size keysize);

#endif							/* BITMAPSET_H */
