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

#include <assert.h>
#include <popcount.h>
#include <sparsemap.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

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
#else
#define __sm_diag(...) ((void)0)
#endif

#ifndef SPARSEMAP_ASSERT
#define SPARSEMAP_ASSERT
#define __sm_assert(expr) \
  if (!(expr))            \
  fprintf(stderr, "%s:%d:%s(): assertion failed! %s", __FILE__, __LINE__, __func__, #expr)
#else
#define __sm_assert(expr) ((void)0)
#endif

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

  /* a mask for checking flags (2 bits) */
  SM_FLAG_MASK = 3,

  /* return code for set(): ok, no further action required */
  SM_OK = 0
};

#define SM_CHUNK_GET_FLAGS(from, at) (((from)) & ((sm_bitvec_t)SM_FLAG_MASK << ((at) * 2))) >> ((at) * 2)

typedef struct {
  sm_bitvec_t *m_data;
} __sm_chunk_t;

struct sparsemap {
  uint8_t *m_data;    /* The serialized bitmap data */
  size_t m_capacity;  /* The total size of m_data */
  size_t m_data_used; /* The used size of m_data */
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
  return ((size_t)lookup[b]);
}

/**
 * Returns the position of a sm_bitvec_t in m_data.
 */
static size_t
__sm_chunk_map_get_position(__sm_chunk_t *map, size_t bv)
{
  // handle 4 indices (1 byte) at a time
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

  return (position);
}

/**
 * Initialize __sm_chunk_t with provided data.
 */
static inline void
__sm_chunk_map_init(__sm_chunk_t *map, uint8_t *data)
{
  map->m_data = (sm_bitvec_t *)data;
}

/**
 * Returns the maximum capacity of this __sm_chunk_t.
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
  return (capacity);
}

/**
 * Sets the capacity.
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

/**
 * Returns true if this __sm_chunk_t is empty.
 */
static bool
__sm_chunk_map_is_empty(__sm_chunk_t *map)
{
  /* The __sm_chunk_t is empty if all flags (in m_data[0]) are zero. */
  if (map->m_data[0] == 0) {
    return (true);
  }

  /* It's also empty if all flags are Zero or None. */
  register uint8_t *p = (uint8_t *)map->m_data;
  for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
    if (*p) {
      for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
        size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
        if (flags != SM_PAYLOAD_NONE && flags != SM_PAYLOAD_ZEROS) {
          return (false);
        }
      }
    }
  }
  return (true);
}

/**
 * Returns the size of the data buffer, in bytes.
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

  return (size);
}

/**
 * Returns the value of a bit at index |idx|.
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
    return (false);
  case SM_PAYLOAD_ONES:
    return (true);
  default:
    __sm_assert(flags == SM_PAYLOAD_MIXED);
    /* FALLTHROUGH */
  }

  /* get the sm_bitvec_t at |bv| */
  sm_bitvec_t w = map->m_data[1 + __sm_chunk_map_get_position(map, bv)];
  /* and finally check the bit in that sm_bitvec_t */
  return ((w & ((sm_bitvec_t)1 << (idx % SM_BITS_PER_VECTOR))) > 0);
}

/**
 * Sets the value of a bit at index |idx|. Returns SM_NEEDS_TO_GROW,
 * SM_NEEDS_TO_SHRINK, or SM_OK. Sets |position| to the position of the
 * sm_bitvec_t which is inserted/deleted and |fill| - the value of the fill
 * word (used when growing).
 *
 * Note, the caller MUST to perform the relevant actions and call set() again,
 * this time with |retried| = true.
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

/**
 * Returns the index of the n'th set bit; sets |*pnew_n| to 0 if the
 * n'th bit was found in this __sm_chunk_t, or to the new, reduced
 * value of |n|.
 */
