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

#include <sys/types.h>

#include <errno.h>
#include <popcount.h>
#include <sparsemap.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SPARSEMAP_DIAGNOSTIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#define __sm_diag(format, ...) __sm_diag_(__FILE__, __LINE__, __func__, format, ##__VA_ARGS__)
#pragma GCC diagnostic pop
void __attribute__((format(printf, 4, 5))) __sm_diag_(const char *file, int line, const char *func, const char *format, ...)
{
  va_list args = { 0 };
  fprintf(stderr, "%s:%d:%s(): ", file, line, func);
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

#define __sm_assert(expr) \
  if (!(expr))            \
  fprintf(stderr, "%s:%d:%s(): assertion failed! %s\n", __FILE__, __LINE__, __func__, #expr)

#define __sm_when_diag(expr) \
  if (1)                     \
  expr
#else
#define __sm_diag(file, line, func, format, ...) ((void)0)
#define __sm_assert(expr) ((void)0)
#define __sm_when_diag(expr) \
  if (0)                     \
  expr
#endif

#define IS_8_BYTE_ALIGNED(addr) (((uintptr_t)(addr)&0x7) == 0)

typedef uint64_t __sm_bitvec_t;
typedef uint32_t __sm_idx_t;

typedef struct {
  __sm_bitvec_t *m_data;
} __sm_chunk_t;

typedef struct {
  size_t rem;
  size_t pos;
} __sm_chunk_rank_t;

// NOTE: When using in production feel free to remove this section of test code.
#ifdef SPARSEMAP_TESTING
#include <inttypes.h>
char *QCC_showSparsemap(void *value, int len);
char *QCC_showChunk(void *value, int len);
static char *_qcc_format_chunk(__sm_idx_t start, __sm_chunk_t *chunk, bool none);

static void __attribute__((format(printf, 2, 3)))
__sm_diag_map(sparsemap_t *map, const char *fmt, ...)
{
  va_list args = { 0 };
  va_start(args, fmt);
  vfprintf(stdout, fmt, args);
  va_end(args);
  char *s = QCC_showSparsemap(map, 0);
  fprintf(stdout, "\n%s\n", s);
  free(s);
}

static void
__sm_diag_chunk(const char *msg, __sm_chunk_t *chunk)
{
  char *s = QCC_showChunk(chunk, 0);
  fprintf(stdout, "%s\n%s\n", msg, s);
  free(s);
}
#endif

enum __SM_CHUNK_INFO {
  /* metadata overhead: 4 bytes for __sm_chunk_t count */
  SM_SIZEOF_OVERHEAD = sizeof(__sm_idx_t),

  /* number of bits that can be stored in a __sm_bitvec_t */
  SM_BITS_PER_VECTOR = (sizeof(__sm_bitvec_t) * 8),

  /* number of flags that can be stored in a single index byte */
  SM_FLAGS_PER_INDEX_BYTE = 4,

  /* number of flags that can be stored in the index */
  SM_FLAGS_PER_INDEX = (sizeof(__sm_bitvec_t) * SM_FLAGS_PER_INDEX_BYTE),

  /* maximum capacity of a __sm_chunk_t (in bits) */
  SM_CHUNK_MAX_CAPACITY = (SM_BITS_PER_VECTOR * SM_FLAGS_PER_INDEX),

  /* maximum capacity of a __sm_chunk_t (31 bits of the RLE) */
  SM_CHUNK_RLE_MAX_CAPACITY = 0x7FFFFFFF,

  /* minimum capacity of a __sm_chunk_t (in bits) */
  SM_CHUNK_MIN_CAPACITY = (SM_BITS_PER_VECTOR - 2),

  /* maximum length of a __sm_chunk_t (31 bits of the RLE) */
  SM_CHUNK_RLE_MAX_LENGTH = 0x7FFFFFFF,

  /* __sm_bitvec_t payload is all zeros (2#00) */
  SM_PAYLOAD_ZEROS = 0,

  /* __sm_bitvec_t payload is all ones (2#11) */
  SM_PAYLOAD_ONES = 3,

  /* __sm_bitvec_t payload is mixed (2#10) */
  SM_PAYLOAD_MIXED = 2,

  /* __sm_bitvec_t is not used (2#01) */
  SM_PAYLOAD_NONE = 1,

  /* a mask for checking flags (2 bits, 2#11) */
  SM_FLAG_MASK = 3,

  /* return code for set(): ok, no further action required */
  SM_OK = 0,

  /* return code for set(): needs to grow this __sm_chunk_t */
  SM_NEEDS_TO_GROW = 1,

  /* return code for set(): needs to shrink this __sm_chunk_t */
  SM_NEEDS_TO_SHRINK = 2
};

/* Used when separating a RLE chunk into 2-3 chunks */
typedef struct {
  struct {
    uint8_t *p;          // pointer into m_data
    size_t offset;       // offset in m_data
    __sm_chunk_t *chunk; // chunk to be split
    __sm_idx_t start;    // start of chunk
    size_t length;       // initial length of chunk
    size_t capacity;     // the capacity of this RLE chunk
  } target;

  struct {
    uint8_t *p;          // location in buf
    sparsemap_idx_t idx; // chunk-aligned to idx
    size_t size;         // byte size of this chunk
  } pivot;

  struct {
    sparsemap_idx_t start;
    sparsemap_idx_t end;
    uint8_t *p;
    size_t size;
    __sm_chunk_t c;
  } ex[2]; // 0 is "on the left", 1 is "on the right"

  uint8_t buf[(SM_SIZEOF_OVERHEAD * 3) + (sizeof(__sm_bitvec_t) * 6)];
  size_t expand_by;
  size_t count;
} __sm_chunk_sep_t;

#define SM_ENOUGH_SPACE(need)                          \
  do {                                                 \
    if (map->m_data_used + (need) > map->m_capacity) { \
      errno = ENOSPC;                                  \
      return SPARSEMAP_IDX_MAX;                        \
    }                                                  \
  } while (0)

#define SM_CHUNK_GET_FLAGS(data, at) ((((data)) & ((__sm_bitvec_t)SM_FLAG_MASK << ((at)*2))) >> ((at)*2))
#define SM_CHUNK_SET_FLAGS(data, at, to) (data) = ((data) & ~((__sm_bitvec_t)SM_FLAG_MASK << ((at)*2))) | ((__sm_bitvec_t)(to) << ((at)*2))
#define SM_IS_CHUNK_RLE(chunk) \
  (((*((__sm_bitvec_t *)(chunk)->m_data) & (((__sm_bitvec_t)0x3) << (SM_BITS_PER_VECTOR - 2))) >> (SM_BITS_PER_VECTOR - 2)) == SM_PAYLOAD_NONE)

#define SM_RLE_FLAGS 0x4000000000000000
#define SM_RLE_FLAGS_MASK 0xC000000000000000
#define SM_RLE_CAPACITY_MASK 0x3FFFFFFF80000000
#define SM_RLE_LENGTH_MASK 0x7FFFFFFF

/** @brief Determines if the chunk is RLE encoded.
 *
 * @param[in] chunk The chunk to test.
 * @return true when the chunk is run-length encoded (RLE), otherwise false.
 */
static inline bool
__sm_chunk_is_rle(__sm_chunk_t *chunk)
{
  __sm_bitvec_t w = chunk->m_data[0];
  return (w & SM_RLE_FLAGS_MASK) == SM_RLE_FLAGS;
}

/** @brief Changes the chunk to be flagged as RLE encoded.
 *
 * This doesn't change any other bits in the chunk's descriptor.
 *
 * @param[in] chunk The chunk to modify.
 */
static inline void
__sm_chunk_set_rle(__sm_chunk_t *chunk)
{
  __sm_bitvec_t w = chunk->m_data[0];
  w &= ~SM_RLE_FLAGS_MASK;
  w |= ((((__sm_bitvec_t)1) << (SM_BITS_PER_VECTOR - 2)) & SM_RLE_FLAGS_MASK);
  chunk->m_data[0] = w;
}

/** @brief Get the capacity of an RLE chunk.
 *
 * This only returns a meaningful value when chunk is RLE encoded.  This will
 * return garbage when called referencing a sparse chunk.
 *
 * @param[in] chunk The chunk in question.
 * @return The capacity of the RLE chunk, at most SM_CHUNK_RLE_MAX_CAPACITY.
 */
static inline size_t
__sm_chunk_rle_get_capacity(__sm_chunk_t *chunk)
{
  __sm_bitvec_t w = chunk->m_data[0] & (__sm_bitvec_t)SM_RLE_CAPACITY_MASK;
  w >>= 31;
  return w;
}

/** @brief Sets the capacity of the RLE chunk.
 *
 * This does not check the chunk type, if the chunk isn't RLE then this function
 * will overwrite flags data in a sparse chunk.
 *
 * @param[in] chunk The chunk in question.
 * @param[in] capacity The new capacity, at most SM_CHUNK_RLE_MAX_CAPACITY.
 */
static inline void
__sm_chunk_rle_set_capacity(__sm_chunk_t *chunk, size_t capacity)
{
  __sm_assert(capacity <= SM_CHUNK_RLE_MAX_CAPACITY);
  __sm_bitvec_t w = chunk->m_data[0];
  w &= ~SM_RLE_CAPACITY_MASK;
  w |= (capacity << 31) & SM_RLE_CAPACITY_MASK;
  chunk->m_data[0] = w;
}

/** @brief Get the current length of an RLE chunk.
 *
 * This only returns a meaningful value when chunk is RLE encoded.  This will
 * return garbage when called referencing a sparse chunk.
 *
 * @param[in] chunk The chunk in question.
 * @reutrn The current length of the run of adjacent ones encoded in this RLE
 * chunk, at most SM_CHUNK_RLE_MAX_CAPACITY.
 */
static inline size_t
__sm_chunk_rle_get_length(__sm_chunk_t *chunk)
{
  __sm_bitvec_t w = chunk->m_data[0] & (__sm_bitvec_t)SM_RLE_LENGTH_MASK;
  return w;
}

/** @brief Sets the length of the RLE chunk.
 *
 * This does not check the chunk type, if the chunk isn't RLE then this function
 * will overwrite flags data in a sparse chunk.
 *
 * @param[in] chunk The chunk in question.
 * @param[in] length The new capacity, at most SM_CHUNK_RLE_MAX_LENGTH.
 */
static inline void
__sm_chunk_rle_set_length(__sm_chunk_t *chunk, size_t length)
{
  __sm_assert(length <= SM_CHUNK_RLE_MAX_LENGTH);
  __sm_bitvec_t w = chunk->m_data[0];
  w &= ~SM_RLE_LENGTH_MASK;
  w |= length & SM_RLE_LENGTH_MASK;
  chunk->m_data[0] = w;
}

/** @brief Returns the length of a run of adjacent ones in this chunk.
 *
 * A "run" is a set of adjacent ones that starts at the 0th bit of this
 * chunk. For an RLE chunk that's encoded in the descriptor.  For a sparse chunk
 * we must see how many flags are SM_PAYLOAD_ONES and then if we find a
 * SM_PAYLOAD_MIXED count the additional adjacent ones if they exist.
 */
static size_t
__sm_chunk_get_run_length(__sm_chunk_t *chunk)
{
  size_t length = 0;

  if (__sm_chunk_is_rle(chunk)) {
    length = __sm_chunk_rle_get_length(chunk);
  } else {
    size_t count = 0;
    int j = SM_FLAGS_PER_INDEX, k = SM_BITS_PER_VECTOR;
    __sm_bitvec_t w = chunk->m_data[0], v = chunk->m_data[1];

    switch (w) {
    case 0:
      return 0;
    case ~(__sm_bitvec_t)0:
      return SM_CHUNK_MAX_CAPACITY;
    default:
      while (j && (w & SM_PAYLOAD_ONES) == SM_PAYLOAD_ONES) {
        count++;
        w >>= 2;
        j--;
      }
      if (count) {
        count *= SM_BITS_PER_VECTOR;
        if ((w & SM_PAYLOAD_MIXED) == SM_PAYLOAD_MIXED) {
          w >>= 2;
          j--;
          while (k && ((v & 1) == 1)) {
            count++;
            v >>= 1;
            k--;
          }
          while (k && ((v & 1) == 0)) {
            v >>= 1;
            k--;
          }
          if (k) {
            return 0;
          }
        }
        while (j--) {
          switch (w & 0x3) {
          case SM_PAYLOAD_NONE:
          case SM_PAYLOAD_ZEROS:
            w >>= 2;
            break;
          default:
            return 0;
          }
        }
        __sm_assert(count < SM_CHUNK_MAX_CAPACITY);
        length = count;
      }
    }
  }
  return length;
}

struct __attribute__((aligned(8))) sparsemap {
  size_t m_capacity;  /* The total size of m_data */
  size_t m_data_used; /* The used size of m_data */
  uint8_t *m_data;    /* The serialized bitmap data */
};

/** @brief Calculates the additional vectors required based on \b b.
 *
 * This function uses a precomputed lookup table to efficiently determine the
 * number of vectors required based on the value of the input byte \b b.
 *
 * Each entry in the lookup table represents a possible combination of 4 2-bit
 * values (00, 01, 10, 11).  The value at each index corresponds to the count of
 * "10" patterns in that 4-bit combination.  For example, lookup[10] is 2
 * because the binary representation of 10 (0000 1010) contains the "01" pattern
 * twice.
 *
 * @param[in] b The input byte used for the calculation.
 * @return The calculated number of vectors.
 * @see bin/gen_chunk_vector_size_table.py
 */
static size_t
__sm_chunk_calc_vector_size(uint8_t b)
{
  // clang-format off
  static int lookup[] = {
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    2,  2,  3,  2,  2,  2,  3,  2,  3,  3,  4,  3,  2,  2,  3,  2,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
    1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
    0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0
  };
  // clang-format on
  return (size_t)lookup[b];
}

/** @brief Calculates the offset into set set of vectors within a chunk for a
 * given index.
 *
 * This function determines the array index into m_data of the requested
 * position.  The code examines the flags within the descriptor and calculates
 * the index within the chunk's data.
 *
 * @param[in] chunk Pointer to the chunk.
 * @param[in] bv Index within the descriptor (0-based).
 * @return Array index of the vector within the chunk's data iff the descriptor
 * was SM_PAYLOAD_MIXED, otherwise meaningless.
 */
static size_t
__sm_chunk_get_position(__sm_chunk_t *chunk, size_t bv)
{
  /* Handle 4 indices (1 byte) at a time. */
  size_t num_bytes;
  size_t position = 0;
  register uint8_t *p = (uint8_t *)chunk->m_data;

  /* Handle RLE by examining the first byte. */
  if (!__sm_chunk_is_rle(chunk)) {
    num_bytes = bv / ((size_t)SM_FLAGS_PER_INDEX_BYTE * SM_BITS_PER_VECTOR);
    for (size_t i = 0; i < num_bytes; i++, p++) {
      position += __sm_chunk_calc_vector_size(*p);
    }

    bv -= num_bytes * SM_FLAGS_PER_INDEX_BYTE;
    for (size_t i = 0; i < bv; i++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*chunk->m_data, i);
      if (flags == SM_PAYLOAD_MIXED) {
        position++;
      }
    }
  }

  return position;
}

/** @brief Initializes a chunk structure with raw data.
 *
 * This function casts the provided raw data pointer to a `__sm_bitvec_t`
 * pointer and stores it in the `m_data` member of the `__sm_chunk_t` structure.
 *
 * @param chunk Pointer to the chunk structure to initialize.
 * @param data Pointer to the raw data to be used by the chunk.
 */
static inline void
__sm_chunk_init(__sm_chunk_t *chunk, uint8_t *data)
{
  chunk->m_data = (__sm_bitvec_t *)data;
}

/** @brief Calculates the capacity of a chunk in bits.
 *
 * Determines the maximum number of bit available for storing data within the
 * chunk.  The capacity is typically `SM_CHUNK_MAX_CAPACITY` bits, but it can
 * be reduced if the chunk contains flags indicating an unused portion of the
 * chunk or larger than max capacity when this chunk represents RLE-encoded
 * data.
 *
 * @param[in] map The sparsemap that contains this chunk.
 * @param[in] chunk Pointer to the chunk to examine.
 * @param[in] start The starting offset of this chunk.
 * @return The maximum usable capacity of the chunk in bits.
 */
static size_t
__sm_chunk_get_capacity(__sm_chunk_t *chunk)
{
  /* Handle RLE which encodes the capacity in the vector. */
  if (__sm_chunk_is_rle(chunk)) {
    return __sm_chunk_rle_get_capacity(chunk);
  }

  size_t capacity = SM_CHUNK_MAX_CAPACITY;
  register uint8_t *p = (uint8_t *)chunk->m_data;

  for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
    if (!*p || *p == 0xff) {
      continue;
    }
    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        capacity -= SM_BITS_PER_VECTOR;
      }
    }
  }
  return capacity;
}

