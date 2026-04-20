/*
 * To the extent possible under law, the author(s) of this file have
 * waived all copyright and related or neighboring rights to this
 * work.  See <https://creativecommons.org/publicdomain/zero/1.0/> for
 * details.
 */

#define MUNIT_NO_FORK (1)
#define MUNIT_ENABLE_ASSERT_ALIASES (1)

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../include/bitmapset.h"
#include "common.h"
#include "munit.h"

#define munit_free free

#if defined(_MSC_VER)
#pragma warning(disable : 4127)
#endif

#define SELECT_FALSE

typedef uint64 bms_idx_t;

struct user_data {
  int foo;
};

/* -------------------------- Supporting Functions for Testing */

void
populate_map(Bitmapset *map, int size, int max_value)
{
  int array[size];
  size_t before;

  setup_test_array(array, size, max_value);
  shuffle(array, size);
  before = bms_num_members(map);
  for (int i = 0; i < size; i++) {
    map = bms_add_member(map, array[i]);
    bool set = bms_is_member(array[i], map);
    assert_true(set);
  }
  assert_true(bms_num_members(map) == before + size);
}

static void *
test_api_setup(const MunitParameter params[], void *user_data)
{
  struct test_info *info = (struct test_info *)user_data;
  (void)info;
  (void)params;
  Bitmapset *map = munit_calloc(1, sizeof(Bitmapset));
  assert_ptr_not_null(map);
  return (void *)(uintptr_t)map;
}

static void
test_api_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}

/* -------------------------- API Tests */

static MunitResult
test_api_new(const MunitParameter params[], void *data)
{
  Bitmapset *map = bms_create(1024);
  (void)params;
  (void)data;

  assert_ptr_not_null(map);
  assert_true(map->size == 1024);
  assert_true(map->used == sizeof(uint32_t));

  munit_free(map);

  return MUNIT_OK;
}

static MunitResult
test_api_new_realloc(const MunitParameter params[], void *data)
{
  Bitmapset *map = bms_create(1024);
  (void)params;
  (void)data;

  assert_ptr_not_null(map);
  assert_true(map->size == 1024);
  assert_true(map->used == sizeof(uint32_t));

  map = bms_resize(map, NULL, 2048);
  assert_true(map->size == 2048);
  assert_true(map->used == sizeof(uint32_t));

  munit_free(map);

  return MUNIT_OK;
}

static MunitResult
test_api_new_heap(const MunitParameter params[], void *data)
{
  Bitmapset *map;
  uint8_t *buf;
  (void)params;
  (void)data;

  map = munit_malloc(sizeof(Bitmapset));
  assert_ptr_not_null(map);
  buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  bms_init(map, buf, 1024);

  bms_init(map, buf, 1024);
  assert_ptr_equal(buf, map->data);
  assert_true(map->size == 1024);
  assert_true(map->used == sizeof(uint32_t));

  munit_free(map->data);
  munit_free(map);

  return MUNIT_OK;
}

static MunitResult
test_api_new_static(const MunitParameter params[], void *data)
{
  Bitmapset a_map, *map = &a_map;
  uint8_t *buf;
  (void)params;
  (void)data;

  buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  bms_init(map, buf, 1024);

  bms_init(map, buf, 1024);
  assert_ptr_equal(buf, map->data);
  assert_true(map->size == 1024);
  assert_true(map->used == sizeof(uint32_t));

  munit_free(map->data);

  return MUNIT_OK;
}

static MunitResult
test_api_new_stack(const MunitParameter params[], void *data)
{
  Bitmapset a_map, *map = &a_map;
  uint8_t buf[1024] = { 0 };

  (void)params;
  (void)data;

  bms_init(map, buf, 1024);
  assert_ptr_equal(&buf, map->data);
  assert_true(map->size == 1024);
  assert_true(map->used == sizeof(uint32_t));

  return MUNIT_OK;
}

static void *
test_api_clear_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_clear_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_clear(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  map = bms_add_member(map, 42);
  assert_true(bms_is_member(42, map));
  bms_empty(map);
  assert_false(bms_is_member(42, map));

  return MUNIT_OK;
}