static size_t
__sm_chunk_map_select(__sm_chunk_t *map, size_t n, ssize_t *pnew_n)
{
  size_t ret = 0;
  register uint8_t *p;

  p = (uint8_t *)map->m_data;
  for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
    if (*p == 0) {
      ret += (size_t)SM_FLAGS_PER_INDEX_BYTE * SM_BITS_PER_VECTOR;
      continue;
    }

    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        continue;
      }
      if (flags == SM_PAYLOAD_ZEROS) {
        ret += SM_BITS_PER_VECTOR;
        continue;
      }
      if (flags == SM_PAYLOAD_ONES) {
        if (n > SM_BITS_PER_VECTOR) {
          n -= SM_BITS_PER_VECTOR;
          ret += SM_BITS_PER_VECTOR;
          continue;
        }

        *pnew_n = -1;
        return (ret + n);
      }
      if (flags == SM_PAYLOAD_MIXED) {
        sm_bitvec_t w = map->m_data[1 + __sm_chunk_map_get_position(map, i * SM_FLAGS_PER_INDEX_BYTE + j)];
        for (int k = 0; k < SM_BITS_PER_VECTOR; k++) {
          if (w & ((sm_bitvec_t)1 << k)) {
            if (n == 0) {
              *pnew_n = -1;
              return (ret);
            }
            n--;
          }
          ret++;
        }
      }
    }
  }

  *pnew_n = (ssize_t)n;
  return (ret);
}

/**
 * Counts the set bits in the range [0, 'idx'] inclusive ignoring the first
 * '*offset' bits.  Modifies '*offset' decreasing it by the number of bits
 * ignored during the search.  The ranking (counting) will start after the
 * '*offset' has been reached 0.
 */
static size_t
__sm_chunk_map_rank(__sm_chunk_t *map, size_t *offset, size_t idx)
{
  size_t ret = 0;

  register uint8_t *p = (uint8_t *)map->m_data;
  for (size_t i = 0; i < sizeof(sm_bitvec_t); i++, p++) {
    for (int j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
      size_t flags = SM_CHUNK_GET_FLAGS(*p, j);
      if (flags == SM_PAYLOAD_NONE) {
        continue;
      }
      if (flags == SM_PAYLOAD_ZEROS) {
        if (idx > SM_BITS_PER_VECTOR) {
          if (*offset > SM_BITS_PER_VECTOR) {
            *offset = *offset - SM_BITS_PER_VECTOR;
          } else {
            idx -= SM_BITS_PER_VECTOR - *offset;
            *offset = 0;
          }
        } else {
          return (ret);
        }
      } else if (flags == SM_PAYLOAD_ONES) {
        if (idx > SM_BITS_PER_VECTOR) {
          if (*offset > SM_BITS_PER_VECTOR) {
            *offset = *offset - SM_BITS_PER_VECTOR;
          } else {
            idx -= SM_BITS_PER_VECTOR - *offset;
            if (*offset == 0) {
              ret += SM_BITS_PER_VECTOR;
            }
            *offset = 0;
          }
        } else {
          return (ret + idx);
        }
      } else if (flags == SM_PAYLOAD_MIXED) {
        sm_bitvec_t w = map->m_data[1 + __sm_chunk_map_get_position(map, i * SM_FLAGS_PER_INDEX_BYTE + j)];
        if (idx > SM_BITS_PER_VECTOR) {
          uint64_t mask_offset = ~(UINT64_MAX >> (SM_BITS_PER_VECTOR - *offset));
          idx -= SM_BITS_PER_VECTOR;
          ret += popcountll(w & mask_offset);
          *offset = (*offset > SM_BITS_PER_VECTOR) ? *offset - SM_BITS_PER_VECTOR : 0;
        } else {
          /* Create a mask for the range between offset and idx inclusive [*offset, idx]. */
          uint64_t offset_mask = (((uint64_t)1 << *offset) - 1);
          uint64_t idx_mask = idx >= 63 ? UINT64_MAX : ((uint64_t)1 << (idx + 1)) - 1;
          ret += popcountll(w & (idx_mask - offset_mask));
          *offset = *offset > idx ? *offset - idx : 0;
          return (ret);
        }
      }
    }
  }
  return (ret);
}

/**
 * Decompresses the whole bitmap; calls visitor's operator() for all bits
 * Returns the number of (set) bits that were passed to the scanner
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
  return (ret);
}

/*
 * The following is the "Sparsemap" implementation, it uses Chunk Maps (above).
 */

/**
 * Returns the number of chunk maps.
 */
