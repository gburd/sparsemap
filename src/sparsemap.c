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

#include <assert.h>
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
#include <stdarg.h>
#define __sm_diag(format, ...) __sm_diag_(__FILE__, __LINE__, __func__, format, ##__VA_ARGS__)
#pragma GCC diagnostic pop
void __attribute__((format(printf, 4, 5))) __sm_diag_(const char *file, int line, const char *func, const char *format, ...)
{
  va_list args;
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

enum __SM_CHUNK_INFO {
  /* metadata overhead: 4 bytes for __sm_chunk_t count */
  SM_SIZEOF_OVERHEAD = sizeof(uint32_t),

  /* number of bits that can be stored in a sm_bitvec_t */
  SM_BITS_PER_VECTOR = (sizeof(sm_bitvec_t) * 8),

  /* number of flags that can be stored in a single index byte */
  SM_FLAGS_PER_INDEX_BYTE = 4,

  /* number of flags that can be stored in the index */
  SM_FLAGS_PER_INDEX = (sizeof(sm_bitvec_t) * SM_FLAGS_PER_INDEX_BYTE),

  /* maximum capacity of a __sm_chunk (in bits) */
  SM_CHUNK_MAX_CAPACITY = (SM_BITS_PER_VECTOR * SM_FLAGS_PER_INDEX),

  /* sm_bitvec_t payload is all zeros (2#00) */
  SM_PAYLOAD_ZEROS = 0,

  /* sm_bitvec_t payload is all ones (2#11) */
  SM_PAYLOAD_ONES = 3,

  /* sm_bitvec_t payload is mixed (2#10) */
  SM_PAYLOAD_MIXED = 2,

  /* sm_bitvec_t is not used (2#01) */
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

#define SM_CHUNK_GET_FLAGS(from, at) (((from)) & ((sm_bitvec_t)SM_FLAG_MASK << ((at)*2))) >> ((at)*2)

typedef struct {
  sm_bitvec_t *m_data;
} __sm_chunk_t;

struct __attribute__((aligned(8))) sparsemap {
  size_t m_capacity;  /* The total size of m_data */
  size_t m_data_used; /* The used size of m_data */
  uint8_t *m_data;    /* The serialized bitmap data */
};

/**
 * Calculates the number of sm_bitvec_ts required by a single byte with flags
 * (in m_data[0]).
 */
static size_t
__sm_chunk_map_calc_vector_size(uint8_t b)
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

/** @brief Returns the position of a sm_bitvec_t in m_data.
 *
 * Each chunk has a set of bitvec that are sometimes abbreviated due to
 * compression (e.g. when a bitvec is all zeros or ones there is no need
 * to store anything, so no wasted space).
 *
 * @param[in] map The chunk in question.
 * @param[in] bv The index of the vector to find in the chunk.
 * @returns the position of a sm_bitvec_t in m_data
 */
static size_t
__sm_chunk_map_get_position(__sm_chunk_t *map, size_t bv)
{
  /* Handle 4 indices (1 byte) at a time. */
  size_t num_bytes = bv / ((size_t)SM_FLAGS_PER_INDEX_BYTE * SM_BITS_PER_VECTOR);

  size_t position = 0;
  register uint8_t *p = (uint8_t *)map->m_data;
  for (size_t i = 0; i < num_bytes; i++, p++) {
    position += __sm_chunk_map_calc_vector_size(*p);
  }

  bv -= num_bytes * SM_FLAGS_PER_INDEX_BYTE;
  for (size_t i = 0; i < bv; i++) {
    size_t flags = SM_CHUNK_GET_FLAGS(*map->m_data, i);
    if (flags == SM_PAYLOAD_MIXED) {
      position++;
    }
  }

  return position;
}

/** @brief Initialize __sm_chunk_t with provided data.
 *
 * @param[in] map The chunk in question.
 * @param[in] data The memory to use within this chunk.
 */
static inline void
__sm_chunk_map_init(__sm_chunk_t *map, uint8_t *data)
{
  map->m_data = (sm_bitvec_t *)data;
}

/** @brief Examines the chunk to determine its current capacity.
 *
 * @param[in] map The chunk in question.
 * @returns the maximum capacity in bytes of this __sm_chunk_t
 */
static size_t
__sm_chunk_map_get_capacity(__sm_chunk_t *map)
{
  size_t capacity = SM_CHUNK_MAX_CAPACITY;

  register uint8_t *p = (uint8_t *)map->m_data;
  for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
    if (!*p) {
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

/** @brief Sets the capacity of this chunk.
 *
 * @param[in] map The chunk in question.
 * @param[in] capacity The new capacity in bytes to assign to the chunk,
 * must be less than SM_CHUNK_MAX_CAPACITY.
 */
static void
__sm_chunk_map_set_capacity(__sm_chunk_t *map, size_t capacity)
{
  if (capacity >= SM_CHUNK_MAX_CAPACITY) {
    return;
  }

  __sm_assert(capacity % SM_BITS_PER_VECTOR == 0);

  size_t reduced = 0;
  register uint8_t *p = (uint8_t *)map->m_data;
  for (ssize_t i = sizeof(sm_bitvec_t) - 1; i >= 0; i--) { // TODO:
    for (int j = SM_FLAGS_PER_INDEX_BYTE - 1; j >= 0; j--) {
      p[i] &= ~((sm_bitvec_t)SM_PAYLOAD_ONES << (j * 2));
      p[i] |= ((sm_bitvec_t)SM_PAYLOAD_NONE << (j * 2));
      reduced += SM_BITS_PER_VECTOR;
      if (capacity + reduced == SM_CHUNK_MAX_CAPACITY) {
        __sm_assert(__sm_chunk_map_get_capacity(map) == capacity);
        return;
      }
    }
  }
  __sm_assert(__sm_chunk_map_get_capacity(map) == capacity);
}

/** @brief Examines the chunk to determine if it is empty.
 *
 * @param[in] map The chunk in question.
 * @returns true if this __sm_chunk_t is empty
 */
static bool
__sm_chunk_map_is_empty(__sm_chunk_t *map)
{
  /* The __sm_chunk_t is empty if all flags (in m_data[0]) are zero. */
  if (map->m_data[0] == 0) {
    return true;
  }

  /* It's also empty if all flags are Zero or None. */
  register uint8_t *p = (uint8_t *)map->m_data;
  for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
    if (*p) {
      for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
        size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
        if (flags != SM_PAYLOAD_NONE && flags != SM_PAYLOAD_ZEROS) {
          return false;
        }
      }
    }
  }
  return true;
}

/** @brief Examines the chunk to determine its size.
 *
 * @param[in] map The chunk in question.
 * @returns the size of the data buffer, in bytes.
 */
static size_t
__sm_chunk_map_get_size(__sm_chunk_t *map)
{
  /* At least one sm_bitvec_t is required for the flags (m_data[0]) */
  size_t size = sizeof(sm_bitvec_t);
  /* Use a lookup table for each byte of the flags */
  register uint8_t *p = (uint8_t *)map->m_data;
  for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
    size += sizeof(sm_bitvec_t) * __sm_chunk_map_calc_vector_size(*p);
  }

  return size;
}

/** @brief Examines the chunk at \b idx to determine that bit's state (set,
 * or unset).
 *
 * @param[in] map The chunk in question.
 * @param[in] idx The 0-based index into this chunk to examine.
 * @returns the value of a bit at index \b idx
 */
static bool
__sm_chunk_map_is_set(__sm_chunk_t *map, size_t idx)
{
  /* in which sm_bitvec_t is |idx| stored? */
  size_t bv = idx / SM_BITS_PER_VECTOR;
  __sm_assert(bv < SM_FLAGS_PER_INDEX);

  /* now retrieve the flags of that sm_bitvec_t */
  size_t flags = SM_CHUNK_GET_FLAGS(*map->m_data, bv);
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

  /* get the sm_bitvec_t at |bv| */
  sm_bitvec_t w = map->m_data[1 + __sm_chunk_map_get_position(map, bv)];
  /* and finally check the bit in that sm_bitvec_t */
  return (w & ((sm_bitvec_t)1 << (idx % SM_BITS_PER_VECTOR))) > 0;
}

/** @brief Assigns a state to a bit in the chunk (set or unset).
 *
 * Sets the value of a bit at index \b idx. Then updates position \b pos to the
 * position of the sm_bitvec_t which is inserted/deleted and \b fill - the value
 * of the fill word (used when growing).
 *
 * @param[in] map The chunk in question.
 * @param[in] idx The 0-based index into this chunk to mutate.
 * @param[in] value The new state for the \b idx'th bit.
 * @param[in,out] pos The position of the sm_bitvec_t inserted/deleted within the chunk.
 * @param[in,out] fill The value of the fill word (when growing).
 * @param[in] retired When not retried, grow the map by a bitvec.
 * @returns \b SM_NEEDS_TO_GROW, \b SM_NEEDS_TO_SHRINK, or \b SM_OK
 * @note, the caller MUST to perform the relevant actions and call set() again,
 * this time with \b retried = true.
 */
static int
__sm_chunk_map_set(__sm_chunk_t *map, size_t idx, bool value, size_t *pos, sm_bitvec_t *fill, bool retried)
{
  /* In which sm_bitvec_t is |idx| stored? */
  size_t bv = idx / SM_BITS_PER_VECTOR;
  __sm_assert(bv < SM_FLAGS_PER_INDEX);

  /* Now retrieve the flags of that sm_bitvec_t. */
  size_t flags = SM_CHUNK_GET_FLAGS(*map->m_data, bv);
  assert(flags != SM_PAYLOAD_NONE);
  if (flags == SM_PAYLOAD_ZEROS) {
    /* Easy - set bit to 0 in a sm_bitvec_t of zeroes. */
    if (value == false) {
      *pos = 0;
      *fill = 0;
      return SM_OK;
    }
    /* The sparsemap must grow this __sm_chunk_t by one additional sm_bitvec_t,
       then try again. */
    if (!retried) {
      *pos = 1 + __sm_chunk_map_get_position(map, bv);
      *fill = 0;
      return SM_NEEDS_TO_GROW;
    }
    /* New flags are 2#10 meaning SM_PAYLOAD_MIXED. Currently, flags are set
       to 2#00, so 2#00 | 2#10 = 2#10. */
    *map->m_data |= ((sm_bitvec_t)SM_PAYLOAD_MIXED << (bv * 2));
    /* FALLTHROUGH */
  } else if (flags == SM_PAYLOAD_ONES) {
    /* Easy - set bit to 1 in a sm_bitvec_t of ones. */
    if (value == true) {
      *pos = 0;
      *fill = 0;
      return SM_OK;
    }
    /* The sparsemap must grow this __sm_chunk_t by one additional sm_bitvec_t,
       then try again. */
    if (!retried) {
      *pos = 1 + __sm_chunk_map_get_position(map, bv);
      *fill = (sm_bitvec_t)-1;
      return SM_NEEDS_TO_GROW;
    }
    /* New flags are 2#10 meaning SM_PAYLOAD_MIXED. Currently, flags are
       set to 2#11, so 2#11 ^ 2#01 = 2#10. */
    map->m_data[0] ^= ((sm_bitvec_t)SM_PAYLOAD_NONE << (bv * 2));
    /* FALLTHROUGH */
  }

  /* Now flip the bit. */
  size_t position = 1 + __sm_chunk_map_get_position(map, bv);
  sm_bitvec_t w = map->m_data[position];
  if (value) {
    w |= (sm_bitvec_t)1 << (idx % SM_BITS_PER_VECTOR);
  } else {
    w &= ~((sm_bitvec_t)1 << (idx % SM_BITS_PER_VECTOR));
  }

  /* If this sm_bitvec_t is now all zeroes or ones then we can remove it. */
  if (w == 0) {
    map->m_data[0] &= ~((sm_bitvec_t)SM_PAYLOAD_ONES << (bv * 2));
    *pos = position;
    *fill = 0;
    return SM_NEEDS_TO_SHRINK;
  }
  if (w == (sm_bitvec_t)-1) {
    map->m_data[0] |= (sm_bitvec_t)SM_PAYLOAD_ONES << (bv * 2);
    *pos = position;
    *fill = 0;
    return SM_NEEDS_TO_SHRINK;
  }

  map->m_data[position] = w;
  *pos = 0;
  *fill = 0;
  return SM_OK;
}

/** @brief Finds the index of the \b n'th bit after \b offset bits with \b value.
 *
 * Scans the chunk \b map until after \b offset bits (of any value) have
 * passed and then begins counting the bits that match \b value looking
 * for the \b n'th bit.  It may not be in this chunk, when it is offset is
 *
 * @param[in] map The chunk in question.
 * @param[in] value Informs what we're seeking, a set or unset bit's position.
 * @param offset[in,out] Sets \b offset to 0 if the n'th bit was found
 * in this __sm_chunk_t, or reduced value of \b n bits observed the search up
 * to a maximum of SM_BITS_PER_VECTOR.
 * @returns the 0-based index of the n'th set bit when found, otherwise
 * SM_BITS_PER_VECTOR
 */
static size_t
__sm_chunk_map_select(__sm_chunk_t *map, size_t n, ssize_t *offset, bool value)
{
  size_t ret = 0;
  register uint8_t *p;

  p = (uint8_t *)map->m_data;
  for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
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
        sm_bitvec_t w = map->m_data[1 + __sm_chunk_map_get_position(map, i * SM_FLAGS_PER_INDEX_BYTE + j)];
        for (int k = 0; k < SM_BITS_PER_VECTOR; k++) {
          if (value) {
            if (w & ((sm_bitvec_t)1 << k)) {
              if (n == 0) {
                *offset = -1;
                return ret;
              }
              n--;
            }
            ret++;
          } else {
            if (!(w & ((sm_bitvec_t)1 << k))) {
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

/** @brief Counts the bits matching \b value in the range [0, \b idx]
 * inclusive after ignoring the first \b offset bits in the chunk.
 *
 * Scans the chunk \b map until after \b offset bits (of any value) have
 * passed and then begins counting the bits that match \b value. The
 * result should never be greater than \b idx + 1 maxing out at
 * SM_BITS_PER_VECTOR.  A range of [0, 0] will count 1 bit at \b offset
 * + 1 in this chunk.  A range of [0, 9] will count 10 bits, starting
 * with the 0th and ending with the 9th and return at most a count of
 * 10.
 *
 * @param[in] map The chunk in question.
 * @param offset[in,out] Decreases \b offset by the number of bits ignored,
 * at most by SM_BITS_PER_VECTOR.
 * @param[in] idx The ending value of the range (inclusive) to count.
 * @param[out] pos The position of the last bit examined in this chunk, always
 * <= SM_BITS_PER_VECTOR, used when counting unset bits that fall within this
 * chunk's range but after the last set bit.
 * @param[out] vec The last sm_bitvec_t, masked and shifted, so as to be able
 * to examine the bits used in the last portion of the ranking as a way to
 * skip forward during a #span() operation.
 * @param[in] value Informs what we're seeking, set or unset bits.
 * @returns the count of the bits matching \b value within the range.
 */
static size_t
__sm_chunk_map_rank(__sm_chunk_t *map, size_t *offset, size_t idx, size_t *pos, sm_bitvec_t *vec, bool value)
{
  size_t ret = 0;

  *pos = 0;

  /* A chunk can only hold at most SM_CHUNK_MAX_CAPACITY bits, so if the
     offset is larger than that, we're basically done. */
  if (*offset > SM_CHUNK_MAX_CAPACITY) {
    *pos = SM_CHUNK_MAX_CAPACITY;
    *offset -= SM_CHUNK_MAX_CAPACITY;
    return 0;
  }

  register uint8_t *p = (uint8_t *)map->m_data;
  for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        continue;
      }
      if (flags == SM_PAYLOAD_ZEROS) {
        *vec = 0;
        if (idx >= SM_BITS_PER_VECTOR) {
          *pos += SM_BITS_PER_VECTOR;
          idx -= SM_BITS_PER_VECTOR;
          if (*offset > SM_BITS_PER_VECTOR) {
            *offset = *offset - SM_BITS_PER_VECTOR;
          } else {
            if (value == false) {
              ret += SM_BITS_PER_VECTOR - *offset;
            }
            *offset = 0;
          }
        } else {
          *pos += idx + 1;
          if (value == false) {
            if (*offset > idx) {
              *offset = *offset - idx;
            } else {
              ret += idx + 1 - *offset;
              *offset = 0;
              return ret;
            }
          } else {
            return ret;
          }
        }
      } else if (flags == SM_PAYLOAD_ONES) {
        *vec = UINT64_MAX;
        if (idx >= SM_BITS_PER_VECTOR) {
          *pos += SM_BITS_PER_VECTOR;
          idx -= SM_BITS_PER_VECTOR;
          if (*offset > SM_BITS_PER_VECTOR) {
            *offset = *offset - SM_BITS_PER_VECTOR;
          } else {
            if (value == true) {
              ret += SM_BITS_PER_VECTOR - *offset;
            }
            *offset = 0;
          }
        } else {
          *pos += idx + 1;
          if (value == true) {
            if (*offset > idx) {
              *offset = *offset - idx;
            } else {
              ret += idx + 1 - *offset;
              *offset = 0;
              return ret;
            }
          } else {
            return ret;
          }
        }
      } else if (flags == SM_PAYLOAD_MIXED) {
        sm_bitvec_t w = map->m_data[1 + __sm_chunk_map_get_position(map, i * SM_FLAGS_PER_INDEX_BYTE + j)];
        if (idx >= SM_BITS_PER_VECTOR) {
          *pos += SM_BITS_PER_VECTOR;
          idx -= SM_BITS_PER_VECTOR;
          uint64_t mask = *offset == 0 ? UINT64_MAX : ~(UINT64_MAX >> (SM_BITS_PER_VECTOR - (*offset >= 64 ? 64 : *offset)));
          sm_bitvec_t mw;
          if (value == true) {
            mw = w & mask;
          } else {
            mw = ~w & mask;
          }
          size_t pc = popcountll(mw);
          ret += pc;
          *offset = (*offset > SM_BITS_PER_VECTOR) ? *offset - SM_BITS_PER_VECTOR : 0;
        } else {
          *pos += idx + 1;
          sm_bitvec_t mw;
          uint64_t mask;
          uint64_t idx_mask = (idx == 63) ? UINT64_MAX : ((uint64_t)1 << (idx + 1)) - 1;
          uint64_t offset_mask = *offset == 0 ? UINT64_MAX : ~(UINT64_MAX >> (SM_BITS_PER_VECTOR - (*offset >= 64 ? 64 : *offset)));
          /* To count the set bits we need to mask off the portion of the vector that we need
             to count then call popcount().  So, let's create a mask for the range between
             offset and idx inclusive [*offset, idx]. */
          mask = idx_mask & offset_mask;
          if (value) {
            mw = w & mask;
          } else {
            mw = ~w & mask;
          }
          int pc = popcountll(mw);
          ret += pc;
          *vec = mw >> ((*offset > 63) ? 63 : *offset);
          *offset = *offset > idx ? *offset - idx + 1 : 0;
          return ret;
        }
      }
    }
  }
  return ret;
}

/** @brief Calls \b scanner with sm_bitmap_t for each vector in this chunk.
 *
 * Decompresses the whole chunk into separate bitmaps then calls visitor's
 * \b #operator() function for all bits.
 *
 * @param[in] map The chunk in question.
 * @param[in] start
 * @param[in] scanner
 * @returns the number of (set) bits that were passed to the scanner
 */
static size_t
__sm_chunk_map_scan(__sm_chunk_t *map, sm_idx_t start, void (*scanner)(sm_idx_t[], size_t), size_t skip)
{
  size_t ret = 0;
  register uint8_t *p = (uint8_t *)map->m_data;
  sm_idx_t buffer[SM_BITS_PER_VECTOR];
  for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
    if (*p == 0) {
      /* skip the zeroes */
      continue;
    }

    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE || flags == SM_PAYLOAD_ZEROS) {
        /* ignore the zeroes */
      } else if (flags == SM_PAYLOAD_ONES) {
        if (skip) {
          if (skip >= SM_BITS_PER_VECTOR) {
            skip -= SM_BITS_PER_VECTOR;
            ret += SM_BITS_PER_VECTOR;
            continue;
          }
          size_t n = 0;
          for (size_t b = skip; b < SM_BITS_PER_VECTOR; b++) {
            buffer[n++] = start + b;
          }
          scanner(&buffer[0], n);
          ret += n;
          skip = 0;
        } else {
          for (size_t b = 0; b < SM_BITS_PER_VECTOR; b++) {
            buffer[b] = start + b;
          }
          scanner(&buffer[0], SM_BITS_PER_VECTOR);
          ret += SM_BITS_PER_VECTOR;
        }
      } else if (flags == SM_PAYLOAD_MIXED) {
        sm_bitvec_t w = map->m_data[1 + __sm_chunk_map_get_position(map, i * SM_FLAGS_PER_INDEX_BYTE + j)];
        int n = 0;
        if (skip) {
          for (int b = 0; b < SM_BITS_PER_VECTOR; b++) {
            if (w & ((sm_bitvec_t)1 << b)) {

              skip--;
              continue;
              // TODO: unreachable lines below... why?
              buffer[n++] = start + b;
              ret++;
            }
          }
        } else {
          for (int b = 0; b < SM_BITS_PER_VECTOR; b++) {
            if (w & ((sm_bitvec_t)1 << b)) {
              buffer[n++] = start + b;
            }
          }
          ret += n;
        }
        __sm_assert(n > 0);
        scanner(&buffer[0], n);
      }
    }
  }
  return ret;
}