static void *
test_api_open_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_open_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_open(const MunitParameter params[], void *data)
{
  Bitmapset _sm, *sm = &_sm, *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  bms_attach_buffer(sm, (uint8_t *)map->data, map->size);
  for (int i = 0; i < 3 * 1024; i++) {
    assert_true(bms_is_member(i, sm) == bms_is_member(i, map));
  }

  return MUNIT_OK;
}

static void *
test_api_set_data_size_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_set_data_size_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_set_data_size(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);
  assert_true(map->size == 1024);
  assert_true(map->size == bms_size(map));
  bms_resize(map, NULL, 512);
  assert_true(map->size == 512);
  assert_true(map->size == bms_size(map));
  return MUNIT_OK;
}

static void *
test_api_remaining_capacity_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_remaining_capacity_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_remaining_capacity(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  int i = 0;
  double cap;
  do {
    map = bms_add_member(map, i++);
    cap = bms_get_utilization(map);
  } while (cap > 1.0 && errno != ENOSPC);
  errno = 0;
  assert_true(cap <= 2.0);

  bms_empty(map);
  cap = bms_get_utilization(map);
  assert_true(cap > 99);

  i = 0;
  do {
    int p = munit_rand_int_range(0, 150000);
    map = bms_add_member(map, p);
    i++;
    cap = bms_size(map);
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
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_get_capacity_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_get_capacity(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  map = bms_add_member(map, 42);
  assert_true(bms_is_member(42, map));
  assert_true(bms_size(map) == 1024);

  return MUNIT_OK;
}

static void *
test_api_is_set_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_is_set_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_is_set(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  map = bms_add_member(map, 42);
  assert_true(bms_is_member(42, map));

  return MUNIT_OK;
}

static void *
test_api_set_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_set_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_set(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  assert_false(bms_is_member(1, map));
  assert_false(bms_is_member(8192, map));
  map = bms_add_member(map, 1);
  map = bms_add_member(map, 8192);
  assert_true(bms_is_member(1, map));
  assert_true(bms_is_member(8192, map));
  map = bms_del_member(map, 1);
  map = bms_del_member(map, 8192);
  assert_false(bms_is_member(1, map));
  assert_false(bms_is_member(8192, map));

  return MUNIT_OK;
}

static void *
test_api_get_size_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_get_size_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_get_size(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  size_t size = bms_get_used_size(map);
  assert_true(size > 400);

  return MUNIT_OK;
}

static void *
test_api_count_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_count_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_count(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  assert_true(bms_num_members(map) == 1024);

  bms_empty(map);
  map = bms_add_member(map, 0);
  assert_true(bms_num_members(map) == 1);

  map = bms_add_member(map, 8675309);
  assert_true(bms_num_members(map) == 2);

  bms_empty(map);
  for (int i = 0; i < 512; i++) {
    map = bms_add_member(map, i + 13);
  }
  assert_true(bms_num_members(map) == 512);

  bms_empty(map);
  assert_true(bms_num_members(map) == 0);

  return MUNIT_OK;
}

static MunitResult
test_api_get_data(const MunitParameter params[], void *data)
{
  (void)data;
  (void)params;
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)bms_create_with_buffer(buf, 1024);
  assert_ptr_not_null(map);
  populate_map(map, 1024, 3 * 1024);

  assert_true(map->data == buf);

  munit_free(buf);
  munit_free(map);

  return MUNIT_OK;
}

static void *
test_api_get_starting_offset_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_get_starting_offset_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_get_starting_offset(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  map = bms_add_member(map, 0);
  assert_true(bms_first_member(map) == 0);
  bms_empty(map);

  map = bms_add_member(map, 1);
  assert_true(bms_first_member(map) == 1);
  bms_empty(map);

  map = bms_add_member(map, 1025);
  assert_true(bms_first_member(map) == 1025);
  bms_empty(map);

  for (int i = 0; i < 1000; i++) {
    map = bms_add_member(map, i);
  }
  assert_true(bms_first_member(map) == 0);
  bms_empty(map);

  for (int i = 0; i < 1000; i++) {
    map = bms_add_member(map, i + 1024);
  }
  assert_true(bms_first_member(map) == 1024);
  bms_empty(map);

  map = bms_add_member(map, 13012);
  assert_true(bms_first_member(map) == 13012);

  return MUNIT_OK;
}

static void *
test_api_get_ending_offset_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_get_ending_offset_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_get_ending_offset(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  map = bms_add_member(map, 0);
  assert_true(bms_last_member(map) == 0);
  bms_empty(map);

  map = bms_add_member(map, 0);
  map = bms_add_member(map, 1);
  assert_true(bms_last_member(map) == 1);
  bms_empty(map);

  map = bms_add_member(map, 0);
  map = bms_add_member(map, 67);
  map = bms_add_member(map, 1002);
  map = bms_add_member(map, 3087);
  map = bms_add_member(map, 13012);
  assert_true(bms_last_member(map) == 13012);

  return MUNIT_OK;
}

static void *
test_api_get_starting_offset_rolling_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  Bitmapset *map = bms_create(10 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_api_get_starting_offset_rolling_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_api_get_starting_offset_rolling(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  for (bms_idx_t i = 0; i < 10 * 2048; i++) {
    map = bms_add_member(map, i);
    if (i > 2047) {
      map = bms_del_member(map, i - 2048);
      assert_true(bms_first_member(map) == i - 2047);
      // printf("%d\t%d\t%zu\n", i, i - 2047, bms_first_member(map));
    }
  }
  return MUNIT_OK;
}

static void *
test_api_scan_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);
  bms_init(map, buf, 1024);
  return (void *)map;
}
static void
test_api_scan_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
void
scan_for_0xfeedfacebadcoffee(bms_idx_t v[], size_t n, void *aux)
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
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);
  sm_bitmap_from_uint64(map, 0, ((uint64_t)0xfeedface << 32) | 0xbadc0ffee);
  bms_scan(map, scan_for_0xfeedfacebadcoffee, 0, NULL);

  return MUNIT_OK;
}