static inline size_t
__sm_get_chunk_map_count(sparsemap_t *map)
{
  return (*(uint32_t *)&map->m_data[0]);
}

/**
 * Returns the data at the specified |offset|.
 */
static inline uint8_t *
__sm_get_chunk_map_data(sparsemap_t *map, size_t offset)
{
  return (uint8_t *)(&map->m_data[SM_SIZEOF_OVERHEAD + offset]);
}

/**
 * Returns a pointer after the end of the used data.
 */
static uint8_t *
__sm_get_chunk_map_end(sparsemap_t *map)
{
  // TODO: could this simply use m_data_used?
  uint8_t *p = __sm_get_chunk_map_data(map, 0);
  size_t count = __sm_get_chunk_map_count(map);
  for (size_t i = 0; i < count; i++) {
    p += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_map_init(&chunk, p);
    p += __sm_chunk_map_get_size(&chunk);
  }
  return (p);
}

/**
 * Returns the used size in the data buffer.
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
  return (SM_SIZEOF_OVERHEAD + p - start);
}

/**
 * Returns the aligned offset (aligned to sm_bitvec_t capacity).
 */
static inline sm_idx_t
__sm_get_aligned_offset(size_t idx)
{
  const size_t capacity = SM_BITS_PER_VECTOR;
  return ((idx / capacity) * capacity);
}

/**
 * Returns the byte offset of a __sm_chunk_t in m_data
 */
static ssize_t
__sm_get_chunk_map_offset(sparsemap_t *map, size_t idx)
{
  size_t count;

  count = __sm_get_chunk_map_count(map);
  if (count == 0) {
    return (-1);
  }

  uint8_t *start = __sm_get_chunk_map_data(map, 0);
  uint8_t *p = start;

  for (size_t i = 0; i < count - 1; i++) {
    sm_idx_t bytes = *(sm_idx_t *)p;
    __sm_assert(bytes == __sm_get_aligned_offset(bytes));
    __sm_chunk_t chunk;
    __sm_chunk_map_init(&chunk, p + sizeof(sm_idx_t));
    if (bytes >= idx || idx < bytes + __sm_chunk_map_get_capacity(&chunk)) {
      break;
    }
    p += sizeof(sm_idx_t) + __sm_chunk_map_get_size(&chunk);
  }

  return ((ssize_t)(p - start));
}

/**
 * Returns the aligned offset (aligned to __sm_chunk_t capacity).
 */
static inline sm_idx_t
__sm_get_fully_aligned_offset(size_t idx)
{
  const size_t capacity = SM_CHUNK_MAX_CAPACITY;
  return ((idx / capacity) * capacity);
}

/**
 * Sets the number of __sm_chunk_t's.
 */
static inline void
__sm_set_chunk_map_count(sparsemap_t *map, size_t new_count)
{
  *(uint32_t *)&map->m_data[0] = (uint32_t)new_count;
}

/**
 * Appends more data.
 */
static void
__sm_append_data(sparsemap_t *map, uint8_t *buffer, size_t buffer_size)
{
  memcpy(&map->m_data[map->m_data_used], buffer, buffer_size);
  map->m_data_used += buffer_size;
}

int
sparsemap_on_heap_resize_fn(sparsemap_t *map, sparsemap_adaptations_t desire, size_t cur_size, size_t *new_size) {
  int rc = 0;
  size_t nsz;
  uint8_t *new, *formerly = map->m_data;

  if (desire == SM_NEEDS_TO_GROW) {
    nsz = (cur_size * 2) * sizeof(uint8_t);
    new = realloc(map->m_data, nsz);
    if (!new) {
      map->m_data = formerly;
      rc = errno;
    } else {
      map->m_data = new;
      *new_size = nsz;
      rc = -(int)(nsz - cur_size);
    }
  }

  // TODO SM_NEEDS_TO_SHRINK
  return rc;
}

/**
 * Inserts data somewhere in the middle of m_data.
 */
