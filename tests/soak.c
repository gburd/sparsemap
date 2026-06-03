#define _POSIX_C_SOURCE 199309L

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <common.h>
#include <roaring.h>
#include <sm.h>
#include <tdigest.h>

#include "midl.c"

#define INITIAL_AMOUNT 1024 * 2

typedef size_t pgno_t;

typedef enum { SM, ML, RB } container_impl_t;

typedef struct container {
  const char *name;
  /* allocate a new container */
  void *(*alloc)(size_t capacity);
  struct {
    td_histogram_t *td;
    void *(*alloc)(size_t capacity);
  } alloc_stats;

  /* free the container */
  void (*free)(void *handle);
  struct {
    td_histogram_t *td;
    void (*free)(void *handle);
  } free_stats;

  /* add pg to the container */
  pgno_t (*set)(void **handle, pgno_t pg);
  struct {
    td_histogram_t *td;
    pgno_t (*set)(void **handle, pgno_t pg);
  } set_stats;
#define timed_set(fn) (pgno_t(*)(void **, pgno_t)) __stats_set, .set_stats.set = fn

  /* is pg in the container */
  bool (*is_set)(void *handle, pgno_t pg);
  struct {
    td_histogram_t *td;
    bool (*is_set)(void *handle, pgno_t pg);
  } is_set_stats;
#define timed_is_set(fn) (bool (*)(void *, pgno_t)) __stats_is_set, .is_set_stats.is_set = fn

  /* remove pg from the container */
  pgno_t (*clear)(void **handle, pgno_t pg);
  struct {
    td_histogram_t *td;
    pgno_t (*clear)(void **handle, pgno_t pg);
  } clear_stats;
#define timed_clear(fn) (pgno_t(*)(void **, pgno_t)) __stats_clear, .clear_stats.clear = fn

  /* find a set of contigious page of len and return the smallest pgno */
  pgno_t (*find_span)(void *handle, unsigned len);
  struct {
    td_histogram_t *td;
    pgno_t (*find_span)(void *handle, unsigned len);
  } find_span_stats;
#define timed_find_span(fn) (pgno_t(*)(void *, unsigned)) __stats_find_span, .find_span_stats.find_span = fn

  /* remove the span [pg, pg + len) from the container */
  bool (*take_span)(void **handle, pgno_t pg, unsigned len);
  struct {
    td_histogram_t *td;
    bool (*take_span)(void **handle, pgno_t pg, unsigned len);
  } take_span_stats;
#define timed_take_span(fn) (bool (*)(void **, pgno_t, unsigned)) __stats_take_span, .take_span_stats.take_span = fn

  /* add the span [pg, pg + len) into the container */
  bool (*release_span)(void **handle, pgno_t pg, unsigned len);
  struct {
    td_histogram_t *td;
    bool (*release_span)(void **handle, pgno_t pg, unsigned len);
  } release_span_stats;
#define timed_release_span(fn) (bool (*)(void **, pgno_t, unsigned)) __stats_release_span, .release_span_stats.release_span = fn

  /* are the pgno in the span [pg, pg+ len) in the container? */
  bool (*is_span)(void *handle, pgno_t pg, unsigned len);
  struct {
    td_histogram_t *td;
    bool (*is_span)(void *handle, pgno_t pg, unsigned len);
  } is_span_stats;
#define timed_is_span(fn) (bool (*)(void *, pgno_t, unsigned)) __stats_is_span, .is_span_stats.is_span = fn

  /* are the pgno in the span [pg, pg+ len) not in the container? */
  bool (*is_empty)(void *handle, pgno_t pg, unsigned len);
  struct {
    td_histogram_t *td;
    bool (*is_empty)(void *handle, pgno_t pg, unsigned len);
  } is_empty_stats;
#define timed_is_empty(fn) (bool (*)(void *, pgno_t, unsigned)) __stats_is_empty, .is_empty_stats.is_empty = fn

  /* is the span the first one (brute force check) */
  bool (*is_first)(void *handle, pgno_t pg, unsigned len);
  struct {
    td_histogram_t *td;
    bool (*is_first)(void *handle, pgno_t pg, unsigned len);
  } is_first_stats;
#define timed_is_first(fn) (bool (*)(void *, pgno_t, unsigned)) __stats_is_first, .is_first_stats.is_first = fn

  /* ensure that all pgno contained in other_handle are also in handle */
  bool (*merge)(void **handle, void *other_handle);
  struct {
    td_histogram_t *td;
    bool (*merge)(void **handle, void *other_handle);
  } merge_stats;
#define timed_merge(fn) (bool (*)(void **, void *)) __stats_merge, .merge_stats.merge = fn

  /* the bytes size of the container */
  size_t (*size)(void *handle);
  td_histogram_t *size_stats;

  /* the number of items in the container */
  size_t (*count)(void *handle);
  td_histogram_t *count_stats;

  /* perform internal validation on the container (optional) */
  bool (*validate)(void *handle);
  td_histogram_t *validate_stats;

} container_t;

/**
 * A "coin toss" function that is critical to the proper operation of the
 * Skiplist.  For example, when `max = 6` this function returns 0 with
 * probability 0.5, 1 with 0.25, 2 with 0.125, etc. until 6 with 0.5^7.
 */
static int
toss(size_t max)
{
  size_t level = 0;
  double probability = 0.5;

  double random_value = (double)xorshift32() / RAND_MAX;
  while (random_value < probability && level < max) {
    level++;
    probability *= 0.5;
  }
  return level;
}

