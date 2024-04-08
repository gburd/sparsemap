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
  sl_raw *slist = (sl_raw *)data;
  (void)params;
  assert_ptr_not_null(data);
  int n = munit_rand_int_range(128, 4096);
  int key = munit_rand_int_range(0, (((uint32_t)0) - 1) / 10);
  while (n--) {
    ex_node_t *node = (ex_node_t *)calloc(sizeof(ex_node_t), 1);
    sl_init_node(&node->snode);
    node->key = key;
    node->value = key;
    sl_insert(slist, &node->snode);
  }
  return MUNIT_OK;
}

static void *
test_api_remove_setup(const MunitParameter params[], void *user_data)
{
  return test_api_setup(params, user_data);
}
static void
test_api_remove_tear_down(void *fixture)
{
  test_api_tear_down(fixture);
}
static MunitResult
test_api_remove(const MunitParameter params[], void *data)
{
  sl_raw *slist = (sl_raw *)data;
  (void)params;
  (void)slist;
  return MUNIT_OK;
}

static void *
test_api_find_setup(const MunitParameter params[], void *user_data)
{
  return test_api_setup(params, user_data);
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
  (void)slist;
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