static int
__sm_insert_data(sparsemap_t *map, size_t offset, uint8_t *buffer, size_t buffer_size)
{
  int rc = 0;
  size_t osz, nsz;

  if (map->m_data_used + buffer_size > map->m_data_size) {
    /* attempt to grow the heap buffer */
    if (map->resize) {
      osz = map->m_data_size;
      rc = map->resize(map, SM_NEEDS_TO_GROW, osz, &nsz);
      if (rc <= 0) {
        sparsemap_set_data_size(map, nsz);
        memset(map->m_data + osz, 0, nsz - osz);
        return rc;
      }
    }
    goto fail;
  }

  uint8_t *p = __sm_get_chunk_map_data(map, offset);
  memmove(p + buffer_size, p, map->m_data_used - offset);
  memcpy(p, buffer, buffer_size);
  map->m_data_used += buffer_size;

  return rc;
fail:;
  __sm_assert(!"buffer overflow");
#ifdef DEBUG
  abort();
#endif
  return rc;
}

/**
 * Removes data from m_data.
 */
static void
__sm_remove_data(sparsemap_t *map, size_t offset, size_t gap_size)
{
  assert(map->m_data_used >= offset + gap_size);
  uint8_t *p = __sm_get_chunk_map_data(map, offset);
  memmove(p, p + gap_size, map->m_data_used - offset - gap_size);
  map->m_data_used -= gap_size;
}

/**
 * Clears the whole buffer.
 */
inline void
sparsemap_clear(sparsemap_t *map)
{
  memset(map->m_data, 0, map->m_capacity);
  map->m_data_used = SM_SIZEOF_OVERHEAD;
  __sm_set_chunk_map_count(map, 0);
}

/**
 * Allocate on a sparsemap_t on the heap and initialize it.
 */
sparsemap_t *
sparsemap(uint8_t *data, size_t size)
{
  sparsemap_t *map = (sparsemap_t *)calloc(1, sizeof(sparsemap_t));
  if (map) {
    sparsemap_init(map, data, size);
  }
  return map;
}

/**
 * Initialize sparsemap_t with data.
 */
void
sparsemap_init(sparsemap_t *map, uint8_t *data, size_t size)
{
  map->m_data = data;
  map->m_data_used = 0;
  map->m_capacity = size == 0 ? UINT64_MAX : size;
  sparsemap_clear(map);
}

/**
 * Opens an existing sparsemap at the specified buffer.
 */
inline void
sparsemap_open(sparsemap_t *map, uint8_t *data, size_t data_size)
{
  map->m_data = data;
  map->m_data_used = 0;
  map->m_capacity = data_size;
}

/**
 * Resizes the data range.
 *
 * TODO/NOTE: This is a dangerous operation because we cannot verify that
 *       data_size is not exceeding the size of the underlying buffer.
 */
inline void
sparsemap_set_data_size(sparsemap_t *map, size_t data_size)
{
  map->m_capacity = data_size;
}

/**
 * Calculates the remaining capacity as an integer that approaches 0 to
 * indicate full.
 */
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

/**
 * Returns the size of the underlying byte array.
 */
inline size_t
sparsemap_get_range_size(sparsemap_t *map)
{
  return (map->m_capacity);
}

/**
 * Returns the value of a bit at index |idx|.
 */
bool
sparsemap_is_set(sparsemap_t *map, size_t idx)
{
  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);

  /* Get the __sm_chunk_t which manages this index */
  ssize_t offset = __sm_get_chunk_map_offset(map, idx);

  /* No __sm_chunk_t's available -> the bit is not set */
  if (offset == -1) {
    return (false);
  }

  /* Otherwise load the __sm_chunk_t */
  uint8_t *p = __sm_get_chunk_map_data(map, offset);
  sm_idx_t start = *(sm_idx_t *)p;
  __sm_chunk_t chunk;
  __sm_chunk_map_init(&chunk, p + sizeof(sm_idx_t));

  /* Determine if the bit is out of bounds of the __sm_chunk_t; if yes then
    the bit is not set. */
  if (idx < start || idx - start >= __sm_chunk_map_get_capacity(&chunk)) {
    return (false);
  }

  /* Otherwise ask the __sm_chunk_t whether the bit is set. */
  return (__sm_chunk_map_is_set(&chunk, idx - start));
}

/**
 * Sets the bit at index |idx| to true or false, depending on |value|.
 */