static size_t
b64_encoded_size(size_t inlen)
{
  size_t ret;

  ret = inlen;
  if (inlen % 3 != 0)
    ret += 3 - (inlen % 3);
  ret /= 3;
  ret *= 4;

  return ret;
}

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *
b64_encode(const unsigned char *in, size_t len)
{
  char *out;
  size_t elen;
  size_t i;
  size_t j;
  size_t v;

  if (in == NULL || len == 0)
    return NULL;

  elen = b64_encoded_size(len);
  out = malloc(elen + 1);
  out[elen] = '\0';

  for (i = 0, j = 0; i < len; i += 3, j += 4) {
    v = in[i];
    v = i + 1 < len ? v << 8 | in[i + 1] : v << 8;
    v = i + 2 < len ? v << 8 | in[i + 2] : v << 8;

    out[j] = b64chars[(v >> 18) & 0x3F];
    out[j + 1] = b64chars[(v >> 12) & 0x3F];
    if (i + 1 < len) {
      out[j + 2] = b64chars[(v >> 6) & 0x3F];
    } else {
      out[j + 2] = '=';
    }
    if (i + 2 < len) {
      out[j + 3] = b64chars[v & 0x3F];
    } else {
      out[j + 3] = '=';
    }
  }

  return out;
}

static size_t
b64_decoded_size(const char *in)
{
  size_t len;
  size_t ret;
  size_t i;

  if (in == NULL)
    return 0;

  len = strlen(in);
  ret = len / 4 * 3;

  for (i = len; i-- > 0;) {
    if (in[i] == '=') {
      ret--;
    } else {
      break;
    }
  }

  return ret;
}

#if 0
static void
b64_generate_decode_table()
{
  int inv[80];
  size_t i;

  memset(inv, -1, sizeof(inv));
  for (i = 0; i < sizeof(b64chars) - 1; i++) {
    inv[b64chars[i] - 43] = i;
  }
}
#endif

static int b64invs[] = { 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
  14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46,
  47, 48, 49, 50, 51 };

static int
b64_isvalidchar(char c)
{
  if (c >= '0' && c <= '9')
    return 1;
  if (c >= 'A' && c <= 'Z')
    return 1;
  if (c >= 'a' && c <= 'z')
    return 1;
  if (c == '+' || c == '/' || c == '=')
    return 1;
  return 0;
}

static int
b64_decode(const char *in, unsigned char *out, size_t outlen)
{
  size_t len;
  size_t i;
  size_t j;
  int v;

  if (in == NULL || out == NULL)
    return 0;

  len = strlen(in);
  if (outlen < b64_decoded_size(in) || len % 4 != 0)
    return 0;

  for (i = 0; i < len; i++) {
    if (!b64_isvalidchar(in[i])) {
      return 0;
    }
  }

  for (i = 0, j = 0; i < len; i += 4, j += 3) {
    v = b64invs[in[i] - 43];
    v = (v << 6) | b64invs[in[i + 1] - 43];
    v = in[i + 2] == '=' ? v << 6 : (v << 6) | b64invs[in[i + 2] - 43];
    v = in[i + 3] == '=' ? v << 6 : (v << 6) | b64invs[in[i + 3] - 43];

    out[j] = (v >> 16) & 0xFF;
    if (in[i + 2] != '=')
      out[j + 1] = (v >> 8) & 0xFF;
    if (in[i + 3] != '=')
      out[j + 2] = v & 0xFF;
  }

  return 1;
}

/* recording ------------------------------------------------------------- */

bool recording = false;

static void
record_set_mutation(FILE *out, pgno_t pg)
{
  if (recording) {
    fprintf(out, "set %lu\n", pg);
  }
}

static void
record_clear_mutation(FILE *out, pgno_t pg)
{
  if (recording) {
    fprintf(out, "clear %lu\n", pg);
  }
}

static void
record_take_span_mutation(FILE *out, pgno_t pg, unsigned len)
{
  if (recording) {
    fprintf(out, "take %lu %u\n", pg, len);
  }
}

static void
record_release_span_mutation(FILE *out, pgno_t pg, unsigned len)
{
  if (recording) {
    fprintf(out, "release %lu %u\n", pg, len);
  }
}

static void
__scan_record_offsets(uint32_t v[], size_t n, void *aux)
{
  FILE *out = (FILE *)aux;
  for (size_t i = 0; i < n; i++) {
    fprintf(out, "%u ", v[i]);
  }
}

static void
record_merge_mutation(FILE *out, void *handle)
{
  if (recording) {
    sparsemap_t *map = (sparsemap_t *)handle;
    fprintf(out, "merge %zu ", sm_maximum(map));
    sm_scan(map, __scan_record_offsets, 0, (void *)out);
    fprintf(out, "\n");
  }
}

static void
record_checkpoint(FILE *out, void *handle)
{
  if (recording) {
    sparsemap_t *map = (sparsemap_t *)handle;
    size_t capacity = sm_get_capacity(map);
    size_t buffer_size = sm_get_size(map);
    size_t encoded_size = b64_encoded_size(buffer_size);
    char *encoded = b64_encode(sm_get_data(map), buffer_size);
    fprintf(out, "checkpoint %zu %zu %zu ", capacity, buffer_size, encoded_size);
    fprintf(out, "%s", encoded);
    fprintf(out, "\n");
  }
}

/* sparsemap ------------------------------------------------------------- */

static uint64_t
_sparsemap_set(sparsemap_t **_map, uint64_t idx, bool value)
{
  sparsemap_t *map = *_map, *new_map = NULL;
  do {
    uint64_t l = sm_assign(map, idx, value);
    if (l != idx) {
      if (errno == ENOSPC) {
        size_t capacity = sm_get_capacity(map) + 64;
        new_map = sm_set_data_size(map, NULL, capacity);
        assert(new_map != NULL);
        errno = 0;
        *_map = new_map;
      } else {
        perror("Unable to grow sparsemap");
      }
    } else {
      return l;
    }
  } while (true);
}

