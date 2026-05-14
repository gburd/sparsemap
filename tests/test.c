/* SPDX-License-Identifier: MIT */
/*
 * sparsemap is MIT-licensed, but for this file:
 *
 * To the extent possible under law, the author(s) of this file have
 * waived all copyright and related or neighboring rights to this
 * work.  See <https://creativecommons.org/publicdomain/zero/1.0/> for
 * details.
 */

#define MUNIT_NO_FORK (1)
#define MUNIT_ENABLE_ASSERT_ALIASES (1)

#include <common.h>
#include <errno.h>
#include <munit.h>
#include <qc.h>
#include <sparsemap.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define munit_free free

#if defined(_MSC_VER)
#pragma warning(disable : 4127)
#endif

#define SELECT_FALSE

char *QCC_showSparsemap(void *value, int len);
char *QCC_showChunk(void *value, int len);

/* !!! Duplicated here for testing purposes. Keep in sync, or suffer. !!! */
/* !!! Duplicated here for testing purposes. Keep in sync, or suffer. !!! */
struct sparsemap {
  size_t m_capacity;
  size_t m_data_used;
  uint8_t *m_data;
  uint8_t m_alloc_kind;
  const sm_allocator_t *m_allocator;
};

struct user_data {
  int foo;
};

/* -------------------------- Supporting Functions for Testing */

size_t
populate_map_rle(sparsemap_t *map, size_t loc, size_t num, size_t amount)
{
  size_t i, len = (num <= 1 ? 1 : munit_rand_int_range(1, num)) * amount;
  for (i = 0; i < len; i++) {
    sm_add(map, loc + i);
  }
  return i;
}

size_t
populate_map(sparsemap_t *map, int size, int max_value)
{
  int array[size];
  size_t i, before;

  setup_test_array(array, size, max_value);
  shuffle(array, size);
  before = sm_cardinality(map);
  for (i = 0; i < (size_t)size; i++) {
    sm_add(map, array[i]);
    bool set = sm_contains(map, array[i]);
    assert_true(set);
  }
  assert_true(sm_cardinality(map) == before + size);

  return i;
}

static void *
test_api_setup(const MunitParameter params[], void *user_data)
{
  struct test_info *info = (struct test_info *)user_data;
  (void)info;
  (void)params;
  sparsemap_t *map = munit_calloc(1, sizeof(sparsemap_t));
  assert_ptr_not_null(map);
  return (void *)(uintptr_t)map;
}

static void
test_api_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}

/* -------------------------- API Tests */

static MunitResult
test_api_new(const MunitParameter params[], void *data)
{
  sparsemap_t *map = sparsemap(1024);
  (void)params;
  (void)data;

  assert_ptr_not_null(map);
  assert_true(map->m_capacity == 1024);
  assert_true(map->m_data_used == sizeof(uint32_t));
  assert_true((((uint8_t)map->m_data[0]) & 0x03) == 0x00);

  munit_free(map);

  return MUNIT_OK;
}

static MunitResult
test_api_new_realloc(const MunitParameter params[], void *data)
{
  sparsemap_t *map = sparsemap(1024);
  (void)params;
  (void)data;

  assert_ptr_not_null(map);
  assert_true(map->m_capacity == 1024);
  assert_true(map->m_data_used == sizeof(uint32_t));

  map = sm_set_data_size(map, NULL, 2048);
  assert_true(map->m_capacity == 2048);
  assert_true(map->m_data_used == sizeof(uint32_t));

  munit_free(map);

  return MUNIT_OK;
}

static MunitResult
test_api_new_heap(const MunitParameter params[], void *data)
{
  sparsemap_t *map;
  uint8_t *buf;
  (void)params;
  (void)data;

  map = munit_malloc(sizeof(sparsemap_t));
  assert_ptr_not_null(map);
  buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sm_init(map, buf, 1024);

  sm_init(map, buf, 1024);
  assert_ptr_equal(buf, map->m_data);
  assert_true(map->m_capacity == 1024);
  assert_true(map->m_data_used == sizeof(uint32_t));

  munit_free(map->m_data);
  munit_free(map);

  return MUNIT_OK;
}

static MunitResult
test_api_new_static(const MunitParameter params[], void *data)
{
  sparsemap_t a_map, *map = &a_map;
  uint8_t *buf;
  (void)params;
  (void)data;

  buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sm_init(map, buf, 1024);

  sm_init(map, buf, 1024);
  assert_ptr_equal(buf, map->m_data);
  assert_true(map->m_capacity == 1024);
  assert_true(map->m_data_used == sizeof(uint32_t));

  munit_free(map->m_data);

  return MUNIT_OK;
}

static MunitResult
test_api_new_stack(const MunitParameter params[], void *data)
{
  sparsemap_t a_map, *map = &a_map;
  uint8_t buf[1024] = { 0 };

  (void)params;
  (void)data;

  sm_init(map, buf, 1024);
  assert_ptr_equal(&buf, map->m_data);
  assert_true(map->m_capacity == 1024);
  assert_true(map->m_data_used == sizeof(uint32_t));

  return MUNIT_OK;
}

static void *
test_api_clear_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_clear_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_clear(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  sm_add(map, 42);
  assert_true(sm_contains(map, 42));
  sm_clear(map);
  assert_false(sm_contains(map, 42));

  return MUNIT_OK;
}

static void *
test_api_open_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_open_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_open(const MunitParameter params[], void *data)
{
  sparsemap_t _sm, *sm = &_sm, *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  sm_open(sm, (uint8_t *)map->m_data, map->m_capacity);
  for (int i = 0; i < 3 * 1024; i++) {
    assert_true(sm_contains(sm, i) == sm_contains(map, i));
  }

  return MUNIT_OK;
}

static void *
test_api_set_data_size_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_set_data_size_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_set_data_size(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);
  assert_true(map->m_capacity == 1024);
  assert_true(map->m_capacity == sm_get_capacity(map));
  sm_set_data_size(map, NULL, 512);
  assert_true(map->m_capacity == 512);
  assert_true(map->m_capacity == sm_get_capacity(map));
  return MUNIT_OK;
}

static void *
test_api_remaining_capacity_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_remaining_capacity_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_remaining_capacity(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  int i = 0;
  double cap;
  do {
    sm_add(map, i++ * 2);
    cap = sm_capacity_remaining(map);
  } while (cap > 1.0 && errno != ENOSPC);
  errno = 0;
  assert_true(cap <= 2.0);

  sm_clear(map);
  cap = sm_capacity_remaining(map);
  assert_true(cap > 99);

  i = 0;
  do {
    int p = munit_rand_int_range(0, 150000);
    sm_add(map, p);
    i++;
    cap = sm_capacity_remaining(map);
  } while (cap > 1.0 && errno != ENOSPC);
  errno = 0;
  assert_true(cap <= 2.0);

  return MUNIT_OK;
}

static void *
test_api_get_capacity_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);
  populate_map(map, 1024, 3 * 1024);
  populate_map_rle(map, 3 * 1024, 5, 4096);

  return (void *)map;
}
static void
test_api_get_capacity_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_get_capacity(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  sm_add(map, 42);
  assert_true(sm_contains(map, 42));
  assert_true(sm_get_capacity(map) == 1024);

  return MUNIT_OK;
}

static void *
test_api_is_set_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_is_set_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_is_set(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  sm_add(map, 42);
  assert_true(sm_contains(map, 42));

  sm_clear(map);
  size_t n = populate_map_rle(map, 0, 10, 2718);

  for (size_t i = 0; i < n; i++) {
    assert_true(sm_contains(map, i));
  }

  return MUNIT_OK;
}

static void *
test_api_set_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_set_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_set(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  assert_false(sm_contains(map, 1));
  assert_false(sm_contains(map, 8192));
  sm_add(map, 1);
  sm_add(map, 8192);
  assert_true(sm_contains(map, 1));
  assert_true(sm_contains(map, 8192));
  sm_remove(map, 1);
  sm_remove(map, 8192);
  assert_false(sm_contains(map, 1));
  assert_false(sm_contains(map, 8192));

  return MUNIT_OK;
}

static void *
test_api_get_size_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);
  populate_map(map, 1024, 3 * 1024);
  populate_map_rle(map, 3 * 1024, 5, 4096);

  return (void *)map;
}
static void
test_api_get_size_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_get_size(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  size_t size = sm_get_size(map);
  assert_true(size > 400);

  return MUNIT_OK;
}