static void
__sm_chunk_increase_capacity(__sm_chunk_t *chunk, size_t capacity)
{
  __sm_assert(capacity % SM_BITS_PER_VECTOR == 0);
  __sm_assert(capacity <= SM_CHUNK_MAX_CAPACITY);
  __sm_assert(capacity > __sm_chunk_get_capacity(chunk));

  size_t initial_capacity = __sm_chunk_get_capacity(chunk);
  if (capacity <= initial_capacity || capacity > SM_CHUNK_MAX_CAPACITY) {
    return;
  }

  size_t increased = 0;
  register uint8_t *p = (uint8_t *)chunk->m_data;
  for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
    if (!*p || *p == 0xff) {
      continue;
    }
    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        *p &= ~((__sm_bitvec_t)SM_PAYLOAD_ONES << (j * 2));
        *p |= ((__sm_bitvec_t)SM_PAYLOAD_ZEROS << (j * 2));
        increased += SM_BITS_PER_VECTOR;
        if (increased + initial_capacity == capacity) {
          __sm_assert(__sm_chunk_get_capacity(chunk) == capacity);
          return;
        }
      }
    }
  }
  __sm_assert(__sm_chunk_get_capacity(chunk) == capacity);
}

/** @brief Examines the chunk to determine if it is empty.
 *
 * @param[in] chunk The chunk in question.
 * @returns true if this __sm_chunk_t is empty
 */
static bool
__sm_chunk_is_empty(__sm_chunk_t *chunk)
{
  if (chunk->m_data[0] != 0) {
    /* A chunk is considered empty if all flags are SM_PAYLOAD_ZERO or _NONE. */
    register uint8_t *p = (uint8_t *)chunk->m_data;
    for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
      if (*p) {
        for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
          size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
          if (flags != SM_PAYLOAD_NONE && flags != SM_PAYLOAD_ZEROS) {
            return false;
          }
        }
      }
    }
  }
  /* The __sm_chunk_t is empty if all flags (in m_data[0]) are zero. */
  return true;
}

/** @brief Examines the chunk to determine its size.
 *
 * @param[in] chunk The chunk in question.
 * @returns the size of the data buffer, in bytes.
 */
static size_t
__sm_chunk_get_size(__sm_chunk_t *chunk)
{
  /* At least one __sm_bitvec_t is required for the flags (m_data[0]) */
  size_t size = sizeof(__sm_bitvec_t);
  if (!__sm_chunk_is_rle(chunk)) {
    /* Use a lookup table for each byte of the flags */
    register uint8_t *p = (uint8_t *)chunk->m_data;
    for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
      size += sizeof(__sm_bitvec_t) * __sm_chunk_calc_vector_size(*p);
    }
  }
  return size;
}

/** @brief Examines the chunk at \b idx to determine that bit's state (set,
 * or unset).
 *
 * @param[in] chunk The chunk in question.
 * @param[in] idx The 0-based index into this chunk to examine.
 * @returns the value of a bit at index \b idx
 */
static bool
__sm_chunk_is_set(__sm_chunk_t *chunk, size_t idx)
{
  if (__sm_chunk_is_rle(chunk)) {
    if (idx <= __sm_chunk_rle_get_length(chunk)) {
      return true;
    }
    return false;
  } else {
    /* in which __sm_bitvec_t is |idx| stored? */
    size_t bv = idx / SM_BITS_PER_VECTOR;
    __sm_assert(bv < SM_FLAGS_PER_INDEX);

    /* now retrieve the flags of that __sm_bitvec_t */
    size_t flags = SM_CHUNK_GET_FLAGS(*chunk->m_data, bv);
    switch (flags) {
    case SM_PAYLOAD_ZEROS:
    case SM_PAYLOAD_NONE:
      return false;
    case SM_PAYLOAD_ONES:
      return true;
    default:
      __sm_assert(flags == SM_PAYLOAD_MIXED);
      /* FALLTHROUGH */
    }

    /* get the __sm_bitvec_t at |bv| */
    __sm_bitvec_t w = chunk->m_data[1 + __sm_chunk_get_position(chunk, bv)];
    /* and finally check the bit in that __sm_bitvec_t */
    return (w & ((__sm_bitvec_t)1 << (idx % SM_BITS_PER_VECTOR))) > 0;
  }
}

/** @brief Clears bit at the chunk-relative idx.
 *
 * @param[in] chunk The chunk in question.
 * @param[in] idx The chunk-relative 0-based index to clear.
 * @param[in,out] pos When non-zero there is a vector available for MIXED
 * mutations, no need to "grow".  When zero then set to the position in the
 * buffer where there needs to be a new vector to grow into.
 * @return SM_OK, GROW, or SHRINK; grow indicates the need for an additional
 * vector (transitioning from ZEROS or ONES to MIXED), shrink that the
 * additional mixed vector isn't needed anymore (transitioning from MIXED to
 * ZEROS or ONES).
 */
static int
__sm_chunk_clr_bit(__sm_chunk_t *chunk, sparsemap_idx_t idx, size_t *pos)
{
  __sm_bitvec_t w;
  size_t bv = idx / SM_BITS_PER_VECTOR;

  __sm_assert(bv < SM_FLAGS_PER_INDEX);

  switch (SM_CHUNK_GET_FLAGS(*chunk->m_data, bv)) {
  case SM_PAYLOAD_ZEROS:
    /* The bit is already clear, no-op. */
    *pos = 0;
    return SM_OK;
    break;
  case SM_PAYLOAD_ONES:
    /* What was all ones transitions to mixed, which requires another vector. */
    if (*pos == 0) {
      *pos = (size_t)1 + __sm_chunk_get_position(chunk, bv);
      return SM_NEEDS_TO_GROW;
    }
    SM_CHUNK_SET_FLAGS(*chunk->m_data, bv, SM_PAYLOAD_MIXED);
    w = chunk->m_data[*pos];
    w &= ~((__sm_bitvec_t)1 << (idx % SM_BITS_PER_VECTOR));
    /* Update the mixed vector. */
    chunk->m_data[*pos] = w;
    return SM_OK;
    break;
  case SM_PAYLOAD_MIXED:
    *pos = 1 + __sm_chunk_get_position(chunk, bv);
    w = chunk->m_data[*pos];
    w &= ~((__sm_bitvec_t)1 << (idx % SM_BITS_PER_VECTOR));
    /* Did the vector transition from mixed to all zeros? If so, remove it. */
    if (w == 0) {
      SM_CHUNK_SET_FLAGS(*chunk->m_data, bv, SM_PAYLOAD_ZEROS);
      return SM_NEEDS_TO_SHRINK;
    }
    /* Update the mixed vector. */
    chunk->m_data[*pos] = w;
    break;
  case SM_PAYLOAD_NONE:
    /* FALLTHROUGH */
  default:
    __sm_assert(!"shouldn't be here");
#ifdef DEBUG
    abort();
#endif
    break;
  }
  return SM_OK;
}

/** @brief Sets a bit at the chunk-relative idx.
 *
 * @param[in] chunk The chunk in question.
 * @param[in] idx The chunk-relative 0-based index to set.
 * @param[in,out] pos When non-zero there is a vector available for MIXED
 * mutations, no need to "grow".  When zero then set to the position in the
 * buffer where there needs to be a new vector to grow into.
 * @return SM_OK, GROW, or SHRINK; grow indicates the need for an additional
 * vector (transitioning from ZEROS or ONES to MIXED), shrink that the
 * additional mixed vector isn't needed anymore (transitioning from MIXED to
 * ZEROS or ONES).
 */
static int
__sm_chunk_set_bit(__sm_chunk_t *chunk, sparsemap_idx_t idx, size_t *pos)
{
  /* Where in the descriptor does this idx fall, which flag should we examine? */
  size_t bv = idx / SM_BITS_PER_VECTOR;
  __sm_assert(bv < SM_FLAGS_PER_INDEX);
  __sm_assert(__sm_chunk_is_rle(chunk) == false);

  switch (SM_CHUNK_GET_FLAGS(*chunk->m_data, bv)) {
  case SM_PAYLOAD_ONES:
    /* The bit is already set, no-op. */
    *pos = 0;
    return SM_OK;
    break;
  case SM_PAYLOAD_ZEROS:
    /* What was all zeros transitions to mixed, which requires another vector. */
    if (*pos == 0) {
      *pos = (size_t)1 + __sm_chunk_get_position(chunk, bv);
      return SM_NEEDS_TO_GROW;
    }
    SM_CHUNK_SET_FLAGS(*chunk->m_data, bv, SM_PAYLOAD_MIXED);
    /* FALLTHROUGH */
  case SM_PAYLOAD_MIXED:
    *pos = 1 + __sm_chunk_get_position(chunk, bv);
    __sm_bitvec_t w = chunk->m_data[*pos];
    w |= (__sm_bitvec_t)1 << (idx % SM_BITS_PER_VECTOR);
    /* Did the vector transition from mixed to all ones? If so, remove it. */
    if (w == ~(__sm_bitvec_t)0) {
      SM_CHUNK_SET_FLAGS(*chunk->m_data, bv, SM_PAYLOAD_ONES);
      return SM_NEEDS_TO_SHRINK;
    }
    /* Update the mixed vector. */
    chunk->m_data[*pos] = w;
    break;
  case SM_PAYLOAD_NONE:
    /* FALLTHROUGH */
  default:
    // __sm_when_diag({ fprintf(stdout, "\n%s\n", _qcc_format_chunk(0, chunk, true)); })
#ifdef DEBUG
    abort();
#endif
    break;
  }
  return SM_OK;
}

/** @brief Finds the index of the \b n'th bit after \b offset bits with \b
 * value.
 *
 * Scans the \b chunk until after \b offset bits (of any value) have
 * passed and then begins counting the bits that match \b value looking
 * for the \b n'th bit.  It may not be in this chunk, when it is offset is set.
 *
 * @param[in] chunk The chunk in question.
 * @param[in] value Informs what we're seeking, a set or unset bit's position.
 * @param offset[in,out] Sets \b offset to n if the n'th bit was found
 * in this __sm_chunk_t, or reduced value of \b n bits observed the search up
 * to a maximum of SM_BITS_PER_VECTOR.
 * @returns the 0-based index of the n'th set bit when found, otherwise
 * SM_BITS_PER_VECTOR
 */
static size_t
__sm_chunk_select(__sm_chunk_t *chunk, ssize_t n, ssize_t *offset, bool value)
{
  size_t ret = 0;
  register uint8_t *p;

  p = (uint8_t *)chunk->m_data;
  for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
    if (*p == 0 && value) {
      ret += (size_t)SM_FLAGS_PER_INDEX_BYTE * SM_BITS_PER_VECTOR;
      continue;
    }

    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        continue;
      }
      if (flags == SM_PAYLOAD_ZEROS) {
        if (value == true) {
          ret += SM_BITS_PER_VECTOR;
          continue;
        } else {
          if (n > SM_BITS_PER_VECTOR) {
            n -= SM_BITS_PER_VECTOR;
            ret += SM_BITS_PER_VECTOR;
            continue;
          }
          *offset = -1;
          return ret + n;
        }
      }
      if (flags == SM_PAYLOAD_ONES) {
        if (value == true) {
          if (n > SM_BITS_PER_VECTOR) {
            n -= SM_BITS_PER_VECTOR;
            ret += SM_BITS_PER_VECTOR;
            continue;
          }
          *offset = -1;
          return ret + n;
        } else {
          ret += SM_BITS_PER_VECTOR;
          continue;
        }
      }
      if (flags == SM_PAYLOAD_MIXED) {
        __sm_bitvec_t w = chunk->m_data[1 + __sm_chunk_get_position(chunk, i * SM_FLAGS_PER_INDEX_BYTE + j)];
        for (int k = 0; k < SM_BITS_PER_VECTOR; k++) {
          if (value) {
            if (w & ((__sm_bitvec_t)1 << k)) {
              if (n == 0) {
                *offset = -1;
                return ret;
              }
              n--;
            }
            ret++;
          } else {
            if (!(w & ((__sm_bitvec_t)1 << k))) {
              if (n == 0) {
                *offset = -1;
                return ret;
              }
              n--;
            }
            ret++;
          }
        }
      }
    }
  }
  *offset = n;
  return ret;
}

/**
 * @brief Ranks bits within the range [from, to].
 *
 * Scans the \b chunk until after \b from bits (of any value) have passed and
 * then begins counting the bits that match \b value. The result should never be
 * greater than \b to + 1.  The range is inclusive and indexes are
 * 0-based. Calling this function with `from = 0` and `to = 0`, which is the
 * range [0, 0], will compare 1 bit at the position 0 against value. The range
 * [0, 9] will examine 10 bits, starting with the 0th and ending with the 9th and
 * return at most a count of 10.
 *
 * @param[out] rank Additional results, remaining bits and last position.
 * @param[in] state The state of bits, set or unset, to rank.
 * @param[in] chunk The chunk to examine.
 * @param[in] from The start of the range, 0-indexed and inclusive.
 * @param[in] to The end of the range, 0-indexed and inclusive.
 * @return the sum of the set bits in the range [from, to], 0 if none.
 */
static size_t
__sm_chunk_rank(__sm_chunk_rank_t *rank, bool state, __sm_chunk_t *chunk, size_t from, size_t to)
{
  size_t amt = 0;
  size_t cap = __sm_chunk_get_capacity(chunk);

  __sm_assert(to >= from);
  rank->rem = cap;
  rank->pos = 0;

  if (from >= cap) {
    rank->pos = cap;
    rank->rem = 0;
    return amt;
  }

  if (SM_IS_CHUNK_RLE(chunk)) {
    /* This is a run-length (RLE) encoded chunk. */
    size_t end = __sm_chunk_rle_get_length(chunk) - 1;
    rank->rem = 0;
    if (state) {
      if (from <= end) {
        amt = to - from + 1;
        rank->pos = to;
        if (to > end) {
          amt -= to - end;
        }
      } else {
        rank->pos = end;
      }
    } else {
      if (to < cap) {
        if (to > end) {
          amt = to - end - 1;
        }
        rank->pos = to + 1;
      } else {
        amt = (cap - (end + 1)) - 1;
        rank->pos = cap;
      }
    }
  } else {
    /* This chunk has sparse encoding. */

    uint8_t *vec = (uint8_t *)chunk->m_data;
    __sm_bitvec_t w, mw;
    uint64_t mask, to_mask, from_mask;
    size_t pc;

    for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, vec++) {
      for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
        size_t flags = SM_CHUNK_GET_FLAGS(*vec, j);

        switch (flags) {

        case SM_PAYLOAD_ZEROS:
          rank->rem = 0;
          if (to >= SM_BITS_PER_VECTOR) {
            rank->pos += SM_BITS_PER_VECTOR;
            to -= SM_BITS_PER_VECTOR;
            if (from >= SM_BITS_PER_VECTOR) {
              from = from - SM_BITS_PER_VECTOR;
            } else {
              if (!state) {
                amt += SM_BITS_PER_VECTOR - from;
              }
              from = 0;
            }
          } else {
            rank->pos += to + 1;
            if (!state) {
              if (from > to) {
                from -= to;
              } else {
                amt += to + 1 - from;
                from = 0;
                goto done;
              }
            } else {
              goto done;
            }
          }
          break;

        case SM_PAYLOAD_ONES:
          rank->rem = UINT64_MAX;
          if (to >= SM_BITS_PER_VECTOR) {
            rank->pos += SM_BITS_PER_VECTOR;
            to -= SM_BITS_PER_VECTOR;
            if (from >= SM_BITS_PER_VECTOR) {
              from = from - SM_BITS_PER_VECTOR;
            } else {
              if (state) {
                amt += SM_BITS_PER_VECTOR - from;
              }
              from = 0;
            }
          } else {
            rank->pos += to + 1;
            if (state) {
              if (from > to) {
                from = from - to;
              } else {
                amt += to + 1 - from;
                from = 0;
                goto done;
              }
            } else {
              goto done;
            }
          }
          break;

        case SM_PAYLOAD_MIXED:
          w = chunk->m_data[1 + __sm_chunk_get_position(chunk, i * SM_FLAGS_PER_INDEX_BYTE + j)];
          if (to >= SM_BITS_PER_VECTOR) {
            rank->pos += SM_BITS_PER_VECTOR;
            to -= SM_BITS_PER_VECTOR;
            mask = from == 0 ? UINT64_MAX : ~(UINT64_MAX >> (SM_BITS_PER_VECTOR - (from >= 64 ? 64 : from)));
            mw = (state ? w : ~w) & mask;
            pc = popcountll(mw);
            amt += pc;
            from = (from > SM_BITS_PER_VECTOR) ? from - SM_BITS_PER_VECTOR : 0;
          } else {
            rank->pos += to + 1;
            to_mask = (to == 63) ? UINT64_MAX : ((uint64_t)1 << (to + 1)) - 1;
            from_mask = from == 0 ? UINT64_MAX : ~(UINT64_MAX >> (SM_BITS_PER_VECTOR - (from >= 64 ? 64 : from)));
            /* Create a mask for the range [from, to] and use popcount. */
            mask = to_mask & from_mask;
            mw = (state ? w : ~w) & mask;
            pc = popcountll(mw);
            amt += pc;
            rank->rem = mw >> ((from > 63) ? 63 : from);
            from = from > to ? from - to + 1 : 0;
            goto done;
          }
          break;

        case SM_PAYLOAD_NONE:
        default:
          continue;
        }
      }
    }
  }