static void *
__sm_alloc(size_t capacity)
{
  sparsemap_t *map = sparsemap(capacity);
  assert(map != NULL);
  return map;
}

static void
__sm_free(void *handle)
{
  sparsemap_t *map = (sparsemap_t *)handle;
  free(map);
}

static pgno_t
__sm_set(void **handle, pgno_t pg)
{
  sparsemap_t **map = (sparsemap_t **)handle;
  return (pgno_t)_sparsemap_set(map, pg, true);
}

static bool
__sm_is_set(void *handle, pgno_t pg)
{
  sparsemap_t *map = (sparsemap_t *)handle;
  return sm_contains(map, pg);
}

static pgno_t
__sm_clear(void **handle, pgno_t pg)
{
  sparsemap_t **map = (sparsemap_t **)handle;
  return (pgno_t)_sparsemap_set(map, pg, false);
}

static pgno_t
__sm_find_span(void *handle, unsigned len)
{
  sparsemap_t *map = (sparsemap_t *)handle;
  pgno_t pgno = (pgno_t)sm_span(map, 0, len, true);
  return SM_NOT_FOUND(pgno) ? (pgno_t)-1 : pgno;
}

static bool
__sm_take_span(void **handle, pgno_t pg, unsigned len)
{
  sparsemap_t **map = (sparsemap_t **)handle;
  for (pgno_t i = pg; i < pg + len; i++) {
    assert(_sparsemap_set(map, i, false) == i);
  }
  return true;
}

static bool
__sm_release_span(void **handle, pgno_t pg, unsigned len)
{
  sparsemap_t **map = (sparsemap_t **)handle;
  for (pgno_t i = pg; i < pg + len; i++) {
    assert(_sparsemap_set(map, i, true) == i);
  }
  return true;
}

static bool
__sm_is_span(void *handle, pgno_t pg, unsigned len)
{
  sparsemap_t *map = (sparsemap_t *)handle;
  for (pgno_t i = pg; i < pg + len; i++) {
    if (sm_contains(map, i) != true) {
      return false;
    }
  }
  return true;
}

static bool
__sm_is_empty(void *handle, pgno_t pg, unsigned len)
{
  sparsemap_t *map = (sparsemap_t *)handle;
  for (pgno_t i = 0; i < len; i++) {
    if (sm_contains(map, pg + i) != false) {
      return false;
    }
  }
  return true;
}

static bool
__sm_is_first(void *handle, pgno_t pg, unsigned len)
{
  sparsemap_t *map = (sparsemap_t *)handle;
  for (uint64_t i = 0; i < pg + len; i++) {
    uint64_t j = 0;
    while (sm_contains(map, i + j) == true && j < len) {
      j++;
    }
    if (j == len) {
      return i == pg;
    }
  }
  return false;
}

static bool
__sm_merge(void **handle, void *other_handle)
{
  sparsemap_t **map = (sparsemap_t **)handle;
  sparsemap_t *other = (sparsemap_t *)other_handle;
  sparsemap_t *merged = sm_union(*map, other);
  if (merged == NULL) {
    /* Both empty — nothing to merge, that's fine. */
    return true;
  }
  free(*map);
  *map = merged;
  return true;
}

static size_t
__sm_size(void *handle)
{
  sparsemap_t *map = (sparsemap_t *)handle;
  return sm_get_size(map);
}

static size_t
__sm_count(void *handle)
{
  sparsemap_t *map = (sparsemap_t *)handle;
  return sm_rank(map, 0, SM_IDX_MAX, true);
}

/* midl ------------------------------------------------------------------ */

// static bool __midl_validate(void *handle);

static void *
__midl_alloc(size_t capacity)
{
  MDB_IDL list = mdb_midl_alloc(capacity);
  assert(list != NULL);
  return (void *)list;
}

static void
__midl_free(void *handle)
{
  MDB_IDL list = (MDB_IDL)handle;
  mdb_midl_free(list);
}

static pgno_t
__midl_set(void **handle, pgno_t pg)
{
  // assert(__midl_validate(*handle));
  MDB_IDL *_list = (MDB_IDL *)handle, list = *_list;
  if (list[0] + 1 == list[-1]) {
    assert(mdb_midl_need(_list, list[-1] + 1) == 0);
    list = *_list;
  }
  mdb_midl_xappend(list, pg);
  mdb_midl_sort(list);
  // assert(mdb_midl_insert(list, pg) == 0);
  // assert(__midl_validate(*handle));
  return pg;
}

static bool
__midl_is_set(void *handle, pgno_t pg)
{
  MDB_IDL list = (MDB_IDL)handle;
  pgno_t i = mdb_midl_search(list, pg);
  return i <= list[0] && list[i] == pg;
}

static pgno_t
__midl_clear(void **handle, pgno_t pg)
{
  // assert(__midl_validate(*handle));
  MDB_IDL list = *(MDB_IDL *)handle;
  unsigned len = list[0];
  list[0] = len -= 1;
  for (unsigned j = pg - 1; j < len;)
    list[++j] = list[++pg];
#ifdef MDB_DEBUG
  for (unsigned j = len + 1; j <= list[-1]; j++)
    list[j] = 0;
#endif
  // assert(__midl_validate(*handle));
  return pg;
}