static void *
test_api_count_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_count_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_count(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  assert_true(sm_cardinality(map) == 1024);

  sm_clear(map);
  sm_add(map, 0);
  assert_true(sm_cardinality(map) == 1);

  sm_add(map, 8675309);
  assert_true(sm_cardinality(map) == 2);

  sm_clear(map);
  for (int i = 0; i < 512; i++) {
    sm_add(map, i + 13);
  }
  assert_true(sm_cardinality(map) == 512);

  sm_clear(map);
  size_t n = populate_map_rle(map, 3 * 1024, 7, 4001);
  assert_true(sm_cardinality(map) == n);

  sm_clear(map);
  assert_true(sm_cardinality(map) == 0);

  return MUNIT_OK;
}

static MunitResult
test_api_get_data(const MunitParameter params[], void *data)
{
  (void)data;
  (void)params;
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)sm_wrap(buf, 1024);
  assert_ptr_not_null(map);
  populate_map(map, 1024, 3 * 1024);

  assert_true(sm_get_data(map) == buf);

  munit_free(buf);
  munit_free(map);

  return MUNIT_OK;
}

static void *
test_api_get_start_offset_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_get_start_offset_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_get_start_offset(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  sm_add(map, 0);
  assert_true(sm_minimum(map) == 0);
  sm_clear(map);

  sm_add(map, 1);
  assert_true(sm_minimum(map) == 1);
  sm_clear(map);

  sm_add(map, 1025);
  assert_true(sm_minimum(map) == 1025);
  sm_clear(map);

  for (int i = 0; i < 1000; i++) {
    sm_add(map, i);
  }
  assert_true(sm_minimum(map) == 0);
  sm_clear(map);

  for (int i = 0; i < 1000; i++) {
    sm_add(map, i + 1024);
  }
  assert_true(sm_minimum(map) == 1024);
  sm_clear(map);

  sm_add(map, 13012);
  assert_true(sm_minimum(map) == 13012);

  return MUNIT_OK;
}

static void *
test_api_get_end_offset_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_get_end_offset_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_get_end_offset(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  sm_add(map, 0);
  assert_true(sm_maximum(map) == 0);
  sm_clear(map);

  sm_add(map, 0);
  sm_add(map, 1);
  assert_true(sm_maximum(map) == 1);
  sm_clear(map);

  sm_add(map, 0);
  sm_add(map, 67);
  sm_add(map, 1002);
  sm_add(map, 3087);
  sm_add(map, 13012);
  assert_true(sm_maximum(map) == 13012);

  sm_clear(map);
  size_t n = populate_map_rle(map, 13012, 10, 2718);
  assert_true(sm_maximum(map) == 13012 + n - 1);
  sm_add(map, 13012 + n + 100);
  assert_true(sm_maximum(map) == 13012 + 100 + n);

  return MUNIT_OK;
}

static void *
test_api_get_start_offset_roll_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  sparsemap_t *map = sparsemap(10 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_api_get_start_offset_roll_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_api_get_start_offset_roll(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  for (uint64_t i = 0; i < 10 * 2048; i++) {
    sm_add(map, i);
    if (i > 2047) {
      sm_remove(map, i - 2048);
      // if (sm_minimum(map) != i - 2047) {
      //   fprintf(stdout, "\n%s\n", QCC_showSparsemap(map, 0));
      //   fprintf(stdout, "%ld\t%ld\t%zu\n", i, i - 2047, sm_minimum(map));
      // }
      assert_true(sm_minimum(map) == i - 2047);
    }
  }
  return MUNIT_OK;
}

static void *
test_api_scan_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);
  sm_init(map, buf, 1024);
  return (void *)map;
}
static void
test_api_scan_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
void
scan_for_0xfeedfacebadcoffee(uint32_t v[], size_t n, void *aux)
{
  size_t bit_pos[] = { 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 22, 23, 24, 26, 27, 29, 31, 32, 33, 34, 35, 38, 39, 41, 43, 44, 45, 46, 47, 48, 50, 51,
    53, 54, 55, 57, 58, 59, 60, 61, 62, 63 };
  (void)aux;

  for (size_t i = 0; i < n; i++) {
    assert(v[i] == bit_pos[i]);
  }
}
static MunitResult
test_api_scan(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);
  sm_bitmap_from_uint64(map, 0, ((uint64_t)0xfeedface << 32) | 0xbadc0ffee);
  sm_scan(map, scan_for_0xfeedfacebadcoffee, 0, NULL);

  return MUNIT_OK;
}

