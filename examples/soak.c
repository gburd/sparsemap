#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/sparsemap.h"
#include "../tests/common.h"
#include "../tests/tdigest.h"

/* midl.h ------------------------------------------------------------------ */
/** @defgroup idls	ID List Management
 *	@{
 */
/** A generic unsigned ID number. These were entryIDs in back-bdb.
 *	Preferably it should have the same size as a pointer.
 */
typedef size_t MDB_ID;

/** An IDL is an ID List, a sorted array of IDs. The first
 * element of the array is a counter for how many actual
 * IDs are in the list. In the original back-bdb code, IDLs are
 * sorted in ascending order. For libmdb IDLs are sorted in
 * descending order.
 */
typedef MDB_ID *MDB_IDL;

/* IDL sizes - likely should be even bigger
 *   limiting factors: sizeof(ID), thread stack size
 */
#define MDB_IDL_LOGN 16 /* DB_SIZE is 2^16, UM_SIZE is 2^17 */
#define MDB_IDL_DB_SIZE (1 << MDB_IDL_LOGN)
#define MDB_IDL_UM_SIZE (1 << (MDB_IDL_LOGN + 1))

#define MDB_IDL_DB_MAX (MDB_IDL_DB_SIZE - 1)
#define MDB_IDL_UM_MAX (MDB_IDL_UM_SIZE - 1)

#define MDB_IDL_SIZEOF(ids) (((ids)[0] + 1) * sizeof(MDB_ID))
#define MDB_IDL_IS_ZERO(ids) ((ids)[0] == 0)
#define MDB_IDL_CPY(dst, src) (memcpy(dst, src, MDB_IDL_SIZEOF(src)))
#define MDB_IDL_FIRST(ids) ((ids)[1])
#define MDB_IDL_LAST(ids) ((ids)[(ids)[0]])

/** Current max length of an #mdb_midl_alloc()ed IDL */
#define MDB_IDL_ALLOCLEN(ids) ((ids)[-1])

/** Append ID to IDL. The IDL must be big enough. */
#define mdb_midl_xappend(idl, id)             \
  do {                                        \
    MDB_ID *xidl = (idl), xlen = ++(xidl[0]); \
    xidl[xlen] = (id);                        \
  } while (0)

/** Search for an ID in an IDL.
 * @param[in] ids	The IDL to search.
 * @param[in] id	The ID to search for.
 * @return	The index of the first ID greater than or equal to \b id.
 */
unsigned mdb_midl_search(MDB_IDL ids, MDB_ID id);

/** Allocate an IDL.
 * Allocates memory for an IDL of the given size.
 * @return	IDL on success, NULL on failure.
 */
MDB_IDL mdb_midl_alloc(int num);

/** Free an IDL.
 * @param[in] ids	The IDL to free.
 */
void mdb_midl_free(MDB_IDL ids);

/** Shrink an IDL.
 * Return the IDL to the default size if it has grown larger.
 * @param[in,out] idp	Address of the IDL to shrink.
 */
void mdb_midl_shrink(MDB_IDL *idp);

/** Shrink an IDL to a specific size.
 * Resize the IDL to \b size if it is larger.
 * @param[in,out] idp	Address of the IDL to shrink.
 * @param[in] size	Capacity to have once resized.
 */
void mdb_midl_shrink(MDB_IDL *idp);

/** Make room for num additional elements in an IDL.
 * @param[in,out] idp	Address of the IDL.
 * @param[in] num	Number of elements to make room for.
 * @return	0 on success, ENOMEM on failure.
 */
int mdb_midl_need(MDB_IDL *idp, unsigned num);

/** Append an ID onto an IDL.
 * @param[in,out] idp	Address of the IDL to append to.
 * @param[in] id	The ID to append.
 * @return	0 on success, ENOMEM if the IDL is too large.
 */
int mdb_midl_append(MDB_IDL *idp, MDB_ID id);

/** Append an IDL onto an IDL.
 * @param[in,out] idp	Address of the IDL to append to.
 * @param[in] app	The IDL to append.
 * @return	0 on success, ENOMEM if the IDL is too large.
 */
int mdb_midl_append_list(MDB_IDL *idp, MDB_IDL app);

/** Append an ID range onto an IDL.
 * @param[in,out] idp	Address of the IDL to append to.
 * @param[in] id	The lowest ID to append.
 * @param[in] n		Number of IDs to append.
 * @return	0 on success, ENOMEM if the IDL is too large.
 */
