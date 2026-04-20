/*
 * postgres.h - PostgreSQL compatibility header for Bitmapset testing
 *
 * This header provides the essential PostgreSQL types, macros, and functions
 * needed to compile the Bitmapset implementation and map it to sparsemap APIs.
 */

#ifndef POSTGRES_H
#define POSTGRES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>

typedef unsigned char uint8;
typedef unsigned int uint32;
typedef unsigned long long uint64;
typedef int32_t int32;
typedef uint16_t uint16;
typedef int16_t int16;
typedef int8_t int8;
typedef size_t Size;
typedef uintptr_t Datum;

/* Assert and alignment macros */
#define Assert assert
#define MAXIMUM_ALIGNOF 8
#define NIL NULL

/* Population count functions using GCC built-ins */
#define pg_popcount32 __builtin_popcount
#define pg_popcount64 __builtin_popcountll

/* Node tag system */
typedef enum NodeTag
{
    T_Invalid = 0,
    T_Bitmapset = 1000
} NodeTag;

/* MAXALIGN macro for aligning memory to MAXIMUM_ALIGNOF boundaries */
#define MAXALIGN(LEN) (((uintptr_t)(LEN) + MAXIMUM_ALIGNOF - 1) & ~((uintptr_t)(MAXIMUM_ALIGNOF - 1)))

/* Min/Max macros - PostgreSQL style */
#ifndef Min
#define Min(x, y)       ((x) < (y) ? (x) : (y))
#endif

#ifndef Max
#define Max(x, y)       ((x) > (y) ? (x) : (y))
#endif

/* Datum conversion macros */
#define DatumGetUInt32(X)       ((uint32) (X))
#define UInt32GetDatum(X)       ((Datum) (X))
#define DatumGetInt32(X)        ((int32) (X))
#define Int32GetDatum(X)        ((Datum) (X))
#define DatumGetPointer(X)      ((void *) (X))
#define PointerGetDatum(X)      ((Datum) (X))

/* Simple hash function for uint32 values */
uint32
hash_uint32(uint32 k)
{
  /* Simple hash function based on Thomas Wang's 32-bit mix function */
  k = (k ^ 61) ^ (k >> 16);
  k = k + (k << 3);
  k = k ^ (k >> 4);
  k = k * 0x27d4eb2d;
  k = k ^ (k >> 15);
  return k;
}

/* Memory management functions */
void *
palloc(Size size)
{
  void *ptr = malloc(size);
  if (!ptr && size > 0)
    {
      fprintf(stderr, "out of memory\n");
      exit(1);
    }
  return ptr;
}

void *
palloc0(Size size)
{
  void *ptr = palloc(size);
  if (ptr)
    memset(ptr, 0, size);
  return ptr;
}

void *
repalloc(void *pointer, Size size)
{
  void *ptr = realloc(pointer, size);
  if (!ptr && size > 0)
    {
      fprintf(stderr, "out of memory\n");
      exit(1);
    }
  return ptr;
}

void
pfree(void *pointer)
{
  if (pointer)
    free(pointer);
}

/* Error levels - matching PostgreSQL's elog levels */
#define DEBUG5      10
#define DEBUG4      11
#define DEBUG3      12
#define DEBUG2      13
#define DEBUG1      14
#define LOG         15
#define INFO        17
#define NOTICE      18
#define WARNING     19
#define ERROR       20
#define FATAL       21
#define PANIC       22

/* Global log level threshold */
static int min_log_level = WARNING;

/* Helper function to get level name string */
const char *
get_level_name(int level)
{
  switch (level)
    {
    case DEBUG5:    return "DEBUG5";
    case DEBUG4:    return "DEBUG4";
    case DEBUG3:    return "DEBUG3";
    case DEBUG2:    return "DEBUG2";
    case DEBUG1:    return "DEBUG1";
    case LOG:       return "LOG";
    case INFO:      return "INFO";
    case NOTICE:    return "NOTICE";
    case WARNING:   return "WARNING";
    case ERROR:     return "ERROR";
    case FATAL:     return "FATAL";
    case PANIC:     return "PANIC";
    default:        return "UNKNOWN";
    }
}

/* Main elog implementation using printf */
#define elog(level, ...) do {				\
    if ((level) >= min_log_level) {			\
      fprintf(stderr, "[%s] ", get_level_name(level));	\
      fprintf(stderr, __VA_ARGS__);			\
      fprintf(stderr, "\n");				\
      fflush(stderr);					\
      if ((level) >= FATAL) {				\
	exit(1);					\
      }							\
    }							\
  } while(0)

/* Convenience function to set minimum log level for testing */
void
set_log_level(int level)
{
  min_log_level = level;
}

/* List support for foreach() macro */
typedef struct ListCell ListCell;
typedef struct List List;

struct ListCell
{
  union
  {
    void       *ptr_value;
    int         int_value;
    uint32      uint_value;
  } data;
  ListCell   *next;
};

struct List
{
  NodeTag     type;
  int         length;
  ListCell   *head;
  ListCell   *tail;
};

/* foreach() macro - simplified version for testing */
#define foreach(cell, list)				\
  for ((cell) = ((list) != NULL) ? (list)->head : NULL; \
       (cell) != NULL;					\
       (cell) = (cell)->next)

/* List access macros */
#define lfirst(cell)        ((cell)->data.ptr_value)
#define lfirst_int(cell)    ((cell)->data.int_value)
#define lfirst_uint(cell)   ((cell)->data.uint_value)


#endif /* POSTGRES_H */