done:;
  return amt;
}

/** @brief Calls \b scanner with sm_bitmap_t for each vector in this chunk.
 *
 * Decompresses the whole chunk into separate bitmaps then calls visitor's
 * \b #operator() function for all bits that are set.
 *
 * @param[in] chunk The chunk in question.
 * @param[in] start Starting offset
 * @param[in] scanner Callback function which receives an array of indices (with
 * bits set to 1), the size of the array and an auxiliary pointer provided by
 * the caller.
 * @param[in] skip The number of bits to skip in the beginning.
 * @returns the number of (set) bits that were passed to the scanner
 */
static size_t
__sm_chunk_scan(__sm_chunk_t *chunk, __sm_idx_t start, void (*scanner)(uint32_t[], size_t, void *aux), size_t skip, void *aux)
{
  size_t ret = 0;
  register uint8_t *p = (uint8_t *)chunk->m_data;
  uint32_t buffer[SM_BITS_PER_VECTOR];
  for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
    if (*p == 0) {
      /* Skip chunks that are all zeroes. */
      skip -= skip > SM_BITS_PER_VECTOR ? SM_BITS_PER_VECTOR : skip;
      continue;
    }

    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE || flags == SM_PAYLOAD_ZEROS) {
        /* Skip when all zeroes. */
        skip -= skip > SM_BITS_PER_VECTOR ? SM_BITS_PER_VECTOR : skip;
      } else if (flags == SM_PAYLOAD_ONES) {
        if (skip) {
          if (skip >= SM_BITS_PER_VECTOR) {
            skip -= SM_BITS_PER_VECTOR;
            ret += SM_BITS_PER_VECTOR;
            continue;
          }
          size_t n = 0;
          for (size_t b = 0; b < SM_BITS_PER_VECTOR; b++) {
            buffer[n++] = start + ret + b;
          }
          scanner(&buffer[0], n, aux);
          ret += n;
          skip = 0;
        } else {
          for (size_t b = 0; b < SM_BITS_PER_VECTOR; b++) {
            buffer[b] = start + ret + b;
          }
          scanner(&buffer[0], SM_BITS_PER_VECTOR, aux);
          ret += SM_BITS_PER_VECTOR;
        }
      } else if (flags == SM_PAYLOAD_MIXED) {
        __sm_bitvec_t w = chunk->m_data[1 + __sm_chunk_get_position(chunk, i * SM_FLAGS_PER_INDEX_BYTE + j)];
        size_t n = 0;
        if (skip) {
          if (skip >= SM_BITS_PER_VECTOR) {
            skip -= SM_BITS_PER_VECTOR;
            ret += SM_BITS_PER_VECTOR;
            continue;
          }
          for (int b = 0; b < SM_BITS_PER_VECTOR; b++) {
            if (skip > 0) {
              skip--;
              continue;
            }
            if (w & ((__sm_bitvec_t)1 << b)) {
              buffer[n++] = start + ret + b;
              ret++;
            }
          }
        } else {
          for (int b = 0; b < SM_BITS_PER_VECTOR; b++) {
            if (w & ((__sm_bitvec_t)1 << b)) {
              buffer[n++] = start + ret + b;
            }
          }
          ret += n;
        }
        __sm_assert(n > 0);
        scanner(&buffer[0], n, aux);
      }
    }
  }
  return ret;
}

/** @brief Provides the number of chunks currently in the map.
 *
 * @param[in] chunk  The sparsemap_t in question.
 * @returns the number of chunks in the sparsemap
 */
static size_t
__sm_get_chunk_count(sparsemap_t *map)
{
  return *(uint32_t *)&map->m_data[0];
}

/** @brief Encapsulates the method to find the starting address of a chunk's
 * data.
 *
 * @param[in] map The sparsemap_t in question.
 * @param[in] offset The offset in bytes for the desired chunk.
 * @returns the data for the specified \b offset
 */
static inline uint8_t *
__sm_get_chunk_data(sparsemap_t *map, size_t offset)
{
  return &map->m_data[SM_SIZEOF_OVERHEAD + offset];
}

/** @brief Either max capacity or limited by next chunk.
 *
 * Use this function to determine the available room (capacity) in bits between
 * the end of an RLE chunk and the beginning of the next chunk (if one exists).
 * This function will assume that \b offset is the location of an RLE chunk and
 * then using that probe for the start of the next chunk.
 *
 * @param[in] map The map containing the RLE chunk.
 * @param[in] start The starting index of the RLE chunk.
 * @param[in] offset The offset in m_data of the RLE chunk.
 * @return a value between [0, SM_CHUNK_RLE_MAX_CAPACITY] based on the
 * position/existence of the next chunk in the map.
 */
static size_t
__sm_chunk_rle_capacity_limit(sparsemap_t *map, __sm_idx_t start, size_t offset)
{
  size_t next_offset = offset + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
  if (next_offset < map->m_data_used - (SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t))) {
    uint8_t *p = __sm_get_chunk_data(map, next_offset);
    __sm_idx_t next_start = *(__sm_idx_t *)p;
    return next_start - start;
  }
  return SM_CHUNK_RLE_MAX_CAPACITY;
}

/** @brief Encapsulates the method to find the address of the first unused byte
 * in \b m_data.
 *
 * @param[in] map The sparsemap_t in question.
 * @returns a pointer after the end of the used data
 */
static uint8_t *
__sm_get_chunk_end(sparsemap_t *map)
{
  uint8_t *p = __sm_get_chunk_data(map, 0);
  size_t count = __sm_get_chunk_count(map);
  for (size_t i = 0; i < count; i++) {
    p += SM_SIZEOF_OVERHEAD;
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);
    p += __sm_chunk_get_size(&chunk);
  }
  return p;
}

/** @brief Aligns to SM_CHUNK_CAPACITY a given index \b idx.
 *
 * Due to integer division discarding the remainder, the final return value is
 * always rounded down to the nearest multiple of SM_CHUNK_MAX_CAPACITY.
 *
 * @param[in] idx The index to align.
 * @returns the aligned offset (aligned to __sm_chunk_t capacity)
 */
static __sm_idx_t
__sm_get_chunk_aligned_offset(size_t idx)
{
  const size_t capacity = SM_CHUNK_MAX_CAPACITY;
  return (idx / capacity) * capacity;
}

/** @brief Provides the byte size amount of \b m_data consumed.
 *
 * @param[in] map The sparsemap_t in question.
 * @returns the used size in the data buffer
 */
static size_t
__sm_get_size_impl(sparsemap_t *map)
{
  uint8_t *start = __sm_get_chunk_data(map, 0);
  uint8_t *p = start;

  size_t count = __sm_get_chunk_count(map);
  for (size_t i = 0; i < count; i++) {
    p += SM_SIZEOF_OVERHEAD;
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);
    p += __sm_chunk_get_size(&chunk);
  }
  return SM_SIZEOF_OVERHEAD + p - start;
}

/** @brief Provides the byte offset of the chunk containing the bit at \b idx.
 *
 * Chunks live within the m_data buffer space, this function will find the
 * 0-based offset into that buffer of the chunk containing idx if one exists.
 * If the index falls outside of all chunks it returns the offset of the chunk
 * before the idx, if there are no chunks in the map it returns -1.
 *
 * @param[in] map A sparsemap_t.
 * @param[in] idx Seeking the offset of a chunk for this index.
 * @returns the offset of the chunk in m_data that contains idx, or -1 if there
 * are no chunks.
 */
static ssize_t
__sm_get_chunk_offset(sparsemap_t *map, sparsemap_idx_t idx)
{
  size_t count = __sm_get_chunk_count(map);

  if (count == 0) {
    return -1;
  }

  uint8_t *start = __sm_get_chunk_data(map, 0);
  uint8_t *p = start;

  for (size_t i = 0; i < count - 1; i++) {
    __sm_idx_t s = *(__sm_idx_t *)p;
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
    __sm_assert(s == __sm_get_chunk_aligned_offset(s));
    if (s >= idx || idx < s + __sm_chunk_get_capacity(&chunk)) {
      break;
    }
    p += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
  }

  return (ssize_t)(p - start);
}

/** @brief Sets the number of __sm_chunk_t's.
 *
 * @param[in] map The sparsemap_t in question.
 * @param[in] new_count The new number of chunks in the map.
 */
static void
__sm_set_chunk_count(sparsemap_t *map, size_t new_count)
{
  *(uint32_t *)&map->m_data[0] = (uint32_t)new_count;
}

/** @brief Appends raw data at the end of used portion of \b m_data.
 *
 * @param[in] map The sparsemap_t in question.
 * @param[in] buffer The bytes to copy into \b m_data.
 * @param[in] buffer_size The size of the byte array \b buffer to copy.
 */
static void
__sm_append_data(sparsemap_t *map, uint8_t *buffer, size_t buffer_size)
{
  __sm_assert(map->m_data_used + buffer_size <= map->m_capacity);

  memcpy(&map->m_data[map->m_data_used], buffer, buffer_size);
  map->m_data_used += buffer_size;
}

/** @brief Inserts data at \b offset in the middle of \b m_data.
 *
 * @param[in] map The sparsemap_t in question.
 * @param[in] offset The offset in bytes into \b m_data to place the buffer.
 * @param[in] buffer The bytes to copy into \b m_data.
 * @param[in] buffer_size The size of the byte array \b buffer to copy.
 */
void
__sm_insert_data(sparsemap_t *map, size_t offset, uint8_t *buffer, size_t buffer_size)
{
  __sm_assert(map->m_data_used + buffer_size <= map->m_capacity);

  uint8_t *p = __sm_get_chunk_data(map, offset);
  memmove(p + buffer_size, p, map->m_data_used - offset);
  memcpy(p, buffer, buffer_size);
  map->m_data_used += buffer_size;
}

/** @brief Removes data from \b m_data.
 *
 * @param[in] map The sparsemap_t in question.
 * @param[in] offset The offset in bytes into \b m_data at which to excise data.
 * @param[in] gap_size The size of the excision.
 */
static void
__sm_remove_data(sparsemap_t *map, size_t offset, size_t gap_size)
{
  __sm_assert(map->m_data_used >= gap_size);
  uint8_t *p = __sm_get_chunk_data(map, offset);
  memmove(p, p + gap_size, map->m_data_used - offset - gap_size);
  map->m_data_used -= gap_size;
}

/** @brief Coalesces chunks adjacent when they form or extend a run.
 *
 * This is called from __sm_chunk_set/unset/merge/split functions when a
 * there is a chance that chunks should combine into runs so as to use less
 * space in the map.
 *
 * The provided chunk may have two adjacent chunks, this function first
 * processes the chunk to the left and then the one to the right.
 *
 * In the case that there is a chunk to the left (with a lower starting index)
 * we examine it's type and ending offset as well as it's run length.  Either
 * type of chunk (sparse and RLE) can have a run.  In the case of an RLE chunk
 * that's all it can express.  With a sparse chunk a run is defined as adjacent
 * set bits starting at the 0th index of the chunk and extending up to at most
 * the maximum size of a chunk without gaps ([1..SM_CHUNK_MAX_CAPACITY] in
 * length).  When the left chunk's run ends at the starting index of this chunk
 * we can combine them. Combining these two will always result in a RLE chunk.
 *
 * Once that is finished... we may have something to the right as well.  We look
 * for an adjacent chunk, then determine if it has a run with a starting point
 * adjacent to the end of a run in this chunk.  At this point we may have
 * mutated and coalesced the left into the center chunk which we further mutate
 * and combine with the right.  At most we can combine three chunks into one in
 * these two phases.
 *
 * @returns the number of chunks to be removed from the map
 */
static int
__sm_coalesce_chunk(sparsemap_t *map, __sm_chunk_t *chunk, size_t offset, __sm_idx_t start, uint8_t *p)
{
  size_t num_removed = 0, run_length = __sm_chunk_get_run_length(chunk);
  /* Did this chunk become all ones, can we compact it with adjacent chunks? */
  if (run_length > 0) {
    __sm_chunk_t adj;

    /* Is there a previous chunk? */
    if (offset > 0) {
      size_t adj_offset = (size_t)__sm_get_chunk_offset(map, start - 1);
      if (adj_offset < offset) {
        uint8_t *adj_p = __sm_get_chunk_data(map, adj_offset);
        __sm_idx_t adj_start = *(__sm_idx_t *)adj_p;
        __sm_chunk_init(&adj, adj_p + SM_SIZEOF_OVERHEAD);
        /* Is the adjacent chunk on the left RLE or a sparse chunk of all ones? */
        if (__sm_chunk_is_rle(&adj) || adj.m_data[0] == ~(__sm_bitvec_t)0) {
          /* Does it align with this full sparse chunk? */
          size_t adj_length = __sm_chunk_get_run_length(&adj);
          if (adj_start + adj_length == start) {
            if (SM_CHUNK_MAX_CAPACITY + run_length < SM_CHUNK_RLE_MAX_LENGTH) {
              /* The stars have aligned, transform to RLE and combine them! */
              // __sm_when_diag({ fprintf(stdout, "\n%s\n", QCC_showChunk(adj_p, 0)); });
              // __sm_when_diag({ fprintf(stdout, "\n%s\n", QCC_showChunk(p, 0)); });
              __sm_chunk_set_rle(&adj);
              __sm_chunk_rle_set_length(&adj, adj_length + run_length);
              __sm_remove_data(map, offset, SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(chunk));
              __sm_chunk_rle_set_capacity(&adj, __sm_chunk_rle_capacity_limit(map, adj_start, adj_offset));
              __sm_set_chunk_count(map, __sm_get_chunk_count(map) - 1);
              // __sm_when_diag({ fprintf(stdout, "\n%s\n", QCC_showChunk(adj_p, 0)); });

              /* Now chunk is shifted to the left, it becomes the adjacent chunk. */
              p = adj_p;
              offset = adj_offset;
              start = adj_start;
              __sm_chunk_init(chunk, p + SM_SIZEOF_OVERHEAD);
              num_removed += 1;
            }
          }
        }
      }
    }

    /* Is there a next chunk? */
    if (__sm_chunk_is_rle(chunk) || chunk->m_data[0] == ~(__sm_bitvec_t)0) {
      size_t adj_offset = offset + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
      if (adj_offset < map->m_data_used - (SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t))) {
        uint8_t *adj_p = __sm_get_chunk_data(map, adj_offset);
        __sm_idx_t adj_start = *(__sm_idx_t *)adj_p;
        __sm_chunk_init(&adj, adj_p + SM_SIZEOF_OVERHEAD);
        /* Is the adjacent right chunk RLE or a sparse with a run of ones? */
        size_t adj_length = __sm_chunk_get_run_length(&adj);
        if (adj_length) {
          /* Does it align with this full sparse chunk? */
          size_t length = __sm_chunk_get_run_length(chunk);
          if (start + length == adj_start) {
            if (adj_length + length < SM_CHUNK_RLE_MAX_LENGTH) {
              /* The stars have aligned, transform to RLE and combine them! */
              // __sm_when_diag({ fprintf(stdout, "\n%s\n", QCC_showChunk(p, 0)); });
              // __sm_when_diag({ fprintf(stdout, "\n%s\n", QCC_showChunk(adj_p, 0)); });
              __sm_chunk_rle_set_length(chunk, length + adj_length);
              __sm_remove_data(map, adj_offset, SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&adj));
              __sm_chunk_set_rle(chunk);
              __sm_chunk_rle_set_capacity(chunk, __sm_chunk_rle_capacity_limit(map, start, offset));
              __sm_set_chunk_count(map, __sm_get_chunk_count(map) - 1);
              // __sm_when_diag({ fprintf(stdout, "\n%s\n", QCC_showChunk(p, 0)); });
              num_removed += 1;
            }
          }
        }
      }
    }
  }

  return num_removed;
}