/** @brief Provides the number of chunks currently in the map.
 *
 * @param[in] map  The sparsemap_t in question.
 * @returns the number of chunk maps in the sparsemap
 */
static size_t
__sm_get_chunk_map_count(sparsemap_t *map)
{
  return *(uint32_t *)&map->m_data[0];
}

/** @brief Encapsulates the method to find the starting address of a chunk's
 * data.
 *
 * @param[in] map The sparsemap_t in question.
 * @param[in] offset The offset in bytes for the desired chunk map.
 * @returns the data for the specified \b offset
 */
static inline uint8_t *
__sm_get_chunk_map_data(sparsemap_t *map, size_t offset)
{
  return &map->m_data[SM_SIZEOF_OVERHEAD + offset];
}

/** @brief Encapsulates the method to find the address of the first unused byte
 * in \b m_data.
 *
 * @param[in] map The sparsemap_t in question.
 * @returns a pointer after the end of the used data
 */
static uint8_t *
__sm_get_chunk_map_end(sparsemap_t *map)
{
  uint8_t *p = __sm_get_chunk_map_data(map, 0);
  size_t count = __sm_get_chunk_map_count(map);
  for (size_t i = 0; i < count; i++) {
    p += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_map_init(&chunk, p);
    p += __sm_chunk_map_get_size(&chunk);
  }
  return p;
}