int
sparsemap_set(sparsemap_t *map, size_t idx, bool value)
{
  int rc = 0;

  __sm_assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);

  /* Get the __sm_chunk_t which manages this index */
  ssize_t offset = __sm_get_chunk_map_offset(map, idx);
  bool dont_grow = false;

  /* If there is no __sm_chunk_t and the bit is set to zero then return
     immediately; otherwise create an initial __sm_chunk_t. */
  if (offset == -1) {
    if (value == false) {
      return 0;
    }

    uint8_t buf[sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2] = { 0 };
    __sm_append_data(map, &buf[0], sizeof(buf));

    uint8_t *p = __sm_get_chunk_map_data(map, 0);
    *(sm_idx_t *)p = __sm_get_aligned_offset(idx);

    __sm_set_chunk_map_count(map, 1);

    /* We already inserted an additional sm_bitvec_t; later on there
       is no need to grow the vector even further. */
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
      return 0;
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
    if (idx - start >= __sm_chunk_map_get_capacity(&chunk)) {
      if (value == false) {
        /* nothing to do */
        return 0;
      }

      size_t size = __sm_chunk_map_get_size(&chunk);
      offset += (ssize_t)(sizeof(sm_idx_t) + size);
      p += sizeof(sm_idx_t) + size;

      uint8_t buf[sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2] = { 0 };
      __sm_insert_data(map, offset, &buf[0], sizeof(buf));

      start += __sm_chunk_map_get_capacity(&chunk);
      if ((size_t)start + SM_CHUNK_MAX_CAPACITY < idx) {
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
      offset += sizeof(sm_idx_t) + position * sizeof(sm_bitvec_t);
      rc = __sm_insert_data(map, offset, (uint8_t *)&fill, sizeof(sm_bitvec_t));
      if (rc > 0) {
        return rc;
      } else if (rc < 0) {
        __sm_diag("added %d bytes to the map", -(rc));
        return sparsemap_set(map, idx, false);
      }
    }
    code = __sm_chunk_map_set(&chunk, idx - start, value, &position, &fill, true);
    ((void)code);
    __sm_assert(code == SM_OK);
    break;
  case SM_NEEDS_TO_SHRINK:
    /* If the __sm_chunk_t is empty then remove it. */
    if (__sm_chunk_map_is_empty(&chunk)) {
      __sm_assert(position == 1);
      __sm_remove_data(map, offset, sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2);
      __sm_set_chunk_map_count(map, __sm_get_chunk_map_count(map) - 1);
    } else {
      offset += (ssize_t)(sizeof(sm_idx_t) + position * sizeof(sm_bitvec_t));
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
  return rc;
}

/**
 * Returns the offset of the very first bit.
 */
sm_idx_t
sparsemap_get_start_offset(sparsemap_t *map)
{
  if (__sm_get_chunk_map_count(map) == 0) {
    return (0);
  }
  return (*(sm_idx_t *)__sm_get_chunk_map_data(map, 0));
}

/**
 * Returns the used size in the data buffer.
 */
size_t
sparsemap_get_size(sparsemap_t *map)
{
  if (map->m_data_used) {
    assert(map->m_data_used == __sm_get_size_impl(map));
    return (map->m_data_used);
  }
  return (map->m_data_used = __sm_get_size_impl(map));
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

/**
 * Appends all chunk maps from |sstart| to |other|, then reduces the chunk
 * map-count appropriately. |sstart| must be BitVector-aligned!
 */
void
sparsemap_split(sparsemap_t *map, size_t sstart, sparsemap_t *other)
{
  assert(sstart % SM_BITS_PER_VECTOR == 0);

  /* |dst| points to the destination buffer */
  uint8_t *dst = __sm_get_chunk_map_end(other);

  /* |src| points to the source-chunk map */
  uint8_t *src = __sm_get_chunk_map_data(map, 0);

  /* |sstart| is relative to the beginning of this sparsemap_t; best
     make it absolute. */
  sstart += *(sm_idx_t *)src;

  bool in_middle = false;
  uint8_t *prev = src;
  size_t i, count = __sm_get_chunk_map_count(map);
  for (i = 0; i < count; i++) {
    sm_idx_t start = *(sm_idx_t *)src;
    __sm_chunk_t chunk;
    __sm_chunk_map_init(&chunk, src + sizeof(sm_idx_t));
    if (start == sstart) {
      break;
    }
    if (start + __sm_chunk_map_get_capacity(&chunk) > sstart) {
      in_middle = true;
      break;
    }
    if (start > sstart) {
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

  /* If |sstart| is in the middle of a chunk then this chunk has to be split */
  if (in_middle) {
    uint8_t buf[sizeof(sm_idx_t) + sizeof(sm_bitvec_t) * 2] = { 0 };
    memcpy(dst, &buf[0], sizeof(buf));

    *(sm_idx_t *)dst = sstart;
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
    __sm_chunk_map_set_capacity(&d_chunk, capacity - (sstart % capacity));

    /* Now copy the bits. */
    size_t d = sstart;
    for (size_t j = sstart % capacity; j < capacity; j++, d++) {
      if (__sm_chunk_map_is_set(&s_chunk, j)) {
        sparsemap_set(other, d, true);
      }
    }

    src += __sm_chunk_map_get_size(&s_chunk);
    size_t dsize = __sm_chunk_map_get_size(&d_chunk);
    dst += dsize;
    i++;

    /* Reduce the capacity of the source-chunk map. */
    __sm_chunk_map_set_capacity(&s_chunk, sstart % capacity);
  }

  /* Now continue with all remaining minimaps. */
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

/**
 * Returns the index of the n'th set bit; uses a 0-based index,
 * i.e. n == 0 for the first bit which is set, n == 1 for the second bit etc.
 */
size_t
sparsemap_select(sparsemap_t *map, size_t n)
{
  assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);
  size_t result;
  size_t count = __sm_get_chunk_map_count(map);
  uint8_t *p = __sm_get_chunk_map_data(map, 0);

  for (size_t i = 0; i < count; i++) {
    result = *(sm_idx_t *)p;
    p += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_map_init(&chunk, p);

    ssize_t new_n = (ssize_t)n;
    size_t index = __sm_chunk_map_select(&chunk, n, &new_n);
    if (new_n == -1) {
      return (result + index);
    }
    n = new_n;

    p += __sm_chunk_map_get_size(&chunk);
  }
#ifdef DEBUG
  assert(!"shouldn't be here");
#endif
  return (size_t)-1;
}

/**
 * Counts the set bits starting at 'offset' until and including 'idx', meaning
 * [offset, idx] inclusive.
 */
size_t
sparsemap_rank(sparsemap_t *map, size_t offset, size_t idx)
{
  assert(sparsemap_get_size(map) >= SM_SIZEOF_OVERHEAD);
  size_t result = 0, prev = 0, count = __sm_get_chunk_map_count(map);
  uint8_t *p = __sm_get_chunk_map_data(map, 0);

  for (size_t i = 0; i < count; i++) {
    sm_idx_t start = *(sm_idx_t *)p;
    if (start > idx) {
      return (result);
    }
    offset -= start - prev;
    prev = start;
    p += sizeof(sm_idx_t);
    __sm_chunk_t chunk;
    __sm_chunk_map_init(&chunk, p);

    result += __sm_chunk_map_rank(&chunk, &offset, idx - start);
    p += __sm_chunk_map_get_size(&chunk);
  }
  return (result);
}

/**
 * Finds a span of set bits of at least |len| after |loc|. Returns the index of
 * the n'th set bit that starts a span of at least |len| bits set to true.
 */
size_t
sparsemap_span(sparsemap_t *map, size_t loc, size_t len)
{
  size_t offset, nth = 0, count;
  (void)loc; // TODO

  offset = sparsemap_select(map, 0);
  if (len == 1) {
    return offset;
  }
  do {
    count = sparsemap_rank(map, offset, offset + len);
    if (count == len) {
      return offset;
    } else {
      count = len;
      while (--count && sparsemap_is_set(map, offset)) {
        nth++;
      }
    }
    offset = sparsemap_select(map, nth);
  } while (offset != ((size_t)-1));

  return offset;
}
