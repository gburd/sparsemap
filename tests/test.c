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

#include <sys/types.h>

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../include/sparsemap.h"
#include "munit.h"

#if defined(_MSC_VER)
#pragma warning(disable : 4127)
#endif

#include "common.c"

struct user_data { };

void
__populate_map(sparsemap_t *map, size_t size, size_t max_value)
{
  int array[size];

  setup_test_array(array, size, max_value);
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
  sparsemap_init(map, buf, sizeof(buf) / sizeof(buf[0]), 0);
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
  __populate_map(map, 1024, 3 * 1024);

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

  assert_true(map->m_data_size == 1024);

  sparsemap_clear(map);

  assert_true(map->m_data_size == 1024);
  assert_true(map->m_data_used == sizeof(uint32_t));

  return MUNIT_OK;
}

static void *
test_api_open_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);
  __populate_map(map, 1024, 3 * 1024);
  assert_true(map->m_data_used == 1024 + sizeof(uint32_t));

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
  assert_true(map->m_data_used == sm->m_data_used);
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
  __populate_map(map, 1024, 3 * 1024);

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
test_api_is_set_setup(const MunitParameter params[], void *user_data)
{
  uint8_t *buf = munit_calloc(1024, sizeof(uint8_t));
  sparsemap_t *map = (sparsemap_t *)test_api_setup(params, user_data);

  sparsemap_init(map, buf, 1024, 0);
  __populate_map(map, 1024, 3 * 1024);

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

static MunitTest api_test_suite[] = { { (char *)"/api/static_init", test_api_static_init, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/clear", test_api_clear, test_api_clear_setup, test_api_clear_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/set_data_size", test_api_set_data_size, test_api_set_data_size_setup, test_api_set_data_size_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/is_set", test_api_is_set, test_api_is_set_setup, test_api_is_set_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
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