/** @brief Provides the byte size amount of \b m_data consumed.
 *
 * @param[in] map The sparsemap_t in question.
 * @returns the used size in the data buffer
 */
static size_t
__sm_get_size_impl(sparsemap_t *map)
{
  uint8_t *start = __sm_get_chunk_map_data(map, 0);
  uint8_t *p = start;

  size_t count = __sm_get_chunk_map_count(map);
  for (size_t i = 0; i < count; i++) {
    p += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_map_init(&chunk, p);
    p += __sm_chunk_map_get_size(&chunk);
  }
  return SM_SIZEOF_OVERHEAD + p - start;
}

/** @brief Aligns to SM_BITS_PER_VECTOR a given index \b idx.
 *
 * @param[in] idx The index to align.
 * @returns the aligned offset (aligned to sm_bitvec_t capacity).
 */
static sm_idx_t
__sm_get_aligned_offset(size_t idx)
{
  const size_t capacity = SM_BITS_PER_VECTOR;
  return (idx / capacity) * capacity;
}

/** @brief Aligns to SM_CHUNK_MAP_CAPACITY a given index \b idx.
 *
 * @param[in] idx The index to align.
 * @returns the aligned offset (aligned to __sm_chunk_t capacity)
 */
static sm_idx_t
__sm_get_fully_aligned_offset(size_t idx)
{
  const size_t capacity = SM_CHUNK_MAX_CAPACITY;
  return (idx / capacity) * capacity;
}