static pgno_t
__midl_find_span(void *handle, unsigned len)
{
  MDB_IDL list = (MDB_IDL)handle;

  /* Seek a big enough contiguous page range. Prefer
     pages at the tail, just truncating the list. */
  int retry = 1;
  unsigned i = 0;
  pgno_t pgno = 0, *mop = list;
  unsigned n2 = len, mop_len = mop[0];
  do {
    if (mop_len > n2) {
      i = mop_len;
      do {
        pgno = mop[i];
        if (mop[i - n2] == pgno + n2)
          goto search_done;
      } while (--i > n2);
      if (--retry < 0)
        break;
    } else {
      return (pgno_t)-1;
    }
  } while (1);
search_done:;
  return retry < 0 ? (pgno_t)-1 : pgno;
}

static bool
__midl_take_span(void **handle, pgno_t pg, unsigned len)
{
  // assert(__midl_validate(*handle));
  MDB_IDL list = *(MDB_IDL *)handle;
  int i = list[list[0]] == pg ? list[0] : mdb_midl_search(list, pg);
  unsigned j, num = len;
  pgno_t *mop = list;
  unsigned mop_len = mop[0];

  mop[0] = mop_len -= num;
  /* Move any stragglers down */
  for (j = i - num; j < mop_len;)
    mop[++j] = mop[++i];
#ifdef MDB_DEBUG
  for (j = mop_len + 1; j <= mop[-1]; j++)
    mop[j] = 0;
#endif
  // assert(__midl_validate(*handle));
  return true;
}

static bool
__midl_release_span(void **handle, pgno_t pg, unsigned len)
{
  // assert(__midl_validate(*handle));
  MDB_IDL *_list = (MDB_IDL *)handle, list = *_list;
  if (list[0] + len >= list[-1]) {
    assert(mdb_midl_need(_list, list[-1] + len) == 0);
    list = *_list;
  }
  for (size_t i = pg; i < pg + len; i++) {
    mdb_midl_xappend(list, i);
    // assert(mdb_midl_insert(list, i) == 0);
  }
  mdb_midl_sort(list);
  // assert(__midl_validate(*handle));
  return true;
}

static bool
__midl_is_span(void *handle, pgno_t pg, unsigned len)
{
  MDB_IDL list = (MDB_IDL)handle;
  pgno_t idx = mdb_midl_search(list, pg);
  bool found = idx <= list[0] && list[idx] == pg;
  if (!found)
    return false;
  if (len == 1)
    return true;
  if (list[idx] + len - 1 != list[idx - len + 1])
    return false;
  return true;
}

static bool
__midl_is_empty(void *handle, pgno_t pg, unsigned len)
{
  MDB_IDL list = (MDB_IDL)handle;
  for (pgno_t i = pg; i < pg + len; i++) {
    pgno_t idx = mdb_midl_search(list, pg);
    bool found = idx <= list[0] && list[idx] == pg;
    if (found)
      return false;
  }
  return true;
}

static bool
__midl_merge(void **handle, void *other_handle)
{
  // assert(__midl_validate(*handle));
  MDB_IDL *_list = (MDB_IDL *)handle, list = *_list, other = (MDB_IDL)other_handle;
  if (list[0] + other[0] >= list[-1]) {
    assert(mdb_midl_need(_list, list[-1] + other[0]) == 0);
    list = *_list;
  }
  mdb_midl_xmerge(list, other_handle);
  mdb_midl_sort(*_list);
  // assert(__midl_validate(*handle));
  return true;
}

static size_t
__midl_size(void *handle)
{
  MDB_IDL list = (MDB_IDL)handle;
  return list[0] * sizeof(pgno_t);
}

static size_t
__midl_count(void *handle)
{
  MDB_IDL list = (MDB_IDL)handle;
  return list[0];
}

static bool
__midl_validate(void *handle)
{
  MDB_IDL list = (MDB_IDL)handle;
  if (list[0] > list[-1]) {
    return false;
  }
  if (list[0] > 1) {
    // check for duplicates
    for (pgno_t i = 2; i < list[0]; i++) {
      if (list[i] == list[i - 1]) {
        return false;
      }
      // ensure ordering
      if (list[i] > list[i - 1])
        return false;
    }
  }
  return true;
}

/* roaring --------------------------------------------------------------- */

static void *
__roar_alloc(size_t capacity)
{
  roaring_bitmap_t *map = roaring_bitmap_create_with_capacity(capacity);
  assert(map != NULL);
  return map;
}

static void
__roar_free(void *handle)
{
  roaring_bitmap_t *rbm = (roaring_bitmap_t *)handle;
  roaring_free(rbm);
}

static pgno_t
__roar_set(void **handle, pgno_t pg)
{
  roaring_bitmap_t **_rbm = (roaring_bitmap_t **)handle, *rbm = *_rbm;
  assert(roaring_bitmap_add_checked(rbm, pg) == true);
  return pg;
}

static bool
__roar_is_set(void *handle, pgno_t pg)
{
  roaring_bitmap_t *rbm = (roaring_bitmap_t *)handle;
  return roaring_bitmap_contains(rbm, pg);
}

static pgno_t
__roar_clear(void **handle, pgno_t pg)
{
  roaring_bitmap_t **_rbm = (roaring_bitmap_t **)handle, *rbm = *_rbm;
  roaring_bitmap_remove(rbm, pg);
  return pg;
}

static pgno_t
__roar_find_span(void *handle, unsigned len)
{
  roaring_bitmap_t *rbm = (roaring_bitmap_t *)handle;
  uint64_t max = roaring_bitmap_maximum(rbm);
  uint64_t offset = roaring_bitmap_minimum(rbm);
  do {
    if (len == 1 || roaring_bitmap_range_cardinality(rbm, offset, offset + len) == len) {
      break;
    }
    offset++;
  } while (offset <= max);
  return offset > max ? (pgno_t)-1 : offset;
}

