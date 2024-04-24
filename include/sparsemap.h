/*
 * Copyright (c) 2024 Gregory Burd <greg@burd.me>.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

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

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * The public interface for a sparse bit-mapped index, a "sparse map".
 *
 * |sm_idx_t| is the user's numerical data type which is mapped to a single bit
 * in the bitmap. Usually this is uint32_t or uint64_t.  |sm_bitvec_t| is the
 * storage type for a bit vector used by the __sm_chunk_t internal maps.
 * Usually this is an uint64_t.
 */

typedef struct sparsemap sparsemap_t;
typedef long int sparsemap_idx_t;
#define SPARSEMAP_IDX_MAX ((1UL << (sizeof(long) * CHAR_BIT - 1)) - 1)
#define SPARSEMAP_IDX_MIN (-(SPARSEMAP_IDX_MAX)-1)
#define SPARSEMAP_NOT_FOUND(_x) ((_x) == SPARSEMAP_IDX_MAX || (_x) == SPARSEMAP_IDX_MIN)
typedef uint32_t sm_idx_t;
typedef uint64_t sm_bitvec_t;

/**
 * Create a new, empty sparsemap_t with a buffer of |size|.
 * Default when set to 0 is 1024.
 */
sparsemap_t *sparsemap(size_t size);

/**
 * Allocate on a sparsemap_t on the heap to wrap the provided fixed-size
 * buffer (heap or stack allocated).
 */
sparsemap_t *sparsemap_wrap(uint8_t *data, size_t size);

/**
 * Initialize a (possibly stack allocated) sparsemap_t with data (potentially
 * also on the stack).
 */
void sparsemap_init(sparsemap_t *map, uint8_t *data, size_t size);

/**
 * Opens an existing sparsemap contained within the specified buffer.
 */
void sparsemap_open(sparsemap_t *, uint8_t *data, size_t data_size);

/**
 * Resets values and empties the buffer making it ready to accept new data.
 */
void sparsemap_clear(sparsemap_t *map);

/**
 * Resizes the data range within the limits of the provided buffer, the map may
 * move to a new address returned iff the map was created with the sparsemap() API.
 * Take care to use the new reference (think: realloc()).  NOTE: If the returned
 * value equals NULL then the map was not resized.
 */
sparsemap_t *sparsemap_set_data_size(sparsemap_t *map, size_t data_size);

/**
 * Calculate remaining capacity, approaches 0 when full.
 */
double sparsemap_capacity_remaining(sparsemap_t *map);

/**
 * Returns the capacity of the underlying byte array.
 */
size_t sparsemap_get_capacity(sparsemap_t *map);

/**
 * Returns the value of a bit at index |idx|, either on/true/1 or off/false/0.
 * When |idx| is negative it is an error.
 */
bool sparsemap_is_set(sparsemap_t *map, sparsemap_idx_t idx);

/**
 * Sets the bit at index |idx| to true or false, depending on |value|.
 * When |idx| is negative is it an error.  Returns the |idx| supplied or
 * SPARSEMAP_IDX_MAX on error with |errno| set to ENOSP when the map is full.
 */
sparsemap_idx_t sparsemap_set(sparsemap_t *map, sparsemap_idx_t idx, bool value);

/**
 * Returns the offset of the very first/last bit in the map.
 */
sm_idx_t sparsemap_get_starting_offset(sparsemap_t *map);

/**
 * Returns the used size in the data buffer in bytes.
 */
size_t sparsemap_get_size(sparsemap_t *map);

/**
 * Decompresses the whole bitmap; calls scanner for all bits with a set of
 * |n| vectors |vec| each a sm_bitmap_t which can be masked and read using
 * bit operators to read the values for each position in the bitmap index.
 * Setting |skip| will start the scan after "skip" bits.
 */
void sparsemap_scan(sparsemap_t *map, void (*scanner)(sm_idx_t vec[], size_t n), size_t skip);

/**
 * Appends all chunk maps from |map| starting at |offset| to |other|, then
 * reduces the chunk map-count appropriately.
 */
void sparsemap_split(sparsemap_t *map, sparsemap_idx_t offset, sparsemap_t *other);

/**
 * Finds the offset of the n'th bit either set (|value| is true) or unset
 * (|value| is false) from the start (positive |n|), or end (negative |n|),
 * of the bitmap and returns that (uses a 0-based index).  Returns -inf or +inf
 * if not found (where "inf" is SPARSEMAP_IDX_MAX and "-inf" is SPARSEMAP_IDX_MIN).
 */
sparsemap_idx_t sparsemap_select(sparsemap_t *map, sparsemap_idx_t n, bool value);

/**
 * Counts the set (|value| is true) or unset (|value| is false) bits starting
 * at |x| bits (0-based) in the range [x, y] (inclusive on either end).
 */
size_t sparsemap_rank(sparsemap_t *map, size_t x, size_t y, bool value);

/**
 * Finds the first span (i.e. a contiguous set of bits), in the bitmap that
 * are set (|value| is true) or unset (|value| is false) and returns the
 * starting offset for the span (0-based).
 */
size_t sparsemap_span(sparsemap_t *map, sparsemap_idx_t idx, size_t len, bool value);

#if defined(__cplusplus)
}
#endif

#endif /* !defined(SPARSEMAP_H) */