static void *
test_api_split_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  sparsemap_t *map = sparsemap(10 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_api_split_tear_down(void *fixture)
{
  munit_free(fixture);
}
static MunitResult
test_api_split(const MunitParameter params[], void *data)
{
  uint64_t offset;
  sparsemap_t *map = (sparsemap_t *)data;
  uint8_t buf[1024] = { 0 };
  sparsemap_t portion;
  (void)params;

  assert_ptr_not_null(map);

  sm_init(&portion, buf, 1024);

  size_t amt = populate_map_rle(map, 0, 7, 8178);
  sparsemap_t *current = map;
  for (uint64_t i = 0; i < amt + 2049; i++) {
    sm_clear(&portion);
    size_t rank = sm_rank(current, 0, i, true);
    offset = sm_split(current, i + 1, &portion);
    assert_true(sm_cardinality(current) == rank);
    assert_true(sm_cardinality(&portion) == amt - rank);
    sparsemap_t *merged = sm_union(current, &portion);
    assert_ptr_not_null(merged);
    if (current != map) free(current);
    current = merged;
  }
  if (current != map) free(current);

  sm_clear(map);
  sm_clear(&portion);

  for (uint64_t off = 0; off < 1024; off++) {
    for (uint64_t seg = 0; seg < 10 * 1024; seg += 1024) {

      for (uint64_t i = 0; i < 1024; i++) {
        assert_true(sm_add(map, i + seg) == i + seg);
      }
      for (uint64_t i = 0; i < 1024; i++) {
        assert_true(sm_contains(map, i + seg));
        assert_false(sm_contains(&portion, i + seg));
      }

      sm_split(map, seg + off, &portion);

      for (uint64_t i = seg; i < seg + 1024; i++) {
        if (i < seg + off) {
          assert_true(sm_contains(map, i));
          assert_false(sm_contains(&portion, i));
        } else {
          assert_false(sm_contains(map, i));
          assert_true(sm_contains(&portion, i));
        }
      }
      sm_clear(map);
      sm_clear(&portion);
    }
  }

  for (uint64_t i = 0; i < 100; i++) {
    assert_true(sm_add(map, i) == i);
  }
  for (uint64_t i = 0; i < 100; i++) {
    assert_true(sm_contains(map, i));
    assert_false(sm_contains(&portion, i));
  }

  offset = sm_split(map, SM_IDX_MAX, &portion);

  for (uint64_t i = 0; i < offset; i++) {
    assert_true(sm_contains(map, i));
    assert_false(sm_contains(&portion, i));
  }
  for (uint64_t i = offset + 1; i < sm_maximum(&portion); i++) {
    assert_false(sm_contains(map, i));
    assert_true(sm_contains(&portion, i));
  }

  sm_clear(&portion);
  sm_clear(map);

  sm_init(&portion, buf, 1024);
  for (uint64_t i = 0; i < 13; i++) {
    assert_true(sm_add(map, i + 24) == i + 24);
  }

  offset = sm_split(map, SM_IDX_MAX, &portion);
  assert_true(sm_maximum(map) < offset);
  assert_true(sm_minimum(&portion) >= offset);
  assert_true(sm_cardinality(map) == 6);
  assert_true(sm_cardinality(&portion) == 7);

  for (uint64_t i = 0; i < offset - 24; i++) {
    assert_true(sm_contains(map, i + 24));
    assert_false(sm_contains(&portion, i + 24));
  }
  for (uint64_t i = offset - 24; i < 13; i++) {
    assert_false(sm_contains(map, i + 24));
    assert_true(sm_contains(&portion, i + 24));
  }

  return MUNIT_OK;
}

/* ---- offset tests ---- */
static void *
test_api_offset_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  sparsemap_t *map = sparsemap(10 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_api_offset_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_api_offset(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  /* Test 1: offset == 0 returns a copy */
  sm_add(map, 10);
  sm_add(map, 20);
  sm_add(map, 30);
  sparsemap_t *copy = sm_offset(map, 0);
  assert_ptr_not_null(copy);
  assert_true(sm_contains(copy, 10));
  assert_true(sm_contains(copy, 20));
  assert_true(sm_contains(copy, 30));
  assert_size(sm_cardinality(copy), ==, 3);
  free(copy);

  /* Test 2: positive offset */
  sparsemap_t *shifted = sm_offset(map, 100);
  assert_ptr_not_null(shifted);
  assert_false(sm_contains(shifted, 10));
  assert_false(sm_contains(shifted, 20));
  assert_false(sm_contains(shifted, 30));
  assert_true(sm_contains(shifted, 110));
  assert_true(sm_contains(shifted, 120));
  assert_true(sm_contains(shifted, 130));
  assert_size(sm_cardinality(shifted), ==, 3);
  free(shifted);

  /* Test 3: negative offset, no bits dropped */
  shifted = sm_offset(map, -5);
  assert_ptr_not_null(shifted);
  assert_true(sm_contains(shifted, 5));
  assert_true(sm_contains(shifted, 15));
  assert_true(sm_contains(shifted, 25));
  assert_size(sm_cardinality(shifted), ==, 3);
  free(shifted);

  /* Test 4: negative offset, some bits dropped */
  shifted = sm_offset(map, -15);
  assert_ptr_not_null(shifted);
  assert_false(sm_contains(shifted, 0)); /* 10-15 < 0, dropped */
  assert_true(sm_contains(shifted, 5));  /* 20-15 = 5 */
  assert_true(sm_contains(shifted, 15)); /* 30-15 = 15 */
  assert_size(sm_cardinality(shifted), ==, 2);
  free(shifted);

  /* Test 5: negative offset, all bits dropped */
  shifted = sm_offset(map, -100);
  assert_ptr_equal(shifted, NULL); /* all bits shifted below 0 */

  /* Test 6: empty map */
  sm_clear(map);
  shifted = sm_offset(map, 10);
  assert_ptr_equal(shifted, NULL);

  /* Test 7: large positive offset */
  sm_add(map, 0);
  sm_add(map, 1);
  sm_add(map, 2);
  shifted = sm_offset(map, 10000);
  assert_ptr_not_null(shifted);
  assert_true(sm_contains(shifted, 10000));
  assert_true(sm_contains(shifted, 10001));
  assert_true(sm_contains(shifted, 10002));
  assert_size(sm_cardinality(shifted), ==, 3);
  free(shifted);

  /* Test 8: RLE range with positive offset */
  sm_clear(map);
  for (int i = 0; i < 5000; i++) {
    sm_add(map, i);
  }
  shifted = sm_offset(map, 64);
  assert_ptr_not_null(shifted);
  assert_false(sm_contains(shifted, 63));
  assert_true(sm_contains(shifted, 64));
  assert_true(sm_contains(shifted, 5063));
  assert_false(sm_contains(shifted, 5064));
  assert_size(sm_cardinality(shifted), ==, 5000);
  free(shifted);

  /* Test 9: Whole-chunk offset (multiple of 2048) */
  sm_clear(map);
  sm_add(map, 100);
  sm_add(map, 500);
  sm_add(map, 2000);
  shifted = sm_offset(map, 2048);
  assert_ptr_not_null(shifted);
  assert_true(sm_contains(shifted, 2148));   /* 100 + 2048 */
  assert_true(sm_contains(shifted, 2548));   /* 500 + 2048 */
  assert_true(sm_contains(shifted, 4048));   /* 2000 + 2048 */
  assert_false(sm_contains(shifted, 100));
  assert_false(sm_contains(shifted, 500));
  assert_size(sm_cardinality(shifted), ==, 3);
  free(shifted);

  /* Test 10: Sub-chunk offset crossing vector boundary (shift by 63) */
  sm_clear(map);
  sm_add(map, 0);
  sm_add(map, 1);
  sm_add(map, 63);
  sm_add(map, 64);
  shifted = sm_offset(map, 63);
  assert_ptr_not_null(shifted);
  assert_true(sm_contains(shifted, 63));    /* 0 + 63 */
  assert_true(sm_contains(shifted, 64));    /* 1 + 63 */
  assert_true(sm_contains(shifted, 126));   /* 63 + 63 */
  assert_true(sm_contains(shifted, 127));   /* 64 + 63 */
  assert_false(sm_contains(shifted, 62));
  assert_false(sm_contains(shifted, 128));
  assert_size(sm_cardinality(shifted), ==, 4);
  free(shifted);

  /* Test 11: Large RLE range with non-aligned offset (+37 on 10,000 bits) */
  sm_clear(map);
  for (int i = 0; i < 10000; i++) {
    sm_add(map, i);
  }
  shifted = sm_offset(map, 37);
  assert_ptr_not_null(shifted);
  assert_false(sm_contains(shifted, 36));
  assert_true(sm_contains(shifted, 37));     /* 0 + 37 */
  assert_true(sm_contains(shifted, 10036));  /* 9999 + 37 */
  assert_false(sm_contains(shifted, 10037));
  assert_size(sm_cardinality(shifted), ==, 10000);
  free(shifted);

  /* Test 12: Negative offset dropping some bits from multi-chunk sparse */
  sm_clear(map);
  sm_add(map, 5);
  sm_add(map, 2050);   /* in second chunk */
  sm_add(map, 4100);   /* in third chunk */
  shifted = sm_offset(map, -10);
  assert_ptr_not_null(shifted);
  assert_false(sm_contains(shifted, 0));     /* 5 - 10 < 0, dropped */
  assert_true(sm_contains(shifted, 2040));   /* 2050 - 10 */
  assert_true(sm_contains(shifted, 4090));   /* 4100 - 10 */
  assert_size(sm_cardinality(shifted), ==, 2);
  free(shifted);

  /* Test 13: Carry across chunk boundary (bit 2047 shifted by +1 = bit 2048) */
  sm_clear(map);
  sm_add(map, 2047);
  shifted = sm_offset(map, 1);
  assert_ptr_not_null(shifted);
  assert_false(sm_contains(shifted, 2047));
  assert_true(sm_contains(shifted, 2048));   /* 2047 + 1 crosses chunk boundary */
  assert_size(sm_cardinality(shifted), ==, 1);
  free(shifted);

  return MUNIT_OK;
}

static void *
test_api_merge_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  sparsemap_t *map = sparsemap(10 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_api_merge_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_api_merge(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  sparsemap_t *other = sparsemap(1024);
  (void)params;

  assert_ptr_not_null(map);
  assert_ptr_not_null(other);

  // Merge two empty maps to get an empty map.
  {
    sparsemap_t *m = sm_union(map, other);
    // Both empty: result may be NULL.
    if (m) free(m);
  }

  // Merge a single set bit in the first chunk into the empty map.
  sm_add(other, 0);
  {
    sparsemap_t *m = sm_union(map, other);
    assert_ptr_not_null(m);
    assert_true(sm_contains(other, 0));
    assert_true(sm_contains(m, 0));
    free(m);
  }
  sm_clear(map);
  sm_clear(other);

  // Merge two maps with the same single bit set.
  sm_add(map, 0);
  sm_add(other, 0);
  {
    sparsemap_t *m = sm_union(map, other);
    assert_ptr_not_null(m);
    assert_true(sm_contains(m, 0));
    free(m);
  }
  sm_clear(map);
  sm_clear(other);

  // Merge an empty map with one that has the first bit set.
  sm_add(map, 0);
  {
    sparsemap_t *m = sm_union(map, other);
    assert_ptr_not_null(m);
    assert_true(sm_contains(m, 0));
    free(m);
  }
  sm_clear(map);
  sm_clear(other);

  sm_add(other, 2049);
  {
    sparsemap_t *m = sm_union(map, other);
    assert_ptr_not_null(m);
    assert_true(sm_contains(m, 2049));
    free(m);
  }
  sm_clear(map);
  sm_clear(other);

  sm_add(other, 1);
  sm_add(other, 2049);
  sm_add(map, 2050);
  sm_add(other, 4097);
  sm_add(map, 6113);
  sm_add(other, 8193);
  {
    sparsemap_t *m = sm_union(map, other);
    assert_ptr_not_null(m);
    assert_true(sm_contains(m, 1));
    assert_true(sm_contains(m, 2049));
    assert_true(sm_contains(m, 2050));
    assert_true(sm_contains(m, 4097));
    assert_true(sm_contains(m, 6113));
    assert_true(sm_contains(m, 8193));
    for (int i = 0; i < 10000; i++) {
      if (i == 2049 || i == 1 || i == 2050 || i == 4097 || i == 6113 || i == 8193)
        continue;
      else
        assert_false(sm_contains(m, i));
    }
    free(m);
  }
  sm_clear(map);
  sm_clear(other);

  sm_add(map, 0);
  sm_add(map, 2048);
  sm_add(map, 8193);
  for (int i = 2049; i < 4096; i++) {
    sm_add(other, i);
  }
  {
    sparsemap_t *m = sm_union(map, other);
    assert_ptr_not_null(m);
    assert(sm_contains(m, 0));
    assert(sm_contains(m, 2048));
    assert(sm_contains(m, 8193));
    for (int i = 2049; i < 4096; i++) {
      assert(sm_contains(m, i));
    }
    free(m);
  }
  sm_clear(map);
  sm_clear(other);

  for (int i = 2049; i < 4096; i++) {
    sm_add(map, i);
  }
  sm_split(map, 2051, other);
  for (int i = 2049; i < 4096; i++) {
    if (i < 2051) {
      assert_true(sm_contains(map, i));
      assert_false(sm_contains(other, i));
    } else {
      assert_false(sm_contains(map, i));
      assert_true(sm_contains(other, i));
    }
  }
  {
    sparsemap_t *m = sm_union(map, other);
    assert_ptr_not_null(m);
    for (int i = 2049; i < 4096; i++) {
      assert_true(sm_contains(m, i));
    }
    free(m);
  }

  munit_free(other);
  return MUNIT_OK;
}

static void *
test_api_select_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);
  sm_bitmap_from_uint64(map, 0, ((uint64_t)0xfeedface << 32) | 0xbadc0ffee);

  return (void *)map;
}
static void
test_api_select_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_select(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  /* NOTE: select() is 0-based, to get the bit position of the 1st logical bit set
     call select(map, 0), to get the 18th, select(map, 17), etc. */
  assert_true(sm_select(map, 0, true) == 1);
  assert_true(sm_select(map, 4, true) == 6);
  assert_true(sm_select(map, 17, true) == 26);

  return MUNIT_OK;
}

#ifdef SELECT_FALSE
static void *
test_api_select_false_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);
  sm_bitmap_from_uint64(map, 0, ((uint64_t)0xfeedface << 32) | 0xbadc0ffee);

  return (void *)map;
}
static void
test_api_select_false_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_select_false(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;
  assert_ptr_not_null(map);

  /* First few 0/off/unset-bits in ((uint64_t)0xfeedface << 32) | 0xbadc0ffee) expressed as an array of offsets. */
  size_t off[] = { 0, 4, 16, 17, 18, 19, 20, 21, 25, 28, 30, 36, 37, 40, 42, 49, 52, 56, 64, 65 };
  for (size_t i = 0; i < 20; i++) {
    uint64_t f = sm_select(map, i, false);
    assert_true(f == off[i]);
    assert_true(sm_contains(map, f) == false);
  }

  return MUNIT_OK;
}
#endif