static bool
__roar_take_span(void **handle, pgno_t pg, unsigned len)
{
  roaring_bitmap_t **_rbm = (roaring_bitmap_t **)handle, *rbm = *_rbm;
  roaring_bitmap_remove_range(rbm, pg, pg + len);
  return true;
}

static bool
__roar_release_span(void **handle, pgno_t pg, unsigned len)
{
  roaring_bitmap_t **_rbm = (roaring_bitmap_t **)handle, *rbm = *_rbm;
  for (size_t i = pg; i < pg + len; i++) {
    assert(roaring_bitmap_add_checked(rbm, i) == true);
  }
  return true;
}

static bool
__roar_is_span(void *handle, pgno_t pg, unsigned len)
{
  roaring_bitmap_t *rbm = (roaring_bitmap_t *)handle;
  return roaring_bitmap_contains_range(rbm, pg, pg + len);
}

static bool
__roar_is_empty(void *handle, pgno_t pg, unsigned len)
{
  roaring_bitmap_t *rbm = (roaring_bitmap_t *)handle;
  return !roaring_bitmap_contains_range(rbm, pg, pg + len);
}

static bool
__roar_merge(void **handle, void *other_handle)
{
  roaring_bitmap_t **_rbm = (roaring_bitmap_t **)handle, *rbm = *_rbm;
  roaring_bitmap_t *other = (roaring_bitmap_t *)other_handle;
  roaring_bitmap_or_inplace(rbm, other);
  return true;
}

static size_t
__roar_size(void *handle)
{
  return roaring_bitmap_frozen_size_in_bytes((roaring_bitmap_t *)handle);
}

static size_t
__roar_count(void *handle)
{
  return roaring_bitmap_get_cardinality((roaring_bitmap_t *)handle);
}

static bool
__roar_validate(void *handle)
{
  roaring_bitmap_t *rbm = (roaring_bitmap_t *)handle;
  roaring_bitmap_run_optimize(rbm);
  return true;
}

/* statistics ------------------------------------------------------------ */

bool statistics = false;

typedef struct sw {
  struct timespec t1; /* start time */
  struct timespec t2; /* stop time */
} sw_t;

void
ts(struct timespec *ts)
{
  if (clock_gettime(CLOCK_REALTIME, ts) == -1) {
    perror("clock_gettime");
  }
}

static double
elapsed(struct timespec *s, struct timespec *e)
{
  long sec, nanos;

  sec = e->tv_sec - s->tv_sec;
  nanos = e->tv_nsec - s->tv_nsec;
  if (nanos < 0) {
    nanos += 1e9;
    sec--;
  }
  return ((double)nanos / 1e9 + (double)sec);
}

static pgno_t
__stats_set(td_histogram_t *stats, void *fn, void **handle, pgno_t pg)
{
  if (statistics && stats) {
    struct timespec s, e;
    ts(&s);
    pgno_t retval = ((pgno_t(*)(void **, pgno_t))fn)(handle, pg);
    ts(&e);
    td_add(stats, elapsed(&s, &e), 1);
    return retval;
  }
  return ((pgno_t(*)(void **, pgno_t))fn)(handle, pg);
}

static bool
__stats_is_set(td_histogram_t *stats, void *fn, void *handle, pgno_t pg)
{
  if (statistics && stats) {
    struct timespec s, e;
    ts(&s);
    bool retval = ((bool (*)(void *, pgno_t))fn)(handle, pg);
    ts(&e);
    td_add(stats, elapsed(&s, &e), 1);
    return retval;
  }
  return ((bool (*)(void *, pgno_t))fn)(handle, pg);
}

static pgno_t
__stats_clear(td_histogram_t *stats, void *fn, void **handle, pgno_t pg)
{
  if (statistics && stats) {
    struct timespec s, e;
    ts(&s);
    pgno_t retval = ((pgno_t(*)(void **, pgno_t))fn)(handle, pg);
    ts(&e);
    td_add(stats, elapsed(&s, &e), 1);
    return retval;
  }
  return ((pgno_t(*)(void **, pgno_t))fn)(handle, pg);
}

static pgno_t
__stats_find_span(td_histogram_t *stats, void *fn, void *handle, unsigned len)
{
  if (statistics && stats) {
    struct timespec s, e;
    ts(&s);
    pgno_t retval = ((pgno_t(*)(void *, unsigned))fn)(handle, len);
    ts(&e);
    td_add(stats, elapsed(&s, &e), 1);
    return retval;
  }
  return ((pgno_t(*)(void *, unsigned))fn)(handle, len);
}

static bool
__stats_take_span(td_histogram_t *stats, void *fn, void **handle, pgno_t pg, unsigned len)
{
  if (statistics && stats) {
    struct timespec s, e;
    ts(&s);
    bool retval = ((bool (*)(void *, pgno_t, unsigned))fn)(handle, pg, len);
    ts(&e);
    td_add(stats, elapsed(&s, &e), 1);
    return retval;
  }
  return ((bool (*)(void *, pgno_t, unsigned))fn)(handle, pg, len);
}

static bool
__stats_release_span(td_histogram_t *stats, void *fn, void **handle, pgno_t pg, unsigned len)
{
  if (statistics && stats) {
    struct timespec s, e;
    ts(&s);
    bool retval = ((bool (*)(void *, pgno_t, unsigned))fn)(handle, pg, len);
    ts(&e);
    td_add(stats, elapsed(&s, &e), 1);
    return retval;
  }
  return ((bool (*)(void *, pgno_t, unsigned))fn)(handle, pg, len);
}

static bool
__stats_is_span(td_histogram_t *stats, void *fn, void *handle, pgno_t pg, unsigned len)
{
  if (statistics && stats) {
    struct timespec s, e;
    ts(&s);
    bool retval = ((bool (*)(void *, pgno_t, unsigned))fn)(handle, pg, len);
    ts(&e);
    td_add(stats, elapsed(&s, &e), 1);
    return retval;
  }
  return ((bool (*)(void *, pgno_t, unsigned))fn)(handle, pg, len);
}