/**
 * TODO
 *
 * @returns the number of chunks to be removed from the map
 */
size_t
__sm_coalesce_map(sparsemap_t *map)
{
  __sm_chunk_t chunk;
  size_t n = 0, offset = 0, count = __sm_get_chunk_count(map);
  uint8_t *p = __sm_get_chunk_data(map, offset);
  __sm_idx_t start;

  while (count > 1) {
    start = *(__sm_idx_t *)p;
    __sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
    size_t amt = __sm_coalesce_chunk(map, &chunk, offset, start, p);
    if (amt > 0) {
      n += amt;
      count = __sm_get_chunk_count(map);
    } else {
      p += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
      count--;
    }
  }

  return n;
}

/** @brief Separates an RLE chunk into new chunks when necessary.
 *
 * This is called from __sm_chunk_set/unset/merge/split functions when a
 * run-length encoded (RLE) chunk must be mutated into one or more new chunks.
 *
 * This function expects that the separation information is complete and that
 * the pivot chunk has yet to be created.  The target will always be RLE and the
 * piviot will always be a new sparse chunk.  The hard part is where the pivot
 * lies in relation to the target.
 *
 * - left aligned
 * - right aligned
 * - centrally aligned
 *
 * When left aligned the chunk-aligned starting index of the pivot matches the
 * starting index of the target. This results in two chunks, one new (the pivot)
 * on the left, and one shortened RLE on the right.
 *
 * When right aligned there are two cases, the second more common one is when
 * the chunk-aligned starting index of the pivot plus its length extends beyond
 * the end of the run length of the target RLE chunk but is still within the
 * capacity of the RLE chunk. This again results in two chunks, one on the left
 * for the remainder of the run and one to the right.  In rare cases the end of
 * the pivot chunk perfectly aligns with the end of the target's length.
 *
 * The last case is when the chunk-aligned starting index is somewhere within
 * the body of the target.  This results in three chunks; left, right, and pivot
 * (or center).
 *
 * In all three cases the new chunks (left and right) may be either RLE or
 * sparse encoded, that's TBD based on their sizes after the pivot area is
 * removed from the body of the run.
 *
 * @param map[in] The sparsemap containing this chunk.
 * @param sep[in,out] An struct with information necessary for this operation.
 * @param idx[in] The map-relative 0-based index of the bit to be mutated.
 * @param state[in] The ending state of idx; set/1, unset/0, or unmodified/-1.
 * @return non-zero on failure, and errno to ENOSPC when the map can't fit the mutations
 */
static int
__sm_separate_rle_chunk(sparsemap_t *map, __sm_chunk_sep_t *sep, sparsemap_idx_t idx, int state)
{
  __sm_chunk_t pivot_chunk;
  size_t aligned_idx;
  __sm_chunk_t lrc;

  __sm_assert(state == 0 || state == 1 || state == -1);
  __sm_assert(SM_IS_CHUNK_RLE(sep->target.chunk));
  if (state == 1) {
    /* setting a bit */
    __sm_assert(idx < sep->target.capacity);
    __sm_assert(idx > sep->target.length + sep->target.start);
  } else if (state == 0) {
    /* clearing a bit */
    __sm_assert(idx >= sep->target.start);
    __sm_assert(idx < sep->target.length + sep->target.start);
  } else if (state == -1) {
    /* If `state == -1` we are splitting at idx but leaving map unmodified. */
  }

  memset(sep->buf, 0, (SM_SIZEOF_OVERHEAD * 3) + (sizeof(__sm_bitvec_t) * 6));

  /* Find the starting offset for our pivot chunk ... */
  aligned_idx = __sm_get_chunk_aligned_offset(idx);
  __sm_assert(idx >= aligned_idx && idx < (aligned_idx + SM_CHUNK_MAX_CAPACITY));
  /* avoid changing the map->m_data and for now work in our buf ... */
  sep->pivot.p = sep->buf;
  *(__sm_idx_t *)sep->pivot.p = aligned_idx;
  __sm_chunk_init(&pivot_chunk, sep->pivot.p + SM_SIZEOF_OVERHEAD);

  /* The pivot, extracted from a run, starts off as all 1s. */
  pivot_chunk.m_data[0] = ~(__sm_bitvec_t)0;

  if (state == 0) {
    /* To unset, change the flag at the position of the idx to "mixed" ... */
    SM_CHUNK_SET_FLAGS(pivot_chunk.m_data[0], idx / SM_BITS_PER_VECTOR, SM_PAYLOAD_MIXED);
    /* and clear only the bit at that index in this chunk. */
    pivot_chunk.m_data[1] = ~(__sm_bitvec_t)0 & ~((__sm_bitvec_t)1 << (idx % SM_BITS_PER_VECTOR));
    sep->pivot.size = SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2;
  } else if (state == 1) {
    if (idx >= sep->target.start && idx < sep->target.start + sep->target.length) {
      /* It's a no-op to set a bit in a range of bits already set. */
      return 0;
    }
    sep->pivot.size = SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2;
  } else if (state == -1) {
    /* Unmodified */
    sep->pivot.size = SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
  }

  /* Where did the pivot chunk fall within the original chunk? */
  do {
    if (aligned_idx == sep->target.start) {
      /* The pivot is left aligned, there will be two chunks in total. */
      sep->count = 2;
      sep->ex[1].start = aligned_idx + SM_CHUNK_MAX_CAPACITY;
      sep->ex[1].end = aligned_idx + sep->target.length - 1;
      sep->ex[1].p = (uint8_t *)((uintptr_t)sep->buf + sep->pivot.size);
      __sm_assert(sep->ex[1].start <= sep->ex[1].end);
      __sm_assert(sep->ex[0].p == 0);
      break;
    }

    if (aligned_idx + SM_CHUNK_MAX_CAPACITY >= sep->target.start + sep->target.length) {
      /* The pivot is right aligned, there will be two chunks in total. */
      sep->count = 2;
      /* Does our pivot extends beyond the end of the run. */
      int amt_over = (int)((aligned_idx + SM_CHUNK_MAX_CAPACITY) - (sep->target.start + sep->target.length));
      if (amt_over > 0) {
        /* The index of the first 0 bit. */
        size_t first_zero = SM_CHUNK_MAX_CAPACITY - amt_over, bv = first_zero / SM_BITS_PER_VECTOR;
        /* Shorten the pivot chunk because it extends beyond the end of the run ... */
        if (amt_over > SM_BITS_PER_VECTOR) {
          pivot_chunk.m_data[0] &= ~(__sm_bitvec_t)0 >> ((amt_over / SM_BITS_PER_VECTOR) * 2);
        }
        if (amt_over % SM_BITS_PER_VECTOR) {
          /* Change only the flag at the position of the last index to "mixed" ... */
          SM_CHUNK_SET_FLAGS(pivot_chunk.m_data[0], bv, SM_PAYLOAD_MIXED);
          /* and unset the bits beyond that. */
          pivot_chunk.m_data[1] = ~(~(__sm_bitvec_t)0 << (first_zero % SM_BITS_PER_VECTOR));
          if (state == -1) {
            sep->pivot.size += sizeof(__sm_bitvec_t);
          }
        }
      }

      /* Are we setting a bit beyond the length where we partially overlap? */
      if (state == 1 && idx > sep->target.start + sep->target.length) {
        /* Change only the flag at the position of the index to "mixed" ... */
        SM_CHUNK_SET_FLAGS(pivot_chunk.m_data[0], idx / SM_BITS_PER_VECTOR, SM_PAYLOAD_MIXED);
        /* and set the bit at that index in this chunk. */
        pivot_chunk.m_data[1] |= (__sm_bitvec_t)1 << (idx % SM_BITS_PER_VECTOR);
      }

      /* Move the pivot chunk over to make room for the new left chunk. */
      memmove((uint8_t *)((uintptr_t)sep->buf + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2), sep->buf, sep->pivot.size);
      memset(sep->buf, 0, SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2);
      sep->pivot.p += SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2;
      /* Record information necessary to construct the left chunk. */
      sep->ex[0].start = sep->target.start;
      sep->ex[0].end = aligned_idx - 1;
      sep->ex[0].p = sep->buf;
      __sm_assert(sep->ex[0].start <= sep->ex[0].end);
      __sm_assert(sep->ex[1].p == 0);
      break;
    }

    if (aligned_idx >= sep->target.start + sep->target.length) {
      /* The pivot is beyond the run but within the capacity, two chunks. */
      sep->count = 2;
      /* Ensure the aligned chunk is fully in the range (length, capacity). */
      if (aligned_idx + SM_CHUNK_MAX_CAPACITY < sep->target.capacity) {
        pivot_chunk.m_data[0] = (__sm_bitvec_t)0;
        if (state == 1) {
          /* Change only the flag at the position of the index to "mixed" ... */
          SM_CHUNK_SET_FLAGS(pivot_chunk.m_data[0], idx / SM_BITS_PER_VECTOR, SM_PAYLOAD_MIXED);
          /* and set the bit at that index in this chunk. */
          pivot_chunk.m_data[1] |= (__sm_bitvec_t)1 << (idx % SM_BITS_PER_VECTOR);
        }
        /* Move the pivot chunk over to make room for the new left chunk. */
        memmove((uint8_t *)((uintptr_t)sep->buf + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2), sep->buf, sep->pivot.size);
        memset(sep->buf, 0, SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2);
        sep->pivot.p += SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2;
        /* Record information necessary to construct the left chunk. */
        sep->ex[0].start = sep->target.start;
        sep->ex[0].end = sep->target.start + sep->target.length - 1;
        sep->ex[0].p = sep->buf;
        break;
      } else {
        // TODO: we can't fit a pivot in this space, yikes! punt, for now...
        return 0;
      }
    }

    /* The pivot's range is central, there will be three chunks in total. */
    sep->count = 3;
    /* Move the pivot chunk over to make room for the new left chunk. */
    memmove((uint8_t *)((uintptr_t)sep->buf + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2), sep->buf, sep->pivot.size);
    memset(sep->buf, 0, SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2);
    sep->pivot.p += SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2;
    /* Record information necessary to construct the left & right chunks. */
    sep->ex[0].start = sep->target.start;
    sep->ex[0].end = aligned_idx - 1;
    sep->ex[0].p = sep->buf;
    sep->ex[1].start = aligned_idx + SM_CHUNK_MAX_CAPACITY;
    sep->ex[1].end = sep->target.length - 1;
    sep->ex[1].p = (uint8_t *)((uintptr_t)sep->buf + (SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2) + sep->pivot.size);
    __sm_assert(sep->ex[0].start < sep->ex[0].end);
    __sm_assert(sep->ex[1].start < sep->ex[1].end);
  } while (0);

  for (int i = 0; i < 2; i++) {
    if (sep->ex[i].p) {
      /* First assign the starting offset ... */
      *(__sm_idx_t *)sep->ex[i].p = sep->ex[i].start;
      /* ... then, construct a chunk ... */
      __sm_chunk_init(&lrc, sep->ex[i].p + SM_SIZEOF_OVERHEAD);
      /* ... determine the type of chunk required ... */
      if (sep->ex[i].end - sep->ex[i].start + 1 > SM_CHUNK_MAX_CAPACITY) {
        /* ... we need a run-length encoding (RLE), chunk ... */
        __sm_chunk_set_rle(&lrc);
        /* ... now assign the length ... */
        __sm_chunk_rle_set_length(&lrc, sep->ex[i].end - sep->ex[i].start + 1);
        /* ... a few things differ left to right ... */
        if (i == 0) {
          /* ... left: extend capacity to the start of the pivot chunk ... */
          __sm_chunk_rle_set_capacity(&lrc, aligned_idx - sep->ex[i].start);
          /* ... and shift the pivot chunk and start of lr[1] left one vector ... */
          memmove((uint8_t *)((uintptr_t)sep->buf + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t)), sep->pivot.p, sep->pivot.size);
          memset((uint8_t *)((uintptr_t)sep->buf + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) + sep->pivot.size), 0, sizeof(__sm_bitvec_t));
          if (sep->ex[1].p) {
            sep->ex[1].p = (uint8_t *)((uintptr_t)sep->ex[1].p - sizeof(__sm_bitvec_t));
          }
        } else {
          /* ... right: extend capacity to max or the start of next chunk */
          size_t right_offset = sep->target.offset + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
          __sm_chunk_rle_set_capacity(&lrc, __sm_chunk_rle_capacity_limit(map, aligned_idx, right_offset));
        }
        /* ... and record our chunk size. */
        sep->ex[i].size = SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
      } else {
        /* ... we need a new sparse chunk, how long should it be? ... */
        size_t lrl = sep->ex[i].end - sep->ex[i].start + 1;
        /* ... how many flags can we mark as all ones? ... */
        if (lrl > SM_BITS_PER_VECTOR) {
          lrc.m_data[0] = ~(__sm_bitvec_t)0 >> ((SM_FLAGS_PER_INDEX - (lrl / SM_BITS_PER_VECTOR)) * 2);
        }
        /* ... do we have a mixed flag to create and vector to assign? ... */
        if (lrl % SM_BITS_PER_VECTOR) {
          SM_CHUNK_SET_FLAGS(lrc.m_data[0], (aligned_idx + lrl) / SM_BITS_PER_VECTOR, SM_PAYLOAD_MIXED);
          lrc.m_data[1] |= ~(__sm_bitvec_t)0 >> (SM_BITS_PER_VECTOR - (lrl % SM_BITS_PER_VECTOR));
          /* ... record our chunk size ... */
          sep->ex[i].size = SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2;
        } else {
          /* ... earlier size estimates were all pessimistic, adjust them ... */
          if (i == 0) {
            /* ... and shift the pivot chunk and start of lr[1] left one vector ... */
            memmove((uint8_t *)((uintptr_t)sep->buf + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t)), sep->pivot.p, sep->pivot.size);
            memset((uint8_t *)((uintptr_t)sep->buf + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) + sep->pivot.size), 0, sizeof(__sm_bitvec_t));
            if (sep->ex[1].p) {
              sep->ex[1].p = (uint8_t *)((uintptr_t)sep->ex[1].p - sizeof(__sm_bitvec_t));
            }
          }
          /* ... record our chunk size ... */
          sep->ex[i].size = SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
        }
      }
      // __sm_when_diag({ /* Sanity check the chunk */ // fprintf(stdout, "\n%s\n", QCC_showChunk(lr[i], 0)); });
    }
  }

  /* Determine if we have room for this construct. */
  sep->expand_by = sep->pivot.size + sep->ex[0].size + sep->ex[1].size - SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
  if (map->m_data_used + sep->expand_by > map->m_capacity) {
    errno = ENOSPC;
    return -1;
  }

  /* Let's knit this into place within the map. */
  __sm_insert_data(map, sep->target.offset + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t), sep->buf + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t), sep->expand_by);
  memcpy(sep->target.p, sep->buf, sep->expand_by + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
  __sm_set_chunk_count(map, __sm_get_chunk_count(map) + (sep->count - 1));

  return 0;
}