static void *
test_api_split_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);
  return (void *)map;
}
static void
test_api_split_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_split(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  uint8_t buf[1024] = { 0 };
  Bitmapset portion;
  (void)params;

  assert_ptr_not_null(map);

  bms_init(&portion, buf, 1024);

  for (bms_idx_t off = 0; off < 1024; off++) {
    for (bms_idx_t seg = 0; seg < 10 * 1024; seg += 1024) {

      for (bms_idx_t i = 0; i < 1024; i++) {
        map = bms_add_member(map, i + seg);
      }
      for (bms_idx_t i = 0; i < 1024; i++) {
        assert_true(bms_is_member(i + seg, map));
        assert_false(bms_is_member(i + seg, &portion));
      }

      bms_split(map, seg + off, &portion);

      for (bms_idx_t i = seg; i < seg + 1024; i++) {
        if (i < seg + off) {
          assert_true(bms_is_member(i, map));
          assert_false(bms_is_member(i, &portion));
        } else {
          assert_false(bms_is_member(i, map));
          assert_true(bms_is_member(i, &portion));
        }
      }
      bms_empty(map);
      bms_empty(&portion);
    }
  }

  for (bms_idx_t i = 0; i < 100; i++) {
    map = bms_add_member(map, i);
  }
  for (bms_idx_t i = 0; i < 100; i++) {
    assert_true(bms_is_member(i, map));
    assert_false(bms_is_member(i, &portion));
  }

  bms_idx_t offset = bms_split(map, (bms_idx_t)-1, &portion);

  for (size_t i = 0; i < offset; i++) {
    assert_true(bms_is_member(i, map));
    assert_false(bms_is_member(i, &portion));
  }
  for (int i = offset; i < 100; i++) {
    assert_false(bms_is_member(i, map));
    assert_true(bms_is_member(i, &portion));
  }

  bms_empty(&portion);
  bms_empty(map);

  bms_init(&portion, buf, 1024);
  for (bms_idx_t i = 0; i < 13; i++) {
    map = bms_add_member(map, i + 24);
  }

  offset = bms_split(map, SPARSEMAP_IDX_MAX, &portion);

  for (bms_idx_t i = 0; i < offset - 24; i++) {
    assert_true(bms_is_member(i + 24, map));
    assert_false(bms_is_member(i + 24, &portion));
  }
  for (bms_idx_t i = offset - 24; i < 13; i++) {
    assert_false(bms_is_member(i + 24, map));
    assert_true(bms_is_member(i + 24, &portion));
  }

  return MUNIT_OK;
}