static bool
__stats_is_empty(td_histogram_t *stats, void *fn, void *handle, pgno_t pg, unsigned len)
{
  if (statistics && stats) {
    struct timespec s, e;
    ts(&s);
    bool retval = ((bool (*)(void *, pgno_t, unsigned))fn)(handle, pg, len);
    ts(&e);
    td_add(stats, elapsed(&s, &e), 1);
    return retval;
  }
  return ((bool (*)(void *, pgno_t, unsigned))fn)(handle, pg, len);
}

static bool
__stats_is_first(td_histogram_t *stats, void *fn, void *handle, pgno_t pg, unsigned len)
{
  if (statistics && stats) {
    struct timespec s, e;
    ts(&s);
    bool retval = ((bool (*)(void *, pgno_t, unsigned))fn)(handle, pg, len);
    ts(&e);
    td_add(stats, elapsed(&s, &e), 1);
    return retval;
  }
  return ((bool (*)(void *, pgno_t, unsigned))fn)(handle, pg, len);
}

static bool
__stats_merge(td_histogram_t *stats, void *fn, void **handle, void *other_handle)
{
  if (statistics && stats) {
    struct timespec s, e;
    ts(&s);
    bool retval = ((bool (*)(void **, void *))fn)(handle, other_handle);
    ts(&e);
    td_add(stats, elapsed(&s, &e), 1);
    return retval;
  }
  return ((bool (*)(void **, void *))fn)(handle, other_handle);
}

/* ----------------------------------------------------------------------- */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
// clang-format off
container_t containers[] = {
  { "sparsemap",
    .alloc = __sm_alloc,
    .free = __sm_free,
    .set = timed_set(__sm_set),
    .is_set = timed_is_set(__sm_is_set),
    .clear = timed_clear(__sm_clear),
    .find_span = timed_find_span(__sm_find_span),
    .take_span = timed_take_span(__sm_take_span),
    .release_span = timed_release_span(__sm_release_span),
    .is_span = timed_is_span(__sm_is_span),
    .is_empty = timed_is_empty(__sm_is_empty),
    .is_first = timed_is_first(__sm_is_first),
    .merge = timed_merge(__sm_merge),
    .size = __sm_size,
    .count = __sm_count,
    .validate = NULL,
  },
  { "midl",
    .alloc = __midl_alloc,
    .free = __midl_free,
    .set = timed_set(__midl_set),
    .is_set = timed_is_set(__midl_is_set),
    .clear = timed_clear(__midl_clear),
    .find_span = timed_find_span(__midl_find_span),
    .take_span = timed_take_span(__midl_take_span),
    .release_span = timed_release_span(__midl_release_span),
    .is_span = timed_is_span(__midl_is_span),
    .is_empty = timed_is_empty(__midl_is_empty),
    .is_first = timed_is_first(NULL),
    .merge = timed_merge(__midl_merge),
    .size = __midl_size,
    .count = __midl_count,
    .validate = __midl_validate,
  },
  { "roaring",
    .alloc = __roar_alloc,
    .free = __roar_free,
    .set = timed_set(__roar_set),
    .is_set = timed_is_set(__roar_is_set),
    .clear = timed_clear(__roar_clear),
    .find_span = timed_find_span(__roar_find_span),
    .take_span = timed_take_span(__roar_take_span),
    .release_span = timed_release_span(__roar_release_span),
    .is_span = timed_is_span(__roar_is_span),
    .is_empty = timed_is_empty(__roar_is_empty),
    .is_first = timed_is_first(NULL),
    .merge = timed_merge(__roar_merge),
    .size = __roar_size,
    .count = __roar_count,
    .validate = __roar_validate,
  },
};
// clang-format on
#pragma GCC diagnostic pop

/* ----------------------------------------------------------------------- */

void *handles[(sizeof((containers)) / sizeof((containers)[0]))];
void *new_handles[(sizeof((containers)) / sizeof((containers)[0]))];
FILE *record_fp;
FILE *stats_fp;

