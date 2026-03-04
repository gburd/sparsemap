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
#include "popcount.h"
#include "sparsemap.h"
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
void __attribute__((format(printf, 4, 5))) __sm_diag_(const char *file, const int line, const char *func, const char *format, ...)
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
static char *_qcc_format_chunk(__sm_idx_t start, const __sm_chunk_t *chunk, bool none);

static void __attribute__((format(printf, 2, 3)))
__sm_diag_map(sparsemap_t *map, const char *fmt, ...)
{
  va_list args = { 0 };
  va_start(args, fmt);
  vfprintf(stdout, fmt, args);
  va_end(args);
  const char *s = QCC_showSparsemap(map, 0);
  fprintf(stdout, "\n%s\n", s);
  free((void *)s);
}

static void
__sm_diag_chunk(const char *msg, __sm_chunk_t *chunk)
{
  const char *s = QCC_showChunk(chunk, 0);
  fprintf(stdout, "%s\n%s\n", msg, s);
  free((char *)s);
}
#endif

enum __SM_CHUNK_INFO {
  /* metadata overhead: 4 bytes for __sm_chunk_t count */
  SM_SIZEOF_OVERHEAD = sizeof(__sm_idx_t),

  /* number of bits that can be stored in a __sm_bitvec_t */
  SM_BITS_PER_VECTOR = sizeof(__sm_bitvec_t) * 8,

  /* number of flags that can be stored in a single index byte */
  SM_FLAGS_PER_INDEX_BYTE = 4,

  /* number of flags that can be stored in the index */
  SM_FLAGS_PER_INDEX = sizeof(__sm_bitvec_t) * SM_FLAGS_PER_INDEX_BYTE,

  /* maximum capacity of a __sm_chunk_t (in bits) */
  SM_CHUNK_MAX_CAPACITY = SM_BITS_PER_VECTOR * SM_FLAGS_PER_INDEX,

  /* maximum capacity of a __sm_chunk_t (31 bits of the RLE) */
  SM_CHUNK_RLE_MAX_CAPACITY = 0x7FFFFFFF,

  /* minimum capacity of a __sm_chunk_t (in bits) */
  SM_CHUNK_MIN_CAPACITY = SM_BITS_PER_VECTOR - 2,

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

/* Used when separating an RLE chunk into 2-3 chunks */
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

  uint8_t buf[(SM_SIZEOF_OVERHEAD * (unsigned long)3) + (sizeof(__sm_bitvec_t) * 6)];
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
#define SM_CHUNK_SET_FLAGS(data, at, to) ((data) = ((data) & ~((__sm_bitvec_t)SM_FLAG_MASK << ((at)*2))) | ((__sm_bitvec_t)(to) << ((at)*2)))
#define SM_IS_CHUNK_RLE(chunk) \
  (((*((__sm_bitvec_t *)(chunk)->m_data) & (((__sm_bitvec_t)0x3) << (SM_BITS_PER_VECTOR - 2))) >> (SM_BITS_PER_VECTOR - 2)) == SM_PAYLOAD_NONE)

/*
 * RLE (Run-Length Encoding) Format
 *
 * RLE chunks encode a contiguous run of set bits (1s) starting at offset 0.
 * The entire chunk is represented by a single 64-bit descriptor word:
 *
 * Bits 63:62 = 01 (RLE flag, matches SM_PAYLOAD_NONE to distinguish from sparse)
 * Bits 61:31 = Chunk capacity in bits (31 bits, max 2,147,483,647)
 * Bits 30:0  = Run length in bits (31 bits, max 2,147,483,647)
 *
 * Example: If length=1000 and capacity=2048, bits 0-999 are set (1), bits 1000-2047 are unset (0).
 *
 * RLE chunks are immutable by design - any modification that would create gaps or
 * partial runs causes the chunk to be converted to sparse encoding.
 */
#define SM_RLE_FLAGS 0x4000000000000000          /* Bits 63:62 = 01 */
#define SM_RLE_FLAGS_MASK 0xC000000000000000     /* Mask for bits 63:62 */
#define SM_RLE_CAPACITY_MASK 0x3FFFFFFF80000000  /* Mask for bits 61:31 (capacity) */
#define SM_RLE_LENGTH_MASK 0x7FFFFFFF            /* Mask for bits 30:0 (length) */

/**
 * @brief Checks if the given chunk is flagged as RLE encoded.
 *
 * This function examines the first element in the chunk's data array to determine
 * if the chunk is run-length encoded (RLE).
 *
 * @param[in] chunk The chunk to check.
 * @return True if the chunk is flagged as RLE encoded, false otherwise.
 */
static bool
__sm_chunk_is_rle(const __sm_chunk_t *chunk)
{
  const __sm_bitvec_t w = chunk->m_data[0];
  return (w & SM_RLE_FLAGS_MASK) == SM_RLE_FLAGS;
}

/**
 * @brief Sets the Run-Length Encoding (RLE) flag on the specified chunk.
 *
 * This function modifies the first element in the chunk's data array to set
 * the RLE flag, indicating that the chunk is encoded using run-length encoding.
 *
 * @param[in,out] chunk The chunk to be flagged as RLE encoded.
 */
static void
__sm_chunk_set_rle(const __sm_chunk_t *chunk)
{
  __sm_bitvec_t w = chunk->m_data[0];
  w &= ~SM_RLE_FLAGS_MASK;
  w |= ((((__sm_bitvec_t)1) << (SM_BITS_PER_VECTOR - 2)) & SM_RLE_FLAGS_MASK);
  chunk->m_data[0] = w;
}

/**
 * @brief Retrieves the capacity of a run-length encoded (RLE) chunk.
 *
 * This function extracts and returns the capacity of an RLE chunk by masking
 * the relevant bits from the first element of the chunk's data array.
 *
 * @param[in] chunk The chunk whose capacity is to be retrieved.
 * @return The capacity of the RLE chunk.
 */
static size_t
__sm_chunk_rle_get_capacity(const __sm_chunk_t *chunk)
{
  __sm_bitvec_t w = chunk->m_data[0] & (__sm_bitvec_t)SM_RLE_CAPACITY_MASK;
  w >>= 31;
  return w;
}

/**
 * @brief Sets the capacity of an RLE encoded chunk.
 *
 * This function modifies the first element of the chunk's data array to set
 * the given capacity for a run-length encoded (RLE) chunk. The capacity is
 * masked and bit-shifted according to the RLE encoding specifications.
 *
 * This does not check the chunk type, if the chunk isn't RLE then this
 * function will overwrite flags data in a sparse chunk corrupting it.
 *
 * @param[in] chunk The chunk whose capacity is to be set.
 * @param[in] capacity The capacity to set for the RLE chunk.
 */
static void
__sm_chunk_rle_set_capacity(const __sm_chunk_t *chunk, const size_t capacity)
{
  __sm_assert(capacity <= SM_CHUNK_RLE_MAX_CAPACITY);
  __sm_bitvec_t w = chunk->m_data[0];
  w &= ~SM_RLE_CAPACITY_MASK;
  w |= capacity << 31 & SM_RLE_CAPACITY_MASK;
  chunk->m_data[0] = w;
}

/**
 * @brief Retrieves the run-length for a given RLE encoded chunk.
 *
 * This function extracts and returns the run-length information from the first
 * element of the chunk's data array using a predefined mask.
 *
 * A "run" is a set of adjacent ones that starts at the 0th bit of this
 * chunk. For an RLE chunk that's encoded in the descriptor.  For a sparse
 * chunk we must see how many flags are SM_PAYLOAD_ONES and then if we find an
 * SM_PAYLOAD_MIXED count the additional adjacent ones if they exist
 *
 * @param[in] chunk The RLE encoded chunk whose run-length is to be retrieved.
 * @return The run-length of the given chunk.
 */
static size_t
__sm_chunk_rle_get_length(const __sm_chunk_t *chunk)
{
  const __sm_bitvec_t w = chunk->m_data[0] & (__sm_bitvec_t)SM_RLE_LENGTH_MASK;
  return w;
}

/**
 * @brief Sets the length of a run-length encoded (RLE) chunk.
 *
 * This function updates the length field of a run-length encoded (RLE) chunk by
 * first validating that the new length is within the permissible maximum length,
 * then modifying the length bits within the chunk's data array accordingly.
 *
 * @param[in] chunk The chunk whose length is to be set.
 * @param[in] length The new length to set for the chunk.
 */
static void
__sm_chunk_rle_set_length(const __sm_chunk_t *chunk, const size_t length)
{
  __sm_assert(length <= SM_CHUNK_RLE_MAX_LENGTH);
  __sm_bitvec_t w = chunk->m_data[0];
  w &= ~SM_RLE_LENGTH_MASK;
  w |= length & SM_RLE_LENGTH_MASK;
  chunk->m_data[0] = w;
}

/**
 * @brief Gets the run length of a given chunk.
 *
 * This function calculates the run length of a given chunk. If the chunk is
 * run-length encoded (RLE), the length is obtained directly. Otherwise, it
 * calculates the run length by analyzing the bit vector data.
 *
 * @param[in] chunk The chunk to evaluate.
 * @return The run length of the chunk. Returns 0 if the chunk is not RLE
 *  encoded and cannot be determined to have a valid run length.
 */
static size_t
__sm_chunk_get_run_length(const __sm_chunk_t *chunk)
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
          while (k && (v & 1) == 1) {
            count++;
            v >>= 1;
            k--;
          }
          while (k && (v & 1) == 0) {
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

/**
 * @brief Calculates the vector size for a given byte value.
 *
 * This function uses a lookup table to determine the vector size associated
 * with a given byte value.
 *
 * Each entry in the lookup table represents a possible combination of 4 2-bit
 * values (00, 01, 10, 11).  The value at each index corresponds to the count
 * of "10" patterns in that 4-bit combination.  For example, lookup[10] is 2
 * because the binary representation of 10 (0000 1010) contains the "1010"
 * pattern twice.
 *
 * @param[in] b The byte value for which the vector size needs to be calculated.
 * @return The vector size associated with the given byte value.
 * @see bin/gen_chunk_vector_size_table.py
 */
static size_t
__sm_chunk_calc_vector_size(const uint8_t b)
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
  return lookup[b];
}

/**
 * @brief Retrieves the position within the chunk corresponding to the specified bit vector index.
 *
 * This function calculates the position in the chunk's data array that
 * corresponds to the given bit vector index. It handles both run-length
 * encoded (RLE) and non-RLE chunks.
 *
 * @param[in] chunk The chunk from which to retrieve the position.
 * @param[in] bv The bit vector index within the chunk.
 * @return The position within the chunk's data array corresponding to the specified bit vector index.
 */
static size_t
__sm_chunk_get_position(const __sm_chunk_t *chunk, size_t bv)
{
  /* Handle 4 indices (1 byte) at a time. */
  size_t position = 0;
  register uint8_t *p = (uint8_t *)chunk->m_data;

  /* Handle RLE by examining the first byte. */
  if (!__sm_chunk_is_rle(chunk)) {
    const size_t num_bytes = bv / ((size_t)SM_FLAGS_PER_INDEX_BYTE * SM_BITS_PER_VECTOR);
    for (size_t i = 0; i < num_bytes; i++, p++) {
      position += __sm_chunk_calc_vector_size(*p);
    }

    bv -= num_bytes * SM_FLAGS_PER_INDEX_BYTE;
    for (size_t i = 0; i < bv; i++) {
      const size_t flags = SM_CHUNK_GET_FLAGS(*chunk->m_data, i);
      if (flags == SM_PAYLOAD_MIXED) {
        position++;
      }
    }
  }

  return position;
}

/**
 * @brief Initializes an __sm_chunk_t structure with the given data.
 *
 * This function sets the m_data member of the provided __sm_chunk_t structure to point
 * to the given data, cast as a pointer to __sm_bitvec_t.
 *
 * @param[in,out] chunk The chunk to initialize.
 * @param[in] data The data to associate with the chunk.
 */
static void
__sm_chunk_init(__sm_chunk_t *chunk, uint8_t *data)
{
  chunk->m_data = (__sm_bitvec_t *)data;
}

/**
 * @brief Retrieves the capacity of the given chunk.
 *
 * This function calculates the total capacity of the specified chunk,
 * considering if the chunk is run-length encoded (RLE) or not. For RLE
 * encoded chunks, the capacity is directly retrieved from the chunk's data.
 * For non-RLE encoded chunks, the capacity is computed by examining the
 * data and assessing the available, unused sections.
 *
 * @param[in] chunk The chunk whose capacity is to be determined.
 * @return The capacity of the chunk.
 */
static size_t
__sm_chunk_get_capacity(const __sm_chunk_t *chunk)
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
      const size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        capacity -= SM_BITS_PER_VECTOR;
      }
    }
  }
  return capacity;
}

