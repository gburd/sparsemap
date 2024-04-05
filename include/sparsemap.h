/*
 * Sparsemap
 *
 * This is an implementation for a sparse, compressed bitmap. It is resizable
 * and mutable, with reasonable performance for random access modifications
 * and lookups.
 *
 * The implementation is separated into tiers.
 *
 * Tier 0 (lowest): bits are stored in a sm_bitvec_t (uint64_t).
 *
 * Tier 1 (middle): multiple sm_bitvec_t are managed in a chunk map. The chunk
 *    map only stores those sm_bitvec_t that have a mixed payload of bits (i.e.
 *    some bits are 1, some are 0). As soon as ALL bits in a sm_bitvec_t are
 *    identical, this sm_bitvec_t is no longer stored, it is compressed.
 *
 *    The chunk maps store additional flags (2 bit) for each sm_bitvec_t in an
 *    additional word (same size as the sm_bitvec_t itself).
 *
 *     00 11 22 33
 *     ^-- descriptor for sm_bitvec_t 1
 *        ^-- descriptor for sm_bitvec_t 2
 *           ^-- descriptor for sm_bitvec_t 3
 *              ^-- descriptor for sm_bitvec_t 4
 *
 *    Those flags (*) can have one of the following values:
 *
 *     00   The sm_bitvec_t is all zero -> sm_bitvec_t is not stored
 *     11   The sm_bitvec_t is all one -> sm_bitvec_t is not stored
 *     10   The sm_bitvec_t contains a bitmap -> sm_bitvec_t is stored
 *     01   The sm_bitvec_t is not used (**)
 *
 *    The serialized size of a chunk map in memory therefore is at least
 *    one sm_bitvec_t for the flags, and (optionally) additional sm_bitvec_ts
 *    if they are required.
 *
 *    (*) The code comments often use the Erlang format for binary
 *    representation, i.e. 2#10 for (binary) 01.
 *
 *    (**) This flag is set to reduce the capacity of a chunk map.
 *
 * Tier 2 (highest): the Sparsemap manages multiple chunk maps. Each chunk
 *    has its own offset (relative to the offset of the Sparsemap). In
 *    addition, the Sparsemap manages the memory of the chunk maps, and
 *    is able to grow or shrink that memory as required.
 */
#ifndef SPARSEMAP_H
#define SPARSEMAP_H

#include <sys/types.h>

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * The public interface for a sparse bit-mapped index, a "sparse map".
 *
 * |sm_idx_t| is the user's numerical data type which is mapped to a single bit
 * in the bitmap. Usually this is uint32_t or uint64_t.  |sm_bitvec_t| is the
 * storage type for a bit vector used by the __sm_chunk_t internal maps.
 * Usually this is an uint64_t.
 */

typedef uint32_t sm_idx_t;
typedef uint64_t sm_bitvec_t;

typedef struct sparsemap {
  uint8_t *m_data;    /* The serialized bitmap data */
  size_t m_data_size; /* The total size of m_data */
  size_t m_data_used; /* The used size of m_data */
} sparsemap_t;

/* Allocate on a sparsemap_t on the heap and initialize it. */
sparsemap_t *sparsemap(uint8_t *data, size_t size, size_t used);

/* Initialize sparsemap_t with data. */
void sparsemap_init(sparsemap_t *map, uint8_t *data, size_t size, size_t used);

/* Clears the whole buffer. */
void sparsemap_clear(sparsemap_t *map);

/* Opens an existing sparsemap at the specified buffer. */
void sparsemap_open(sparsemap_t *, uint8_t *data, size_t data_size);

/* Resizes the data range. */
void sparsemap_set_data_size(sparsemap_t *map, size_t data_size);

/* Returns the size of the underlying byte array. */
size_t sparsemap_get_range_size(sparsemap_t *map);

/* Returns the value of a bit at index |idx|. */
bool sparsemap_is_set(sparsemap_t *map, size_t idx);

/* Sets the bit at index |idx| to true or false, depending on |value|. */
void sparsemap_set(sparsemap_t *map, size_t idx, bool value);

/* Returns the offset of the very first bit. */
sm_idx_t sparsemap_get_start_offset(sparsemap_t *map);

/* Returns the used size in the data buffer. */
size_t sparsemap_get_size(sparsemap_t *map);

/* Decompresses the whole bitmap; calls scanner for all bits. */
void sparsemap_scan(sparsemap_t *map, void (*scanner)(sm_idx_t[], size_t),
  size_t skip);

/* Appends all chunk maps from |map| starting at |sstart| to |other|, then
   reduces the chunk map-count appropriately. */
void sparsemap_split(sparsemap_t *map, size_t sstart, sparsemap_t *other);

#if 0 // TODO
/* Sets/clears bits starting at |ssize| in other in |map| possibly invoking the resize function. */
void sparsemap_combine(sparsemap_t *map, size_t sstart, sparsemap_t *other);
#endif

/* Returns the index of the n'th set bit; uses a 0-based index. */
size_t sparsemap_select(sparsemap_t *map, size_t offset, size_t n);

/* Counts the set bits in the range [offset, idx]. */
size_t sparsemap_rank(sparsemap_t *map, size_t offset, size_t idx);

/* Returns the 0-based index of a span of the first set bits of at least |len|
 * starting after |offset|. */
size_t sparsemap_span(sparsemap_t *map, size_t offset, size_t len);

#endif