/** @brief Merges into the chunk at \b offset all set bits from \b src.
 *
 * @param[in] map The map the chunk belongs too.
 * @param[in] offset The offset of the first bit in the chunk to be merged.
 * @todo merge at the vector level not offset
 */
void
__sm_merge_chunk(sparsemap_t *map, sparsemap_idx_t src_start, sparsemap_idx_t dst_start, sparsemap_idx_t capacity, __sm_chunk_t *dst_chunk,
  __sm_chunk_t *src_chunk)
{
  __sm_bitvec_t fill = 0;
  ssize_t delta = src_start - dst_start;
  for (sparsemap_idx_t j = 0; j < capacity; j++) {
    ssize_t offset = __sm_get_chunk_offset(map, src_start + j);
    if (__sm_chunk_is_set(src_chunk, j) && !__sm_chunk_is_set(dst_chunk, j + delta)) {
      size_t position = 0;
      switch (__sm_chunk_set_bit(dst_chunk, j + delta, &position)) {
      case SM_NEEDS_TO_GROW:
        offset += SM_SIZEOF_OVERHEAD + position * sizeof(__sm_bitvec_t);
        __sm_insert_data(map, offset, (uint8_t *)&fill, sizeof(__sm_bitvec_t));
        __sm_chunk_set_bit(dst_chunk, j + delta, &position);
        break;
      case SM_NEEDS_TO_SHRINK:
        if (__sm_chunk_is_empty(src_chunk)) {
          __sm_assert(position == 1);
          __sm_remove_data(map, offset, SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2);
          __sm_set_chunk_count(map, __sm_get_chunk_count(map) - 1);
        } else {
          offset += SM_SIZEOF_OVERHEAD + position * sizeof(__sm_bitvec_t);
          __sm_remove_data(map, offset, sizeof(__sm_bitvec_t));
        }
        break;
      case SM_OK:
      default:
        break;
      }
    }
  }
}

void
sparsemap_clear(sparsemap_t *map)
{
  if (map == NULL) {
    return;
  }
  memset(map->m_data, 0, map->m_capacity);
  map->m_data_used = SM_SIZEOF_OVERHEAD;
  __sm_set_chunk_count(map, 0);
}

sparsemap_t *
sparsemap(size_t size)
{
  if (size == 0) {
    size = 1024;
  }

  size_t data_size = (size * sizeof(uint8_t));

  /* Ensure that m_data is 8-byte aligned. */
  size_t total_size = sizeof(sparsemap_t) + data_size;
  size_t padding = total_size % 8 == 0 ? 0 : 8 - (total_size % 8);
  total_size += padding;

  sparsemap_t *map = (sparsemap_t *)calloc(1, total_size);
  if (map) {
    uint8_t *data = (uint8_t *)(((uintptr_t)map + sizeof(sparsemap_t)) & ~(uintptr_t)7);
    sparsemap_init(map, data, size);
    __sm_when_diag({ __sm_assert(IS_8_BYTE_ALIGNED(map->m_data)); });
  }
  return map;
}

sparsemap_t *
sparsemap_copy(sparsemap_t *other)
{
  size_t cap = sparsemap_get_capacity(other);
  sparsemap_t *map = sparsemap(cap);
  if (map) {
    map->m_capacity = other->m_capacity;
    map->m_data_used = other->m_data_used;
    memcpy(map->m_data, other->m_data, cap);
  }
  return map;
}

sparsemap_t *
sparsemap_wrap(uint8_t *data, size_t size)
{
  sparsemap_t *map = (sparsemap_t *)calloc(1, sizeof(sparsemap_t));
  if (map) {
    map->m_data = data;
    map->m_data_used = 0;
    map->m_capacity = size;
  }
  return map;
}

void
sparsemap_init(sparsemap_t *map, uint8_t *data, size_t size)
{
  map->m_data = data;
  map->m_data_used = 0;
  map->m_capacity = size;
  sparsemap_clear(map);
}

void
sparsemap_open(sparsemap_t *map, uint8_t *data, size_t size)
{
  map->m_data = data;
  map->m_data_used = __sm_get_size_impl(map);
  map->m_capacity = size;
}

sparsemap_t *
sparsemap_set_data_size(sparsemap_t *map, uint8_t *data, size_t size)
{
  size_t data_size = (size * sizeof(uint8_t));

  /*
   * If this sparsemap was allocated by the sparsemap() API and we're not handed
   * a new data, it's up to us to resize it.
   */
  if (data == NULL && (uintptr_t)map->m_data == (uintptr_t)map + sizeof(sparsemap_t) && size > map->m_capacity) {

    /* Ensure that m_data is 8-byte aligned. */
    size_t total_size = sizeof(sparsemap_t) + data_size;
    size_t padding = total_size % 8 == 0 ? 0 : 8 - (total_size % 8);
    total_size += padding;

    sparsemap_t *m = (sparsemap_t *)realloc(map, total_size);
    if (!m) {
      return NULL;
    }
    memset(((uint8_t *)m) + sizeof(sparsemap_t) + (m->m_capacity * sizeof(uint8_t)), 0, size - m->m_capacity + padding);
    m->m_capacity = data_size;
    m->m_data = (uint8_t *)(((uintptr_t)m + sizeof(sparsemap_t)) & ~(uintptr_t)7);
    __sm_when_diag({ __sm_assert(IS_8_BYTE_ALIGNED(m->m_data)); }) return m;
  } else {
    /*
     * NOTE: It is up to the caller to realloc their buffer and provide it here
     * for reassignment.
     */
    if (data != NULL && data != map->m_data) {
      map->m_data = data;
    }
    map->m_capacity = size;
    return map;
  }
}

double
sparsemap_capacity_remaining(sparsemap_t *map)
{
  if (map->m_data_used >= map->m_capacity) {
    return 0;
  }
  if (map->m_capacity == 0) {
    return 100.0;
  }
  return 100 - (((double)map->m_data_used / (double)map->m_capacity) * 100);
}

size_t
sparsemap_get_capacity(sparsemap_t *map)
{
  return map->m_capacity;
}

bool
sparsemap_is_set(sparsemap_t *map, sparsemap_idx_t idx)
{
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);

  /* Get the __sm_chunk_t which manages this index */
  ssize_t offset = __sm_get_chunk_offset(map, idx);

  /* No __sm_chunk_t's available -> the bit is not set */
  if (offset == -1) {
    return false;
  }

  /* Otherwise load the __sm_chunk_t */
  uint8_t *p = __sm_get_chunk_data(map, offset);
  __sm_idx_t start = *(__sm_idx_t *)p;
  __sm_chunk_t chunk;
  __sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);

  /*
   * Determine if the bit is out of bounds of the __sm_chunk_t; if yes then
   * the bit is not set.
   */
  if (idx < start || (__sm_idx_t)idx - start >= __sm_chunk_get_capacity(&chunk)) {
    return false;
  }

  /* Otherwise ask the __sm_chunk_t whether the bit is set. */
  return __sm_chunk_is_set(&chunk, idx - start);
}

sparsemap_idx_t
__sm_map_unset(sparsemap_t *map, sparsemap_idx_t idx, bool coalesce)
{
  sparsemap_idx_t ret_idx = idx;
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);

  /* Clearing a bit could require an additional vector, let's ensure we have that
   * space available in the buffer first, or ENOMEM now. */
  SM_ENOUGH_SPACE(SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));

  /* Determine if there is a chunk that could contain this index. */
  size_t offset = (size_t)__sm_get_chunk_offset(map, idx);

  if ((ssize_t)offset == -1) {
    /* There are no chunks in the map, there is nothing to clear, this is a
     * no-op. */
    goto done;
  }

  /*
   * Try to locate a chunk for this idx.  We could find that:
   * - the first chunk's offset is greater than the index, or
   * - the index is beyond the end of the last chunk, or
   * - we found a chunk that can contain this index.
   */
  uint8_t *p = __sm_get_chunk_data(map, offset);
  __sm_idx_t start = *(__sm_idx_t *)p;
  __sm_assert(start == __sm_get_chunk_aligned_offset(start));

  if (idx < start) {
    /* Our search resulted in the first chunk that starts after the index but
     * that means there is no chunk that contains this index, so again this is
     * a no-op. */
    goto done;
  }

  __sm_chunk_t chunk;
  __sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
  size_t capacity = __sm_chunk_get_capacity(&chunk);

  if (idx - start >= capacity) {
    /*
     * Our search resulted in a chunk however it's capacity doesn't encompass
     * this index, so again a no-op.
     */
    goto done;
  }

  if (__sm_chunk_is_rle(&chunk)) {
    /*
     * Our search resulted in a chunk that is run-length encoded (RLE).  There
     * are three possibilities at this point: 1) the index is at the end of the
     * run, so we just shorten then length; 2) the index is between start and
     * end [start, end) so we have to split this chunk up; 3) the index is
     * beyond the length but within the capacity, then clearing it is a no-op.
     * If the chunk length shrinks to the max capacity of sparse encoding we
     * have to transition its encoding.
     */

    /* Is the 0-based index beyond the run length? */
    size_t length = __sm_chunk_rle_get_length(&chunk);
    if (idx >= start + length) {
      goto done;
    }

    /* Is the 0-based index referencing the last bit in the run? */
    if (idx - start + 1 == length) {
      /* Should the run-length chunk transition into a sparse chunk? */
      if (length - 1 == SM_CHUNK_MAX_CAPACITY) {
        chunk.m_data[0] = ~(__sm_bitvec_t)0;
      } else {
        __sm_chunk_rle_set_length(&chunk, length - 1);
      }
      goto done;
    }

    /*
     * Now that we've addressed (1) and (3) we have to work on (2) where the
     * index is within the body of this RLE chunk. Chunks must have an aligned
     * starting offset, so let's first find what we'll call the "pivot" chunk
     * wherein we'll find the index we need to clear. That chunk will be sparse.
     */
    __sm_chunk_sep_t sep = { .target = { .p = p, .offset = offset, .chunk = &chunk, .start = start, .length = length, .capacity = capacity } };
    SM_ENOUGH_SPACE(__sm_separate_rle_chunk(map, &sep, idx, 0));
    goto done;
  }

  size_t pos = 0;
  __sm_bitvec_t vec = ~(__sm_bitvec_t)0;
  switch (__sm_chunk_clr_bit(&chunk, idx - start, &pos)) {
  case SM_OK:
    break;
  case SM_NEEDS_TO_GROW:
    offset += (SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t));
    __sm_insert_data(map, offset, (uint8_t *)&vec, sizeof(__sm_bitvec_t));
    __sm_chunk_clr_bit(&chunk, idx - start, &pos);
    break;
  case SM_NEEDS_TO_SHRINK:
    /* The vector is empty, perhaps the entire chunk is empty? */
    if (__sm_chunk_is_empty(&chunk)) {
      __sm_remove_data(map, offset, SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2);
      __sm_set_chunk_count(map, __sm_get_chunk_count(map) - 1);
    } else {
      offset += (SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t));
      __sm_remove_data(map, offset, sizeof(__sm_bitvec_t));
    }
    break;
  default:
    __sm_assert(!"shouldn't be here");
#ifdef DEBUG
    abort();
#endif
    break;
  }

done:;
  if (coalesce && offset != SPARSEMAP_IDX_MAX) {
    __sm_coalesce_chunk(map, &chunk, offset, start, p);
  }
#if 0
  __sm_when_diag({
    char *s = QCC_showSparsemap(map, 0);
    fprintf(stdout, "\n++++++++++++++++++++++++++++++ unset: %lu\n%s\n", idx, s);
    free(s);
  });
#endif
  return ret_idx;
}

sparsemap_idx_t
sparsemap_unset(sparsemap_t *map, sparsemap_idx_t idx)
{
  return __sm_map_unset(map, idx, true);
}

/*
 * When v is non-NULL we've just added a new chunk and we knew in advance that a
 * new chunk will result in a SM_PAYLOAD_MIXED which in turn requires space to
 * store the bit pattern, so given that we allocated the space ahead of time and
 * don't need to allocate it now.
 */
static sparsemap_idx_t
__sparsemap_set(sparsemap_t *map, sparsemap_idx_t idx, uint8_t *p, size_t offset, __sm_bitvec_t *v)
{
  size_t pos = v ? -1 : 0;
  __sm_chunk_t chunk;
  __sm_idx_t start = *(__sm_idx_t *)p;

  __sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
  __sm_assert(__sm_chunk_is_rle(&chunk) == false);

  switch (__sm_chunk_set_bit(&chunk, idx - start, &pos)) {
  case SM_OK:
    break;
  case SM_NEEDS_TO_GROW:
    if (!v) {
      __sm_bitvec_t vec = 0;
      offset += (SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t));
      __sm_insert_data(map, offset, (uint8_t *)&vec, sizeof(__sm_bitvec_t));
      pos = -1;
    }
    __sm_chunk_set_bit(&chunk, idx - start, &pos);
    break;
  case SM_NEEDS_TO_SHRINK:
    /* The vector is empty, perhaps the entire chunk is empty? */
    if (__sm_chunk_is_empty(&chunk)) {
      __sm_remove_data(map, offset, SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2);
      __sm_set_chunk_count(map, __sm_get_chunk_count(map) - 1);
    } else {
      offset += (SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t));
      __sm_remove_data(map, offset, sizeof(__sm_bitvec_t));
    }
    break;
  default:
    __sm_assert(!"shouldn't be here");
#ifdef DEBUG
    abort();
#endif
    break;
  }

  return idx;
}