int mdb_midl_append_range(MDB_IDL *idp, MDB_ID id, unsigned n);

/** Merge an IDL onto an IDL. The destination IDL must be big enough.
 * @param[in] idl	The IDL to merge into.
 * @param[in] merge	The IDL to merge.
 */
void mdb_midl_xmerge(MDB_IDL idl, MDB_IDL merge);

/** Sort an IDL.
 * @param[in,out] ids	The IDL to sort.
 */
void mdb_midl_sort(MDB_IDL ids);

/* midl.c ------------------------------------------------------------------ */
/** @defgroup idls	ID List Management
 *	@{
 */
#define CMP(x, y) ((x) < (y) ? -1 : (x) > (y))

unsigned
mdb_midl_search(MDB_IDL ids, MDB_ID id)
{
  /*
   * binary search of id in ids
   * if found, returns position of id
   * if not found, returns first position greater than id
   */
  unsigned base = 0;
  unsigned cursor = 1;
  int val = 0;
  unsigned n = ids[0];

  while (0 < n) {
    unsigned pivot = n >> 1;
    cursor = base + pivot + 1;
    val = CMP(ids[cursor], id);

    if (val < 0) {
      n = pivot;

    } else if (val > 0) {
      base = cursor;
      n -= pivot + 1;

    } else {
      return cursor;
    }
  }

  if (val > 0) {
    ++cursor;
  }
  return cursor;
}

int
mdb_midl_insert(MDB_IDL ids, MDB_ID id)
{
  unsigned x, i;

  x = mdb_midl_search(ids, id);
  assert(x > 0);

  if (x < 1) {
    /* internal error */
    return -2;
  }

  if (x <= ids[0] && ids[x] == id) {
    /* duplicate */
    assert(0);
    return -1;
  }

  if (++ids[0] >= MDB_IDL_DB_MAX) {
    /* no room */
    --ids[0];
    return -2;

  } else {
    /* insert id */
    for (i = ids[0]; i > x; i--)
      ids[i] = ids[i - 1];
    ids[x] = id;
  }

  return 0;
}

inline void
mdb_midl_popn(MDB_IDL ids, unsigned n)
{
  ids[0] = ids[0] - n;
}

void
mdb_midl_remove_at(MDB_IDL ids, unsigned idx)
{
  for (int i = idx - 1; idx < ids[0] - 1;)
    ids[++i] = ids[++idx];
  ids[0] = ids[0] - 1;
}

void
mdb_midl_remove(MDB_IDL ids, MDB_ID id)
{
  unsigned idx = mdb_midl_search(ids, id);
  if (idx <= ids[0] && ids[idx] == id)
    mdb_midl_remove_at(ids, idx);
}

MDB_IDL
mdb_midl_alloc(int num)
{
  MDB_IDL ids = malloc((num + 2) * sizeof(MDB_ID));
  if (ids) {
    *ids++ = num;
    *ids = 0;
  }
  return ids;
}

void
mdb_midl_free(MDB_IDL ids)
{
  if (ids)
    free(ids - 1);
}

void
mdb_midl_shrink(MDB_IDL *idp)
{
  MDB_IDL ids = *idp;
  if (*(--ids) > MDB_IDL_UM_MAX && (ids = realloc(ids, (MDB_IDL_UM_MAX + 2) * sizeof(MDB_ID)))) {
    *ids++ = MDB_IDL_UM_MAX;
    *idp = ids;
  }
}

void
mdb_midl_shrink_to(MDB_IDL *idp, size_t size)
{
  MDB_IDL ids = *idp;
  if (*(--ids) > size && (ids = realloc(ids, (size + 2) * sizeof(MDB_ID)))) {
    *ids++ = size;
    *idp = ids;
    *idp[0] = *idp[0] > size ? size : *idp[0];
  }
}

static int
mdb_midl_grow(MDB_IDL *idp, int num)
{
  MDB_IDL idn = *idp - 1;
  /* grow it */
  idn = realloc(idn, (*idn + num + 2) * sizeof(MDB_ID));
  if (!idn)
    return ENOMEM;
  *idn++ += num;
  *idp = idn;
  return 0;
}

int
mdb_midl_need(MDB_IDL *idp, unsigned num)
{
  MDB_IDL ids = *idp;
  num += ids[0];
  if (num > ids[-1]) {
    num = (num + num / 4 + (256 + 2)) & -256;
    if (!(ids = realloc(ids - 1, num * sizeof(MDB_ID))))
      return ENOMEM;
    *ids++ = num - 2;
    *idp = ids;
  }
  return 0;
}