static void *
test_api_merge_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  Bitmapset *map = bms_create(10 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_api_merge_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_api_merge(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  Bitmapset *other = bms_create(1024);
  (void)params;

  assert_ptr_not_null(map);
  assert_ptr_not_null(other);

  // Merge two empty maps to get an empty map.
  bms_union(map, other);

  // Merge a single set bit in the first chunk into the empty map.
  bms_add_member(other, 0);
  bms_union(map, other);
  assert_true(bms_is_member(other, 0));
  assert_true(bms_is_member(map, 0));
  bms_empty(map);
  bms_empty(other);

  // Merge two maps with the same single bit set.
  map = bms_add_member(map, 0);
  bms_add_member(other, 0);
  bms_union(map, other);
  assert_true(bms_is_member(map, 0));
  bms_empty(map);
  bms_empty(other);

  // Merge an empty map with one that has the first bit set.
  map = bms_add_member(map, 0);
  bms_union(map, other);
  assert_true(bms_is_member(map, 0));
  bms_empty(map);
  bms_empty(other);

  bms_add_member(other, 2049);
  bms_union(map, other);
  assert_true(bms_is_member(map, 2049));
  bms_empty(map);
  bms_empty(other);

  bms_add_member(other, 1);
  bms_add_member(other, 2049);
  map = bms_add_member(map, 2050);
  bms_add_member(other, 4097);
  map = bms_add_member(map, 6113);
  bms_add_member(other, 8193);
  bms_union(map, other);
  assert_true(bms_is_member(map, 1));
  assert_true(bms_is_member(map, 2049));
  assert_true(bms_is_member(map, 2050));
  assert_true(bms_is_member(map, 4097));
  assert_true(bms_is_member(map, 6113));
  assert_true(bms_is_member(map, 8193));
  for (int i = 0; i < 10000; i++) {
    if (i == 2049 || i == 1 || i == 2050 || i == 4097 || i == 6113 || i == 8193)
      continue;
    else
      assert_false(bms_is_member(map, i));
  }
  bms_empty(map);
  bms_empty(other);

  map = bms_add_member(map, 0);
  map = bms_add_member(map, 2048);
  map = bms_add_member(map, 8193);
  for (int i = 2049; i < 4096; i++) {
    bms_add_member(other, i);
  }
  bms_union(map, other);
  assert(bms_is_member(map, 0));
  assert(bms_is_member(map, 2048));
  assert(bms_is_member(map, 8193));
  for (int i = 2049; i < 4096; i++) {
    assert(bms_is_member(map, i));
  }
  bms_empty(map);
  bms_empty(other);

  for (int i = 2049; i < 4096; i++) {
    map = bms_add_member(map, i);
  }
  bms_split(map, 2051, other);
  for (int i = 2049; i < 4096; i++) {
    if (i < 2051) {
      assert_true(bms_is_member(map, i));
      assert_false(bms_is_member(other, i));
    } else {
      assert_false(bms_is_member(map, i));
      assert_true(bms_is_member(other, i));
    }
  }
  bms_union(map, other);
  for (int i = 2049; i < 4096; i++) {
    bms_is_member(map, i);
  }

  munit_free(other);
  return MUNIT_OK;
}

static void *
test_api_select_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);
  sm_bitmap_from_uint64(map, 0, ((uint64_t)0xfeedface << 32) | 0xbadc0ffee);

  return (void *)map;
}
static void
test_api_select_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_select(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  /* NOTE: select() is 0-based, to get the bit position of the 1st logical bit set
     call select(map, 0), to get the 18th, select(map, 17), etc. */
  assert_true(bms_select(map, 0, true) == 1);
  assert_true(bms_select(map, 4, true) == 6);
  assert_true(bms_select(map, 17, true) == 26);

  return MUNIT_OK;
}