/** @brief Provides the byte offset of a chunk map at index \b idx.
 *
 * @param[in] map The sparsemap_t in question.
 * @param[in] idx The index of the chunk map to locate.
 * @returns the byte offset of a __sm_chunk_t in m_data, or -1 there
 * are no chunks.
 */
static size_t
__sm_get_chunk_map_offset(sparsemap_t *map, sparsemap_idx_t idx)
{
  size_t count = __sm_get_chunk_map_count(map);
  if (count == 0) {
    return -1;
  }

  uint8_t *start = __sm_get_chunk_map_data(map, 0);
  uint8_t *p = start;

  for (sparsemap_idx_t i = 0; i < count - 1; i++) {
    sm_idx_t s = *(sm_idx_t *)p;
    __sm_assert(s == __sm_get_aligned_offset(s));
    __sm_chunk_t chunk;
    __sm_chunk_map_init(&chunk, p + sizeof(sm_idx_t));
    if (s >= idx || idx < s + __sm_chunk_map_get_capacity(&chunk)) {
      break;
    }
    p += sizeof(sm_idx_t) + __sm_chunk_map_get_size(&chunk);
  }

  return (ssize_t)(p - start);
}

/** @brief Sets the number of __sm_chunk_t's.
 *
 * @param[in] map The sparsemap_t in question.
 * @param[in] new_count The new number of chunks in the map.
 */
