/*-------------------------------------------------------------------------
 *
 * postgres_compat.h
 *	  PostgreSQL compatibility shim for building outside PostgreSQL.
 *
 * This header provides minimal replacements for PostgreSQL infrastructure
 * (palloc, elog, Assert, etc.) so that bitmapset.h and bitmapset.c can
 * compile and run as a standalone library using only the C standard library.
 *
 * Include this header instead of postgres.h when BUILDING_OUTSIDE_POSTGRES
 * is defined.
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGRES_COMPAT_H
#define POSTGRES_COMPAT_H

/* Signal that we are not inside a PostgreSQL build */
#ifndef BUILDING_OUTSIDE_POSTGRES
#define BUILDING_OUTSIDE_POSTGRES
#endif

/*
 * Standard C includes that postgres.h would normally pull in.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

/*
 * PostgreSQL's Size type (used by palloc and friends).
 */
typedef size_t Size;

/* ----------------------------------------------------------------
 *	Memory allocation shims
 *
 *	These replace PostgreSQL's palloc/pfree family with plain malloc/free.
 *	All allocators abort on out-of-memory, matching PostgreSQL's behaviour
 *	where elog(ERROR, ...) would longjmp out.
 * ----------------------------------------------------------------
 */
static inline void *
palloc(Size size)
{
	void	   *p = malloc(size);

	if (!p)
	{
		fprintf(stderr, "out of memory\n");
		abort();
	}
	return p;
}

static inline void *
palloc0(Size size)
{
	void	   *p = calloc(1, size);

	if (!p)
	{
		fprintf(stderr, "out of memory\n");
		abort();
	}
	return p;
}

static inline void *
repalloc(void *ptr, Size size)
{
	void	   *p = realloc(ptr, size);

	if (!p)
	{
		fprintf(stderr, "out of memory\n");
		abort();
	}
	return p;
}

static inline void
pfree(void *ptr)
{
	free(ptr);
}

/* ----------------------------------------------------------------
 *	Error reporting
 *
 *	PostgreSQL's elog() longjmps on ERROR; here we just print and abort.
 * ----------------------------------------------------------------
 */
#define ERROR 21

#define elog(level, ...) \
	do { \
		fprintf(stderr, "ERROR: " __VA_ARGS__); \
		fprintf(stderr, "\n"); \
		abort(); \
	} while (0)

/* ----------------------------------------------------------------
 *	Assertions
 * ----------------------------------------------------------------
 */
#define Assert(expr) assert(expr)

/* ----------------------------------------------------------------
 *	Utility macros
 * ----------------------------------------------------------------
 */
#define unlikely(x)		__builtin_expect(!!(x), 0)

#define Min(a, b)		((a) < (b) ? (a) : (b))
#define Max(a, b)		((a) > (b) ? (a) : (b))

/* 8-byte alignment, matching PostgreSQL's MAXALIGN on 64-bit platforms */
#define MAXALIGN(x)		(((x) + 7) & ~7)

#endif							/* POSTGRES_COMPAT_H */
