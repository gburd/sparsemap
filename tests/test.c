/*
 * smartmap is MIT-licensed, but for this file:
 *
 * To the extent possible under law, the author(s) of this file have
 * waived all copyright and related or neighboring rights to this
 * work.  See <https://creativecommons.org/publicdomain/zero/1.0/> for
 * details.
 */

#define MUNIT_NO_FORK (1)
#define MUNIT_ENABLE_ASSERT_ALIASES (1)

#include <stdlib.h>
#include <unistd.h>

#include "../include/sparsemap.h"
#include "common.h"
#include "munit.h"

#if defined(_MSC_VER)
#pragma warning(disable : 4127)
#endif

struct user_data {
  int foo;
};

void
populate_map(sparsemap_t *map, int size, int max_value)
{
  int array[size];

  setup_test_array(array, size, max_value);
  ensure_sequential_set(array, size, 10);
  shuffle(array, size);
  for (int i = 0; i < size; i++) {
    sparsemap_set(map, array[i], true);
    munit_assert_true(sparsemap_is_set(map, array[i]));
  }
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
  free(map);
}

static MunitResult
test_api_static_init(const MunitParameter params[], void *data)
{
  sparsemap_t a_map, *map = &a_map;
  uint8_t buf[1024];

  (void)params;
  (void)data;

  assert_ptr_not_null(map);
  sparsemap_init(map, buf, 1024, 0);
  assert_ptr_equal(&buf, map->m_data);
  assert_true(map->m_data_size == 1024);
  assert_true(map->m_data_used == sizeof(uint32_t));

  return MUNIT_OK;
}

static void *
test_api_clear_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);

  return (void *)map;
}
static void
test_api_clear_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_clear(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  sparsemap_set(map, 42, true);
  assert_true(sparsemap_is_set(map, 42));
  sparsemap_clear(map);
  assert_false(sparsemap_is_set(map, 42));

  return MUNIT_OK;
}

static void *
test_api_open_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_open_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_open(const MunitParameter params[], void *data)
{
  sparsemap_t _sm, *sm = &_sm, *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  sparsemap_open(sm, map->m_data, map->m_data_size);
  for (int i = 0; i < 3 * 1024; i++) {
    assert_true(sparsemap_is_set(sm, i) == sparsemap_is_set(map, i));
  }

  return MUNIT_OK;
}

static void *
test_api_set_data_size_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_set_data_size_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_set_data_size(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);
  assert_true(map->m_data_size == 1024);
  assert_true(map->m_data_size == sparsemap_get_range_size(map));
  sparsemap_set_data_size(map, 512);
  assert_true(map->m_data_size == 512);
  assert_true(map->m_data_size == sparsemap_get_range_size(map));
  return MUNIT_OK;
}

static void *
test_api_remaining_capacity_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);

  return (void *)map;
}
static void
test_api_remaining_capacity_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  free(map->m_data);
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
    sparsemap_set(map, i++, true);
    cap = sparsemap_capacity_remaining(map);
  } while (cap > 1.0);
  //assert_true(i == 169985); when seed is 8675309
  assert_true(cap <= 1.0);

  sparsemap_clear(map);
  i = 0;
  do {
    int p = munit_rand_int_range(0, 150000);
    sparsemap_set(map, p, true);
    i++;
    cap = sparsemap_capacity_remaining(map);
  } while (cap > 1.0);
  //assert_true(i == 64); when seed is 8675309
  assert_true(cap <= 1.0);

  return MUNIT_OK;
}

static void *
test_api_get_range_size_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_get_range_size_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_get_range_size(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  sparsemap_set(map, 42, true);
  assert_true(sparsemap_is_set(map, 42));
  size_t size = sparsemap_get_range_size(map);
  assert_true(size == 1024);

  return MUNIT_OK;
}

static void *
test_api_is_set_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_is_set_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_is_set(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  sparsemap_set(map, 42, true);
  assert_true(sparsemap_is_set(map, 42));

  return MUNIT_OK;
}

static void *
test_api_set_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);

  return (void *)map;
}
static void
test_api_set_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_set(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  assert_false(sparsemap_is_set(map, 1));
  assert_false(sparsemap_is_set(map, 8192));
  sparsemap_set(map, 1, true);
  sparsemap_set(map, 8192, true);
  assert_true(sparsemap_is_set(map, 1));
  assert_true(sparsemap_is_set(map, 8192));
  sparsemap_set(map, 1, false);
  sparsemap_set(map, 8192, false);
  assert_false(sparsemap_is_set(map, 1));
  assert_false(sparsemap_is_set(map, 8192));

  return MUNIT_OK;
}

static void *
test_api_get_start_offset_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_get_start_offset_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_get_start_offset(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  sparsemap_set(map, 42, true);
  assert_true(sparsemap_is_set(map, 42));
  size_t offset = sparsemap_get_start_offset(map);
  assert_true(offset == 0);

  return MUNIT_OK;
}

static void *
test_api_get_size_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_get_size_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_get_size(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  size_t size = sparsemap_get_size(map);
  assert_true(size > 400);

  return MUNIT_OK;
}