static void
__sm_set_chunk_map_count(sparsemap_t *map, size_t new_count)
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
  uint8_t *p = __sm_get_chunk_map_data(map, offset);
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
  assert(map->m_data_used >= offset + gap_size);
  uint8_t *p = __sm_get_chunk_map_data(map, offset);
  memmove(p, p + gap_size, map->m_data_used - offset - gap_size);
  map->m_data_used -= gap_size;
}

/*
 * The following is the "Sparsemap" implementation, it uses chunk maps (code above)
 * and is the public API for this compressed bitmap representation.
 */

void
sparsemap_clear(sparsemap_t *map)
{
  if (map == NULL) {
    return;
  }
  memset(map->m_data, 0, map->m_capacity);
  map->m_data_used = SM_SIZEOF_OVERHEAD;
  __sm_set_chunk_map_count(map, 0);
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
  sparsemap_clear(map);

  return map;
}

sparsemap_t *
sparsemap_wrap(uint8_t *data, size_t size)
{
  sparsemap_t *map = (sparsemap_t *)calloc(1, sizeof(sparsemap_t));
  if (map) {
    sparsemap_init(map, data, size);
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
  map->m_data_used = map->m_data_used > 0 ? map->m_data_used : 0;
  map->m_capacity = size;
}

/*
 * TODO/NOTE: This is a dangerous operation because we cannot verify that
 *       data_size is not exceeding the size of the underlying buffer.
 */
sparsemap_t *
sparsemap_set_data_size(sparsemap_t *map, size_t size, uint8_t *data)
{
  size_t data_size = (size * sizeof(uint8_t));

  /* If this sparsemap was allocated by the sparsemap() API and we're not handed
     a new data, it's up to us to resize it. */
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
    if (data != NULL && data_size > sparsemap_get_capacity(map) && data != map->m_data) {
      map->m_data = data;
    }
    map->m_capacity = size;
    return map;
  }
}

