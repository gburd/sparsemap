/*
 * skiplist is MIT-licensed, but for this file:
 *
 * To the extent possible under law, the author(s) of this file have
 * waived all copyright and related or neighboring rights to this
 * work.  See <https://creativecommons.org/publicdomain/zero/1.0/> for
 * details.
 */

#define MUNIT_NO_FORK (1)
#define MUNIT_ENABLE_ASSERT_ALIASES (1)

#include <stdio.h>
#include <stdlib.h>

#define __SL_DEBUG 0
#if __SL_DEBUG > 0
#include <sys/types.h>

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#endif
#if __SL_DEBUG >= 1
#define __SLD_ASSERT(cond) assert(cond)
#define __SLD_(b) b
#elif __SL_DEBUG >= 2
#define __SLD_P(...) printf(__VA_ARGS__)
#elif __SL_DEBUG >= 3
typedef struct dbg_node {
  sl_node snode;
  int value;
} dbg_node_t;

inline void
__sld_rt_ins(int error_code, sl_node *node, int top_layer, int cur_layer)
{
  dbg_node_t *ddd = sl_get_entry(node, dbg_node_t, snode);
  printf("[INS] retry (code %d) "
         "%p (top %d, cur %d) %d\n",
    error_code, node, top_layer, cur_layer, ddd->value);
}

inline void
__sld_nc_ins(sl_node *node, sl_node *next_node, int top_layer, int cur_layer)
{
  dbg_node_t *ddd = sl_get_entry(node, dbg_node_t, snode);
  dbg_node_t *ddd_next = sl_get_entry(next_node, dbg_node_t, snode);

  printf("[INS] next node changed, "
         "%p %p (top %d, cur %d) %d %d\n",
    node, next_node, top_layer, cur_layer, ddd->value, ddd_next->value);
}

inline void
__sld_rt_rmv(int error_code, sl_node *node, int top_layer, int cur_layer)
{
  dbg_node_t *ddd = sl_get_entry(node, dbg_node_t, snode);
  printf("[RMV] retry (code %d) "
         "%p (top %d, cur %d) %d\n",
    error_code, node, top_layer, cur_layer, ddd->value);
}

inline void
__sld_nc_rmv(sl_node *node, sl_node *next_node, int top_layer, int cur_layer)
{
  dbg_node_t *ddd = sl_get_entry(node, dbg_node_t, snode);
  dbg_node_t *ddd_next = sl_get_entry(next_node, dbg_node_t, snode);

  printf("[RMV] next node changed, "
         "%p %p (top %d, cur %d) %d %d\n",
    node, next_node, top_layer, cur_layer, ddd->value, ddd_next->value);
}

inline void
__sld_bm(sl_node *node)
{
  dbg_node_t *ddd = sl_get_entry(node, dbg_node_t, snode);
  printf("[RMV] node is being modified %d\n", ddd->value);
}

#define __SLD_RT_INS(e, n, t, c) __sld_rt_ins(e, n, t, c)
#define __SLD_NC_INS(n, nn, t, c) __sld_nc_ins(n, nn, t, c)
#define __SLD_RT_RMV(e, n, t, c) __sld_rt_rmv(e, n, t, c)
#define __SLD_NC_RMV(n, nn, t, c) __sld_nc_rmv(n, nn, t, c)
#define __SLD_BM(n) __sld_bm(n)
#endif

#include "../include/skiplist.h"
#include "munit.h"

#if defined(_MSC_VER)
#pragma warning(disable : 4127)
#endif

struct user_data {
  size_t n_ele;
};

typedef struct ex_node {
  sl_node snode;
  uint32_t key;
  uint32_t value;
} ex_node_t;

typedef sl_raw ex_sl_t;

static int
uint32_key_cmp(sl_node *a, sl_node *b, void *aux)
{
  ex_node_t *aa, *bb;
  (void)aux;
  aa = sl_get_entry(a, ex_node_t, snode);
  bb = sl_get_entry(b, ex_node_t, snode);

  if (aa->key < bb->key)
    return -1;
  if (aa->key > bb->key)
    return 1;
  return 0;
}

static size_t
__populate_slist(ex_sl_t *slist)
{
  size_t inserted = 0;
  uint32_t n, key;
  ex_node_t *node;

  n = munit_rand_int_range(1024, 4196);
  while (n--) {
    key = munit_rand_int_range(0, (((uint32_t)0) - 1) / 10);
    node = (ex_node_t *)calloc(sizeof(ex_node_t), 1);
    if (node == NULL)
      return MUNIT_ERROR;
    sl_init_node(&node->snode);
    node->key = key;
    node->value = key;
    if (sl_insert_nodup(slist, &node->snode) == -1)
      continue; /* a random duplicate appeared */
    else
      inserted++;
  }
  return inserted;
}