#ifdef SELECT_NEG
static void *
test_api_select_neg_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);
  sm_bitmap_from_uint64(map, 0, ((uint64_t)0xfeedface << 32) | 0xbadc0ffee);

  return (void *)map;
}
static void
test_api_select_neg_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_select_neg(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  sm_add(map, 42);
  sm_add(map, 420);
  sm_add(map, 4200);

  f = sm_select(map, 0, false);
  assert_true(f == 0);

  f = sm_select(map, -1, true);
  assert_true(f == 4200);
  f = sm_select(map, -2, true);
  assert_true(f == 420);
  f = sm_select(map, -3, true);
  assert_true(f == 42);

  return MUNIT_OK;
}
#endif

/* -------------------------- RLE Tests */

static void *
test_api_select_rle_true_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(8192, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 8192);
  /* Create RLE run of 1000 consecutive set bits (0-999) */
  populate_map_rle(map, 0, 1, 1000);

  return (void *)map;
}
static void
test_api_select_rle_true_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_select_rle_true(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  /* Test selecting set bits in RLE run */
  /* First bit: select(0, true) == 0 */
  assert_true(sm_select(map, 0, true) == 0);
  /* Middle bit: select(500, true) == 500 */
  assert_true(sm_select(map, 500, true) == 500);
  /* Last bit: select(999, true) == 999 */
  assert_true(sm_select(map, 999, true) == 999);
  /* Beyond run: select(1000, true) == SM_IDX_MAX */
  assert_true(sm_select(map, 1000, true) == SM_IDX_MAX);

  return MUNIT_OK;
}

static MunitResult
test_api_select_rle_false(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  /* Test selecting unset bits in RLE run */
  /* First unset: select(0, false) == 1000 */
  assert_true(sm_select(map, 0, false) == 1000);
  /* Subsequent: select(1, false) == 1001 */
  assert_true(sm_select(map, 1, false) == 1001);
  /* select(100, false) == 1100 */
  assert_true(sm_select(map, 100, false) == 1100);

  return MUNIT_OK;
}

static void *
test_api_scan_rle_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(8192, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 8192);
  /* Create RLE run of 1000 consecutive set bits */
  populate_map_rle(map, 0, 1, 1000);

  return (void *)map;
}
static void
test_api_scan_rle_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}

/* Scanner callback that counts bits and tracks last index */
static size_t scan_rle_count = 0;
static uint32_t scan_rle_last_idx = 0;
void
scan_rle_counter(uint32_t v[], size_t n, void *aux)
{
  (void)aux;
  for (size_t i = 0; i < n; i++) {
    scan_rle_count++;
    scan_rle_last_idx = v[i];
  }
}

static MunitResult
test_api_scan_rle(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  /* Reset counters */
  scan_rle_count = 0;
  scan_rle_last_idx = 0;

  /* Scan all bits */
  sm_scan(map, scan_rle_counter, 0, NULL);

  /* Verify total count = 1000 */
  assert_true(scan_rle_count == 1000);
  /* Verify last index = 999 */
  assert_true(scan_rle_last_idx == 999);

  return MUNIT_OK;
}

static MunitResult
test_api_scan_rle_skip(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  /* Reset counters */
  scan_rle_count = 0;
  scan_rle_last_idx = 0;

  /* Scan with skip=500, should scan only 500 bits (500-999) */
  sm_scan(map, scan_rle_counter, 500, NULL);

  /* Verify total count = 500 */
  assert_true(scan_rle_count == 500);
  /* Verify last index = 999 */
  assert_true(scan_rle_last_idx == 999);

  return MUNIT_OK;
}

static void *
test_api_rle_edge_cases_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(32768, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 32768);

  return (void *)map;
}
static void
test_api_rle_edge_cases_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_rle_edge_cases(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  /* Test 1: Single bit RLE */
  sm_clear(map);
  sm_add(map, 0);
  assert_true(sm_contains(map, 0) == true);
  assert_true(sm_select(map, 0, true) == 0);
  assert_true(sm_cardinality(map) == 1);

  /* Test 2: Large RLE run (10,000 bits) to stress-test batching in scan */
  sm_clear(map);
  scan_rle_count = 0;
  scan_rle_last_idx = 0;
  populate_map_rle(map, 0, 1, 10000);
  assert_true(sm_cardinality(map) == 10000);
  sm_scan(map, scan_rle_counter, 0, NULL);
  assert_true(scan_rle_count == 10000);
  assert_true(scan_rle_last_idx == 9999);

  /* Test 3: is_set boundary check (the bug we fixed) */
  sm_clear(map);
  populate_map_rle(map, 0, 1, 1000);
  assert_true(sm_contains(map, 0) == true);
  assert_true(sm_contains(map, 999) == true);
  assert_true(sm_contains(map, 1000) == false); /* Beyond run */

  return MUNIT_OK;
}