double
sparsemap_capacity_remaining(sparsemap_t *map)
{
  if (map->m_data_used > map->m_capacity) {
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
  ssize_t offset = __sm_get_chunk_map_offset(map, idx);

  /* No __sm_chunk_t's available -> the bit is not set */
  if (offset == -1) {
    return false;
  }

  /* Otherwise load the __sm_chunk_t */
  uint8_t *p = __sm_get_chunk_map_data(map, offset);
  sm_idx_t start = *(sm_idx_t *)p;
  __sm_chunk_t chunk;
  __sm_chunk_map_init(&chunk, p + sizeof(sm_idx_t));

  /* Determine if the bit is out of bounds of the __sm_chunk_t; if yes then
  the bit is not set. */
  if (idx < start || (unsigned long)idx - start >= __sm_chunk_map_get_capacity(&chunk)) {
    return false;
  }

  /* Otherwise ask the __sm_chunk_t whether the bit is set. */
  return __sm_chunk_map_is_set(&chunk, idx - start);
}

sparsemap_idx_t
sparsemap_set(sparsemap_t *map, sparsemap_idx_t idx, bool value)
{
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);

  /* Get the __sm_chunk_t which manages this index */
  ssize_t offset = __sm_get_chunk_map_offset(map, idx);
  bool dont_grow = false;
  if (map->m_data_used + sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2 > map->m_capacity) {
    errno = ENOSPC;
    return SPARSEMAP_IDX_MAX;
  }

  /* If there are no __sm_chunk_t and the bit is set to zero then return
     immediately; otherwise create an initial __sm_chunk_t. */
  if (offset == -1) {
    if (value == false) {
      return idx;
    }

    uint8_t buf[sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2] = { 0 };
    __sm_append_data(map, &buf[0], sizeof(buf));

    uint8_t *p = __sm_get_chunk_map_data(map, 0);
    *(sm_idx_t *)p = __sm_get_aligned_offset(idx);

    __sm_set_chunk_map_count(map, 1);

    /* We already inserted an additional sm_bitvec_t; given that has happened
       there is no need to grow the vector even further. */
    dont_grow = true;
    offset = 0;
  }

  /* Load the __sm_chunk_t */
  uint8_t *p = __sm_get_chunk_map_data(map, offset);
  sm_idx_t start = *(sm_idx_t *)p;

  /* The new index is smaller than the first __sm_chunk_t: create a new
     __sm_chunk_t and insert it at the front. */
  if (idx < start) {
    if (value == false) {
      /* nothing to do */
      return idx;
    }

    uint8_t buf[sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2] = { 0 };
    __sm_insert_data(map, offset, &buf[0], sizeof(buf));

    size_t aligned_idx = __sm_get_fully_aligned_offset(idx);
    if (start - aligned_idx < SM_CHUNK_MAX_CAPACITY) {
      __sm_chunk_t chunk;
      __sm_chunk_map_init(&chunk, p + sizeof(sm_idx_t));
      __sm_chunk_map_set_capacity(&chunk, start - aligned_idx);
    }
    *(sm_idx_t *)p = start = aligned_idx;

    /* We just added another chunk map! */
    __sm_set_chunk_map_count(map, __sm_get_chunk_map_count(map) + 1);

    /* We already inserted an additional sm_bitvec_t; later on there
      is no need to grow the vector even further. */
    dont_grow = true;
  }

  /* A __sm_chunk_t exists, but the new index exceeds its capacities: create
    a new __sm_chunk_t and insert it after the current one. */
  else {
    __sm_chunk_t chunk;
    __sm_chunk_map_init(&chunk, p + sizeof(sm_idx_t));
    if (idx - start >= (sparsemap_idx_t)__sm_chunk_map_get_capacity(&chunk)) {
      if (value == false) {
        /* nothing to do */
        return idx;
      }

      size_t size = __sm_chunk_map_get_size(&chunk);
      offset += (sizeof(sm_idx_t) + size);
      p += sizeof(sm_idx_t) + size;

      uint8_t buf[sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2] = { 0 };
      __sm_insert_data(map, offset, &buf[0], sizeof(buf));

      start += __sm_chunk_map_get_capacity(&chunk);
      if ((sparsemap_idx_t)start + SM_CHUNK_MAX_CAPACITY < idx) {
        start = __sm_get_fully_aligned_offset(idx);
      }
      *(sm_idx_t *)p = start;

      /* We just added another chunk map! */
      __sm_set_chunk_map_count(map, __sm_get_chunk_map_count(map) + 1);

      /* We already inserted an additional sm_bitvec_t; later on there
         is no need to grow the vector even further. */
      dont_grow = true;
    }
  }

  __sm_chunk_t chunk;
  __sm_chunk_map_init(&chunk, p + sizeof(sm_idx_t));

  /* Now update the __sm_chunk_t. */
  size_t position;
  sm_bitvec_t fill;
  int code = __sm_chunk_map_set(&chunk, idx - start, value, &position, &fill, false);
  switch (code) {
  case SM_OK:
    break;
  case SM_NEEDS_TO_GROW:
    if (!dont_grow) {
      offset += (sizeof(sm_idx_t) + position * sizeof(sm_bitvec_t));
      __sm_insert_data(map, offset, (uint8_t *)&fill, sizeof(sm_bitvec_t));
    }
    __sm_chunk_map_set(&chunk, idx - start, value, &position, &fill, true);
    break;
  case SM_NEEDS_TO_SHRINK:
    /* If the __sm_chunk_t is empty then remove it. */
    if (__sm_chunk_map_is_empty(&chunk)) {
      __sm_assert(position == 1);
      __sm_remove_data(map, offset, sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2);
      __sm_set_chunk_map_count(map, __sm_get_chunk_map_count(map) - 1);
    } else {
      offset += (sizeof(sm_idx_t) + position * sizeof(sm_bitvec_t));
      __sm_remove_data(map, offset, sizeof(sm_bitvec_t));
    }
    break;
  default:
    __sm_assert(!"shouldn't be here");
#ifdef DEBUG
    abort();
#endif
    break;
  }
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);
  return idx;
}

sparsemap_idx_t
sparsemap_get_starting_offset(sparsemap_t *map)
{
  size_t count = __sm_get_chunk_map_count(map);
  if (count == 0) {
    return 0;
  }
  sm_idx_t *chunk = (sm_idx_t *)__sm_get_chunk_map_data(map, 0);
  return (sparsemap_idx_t)*chunk;
}

/**
 * Returns the used size in the data buffer.
 */
size_t
sparsemap_get_size(sparsemap_t *map)
{
  if (map->m_data_used) {
    __sm_when_diag({ __sm_assert(map->m_data_used == __sm_get_size_impl(map)); });
    return map->m_data_used;
  }
  return map->m_data_used = __sm_get_size_impl(map);
}

/**
 * Decompresses the whole bitmap; calls scanner for all bits.
 */
void
sparsemap_scan(sparsemap_t *map, void (*scanner)(sm_idx_t[], size_t), size_t skip)
{
  uint8_t *p = __sm_get_chunk_map_data(map, 0);
  size_t count = __sm_get_chunk_map_count(map);

  for (size_t i = 0; i < count; i++) {
    sm_idx_t start = *(sm_idx_t *)p;
    p += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_map_init(&chunk, p);
    size_t skipped = __sm_chunk_map_scan(&chunk, start, scanner, skip);
    if (skip) {
      assert(skip >= skipped);
      skip -= skipped;
    }
    p += __sm_chunk_map_get_size(&chunk);
  }
}