#ifdef SELECT_FALSE
static void *
test_api_select_false_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);
  sm_bitmap_from_uint64(map, 0, ((uint64_t)0xfeedface << 32) | 0xbadc0ffee);

  return (void *)map;
}
static void
test_api_select_false_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_select_false(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;
  assert_ptr_not_null(map);

  /* First few 0/off/unset-bits in ((uint64_t)0xfeedface << 32) | 0xbadc0ffee) expressed as an array of offsets. */
  size_t off[] = { 0, 4, 16, 17, 18, 19, 20, 21, 25, 28, 30, 36, 37, 40, 42, 49, 52, 56, 64, 65 };
  for (size_t i = 0; i < 20; i++) {
    bms_idx_t f = bms_select(map, i, false);
    assert_true(f == off[i]);
    assert_true(bms_is_member(map, f) == false);
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
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);
  sm_bitmap_from_uint64(map, 0, ((uint64_t)0xfeedface << 32) | 0xbadc0ffee);

  return (void *)map;
}
static void
test_api_select_neg_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_select_neg(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  map = bms_add_member(map, 42);
  map = bms_add_member(map, 420);
  map = bms_add_member(map, 4200);

  f = bms_select(map, 0, false);
  assert_true(f == 0);

  f = bms_select(map, -1, true);
  assert_true(f == 4200);
  f = bms_select(map, -2, true);
  assert_true(f == 420);
  f = bms_select(map, -3, true);
  assert_true(f == 42);

  return MUNIT_OK;
}
#endif