static void *
test_api_rank_true_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_rank_true_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_rank_true(const MunitParameter params[], void *data)
{
  int r1, r2;
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  for (int i = 0; i < 10; i++) {
    sm_add(map, i);
  }
  /* rank() is also 0-based, for consistency (and confusion sake); consider the
     range as [start, end] of [0, 9] counts the bits set in the first 10
     positions (starting from the LSB) in the index. */
  r1 = rank_uint64((uint64_t)-1, 0, 9);
  r2 = sm_rank(map, 0, 9, true);
  assert_true(r1 == r2);
  assert_true(sm_rank(map, 0, 9, true) == 10);
  assert_true(sm_rank(map, 1000, 1050, true) == 0);

  sm_clear(map);

  for (int i = 0; i < 10000; i++) {
    sm_add(map, i);
  }

  // TODO: separate test for slicing a run within the chunk size of the end of the run

  uint64_t hole = 4999;
  sm_remove(map, hole);
  for (size_t i = 0; i < 10000; i++) {
    for (size_t j = i; j < 10000; j++) {
      size_t amt = (i > j) ? 0 : j - i + 1 - ((hole >= i && j >= hole) ? 1 : 0);
      size_t r = sm_rank(map, i, j, true);
      assert_true(r == amt);
    }
  }

  return MUNIT_OK;
}

static void *
test_api_rank_false_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_rank_false_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_rank_false(const MunitParameter params[], void *data)
{
  size_t r;
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  // empty map
  for (int i = 0; i < 10000; i++) {
    for (int j = i; j < 10000; j++) {
      r = sm_rank(map, i, j, false);
      assert_true(r == (size_t)(j - i + 1));
    }
  }

  // One chunk means not so empty now!
  uint64_t hole = 4999;
  sm_add(map, hole);
  for (size_t i = 0; i < 10000; i++) {
    for (size_t j = i; j < 10000; j++) {
      size_t amt = (i > j) ? 0 : j - i + 1 - ((hole >= i && j >= hole) ? 1 : 0);
      r = sm_rank(map, i, j, false);
      assert_true(r == amt);
    }
  }

  // RLE
  for (size_t i = 0; i < 10000; i++) {
    sm_add(map, i);
  }
  r = sm_rank(map, 9990, 10010, false);
  assert_true(r == 11);

  r = sm_rank(map, 9990, 4294967295, false);
  assert_true(r == 4294957296);

  sm_clear(map);
  sm_add(map, 1);
  sm_add(map, 11);
  r = sm_rank(map, 0, 11, false);
  assert_true(r == 10);

  return MUNIT_OK;
}

static void *
test_api_span_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_span_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_span(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  int located_at, placed_at, amt = 10000;

  placed_at = sm_add_span(map, amt, 1);
  located_at = sm_span(map, 0, 1, true);
  assert_true(located_at == placed_at);

  sm_clear(map);

  placed_at = sm_add_span(map, amt, 50);
  located_at = sm_span(map, 0, 50, true);
  assert_true(located_at == placed_at);

  sm_clear(map);

  placed_at = sm_add_span(map, amt, 50);
  located_at = sm_span(map, placed_at / 2, 50, true);
  assert_true(located_at == placed_at);

  return MUNIT_OK;
}