void
__sm_chunk_map_merge(sparsemap_t *map, sparsemap_idx_t offset, __sm_chunk_t src)
{
  size_t capacity = __sm_chunk_map_get_capacity(&src);
  for (sparsemap_idx_t j = 0; j < capacity; j++, offset++) {
    if (__sm_chunk_map_is_set(&src, j)) {
      sparsemap_set(map, offset, true);
    }
  }
}

void
sparsemap_merge(sparsemap_t *map, sparsemap_t *other)
{
  uint8_t *src, *dst;
  size_t src_count = __sm_get_chunk_map_count(other), dst_count = __sm_get_chunk_map_count(map), max_chunk_count = src_count + dst_count;

  dst = __sm_get_chunk_map_data(map, 0);
  src = __sm_get_chunk_map_data(other, 0);
  for (size_t i = 0; i < max_chunk_count && src_count; i++) {
    sm_idx_t dst_start = *(sm_idx_t *)dst;
    sm_idx_t src_start = *(sm_idx_t *)src;
    if (src_start > dst_start && dst_count > 0) {
      __sm_chunk_t dst_chunk;
      __sm_chunk_map_init(&dst_chunk, dst + sizeof(sm_idx_t));
      dst += sizeof(sm_idx_t) + __sm_chunk_map_get_size(&dst_chunk);
      continue;
    }
    if (src_start == dst_start && dst_count > 0) {
      /* Chunks overlap, merge them. */
      __sm_chunk_t src_chunk;
      __sm_chunk_map_init(&src_chunk, src + sizeof(sm_idx_t));
      __sm_chunk_t dst_chunk;
      __sm_chunk_map_init(&dst_chunk, dst + sizeof(sm_idx_t));
      __sm_chunk_map_merge(map, src_start, src_chunk);
      *(sm_idx_t *)dst = __sm_get_aligned_offset(src_start);
      src += sizeof(sm_idx_t) + __sm_chunk_map_get_size(&src_chunk);
      dst += sizeof(sm_idx_t) + __sm_chunk_map_get_size(&dst_chunk);
      dst_count--;
      src_count--;
      continue;
    }
    if (src_start < dst_start || dst_count == 0) {
      __sm_chunk_t src_chunk;
      __sm_chunk_map_init(&src_chunk, src + sizeof(sm_idx_t));
      size_t src_size = __sm_chunk_map_get_size(&src_chunk);
      if (dst_count == 0) {
        __sm_append_data(map, src, sizeof(sm_idx_t) + src_size);
      } else {
        size_t offset = __sm_get_chunk_map_offset(map, dst_start);
        __sm_insert_data(map, offset, src, sizeof(sm_idx_t) + src_size);
      }

      /* Update the chunk count and data_used. */
      __sm_set_chunk_map_count(map, __sm_get_chunk_map_count(map) + 1);

      /* Carry on to the next chunk. */
      __sm_chunk_t dst_chunk;
      __sm_chunk_map_init(&dst_chunk, dst + sizeof(sm_idx_t));
      src += sizeof(sm_idx_t) + __sm_chunk_map_get_size(&src_chunk);
      dst += sizeof(sm_idx_t) + __sm_chunk_map_get_size(&dst_chunk);
      src_count--;
    }
  }
}

void
sparsemap_split(sparsemap_t *map, sparsemap_idx_t offset, sparsemap_t *other)
{
  assert(offset % SM_BITS_PER_VECTOR == 0);

  /* |dst| points to the destination buffer */
  uint8_t *dst = __sm_get_chunk_map_end(other);

  /* |src| points to the source-chunk map */
  uint8_t *src = __sm_get_chunk_map_data(map, 0);

  /* |offset| is relative to the beginning of this sparsemap_t; best
     make it absolute. */
  offset += *(sm_idx_t *)src;

  bool in_middle = false;
  uint8_t *prev = src;
  size_t i, count = __sm_get_chunk_map_count(map);
  for (i = 0; i < count; i++) {
    sm_idx_t start = *(sm_idx_t *)src;
    __sm_chunk_t chunk;
    __sm_chunk_map_init(&chunk, src + sizeof(sm_idx_t));
    if (start == offset) {
      break;
    }
    if (start + __sm_chunk_map_get_capacity(&chunk) > (unsigned long)offset) {
      in_middle = true;
      break;
    }
    if (start > offset) {
      src = prev;
      i--;
      break;
    }

    prev = src;
    src += sizeof(sm_idx_t) + __sm_chunk_map_get_size(&chunk);
  }
  if (i == count) {
    assert(sparsemap_get_size(map) > SM_SIZEOF_OVERHEAD);
    assert(sparsemap_get_size(other) > SM_SIZEOF_OVERHEAD);
    return;
  }

  /* Now copy all the remaining chunks. */
  int moved = 0;

  /* If |offset| is in the middle of a chunk then this chunk has to be split */
  if (in_middle) {
    uint8_t buf[sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2] = { 0 };
    memcpy(dst, &buf[0], sizeof(buf));

    *(sm_idx_t *)dst = offset;
    dst += sizeof(sm_idx_t);

    /* the |other| sparsemap_t now has one additional chunk */
    __sm_set_chunk_map_count(other, __sm_get_chunk_map_count(other) + 1);
    if (other->m_data_used != 0) {
      other->m_data_used += sizeof(sm_idx_t) + sizeof(sm_bitvec_t);
    }

    src += sizeof(sm_idx_t);
    __sm_chunk_t s_chunk;
    __sm_chunk_map_init(&s_chunk, src);
    size_t capacity = __sm_chunk_map_get_capacity(&s_chunk);

    __sm_chunk_t d_chunk;
    __sm_chunk_map_init(&d_chunk, dst);
    __sm_chunk_map_set_capacity(&d_chunk, capacity - (offset % capacity));

    /* Now copy the bits. */
    sparsemap_idx_t d = offset;
    for (size_t j = offset % capacity; j < capacity; j++, d++) {
      if (__sm_chunk_map_is_set(&s_chunk, j)) {
        sparsemap_set(other, d, true);
      }
    }

    src += __sm_chunk_map_get_size(&s_chunk);
    size_t dsize = __sm_chunk_map_get_size(&d_chunk);
    dst += dsize;
    i++;

    /* Reduce the capacity of the source-chunk map. */
    __sm_chunk_map_set_capacity(&s_chunk, offset % capacity);
  }

  /* Now continue with all remaining chunk maps. */
  for (; i < count; i++) {
    sm_idx_t start = *(sm_idx_t *)src;
    src += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_map_init(&chunk, src);
    size_t s = __sm_chunk_map_get_size(&chunk);

    *(sm_idx_t *)dst = start;
    dst += sizeof(sm_idx_t);
    memcpy(dst, src, s);
    src += s;
    dst += s;

    moved++;
  }

  /* Force new calculation. */
  other->m_data_used = 0;
  map->m_data_used = 0;

  /* Update the Chunk Map counters. */
  __sm_set_chunk_map_count(map, __sm_get_chunk_map_count(map) - moved);
  __sm_set_chunk_map_count(other, __sm_get_chunk_map_count(other) + moved);

  assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);
  assert(sparsemap_get_size(other) > SM_SIZEOF_OVERHEAD);
}