int
mdb_midl_append(MDB_IDL *idp, MDB_ID id)
{
  MDB_IDL ids = *idp;
  /* Too big? */
  if (ids[0] >= ids[-1]) {
    if (mdb_midl_grow(idp, MDB_IDL_UM_MAX))
      return ENOMEM;
    ids = *idp;
  }
  ids[0]++;
  ids[ids[0]] = id;
  return 0;
}

int
mdb_midl_append_list(MDB_IDL *idp, MDB_IDL app)
{
  MDB_IDL ids = *idp;
  /* Too big? */
  if (ids[0] + app[0] >= ids[-1]) {
    if (mdb_midl_grow(idp, app[0]))
      return ENOMEM;
    ids = *idp;
  }
  memcpy(&ids[ids[0] + 1], &app[1], app[0] * sizeof(MDB_ID));
  ids[0] += app[0];
  return 0;
}

int
mdb_midl_append_range(MDB_IDL *idp, MDB_ID id, unsigned n)
{
  MDB_ID *ids = *idp, len = ids[0];
  /* Too big? */
  if (len + n > ids[-1]) {
    if (mdb_midl_grow(idp, n | MDB_IDL_UM_MAX))
      return ENOMEM;
    ids = *idp;
  }
  ids[0] = len + n;
  ids += len;
  while (n)
    ids[n--] = id++;
  return 0;
}

void
mdb_midl_xmerge(MDB_IDL idl, MDB_IDL merge)
{
  MDB_ID old_id, merge_id, i = merge[0], j = idl[0], k = i + j, total = k;
  idl[0] = (MDB_ID)-1; /* delimiter for idl scan below */
  old_id = idl[j];
  while (i) {
    merge_id = merge[i--];
    for (; old_id < merge_id; old_id = idl[--j])
      idl[k--] = old_id;
    idl[k--] = merge_id;
  }
  idl[0] = total;
}

/* Quicksort + Insertion sort for small arrays */

#define SMALL 8
#define MIDL_SWAP(a, b) \
  {                     \
    itmp = (a);         \
    (a) = (b);          \
    (b) = itmp;         \
  }

void
mdb_midl_sort(MDB_IDL ids)
{
  /* Max possible depth of int-indexed tree * 2 items/level */
  int istack[sizeof(int) * CHAR_BIT * 2];
  int i, j, k, l, ir, jstack;
  MDB_ID a, itmp;

  ir = (int)ids[0];
  l = 1;
  jstack = 0;
  for (;;) {
    if (ir - l < SMALL) { /* Insertion sort */
      for (j = l + 1; j <= ir; j++) {
        a = ids[j];
        for (i = j - 1; i >= 1; i--) {
          if (ids[i] >= a)
            break;
          ids[i + 1] = ids[i];
        }
        ids[i + 1] = a;
      }
      if (jstack == 0)
        break;
      ir = istack[jstack--];
      l = istack[jstack--];
    } else {
      k = (l + ir) >> 1; /* Choose median of left, center, right */
      MIDL_SWAP(ids[k], ids[l + 1]);
      if (ids[l] < ids[ir]) {
        MIDL_SWAP(ids[l], ids[ir]);
      }
      if (ids[l + 1] < ids[ir]) {
        MIDL_SWAP(ids[l + 1], ids[ir]);
      }
      if (ids[l] < ids[l + 1]) {
        MIDL_SWAP(ids[l], ids[l + 1]);
      }
      i = l + 1;
      j = ir;
      a = ids[l + 1];
      for (;;) {
        do
          i++;
        while (ids[i] > a);
        do
          j--;
        while (ids[j] < a);
        if (j < i)
          break;
        MIDL_SWAP(ids[i], ids[j]);
      }
      ids[l + 1] = ids[j];
      ids[j] = a;
      jstack += 2;
      if (ir - i + 1 >= j - l) {
        istack[jstack] = ir;
        istack[jstack - 1] = i;
        ir = j - 1;
      } else {
        istack[jstack] = j - 1;
        istack[jstack - 1] = l;
        l = i;
      }
    }
  }
}
/* ------------------------------------------------------------------------- */

typedef MDB_ID pgno_t;

