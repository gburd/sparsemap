/** @defgroup idls	ID List Management
 *	@{
 */

/* Required standard library headers */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
mdb_midl_pop_n(MDB_IDL ids, unsigned n)
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