sparsemap_idx_t
sparsemap_select(sparsemap_t *map, sparsemap_idx_t n, bool value)
{
  assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);
  sm_idx_t start;
  size_t count = __sm_get_chunk_map_count(map);

  uint8_t *p = __sm_get_chunk_map_data(map, 0);

  for (size_t i = 0; i < count; i++) {
    start = *(sm_idx_t *)p;
    /* Start of this chunk is greater than n meaning there are a set of 0s
       before the first 1 sufficient to consume n. */
    if (value == false && i == 0 && start > n) {
      return n;
    }
    p += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_map_init(&chunk, p);

    ssize_t new_n = n;
    size_t index = __sm_chunk_map_select(&chunk, n, &new_n, value);
    if (new_n == -1) {
      return start + index;
    }
    n = new_n;

    p += __sm_chunk_map_get_size(&chunk);
  }
  return SPARSEMAP_IDX_MAX;
}

size_t
sparsemap_rank_vec(sparsemap_t *map, size_t x, size_t y, bool value, sm_bitvec_t *vec)
{
  assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);
  size_t amt, gap, pos = 0, result = 0, prev = 0, count, len = y - x + 1;
  uint8_t *p;

  if (x > y) {
    return 0;
  }

  count = __sm_get_chunk_map_count(map);

  if (count == 0) {
    if (value == false) {
      /* The count/rank of unset bits in an empty map is inf, so what you requested is the answer. */
      return len;
    }
  }

  p = __sm_get_chunk_map_data(map, 0);

  for (size_t i = 0; i < count; i++) {
    sm_idx_t start = *(sm_idx_t *)p;
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
    if (start > y) {
      if (value == true) {
        /* We're counting set bits and this chunk starts after the range
           [x, y], we're done. */
        return result;
      } else {
        if (i == 0) {
          /* We're counting unset bits and the first chunk starts after the
             range meaning everything proceeding this chunk was zero and should
             be counted, also we're done. */
          result += (y - x) + 1;
          return result;
        } else {
          /* We're counting unset bits and some chunk starts after the range, so
             we've counted enough, we're done. */
          if (pos > y) {
            return result;
          } else {
            if (y - pos < gap) {
              result += y - pos;
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
        if (x > gap) {
          x -= gap;
        } else {
          result += gap - x;
          x = 0;
        }
      } else {
        if (x > gap) {
          x -= gap;
        }
      }
    }
    prev = start;
    p += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_map_init(&chunk, p);

    /* Count all the set/unset inside this chunk. */
    amt = __sm_chunk_map_rank(&chunk, &x, y - start, &pos, vec, value);
    result += amt;
    p += __sm_chunk_map_get_size(&chunk);
  }
  /* Count any additional unset bits that fall outside the last chunk but
     within the range. */
  if (value == false) {
    size_t last = prev - 1 + pos;
    if (y > last) {
      result += y - last - x;
    }
  }
  return result;
}

size_t
sparsemap_rank(sparsemap_t *map, size_t x, size_t y, bool value)
{
  sm_bitvec_t vec;
  return sparsemap_rank_vec(map, x, y, value, &vec);
}

size_t
sparsemap_span(sparsemap_t *map, sparsemap_idx_t idx, size_t len, bool value)
{
  size_t rank, nth;
  sm_bitvec_t vec = 0;
  sparsemap_idx_t offset = 0;

  /* When skipping forward to `idx` offset in the map we can determine how
     many selects we can avoid by taking the rank of the range and starting
     at that bit. */
  nth = (idx < 1) ? 0 : sparsemap_rank(map, 0, idx - 1, value);
  /* Find the first bit that matches value, then... */
  offset = sparsemap_select(map, nth, value);
  do {
    /* See if the rank of the bits in the range starting at offset is equal
       to the desired amount. */
    rank = len == 1 ? 1 : sparsemap_rank_vec(map, offset, offset + len - 1, value, &vec);
    if (rank >= len) {
      /* We've found what we're looking for, return the index of the first
         bit in the range. */
      break;
    }
    /* Now we try to jump forward as much as possible before we look for a
       new match. We do this by counting the remaining bits in the returned
       vec from the call to rank_vec(). */
    int amt = 0;
    if (vec == 0) {
      /* The returned vec had no set bits, let's move forward in the map. */
      amt = (rank == 0) ? len : 1;
    } else {
      /* We might be able to jump forward up to 64 bit positions saving us repeated
         calls to select()/rank(). */
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