char *
bytes_as(double bytes, char *s, size_t size)
{
  const char *units[] = { "b", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB" };
  size_t i = 0;

  while (bytes >= 1024 && i < sizeof(units) / sizeof(units[0]) - 1) {
    bytes /= 1024;
    i++;
  }

  snprintf(s, size, "%.2f %s", bytes, units[i]);
  return s;
}

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

bool
verify_midl_contains(MDB_IDL list, pgno_t pg)
{
  unsigned idx = mdb_midl_search(list, pg);
  return idx <= list[0] && list[idx] == pg;
}

bool
verify_midl_nodups(MDB_IDL list)
{
  pgno_t id = 1;
  while (id < list[0]) {
    if (list[id] == list[id + 1])
      return false;
    id++;
  }
  return true;
}

bool
verify_span_midl(MDB_IDL list, pgno_t pg, unsigned len)
{
  pgno_t idx = mdb_midl_search(list, pg);
  bool found = idx <= list[0] && list[idx] == pg;
  if (!found)
    return false;
  if (len == 1)
    return true;
  if (list[len] + 1 != list[len - 1])
    return false;
  return true;
}

bool
verify_empty_midl(MDB_IDL list, pgno_t pg, unsigned len)
{
  for (pgno_t i = pg; i < pg + len; i++) {
    pgno_t idx = mdb_midl_search(list, pg);
    bool found = idx <= list[0] && list[idx] == pg;
    if (found)
      return false;
  }
  return true;
}

bool
verify_span_sparsemap(sparsemap_t *map, pgno_t pg, unsigned len)
{
  for (pgno_t i = pg; i < pg + len; i++) {
    if (sparsemap_is_set(map, i) != true) {
      return false;
    }
  }
  return true;
}

bool
verify_empty_sparsemap(sparsemap_t *map, pgno_t pg, unsigned len)
{
  for (pgno_t i = 0; i < len; i++) {
    if (sparsemap_is_set(map, pg + i) != false) {
      return false;
    }
  }
  return true;
}

bool
verify_sm_eq_ml(sparsemap_t *map, MDB_IDL list)
{
  for (MDB_ID i = 1; i <= list[0]; i++) {
    pgno_t pg = list[i];
    unsigned skipped = i == 1 ? 0 : list[i - 1] - list[i] - 1;
    if (skipped) {
      for (MDB_ID j = list[i - 1]; j > list[i]; j--) {
        if (sparsemap_is_set(map, pg - j) != false) {
          __diag("%zu\n", pg - j);
          return false;
        }
      }
    }
    if (sparsemap_is_set(map, pg) != true) {
      __diag("%zu\n", pg);
      return false;
    }
  }
  return true;
}

sparsemap_idx_t
_sparsemap_set(sparsemap_t **map, sparsemap_idx_t idx, bool value)
{
  sparsemap_idx_t l = sparsemap_set(*map, idx, value);
  if (errno == ENOSPC) {
    *map = sparsemap_set_data_size(*map, sparsemap_get_capacity(*map) + 64, NULL);
    assert(*map != NULL);
    errno = 0;
  }
  return l;
}

td_histogram_t *l_span_loc;
td_histogram_t *b_span_loc;
td_histogram_t *l_span_take;
td_histogram_t *b_span_take;

void
stats_header()
{
  printf(
    "iterations,idl_cap,idl_used,idl_bytes,sm_cap,sm_used,idl_loc_p50,idl_loc_p75,idl_loc_p90,idl_loc_p99,idl_loc_p999,sm_loc_p50,sm_loc_p75,sm_loc_p90,sm_loc_p99,sm_loc_p999,idl_take_p50,idl_take_p75,idl_take_p90,idl_take_p99,idl_take_p999,sm_take_p50,sm_take_p75,sm_take_p90,sm_take_p99,sm_take_p999\n");
}

void
stats(size_t iterations, sparsemap_t *map, MDB_IDL list)
{
  if (iterations < 10)
    return;

  td_compress(l_span_loc);
  td_compress(b_span_loc);
  td_compress(l_span_take);
  td_compress(b_span_take);

  printf("%f,%zu,%zu,%zu,%zu,%zu,%zu,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f\n",
    nsts(), iterations, list[-1], list[0], MDB_IDL_SIZEOF(list), sparsemap_get_capacity(map), sparsemap_get_size(map), td_quantile(l_span_loc, .5),
    td_quantile(l_span_loc, .75), td_quantile(l_span_loc, .90), td_quantile(l_span_loc, .99), td_quantile(l_span_loc, .999), td_quantile(b_span_loc, .5),
    td_quantile(b_span_loc, .75), td_quantile(b_span_loc, .90), td_quantile(b_span_loc, .99), td_quantile(b_span_loc, .999), td_quantile(l_span_take, .5),
    td_quantile(l_span_take, .75), td_quantile(l_span_take, .90), td_quantile(l_span_take, .99), td_quantile(l_span_take, .999), td_quantile(b_span_take, .5),
    td_quantile(b_span_take, .75), td_quantile(b_span_take, .90), td_quantile(b_span_take, .99), td_quantile(b_span_take, .999));

#if 0
  static double pct[] = { .5, .75, .90, .99, .999 };
  bytes_as(MDB_IDL_SIZEOF(list), m, 1024);
  bytes_as(sparsemap_get_capacity(map), l, 1024);

  for (int i = 0; i < 5; i++)
    printf("%.10f,%.10f,%.10f,%.10f,%.10f", iterations, pct[i] * 100, td_quantile(l_span_loc, pct[i]), td_quantile(b_span_loc, pct[i]));
  for (int i = 0; i < 5; i++)
    __diag("%lu\tspan_take:\t%f l: %.10f\tb: %.10f\n", iterations, pct[i] * 100, td_quantile(l_span_take, pct[i]), td_quantile(b_span_take, pct[i]));
#endif
}

#define INITIAL_AMOUNT 1024 * 2

/*
 * A "soak test" that tries to replicate behavior in LMDB for page allocation.
 */
int
main()
{
  size_t replenish = 0, iterations = 0;
  bool prefer_mdb_idl_location = (bool)xorshift32() % 2;

  // disable buffering
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  l_span_loc = td_new(100);
  b_span_loc = td_new(100);
  l_span_take = td_new(100);
  b_span_take = td_new(100);

  stats_header();

  sparsemap_idx_t amt = INITIAL_AMOUNT;
  MDB_IDL list = mdb_midl_alloc(amt);
  sparsemap_t *map = sparsemap(INITIAL_AMOUNT);

  // start with 2GiB of 4KiB free pages to track:
  //  - MDB_IDL requires one int for each free page
  //  - Sparsemap will compress the set bits using less memory
  mdb_midl_need(&list, amt);
  for (sparsemap_idx_t pg = 0; pg < amt; pg++) {
    // We list every free (unallocated) page in the IDL, while...
    mdb_midl_xappend(list, pg);
    // ... true (unset in the bitmap) indicates free in the bitmap.
    assert(_sparsemap_set(&map, pg, true) == pg);
  }
  mdb_midl_sort(list);
  stats(0, map, list);
  assert(verify_sm_eq_ml(map, list));

  double b, e;
  while (1) {
    unsigned mi;
    pgno_t ml, sl;

    // get an amount [1, 16] of pages to find preferring smaller sizes
    unsigned n = toss(15) + 1;

    // find a set of pages using the MDB_IDL
    {
      b = nsts();
      /* Seek a big enough contiguous page range. Prefer
       * pages at the tail, just truncating the list.
       */
      int retry = 1;
      unsigned i = 0;
      pgno_t pgno = 0, *mop = list;
      unsigned n2 = n, mop_len = mop[0];
      if (mop_len > n2) {
        i = mop_len;
        do {
          pgno = mop[i];
          if (mop[i - n2] == pgno + n2)
            goto search_done;
        } while (--i > n2);
        if (--retry < 0)
          break;
      }
    search_done:;
      ml = pgno;
      mi = i;
      e = nsts();
      td_add(l_span_loc, e - b, 1);
    }
    assert(verify_span_midl(list, ml, n));
    assert(verify_span_sparsemap(map, ml, n));

    // find a set of pages using the Sparsemap
    {
      b = nsts();
      pgno_t pgno = sparsemap_span(map, 0, n, true);
      assert(SPARSEMAP_NOT_FOUND(pgno) == false);
      sl = pgno;
      e = nsts();
      td_add(b_span_loc, e - b, 1);
    }
    assert(verify_span_midl(list, sl, n));
    assert(verify_span_sparsemap(map, sl, n));

    // acquire the set of pages within the list
    if (prefer_mdb_idl_location) {
      b = nsts();
      unsigned j, num = n;
      int i = mi;
      pgno_t *mop = list;
      unsigned mop_len = mop[0];

      mop[0] = mop_len -= num;
      /* Move any stragglers down */
      for (j = i - num; j < mop_len;)
        mop[++j] = mop[++i];
      e = nsts();
      for (j = mop_len + 1; j <= mop[-1]; j++)
        mop[j] = 0;
      td_add(l_span_take, e - b, 1);
    } else {
      b = nsts();
      unsigned j, num = n;
      int i = mdb_midl_search(list, sl) + num;
      pgno_t *mop = list;
      unsigned mop_len = mop[0];

      mop[0] = mop_len -= num;
      /* Move any stragglers down */
      for (j = i - num; j < mop_len;)
        mop[++j] = mop[++i];
      e = nsts();
      for (j = mop_len + 1; j <= mop[-1]; j++)
        mop[j] = 0;
      td_add(l_span_take, e - b, 1);
    }

    // acquire the set of pages within the sparsemap
    if (prefer_mdb_idl_location) {
      b = nsts();
      for (pgno_t i = ml; i < ml + n; i++) {
        assert(_sparsemap_set(&map, i, false) == i);
      }
      e = nsts();
      td_add(b_span_take, e - b, 1);
    } else {
      b = nsts();
      for (pgno_t i = sl; i <= sl + n; i++) {
        assert(_sparsemap_set(&map, i, false) == i);
      }
      e = nsts();
      td_add(b_span_take, e - b, 1);
    }

    assert(verify_sm_eq_ml(map, list));

    // Once we've used half of the free list, let's replenish it a bit.
    if (list[0] < amt / 2) {
      do {
        pgno_t pg;
        size_t len, retries = amt;
        do {
          len = toss(15) + 1;
          pg = sparsemap_span(map, 0, len, false);
          //__diag("%zu\t%zu,%zu\n", iterations, replenish, retries);
        } while (SPARSEMAP_NOT_FOUND(pg) && --retries);
        if (retries == 0) {
          goto larger_please;
        }
        if (SPARSEMAP_FOUND(pg)) {
          assert(verify_empty_midl(list, pg, len));
          assert(verify_empty_sparsemap(map, pg, len));
          assert(verify_sm_eq_ml(map, list));
          if (list[-1] - list[0] < len) {
            mdb_midl_need(&list, list[-1] + len);
          }
          for (size_t i = pg; i < pg + len; i++) {
            assert(verify_midl_contains(list, i) == false);
            assert(sparsemap_is_set(map, i) == false);
            mdb_midl_insert(list, i);
            assert(verify_midl_contains(list, i) == true);
            assert(_sparsemap_set(&map, i, true) == i);
            assert(sparsemap_is_set(map, i) == true);
          }
          mdb_midl_sort(list);
          assert(verify_midl_nodups(list));
          assert(verify_span_midl(list, pg, len));
          assert(verify_span_sparsemap(map, pg, len));
        }
        assert(verify_sm_eq_ml(map, list));
        replenish++;
      } while (list[0] < amt - 32);
    }
    replenish = 0;

    // every so often, either ...
    if (iterations % 1000 == 0) {
    larger_please:;
      const int COUNT = 1024;
      if (toss(6) + 1 < 7) {
        // ... add COUNT 4KiB pages, or
        int len = COUNT;
        // The largest page is at list[1] because this is a reverse sorted list.
        pgno_t pg = list[1] + 1;
        if (list[0] + COUNT > list[-1]) {
          mdb_midl_grow(&list, list[0] + len);
        }
        for (size_t i = pg; i < pg + len; i++) {
          assert(verify_midl_contains(list, i) == false);
          assert(sparsemap_is_set(map, i) == false);
          mdb_midl_insert(list, i);
          assert(_sparsemap_set(&map, i, true) == i);
        }
        mdb_midl_sort(list);
        assert(verify_midl_nodups(list));
        verify_sm_eq_ml(map, list);
        amt += COUNT;
      } else {
        if (list[-1] > INITIAL_AMOUNT) {
          // ... a fraction of the time, remove COUNT / 2 of 4KiB pages.
          pgno_t pg;
          for (int i = 0; i < COUNT; i++) {
            pg = list[list[0] - i];
            assert(sparsemap_is_set(map, pg) == true);
            assert(_sparsemap_set(&map, pg, false) == pg);
          }
          mdb_midl_shrink_to(&list, list[0] - COUNT);
          assert(list[list[0]] != pg);
          assert(verify_midl_nodups(list));
          verify_sm_eq_ml(map, list);
        }
      }
    }
    iterations++;
    stats(iterations, map, list);
  }

  return 0;
}