#define alloc(type, size) containers[type].alloc(size);
#define cast(type, fn, ...) \
  if (containers[type].fn)  \
  containers[type].fn(handles[type], ##__VA_ARGS__)

#define invoke(type, fn, ...) __stats_##fn(containers[type].fn##_stats.td, containers[type].fn##_stats.fn, handles[type], __VA_ARGS__)
#define mutate(type, fn, ...)                                             \
  (type == 0) ? record_##fn##_mutation(record_fp, __VA_ARGS__) : (void)0, \
    __stats_##fn(containers[type].fn##_stats.td, containers[type].fn##_stats.fn, &handles[type], __VA_ARGS__)
#define foreach(set) for (unsigned type = 0; type < (sizeof((set)) / sizeof((set)[0])); type++)
#define checkpoint(set)                                                        \
  for (unsigned type = 1; type < (sizeof((set)) / sizeof((set)[0])); type++) { \
    verify_eq(0, handles[0], type, handles[type]);                             \
  }                                                                            \
  record_checkpoint(record_fp, handles[0])

bool
verify_sm_eq_rb(sparsemap_t *map, roaring_bitmap_t *rbm)
{
  bool ret = true;
  uint64_t max = roaring_bitmap_maximum(rbm);
  roaring_uint32_iterator_t iter;
  roaring_iterator_init(rbm, &iter);
  for (uint64_t i = 0; i <= max; i++) {
    if (i == iter.current_value) {
      if (sm_contains(map, i) == false) {
        fprintf(stdout, "- %zu ", i);
        ret = false;
      }
      roaring_uint32_iterator_advance(&iter);
    } else {
      if (sm_contains(map, i) == true) {
        fprintf(stdout, "+ %zu ", i);
        ret = false;
      }
    }
  }
  return ret;
}

bool
verify_sm_eq_ml(sparsemap_t *map, MDB_IDL list)
{
  bool ret = true;
  for (MDB_ID i = 1; i <= list[0]; i++) {
    pgno_t pg = list[i];
    unsigned skipped = i == 1 ? 0 : list[i - 1] - list[i] - 1;
    if (skipped) {
      for (MDB_ID j = list[i - 1]; j > list[i]; j--) {
        if (sm_contains(map, pg - j) != false) {
          fprintf(stdout, "+ %zu ", pg - j);
          ret = false;
        }
      }
    }
    if (sm_contains(map, pg) != true) {
      fprintf(stdout, "- %zu ", pg);
      ret = false;
    }
  }
  return ret;
}

bool
verify_eq(unsigned a, void *ad, unsigned b, void *bd)
{
  bool ret = true;

  // 'a' should always be a Sparsemap
  assert(a == SM);
  switch (b) {
  case ML:
    assert((ret = verify_sm_eq_ml((sparsemap_t *)ad, (MDB_IDL)bd)) == true);
    break;
  case RB:
    assert((ret = verify_sm_eq_rb((sparsemap_t *)ad, (roaring_bitmap_t *)bd)) == true);
    break;
  default:
    break;
  }
  return ret;
}

void
print_usage(const char *program_name)
{
  printf("Usage: %s [OPTIONS]\n", program_name);
  printf("  -r <file>    Path to the file for recording (optional)\n");
  printf("  -s <file>    Path to the file for statistics (optional)\n");
  printf("  -f           Force overwrite of existing file (optional)\n");
  printf("  -b           Disable buffering writes to stdout/err (optional)\n");
  printf("  -a <number>  Specify the number of entries to record (must be positive, optional)\n");
  printf("  -h           Print this help message\n");
}
#define SHORT_OPT "r:s:fa:bh"

int
main(int argc, char *argv[])
{
  int opt;
  const char *record_file = NULL;
  const char *stats_file = NULL;
  int force_flag = 0;
  size_t left, iteration = 0, amt = INITIAL_AMOUNT;
  bool buffer = true;

  while ((opt = getopt(argc, argv, SHORT_OPT)) != -1) {
    switch (opt) {
    case 'r':
      recording = true;
      record_file = optarg;
      break;
    case 's':
      statistics = true;
      stats_file = optarg;
      break;
    case 'f':
      force_flag = 1;
      break;
    case 'b':
      buffer = false;
      break;
    case 'a':
      amt = atoi(optarg);
      if (amt <= 0) {
        fprintf(stderr, "Error: Invalid amount. Amount must be a positive number.\n");
        return 1;
      }
      break;
    case 'h':
      print_usage(argv[0]);
      return 0;
    case '?':
      fprintf(stderr, "Unknown option: %c\n", optopt);
      return 1;
    default:
      break;
    }
  }

  if (recording) {
    record_fp = stdout;

    // Check for existing file without force flag
    if (access(record_file, F_OK) == 0 && !force_flag) {
      fprintf(stderr, "Warning: File '%s' already exists. Use -f or --force to overwrite.\n", record_file);
      return 1;
    }

    // Open the file for writing (truncate if force flag is set)
    record_fp = fopen(record_file, force_flag ? "w" : "a");
    if (record_fp == NULL) {
      perror("Error opening file");
      return 1;
    }
  }

  // Check if statistics file is specified
  if (statistics) {
    if (stats_file[0] == '-') {
      stats_fp = stdout;
      setvbuf(stdout, NULL, _IONBF, 0);
    } else {
      // Check for existing file without force flag
      if (access(stats_file, F_OK) == 0 && !force_flag) {
        fprintf(stderr, "Warning: File '%s' already exists. Use -f or --force to overwrite.\n", stats_file);
        return 1;
      }

      // Open the file for writing (truncate if force flag is set)
      stats_fp = fopen(stats_file, force_flag ? "w" : "a");
      if (stats_fp == NULL) {
        perror("Error opening file");
        return 1;
      }
    }
  }

  // disable buffering
  if (!buffer) {
    setvbuf(record_fp, NULL, _IONBF, 0);
    setvbuf(stats_fp, NULL, _IONBF, 0);
  }

  const char *names[] = { "sm", "ml", "rb" };
  unsigned types[] = { SM, ML, RB };
  unsigned num_types = (sizeof((types)) / sizeof((types)[0]));
  foreach(types)
  {
    containers[type].alloc_stats.td = NULL;
    containers[type].free_stats.td = NULL;
    containers[type].set_stats.td = td_new(100);
    containers[type].is_set_stats.td = td_new(100);
    containers[type].clear_stats.td = td_new(100);
    containers[type].find_span_stats.td = td_new(100);
    containers[type].take_span_stats.td = td_new(100);
    containers[type].release_span_stats.td = td_new(100);
    containers[type].is_span_stats.td = NULL;
    containers[type].is_empty_stats.td = NULL;
    containers[type].is_first_stats.td = NULL;
    containers[type].merge_stats.td = td_new(100);
  }

#define digest(idx) *((td_histogram_t **)((uintptr_t)(containers + type) + digests[(idx)].td_offset))
#define digest_offset(name) (offsetof(container_t, name##_stats.td))

  // clang-format off
  static struct {
    size_t td_offset;
    const char *statistic;
  } digests[] = {
    { .td_offset = digest_offset(set), .statistic = "set" },
    { .td_offset = digest_offset(is_set), .statistic = "is_set" },
    { .td_offset = digest_offset(clear), .statistic = "clear" },
    { .td_offset = digest_offset(find_span), .statistic = "find_span" },
    { .td_offset = digest_offset(take_span), .statistic = "take_span" },
    { .td_offset = digest_offset(release_span), .statistic = "release_span" },
    { .td_offset = digest_offset(is_span), .statistic = "is_span" },
    { .td_offset = digest_offset(is_empty), .statistic = "is_empty" },
    { .td_offset = digest_offset(is_first), .statistic = "is_first" },
    { .td_offset = digest_offset(merge), .statistic = "merge" },
  };
  size_t num_digests = (sizeof((digests)) / sizeof((digests)[0]));

  static struct {
    double numeric;
    const char *name;
  } pctils[] = {
    { .numeric = 0.5, .name = "p50" },
    { .numeric = 0.75, .name = "p75" },
    { .numeric = 0.90, .name = "p90" },
    { .numeric = 0.99, .name = "p99" },
    { .numeric = 0.999, .name = "p999" },
  };
  // clang-format on
  size_t num_pctils = (sizeof((pctils)) / sizeof((pctils)[0]));

  /* Setup: add an amt of bits to each container. */
  foreach(types)
  {
    handles[type] = alloc(type, amt);
    for (size_t i = 0; i < amt; i++) {
      assert(invoke(type, is_set, i) == false);
      assert(mutate(type, set, i) == i);
      assert(invoke(type, is_set, i) == true);
    }
    cast(type, validate);
  }
  checkpoint(types);
  left = amt;

  while (true) {
    iteration++;
    // the an amount [1, 16] of pages to find preferring smaller sizes
    unsigned len = toss(15) + 1;
    pgno_t loc[num_types];
    foreach(types)
    {
      loc[type] = invoke(type, find_span, len);
      if (loc[type] == (pgno_t)-1) {
        goto larger_please;
      }
    }
    for (unsigned n = 0; n < num_types; n++) {
      foreach(types)
      {
        assert(invoke(type, is_span, loc[n], len));
      }
    }
    foreach(types)
    {
      cast(type, validate);
    }

    unsigned which_loc = (unsigned)xorshift32() % num_types;
    foreach(types)
    {
      assert(mutate(type, take_span, loc[which_loc], len));
      cast(type, validate);
    }
    checkpoint(types);
    left -= len;

    if (toss(7) == 6) {
      do {
        pgno_t pgno;
        size_t len, retries = amt / 10;
        // Find a hole in the map to replenish.
        do {
          len = toss(15) + 1;
          pgno = sm_span(handles[SM], 0, len, false);
        } while (SM_NOT_FOUND(pgno) && --retries);
        if (retries == 0) {
          goto larger_please;
        }
        if (SM_FOUND(pgno)) {
          foreach(types)
          {
            assert(invoke(type, is_empty, pgno, len));
          }
          checkpoint(types);
          foreach(types)
          {
            assert(invoke(type, is_empty, pgno, len));
            assert(mutate(type, release_span, pgno, len));
            assert(invoke(type, is_span, pgno, len));
            cast(type, validate);
          }
          checkpoint(types);
          left += len;
        }
      } while (toss(4) < 3);
    }

    // if (toss(10) > 8) {
    if (0) {
      size_t new_offset, new_amt;
    larger_please:
      new_amt = 1024 + (xorshift32() % 2048) + toss(1024);
      new_offset = sm_maximum(handles[SM]) + 1;

      // Build a new container to merge with the existing one.
      foreach(types)
      {
        new_handles[type] = alloc(type, new_amt);
        for (size_t i = 0; i < new_amt; i++) {
          // We don't want to record and we're using new_handles not
          // handles, so call fn directly.
          assert(containers[type].is_set_stats.is_set(handles[type], i + new_offset) == false);
          assert(containers[type].is_set_stats.is_set(new_handles[type], i + new_offset) == false);
          containers[type].set_stats.set(&new_handles[type], i + new_offset);
          assert(containers[type].is_set_stats.is_set(new_handles[type], i + new_offset) == true);
        }
      }
      foreach(types)
      {
        assert(mutate(type, merge, new_handles[type]));
        cast(type, validate);
      }
      checkpoint(types);
      left += new_amt;
      amt += new_amt;
      foreach(types)
      {
        containers[type].free(new_handles[type]);
      }
    }

    if (statistics) {
      if (iteration > 1) {
        fprintf(stats_fp, "%f,%zu,", nsts(), iteration);
      }
      foreach(types)
      {
        if (iteration == 1 && type == SM) {
          fprintf(stats_fp, "timestamp,iterations,");
          fprintf(stats_fp, "%s_size,%s_bytes,", names[type], names[type]);
          for (size_t i = 0; i < num_digests; i++) {
            td_histogram_t *h = digest(i);
            if (h) {
              for (size_t j = 0; j < num_pctils; j++) {
                fprintf(stats_fp, "%s_%s_%s,", names[type], digests[i].statistic, pctils[j].name);
              }
            }
          }
          fprintf(stats_fp, "\n");
        }

        if (iteration > 1) {
          fprintf(stats_fp, "%zu,%zu,", containers[type].count(handles[type]), containers[type].size(handles[type]));
          for (size_t i = 0; i < num_digests; i++) {
            td_histogram_t *h = digest(i);
            if (h) {
              td_compress(h);
              for (size_t j = 0; j < num_pctils; j++) {
                fprintf(stats_fp, "%.10f,", td_quantile(h, pctils[j].numeric));
              }
            }
          }
        }
      }
      fprintf(stats_fp, "\n");
    }
  }
  return 0;
}