sparsemap_idx_t
__sm_map_set(sparsemap_t *map, sparsemap_idx_t idx, bool coalesce)
{
  __sm_chunk_t chunk;
  sparsemap_idx_t ret_idx = idx;
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);

  /*
   * Setting a bit could require an additional vector, let's ensure we have that
   * space available in the buffer first, or ENOMEM now.
   */
  SM_ENOUGH_SPACE(SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));

  /* Determine if there is a chunk that could contain this index. */
  size_t offset = (size_t)__sm_get_chunk_offset(map, idx);

  if ((ssize_t)offset == -1) {
    /*
     * No chunks exist, the map is empty, so we must append a new chunk to the
     * end of the buffer and initialize it so that it can contain this index.
     */
    uint8_t buf[SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2] = { 0 };
    __sm_append_data(map, &buf[0], sizeof(buf));
    uint8_t *p = __sm_get_chunk_data(map, 0);
    *(__sm_idx_t *)p = __sm_get_chunk_aligned_offset(idx);
    __sm_set_chunk_count(map, 1);

    __sm_bitvec_t *v = (__sm_bitvec_t *)(uintptr_t)p + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
    ret_idx = __sparsemap_set(map, idx, p, 0, v);

    __sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
    offset = 0;
    goto done;
  }

  /*
   * Try to locate a chunk for this idx.  We could find that:
   *  - the first chunk's offset is greater than the index, or
   *  - the index is beyond the end of the last chunk, or
   *  - we found a chunk that can contain this index.
   */
  uint8_t *p = __sm_get_chunk_data(map, offset);
  __sm_idx_t start = *(__sm_idx_t *)p;
  __sm_assert(start == __sm_get_chunk_aligned_offset(start));

  if (idx < start) {
    /*
     * Our search resulted in the first chunk that starts after the index but
     * that means there is no chunk that can contain this index, so we need to
     * insert a new chunk before this one and initialize it so that it can
     * contain this index.
     */
    uint8_t buf[SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2] = { 0 };
    __sm_insert_data(map, offset, &buf[0], sizeof(buf));
    /* NOTE: insert moves the memory over meaning `p` is now the new chunk */
    *(__sm_idx_t *)p = __sm_get_chunk_aligned_offset(idx);
    __sm_set_chunk_count(map, __sm_get_chunk_count(map) + 1);

    __sm_bitvec_t *v = (__sm_bitvec_t *)(uintptr_t)p + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
    ret_idx = __sparsemap_set(map, idx, p, offset, v);
    goto done;
  }

  __sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
  size_t capacity = __sm_chunk_get_capacity(&chunk);

  if (capacity < SM_CHUNK_MAX_CAPACITY && idx - start < SM_CHUNK_MAX_CAPACITY) {
    /*
     * Special case, we have a sparse chunk with one or more flags set to
     * SM_PAYLOAD_NONE which reduces the carrying capacity of the chunk. In
     * this case we should remove those flags and try again.
     */
    __sm_assert(__sm_chunk_is_rle(&chunk) == false);
    __sm_chunk_increase_capacity(&chunk, SM_CHUNK_MAX_CAPACITY);
    capacity = __sm_chunk_get_capacity(&chunk);
  }

  if (chunk.m_data[0] == ~(__sm_bitvec_t)0 && idx - start == SM_CHUNK_MAX_CAPACITY) {
    /*
     * Our search resulted in a chunk that is full of ones and this index is the
     * next one after the capacity, we have a run of ones longer than the
     * capacity of the sparse encoding, let's transition this chunk to
     * run-length encoding (RLE).
     *
     * NOTE: Keep in mind that idx is 0-based, so idx=2048 is the 2049th bit.
     * When a chunk is at maximum capacity it is storing indexes [0, 2048).
     *
     * ALSO: Keep in mind the RLE "length" is the current length of 1s in the
     * run, so in this case we transition from 2048 to a length of 2049.
     * in this run.
     */

    __sm_chunk_set_rle(&chunk);
    __sm_chunk_rle_set_length(&chunk, SM_CHUNK_MAX_CAPACITY + 1);
    __sm_chunk_rle_set_capacity(&chunk, __sm_chunk_rle_capacity_limit(map, start, offset));
    goto done;
  }

  /* Is this an RLE chunk */
  if (__sm_chunk_is_rle(&chunk)) {
    size_t length = __sm_chunk_rle_get_length(&chunk);

    /* Is the index within its range, or at the end? */
    if (idx >= start && idx - start < capacity) {
      /*
       * This RLE contains the bits in [start, start + length] so the index of
       * the last bit in this RLE chunk is `start + length - 1` which is why
       * we test index (0-based) against current length (1-based) below.
       */
      if ((idx - start) == length) {
        __sm_chunk_rle_set_length(&chunk, length + 1);
        __sm_assert(__sm_chunk_rle_get_length(&chunk) == length + 1);
        goto done;
      }
    }

    /*
     * We've been asked to set a bit that is within this RLE chunk's range but
     * not within its run.  That means this chunk's capacity must shrink, and
     * we need a new sparse chunk to hold this value.
     */
    __sm_chunk_sep_t sep = { .target = { .p = p, .offset = offset, .chunk = &chunk, .start = start, .length = length, .capacity = capacity } };
    SM_ENOUGH_SPACE(__sm_separate_rle_chunk(map, &sep, idx, 1));
    goto done;
  }

  if (idx - start >= capacity) {
    /*
     * Our search resulted in a chunk however it's capacity doesn't encompass
     * this index, so we need to insert a new chunk after this one and
     * initialize it so that it can contain this index.
     */
    uint8_t buf[SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2] = { 0 };
    size_t size = __sm_chunk_get_size(&chunk);
    offset += (SM_SIZEOF_OVERHEAD + size);
    p += SM_SIZEOF_OVERHEAD + size;
    __sm_insert_data(map, offset, &buf[0], sizeof(buf));

    start += __sm_chunk_get_capacity(&chunk);
    if (start + SM_CHUNK_MAX_CAPACITY <= idx) {
      start = __sm_get_chunk_aligned_offset(idx);
    }
    *(__sm_idx_t *)p = start;
    __sm_assert(start == __sm_get_chunk_aligned_offset(start));
    __sm_set_chunk_count(map, __sm_get_chunk_count(map) + 1);

    __sm_bitvec_t *v = (__sm_bitvec_t *)(uintptr_t)p + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
    ret_idx = __sparsemap_set(map, idx, p, offset, v);
    goto done;
  }

  ret_idx = __sparsemap_set(map, idx, p, offset, NULL);
  if (ret_idx != idx) {
    goto done;
  }

done:;
  if (coalesce) {
    __sm_coalesce_chunk(map, &chunk, offset, start, p);
  }
#if 0
  __sm_when_diag({
    char *s = QCC_showSparsemap(map, 0);
    fprintf(stdout, "\n++++++++++++++++++++++++++++++ set: %lu\n%s\n", idx, s);
    free(s);
  });
#endif
  return ret_idx;
}

sparsemap_idx_t
sparsemap_set(sparsemap_t *map, sparsemap_idx_t idx)
{
  return __sm_map_set(map, idx, true);
}

sparsemap_idx_t
sparsemap_assign(sparsemap_t *map, sparsemap_idx_t idx, bool value)
{
  if (value) {
    return sparsemap_set(map, idx);
  } else {
    return sparsemap_unset(map, idx);
  }
}

sparsemap_idx_t
sparsemap_get_starting_offset(sparsemap_t *map)
{
  sparsemap_idx_t offset = 0;
  size_t count = __sm_get_chunk_count(map);
  if (count == 0) {
    return 0;
  }
  uint8_t *p = __sm_get_chunk_data(map, 0);
  sparsemap_idx_t relative_position = *(__sm_idx_t *)p;
  p += SM_SIZEOF_OVERHEAD;
  __sm_chunk_t chunk;
  __sm_chunk_init(&chunk, p);
  if (__sm_chunk_is_rle(&chunk)) {
    offset = relative_position;
    goto done;
  }
  for (size_t m = 0; m < sizeof(__sm_bitvec_t); m++, p++) {
    for (int n = 0; n < SM_FLAGS_PER_INDEX_BYTE; n++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, n);
      if (flags == SM_PAYLOAD_NONE) {
        continue;
      } else if (flags == SM_PAYLOAD_ZEROS) {
        relative_position += SM_BITS_PER_VECTOR;
      } else if (flags == SM_PAYLOAD_ONES) {
        offset = relative_position;
        goto done;
      } else if (flags == SM_PAYLOAD_MIXED) {
        __sm_bitvec_t w = chunk.m_data[1 + __sm_chunk_get_position(&chunk, m * SM_FLAGS_PER_INDEX_BYTE + n)];
        for (int k = 0; k < SM_BITS_PER_VECTOR; k++) {
          if (w & ((__sm_bitvec_t)1 << k)) {
            offset = relative_position + k;
            goto done;
          }
        }
        relative_position += SM_BITS_PER_VECTOR;
      }
    }
  }
done:;
  return offset;
}

sparsemap_idx_t
sparsemap_get_ending_offset(sparsemap_t *map)
{
  sparsemap_idx_t offset = 0;
  size_t count = __sm_get_chunk_count(map);
  if (count == 0) {
    return 0;
  }
  uint8_t *p = __sm_get_chunk_data(map, 0);
  for (size_t i = 0; i < count - 1; i++) {
    p += SM_SIZEOF_OVERHEAD;
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);
    p += __sm_chunk_get_size(&chunk);
  }
  __sm_idx_t start = *(__sm_idx_t *)p;
  p += SM_SIZEOF_OVERHEAD;
  __sm_chunk_t chunk;
  __sm_chunk_init(&chunk, p);
  if (SM_IS_CHUNK_RLE(&chunk)) {
    return start + __sm_chunk_rle_get_length(&chunk) - 1;
  } else {
    sparsemap_idx_t relative_position = start;
    for (size_t m = 0; m < sizeof(__sm_bitvec_t); m++, p++) {
      for (int n = 0; n < SM_FLAGS_PER_INDEX_BYTE; n++) {
        size_t flags = SM_CHUNK_GET_FLAGS(*p, n);
        if (flags == SM_PAYLOAD_NONE) {
          continue;
        } else if (flags == SM_PAYLOAD_ZEROS) {
          relative_position += SM_BITS_PER_VECTOR;
        } else if (flags == SM_PAYLOAD_ONES) {
          relative_position += SM_BITS_PER_VECTOR;
          offset = relative_position;
        } else if (flags == SM_PAYLOAD_MIXED) {
          __sm_bitvec_t w = chunk.m_data[1 + __sm_chunk_get_position(&chunk, m * SM_FLAGS_PER_INDEX_BYTE + n)];
          int idx = 0;
          for (int k = 0; k < SM_BITS_PER_VECTOR; k++) {
            if (w & ((__sm_bitvec_t)1 << k)) {
              idx = k;
            }
          }
          offset = relative_position + idx;
          relative_position += SM_BITS_PER_VECTOR;
        }
      }
    }
    return offset;
  }
}

double
sparsemap_fill_factor(sparsemap_t *map)
{
  size_t rank = sparsemap_rank(map, 0, SPARSEMAP_IDX_MAX, true);
  sparsemap_idx_t end = sparsemap_get_ending_offset(map);
  return (double)rank / (double)end * 100.0;
}

void *
sparsemap_get_data(sparsemap_t *map)
{
  return map->m_data;
}

size_t
sparsemap_get_size(sparsemap_t *map)
{
  if (map->m_data_used) {
    size_t size = __sm_get_size_impl(map);
    if (size != map->m_data_used) {
      map->m_data_used = size;
    }
    __sm_when_diag({ __sm_assert(map->m_data_used == __sm_get_size_impl(map)); });
    return map->m_data_used;
  }
  return map->m_data_used = __sm_get_size_impl(map);
}

size_t
sparsemap_count(sparsemap_t *map)
{
  return sparsemap_rank(map, 0, SPARSEMAP_IDX_MAX, true);
}

void
sparsemap_scan(sparsemap_t *map, void (*scanner)(__sm_idx_t[], size_t, void *aux), size_t skip, void *aux)
{
  uint8_t *p = __sm_get_chunk_data(map, 0);
  size_t count = __sm_get_chunk_count(map);

  for (size_t i = 0; i < count; i++) {
    __sm_idx_t start = *(__sm_idx_t *)p;
    p += SM_SIZEOF_OVERHEAD;
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);
    size_t skipped = __sm_chunk_scan(&chunk, start, scanner, skip, aux);
    if (skip) {
      __sm_assert(skip >= skipped);
      skip -= skipped;
    }
    p += __sm_chunk_get_size(&chunk);
  }
}

int
sparsemap_merge(sparsemap_t *destination, sparsemap_t *source)
{
  uint8_t *src, *dst;
  size_t src_count = __sm_get_chunk_count(source);
  sparsemap_idx_t dst_ending_offset = sparsemap_get_ending_offset(destination);

  if (src_count == 0) {
    return 0;
  }

  // TODO: rethink this method of estimating space... seems off to me now...
  ssize_t remaining_capacity = destination->m_capacity - destination->m_data_used -
    (source->m_data_used + src_count * (SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2));

  /* Estimate worst-case overhead required for merge. */
  if (remaining_capacity <= 0) {
    errno = ENOSPC;
    return -remaining_capacity;
  }

  /*
   * Strategy here it to walk the ordered set of chunks in the source map
   * examining each one against the current destination chunk.  This is similar
   * to a merge sort with two cursors, one in src and one in dst.  Then there
   * are a number of cases to consider:
   * - src proceeds dst
   * - src follows dst
   * - src and dst overlap
   *   - perfect overlap
   *   - non-uniform overlap
   */
  src = __sm_get_chunk_data(source, 0);
  while (src_count) {
    __sm_idx_t src_start = *(__sm_idx_t *)src;
    __sm_chunk_t src_chunk;
    __sm_chunk_init(&src_chunk, src + SM_SIZEOF_OVERHEAD);
    bool src_is_rle = SM_IS_CHUNK_RLE(&src_chunk);
    size_t src_capacity = __sm_chunk_get_capacity(&src_chunk);
    ssize_t dst_offset = __sm_get_chunk_offset(destination, src_start);
    if (dst_offset >= 0) {
      dst = __sm_get_chunk_data(destination, dst_offset);
      __sm_idx_t dst_start = *(__sm_idx_t *)dst;
      __sm_chunk_t dst_chunk;
      __sm_chunk_init(&dst_chunk, dst + SM_SIZEOF_OVERHEAD);
      bool dst_is_rle = SM_IS_CHUNK_RLE(&dst_chunk);
      size_t dst_capacity = __sm_chunk_get_capacity(&dst_chunk);

      /* Try to expand the capacity if there's room before the start of the next chunk. */
      if (!(src_is_rle || dst_is_rle)) {
        if (src_start == dst_start && dst_capacity < src_capacity) {
          ssize_t nxt_offset = __sm_get_chunk_offset(destination, dst_start + dst_capacity + 1);
          uint8_t *nxt_dst = __sm_get_chunk_data(destination, nxt_offset);
          __sm_idx_t nxt_dst_start = *(__sm_idx_t *)nxt_dst;
          if (nxt_dst_start > dst_start + src_capacity) {
            __sm_chunk_increase_capacity(&dst_chunk, src_capacity);
            dst_capacity = __sm_chunk_get_capacity(&dst_chunk);
          }
        }
      }

      /* Source chunk (sparse/RLE) precedes next destination chunk. */
      if ((src_start + src_capacity) <= dst_start) {
        size_t src_size = __sm_chunk_get_size(&src_chunk);
        ssize_t offset = __sm_get_chunk_offset(destination, dst_start);
        /* Insert a copy of the src chunk in dst at the proper offset. */
        __sm_insert_data(destination, offset, src, SM_SIZEOF_OVERHEAD + src_size);
        /* Update the chunk count in dst. */
        __sm_set_chunk_count(destination, __sm_get_chunk_count(destination) + 1);

        /* Move to the next src chunk. */
        src_count--;
        src += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&src_chunk);
        continue;
      }

      /* Source chunk (sparse/RLE) follows next destination chunk. */
      if (src_start >= (dst_start + dst_capacity)) {
        size_t src_size = __sm_chunk_get_size(&src_chunk);
        /* Insert or append a copy of the src chunk in dst. */
        if (dst_offset == __sm_get_chunk_offset(destination, SPARSEMAP_IDX_MAX)) {
          __sm_append_data(destination, src, SM_SIZEOF_OVERHEAD + src_size);
        } else {
          ssize_t offset = __sm_get_chunk_offset(destination, src_start);
          __sm_insert_data(destination, offset, src, SM_SIZEOF_OVERHEAD + src_size);
        }
        /* Update the chunk count and data_used. */
        __sm_set_chunk_count(destination, __sm_get_chunk_count(destination) + 1);

        /* Move to the next src chunk. */
        src_count--;
        src += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&src_chunk);
        continue;
      }

      /* At this point we know that the dst chunk and src chunk overlap. */
      size_t src_length = __sm_chunk_rle_get_length(&src_chunk);
      size_t dst_length = __sm_chunk_rle_get_length(&dst_chunk);
      size_t src_capacity = __sm_chunk_get_capacity(&src_chunk);

      if (src_is_rle && dst_is_rle) {
        /* Both src and dst are RLE ... */
        __sm_chunk_rle_set_capacity(&dst_chunk, __sm_chunk_rle_capacity_limit(destination, src_start, dst_offset));
        if (src_length >= dst_length) {
          /* ... and src is larger than dst. */
          __sm_chunk_rle_set_length(&dst_chunk, __sm_chunk_rle_get_length(&src_chunk));
        }
        if (src_start <= dst_start) {
          /* ... and src starts before dst. */
          *(__sm_idx_t *)dst = src_start;
        }

        /* Move to the next src chunk. */
        src_count--;
        src += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&src_chunk);
        continue;
      }

      /* Source and destination start at the same point. */
      if (src_start == dst_start && src_capacity == dst_capacity) {
        /* Source and destination and a perfect overlapping non-RLE pair. */
        __sm_merge_chunk(destination, src_start, dst_start, dst_capacity, &dst_chunk, &src_chunk);

        /* Move to the next src chunk. */
        src_count--;
        src += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&src_chunk);
        continue;
      }

      /* Non-uniform overlapping chunks. */
      if (dst_start < src_start || (dst_start == src_start && dst_capacity != src_capacity)) {
        size_t src_end = src_start + src_capacity;
        size_t dst_end = dst_start + dst_capacity;
        size_t overlap = src_end > dst_end ? src_capacity - (src_end - dst_end) : src_capacity;
        __sm_merge_chunk(destination, src_start, dst_start, overlap, &dst_chunk, &src_chunk);
        for (size_t n = src_start + overlap; n <= src_end; n++) {
          if (sparsemap_is_set(source, n)) {
            sparsemap_set(destination, n);
          }
        }

        /* Move to the next src chunk. */
        src_count--;
        src += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&src_chunk);
        continue;
      }

      abort();
    } else {
      /* A negative destination offset indicates an empty map. */

      if (src_start >= dst_ending_offset) {
        /* Starting offset is after destination chunks, so append data. */
        size_t src_size = __sm_chunk_get_size(&src_chunk);
        __sm_append_data(destination, src, SM_SIZEOF_OVERHEAD + src_size);

        /* Update the chunk count and data_used. */
        __sm_set_chunk_count(destination, __sm_get_chunk_count(destination) + 1);

        /* Move to the next src chunk. */
        src_count--;
        src += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&src_chunk);
        continue;
      } else {
        /* Source chunk precedes next destination chunk. */
        size_t src_size = __sm_chunk_get_size(&src_chunk);
        ssize_t offset = __sm_get_chunk_offset(destination, src_start);
        __sm_insert_data(destination, offset, src, SM_SIZEOF_OVERHEAD + src_size);

        /* Update the chunk count and data_used. */
        __sm_set_chunk_count(destination, __sm_get_chunk_count(destination) + 1);

        /* Move to the next src chunk. */
        src_count--;
        src += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&src_chunk);
        continue;
      }
    }
  }

  __sm_coalesce_map(destination);
  return 0;
}