static void *
test_api_random_set_clear_setup(const MunitParameter params[],
                                void *user_data)
{
  (void)params;
  (void)user_data;
  sparsemap_t *map = sparsemap(64 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_api_random_set_clear_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_api_random_set_clear(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  /* Track ranges we set: each range is [start, start+len). */
  int num_ranges = munit_rand_int_range(10, 20);
  size_t range_start[20];
  size_t range_len[20];
  size_t total_set = 0;

  /* Helper: set a bit with auto-grow on ENOSPC. */
  #define SET_BIT(m, idx) do {                                   \
    if (sm_add((m), (idx)) != (idx)) {                    \
      (m) = sm_set_data_size(                             \
          (m), NULL, sm_get_capacity((m)) * 2);           \
      assert_ptr_not_null((m));                                  \
      errno = 0;                                                 \
      assert_true(sm_add((m), (idx)) == (idx));           \
    }                                                            \
  } while (0)

  /* Set random ranges of 512-8192 bits. */
  size_t offset = 0;
  for (int r = 0; r < num_ranges; r++) {
    size_t len = munit_rand_int_range(512, 8192);
    size_t gap = munit_rand_int_range(0, 1024);
    offset += gap;
    range_start[r] = offset;
    range_len[r] = len;
    for (size_t i = 0; i < len; i++) {
      SET_BIT(map, offset + i);
    }
    offset += len;
  }

  /* Count total unique bits set. */
  total_set = sm_cardinality(map);
  assert_true(total_set > 0);

  /* Verify all bits in each range are set. */
  for (int r = 0; r < num_ranges; r++) {
    for (size_t i = 0; i < range_len[r]; i++) {
      assert_true(sm_contains(map, range_start[r] + i));
    }
  }

  /* --- Mode 1: Clear all at once --- */
  sm_clear(map);
  assert_true(sm_cardinality(map) == 0);

  /* Re-populate for mode 2. */
  for (int r = 0; r < num_ranges; r++) {
    for (size_t i = 0; i < range_len[r]; i++) {
      SET_BIT(map, range_start[r] + i);
    }
  }
  total_set = sm_cardinality(map);

  /* --- Mode 2: Clear low-to-high order --- */
  for (int r = 0; r < num_ranges; r++) {
    for (size_t i = 0; i < range_len[r]; i++) {
      sm_remove(map, range_start[r] + i);
    }
    total_set -= range_len[r];
    assert_true(sm_cardinality(map) == total_set);
  }
  assert_true(sm_cardinality(map) == 0);

  /* Re-populate for mode 3. */
  for (int r = 0; r < num_ranges; r++) {
    for (size_t i = 0; i < range_len[r]; i++) {
      SET_BIT(map, range_start[r] + i);
    }
  }
  total_set = sm_cardinality(map);

  /* --- Mode 3: Clear high-to-low order --- */
  for (int r = num_ranges - 1; r >= 0; r--) {
    for (size_t i = range_len[r]; i > 0; i--) {
      sm_remove(map, range_start[r] + i - 1);
    }
    total_set -= range_len[r];
    assert_true(sm_cardinality(map) == total_set);
  }
  assert_true(sm_cardinality(map) == 0);

  #undef SET_BIT

  return MUNIT_OK;
}

/* ---- intersection tests ---- */
static void *
test_api_intersection_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  sparsemap_t *map = sparsemap(64 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_api_intersection_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_api_intersection(const MunitParameter params[], void *data)
{
  (void)params;
  (void)data;

  /* Test 1: Disjoint maps = empty result */
  {
    sparsemap_t *a = sparsemap(1024);
    sparsemap_t *b = sparsemap(1024);
    sm_add(a, 10);
    sm_add(a, 20);
    sm_add(b, 100);
    sm_add(b, 200);
    sparsemap_t *r = sm_intersection(a, b);
    assert_ptr_equal(r, NULL);
    free(a);
    free(b);
  }

  /* Test 2: Identical maps = same cardinality */
  {
    sparsemap_t *a = sparsemap(1024);
    sm_add(a, 10);
    sm_add(a, 20);
    sm_add(a, 30);
    sparsemap_t *b = sm_copy(a);
    sparsemap_t *r = sm_intersection(a, b);
    assert_ptr_not_null(r);
    assert_size(sm_cardinality(r), ==, 3);
    assert_true(sm_contains(r, 10));
    assert_true(sm_contains(r, 20));
    assert_true(sm_contains(r, 30));
    free(r);
    free(a);
    free(b);
  }

  /* Test 3: One empty map = empty result */
  {
    sparsemap_t *a = sparsemap(1024);
    sparsemap_t *b = sparsemap(1024);
    sm_add(a, 10);
    sm_add(a, 20);
    sparsemap_t *r = sm_intersection(a, b);
    assert_ptr_equal(r, NULL);
    free(a);
    free(b);
  }

  /* Test 4: Partial overlap with sparse chunks */
  {
    sparsemap_t *a = sparsemap(1024);
    sparsemap_t *b = sparsemap(1024);
    for (int i = 0; i < 100; i++) {
      sm_add(a, i);
    }
    for (int i = 50; i < 150; i++) {
      sm_add(b, i);
    }
    sparsemap_t *r = sm_intersection(a, b);
    assert_ptr_not_null(r);
    assert_size(sm_cardinality(r), ==, 50);
    for (int i = 50; i < 100; i++) {
      assert_true(sm_contains(r, i));
    }
    assert_false(sm_contains(r, 49));
    assert_false(sm_contains(r, 100));
    free(r);
    free(a);
    free(b);
  }

  /* Test 5: RLE range overlap: [0,5000) & [3000,8000) = [3000,5000) */
  {
    sparsemap_t *a = sparsemap(64 * 1024);
    sparsemap_t *b = sparsemap(64 * 1024);
    for (int i = 0; i < 5000; i++) {
      sm_add(a, i);
    }
    for (int i = 3000; i < 8000; i++) {
      sm_add(b, i);
    }
    sparsemap_t *r = sm_intersection(a, b);
    assert_ptr_not_null(r);
    assert_size(sm_cardinality(r), ==, 2000);
    assert_true(sm_contains(r, 3000));
    assert_true(sm_contains(r, 4999));
    assert_false(sm_contains(r, 2999));
    assert_false(sm_contains(r, 5000));
    free(r);
    free(a);
    free(b);
  }

  /* Test 6: Mixed RLE/sparse overlap */
  {
    sparsemap_t *a = sparsemap(64 * 1024);
    sparsemap_t *b = sparsemap(1024);
    /* a: RLE run [0, 5000) */
    for (int i = 0; i < 5000; i++) {
      sm_add(a, i);
    }
    /* b: sparse bits at 100, 200, 300, 6000 */
    sm_add(b, 100);
    sm_add(b, 200);
    sm_add(b, 300);
    sm_add(b, 6000);
    sparsemap_t *r = sm_intersection(a, b);
    assert_ptr_not_null(r);
    assert_size(sm_cardinality(r), ==, 3);
    assert_true(sm_contains(r, 100));
    assert_true(sm_contains(r, 200));
    assert_true(sm_contains(r, 300));
    assert_false(sm_contains(r, 6000));
    free(r);
    free(a);
    free(b);
  }

  return MUNIT_OK;
}

/* ---- difference tests ---- */
static void *
test_api_difference_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  sparsemap_t *map = sparsemap(64 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_api_difference_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_api_difference(const MunitParameter params[], void *data)
{
  (void)params;
  (void)data;

  /* Test 1: Difference with empty b = copy of a */
  {
    sparsemap_t *a = sparsemap(1024);
    sparsemap_t *b = sparsemap(1024);
    sm_add(a, 10);
    sm_add(a, 20);
    sm_add(a, 30);
    sparsemap_t *r = sm_difference(a, b);
    assert_ptr_not_null(r);
    assert_size(sm_cardinality(r), ==, 3);
    assert_true(sm_contains(r, 10));
    assert_true(sm_contains(r, 20));
    assert_true(sm_contains(r, 30));
    free(r);
    free(a);
    free(b);
  }

  /* Test 2: Difference with identical b = empty */
  {
    sparsemap_t *a = sparsemap(1024);
    sm_add(a, 10);
    sm_add(a, 20);
    sm_add(a, 30);
    sparsemap_t *b = sm_copy(a);
    sparsemap_t *r = sm_difference(a, b);
    assert_ptr_equal(r, NULL);
    free(a);
    free(b);
  }

  /* Test 3: Disjoint b = copy of a */
  {
    sparsemap_t *a = sparsemap(1024);
    sparsemap_t *b = sparsemap(1024);
    sm_add(a, 10);
    sm_add(a, 20);
    sm_add(b, 100);
    sm_add(b, 200);
    sparsemap_t *r = sm_difference(a, b);
    assert_ptr_not_null(r);
    assert_size(sm_cardinality(r), ==, 2);
    assert_true(sm_contains(r, 10));
    assert_true(sm_contains(r, 20));
    free(r);
    free(a);
    free(b);
  }

  /* Test 4: Partial overlap subtraction */
  {
    sparsemap_t *a = sparsemap(1024);
    sparsemap_t *b = sparsemap(1024);
    for (int i = 0; i < 100; i++) {
      sm_add(a, i);
    }
    for (int i = 50; i < 150; i++) {
      sm_add(b, i);
    }
    sparsemap_t *r = sm_difference(a, b);
    assert_ptr_not_null(r);
    assert_size(sm_cardinality(r), ==, 50);
    for (int i = 0; i < 50; i++) {
      assert_true(sm_contains(r, i));
    }
    assert_false(sm_contains(r, 50));
    free(r);
    free(a);
    free(b);
  }

  /* Test 5: RLE difference: [0,5000) - [3000,8000) = [0,3000) */
  {
    sparsemap_t *a = sparsemap(64 * 1024);
    sparsemap_t *b = sparsemap(64 * 1024);
    for (int i = 0; i < 5000; i++) {
      sm_add(a, i);
    }
    for (int i = 3000; i < 8000; i++) {
      sm_add(b, i);
    }
    sparsemap_t *r = sm_difference(a, b);
    assert_ptr_not_null(r);
    assert_size(sm_cardinality(r), ==, 3000);
    assert_true(sm_contains(r, 0));
    assert_true(sm_contains(r, 2999));
    assert_false(sm_contains(r, 3000));
    free(r);
    free(a);
    free(b);
  }

  /* Test 6: Subtract from middle of sparse chunk */
  {
    sparsemap_t *a = sparsemap(1024);
    sparsemap_t *b = sparsemap(1024);
    /* a: bits 0..63 all set */
    for (int i = 0; i < 64; i++) {
      sm_add(a, i);
    }
    /* b: bits 20..39 set */
    for (int i = 20; i < 40; i++) {
      sm_add(b, i);
    }
    sparsemap_t *r = sm_difference(a, b);
    assert_ptr_not_null(r);
    assert_size(sm_cardinality(r), ==, 44);
    for (int i = 0; i < 20; i++) {
      assert_true(sm_contains(r, i));
    }
    for (int i = 20; i < 40; i++) {
      assert_false(sm_contains(r, i));
    }
    for (int i = 40; i < 64; i++) {
      assert_true(sm_contains(r, i));
    }
    free(r);
    free(a);
    free(b);
  }

  return MUNIT_OK;
}

// clang-format off
static MunitTest api_test_suite[] = {
  { (char *)"/new", test_api_new, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/new/realloc", test_api_new_realloc, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/new/heap", test_api_new_heap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/new/static", test_api_new_static, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/new/stack", test_api_new_stack, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/clear", test_api_clear, test_api_clear_setup, test_api_clear_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/open", test_api_open, test_api_open_setup, test_api_open_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/set_data_size", test_api_set_data_size, test_api_set_data_size_setup, test_api_set_data_size_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/remaining_capacity", test_api_remaining_capacity, test_api_remaining_capacity_setup, test_api_remaining_capacity_tear_down,
    MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/get_capacity", test_api_get_capacity, test_api_get_capacity_setup, test_api_get_capacity_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/is_set", test_api_is_set, test_api_is_set_setup, test_api_is_set_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/set", test_api_set, test_api_set_setup, test_api_set_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/get_size", test_api_get_size, test_api_get_size_setup, test_api_get_size_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/count", test_api_count, test_api_count_setup, test_api_count_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/get_data", test_api_get_data, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/get_start_offset", test_api_get_start_offset, test_api_get_start_offset_setup, test_api_get_start_offset_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/get_start_offset/roll", test_api_get_start_offset_roll, test_api_get_start_offset_roll_setup, test_api_get_start_offset_roll_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/get_end_offset", test_api_get_end_offset, test_api_get_end_offset_setup, test_api_get_end_offset_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/scan", test_api_scan, test_api_scan_setup, test_api_scan_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/split", test_api_split, test_api_split_setup, test_api_split_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/offset", test_api_offset, test_api_offset_setup, test_api_offset_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/merge", test_api_merge, test_api_merge_setup, test_api_merge_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/intersection", test_api_intersection, test_api_intersection_setup, test_api_intersection_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/difference", test_api_difference, test_api_difference_setup, test_api_difference_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/select/true", test_api_select, test_api_select_setup, test_api_select_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
#ifdef SELECT_FALSE
  { (char *)"/select/false", test_api_select_false, test_api_select_false_setup, test_api_select_false_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
#endif
#ifdef SELECT_NEG
  { (char *)"/select/neg", test_api_select_neg, test_api_select_neg_setup, test_api_select_neg_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
#endif
  { (char *)"/select/rle/true", test_api_select_rle_true, test_api_select_rle_true_setup, test_api_select_rle_true_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/select/rle/false", test_api_select_rle_false, test_api_select_rle_true_setup, test_api_select_rle_true_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/scan/rle", test_api_scan_rle, test_api_scan_rle_setup, test_api_scan_rle_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/scan/rle/skip", test_api_scan_rle_skip, test_api_scan_rle_setup, test_api_scan_rle_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/rle/edge_cases", test_api_rle_edge_cases, test_api_rle_edge_cases_setup, test_api_rle_edge_cases_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/rank/true", test_api_rank_true, test_api_rank_true_setup, test_api_rank_true_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/rank/false", test_api_rank_false, test_api_rank_false_setup, test_api_rank_false_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/span", test_api_span, test_api_span_setup, test_api_span_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/random_set_clear", test_api_random_set_clear, test_api_random_set_clear_setup, test_api_random_set_clear_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
// clang-format on

/* -------------------------- Quickcheck, Property Based Tests */

extern QCC_GenValue *QCC_genChunk();
extern QCC_GenValue *QCC_genSparsemap();
extern QCC_TestStatus _tst_chunk_calc_vector_size_equality(QCC_GenValue **vals, int len, QCC_Stamp **stamp);
extern QCC_TestStatus _tst_chunk_get_position(QCC_GenValue **vals, int len, QCC_Stamp **stamp);
extern QCC_TestStatus _tst_chunk_get_capacity(QCC_GenValue **vals, int len, QCC_Stamp **stamp);
extern QCC_TestStatus _tst_get_chunk_offset(QCC_GenValue **vals, int len, QCC_Stamp **stamp);
extern QCC_TestStatus _tst_rle_select_rank_consistency(QCC_GenValue **vals, int len, QCC_Stamp **stamp);
extern QCC_TestStatus _tst_rle_scan_completeness(QCC_GenValue **vals, int len, QCC_Stamp **stamp);

static MunitResult
qc__sm_chunk_calc_vector_size(const MunitParameter params[], void *data)
{
  (void)params;
  (void)data;

  return QCC_testForAll(1000, 1000, _tst_chunk_calc_vector_size_equality, 1, QCC_genInt);
}

static MunitResult
qc__sm_chunk_get_position(const MunitParameter params[], void *data)
{
  (void)params;
  (void)data;

  return QCC_testForAll(100, 1000, _tst_chunk_get_position, 1, QCC_genChunk);
}

static MunitResult
qc__sm_chunk_get_capacity(const MunitParameter params[], void *data)
{
  (void)params;
  (void)data;

  return QCC_testForAll(100, 1000, _tst_chunk_get_capacity, 1, QCC_genChunk);
}

static MunitResult
qc__sm_get_chunk_offset(const MunitParameter params[], void *data)
{
  (void)params;
  (void)data;

  return QCC_testForAll(100, 1000, _tst_get_chunk_offset, 2, QCC_genInt, QCC_genSparsemap);
}

static MunitResult
qc_rle_select_rank_consistency(const MunitParameter params[], void *data)
{
  (void)params;
  (void)data;

  return QCC_testForAll(100, 1000, _tst_rle_select_rank_consistency, 1, QCC_genSparsemap);
}

static MunitResult
qc_rle_scan_completeness(const MunitParameter params[], void *data)
{
  (void)params;
  (void)data;

  return QCC_testForAll(100, 1000, _tst_rle_scan_completeness, 1, QCC_genSparsemap);
}

// clang-format off
static MunitTest qc_test_suite[] = {
  { (char *)"/__sm_chunk_calc_vector_size", qc__sm_chunk_calc_vector_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/__sm_chunk_get_position", qc__sm_chunk_get_position, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/__sm_chunk_get_capacity", qc__sm_chunk_get_capacity, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/__sm_get_chunk_offset", qc__sm_get_chunk_offset, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/rle_select_rank_consistency", qc_rle_select_rank_consistency, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/rle_scan_completeness", qc_rle_scan_completeness, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
// clang-format off

/* -------------------------- Integration Tests */

static void *
test_integration_rle_transition_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  uint8_t *buf = munit_calloc(65536, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);
  sm_init(map, buf, 65536);
  return (void *)map;
}

static void
test_integration_rle_transition_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}

static MunitResult
test_integration_rle_transition(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  /* Phase 1: Create sparse chunk with mixed set/unset bits */
  sm_clear(map);
  for (int i = 0; i < 100; i += 2) {
    sm_add(map, i); /* Set every other bit: 0, 2, 4, ..., 98 */
  }
  assert_true(sm_cardinality(map) == 50);
  assert_true(sm_contains(map, 0) == true);
  assert_true(sm_contains(map, 1) == false);

  /* Verify select works on sparse chunk */
  assert_true(sm_select(map, 0, true) == 0);
  assert_true(sm_select(map, 1, true) == 2);
  assert_true(sm_select(map, 49, true) == 98);

  /* Phase 2: Fill gaps to create long contiguous run (trigger RLE encoding) */
  sm_clear(map);
  for (int i = 0; i < 3000; i++) {
    sm_add(map, i); /* Create RLE run 0-2999 */
  }
  assert_true(sm_cardinality(map) == 3000);

  /* Verify all operations work on RLE chunk */
  assert_true(sm_contains(map, 0) == true);
  assert_true(sm_contains(map, 2999) == true);
  assert_true(sm_contains(map, 3000) == false);

  /* Verify select on RLE */
  assert_true(sm_select(map, 0, true) == 0);
  assert_true(sm_select(map, 1500, true) == 1500);
  assert_true(sm_select(map, 2999, true) == 2999);

  /* Verify rank on RLE */
  assert_true(sm_rank(map, 0, 100, true) == 101);
  assert_true(sm_rank(map, 0, 2999, true) == 3000);

  /* Verify scan on RLE */
  scan_rle_count = 0;
  scan_rle_last_idx = 0;
  sm_scan(map, scan_rle_counter, 0, NULL);
  assert_true(scan_rle_count == 3000);
  assert_true(scan_rle_last_idx == 2999);

  /* Phase 3: Modify chunk (clear bit in middle to split RLE back to sparse) */
  sm_remove(map, 1500); /* Creates gap, should convert to sparse */
  assert_true(sm_cardinality(map) == 2999);
  assert_true(sm_contains(map, 1499) == true);
  assert_true(sm_contains(map, 1500) == false);
  assert_true(sm_contains(map, 1501) == true);

  /* Verify operations still work after transition back to sparse */
  assert_true(sm_select(map, 0, true) == 0);
  assert_true(sm_rank(map, 0, 1499, true) == 1500);

  /* Verify scan after transition */
  scan_rle_count = 0;
  sm_scan(map, scan_rle_counter, 0, NULL);
  assert_true(scan_rle_count == 2999); /* One less due to unset */

  return MUNIT_OK;
}

// clang-format off
static MunitTest integration_test_suite[] = {
  { (char *)"/rle_transition", test_integration_rle_transition, test_integration_rle_transition_setup, test_integration_rle_transition_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
// clang-format on

/* -------------------------- Scale Tests */

static void *
test_scale_lots_o_spans_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  sparsemap_t *map = sparsemap(10 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_scale_lots_o_spans_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_scale_lots_o_spans(const MunitParameter params[], void *data)
{
  size_t amt = 897915; // 268435456
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  for (size_t i = 0; i < amt;) {
    int l = i % 31 + 16;
    sm_add_span(map, 10000, l);
    if (errno == ENOSPC) {
      map = sm_set_data_size(map, NULL, sm_get_capacity(map) * 2);
      errno = 0;
    }
    i += l;
    /* ANSI esc code to clear line, carriage return, then print on the same line */
    // printf("\033[2K\r%d", i);
    // printf("%d\t%d\n", l, i);
  }

  return MUNIT_OK;
}

#ifdef SCALE_ONDREJ
static void *
test_scale_ondrej_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  sparsemap_t *map = sparsemap(10 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_scale_ondrej_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_scale_ondrej(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  uint64_t stride = 18;
  //  uint64_t top = 268435456;
  uint64_t top = 2000;
  uint64_t needle = munit_rand_int_range(1, top / stride);
  for (uint64_t i = 0; i < top / stride; i += stride) {
    for (uint64_t j = 0; j < stride; j++) {
      bool set = (i != needle) ? (j < 10) : (j < 9);
      sm_add(map, i, set);
      if (errno == ENOSPC) {
        map = sm_set_data_size(map, NULL, sm_get_capacity(map) * 2);
        errno = 0;
      }
    }
    assert_true(sm_is_span(map, i + ((i != needle) ? 10 : 9), (i != needle) ? 8 : 9, true));
  }
  uint64_t a = sm_span(map, 0, 9, false);
  uint64_t l = a / stride;
  printf("%" PRIu64 "\t%" PRIu64 "\n", a, l);
  assert_true(l == needle);
  return MUNIT_OK;
}
#endif // SCALE_ONDREJ

static void *
test_scale_fuzz_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  sparsemap_t *map = sparsemap(10 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_scale_fuzz_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_scale_fuzz(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  /* Quick fuzz: random set/unset cycles, verify count consistency */
  size_t expected_count = 0;
  bool bits[2048] = { 0 };
  for (int round = 0; round < 500; round++) {
    size_t idx = munit_rand_int_range(0, 2047);
    if (munit_rand_int_range(0, 1)) {
      sm_add(map, idx);
      if (!bits[idx]) { expected_count++; bits[idx] = true; }
    } else {
      sm_remove(map, idx);
      if (bits[idx]) { expected_count--; bits[idx] = false; }
    }
  }
  assert_true(sm_cardinality(map) == expected_count);
  for (size_t i = 0; i < 2048; i++) {
    assert_true(sm_contains(map, i) == bits[i]);
  }
  return MUNIT_OK;
}

static void *
test_scale_alternating_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  sparsemap_t *map = sparsemap(10 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_scale_alternating_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
extern char *bytes_as(double bytes, char *s, size_t size);
static MunitResult
test_scale_alternating(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  for (uint64_t i = 0; i < (1000 * 8192); i++) {
    if (i % 2) {
      if (sm_add(map, i) != i) {
        // printf("%zu\n", i);
        break;
      }
    }
  }
  return MUNIT_OK;
}

static void *
test_scale_spans_come_spans_go_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  sparsemap_t *map = sparsemap(1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_scale_spans_come_spans_go_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_scale_spans_come_spans_go(const MunitParameter params[], void *data)
{
  size_t amt = 8192; // 268435456; // ~5e7 iterations due to 2e9 / avg(l)
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  for (size_t i = 0; i < amt;) {
    int l = i % 31 + 16;
    sm_add_span(map, amt, l);
    if (errno == ENOSPC) {
      map = sm_set_data_size(map, NULL, sm_get_capacity(map) + 1024);
      assert_ptr_not_null(map);
      errno = 0;
    }

    /* After 1,000 spans are in the map start consuming a span every iteration. */
    if (l > 1000) {
      do {
        int s = munit_rand_int_range(1, 30);
        int o = munit_rand_int_range(1, 268435456 - s - 1);
        size_t b = sm_span(map, o, s, true);
        if (b == SM_IDX_MAX) {
          continue;
        }
        for (int j = b; j < s; j++) {
          assert_true(sm_contains(map, j) == true);
        }
        for (int j = b; j < s; j++) {
          sm_remove(map, j);
        }
        for (int j = b; j < s; j++) {
          assert_true(sm_contains(map, j) == false);
        }
        break;
      } while (true);
    }
    i += l;
  }

  return MUNIT_OK;
}

static void *
test_scale_best_case_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);

  return (void *)map;
}
static void
test_scale_best_case_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_scale_best_case(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  /* Best case a map can contain 2048 bits in 8 bytes.
     So, in a 1KiB buffer you have:
       (1024 KiB / 8 bytes) * 2048 = 268,435,456 bits
     or 1.09 TiB of 4KiB pages. Let's investigate, and find out if that's the case.
  */

  /* Set every bit on, that should be the best case. */
  // for (int i = 0; i < 268435456; i++) {
  for (int i = 0; i < 172032; i++) {
    /* ANSI esc code to clear line, carrage return, then print on the same line */
    //    printf("\033[2K\r%d", i);
    sm_add(map, i);
  }

  return MUNIT_OK;
}

static void *
test_scale_worst_case_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 1024);

  return (void *)map;
}
static void
test_scale_worst_case_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_scale_worst_case(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  /* Worst case a map can contain 2048 bits in 265 + 8 = 264 bytes.
     So, in a 1KiB buffer you have:
       (1024 KiB / 264 bytes) * 2048 = 8,134,407.75758 bits
     or 33.3 GiB of 4KiB pages. Let's investigate, and find out if that's the case.
  */

  /* Set every other bit, that has to be the "worst case" for this index. */
  // for (int i = 0; i < 8134407; i += 2) {
  for (int i = 0; i < 7744; i += 2) {
    /* ANSI esc code to clear line, carrage return, then print on the same line */
    //    printf("\033[2K\r%d", i);
    sm_add(map, i);
  }

  return MUNIT_OK;
}

/* -------------------------- Performance Tests */

static void *
test_perf_span_solo_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024 * 3, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 3 * 1024);

  return (void *)map;
}
static void
test_perf_span_solo_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_perf_span_solo(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;
  int located_at, placed_at, amt = 500;

  assert_ptr_not_null(map);

  for (int i = 1; i < amt; i++) {
    for (int length = 1; length <= 100; length++) {
      sm_clear(map);
      placed_at = sm_add_span(map, amt, length);
      located_at = sm_span(map, 0, length, true);
      if (placed_at != located_at)
        logf("a: i = %d, length = %d\tplaced_at %d located_at %d\n", i, length, placed_at, located_at);
    }
  }
  return MUNIT_OK;
}

static void *
test_perf_span_tainted_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024 * 3, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sm_init(map, buf, 3 * 1024);

  return (void *)map;
}
static void
test_perf_span_tainted_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  assert_ptr_not_null(map->m_data);
  munit_free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_perf_span_tainted(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  // double stop, start;
  (void)params;

  assert_ptr_not_null(map);

  int located_at, placed_at, amt = 500;
  for (int i = 1; i < amt; i++) {
    for (int j = 100; j <= 10; j++) {
      sm_clear(map);
      populate_map(map, 1024, 1 * 1024);
      placed_at = sm_add_span(map, amt, j);
      // start = nsts();
      located_at = sm_span(map, 0, j, true);
      // stop = nsts();
      // double amt = (stop - start) * 1e6;
      // if (amt > 0) {
      // fprintf(stdout, "%0.8f\n", amt);
      // }
      if (located_at >= placed_at)
        logf("b: i = %d, j = %d\tplaced_at %d located_at %d\n", i, j, placed_at, located_at);
    }
  }

  return MUNIT_OK;
}

// clang-format off
static MunitTest scale_test_suite[] = {
  { (char *)"/lots-o-spans", test_scale_lots_o_spans, test_scale_lots_o_spans_setup, test_scale_lots_o_spans_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
#ifdef SCALE_ONDREJ
{ (char *)"/ondrej", test_scale_ondrej, test_scale_ondrej_setup, test_scale_ondrej_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
#endif
  { (char *)"/fuzz", test_scale_fuzz, test_scale_fuzz_setup, test_scale_fuzz_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/alternating", test_scale_alternating, test_scale_alternating_setup, test_scale_alternating_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/spans_come_spans_go", test_scale_spans_come_spans_go, test_scale_spans_come_spans_go_setup, test_scale_spans_come_spans_go_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/best-case", test_scale_best_case, test_scale_best_case_setup, test_scale_best_case_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/worst-case", test_scale_worst_case, test_scale_worst_case_setup, test_scale_worst_case_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL } };
// clang-format on

// clang-format off
static MunitTest perf_test_suite[] = {
  { (char *)"/span/solo", test_perf_span_solo, test_perf_span_solo_setup, test_perf_span_solo_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/span/tainted", test_perf_span_tainted, test_perf_span_tainted_setup, test_perf_span_tainted_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL } };
// clang-format on

// clang-format off
static MunitSuite other_test_suite[] = {
  { "/api", api_test_suite, NULL, 1, MUNIT_SUITE_OPTION_NONE },
  { "/qc", qc_test_suite, NULL, 1, MUNIT_SUITE_OPTION_NONE },
  { "/integration", integration_test_suite, NULL, 1, MUNIT_SUITE_OPTION_NONE },
  { "/perf", perf_test_suite, NULL, 1, MUNIT_SUITE_OPTION_NONE },
  { "/scale", scale_test_suite, NULL, 1, MUNIT_SUITE_OPTION_NONE },
  { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE } };
// clang-format on

// clang-format off
static MunitTest sparsemap_test_suite[] = {
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
// clang-format on

static const MunitSuite main_test_suite = { (char *)"", sparsemap_test_suite, other_test_suite, 1, MUNIT_SUITE_OPTION_NONE };

int
main(int argc, char *argv[MUNIT_ARRAY_PARAM(argc + 1)])
{
  struct user_data info;

  /* Disable buffering on std{out,err} to avoid having to call fflush(). */
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  QCC_init(0);

  return munit_suite_main(&main_test_suite, (void *)&info, argc, argv);
}

/* ARGS: --no-fork --seed 8675309 */