static void *
test_api_rank_true_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_rank_true_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_rank_true(const MunitParameter params[], void *data)
{
  int r1, r2;
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  for (int i = 0; i < 10; i++) {
    map = bms_add_member(map, i);
  }
  /* rank() is also 0-based, for consistency (and confusion sake); consider the
     range as [start, end] of [0, 9] counts the bits set in the first 10
     positions (starting from the LSB) in the index. */
  r1 = rank_uint64((uint64_t)-1, 0, 9);
  r2 = bms_rank(map, 0, 9, true);
  assert_true(r1 == r2);
  assert_true(bms_rank(map, 0, 9, true) == 10);
  assert_true(bms_rank(map, 1000, 1050, true) == 0);

  bms_empty(map);

  for (int i = 0; i < 10000; i++) {
    map = bms_add_member(map, i);
  }
  bms_idx_t hole = 4999;
  map = bms_add_member(map, hole, false);
  for (size_t i = 0; i < 10000; i++) {
    for (size_t j = i; j < 10000; j++) {
      size_t amt = (i > j) ? 0 : j - i + 1 - ((hole >= i && j >= hole) ? 1 : 0);
      size_t r = bms_rank(map, i, j, true);
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
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_rank_false_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_rank_false(const MunitParameter params[], void *data)
{
  int r;
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  // empty map
  for (int i = 0; i < 10000; i++) {
    for (int j = i; j < 10000; j++) {
      r = bms_rank(map, i, j, false);
      assert_true(r == j - i + 1);
    }
  }

  // One chunk means not so empty now!
  bms_idx_t hole = 4999;
  map = bms_add_member(map, hole);
  for (size_t i = 0; i < 10000; i++) {
    for (size_t j = i; j < 10000; j++) {
      int amt = (i > j) ? 0 : j - i + 1 - ((hole >= i && j >= hole) ? 1 : 0);
      r = bms_rank(map, i, j, false);
      assert_true(r == amt);
    }
  }

  bms_empty(map);
  map = bms_add_member(map, 1);
  map = bms_add_member(map, 11);
  r = bms_rank(map, 0, 11, false);
  assert_true(r == 10);

  return MUNIT_OK;
}

static void *
test_api_span_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);

  return (void *)map;
}
static void
test_api_span_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_span(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  int located_at, placed_at, amt = 10000;

  placed_at = sm_add_span(map, amt, 1);
  located_at = bms_find_span(map, 0, 1, true);
  assert_true(located_at == placed_at);

  bms_empty(map);

  placed_at = sm_add_span(map, amt, 50);
  located_at = bms_find_span(map, 0, 50, true);
  assert_true(located_at == placed_at);

  bms_empty(map);

  placed_at = sm_add_span(map, amt, 50);
  located_at = bms_find_span(map, placed_at / 2, 50, true);
  assert_true(located_at == placed_at);

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
  { (char *)"/get_starting_offset", test_api_get_starting_offset, test_api_get_starting_offset_setup, test_api_get_starting_offset_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/get_starting_offset/rolling", test_api_get_starting_offset_rolling, test_api_get_starting_offset_rolling_setup, test_api_get_starting_offset_rolling_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/get_ending_offset", test_api_get_ending_offset, test_api_get_ending_offset_setup, test_api_get_ending_offset_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/scan", test_api_scan, test_api_scan_setup, test_api_scan_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/split", test_api_split, test_api_split_setup, test_api_split_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/merge", test_api_merge, test_api_merge_setup, test_api_merge_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/select/true", test_api_select, test_api_select_setup, test_api_select_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
#ifdef SELECT_FALSE
  { (char *)"/select/false", test_api_select_false, test_api_select_false_setup, test_api_select_false_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
#endif
#ifdef SELECT_NEG
  { (char *)"/select/neg", test_api_select_neg, test_api_select_neg_setup, test_api_select_neg_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
#endif
  { (char *)"/rank/true", test_api_rank_true, test_api_rank_true_setup, test_api_rank_true_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/rank/false", test_api_rank_false, test_api_rank_false_setup, test_api_rank_false_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/span", test_api_span, test_api_span_setup, test_api_span_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
// clang-format on

/* -------------------------- Scale Tests */

static void *
test_scale_lots_o_spans_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  Bitmapset *map = bms_create(10 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_scale_lots_o_spans_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_scale_lots_o_spans(const MunitParameter params[], void *data)
{
  size_t amt = 897915; // 268435456
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  for (size_t i = 0; i < amt;) {
    int l = i % 31 + 16;
    sm_add_span(map, 10000, l);
    if (errno == ENOSPC) {
      map = bms_resize(map, NULL, bms_size(map) * 2);
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
  Bitmapset *map = bms_create(10 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_scale_ondrej_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_scale_ondrej(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  bms_idx_t stride = 18;
  //  bms_idx_t top = 268435456;
  bms_idx_t top = 2000;
  bms_idx_t needle = munit_rand_int_range(1, top / stride);
  for (bms_idx_t i = 0; i < top / stride; i += stride) {
    for (bms_idx_t j = 0; j < stride; j++) {
      bool set = (i != needle) ? (j < 10) : (j < 9);
      if (set)
	map = bms_add_member(map, i);
      else
	map = bms_del_member(map, i);
      if (errno == ENOSPC) {
        map = bms_resize(map, NULL, bms_size(map) * 2);
        errno = 0;
      }
    }
    assert_true(sm_is_span(map, i + ((i != needle) ? 10 : 9), (i != needle) ? 8 : 9, true));
  }
  bms_idx_t a = bms_find_span(map, 0, 9, false);
  bms_idx_t l = a / stride;
  printf("%ld\t%ld\n", a, l);
  assert_true(l == needle);
  return MUNIT_OK;
}
#endif // SCALE_ONDREJ

static void *
test_scale_fuzz_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  Bitmapset *map = bms_create(10 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_scale_fuzz_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_scale_fuzz(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;
  (void)map; // TODO...
  return MUNIT_OK;
}

static void *
test_scale_alternating_setup(const MunitParameter params[], void *user_data)
{
  (void)params;
  (void)user_data;
  Bitmapset *map = bms_create(10 * 1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_scale_alternating_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
extern char *bytes_as(double bytes, char *s, size_t size);
static MunitResult
test_scale_alternating(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  for (bms_idx_t i = 0; i < (1000 * 8192); i++) {
    if (i % 2) {
      map = bms_add_member(map, i);
      if (!bms_is_member(i, map)) {
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
  Bitmapset *map = bms_create(1024);
  assert_ptr_not_null(map);
  return (void *)map;
}
static void
test_scale_spans_come_spans_go_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map);
  munit_free(map);
}
static MunitResult
test_scale_spans_come_spans_go(const MunitParameter params[], void *data)
{
  size_t amt = 8192; // 268435456; // ~5e7 iterations due to 2e9 / avg(l)
  Bitmapset *map = (Bitmapset *)data;
  (void)params;

  assert_ptr_not_null(map);

  for (size_t i = 0; i < amt;) {
    int l = i % 31 + 16;
    sm_add_span(map, amt, l);
    if (errno == ENOSPC) {
      map = bms_resize(map, NULL, bms_size(map) + 1024);
      assert_ptr_not_null(map);
      errno = 0;
    }

    /* After 1,000 spans are in the map start consuming a span every iteration. */
    if (l > 1000) {
      do {
        int s = munit_rand_int_range(1, 30);
        int o = munit_rand_int_range(1, 268435456 - s - 1);
        size_t b = bms_find_span(map, o, s, true);
        if (b == SPARSEMAP_IDX_MAX) {
          continue;
        }
        for (int j = b; j < s; j++) {
          assert_true(bms_is_member(map, j) == true);
        }
        for (int j = b; j < s; j++) {
          map = bms_del_member(map, j);
        }
        for (int j = b; j < s; j++) {
          assert_true(bms_is_member(map, j) == false);
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
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);

  return (void *)map;
}
static void
test_scale_best_case_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_scale_best_case(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
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
    map = bms_add_member(map, i);
  }

  return MUNIT_OK;
}

static void *
test_scale_worst_case_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 1024);

  return (void *)map;
}
static void
test_scale_worst_case_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_scale_worst_case(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
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
    map = bms_add_member(map, i);
  }

  return MUNIT_OK;
}

/* -------------------------- Performance Tests */

static void *
test_perf_span_solo_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024 * 3, sizeof(uint8_t));
  assert_ptr_not_null(buf);
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 3 * 1024);

  return (void *)map;
}
static void
test_perf_span_solo_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_perf_span_solo(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  (void)params;
  int located_at, placed_at, amt = 500;

  assert_ptr_not_null(map);

  for (int i = 1; i < amt; i++) {
    for (int length = 1; length <= 100; length++) {
      bms_empty(map);
      placed_at = sm_add_span(map, amt, length);
      located_at = bms_find_span(map, 0, length, true);
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
  Bitmapset *map = (Bitmapset *)test_api_setup(params, user_data);

  bms_init(map, buf, 3 * 1024);

  return (void *)map;
}
static void
test_perf_span_tainted_tear_down(void *fixture)
{
  Bitmapset *map = (Bitmapset *)fixture;
  assert_ptr_not_null(map->data);
  munit_free(map->data);
  test_api_tear_down(fixture);
}
static MunitResult
test_perf_span_tainted(const MunitParameter params[], void *data)
{
  Bitmapset *map = (Bitmapset *)data;
  // double stop, start;
  (void)params;

  assert_ptr_not_null(map);

  int located_at, placed_at, amt = 500;
  for (int i = 1; i < amt; i++) {
    for (int j = 100; j <= 10; j++) {
      bms_empty(map);
      populate_map(map, 1024, 1 * 1024);
      placed_at = sm_add_span(map, amt, j);
      // start = nsts();
      located_at = bms_find_span(map, 0, j, true);
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
  { "/perf", perf_test_suite, NULL, 1, MUNIT_SUITE_OPTION_NONE },
  { "/scale", scale_test_suite, NULL, 1, MUNIT_SUITE_OPTION_NONE },
  { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE } };
// clang-format on

// clang-format off
static MunitTest Bitmapsetest_suite[] = {
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
// clang-format on

static const MunitSuite main_test_suite = { (char *)"/bitmapset", bitmapset_test_suite, other_test_suite, 1, MUNIT_SUITE_OPTION_NONE };

int
main(int argc, char *argv[MUNIT_ARRAY_PARAM(argc + 1)])
{
  struct user_data info;

  /* Disable buffering on std{out,err} to avoid having to call fflush(). */
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  return munit_suite_main(&main_test_suite, (void *)&info, argc, argv);
}

/* ARGS: --no-fork --seed 8675309 */