/**
 * @brief Increases the capacity of a chunk to the specified value.
 *
 * This function adjusts the capacity of a given chunk, ensuring that the new capacity
 * is a multiple of SM_BITS_PER_VECTOR, does not exceed the maximum allowed capacity,
 * and is greater than the current capacity of the chunk. The capacity is increased by
 * marking payload bits in the chunk's data array.
 *
 * @param[in,out] chunk The chunk whose capacity is to be increased.
 * @param[in] capacity The new capacity to set for the chunk.
 */
static void
__sm_chunk_increase_capacity(const __sm_chunk_t *chunk, const size_t capacity)
{
  __sm_assert(capacity % SM_BITS_PER_VECTOR == 0);
  __sm_assert(capacity <= SM_CHUNK_MAX_CAPACITY);
  __sm_assert(capacity > __sm_chunk_get_capacity(chunk));

  const size_t initial_capacity = __sm_chunk_get_capacity(chunk);
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
      const size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        *p &= ~((__sm_bitvec_t)SM_PAYLOAD_ONES << j * 2);
        *p |= (__sm_bitvec_t)SM_PAYLOAD_ZEROS << j * 2;
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

/**
 * @brief Determines if a given chunk is empty.
 *
 * This function checks if all flags within the chunk's data are either
 * SM_PAYLOAD_ZEROS or SM_PAYLOAD_NONE. If any flag doesn't meet these
 * criteria, the chunk is considered not empty.
 *
 * @param[in] chunk The chunk to be evaluated.
 * @return True if the chunk is empty, otherwise false.
 */
static bool
__sm_chunk_is_empty(const __sm_chunk_t *chunk)
{
  if (chunk->m_data[0] != 0) {
    /* A chunk is considered empty if all flags are SM_PAYLOAD_ZERO or _NONE. */
    register uint8_t *p = (uint8_t *)chunk->m_data;
    for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
      if (*p) {
        for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
          const size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
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

/**
 * @brief Retrieves the size of the specified chunk.
 *
 * This function calculates the memory size required by the given chunk.
 * If the chunk is not run-length encoded (RLE), the function iterates
 * over the chunk's data array and computes the size using a lookup table.
 *
 * @param[in] chunk The chunk whose size is to be determined.
 * @return The size of the chunk in bytes.
 */
static size_t
__sm_chunk_get_size(const __sm_chunk_t *chunk)
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

/**
 * @brief Checks if a specific bit is set in a given chunk.
 *
 * This function determines if a bit at a specific index within a chunk is set. The
 * chunk can be either run-length encoded (RLE) or contain a mixture of payloads.
 *
 * @param[in] chunk The chunk to check.
 * @param[in] idx The index of the bit to check within the chunk.
 * @return True if the bit at the specified index is set, false otherwise.
 */
static bool
__sm_chunk_is_set(const __sm_chunk_t *chunk, const size_t idx)
{
  if (__sm_chunk_is_rle(chunk)) {
    if (idx < __sm_chunk_rle_get_length(chunk)) {
      return true;
    }
    return false;
  }
  /* in which __sm_bitvec_t is |idx| stored? */
  const size_t bv = idx / SM_BITS_PER_VECTOR;
  __sm_assert(bv < SM_FLAGS_PER_INDEX);

  /* now retrieve the flags of that __sm_bitvec_t */
  const size_t flags = SM_CHUNK_GET_FLAGS(*chunk->m_data, bv);
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
  const __sm_bitvec_t w = chunk->m_data[1 + __sm_chunk_get_position(chunk, bv)];
  /* and finally check the bit in that __sm_bitvec_t */
  return (w & (__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR) > 0;
}

/**
 * @brief Clears a specific bit in a chunk.
 *
 * This function attempts to clear a specified bit within a given chunk.
 * Based on the payload flags in the chunk, it will update the position of
 * the bit and handle transitions between different payload states
 * (ZEROS, ONES, MIXED). If the bit is already clear, it performs a no-op.
 * If the bit is set, it updates the relevant data structures accordingly,
 * possibly requiring the chunk to grow or shrink.
 *
 * @param[in] chunk The chunk in which to clear the bit.
 * @param[in] idx The index of the bit to be cleared.
 * @param[out] pos The position of the bit to be cleared; updated internally.
 * @return An integer status code indicating the result:
 *         - SM_OK if the operation was successful,
 *         - SM_NEEDS_TO_GROW if the chunk needs to grow,
 *         - SM_NEEDS_TO_SHRINK if the chunk needs to shrink.
 */
static int
__sm_chunk_clr_bit(const __sm_chunk_t *chunk, const sparsemap_idx_t idx, size_t *pos)
{
  __sm_bitvec_t w;
  const size_t bv = idx / SM_BITS_PER_VECTOR;

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
    w &= ~((__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR);
    /* Update the mixed vector. */
    chunk->m_data[*pos] = w;
    return SM_OK;
    break;
  case SM_PAYLOAD_MIXED:
    *pos = 1 + __sm_chunk_get_position(chunk, bv);
    w = chunk->m_data[*pos];
    w &= ~((__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR);
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

/**
 * @brief Sets a bit within a chunk at the specified index.
 *
 * This function sets a bit in the given chunk at the location specified by the index.
 * It handles different payload states (all ones, all zeros, and mixed) and updates
 * the chunk's data and flags accordingly.
 *
 * @param[in] chunk The chunk to modify.
 * @param[in] idx The index within the chunk where the bit should be set.
 * @param[out] pos Pointer to a size_t that will be set to the position of the bit.
 * @return An integer indicating the status of the operation. Possible return values are:
 *         - SM_OK: The bit was successfully set.
 *         - SM_NEEDS_TO_GROW: The chunk needs additional space.
 *         - SM_NEEDS_TO_SHRINK: The chunk has excess space that can be reclaimed.
 */
static int
__sm_chunk_set_bit(const __sm_chunk_t *chunk, const sparsemap_idx_t idx, size_t *pos)
{
  /* Where in the descriptor does this idx fall, which flag should we examine? */
  const size_t bv = idx / SM_BITS_PER_VECTOR;
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
    w |= (__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR;
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

/**
 * @brief Selects the nth bit with the specified value from a chunk.
 *
 * This function scans a chunk of data to find the nth occurrence of a bit
 * with the specified value (true for 1, false for 0) after skipping offset
 * bits (of any value).
 *
 * @param[in] chunk The chunk to scan for the bit.
 * @param[in] n The number of bits of value to count before returning.
 * @param[in,out] offset The number of bits to skip before starting to count.
 * @param[in] value The bit value to search for (true for 1, false for 0).
 * @return The index within this chunk of the bit when found, otherwise the
 * number of bits scanned (at most SM_BITS_PER_VECTOR).
 */
static size_t
__sm_chunk_select(const __sm_chunk_t *chunk, ssize_t n, ssize_t *offset, const bool value)
{
  /* RLE fast path */
  if (__sm_chunk_is_rle(chunk)) {
    const size_t length = __sm_chunk_rle_get_length(chunk);
    const size_t capacity = __sm_chunk_rle_get_capacity(chunk);

    if (value) {
      /* Selecting nth set bit (1) */
      /* RLE has run of 1s from index 0 to length-1 */
      if (n < (ssize_t)length) {
        *offset = -1;
        return n;  /* nth set bit is at index n */
      } else {
        *offset = n - length;  /* propagate remainder to next chunk */
        return capacity;
      }
    } else {
      /* Selecting nth unset bit (0) */
      /* Unset bits start at index length */
      if (length >= capacity) {
        /* No unset bits in this chunk */
        *offset = n;
        return capacity;
      }
      const size_t unset_count = capacity - length;
      if (n < (ssize_t)unset_count) {
        *offset = -1;
        return length + n;  /* nth unset bit is at (length + n) */
      } else {
        *offset = n - unset_count;  /* propagate remainder */
        return capacity;
      }
    }
  }

  /*
   * Sparse encoding path
   *
   * Algorithm: Iterate through flag bytes examining 2-bit descriptors for each 64-bit vector.
   * Skip vectors that can't contain the target value (ZEROS when searching for 1s, ONES when
   * searching for 0s). For MIXED vectors, use popcount to quickly check if we need to scan
   * individual bits. Accumulate bit positions until we've found the nth occurrence.
   */
  size_t ret = 0;
  register uint8_t *p = (uint8_t *)chunk->m_data;
  for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, p++) {
    /* Quick skip: if flag byte is 0 (all NONE descriptors) and seeking 1s, skip 4 vectors */
    if (*p == 0 && value) {
      ret += (size_t)SM_FLAGS_PER_INDEX_BYTE * SM_BITS_PER_VECTOR;
      continue;
    }

    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      const size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        continue;
      }
      if (flags == SM_PAYLOAD_ZEROS) {
        if (value == true) {
          ret += SM_BITS_PER_VECTOR;
          continue;
        }
        if (n > SM_BITS_PER_VECTOR) {
          n -= SM_BITS_PER_VECTOR;
          ret += SM_BITS_PER_VECTOR;
          continue;
        }
        *offset = -1;
        return ret + n;
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
        }
        ret += SM_BITS_PER_VECTOR;
        continue;
      }
      if (flags == SM_PAYLOAD_MIXED) {
        const __sm_bitvec_t w = chunk->m_data[1 + __sm_chunk_get_position(chunk, (i * SM_FLAGS_PER_INDEX_BYTE) + j)];
        for (int k = 0; k < SM_BITS_PER_VECTOR; k++) {
          if (value) {
            if (w & (__sm_bitvec_t)1 << k) {
              if (n == 0) {
                *offset = -1;
                return ret;
              }
              n--;
            }
            ret++;
          } else {
            if (!(w & (__sm_bitvec_t)1 << k)) {
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
 * @brief Calculates the rank of a bit in a chunk between specified indices.
 *
 * This function computes the number of bits set to a particular state (true
 * or false) within a chunk of data, starting from a specified index and ending
 * at a specified index. The chunk can either be run-length encoded (RLE) or
 * sparsely encoded.
 *
 * Invoking this function with `from = 0` and `to = 0` (the range [0, 0]), will
 * compare 1 bit at the position 0 against value. The range [0, 9] will examine
 * 10 bits, starting with the 0th and ending with the 9th and return at most a
 * count of 10.
 *
 * @param[out] rank Pointer to the rank data structure to populate.
 * @param[in] value The bit state to calculate the rank for (true or false).
 * @param[in] chunk Pointer to the chunk to be examined.
 * @param[in] from The starting index within the chunk.
 * @param[in] to The ending index within the chunk.
 * @return The number of bits in the specified state between the indices [from, to].
 */
static size_t
__sm_chunk_rank(__sm_chunk_rank_t *rank, const bool value, const __sm_chunk_t *chunk, size_t from, size_t to)
{
  size_t amt = 0;
  const size_t cap = __sm_chunk_get_capacity(chunk);

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
    const size_t end = __sm_chunk_rle_get_length(chunk) - 1;
    rank->rem = 0;
    if (value) {
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
        amt = cap - (end + 1) - 1;
        rank->pos = cap;
      }
    }
  } else {
    /*
     * Sparse encoding rank algorithm
     *
     * Strategy: Iterate through flag bytes and use popcounts for efficient bit counting.
     * For ZEROS/ONES payloads, we know the count immediately (0 or 64). For MIXED payloads,
     * extract the 64-bit vector and use hardware popcount. Apply range masks to only count
     * bits within [from, to] range. This achieves O(chunks) performance instead of O(bits).
     */
    uint8_t *vec = (uint8_t *)chunk->m_data;
    __sm_bitvec_t w, mw;
    uint64_t mask;
    size_t pc;

    for (size_t i = 0; i < sizeof(__sm_bitvec_t); i++, vec++) {
      for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
        const size_t flags = SM_CHUNK_GET_FLAGS(*vec, j);

        switch (flags) {

        case SM_PAYLOAD_ZEROS:
          rank->rem = 0;
          if (to >= SM_BITS_PER_VECTOR) {
            rank->pos += SM_BITS_PER_VECTOR;
            to -= SM_BITS_PER_VECTOR;
            if (from >= SM_BITS_PER_VECTOR) {
              from = from - SM_BITS_PER_VECTOR;
            } else {
              if (!value) {
                amt += SM_BITS_PER_VECTOR - from;
              }
              from = 0;
            }
          } else {
            rank->pos += to + 1;
            if (!value) {
              if (from > to) {
                from -= to;
              } else {
                amt += to + 1 - from;
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
              if (value) {
                amt += SM_BITS_PER_VECTOR - from;
              }
              from = 0;
            }
          } else {
            rank->pos += to + 1;
            if (value) {
              if (from > to) {
                from = from - to;
              } else {
                amt += to + 1 - from;
                goto done;
              }
            } else {
              goto done;
            }
          }
          break;

        case SM_PAYLOAD_MIXED:
          w = chunk->m_data[1 + __sm_chunk_get_position(chunk, (i * SM_FLAGS_PER_INDEX_BYTE) + j)];
          if (to >= SM_BITS_PER_VECTOR) {
            rank->pos += SM_BITS_PER_VECTOR;
            to -= SM_BITS_PER_VECTOR;
            mask = from == 0 ? UINT64_MAX : ~(UINT64_MAX >> (SM_BITS_PER_VECTOR - (from >= 64 ? 64 : from)));
            mw = (value ? w : ~w) & mask;
            pc = popcountll(mw);
            amt += pc;
            from = from > SM_BITS_PER_VECTOR ? from - SM_BITS_PER_VECTOR : 0;
          } else {
            rank->pos += to + 1;
            const uint64_t to_mask = (to == 63) ? UINT64_MAX : ((uint64_t)1 << (to + 1)) - 1;
            const uint64_t from_mask = from == 0 ? UINT64_MAX : ~(UINT64_MAX >> (SM_BITS_PER_VECTOR - (from >= 64 ? 64 : from)));
            /* Create a mask for the range [from, to] and use popcount. */
            mask = to_mask & from_mask;
            mw = (value ? w : ~w) & mask;
            pc = popcountll(mw);
            amt += pc;
            rank->rem = mw >> (from > 63 ? 63 : from);
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

/**
 * @brief Scans a chunk allowing the callee to process each vector.
 *
 * This function iterates through a chunk's data and processes these
 * payloads using the provided scanner function.
 *
 * @param[in] chunk The chunk to scan.
 * @param[in] start The starting index for the scan.
 * @param[in] scanner The callback function to process discovered vectors.
 * @param[in] skip The number of vectors to skip before processing.
 * @param[in] aux Auxiliary data to pass to the scanner function.
 * @return The total number of processed vectors.
 */
static size_t
__sm_chunk_scan(const __sm_chunk_t *chunk, const __sm_idx_t start, void (*scanner)(uint32_t[], size_t, void *aux), size_t skip, void *aux)
{
  /* RLE fast path */
  if (__sm_chunk_is_rle(chunk)) {
    const size_t length = __sm_chunk_rle_get_length(chunk);

    /* RLE chunks only contain set bits from 0 to length-1 */
    if (skip >= length) {
      return length;  /* Skipped all bits in this chunk */
    }

    /* Skip first `skip` bits, then scan the rest */
    const size_t scan_start = skip;

    /* Process in batches using same buffer size as sparse code */
    uint32_t buffer[SM_BITS_PER_VECTOR];

    for (size_t i = scan_start; i < length; ) {
      size_t batch_size = SM_BITS_PER_VECTOR;
      if (i + batch_size > length) {
        batch_size = length - i;
      }

      /* Fill buffer with consecutive indices */
      for (size_t j = 0; j < batch_size; j++) {
        buffer[j] = start + i + j;
      }

      scanner(&buffer[0], batch_size, aux);
      i += batch_size;
    }

    return skip;  /* Return number of bits skipped in this chunk */
  }

  /* Sparse encoding path */
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
      const size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
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
        const __sm_bitvec_t w = chunk->m_data[1 + __sm_chunk_get_position(chunk, (i * SM_FLAGS_PER_INDEX_BYTE) + j)];
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

/**
 * @brief Retrieves the count of chunks in the sparse map.
 *
 * This function reads the first 32-bit integer from the `m_data` array
 * of the given sparse map to determine and return the number of chunks.
 *
 * @param[in] map The sparse map from which to retrieve the chunk count.
 * @return The number of chunks in the sparse map.
 */
static size_t
__sm_get_chunk_count(const sparsemap_t *map)
{
  return *(uint32_t *)&map->m_data[0];
}

/**
 * @brief Retrieves a pointer to the data at the specified offset within the sparse map.
 *
 * This function calculates the address of the data starting after a predefined
 * overhead and adds the provided offset to this start point. The resulting
 * pointer points to the actual data within the sparse map.
 *
 * @param[in] map A pointer to the sparse map.
 * @param[in] offset The offset within the sparse map where the data starts.
 * @return A pointer to the data at the specified offset within the sparse map.
 */
static uint8_t *
__sm_get_chunk_data(const sparsemap_t *map, const size_t offset)
{
  return &map->m_data[SM_SIZEOF_OVERHEAD + offset];
}

/**
 * @brief Calculates the capacity limit for a run-length encoded (RLE) chunk.
 *
 * This function determines the capacity limit of a run-length encoded (RLE)
 * chunk in a sparse map, based on the provided map, start index, and offset.
 *
 * @param[in] map The sparse map containing the chunk.
 * @param[in] start The starting index of the chunk.
 * @param[in] offset The offset within the sparse map's data.
 * @return The capacity limit of the RLE chunk.
 */
static size_t
__sm_chunk_rle_capacity_limit(const sparsemap_t *map, const __sm_idx_t start, const size_t offset)
{
  const size_t next_offset = offset + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
  if (next_offset < map->m_data_used - (SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t))) {
    uint8_t *p = __sm_get_chunk_data(map, next_offset);
    const __sm_idx_t next_start = *(__sm_idx_t *)p;
    return next_start - start;
  }
  return SM_CHUNK_RLE_MAX_CAPACITY;
}

/**
 * @brief Computes the end pointer of the chunk data in the sparse map.
 *
 * This function calculates the end of the chunk data by iterating through all
 * the chunks present in the sparse map, taking into account the overhead size
 * and the size of each chunk.
 *
 * @param[in] map The sparse map whose chunk end pointer needs to be calculated.
 * @return A pointer to the end of the chunk data in the sparse map.
 */
static uint8_t *
__sm_get_chunk_end(const sparsemap_t *map)
{
  uint8_t *p = __sm_get_chunk_data(map, 0);
  const size_t count = __sm_get_chunk_count(map);
  for (size_t i = 0; i < count; i++) {
    p += SM_SIZEOF_OVERHEAD;
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);
    p += __sm_chunk_get_size(&chunk);
  }
  return p;
}

/**
 * @brief Computes the aligned offset for a given index based on chunk capacity.
 *
 * This function calculates the offset for the provided index such that
 * it aligns with the chunk boundaries defined by the maximum chunk capacity.
 *
 * @param[in] idx The index for which the aligned offset is to be computed.
 * @return The aligned offset corresponding to the given index.
 */
static __sm_idx_t
__sm_get_chunk_aligned_offset(const size_t idx)
{
  const size_t capacity = SM_CHUNK_MAX_CAPACITY;
  return idx / capacity * capacity;
}

/**
 * @brief Calculates the total size of the sparse map's used data.
 *
 * This function iterates through each chunk in the sparse map and computes
 * the total memory used by the map, including overhead.
 *
 * @param[in] map Pointer to the sparse map.
 * @return Total size of the used data in the sparse map.
 */
static size_t
__sm_get_size_impl(const sparsemap_t *map)
{
  uint8_t *start = __sm_get_chunk_data(map, 0);
  uint8_t *p = start;

  const size_t count = __sm_get_chunk_count(map);
  for (size_t i = 0; i < count; i++) {
    p += SM_SIZEOF_OVERHEAD;
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);
    p += __sm_chunk_get_size(&chunk);
  }
  return SM_SIZEOF_OVERHEAD + p - start;
}

/**
 * @brief Retrieves the offset of a specified chunk within the sparse map.
 *
 * This function iterates through the chunks in the sparse map to find the
 * offset of the chunk that either contains or would logically contain the
 * given index.
 *
 * @param[in] map The sparse map to search within.
 * @param[in] idx The index to find the corresponding chunk offset for.
 * @return The offset of the chunk if found, otherwise -1 if no appropriate chunk is found.
 */
static ssize_t
__sm_get_chunk_offset(const sparsemap_t *map, const sparsemap_idx_t idx)
{
  const size_t count = __sm_get_chunk_count(map);

  if (count == 0) {
    return -1;
  }

  uint8_t *start = __sm_get_chunk_data(map, 0);
  uint8_t *p = start;

  for (size_t i = 0; i < count - 1; i++) {
    const __sm_idx_t s = *(__sm_idx_t *)p;
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
    __sm_assert(s == __sm_get_chunk_aligned_offset(s));
    if (s >= idx || idx < s + __sm_chunk_get_capacity(&chunk)) {
      break;
    }
    p += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
  }

  return p - start;
}

/**
 * @brief Sets the chunk count for the sparsemap to a new value.
 *
 * This function updates the chunk count stored in the map's data array
 * to the specified new count.
 *
 * @param[in,out] map The sparsemap in which to set the chunk count.
 * @param[in] new_count The new chunk count to set.
 */
static void
__sm_set_chunk_count(const sparsemap_t *map, const size_t new_count)
{
  *(uint32_t *)&map->m_data[0] = (uint32_t)new_count;
}

/**
 * @brief Appends data to the sparsemap's internal buffer.
 *
 * This function appends the provided buffer to the sparsemap's internal data
 * storage, ensuring that there is enough capacity in the buffer to accommodate
 * the new data.
 *
 * @param[in] map Pointer to the sparsemap structure where data will be appended.
 * @param[in,out] buffer Pointer to the data buffer to be appended to the sparsemap.
 * @param[in] buffer_size Size of the data buffer to be appended.
 */
static void
__sm_append_data(sparsemap_t *map, const uint8_t *buffer, const size_t buffer_size)
{
  __sm_assert(map->m_data_used + buffer_size <= map->m_capacity);

  memcpy(&map->m_data[map->m_data_used], buffer, buffer_size);
  map->m_data_used += buffer_size;
}

/**
 * @brief Inserts data into the sparse map at the specified offset.
 *
 * This function asserts that there is enough capacity in the map to accommodate
 * the new data, retrieves the appropriate chunk of data from the map, and then
 * inserts the provided buffer at the given offset. The existing data is moved
 * to make space for the new data, and the map's used data size is updated accordingly.
 *
 * @param[in,out] map Pointer to the sparse map where data will be inserted.
 * @param[in] offset Offset in the map where the data should be inserted.
 * @param[in] buffer Pointer to the buffer containing the data to be inserted.
 * @param[in] buffer_size Size of the buffer in bytes.
 */
void
__sm_insert_data(sparsemap_t *map, const size_t offset, const uint8_t *buffer, const size_t buffer_size)
{
  __sm_assert(map->m_data_used + buffer_size <= map->m_capacity);

  uint8_t *p = __sm_get_chunk_data(map, offset);
  memmove(p + buffer_size, p, map->m_data_used - offset);
  memcpy(p, buffer, buffer_size);
  map->m_data_used += buffer_size;
}

/**
 * @brief Removes a contiguous block of data from the sparsemap.
 *
 * This function removes a block of data from the sparsemap at the specified offset
 * and reduces the size of the data used accordingly.
 *
 * @param[in,out] map A pointer to the sparsemap from which data will be removed.
 * @param[in] offset The starting position of the block to be removed.
 * @param[in] gap_size The size of the block to be removed.
 */
static void
__sm_remove_data(sparsemap_t *map, const size_t offset, const size_t gap_size)
{
  __sm_assert(map->m_data_used >= gap_size);
  uint8_t *p = __sm_get_chunk_data(map, offset);
  memmove(p, p + gap_size, map->m_data_used - offset - gap_size);
  map->m_data_used -= gap_size;
}

/**
 * @brief Coalesces the specified chunk with adjacent chunks if conditions are met.
 *
 * This function attempts to merge the provided chunk with its adjacent chunks
 * in a sparse map if they meet certain conditions. The goal is to reduce the
 * number of chunks by combining adjacent ones that form continuous runs.
 *
 * @param[in] map The sparse map that contains the chunk.
 * @param[in] chunk The chunk to be potentially coalesced.
 * @param[in] offset The offset of the chunk in the sparse map.
 * @param[in] start The starting index of the chunk.
 * @param[in,out] p Pointer to the chunk's data.
 * @return The number of chunks that were removed during the coalescing process.
 */
static int
__sm_coalesce_chunk(sparsemap_t *map, __sm_chunk_t *chunk, size_t offset, __sm_idx_t start, uint8_t *p)
{
  /*
  * This is called from __sm_chunk_set/unset/merge/split functions when a
  * there is a chance that chunks should combine into runs to use less
  * space in the map.
  *
  * The provided chunk may have two adjacent chunks, this function first
  * processes the chunk to the left and then the one to the right.
  *
  * In the case that there is a chunk to the left (with a lower starting index)
  * we examine its type and ending offset as well as it's run length.  Either
  * type of chunk (sparse and RLE) can have a run.  In the case of an RLE chunk
  * that's all it can express.  With a sparse chunk a run is defined as adjacent
  * set bits starting at the 0th index of the chunk and extending up to at most
  * the maximum size of a chunk without gaps ([1..SM_CHUNK_MAX_CAPACITY] in
  * length).  When the left chunk's run ends at the starting index of this chunk
  * we can combine them. Combining these two will always result in an RLE chunk.
  *
  * Once that is finished... we may have something to the right as well.  We look
  * for an adjacent chunk, then determine if it has a run with a starting point
  * adjacent to the end of a run in this chunk.  At this point we may have
  * mutated and coalesced the left into the center chunk which we further mutate
  * and combine with the right.  At most, we can combine three chunks into one in
  * these two phases.
  */
  int num_removed = 0;
  const size_t run_length = __sm_chunk_get_run_length(chunk);
  /* Did this chunk become all ones, can we compact it with adjacent chunks? */
  if (run_length > 0) {
    __sm_chunk_t adj;

    /* Is there a previous chunk? */
    if (offset > 0) {
      const size_t adj_offset = __sm_get_chunk_offset(map, start - 1);
      if (adj_offset < offset) {
        uint8_t *adj_p = __sm_get_chunk_data(map, adj_offset);
        const __sm_idx_t adj_start = *(__sm_idx_t *)adj_p;
        __sm_chunk_init(&adj, adj_p + SM_SIZEOF_OVERHEAD);
        /* Is the adjacent chunk on the left RLE or a sparse chunk of all ones? */
        if (__sm_chunk_is_rle(&adj) || adj.m_data[0] == ~(__sm_bitvec_t)0) {
          /* Does it align with this full sparse chunk? */
          const size_t adj_length = __sm_chunk_get_run_length(&adj);
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
      const size_t adj_offset = offset + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
      if (adj_offset < map->m_data_used - (SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t))) {
        uint8_t *adj_p = __sm_get_chunk_data(map, adj_offset);
        const __sm_idx_t adj_start = *(__sm_idx_t *)adj_p;
        __sm_chunk_init(&adj, adj_p + SM_SIZEOF_OVERHEAD);
        /* Is the adjacent right chunk RLE or a sparse with a run of ones? */
        const size_t adj_length = __sm_chunk_get_run_length(&adj);
        if (adj_length) {
          /* Does it align with this full sparse chunk? */
          const size_t length = __sm_chunk_get_run_length(chunk);
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
 * @brief Coalesces adjacent chunks in a sparse map, optimizing its structure.
 *
 * This function iterates through the chunks in the provided sparse map and
 * attempts to coalesce adjacent chunks to reduce fragmentation and improve
 * efficiency.
 *
 * @param[in] map The sparse map to coalesce.
 * @return The number of bytes coalesced during the operation.
 */
size_t
__sm_coalesce_map(sparsemap_t *map)
{
  __sm_chunk_t chunk;
  size_t n = 0, count = __sm_get_chunk_count(map);
  const size_t offset = 0;
  uint8_t *p = __sm_get_chunk_data(map, offset);

  while (count > 1) {
    const __sm_idx_t start = *(__sm_idx_t *)p;
    __sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
    const size_t amt = __sm_coalesce_chunk(map, &chunk, offset, start, p);
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

/**
 * @brief Separates a run-length encoded (RLE) chunk into new chunks based on the provided parameters.
 *
 * This function is called from various chunk manipulation functions such as
 * set, unset, merge, and split when an RLE chunk needs to be mutated into one
 * or more new chunks. It determines the separation and alignment of the pivot
 * chunk with respect to the target chunk.
 *
 * @param[in] map The sparse map containing the chunks.
 * @param[in] sep The separation information required to perform the chunk separation.
 * @param[in] idx The index within the chunk where the separation or mutation is required.
 * @param[in] state The state representing the operation: 0 for clearing a bit, 1 for setting a bit,
 *                  and -1 for splitting without modifying the map.
 * @return Integer value indicating the status of the operation:
 *         0 if the operation is successful,
 *         an error code otherwise.
 */
static int
__sm_separate_rle_chunk(sparsemap_t *map, __sm_chunk_sep_t *sep, const sparsemap_idx_t idx, const int state)
{
/*
 * This is called from __sm_chunk_set/unset/merge/split functions when a
 * run-length encoded (RLE) chunk must be mutated into one or more new chunks.
 *
 * This function expects that the separation information is complete and that
 * the pivot chunk has yet to be created.  The target will always be RLE and the
 * pivot will always be a new sparse chunk.  The hard part is where the pivot
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
 */

  __sm_chunk_t pivot_chunk;
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
    /* if `state == -1` we are splitting at idx but leaving map unmodified */
  }

  memset(sep->buf, 0, (SM_SIZEOF_OVERHEAD * (unsigned long)3) + (sizeof(__sm_bitvec_t) * 6));

  /* Find the starting offset for our pivot chunk ... */
  const sparsemap_idx_t aligned_idx = __sm_get_chunk_aligned_offset(idx);
  __sm_assert(idx >= aligned_idx && idx < aligned_idx + SM_CHUNK_MAX_CAPACITY);
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
    pivot_chunk.m_data[1] = ~(__sm_bitvec_t)0 & ~((__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR);
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
      /* Does our pivot extend beyond the end of the run. */
      const sparsemap_idx_t amt_over = aligned_idx + SM_CHUNK_MAX_CAPACITY - (sep->target.start + sep->target.length);
      if (amt_over > 0) {
        /* The index of the first 0 bit. */
        const size_t first_zero = SM_CHUNK_MAX_CAPACITY - amt_over;
        const size_t bv = first_zero / SM_BITS_PER_VECTOR;
        /* Shorten the pivot chunk because it extends beyond the end of the run ... */
        if (amt_over > SM_BITS_PER_VECTOR) {
          pivot_chunk.m_data[0] &= ~(__sm_bitvec_t)0 >> amt_over / SM_BITS_PER_VECTOR * 2;
        }
        if (amt_over % SM_BITS_PER_VECTOR) {
          /* Change only the flag at the position of the last index to "mixed" ... */
          SM_CHUNK_SET_FLAGS(pivot_chunk.m_data[0], bv, SM_PAYLOAD_MIXED);
          /* and unset the bits beyond that. */
          pivot_chunk.m_data[1] = ~(~(__sm_bitvec_t)0 << first_zero % SM_BITS_PER_VECTOR);
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
        pivot_chunk.m_data[1] |= (__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR;
      }

      /* Move the pivot chunk over to make room for the new left chunk. */
      memmove((uint8_t *)((uintptr_t)sep->buf + SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2)), sep->buf, sep->pivot.size);
      memset(sep->buf, 0, SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2));
      sep->pivot.p += SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2);
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
          pivot_chunk.m_data[1] |= (__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR;
        }
        /* Move the pivot chunk over to make room for the new left chunk. */
        memmove((uint8_t *)((uintptr_t)sep->buf + SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2)), sep->buf, sep->pivot.size);
        memset(sep->buf, 0, SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2));
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
    memmove((uint8_t *)((uintptr_t)sep->buf + SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2)), sep->buf, sep->pivot.size);
    memset(sep->buf, 0, SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2));
    sep->pivot.p += SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2);
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
          const size_t right_offset = sep->target.offset + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
          __sm_chunk_rle_set_capacity(&lrc, __sm_chunk_rle_capacity_limit(map, aligned_idx, right_offset));
        }
        /* ... and record our chunk size. */
        sep->ex[i].size = SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
      } else {
        /* ... we need a new sparse chunk, how long should it be? ... */
        const size_t lrl = sep->ex[i].end - sep->ex[i].start + 1;
        /* ... how many flags can we mark as all ones? ... */
        if (lrl > SM_BITS_PER_VECTOR) {
          lrc.m_data[0] = ~(__sm_bitvec_t)0 >> (SM_FLAGS_PER_INDEX - lrl / SM_BITS_PER_VECTOR) * 2;
        }
        /* ... do we have a mixed flag to create and vector to assign? ... */
        if (lrl % SM_BITS_PER_VECTOR) {
          SM_CHUNK_SET_FLAGS(lrc.m_data[0], (aligned_idx + lrl) / SM_BITS_PER_VECTOR, SM_PAYLOAD_MIXED);
          lrc.m_data[1] |= ~(__sm_bitvec_t)0 >> (SM_BITS_PER_VECTOR - lrl) % SM_BITS_PER_VECTOR;
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

/**
 * @brief Merges two chunks from the sparse map, aligning the source chunk with
 *        the destination chunk while adjusting the underlying data structure.
 *
 * This function merges a source chunk into a destination chunk within the sparse map.
 * It adjusts offsets and manages the insertion and removal of data to keep the
 * map consistent.
 *
 * @param[in] map        Pointer to the sparse map object.
 * @param[in] src_start  Starting index of the source chunk.
 * @param[in] dst_start  Starting index of the destination chunk.
 * @param[in] capacity   The capacity of the chunk, indicating the number of elements to process.
 * @param[in] dst_chunk  Pointer to the destination chunk.
 * @param[in] src_chunk  Pointer to the source chunk.
 */
void
__sm_merge_chunk(sparsemap_t *map, const sparsemap_idx_t src_start, const sparsemap_idx_t dst_start, const sparsemap_idx_t capacity, const __sm_chunk_t *dst_chunk,
  const __sm_chunk_t *src_chunk)
{
  __sm_bitvec_t fill = 0;
  const ssize_t delta = (ssize_t)src_start - (ssize_t)dst_start;
  for (sparsemap_idx_t j = 0; j < capacity; j++) {
    ssize_t offset = __sm_get_chunk_offset(map, src_start + j);
    if (__sm_chunk_is_set(src_chunk, j) && !__sm_chunk_is_set(dst_chunk, j + delta)) {
      size_t position = 0;
      switch (__sm_chunk_set_bit(dst_chunk, j + delta, &position)) {
      case SM_NEEDS_TO_GROW:
        offset += (ssize_t)(SM_SIZEOF_OVERHEAD + (position * sizeof(__sm_bitvec_t)));
        __sm_insert_data(map, offset, (uint8_t *)&fill, sizeof(__sm_bitvec_t));
        __sm_chunk_set_bit(dst_chunk, j + delta, &position);
        break;
      case SM_NEEDS_TO_SHRINK:
        if (__sm_chunk_is_empty(src_chunk)) {
          __sm_assert(position == 1);
          __sm_remove_data(map, offset, SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2));
          __sm_set_chunk_count(map, __sm_get_chunk_count(map) - 1);
        } else {
          offset += SM_SIZEOF_OVERHEAD + (ssize_t)(position * sizeof(__sm_bitvec_t));
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

/**
 * @brief Clears the given sparse map.
 *
 * This function resets the sparse map by setting all its data to zero and updating
 * its metadata to reflect an empty map.
 *
 * @param[in] map The sparse map to clear.
 */
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

/**
 * @brief Allocates and initializes a sparsemap of the given size.
 *
 * This function creates a new sparsemap structure with allocated memory.
 * If the specified size is zero, a default size of 1024 is used. The function
 * ensures that the internal data array is 8-byte aligned and initializes the sparsemap
 * structure.
 *
 * @param[in] size The size of the sparsemap to allocate.
 * @return A pointer to the allocated sparsemap structure, or NULL if allocation fails.
 */
sparsemap_t *
sparsemap(size_t size)
{
  if (size == 0) {
    size = 1024;
  }

  const size_t data_size = size * sizeof(uint8_t);

  /* Ensure that m_data is 8-byte aligned. */
  size_t total_size = sizeof(sparsemap_t) + data_size;
  const size_t padding = total_size % 8 == 0 ? 0 : 8 - (total_size % 8);
  total_size += padding;

  sparsemap_t *map = calloc(1, total_size);
  if (map) {
    uint8_t *data = (uint8_t *)(((uintptr_t)map + sizeof(sparsemap_t)) & ~(uintptr_t)7);
    sparsemap_init(map, data, size);
    __sm_when_diag({ __sm_assert(IS_8_BYTE_ALIGNED(map->m_data)); });
  }
  return map;
}

/**
 * @brief Creates a copy of the given sparse map.
 *
 * This function duplicates the provided sparse map, allocating a new sparse
 * map instance with the same capacity and copying over the used data.
 *
 * @param[in] other The sparse map to be copied.
 * @return A pointer to the newly created sparse map that is a copy of the input,
 *         or NULL if the memory allocation fails.
 */
sparsemap_t *
sparsemap_copy(const sparsemap_t *other)
{
  const size_t cap = sparsemap_get_capacity(other);
  sparsemap_t *map = sparsemap(cap);
  if (map) {
    map->m_capacity = other->m_capacity;
    map->m_data_used = other->m_data_used;
    memcpy(map->m_data, other->m_data, cap);
  }
  return map;
}

/**
 * @brief Wraps a given data array into a sparsemap structure.
 *
 * Allocates and initializes a sparsemap_t structure to manage a provided data array.
 * The sparsemap structure will point to the data array and will track its capacity.
 *
 * @param[in] data Pointer to the data array to be managed by the sparsemap.
 * @param[in] size The size of the data array.
 * @return A pointer to the initialized sparsemap_t structure, or NULL if allocation fails.
 */
sparsemap_t *
sparsemap_wrap(uint8_t *data, const size_t size)
{
  sparsemap_t *map = calloc(1, sizeof(sparsemap_t));
  if (map) {
    map->m_data = data;
    map->m_data_used = 0;
    map->m_capacity = size;
  }
  return map;
}

/**
 * @brief Initializes a sparsemap with the provided data and size.
 *
 * This function sets up the initial state of a sparsemap by assigning the given
 * data buffer and capacity. It also clears the sparsemap to ensure it starts empty.
 *
 * @param[in] map A pointer to the sparsemap to initialize.
 * @param[in] data A pointer to the data buffer to be used by the sparsemap.
 * @param[in] size The size of the data buffer in bytes.
 */
void
sparsemap_init(sparsemap_t *map, uint8_t *data, const size_t size)
{
  map->m_data = data;
  map->m_data_used = 0;
  map->m_capacity = size;
  sparsemap_clear(map);
}

/**
 * @brief Initializes a sparse map with given data and size.
 *
 * This function sets up the sparse map by assigning the provided data array and
 * size, and calculates the initial data usage.
 *
 * @param[in,out] map The sparse map to initialize.
 * @param[in] data Pointer to the data array to be used by the sparse map.
 * @param[in] size The capacity of the data array.
 */
void
sparsemap_open(sparsemap_t *map, uint8_t *data, const size_t size)
{
  map->m_data = data;
  map->m_data_used = __sm_get_size_impl(map);
  map->m_capacity = size;
}

/**
 * @brief Sets the data size of the given sparsemap.
 *
 * This function adjusts the data size of the provided sparsemap. If the `data`
 * parameter is `NULL`, and the sparsemap was allocated using the `sparsemap()`
 * API, the sparsemap will be resized accordingly. If new data is provided, it
 * updates the sparsemap with the new data buffer. The function ensures that
 * the data is properly aligned to 8 bytes.
 *
 * @param[in,out] map The sparsemap to modify.
 * @param[in] data The new data buffer. If NULL, the sparsemap's internal data
 *                 will be resized.
 * @param[in] size The new size for the data buffer.
 * @return The updated sparsemap pointer if successful, or NULL if resizing fails.
 */
sparsemap_t *
sparsemap_set_data_size(sparsemap_t *map, uint8_t *data, const size_t size)
{
  const size_t data_size = size * sizeof(uint8_t);

  /*
   * If this sparsemap was allocated by the sparsemap() API and we're not handed
   * a new data, it's up to us to resize it.
   */
  if (data == NULL && (uintptr_t)map->m_data == (uintptr_t)map + sizeof(sparsemap_t) && size > map->m_capacity) {

    /* Ensure that m_data is 8-byte aligned. */
    size_t total_size = sizeof(sparsemap_t) + data_size;
    const size_t padding = total_size % 8 == 0 ? 0 : 8 - (total_size % 8);
    total_size += padding;

    sparsemap_t *m = realloc(map, total_size);
    if (!m) {
      return NULL;
    }
    memset((uint8_t *)m + sizeof(sparsemap_t) + (m->m_capacity * sizeof(uint8_t)), 0, size - m->m_capacity + padding);
    m->m_capacity = data_size;
    m->m_data = (uint8_t *)(((uintptr_t)m + sizeof(sparsemap_t)) & ~(uintptr_t)7);
    __sm_when_diag({ __sm_assert(IS_8_BYTE_ALIGNED(m->m_data)); }) return m;
  }
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

/**
 * @brief Calculates the remaining capacity of the sparsemap.
 *
 * This function returns the percentage of unused capacity in the sparse map.
 * If the used capacity is equal to or exceeds the total capacity, it returns 0.
 * If the total capacity is 0, it returns 100. Otherwise, it returns the
 * percentage of capacity remaining.
 *
 * @param[in] map The sparsemap for which the remaining capacity is calculated.
 * @return The percentage of remaining capacity in the sparsemap.
 */
double
sparsemap_capacity_remaining(const sparsemap_t *map)
{
  if (map->m_data_used >= map->m_capacity) {
    return 0;
  }
  if (map->m_capacity == 0) {
    return 100.0;
  }
  return (100 - (map->m_data_used / (double)map->m_capacity)) * 100;
}

/**
 * @brief Retrieves the capacity of the sparse map.
 *
 * This function returns the total capacity of the given sparse map, which is
 * the size of the underlying data structure.
 *
 * @param[in] map Pointer to the sparse map.
 * @return The capacity of the sparse map.
 */
size_t
sparsemap_get_capacity(const sparsemap_t *map)
{
  return map->m_capacity;
}

/**
 * @brief Checks if a specific bit is set in the sparse map.
 *
 * This function determines whether the bit at the given index is set in the
 * sparse map. It performs various checks and traverses to the appropriate
 * chunk to verify the bit's state.
 *
 * @param[in] map The sparse map to check.
 * @param[in] idx The index of the bit to check.
 * @return True if the bit is set, false otherwise.
 */
bool
sparsemap_is_set(sparsemap_t *map, sparsemap_idx_t idx)
{
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);

  /* Get the __sm_chunk_t which manages this index */
  const ssize_t offset = __sm_get_chunk_offset(map, idx);

  /* No __sm_chunk_t's available -> the bit is not set */
  if (offset == -1) {
    return false;
  }

  /* Otherwise load the __sm_chunk_t */
  uint8_t *p = __sm_get_chunk_data(map, offset);
  const __sm_idx_t start = *(__sm_idx_t *)p;
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

/**
 * @brief Unsets a bit at a specified index in the given sparse map.
 *
 * This function clears the bit at the given index in the sparse map. It handles
 * different scenarios, including chunks that do not exist for the specified index,
 * run-length encoded (RLE) chunks, and sparse chunks.
 *
 * The function also optionally performs chunk coalescing if the `coalesce` flag is set.
 *
 * @param[in,out] map The sparse map in which the bit needs to be unset.
 * @param[in] idx The index of the bit to be unset.
 * @param[in] coalesce A flag indicating whether to perform chunk coalescing.
 * @return The index of the bit that was unset.
 */
sparsemap_idx_t
__sm_map_unset(sparsemap_t *map, sparsemap_idx_t idx, const bool coalesce)
{
  const sparsemap_idx_t ret_idx = idx;
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);

  /* Clearing a bit could require an additional vector, let's ensure we have that
   * space available in the buffer first, or ENOMEM now. */
  SM_ENOUGH_SPACE(SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));

  /* Determine if there is a chunk that could contain this index. */
  size_t offset = __sm_get_chunk_offset(map, idx);

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
  const __sm_idx_t start = *(__sm_idx_t *)p;
  __sm_assert(start == __sm_get_chunk_aligned_offset(start));

  if (idx < start) {
    /* Our search resulted in the first chunk that starts after the index but
     * that means there is no chunk that contains this index, so again this is
     * a no-op. */
    goto done;
  }

  __sm_chunk_t chunk;
  __sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
  const size_t capacity = __sm_chunk_get_capacity(&chunk);

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
    const size_t length = __sm_chunk_rle_get_length(&chunk);
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
    offset += SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t);
    __sm_insert_data(map, offset, (uint8_t *)&vec, sizeof(__sm_bitvec_t));
    __sm_chunk_clr_bit(&chunk, idx - start, &pos);
    break;
  case SM_NEEDS_TO_SHRINK:
    /* The vector is empty, perhaps the entire chunk is empty? */
    if (__sm_chunk_is_empty(&chunk)) {
      __sm_remove_data(map, offset, SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2));
      __sm_set_chunk_count(map, __sm_get_chunk_count(map) - 1);
    } else {
      offset += SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t);
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

/**
 * @brief Unsets the value at a specific index in the sparse map.
 *
 * This function calls the internal __sm_map_unset function with the coalesce parameter
 * set to true, which removes an entry at the specified index and attempts to merge adjacent
 * segments to maintain the map's sparsity.
 *
 * @param[in] map The sparse map in which the value will be unset.
 * @param[in] idx The index at which the value will be unset.
 * @return The index that was unset.
 */
sparsemap_idx_t
sparsemap_unset(sparsemap_t *map, const sparsemap_idx_t idx)
{
  return __sm_map_unset(map, idx, true);
}

/**
 * @brief Sets a bit in a chunk within the sparse map and manages chunk resizing.
 *
 * This function sets a bit in the chunk of a sparse map corresponding to the
 * given index. It handles the initialization, setting the bit, and necessary
 * memory adjustments for growing or shrinking chunks, including allocation and
 * deallocation of bit vectors.
 *
 * @param[in,out] map The sparse map where the bit will be set.
 * @param[in] idx The index within the sparse map where the bit will be set.
 * @param[in] p A pointer to the chunk data within the sparse map.
 * @param[in] offset The offset within the sparse map's data where the chunk is located.
 * @param[in] v A bit vector, when non-NULL, indicates that a new chunk has been added.
 *
 * @return The index at which the bit was set.
 */
static sparsemap_idx_t
__sparsemap_set(sparsemap_t *map, const sparsemap_idx_t idx, uint8_t *p, size_t offset, const __sm_bitvec_t *v)
{
  /*
   * When v is non-NULL we've just added a new chunk, and we knew in advance that a
   * new chunk would result in an SM_PAYLOAD_MIXED which in turn requires space to
   * store the bit pattern, so given that we allocated the space ahead of time we
   * don't need to allocate it now.
   */
  size_t pos = v ? -1 : 0;
  __sm_chunk_t chunk;
  const __sm_idx_t start = *(__sm_idx_t *)p;

  __sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
  __sm_assert(__sm_chunk_is_rle(&chunk) == false);

  switch (__sm_chunk_set_bit(&chunk, idx - start, &pos)) {
  case SM_OK:
    break;
  case SM_NEEDS_TO_GROW:
    if (!v) {
      __sm_bitvec_t vec = 0;
      offset += SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t);
      __sm_insert_data(map, offset, (uint8_t *)&vec, sizeof(__sm_bitvec_t));
      pos = -1;
    }
    __sm_chunk_set_bit(&chunk, idx - start, &pos);
    break;
  case SM_NEEDS_TO_SHRINK:
    /* The vector is empty, perhaps the entire chunk is empty? */
    if (__sm_chunk_is_empty(&chunk)) {
      __sm_remove_data(map, offset, SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2));
      __sm_set_chunk_count(map, __sm_get_chunk_count(map) - 1);
    } else {
      offset += SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t);
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

/**
 * @brief Sets a bit in the sparse bit map.
 *
 * This function sets a bit at the given index in the provided sparse bit map.
 * It performs various internal checks and operations to ensure the data integrity of the map,
 * including initializing, inserting new chunks, and transitioning chunk states when necessary.
 *
 * @param[in,out] map The sparse bit map to be modified.
 * @param[in] idx The index of the bit to set.
 * @param[in] coalesce A flag indicating whether to attempt chunk coalescing.
 * @return Returns the adjusted index within the sparse bit map or the given index.
 */
sparsemap_idx_t
__sm_map_set(sparsemap_t *map, sparsemap_idx_t idx, const bool coalesce)
{
  __sm_chunk_t chunk;
  sparsemap_idx_t ret_idx = idx;
  __sm_idx_t start;
  uint8_t *p;
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);

  /*
   * Setting a bit could require an additional vector, let's ensure we have that
   * space available in the buffer first, or ENOMEM now.
   */
  SM_ENOUGH_SPACE(SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));

  /* Determine if there is a chunk that could contain this index. */
  size_t offset = __sm_get_chunk_offset(map, idx);

  if ((ssize_t)offset == -1) {
    /*
     * No chunks exist, the map is empty, so we must append a new chunk to the
     * end of the buffer and initialize it so that it can contain this index.
     */
    const uint8_t buf[SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2)] = { 0 };
    __sm_append_data(map, &buf[0], sizeof(buf));
    p = __sm_get_chunk_data(map, 0);
    *(__sm_idx_t *)p = __sm_get_chunk_aligned_offset(idx);
    __sm_set_chunk_count(map, 1);

    const __sm_bitvec_t *v = (__sm_bitvec_t *)((uintptr_t)p + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
    ret_idx = __sparsemap_set(map, idx, p, 0, v);

    __sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
    start = *(__sm_idx_t *)p;
    offset = 0;
    goto done;
  }

  /*
   * Try to locate a chunk for this idx.  We could find that:
   *  - the first chunk's offset is greater than the index, or
   *  - the index is beyond the end of the last chunk, or
   *  - we found a chunk that can contain this index.
   */
  p = __sm_get_chunk_data(map, offset);
  start = *(__sm_idx_t *)p;
  __sm_assert(start == __sm_get_chunk_aligned_offset(start));

  if (idx < start) {
    /*
     * Our search resulted in the first chunk, but it starts after the index,
     * so that means there is no chunk that can contain this index.  We need
     * to insert a new chunk before this one and initialize it so that it can
     * contain this index.
     */
    const uint8_t buf[SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2)] = { 0 };
    __sm_insert_data(map, offset, &buf[0], sizeof(buf));
    __sm_set_chunk_count(map, __sm_get_chunk_count(map) + 1);

    /* NOTE: insert moves the memory over meaning `p` is now the new chunk */
    *(__sm_idx_t *)p = __sm_get_chunk_aligned_offset(idx);
    __sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);

    const __sm_bitvec_t *v = (__sm_bitvec_t *)((uintptr_t)p + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
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

  /* is this an RLE chunk */
  if (__sm_chunk_is_rle(&chunk)) {
    const size_t length = __sm_chunk_rle_get_length(&chunk);

    /* Is the index within its range, or at the end? */
    if (idx >= start && idx - start < capacity) {
      /*
       * This RLE contains the bits in [start, start + length] so the index of
       * the last bit in this RLE chunk is `start + length - 1` which is why
       * we test index (0-based) against current length (1-based) below.
       */
      if (idx - start == length) {
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
    const uint8_t buf[SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2)] = { 0 };
    const size_t size = __sm_chunk_get_size(&chunk);
    offset += SM_SIZEOF_OVERHEAD + size;
    p += SM_SIZEOF_OVERHEAD + size;
    __sm_insert_data(map, offset, &buf[0], sizeof(buf));

    start += __sm_chunk_get_capacity(&chunk);
    if (start + SM_CHUNK_MAX_CAPACITY <= idx) {
      start = __sm_get_chunk_aligned_offset(idx);
    }
    *(__sm_idx_t *)p = start;
    __sm_assert(start == __sm_get_chunk_aligned_offset(start));
    __sm_set_chunk_count(map, __sm_get_chunk_count(map) + 1);

    const __sm_bitvec_t *v = (__sm_bitvec_t *)((uintptr_t)p + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
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

/**
 * @brief Sets the specified index in the sparsemap.
 *
 * This function marks the given index in the sparsemap as set.
 * Internally, it calls the __sm_map_set function with coalesce set to true.
 *
 * @param[in] map The sparsemap to modify.
 * @param[in] idx The index to set in the sparsemap.
 * @return The index that was set in the sparsemap.
 */
sparsemap_idx_t
sparsemap_set(sparsemap_t *map, const sparsemap_idx_t idx)
{
  return __sm_map_set(map, idx, true);
}

/**
 * @brief Sets or unsets a value in the sparse map at the specified index.
 *
 * This function assigns a value to the sparse map at the given index.
 * It either sets or unsets (clears) the bit at the index based on
 * the provided boolean value.
 *
 * @param[in,out] map Pointer to the sparsemap structure.
 * @param[in] idx The index at which the value should be assigned.
 * @param[in] value Boolean value indicating whether to set (true) or unset (false) the bit.
 * @return The index at which the operation was performed.
 */
sparsemap_idx_t
sparsemap_assign(sparsemap_t *map, const sparsemap_idx_t idx, const bool value)
{
  return value ? sparsemap_set(map, idx) : sparsemap_unset(map, idx);
}

/**
 * @brief Retrieves the starting offset in a sparse map.
 *
 * This function determines the starting offset of a sparse map by analyzing
 * the chunks within the map. It iterates over the chunk data to find the first
 * payload of interest, either `ones` or `mixed`, and returns the corresponding
 * offset. If the chunk is run-length encoded (RLE), it shortcuts to this calculation.
 *
 * @param[in] map Pointer to the sparse map to analyze.
 * @return The starting offset within the sparse map.
 */
sparsemap_idx_t
sparsemap_get_starting_offset(const sparsemap_t *map)
{
  sparsemap_idx_t offset = 0;
  const size_t count = __sm_get_chunk_count(map);
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
      const size_t flags = SM_CHUNK_GET_FLAGS(*p, n);
      if (flags == SM_PAYLOAD_NONE) {
        continue;
      } else if (flags == SM_PAYLOAD_ZEROS) {
        relative_position += SM_BITS_PER_VECTOR;
      } else if (flags == SM_PAYLOAD_ONES) {
        offset = relative_position;
        goto done;
      } else if (flags == SM_PAYLOAD_MIXED) {
        const __sm_bitvec_t w = chunk.m_data[1 + __sm_chunk_get_position(&chunk, (m * SM_FLAGS_PER_INDEX_BYTE) + n)];
        for (int k = 0; k < SM_BITS_PER_VECTOR; k++) {
          if (w & (__sm_bitvec_t)1 << k) {
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

/**
 * @brief Retrieves the ending offset of a sparse map.
 *
 * This function calculates the ending offset of a sparse map by examining
 * each chunk within the map. If the map is empty, the offset is zero. For
 * maps with chunks, it iterates over the chunks, evaluating their data and
 * calculating the final offset.
 *
 * @param[in] map Pointer to the sparse map structure.
 * @return The calculated ending offset of the map.
 */
sparsemap_idx_t
sparsemap_get_ending_offset(const sparsemap_t *map)
{
  const size_t count = __sm_get_chunk_count(map);

  /* the ending offset of a map containing zero chunks is zero */
  if (count == 0) {
    return 0;
  }

  /* the ending offset will be the last offset in the last chunk */
  uint8_t *p = __sm_get_chunk_data(map, 0);
  for (size_t i = 0; i < count - 1; i++) {
    p += SM_SIZEOF_OVERHEAD;
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);
    p += __sm_chunk_get_size(&chunk);
  }

  /* examine the last chunk in the map */
  const __sm_idx_t start = *(__sm_idx_t *)p;
  p += SM_SIZEOF_OVERHEAD;
  __sm_chunk_t chunk;
  __sm_chunk_init(&chunk, p);

  /* the ending offset of an RLE chunk is its starting offset + length */
  if (SM_IS_CHUNK_RLE(&chunk)) {
    return start + __sm_chunk_rle_get_length(&chunk) - 1;
  }

  /* the last chunk is not RLE, let's examine it further */
  sparsemap_idx_t offset = 0;
  sparsemap_idx_t relative_position = start;
  for (size_t m = 0; m < sizeof(__sm_bitvec_t); m++, p++) {
    for (int n = 0; n < SM_FLAGS_PER_INDEX_BYTE; n++) {
      const size_t flags = SM_CHUNK_GET_FLAGS(*p, n);
      switch (flags) {
      case SM_PAYLOAD_ZEROS:
        relative_position += SM_BITS_PER_VECTOR;
        break;
      case SM_PAYLOAD_ONES:
        relative_position += SM_BITS_PER_VECTOR;
        offset = relative_position;
        break;
      case SM_PAYLOAD_MIXED: {
        const __sm_bitvec_t w = chunk.m_data[1 + __sm_chunk_get_position(&chunk, (m * SM_FLAGS_PER_INDEX_BYTE) + n)];
        int idx = 0;
        for (int k = 0; k < SM_BITS_PER_VECTOR; k++) {
          if (w & (__sm_bitvec_t)1 << k) {
            idx = k;
          }
        }
        offset = relative_position + idx;
        relative_position += SM_BITS_PER_VECTOR;
        break;
      }
      case SM_PAYLOAD_NONE:
      default:
        continue;
      }
    }
  }
  return offset;
}

/**
 * @brief Calculates the fill factor of a sparse map.
 *
 * This function computes the fill factor of a sparse map by determining
 * the proportion of occupied elements relative to its total offset.
 * The fill factor is expressed as a percentage.
 *
 * @param[in] map A pointer to the sparse map.
 * @return The fill factor of the map as a percentage.
 */
double
sparsemap_fill_factor(sparsemap_t *map)
{
  const size_t rank = sparsemap_rank(map, 0, SPARSEMAP_IDX_MAX, true);
  const sparsemap_idx_t end = sparsemap_get_ending_offset(map);
  return (double)rank / (double)end * 100.0;
}

/**
 * @brief Retrieves the serialized bitmap data from a sparse map.
 *
 * This function returns a pointer to the serialized data contained within
 * a given sparse map.
 *
 * @param[in] map Pointer to the sparse map from which to retrieve the data.
 * @return Pointer to the serialized bitmap data.
 */
void *
sparsemap_get_data(const sparsemap_t *map)
{
  return map->m_data;
}

/**
 * @brief Retrieves the size of the sparse map.
 *
 * This function calculates the utilized size of the sparse map. If the stored
 * size does not match the calculated size, it updates the stored size.
 *
 * @param[in] map Pointer to the sparse map.
 * @return The size of the sparse map.
 */
size_t
sparsemap_get_size(sparsemap_t *map)
{
  if (map->m_data_used) {
    const size_t size = __sm_get_size_impl(map);
    if (size != map->m_data_used) {
      map->m_data_used = size;
    }
    __sm_when_diag({ __sm_assert(map->m_data_used == __sm_get_size_impl(map)); });
    return map->m_data_used;
  }
  return map->m_data_used = __sm_get_size_impl(map);
}

/**
 * @brief Counts the number of elements in a sparse map.
 *
 * This function returns the total count of elements stored in a given
 * sparsemap_t instance by invoking the sparsemap_rank function.
 *
 * @param[in] map A pointer to the sparsemap_t instance to be counted.
 * @return The total number of elements in the sparse map.
 */
size_t
sparsemap_count(sparsemap_t *map)
{
  return sparsemap_rank(map, 0, SPARSEMAP_IDX_MAX, true);
}

/**
 * @brief Scans through each chunk in a sparse map and applies a scanning function to each chunk.
 *
 * This function iterates over all chunks in the provided sparse map, initializing each chunk
 * and applying a user-defined scanning function to it. The scan may optionally skip a specified
 * number of elements before commencing.
 *
 * @param[in] map Pointer to the sparse map to scan.
 * @param[in] scanner User-defined scanning function to be applied to each chunk.
 * @param[in] skip Number of elements to skip before starting the scan.
 * @param[in] aux Auxiliary data to pass to the scanning function.
 */
void
sparsemap_scan(const sparsemap_t *map, void (*scanner)(__sm_idx_t[], size_t, void *aux), size_t skip, void *aux)
{
  uint8_t *p = __sm_get_chunk_data(map, 0);
  const size_t count = __sm_get_chunk_count(map);

  for (size_t i = 0; i < count; i++) {
    const __sm_idx_t start = *(__sm_idx_t *)p;
    p += SM_SIZEOF_OVERHEAD;
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);
    const size_t skipped = __sm_chunk_scan(&chunk, start, scanner, skip, aux);
    if (skip) {
      __sm_assert(skip >= skipped);
      skip -= skipped;
    }
    p += __sm_chunk_get_size(&chunk);
  }
}

/**
 * @brief Merges two sparsemaps into the destination sparsemap.
 *
 * This function integrates chunks from the source sparsemap into the
 * destination sparsemap, updating their offsets and capacity as necessary. It
 * handles cases where chunks in the source sparsemap may overlap with,
 * precede, or follow chunks in the destination sparsemap. The function
 * employs strategies similar to a merge sort to reconcile ordered sets of
 * chunks.
 *
 * The function also checks and adjusts for remaining capacity before
 * attempting the merge, ensuring that there is sufficient space in the
 * destination sparsemap to accommodate the merged data.
 *
 * @param[in,out] destination Pointer to the sparsemap that will be changed to include the data from the source.
 * @param[in] source Pointer to the sparsemap that contains data to be merged into the destination.
 * @return 0 if the merge was successful, or sets errno to ENOSPC and returns
 * the amount of additional space required to successfully merge the maps.
 */
size_t
sparsemap_merge(sparsemap_t *destination, sparsemap_t *source)
{
  size_t src_count = __sm_get_chunk_count(source);
  const sparsemap_idx_t dst_ending_offset = sparsemap_get_ending_offset(destination);

  if (src_count == 0) {
    return 0;
  }

  // TODO: rethink this method of estimating space... seems off to me now...
  const ssize_t remaining_capacity = destination->m_capacity - destination->m_data_used -
    (source->m_data_used + src_count * (SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) * 2));

  /* Estimate worst-case overhead required for merge. */
  if (remaining_capacity <= 0) {
    errno = ENOSPC;
    return -remaining_capacity;
  }

  /*
   * Strategy here it to walk the ordered set of chunks in the source map
   * examining each one against the current destination chunk.  Then there
   * are a number of cases to consider:
   * - src proceeds dst
   * - src follows dst
   * - src and dst overlap
   *   - perfect overlap
   *   - non-uniform overlap
   */
  uint8_t *src = __sm_get_chunk_data(source, 0);
  while (src_count) {
    const __sm_idx_t src_start = *(__sm_idx_t *)src;
    __sm_chunk_t src_chunk;
    __sm_chunk_init(&src_chunk, src + SM_SIZEOF_OVERHEAD);
    const bool src_is_rle = SM_IS_CHUNK_RLE(&src_chunk);
    const size_t src_capacity = __sm_chunk_get_capacity(&src_chunk);
    const ssize_t dst_offset = __sm_get_chunk_offset(destination, src_start);
    if (dst_offset >= 0) {
      uint8_t *dst = __sm_get_chunk_data(destination, dst_offset);
      const __sm_idx_t dst_start = *(__sm_idx_t *)dst;
      __sm_chunk_t dst_chunk;
      __sm_chunk_init(&dst_chunk, dst + SM_SIZEOF_OVERHEAD);
      const bool dst_is_rle = SM_IS_CHUNK_RLE(&dst_chunk);
      size_t dst_capacity = __sm_chunk_get_capacity(&dst_chunk);

      /* Try to expand the capacity if there's room before the start of the next chunk. */
      if (!(src_is_rle || dst_is_rle)) {
        if (src_start == dst_start && dst_capacity < src_capacity) {
          const ssize_t nxt_offset = __sm_get_chunk_offset(destination, dst_start + dst_capacity + 1);
          uint8_t *nxt_dst = __sm_get_chunk_data(destination, nxt_offset);
          const __sm_idx_t nxt_dst_start = *(__sm_idx_t *)nxt_dst;
          if (nxt_dst_start > dst_start + src_capacity) {
            __sm_chunk_increase_capacity(&dst_chunk, src_capacity);
            dst_capacity = __sm_chunk_get_capacity(&dst_chunk);
          }
        }
      }

      /* Source chunk (sparse/RLE) precedes next destination chunk. */
      if (src_start + src_capacity <= dst_start) {
        const size_t src_size = __sm_chunk_get_size(&src_chunk);
        const ssize_t offset = __sm_get_chunk_offset(destination, dst_start);
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
        const size_t src_size = __sm_chunk_get_size(&src_chunk);
        /* Insert or append a copy of the src chunk in dst. */
        if (dst_offset == __sm_get_chunk_offset(destination, SPARSEMAP_IDX_MAX)) {
          __sm_append_data(destination, src, SM_SIZEOF_OVERHEAD + src_size);
        } else {
          const ssize_t offset = __sm_get_chunk_offset(destination, src_start);
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
      const size_t src_length = __sm_chunk_rle_get_length(&src_chunk);
      const size_t dst_length = __sm_chunk_rle_get_length(&dst_chunk);

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
  int moved = 0;
  size_t i;
  const size_t count = __sm_get_chunk_count(map);
  bool in_middle = false;

  __sm_assert(sparsemap_count(other) == 0);

  //GSB __sm_when_diag({ __sm_diag_map(map, "========== START: %lu", idx); });

  /*
   * According to the API when idx is SPARSEMAP_IDX_MAX the client is
   * requesting that we divide the bits in two equal portions, so we
   * calculate that index here.
   */
  if (idx == SPARSEMAP_IDX_MAX) {
    const sparsemap_idx_t begin = sparsemap_get_starting_offset(map);
    const sparsemap_idx_t end = sparsemap_get_ending_offset(map);
    if (begin != end) {
      const size_t rank = sparsemap_rank(map, begin, end, true);
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
  uint8_t *src = __sm_get_chunk_data(map, 0);
  uint8_t *dst = __sm_get_chunk_end(other);

  /* (1): skip over chunks that are entirely to the left. */
  uint8_t *prev = src;
  for (i = 0; i < count; i++) {
    const __sm_idx_t start = *(__sm_idx_t *)src;
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
      uint8_t buf[(SM_SIZEOF_OVERHEAD * (unsigned long)3) + (sizeof(__sm_bitvec_t) * 6)] = { 0 };

      /* Copy the source chunk into the buffer. */
      memcpy(buf + SM_SIZEOF_OVERHEAD, src, SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
      /* Set the number of chunks to 1 in our stunt map. */
      buf[0] = (uint32_t)1;
      /* And initialize the stunt double chunk we need to split. */
      sparsemap_open(&stunt, buf, (SM_SIZEOF_OVERHEAD * (unsigned long)3) + (sizeof(__sm_bitvec_t) * 6));
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

      //GSB __sm_when_diag({ __sm_diag_map(map, "========== PREPARED:"); });
      return sparsemap_split(map, idx, other);
    }

    /*
     * (3) We're in the middle of a sparse chunk, let's split it.
     */

    /* Zero out the space we'll need at the proper location in dst. */
    uint8_t buf[SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2)] = { 0 };
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

  // GSB__sm_when_diag({
  //  __sm_diag_map(map, "SRC");
  //  __sm_diag_map(other, "DST");
  //});

  return idx;
}

sparsemap_idx_t
sparsemap_select(sparsemap_t *map, sparsemap_idx_t n, bool value)
{
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);
  const size_t count = __sm_get_chunk_count(map);

  if (count == 0 && value == false) {
    return n;
  }

  uint8_t *p = __sm_get_chunk_data(map, 0);

  for (size_t i = 0; i < count; i++) {
    const __sm_idx_t start = *(__sm_idx_t *)p;
    /* Start of this chunk is greater than n meaning there are a set of 0s
     * before the first 1 sufficient to consume n. */
    if (value == false && i == 0 && start > n) {
      return n;
    }
    p += SM_SIZEOF_OVERHEAD;
    __sm_chunk_t chunk;
    __sm_chunk_init(&chunk, p);

    ssize_t new_n = n;
    const size_t index = __sm_chunk_select(&chunk, n, &new_n, value);
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
  (void)vec; /* unused parameter */
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);
  size_t gap, pos = 0, result = 0, prev = 0, len = end - begin + 1;

  if (begin > end) {
    return 0;
  }

  if (begin == end) {
    return sparsemap_is_set(map, begin) == value ? 1 : 0;
  }

  const size_t count = __sm_get_chunk_count(map);

  if (count == 0) {
    if (value == false) {
      /* The count/rank of unset bits in an empty map is inf, so what you requested is the answer. */
      return len;
    }
  }

  uint8_t *p = __sm_get_chunk_data(map, 0);
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
    const size_t amt = __sm_chunk_rank(&rank, value, &chunk, begin, end - start);
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
  __sm_bitvec_t vec = 0;

  /* When skipping forward to `idx` offset in the map we can determine how
   * many selects we can avoid by taking the rank of the range and starting
   * at that bit. */
  size_t nth = (idx == 0) ? 0 : sparsemap_rank(map, 0, idx - 1, value);
  if (SPARSEMAP_NOT_FOUND(nth)) {
    return nth;
  }
  /* Find the first bit that matches value, then... */
  sparsemap_idx_t offset = sparsemap_select(map, nth, value);
  do {
    /* See if the rank of the bits in the range starting at offset is equal
     * to the desired amount. */
    size_t rank = (len == 1) ? 1 : __sm_rank_vec(map, offset, offset + len - 1, value, &vec);
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
      const int max = len > SM_BITS_PER_VECTOR ? SM_BITS_PER_VECTOR : len;
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

#if 0
static double
_tst_pow(const double base, const int exponent)
{
  if (exponent == 0) {
    return 1.0; // 0^0 is 1
  }
  if (base == 0.0) {
    return 0.0; // 0 raised to any positive exponent is 0 (except 0^0)
  }
  if (base < 0.0 && (exponent & 1) != 0) {
    // negative base with odd exponent, results in a negative
    return -_tst_pow(-base, exponent);
  }

  double result = base;
  for (unsigned int i = 1; i < exponent; i++) {
    result *= base;
  }
  return result;
}
#endif

static char *
_qcc_format_chunk(const __sm_idx_t start, const __sm_chunk_t *chunk, const bool none)
{
  size_t amt = sizeof(wchar_t) * (SM_FLAGS_PER_INDEX * 16 + SM_BITS_PER_VECTOR * 64 + 16) * 2;
  char *buf = malloc(amt);

  const __sm_bitvec_t desc = chunk->m_data[0];

  if (!__sm_chunk_is_rle(chunk)) {
    char desc_str[(2 * SM_FLAGS_PER_INDEX + 1) * sizeof(wchar_t)] = { 0 };
    char *str = desc_str;
    int mixed = 0;
    for (int i = 1; i <= SM_FLAGS_PER_INDEX; i++) {
      const uint8_t flag = SM_CHUNK_GET_FLAGS(desc, i);
      switch (flag) {
      case SM_PAYLOAD_NONE:
        if (!none) {
          __sm_assert(flag == SM_PAYLOAD_NONE);
        }
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
      if (i % 8 == 0 && i < 32) {
        str += sprintf(str, " ");
      }
    }
    str = buf + sprintf(buf, "%.10u\t|%s|%s", start, desc_str, mixed ? " :: " : "");
    for (int i = 0; i < mixed; i++) {
      const size_t n = snprintf(str, amt - 1, "%#018" PRIx64 "%s", chunk->m_data[1 + i], i + 1 < mixed ? " " : "");
      str += n;
      amt -= n;
    }
  } else {
    const size_t len = __sm_chunk_rle_get_length(chunk);
    const size_t cap = __sm_chunk_rle_get_capacity(chunk);
    sprintf(buf, "%.10u\t[%u, %zu) %zu of %zu", start, start, start + len - 1, len, cap);
  }
  return buf;
}

char *
QCC_showChunk(void *value, int len)
{
  const __sm_idx_t start = *(__sm_idx_t *)value;
  __sm_chunk_t chunk;
  __sm_chunk_init(&chunk, value + SM_SIZEOF_OVERHEAD);

  return _qcc_format_chunk(start, &chunk, false);
}

char *
QCC_showSparsemap(void *value, int len)
{
  char *buf = NULL;
  const sparsemap_t *map = (sparsemap_t *)value;
  const size_t count = __sm_get_chunk_count(map);

  if (count > 0) {
    char *str = NULL;
    uint8_t *p = __sm_get_chunk_data(map, 0);
    for (size_t i = 0; i < count; i++) {
      __sm_chunk_t chunk;
      const __sm_idx_t start = *(__sm_idx_t *)p;
      __sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
      char *c = _qcc_format_chunk(start, &chunk, true);
      if (buf) {
        char *new_buf = realloc(buf, strlen(buf) + strlen(c) + 24);
        if (new_buf) {
          buf = new_buf;
          str += sprintf(str, "\n%s", c);
        }
      } else {
        buf = c;
        str = buf + strlen(c);
      }
      p += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
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
  if ((double)random() / (double)RAND_MAX > 0.5) {
    // Generate a run-length encoded (RLE) chunk:
    const sparsemap_idx_t from = 1, to = SM_CHUNK_RLE_MAX_LENGTH;
    const unsigned int len = ((unsigned int)random() % (to - from)) + from;
    // First allocate enough room for the chunk data ...
    uint8_t *p = malloc(SM_SIZEOF_OVERHEAD + sizeof(__sm_chunk_t) + (sizeof(__sm_bitvec_t) * 2));
    // ... then set the offset to the length so we can test for that later ...
    *(__sm_idx_t *)p = len;
    // ... next is the chunk begins after the offset ...
    __sm_chunk_t *chunk = (__sm_chunk_t *)((uintptr_t)p + SM_SIZEOF_OVERHEAD);
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
  }
  // Generate a chunk with the offset equal to the number of additional
  // vectors (len) and a descriptor that matches that with random data.
  const unsigned int from = 0, to = SM_FLAGS_PER_INDEX;
  const unsigned int len = ((unsigned int)random() % (to - from)) + from;
  const unsigned int cut = ((unsigned int)random() % ((SM_FLAGS_PER_INDEX - len) - from)) + from;
  // First allocate enough room for the chunk data ...
  uint8_t *p = malloc(SM_SIZEOF_OVERHEAD + sizeof(__sm_chunk_t) + (sizeof(__sm_bitvec_t) * (len + 1)));
  // ... then set the offset to the capacity ...
  *(__sm_idx_t *)p = SM_CHUNK_MAX_CAPACITY - (cut * SM_BITS_PER_VECTOR);
  // ... next is the chunk begins after the offset ...
  __sm_chunk_t *chunk = (__sm_chunk_t *)((uintptr_t)p + SM_SIZEOF_OVERHEAD);
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
    const double coin = (double)random() / (double)RAND_MAX;
    if (SM_CHUNK_GET_FLAGS(*desc, i) != SM_PAYLOAD_MIXED && coin >= 0.5) {
      SM_CHUNK_SET_FLAGS(*desc, i, SM_PAYLOAD_ONES);
    }
  }
  // ... shuffle those around ...
  for (size_t i = 0; i < SM_FLAGS_PER_INDEX - cut - 1; i++) {
    const size_t j = ((size_t)random() % SM_FLAGS_PER_INDEX) - cut - i + i;
    const int flags = SM_CHUNK_GET_FLAGS(*desc, j);
    SM_CHUNK_SET_FLAGS(*desc, j, SM_CHUNK_GET_FLAGS(*desc, i));
    SM_CHUNK_SET_FLAGS(*desc, i, flags);
  }
  // ... reduce the capacity by setting trailing flags to SM_PAYLOAD_NONE ...
  *desc <<= cut * 2;
  for (int i = 0; i < cut; i++) {
    SM_CHUNK_SET_FLAGS(*desc, i, SM_PAYLOAD_NONE);
  }
  // fprintf(stdout, "\n%s\n", QCC_showChunk(p, 0));
  // ... and check that our franken-chunk appears to be correct.
  __sm_assert(__sm_chunk_is_rle(chunk) == false);
  return QCC_initGenValue(p, 1, QCC_showChunk, QCC_freeChunkValue);
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
  uint8_t *p = QCC_getValue(vals, 0, void *);
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
  const unsigned int idx = *QCC_getValue(vals, 0, unsigned int *);
  sparsemap_t *map = QCC_getValue(vals, 1, sparsemap_t *);
  const unsigned int max_offset = (SM_FLAGS_PER_INDEX - 1) * sizeof(__sm_bitvec_t);
  const unsigned int rnd_offset = (idx % max_offset) - (idx % max_offset % sizeof(__sm_bitvec_t));
  const unsigned int rnd_nvec = rnd_offset / sizeof(__sm_bitvec_t);
  const __sm_idx_t offset = __sm_get_chunk_aligned_offset(idx);

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