static void *
test_api_scan_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);
  bitmap_from_uint64(map, ((uint64_t)0xfeedface << 32) | 0xbadc0ffee);

  return (void *)map;
}
static void
test_api_scan_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  free(map->m_data);
  test_api_tear_down(fixture);
}
void
scan_for_0xfeedfacebadcoffee(sm_idx_t v[], size_t n) {
  /* Called multiple times */
  ((void)v);
  ((void)n);
}
static MunitResult
test_api_scan(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  sparsemap_set(map, 4200, true);
  assert_true(sparsemap_is_set(map, 42));
  sparsemap_scan(map, scan_for_0xfeedfacebadcoffee, 0);

  return MUNIT_OK;
}

static void *
test_api_split_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);
  for(int i = 0; i < 1024; i ++) {
    sparsemap_set(map, i, true);
  }
  return (void *)map;
}
static void
test_api_split_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_split(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  uint8_t buf[1024];
  sparsemap_t portion;
  (void)params;

  assert_ptr_not_null(map);

  sparsemap_init(&portion, buf, 512, 0);
  sparsemap_split(map, 512, &portion);
  for (int i = 0; i < 512; i++) {
    assert_true(sparsemap_is_set(map, i));
    assert_false(sparsemap_is_set(&portion, i));
  }
  for (int i = 513; i < 1024; i++) {
    assert_false(sparsemap_is_set(map, i));
    assert_true(sparsemap_is_set(&portion, i));
  }

  return MUNIT_OK;
}

static void *
test_api_select_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);
  bitmap_from_uint64(map, ((uint64_t)0xfeedface << 32) | 0xbadc0ffee);

  return (void *)map;
}
static void
test_api_select_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  free(map->m_data);
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
  assert_true(sparsemap_select(map, 0) == 1);
  assert_true(sparsemap_select(map, 4) == 6);
  assert_true(sparsemap_select(map, 17) == 26);

  return MUNIT_OK;
}

static void *
test_api_rank_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);

  return (void *)map;
}
static void
test_api_rank_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_rank(const MunitParameter params[], void *data)
{
  int r1, r2;
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  for (int i = 0; i < 10; i++) {
    sparsemap_set(map, i, true);
  }
  for (int i = 0; i < 10; i++) {
    assert_true(sparsemap_is_set(map, i));
  }
  for (int i = 10; i < 1000; i++) {
    assert_true(!sparsemap_is_set(map, i));
  }
  /* rank() is also 0-based, for consistency (and confusion sake); consider the
     range as [start, end] of [0, 9] counts the bits set in the first 10
     positions (starting from the LSB) in the index. */
  r1 = sparsemap_rank(map, 0, 9);
  r2 = rank_uint64((uint64_t)-1, 0, 9);
  assert_true(r1 == r2);
  assert_true(sparsemap_rank(map, 0, 9) == 10);
  assert_true(sparsemap_rank(map, 1000, 1050) == 0);

  for (int i = 0; i < 10; i++) {
    for (int j = i; j < 10; j++) {
      r1 = sparsemap_rank(map, i, j);
      r2 = rank_uint64((uint64_t)-1, i, j);
      assert_true(r1 == r2);
    }
  }

  return MUNIT_OK;
}

static void *
test_api_span_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);
  populate_map(map, 1024, 3 * 1024);

  return (void *)map;
}
static void
test_api_span_tear_down(void *fixture)
{
  sparsemap_t *map = (sparsemap_t *)fixture;
  free(map->m_data);
  test_api_tear_down(fixture);
}
static MunitResult
test_api_span(const MunitParameter params[], void *data)
{
  sparsemap_t *map = (sparsemap_t *)data;
  (void)params;

  assert_ptr_not_null(map);

  sparsemap_span(map, 0, 1);

  return MUNIT_OK;
}

static MunitTest api_test_suite[] = { { (char *)"/api/static_init", test_api_static_init, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/clear", test_api_clear, test_api_clear_setup, test_api_clear_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/open", test_api_open, test_api_open_setup, test_api_open_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/set_data_size", test_api_set_data_size, test_api_set_data_size_setup, test_api_set_data_size_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/remaining_capacity", test_api_remaining_capacity, test_api_remaining_capacity_setup, test_api_remaining_capacity_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/get_range_size", test_api_get_range_size, test_api_get_range_size_setup, test_api_get_range_size_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/is_set", test_api_is_set, test_api_is_set_setup, test_api_is_set_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/set", test_api_set, test_api_set_setup, test_api_set_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/get_start_offset", test_api_get_start_offset, test_api_get_start_offset_setup, test_api_get_start_offset_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/get_size", test_api_get_size, test_api_get_size_setup, test_api_get_size_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/scan", test_api_scan, test_api_scan_setup, test_api_scan_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/split", test_api_split, test_api_split_setup, test_api_split_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/select", test_api_select, test_api_select_setup, test_api_select_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/rank", test_api_rank, test_api_rank_setup, test_api_rank_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/span", test_api_span, test_api_span_setup, test_api_span_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL } };

static MunitTest scale_tests[] = { { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL } };

static MunitSuite other_test_suite[] = { { "/scale", scale_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE }, { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE } };

static const MunitSuite main_test_suite = { (char *)"/api", api_test_suite, other_test_suite, 1, MUNIT_SUITE_OPTION_NONE };

int
main(int argc, char *argv[MUNIT_ARRAY_PARAM(argc + 1)])
{
  struct user_data info;
  return munit_suite_main(&main_test_suite, (void *)&info, argc, argv);
}

/* ARGS: --no-fork --seed 8675309 */