sparsemap_idx_t
sparsemap_split(sparsemap_t *map, sparsemap_idx_t idx, sparsemap_t *other)
{
  uint8_t *src, *dst, *prev;
  int moved = 0;
  size_t i, count = __sm_get_chunk_count(map);
  bool in_middle = false;

  __sm_assert(sparsemap_count(other) == 0);

  __sm_when_diag({ __sm_diag_map(map, "========== START: %lu", idx); });

  /*
   * According to the API when idx is SPARSEMAP_IDX_MAX the client is
   * requesting that we divide the bits in two equal portions, so we
   * calculate that index here.
   */
  if (idx == SPARSEMAP_IDX_MAX) {
    sparsemap_idx_t begin = sparsemap_get_starting_offset(map);
    sparsemap_idx_t end = sparsemap_get_ending_offset(map);
    if (begin != end) {
      size_t rank = sparsemap_rank(map, begin, end, true);
      idx = sparsemap_select(map, rank / 2, true);
    } else {
      return SPARSEMAP_IDX_MAX;
    }
  }

  /* Is the index beyond the last bit set in the source? */
  if (idx >= sparsemap_get_ending_offset(map)) {
    return idx;
  }

  /*
   * Here's how this is going to work, there are three phases.
   * 1) Skip over any chunks before the idx.
   * 2) If the idx falls within a chunk, ...
   *  2a) If that chunk is RLE, separate the RLE into two or three chunks
   *  2b) Recursively call sparsemap_split() because now we have a sparse chunk
   * 3) Split the sparse chunk
   * 4) Keep half in the src and insert the other half into the dst
   * 5) Move any remaining chunks to dst.
   */
  src = __sm_get_chunk_data(map, 0);
  dst = __sm_get_chunk_end(other);

  /* (1): skip over chunks that are entirely to the left. */
  prev = src;
  for (i = 0; i < count; i++) {
    __sm_idx_t start = *(__sm_idx_t *)src;
    if (start == idx) {
      break;
    }
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, src + SM_SIZEOF_OVERHEAD);
    if (start + __sm_chunk_get_capacity(&chunk) > idx) {
      in_middle = true;
      break;
    }
    if (start > idx) {
      src = prev;
      i--;
      break;
    }

    prev = src;
    src += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
  }

  /* (2): The idx falls within a chunk then it has to be split. */
  if (in_middle) {
    __sm_chunk_t s_chunk, d_chunk;
    __sm_chunk_init(&s_chunk, src + SM_SIZEOF_OVERHEAD);
    __sm_chunk_init(&d_chunk, dst + SM_SIZEOF_OVERHEAD);
    __sm_idx_t src_start = *(__sm_idx_t *)src;

    /* (2a) Does the idx fall within the range of an RLE chunk? */
    if (SM_IS_CHUNK_RLE(&s_chunk)) {
      /*
       * There is a function that can split an RLE chunk at an index, but to use
       * it and not mutate anything we'll need to jump through a few hoops.
       * To perform this trick we need to first need a new static buffer
       * that we can use with a new "stunt" map. Once we have the chunk we need
       * to split in that new buffer wrapped into a new map we can call our API
       * that separates the RLE chunk at the index.
       */

      sparsemap_t stunt;
      __sm_chunk_t chunk;
      uint8_t buf[(SM_SIZEOF_OVERHEAD * 3) + (sizeof(__sm_bitvec_t) * 6)] = { 0 };

      /* Copy the source chunk into the buffer. */
      memcpy(buf + SM_SIZEOF_OVERHEAD, src, SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
      /* Set the number of chunks to 1 in our stunt map. */
      buf[0] = (uint32_t)1;
      /* And initialize the stunt double chunk we need to split. */
      sparsemap_open(&stunt, buf, (SM_SIZEOF_OVERHEAD * 3) + (sizeof(__sm_bitvec_t) * 6));
      __sm_chunk_init(&chunk, buf + SM_SIZEOF_OVERHEAD);

      /* Finally, let's separate the RLE chunk at index. */
      __sm_chunk_sep_t sep = { .target = { .p = buf + SM_SIZEOF_OVERHEAD,
                                 .offset = 0,
                                 .chunk = &chunk,
                                 .start = src_start,
                                 .length = __sm_chunk_rle_get_length(&s_chunk),
                                 .capacity = __sm_chunk_get_capacity(&s_chunk) } };
      __sm_separate_rle_chunk(&stunt, &sep, idx, -1);

      /*
       * (2b) Assuming we have the space we'll update the source map with the
       * separate, but equivalent chunks and then recurse confident that next time
       * our index will fall inside a sparse chunk (that we just made).
       */
      SM_ENOUGH_SPACE(sep.expand_by);
      __sm_insert_data(map, __sm_get_chunk_offset(map, idx) + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t), sep.buf + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t),
        sep.expand_by);
      memcpy(src, sep.buf, sep.expand_by + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
      __sm_set_chunk_count(map, __sm_get_chunk_count(map) + (sep.count - 1));

      __sm_when_diag({ __sm_diag_map(map, "========== PREPARED:"); });
      return sparsemap_split(map, idx, other);
    }

    /*
     * (3) We're in the middle of a sparse chunk, let's split it.
     */

    /* Zero out the space we'll need at the proper location in dst. */
    uint8_t buf[SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2] = { 0 };
    memcpy(dst, &buf, sizeof(buf));

    /* And add a chunk to the other map. */
    __sm_set_chunk_count(other, __sm_get_chunk_count(other) + 1);
    if (other->m_data_used != 0) {
      other->m_data_used += SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
    }

    /* Copy the bits in the sparse chunk, at most SM_CHUNK_MAX_CAPACITY. */
    *(__sm_idx_t *)dst = src_start;
    for (size_t j = idx; j < src_start + SM_CHUNK_MAX_CAPACITY; j++) {
      if (sparsemap_is_set(map, j)) {
        __sm_map_set(other, j, false);
        __sm_map_unset(map, j, false);
      }
    }
    src += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&s_chunk);
    dst += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&d_chunk);
    i++;
  }

  /* Now continue with all remaining chunks. */
  for (; i < count; i++) {
    __sm_idx_t start = *(__sm_idx_t *)src;
    src += SM_SIZEOF_OVERHEAD;
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, src);
    size_t s = __sm_chunk_get_size(&chunk);

    *(__sm_idx_t *)dst = start;
    dst += SM_SIZEOF_OVERHEAD;
    memcpy(dst, src, s);
    src += s;
    dst += s;

    moved++;
  }

  /* Force new calculation. */
  other->m_data_used = 0;
  map->m_data_used = 0;

  /* Update the Chunk counters. */
  __sm_set_chunk_count(map, __sm_get_chunk_count(map) - moved);
  __sm_set_chunk_count(other, __sm_get_chunk_count(other) + moved);

  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);
  __sm_assert(sparsemap_get_size(other) > SM_SIZEOF_OVERHEAD);

  __sm_coalesce_map(map);
  __sm_coalesce_map(other);

  __sm_when_diag({
    __sm_diag_map(map, "SRC");
    __sm_diag_map(other, "DST");
  });

  return idx;
}

sparsemap_idx_t
sparsemap_select(sparsemap_t *map, sparsemap_idx_t n, bool value)
{
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);
  __sm_idx_t start;
  size_t count = __sm_get_chunk_count(map);

  if (count == 0 && value == false) {
    return n;
  }

  uint8_t *p = __sm_get_chunk_data(map, 0);

  for (size_t i = 0; i < count; i++) {
    start = *(__sm_idx_t *)p;
    /* Start of this chunk is greater than n meaning there are a set of 0s
     * before the first 1 sufficient to consume n. */
    if (value == false && i == 0 && start > n) {
      return n;
    }
    p += SM_SIZEOF_OVERHEAD;
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);

    ssize_t new_n = n;
    size_t index = __sm_chunk_select(&chunk, n, &new_n, value);
    if (new_n == -1) {
      return start + index;
    }
    n = new_n;

    p += __sm_chunk_get_size(&chunk);
  }
  return SPARSEMAP_IDX_MAX;
}

static size_t
__sm_rank_vec(sparsemap_t *map, size_t begin, size_t end, bool value, __sm_bitvec_t *vec)
{
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);
  size_t amt, gap, pos = 0, result = 0, prev = 0, count, len = end - begin + 1;
  uint8_t *p;

  if (begin > end) {
    return 0;
  }

  if (begin == end) {
    return sparsemap_is_set(map, begin) == value ? 1 : 0;
  }

  count = __sm_get_chunk_count(map);

  if (count == 0) {
    if (value == false) {
      /* The count/rank of unset bits in an empty map is inf, so what you requested is the answer. */
      return len;
    }
  }

  p = __sm_get_chunk_data(map, 0);
  for (size_t i = 0; i < count; i++) {
    __sm_idx_t start = *(__sm_idx_t *)p;
    /* [prev, start + pos), prev is the last bit examined 0-based. */
    if (i == 0) {
      gap = start;
    } else {
      if (prev + SM_CHUNK_MAX_CAPACITY == start) {
        gap = 0;
      } else {
        gap = start - (prev + pos);
      }
    }
    /* Start of this chunk is greater than the end of the desired range. */
    if (start > end) {
      if (value == true) {
        /* We're counting set bits and this chunk starts after the range
         * [begin, end], we're done. */
        return result;
      } else {
        if (i == 0) {
          /* We're counting unset bits and the first chunk starts after the
           * range meaning everything proceeding this chunk was zero and should
           * be counted, also we're done. */
          result += (end - begin) + 1;
          return result;
        } else {
          /* We're counting unset bits and some chunk starts after the range, so
           * we've counted enough, we're done. */
          if (pos > end) {
            return result;
          } else {
            if (end - pos < gap) {
              result += end - pos;
              return result;
            } else {
              result += gap;
              return result;
            }
          }
        }
      }
    } else {
      /* The range and this chunk overlap. */
      if (value == false) {
        if (begin > gap) {
          begin -= gap;
        } else {
          result += gap - begin;
          begin = 0;
        }
      } else {
        if (begin >= gap) {
          begin -= gap;
        }
      }
    }
    prev = start;
    p += SM_SIZEOF_OVERHEAD;
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);

    /* Count all the set/unset inside this chunk within the range. */
    __sm_chunk_rank_t rank;
    amt = __sm_chunk_rank(&rank, value, &chunk, begin, end - start);
    result += amt;
    pos = rank.pos;
    begin = rank.pos > begin ? 0 : begin - rank.pos;
    // vec = rank.rem;
    p += __sm_chunk_get_size(&chunk);
  }
  /* Count any additional unset bits that fall outside the last chunk but
   * within the range. */
  if (value == false) {
    size_t last = prev - 1 + pos;
    if (end > last) {
      result += end - last - begin;
    }
  }
  return result;
}

size_t
sparsemap_rank(sparsemap_t *map, sparsemap_idx_t begin, sparsemap_idx_t end, bool value)
{
  __sm_bitvec_t vec;
  return __sm_rank_vec(map, begin, end, value, &vec);
}

size_t
sparsemap_span(sparsemap_t *map, sparsemap_idx_t idx, size_t len, bool value)
{
  size_t rank, nth;
  __sm_bitvec_t vec = 0;
  sparsemap_idx_t offset;

  /* When skipping forward to `idx` offset in the map we can determine how
   * many selects we can avoid by taking the rank of the range and starting
   * at that bit. */
  nth = (idx == 0) ? 0 : sparsemap_rank(map, 0, idx - 1, value);
  if (SPARSEMAP_NOT_FOUND(nth)) {
    return nth;
  }
  /* Find the first bit that matches value, then... */
  offset = sparsemap_select(map, nth, value);
  do {
    /* See if the rank of the bits in the range starting at offset is equal
     * to the desired amount. */
    rank = (len == 1) ? 1 : __sm_rank_vec(map, offset, offset + len - 1, value, &vec);
    if (rank >= len) {
      /* We've found what we're looking for, return the index of the first
       * bit in the range. */
      break;
    }
    /* Now we try to jump forward as much as possible before we look for a
     * new match. We do this by counting the remaining bits in the returned
     * vec from the call to rank_vec(). */
    int amt = 1;
    if (vec > 0) {
      /* The returned vec had some set bits, let's move forward in the map as
       * much as possible (max: 64 bit positions). */
      int max = len > SM_BITS_PER_VECTOR ? SM_BITS_PER_VECTOR : len;
      while (amt < max && (vec & 1 << amt)) {
        amt++;
      }
    }
    nth += amt;
    offset = sparsemap_select(map, nth, value);
  } while (SPARSEMAP_FOUND(offset));

  return offset;
}

#ifdef SPARSEMAP_TESTING

#include <qc.h>

static double
_tst_pow(double base, int exponent)
{
  if (exponent == 0) {
    return 1.0; // 0^0 is 1
  } else if (base == 0.0) {
    return 0.0; // 0 raised to any positive exponent is 0 (except 0^0)
  } else if (base < 0.0 && (exponent & 1) != 0) {
    // negative base with odd exponent, results in a negative
    return -_tst_pow(-base, exponent);
  }

  double result = base;
  for (unsigned int i = 1; i < exponent; i++) {
    result *= base;
  }
  return result;
}