static void *
test_api_setup(const MunitParameter params[], void *user_data)
{
  struct test_info *info = (struct test_info *)user_data;
  (void)info;
  (void)params;

  ex_sl_t *slist = calloc(sizeof(ex_sl_t), 1);
  if (slist == NULL)
    return NULL;
  sl_init(slist, uint32_key_cmp);
  return (void *)(uintptr_t)slist;
}

static void
test_api_tear_down(void *fixture)
{
  ex_sl_t *slist = (ex_sl_t *)fixture;
  assert_ptr_not_null(slist);
  sl_node *cursor = sl_begin(slist);
  while (cursor) {
    assert_ptr_not_null(cursor);
    ex_node_t *entry = sl_get_entry(cursor, ex_node_t, snode);
    assert_ptr_not_null(entry);
    assert_uint32(entry->key, ==, entry->value);
    cursor = sl_next(slist, cursor);
    sl_erase_node(slist, &entry->snode);
    sl_release_node(&entry->snode);
    sl_wait_for_free(&entry->snode);
    sl_free_node(&entry->snode);
    free(entry);
  }
  sl_free(slist);
  free(fixture);
}

static void *
test_api_insert_setup(const MunitParameter params[], void *user_data)
{
  return test_api_setup(params, user_data);
}
static void
test_api_insert_tear_down(void *fixture)
{
  test_api_tear_down(fixture);
}
static MunitResult
test_api_insert(const MunitParameter params[], void *data)
{
  int ret;
  size_t inserted = 0;
  uint32_t n, key;
  sl_raw *slist = (sl_raw *)data;
  ex_node_t *node;
  (void)params;

  assert_ptr_not_null(slist);
  n = munit_rand_int_range(4096, 8192);
  while (n--) {
    key = munit_rand_int_range(0, ((uint32_t)0 - 1) / 10);
    node = (ex_node_t *)calloc(sizeof(ex_node_t), 1);
    if (node == NULL)
      return MUNIT_ERROR;
    sl_init_node(&node->snode);
    node->key = key;
    node->value = key;
    if ((ret = sl_insert_nodup(slist, &node->snode)) == -1)
      continue; /* a random duplicate appeared */
    else {
      assert_int(ret, ==, 0);
      inserted++;
    }
  }
  assert_size(inserted, ==, sl_get_size(slist));
  return MUNIT_OK;
}

static void *
test_api_remove_setup(const MunitParameter params[], void *user_data)
{
  sl_raw *slist = (sl_raw *)test_api_setup(params, user_data);
  __populate_slist(slist);
  return (void *)slist;
}
static void
test_api_remove_tear_down(void *fixture)
{
  test_api_tear_down(fixture);
}
static MunitResult
test_api_remove(const MunitParameter params[], void *data)
{
  uint32_t key;
  sl_raw *slist = (sl_raw *)data;
  ex_node_t *node;
  (void)params;

  assert_ptr_not_null(slist);
  key = munit_rand_int_range((((uint32_t)0 - 1) / 10) + 1, ((uint32_t)0 - 1));
  node = (ex_node_t *)calloc(sizeof(ex_node_t), 1);
  if (node == NULL)
    return MUNIT_ERROR;
  sl_init_node(&node->snode);
  node->key = key;
  node->value = key;
  if (sl_insert_nodup(slist, &node->snode) == -1)
    return MUNIT_ERROR;
  else {
    ex_node_t query;
    query.key = key;
    sl_node *cursor = sl_find(slist, &query.snode);
    assert_ptr_not_null(cursor);
    ex_node_t *entry = sl_get_entry(cursor, ex_node_t, snode);
    sl_erase_node(slist, &entry->snode);
    sl_release_node(&entry->snode);
    sl_wait_for_free(&entry->snode);
    sl_free_node(&entry->snode);
    free(entry);
  }
  return MUNIT_OK;
}