static char *
_qcc_format_chunk(__sm_idx_t start, __sm_chunk_t *chunk, bool none)
{
  size_t amt = sizeof(wchar_t) * ((SM_FLAGS_PER_INDEX * 16) + (SM_BITS_PER_VECTOR * 64) + 16) * 2;
  char *buf = (char *)malloc(sizeof(wchar_t) * ((SM_FLAGS_PER_INDEX * 16) + (SM_BITS_PER_VECTOR * 64) + 16) * 2);

  __sm_bitvec_t desc = chunk->m_data[0];

  if (!__sm_chunk_is_rle(chunk)) {
    char desc_str[((2 * SM_FLAGS_PER_INDEX) + 1) * sizeof(wchar_t)] = { 0 };
    char *str = desc_str;
    int mixed = 0;
    // for (int i = SM_FLAGS_PER_INDEX - 1; i >= 0; i--) {
    for (int i = 1; i <= SM_FLAGS_PER_INDEX; i++) {
      uint8_t flag = SM_CHUNK_GET_FLAGS(desc, i);
      switch (flag) {
      case SM_PAYLOAD_NONE:
        if (!none)
          __sm_assert(flag == SM_PAYLOAD_NONE);
        str += sprintf(str, "_"); // ∘
        break;
      case SM_PAYLOAD_ONES:
        str += sprintf(str, "1");
        break;
      case SM_PAYLOAD_ZEROS:
        str += sprintf(str, "0");
        break;
      case SM_PAYLOAD_MIXED:
        str += sprintf(str, "X"); // ①
        mixed++;
        break;
      }
      if (i % 8 == 0 && i < 32)
        str += sprintf(str, " ");
    }
    str = buf + sprintf(buf, "%.10u\t|%s|%s", start, desc_str, mixed ? " :: " : "");
    for (int i = 0; i < mixed; i++) {
      // str += sprintf(str, "0x%0lX%s", chunk->m_data[1 + i], i + 1 < mixed ? " " : "");
      size_t n = snprintf(str, amt - 1, "%#018" PRIx64 "%s", chunk->m_data[1 + i], i + 1 < mixed ? " " : "");
      str += n;
      amt -= n;
    }
  } else {
    // sprintf(buf, "%.10u\t1»%zu of %zu", start, __sm_chunk_rle_get_length(chunk), __sm_chunk_rle_get_capacity(chunk));
    size_t len = __sm_chunk_rle_get_length(chunk);
    size_t cap = __sm_chunk_rle_get_capacity(chunk);
    sprintf(buf, "%.10u\t[%u, %zu) %zu of %zu", start, start, start + len - 1, len, cap);
  }
  return buf;
}

char *
QCC_showChunk(void *value, int len)
{
  __sm_idx_t start = *(__sm_idx_t *)value;
  __sm_chunk_t chunk;
  __sm_chunk_init(&chunk, value + SM_SIZEOF_OVERHEAD);

  return _qcc_format_chunk(start, &chunk, false);
}

char *
QCC_showSparsemap(void *value, int len)
{
  sparsemap_t *map = (sparsemap_t *)value;
  size_t off = 0, count = __sm_get_chunk_count(map);
  char *str, *buf = NULL;

  if (count > 0) {
    uint8_t *s, *p = __sm_get_chunk_data(map, 0);
    for (size_t i = 0; i < count; i++) {
      __sm_chunk_t chunk;
      __sm_idx_t start = *(__sm_idx_t *)p;
      __sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
      char *c = _qcc_format_chunk(start, &chunk, true);
      if (buf) {
        char *new = realloc(buf, strlen(buf) + strlen(c) + 24);
        if (new) {
          buf = new;
          str += sprintf(str, "\n%s", c);
        }
      } else {
        buf = c;
        str = buf + strlen(c);
      }
      p += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
      off = p - s;
    }
  }

  return buf;
}

static void
QCC_freeChunkValue(void *value)
{
  free(value);
}

static void
QCC_freeSparsemapValue(void *value)
{
  free(value);
}

QCC_GenValue *
QCC_genChunk()
{
  if (((double)random() / (double)RAND_MAX) > 0.5) {
    // Generate a run-length encoded (RLE) chunk:
    sparsemap_idx_t from = 1, to = SM_CHUNK_RLE_MAX_LENGTH;
    unsigned int len = ((unsigned int)random() % (to - from)) + from;
    // First allocate enough room for the chunk data ...
    uint8_t *p = malloc(SM_SIZEOF_OVERHEAD + sizeof(__sm_chunk_t) + (sizeof(__sm_bitvec_t) * 2));
    __sm_chunk_t *chunk;
    // ... then set the offset to the length so we can test for that later ...
    *(__sm_idx_t *)p = len;
    // ... next is the chunk begins after the offset ...
    chunk = (__sm_chunk_t *)((uintptr_t)p + SM_SIZEOF_OVERHEAD);
    // ... this contains a single vector ...
    chunk->m_data = (__sm_bitvec_t *)((uintptr_t)chunk + sizeof(__sm_chunk_t));
    chunk->m_data[0] = 0;
    // ... set the flags on this vector to indicate that is it RLE ...
    __sm_chunk_set_rle(chunk);
    // ... set the RLE chunk's initial capacity ...
    __sm_chunk_rle_set_capacity(chunk, SM_CHUNK_RLE_MAX_CAPACITY);
    // ... and set the RLE chunk's length of 1s to len.
    __sm_chunk_rle_set_length(chunk, len);
    // Now, test what we've generated to ensure it's correct.
    __sm_assert(*(__sm_idx_t *)p == len);
    __sm_assert(__sm_chunk_is_rle(chunk));
    __sm_assert(__sm_chunk_rle_get_capacity(chunk) == SM_CHUNK_RLE_MAX_CAPACITY);
    __sm_assert(__sm_chunk_rle_get_length(chunk) == len);
    return QCC_initGenValue(p, 1, QCC_showChunk, QCC_freeChunkValue);
  } else {
    // Generate a chunk with the offset equal to the number of additional
    // vectors (len) and a descriptor that matches that with random data.
    unsigned int from = 0, to = SM_FLAGS_PER_INDEX;
    unsigned int len = ((unsigned int)random() % (to - from)) + from;
    unsigned int cut = ((unsigned int)random() % ((SM_FLAGS_PER_INDEX - len) - from)) + from;
    // First allocate enough room for the chunk data ...
    uint8_t *p = malloc(SM_SIZEOF_OVERHEAD + sizeof(__sm_chunk_t) + (sizeof(__sm_bitvec_t) * (len + 1)));
    __sm_chunk_t *chunk;
    // ... then set the offset to the capacity ...
    *(__sm_idx_t *)p = SM_CHUNK_MAX_CAPACITY - (cut * SM_BITS_PER_VECTOR);
    // ... next is the chunk begins after the offset ...
    chunk = (__sm_chunk_t *)((uintptr_t)p + SM_SIZEOF_OVERHEAD);
    // ... this contains a len + 1 vectors ...
    chunk->m_data = (__sm_bitvec_t *)((uintptr_t)chunk + sizeof(__sm_chunk_t));
    // ... the first is the descriptor with the flags ...
    __sm_bitvec_t *desc = chunk->m_data;
    *desc = 0;
    // ... ensure that exactly `len` flags are set to SM_PAYLOAD_MIXED ...
    for (size_t i = 0; i < len; i++) {
      SM_CHUNK_SET_FLAGS(*desc, i, SM_PAYLOAD_MIXED);
      chunk->m_data[1 + i] = (uintptr_t)chunk + i;
    }
    // ... and, on average, 50% of the rest are SM_PAYLOAD_ONES ...
    for (size_t i = len; i < SM_FLAGS_PER_INDEX - cut; i++) {
      double coin = (double)random() / (double)RAND_MAX;
      if (SM_CHUNK_GET_FLAGS(*desc, i) != SM_PAYLOAD_MIXED && coin >= 0.5) {
        SM_CHUNK_SET_FLAGS(*desc, i, SM_PAYLOAD_ONES);
      }
    }
    // ... shuffle those around ...
    for (size_t i = 0; i < SM_FLAGS_PER_INDEX - cut - 1; i++) {
      size_t j = ((size_t)random() % ((SM_FLAGS_PER_INDEX - cut) - i)) + i;
      int flags = SM_CHUNK_GET_FLAGS(*desc, j);
      SM_CHUNK_SET_FLAGS(*desc, j, SM_CHUNK_GET_FLAGS(*desc, i));
      SM_CHUNK_SET_FLAGS(*desc, i, flags);
    }
    // ... reduce the capacity by setting trailing flags to SM_PAYLOAD_NONE ...
    *desc <<= (cut * 2);
    for (int i = 0; i < cut; i++) {
      SM_CHUNK_SET_FLAGS(*desc, i, SM_PAYLOAD_NONE);
    }
    // fprintf(stdout, "\n%s\n", QCC_showChunk(p, 0));
    // ... and check that our franken-chunk appears to be correct.
    __sm_assert(__sm_chunk_is_rle(chunk) == false);
    return QCC_initGenValue(p, 1, QCC_showChunk, QCC_freeChunkValue);
  }
}

extern void populate_map(sparsemap_t *map, int size, int max_value);

QCC_GenValue *
QCC_genSparsemap()
{
  sparsemap_t *map = sparsemap(1024);
  return QCC_initGenValue(map, 1, QCC_showSparsemap, QCC_freeSparsemapValue);
}

static size_t
_tst_sm_chunk_calc_vector_size(uint8_t b)
{
  int count = 0;

  for (int i = 0; i < 4; i++) {
    if (((b >> (i * 2)) & 0x03) == 0x02) {
      count++;
    }
  }

  return count;
}

QCC_TestStatus
_tst_chunk_calc_vector_size_equality(QCC_GenValue **vals, int len, QCC_Stamp **stamp)
{
  unsigned int a = *QCC_getValue(vals, 0, unsigned int *) % 256;
  if (_tst_sm_chunk_calc_vector_size(a) != __sm_chunk_calc_vector_size(a)) {
    return QCC_FAIL;
  }
  return QCC_OK;
}

QCC_TestStatus
_tst_chunk_get_position(QCC_GenValue **vals, int len, QCC_Stamp **stamp)
{
  uint8_t *p = (uint8_t *)QCC_getValue(vals, 0, void *);
  __sm_idx_t start = *(__sm_idx_t *)p;
  __sm_chunk_t *chunk = (__sm_chunk_t *)((uintptr_t)p + SM_SIZEOF_OVERHEAD);
  size_t pos;

  if (__sm_chunk_is_rle(chunk)) {
    for (size_t i = 0; i < SM_FLAGS_PER_INDEX; i++) {
      pos = __sm_chunk_get_position(chunk, i);
      if (pos != 0) {
        return QCC_FAIL;
      }
    }
  } else {
    size_t mixed = 0;
    for (size_t i = 0; i < SM_FLAGS_PER_INDEX; i++) {
      uint8_t flag = SM_CHUNK_GET_FLAGS(*chunk->m_data, i);
      switch (flag) {
      case SM_PAYLOAD_MIXED:
        pos = __sm_chunk_get_position(chunk, i);
        if (chunk->m_data[1 + pos] != (uintptr_t)chunk + pos) {
          return QCC_FAIL;
        }
        mixed++;
        break;
      case SM_PAYLOAD_ONES:
      case SM_PAYLOAD_ZEROS:
        pos = __sm_chunk_get_position(chunk, i);
        if (pos != mixed) {
          return QCC_FAIL;
        }
        break;
      case SM_PAYLOAD_NONE:
      default:
        break;
      }
    }
  }
  return QCC_OK;
}

QCC_TestStatus
_tst_chunk_get_capacity(QCC_GenValue **vals, int len, QCC_Stamp **stamp)
{
  uint8_t *p = (uint8_t *)QCC_getValue(vals, 0, void *);
  __sm_idx_t start = *(__sm_idx_t *)p;
  __sm_chunk_t *chunk = (__sm_chunk_t *)((uintptr_t)p + SM_SIZEOF_OVERHEAD);

  if (__sm_chunk_is_rle(chunk)) {
    if (__sm_chunk_rle_get_length(chunk) != start) {
      return QCC_FAIL;
    }
  } else {
    if (__sm_chunk_get_capacity(chunk) != start) {
      return QCC_FAIL;
    }
  }
  return QCC_OK;
}

QCC_TestStatus
_tst_get_chunk_offset(QCC_GenValue **vals, int len, QCC_Stamp **stamp)
{
  unsigned int idx = *QCC_getValue(vals, 0, unsigned int *);
  sparsemap_t *map = QCC_getValue(vals, 1, sparsemap_t *);
  unsigned int max_offset = ((SM_FLAGS_PER_INDEX - 1) * sizeof(__sm_bitvec_t));
  unsigned int rnd_offset = (idx % max_offset) - ((idx % max_offset) % sizeof(__sm_bitvec_t));
  unsigned int rnd_nvec = rnd_offset / sizeof(__sm_bitvec_t);
  __sm_idx_t offset = __sm_get_chunk_aligned_offset(idx);

  // An empty map should return -1 (no chunks present, so offset of -1).
  for (unsigned int i = offset; i < SM_CHUNK_MAX_CAPACITY + offset; i++) {
    if (__sm_get_chunk_offset(map, idx) != -1) {
      return QCC_FAIL;
    }
  }

  // By setting the first bit in each of rnd_nvec chunks we create one chunk
  // per and with exactly one additional bitvec per so we should observe...
  for (int i = 0; i < rnd_nvec; i++) {
    sparsemap_idx_t l = offset + (i * SM_CHUNK_MAX_CAPACITY);
    sparsemap_set(map, l);
  }
  for (int i = 0; i < rnd_nvec; i++) {
    size_t expected_offset = __sm_get_chunk_offset(map, offset + (i * SM_CHUNK_MAX_CAPACITY));
    size_t calculated_offset = i * (SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2));
    if (calculated_offset != expected_offset) {
      return QCC_FAIL;
    }
  }

  // Now for RLE, first let's clear and check a full chunk.
  sparsemap_clear(map);
  for (int i = 0; i < SM_CHUNK_MAX_CAPACITY; i++) {
    sparsemap_set(map, i);
  }
  for (int i = 0; i < SM_CHUNK_MAX_CAPACITY; i++) {
    if (__sm_get_chunk_offset(map, i) != 0) {
      return QCC_FAIL;
    }
  }
  if (__sm_get_chunk_offset(map, SM_CHUNK_MAX_CAPACITY) != 0) {
    return QCC_FAIL;
  }

  // This should trigger the transformation of the 0th chunk into RLE.
  sparsemap_set(map, SM_CHUNK_MAX_CAPACITY);
  if (__sm_get_chunk_offset(map, SM_CHUNK_MAX_CAPACITY) != 0) {
    return QCC_FAIL;
  }
  // This should trigger the transformation of the 0th chunk back to sparse.
  sparsemap_unset(map, SM_CHUNK_MAX_CAPACITY);
  if (__sm_get_chunk_offset(map, SM_CHUNK_MAX_CAPACITY) != 0) {
    return QCC_FAIL;
  }

  // This should trigger the transformation of the 0th chunk into RLE again.
  for (int i = 0; i < 3000; i++) {
    sparsemap_set(map, SM_CHUNK_MAX_CAPACITY + i);
  }
  // This should trigger the transformation of the 0th chunk back to sparse,
  // but also create a second and third sparse chunks.
  sparsemap_unset(map, 0);
  if (__sm_get_chunk_offset(map, 0) != 0) {
    return QCC_FAIL;
  }
  sparsemap_set(map, 0);

  // This will split the chunk into two chunks; sparse, RLE.
  sparsemap_unset(map, 129);
  if (__sm_get_chunk_offset(map, 129) != 0) {
    return QCC_FAIL;
  }
  sparsemap_set(map, 129);

  // This will split the chunk into three chunks; sparse, sparse, RLE.
  sparsemap_unset(map, 2050);
  if (__sm_get_chunk_offset(map, 0) != 0) {
    return QCC_FAIL;
  }
  if (__sm_get_chunk_offset(map, 2050) != SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t)) {
    return QCC_FAIL;
  }
  if (__sm_get_chunk_offset(map, 2050 + SM_CHUNK_MAX_CAPACITY) != 2 * SM_SIZEOF_OVERHEAD + 3 * sizeof(__sm_bitvec_t)) {
    return QCC_FAIL;
  }
  sparsemap_set(map, 2050);

  // This won't split the chunk, it just shrinks the RLE by one.
  sparsemap_unset(map, 5047);
  if (__sm_get_chunk_offset(map, 5046) != 0) {
    return QCC_FAIL;
  }

  // This will split the chunk, the index is outside the range but inside the capacity.
  sparsemap_set(map, 5048);
  if (__sm_get_chunk_offset(map, 4090) != 0) {
    return QCC_FAIL;
  }
  if (__sm_get_chunk_offset(map, 5046) != 12) {
    return QCC_FAIL;
  }

  sparsemap_unset(map, 5048);
  if (__sm_get_chunk_offset(map, 5046) != 0) {
    return QCC_FAIL;
  }

  return QCC_OK;
}

#endif