static void *
test_api_find_setup(const MunitParameter params[], void *user_data)
{
  sl_raw *slist = (sl_raw *)test_api_setup(params, user_data);
  ex_node_t *node;
  for (int i = 1; i <= 100; ++i) {
    node = calloc(sizeof(ex_node_t), 1);
    if (node == NULL)
      return NULL;
    node = (ex_node_t *)calloc(sizeof(ex_node_t), 1);
    sl_init_node(&node->snode);
    node->key = i;
    node->value = i;
    sl_insert(slist, &node->snode);
  }
  return (void *)slist;
}
static void
test_api_find_tear_down(void *fixture)
{
  test_api_tear_down(fixture);
}
static MunitResult
test_api_find(const MunitParameter params[], void *data)
{
  sl_raw *slist = (sl_raw *)data;
  (void)params;

  /* find equal every value */
  assert_ptr_not_null(data);
  for (int i = 1; i <= 100; i++) {
    ex_node_t query;
    query.key = i;
    sl_node *cursor = sl_find(slist, &query.snode);
    assert_ptr_not_null(cursor);
    ex_node_t *entry = sl_get_entry(cursor, ex_node_t, snode);
    assert_uint32(entry->key, ==, i);
  }

  /*  */
  return MUNIT_OK;
}

static void *
test_api_update_setup(const MunitParameter params[], void *user_data)
{
  return test_api_setup(params, user_data);
}
static void
test_api_update_tear_down(void *fixture)
{
  test_api_tear_down(fixture);
}
static MunitResult
test_api_update(const MunitParameter params[], void *data)
{
  sl_raw *slist = (sl_raw *)data;
  (void)params;
  (void)slist;
  return MUNIT_OK;
}

static void *
test_api_delete_setup(const MunitParameter params[], void *user_data)
{
  return test_api_setup(params, user_data);
}
static void
test_api_delete_tear_down(void *fixture)
{
  test_api_tear_down(fixture);
}
static MunitResult
test_api_delete(const MunitParameter params[], void *data)
{
  sl_raw *slist = (sl_raw *)data;
  (void)params;
  (void)slist;
  return MUNIT_OK;
}

static void *
test_api_duplicates_setup(const MunitParameter params[], void *user_data)
{
  return test_api_setup(params, user_data);
}
static void
test_api_duplicates_tear_down(void *fixture)
{
  test_api_tear_down(fixture);
}
static MunitResult
test_api_duplicates(const MunitParameter params[], void *data)
{
  sl_raw *slist = (sl_raw *)data;
  (void)params;
  (void)slist;
  return MUNIT_OK;
}

static void *
test_api_size_setup(const MunitParameter params[], void *user_data)
{
  return test_api_setup(params, user_data);
}
static void
test_api_size_tear_down(void *fixture)
{
  test_api_tear_down(fixture);
}
static MunitResult
test_api_size(const MunitParameter params[], void *data)
{
  sl_raw *slist = (sl_raw *)data;
  (void)params;
  (void)slist;
  return MUNIT_OK;
}

static void *
test_api_iterators_setup(const MunitParameter params[], void *user_data)
{
  return test_api_setup(params, user_data);
}
static void
test_api_iterators_tear_down(void *fixture)
{
  test_api_tear_down(fixture);
}
static MunitResult
test_api_iterators(const MunitParameter params[], void *data)
{
  sl_raw *slist = (sl_raw *)data;
  (void)params;
  (void)slist;
  return MUNIT_OK;
}

static MunitTest api_test_suite[] = {
  { (char *)"/api/insert", test_api_insert, test_api_insert_setup,
    test_api_insert_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/remove", test_api_remove, test_api_remove_setup,
    test_api_remove_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/find", test_api_find, test_api_find_setup,
    test_api_find_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/update", test_api_update, test_api_update_setup,
    test_api_update_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/delete", test_api_delete, test_api_delete_setup,
    test_api_delete_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/duplicates", test_api_duplicates, test_api_duplicates_setup,
    test_api_duplicates_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/size", test_api_size, test_api_size_setup,
    test_api_size_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char *)"/api/iterators", test_api_iterators, test_api_iterators_setup,
    test_api_iterators_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static MunitTest mt_tests[] = { { NULL, NULL, NULL, NULL,
  MUNIT_TEST_OPTION_NONE, NULL } };

static MunitTest scale_tests[] = { { NULL, NULL, NULL, NULL,
  MUNIT_TEST_OPTION_NONE, NULL } };

static MunitSuite other_test_suite[] = { { "/mt", mt_tests, NULL, 1,
                                           MUNIT_SUITE_OPTION_NONE },
  { "/scale", scale_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },
  { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE } };

static const MunitSuite main_test_suite = { (char *)"/api", api_test_suite,
  other_test_suite, 1, MUNIT_SUITE_OPTION_NONE };

int
main(int argc, char *argv[MUNIT_ARRAY_PARAM(argc + 1)])
{
  struct user_data info;
  return munit_suite_main(&main_test_suite, (void *)&info, argc, argv);
}

/* ARGS: --no-fork --seed 8675309 */
