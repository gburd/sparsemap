/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2014 Christoph Rupp <chris@crupp.de>.
 * Copyright (c) 2024 Gregory Burd <greg@burd.me>.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if !defined(_MSC_VER)
#include <sys/types.h>
#endif

/* Expose the full struct definition from <sm.h> to this translation
 * unit; the library needs the layout, consumers get it only via
 * SM_EXPOSE_STRUCT. */
#define SM_INTERNAL
#include "sm.h"
#if defined(__BMI2__) && defined(__x86_64__)
#include <immintrin.h>
#define SM_HAVE_PDEP64 1
#endif
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Portability shims.
 *
 * sparsemap leans on four compiler builtins in its hot paths.  On gcc
 * and clang they expand to the corresponding __builtin_*; on MSVC to
 * the matching intrinsic; on unknown compilers they fall back to a
 * no-op (prefetch) or a portable scalar implementation (popcount,
 * ctz, clz).  Kept inline here so the library is exactly two files,
 * sm.h and sm.c, with nothing else to vendor.
 *
 *	SM_PREFETCH(addr)   Hot-loop prefetch hint.  Non-binding; safe
 *			    to drop on toolchains without the intrinsic.
 *	SM_POPCOUNT64(x)    Population count of a 64-bit value (see the
 *			    note on x86 below).
 *	SM_CTZ64(x)	    Count trailing zeros (undefined on x == 0;
 *			    the caller must guard).
 *	SM_CLZ64(x)	    Count leading zeros (undefined on x == 0;
 *			    the caller must guard).
 *
 * Each macro may be overridden by the consumer: define it before
 * including this translation unit (e.g. on the compiler command line
 * with -DSM_POPCOUNT64=my_popcount) and sparsemap uses your version
 * verbatim, skipping the built-in detection below.  This lets a host
 * environment route these primitives through its own intrinsics
 * (for example PostgreSQL's pg_popcount64 / pg_rightmost_one_pos64).
 * The override must have the same call signature and return an int
 * (popcount/ctz/clz) or evaluate to void (prefetch).
 */

#ifndef SM_PREFETCH
#if defined(__GNUC__) || defined(__clang__)
#define SM_PREFETCH(addr) __builtin_prefetch((addr), 0, 1)
#elif defined(_MSC_VER)
#include <intrin.h>
#if defined(_M_ARM64) || defined(_M_ARM)
#define SM_PREFETCH(addr) __prefetch((const void *)(addr))
#elif defined(_M_X64) || defined(_M_IX86)
#define SM_PREFETCH(addr) _mm_prefetch((const char *)(addr), _MM_HINT_T0)
#else
#define SM_PREFETCH(addr) ((void)0)
#endif
#else
#define SM_PREFETCH(addr) ((void)0)
#endif
#endif /* SM_PREFETCH */

/*
 * On x86 without POPCNT (the default x86-64 target), gcc turns
 * __builtin_popcountll into a call to libgcc's __popcountdi2.  The
 * inline SWAR form below measured 6-34% faster than that call on
 * contains, rank and select over chunks, so x86 uses it unless the
 * build enables POPCNT (e.g., -mpopcnt or -march=x86-64-v2), where the
 * builtin is one instruction.  Other gcc/clang targets keep the builtin.
 */
#ifndef SM_POPCOUNT64
#if (defined(__GNUC__) || defined(__clang__)) &&                     \
    (defined(__POPCNT__) || !(defined(__x86_64__) || defined(__i386__)))
#define SM_POPCOUNT64(x) ((int)__builtin_popcountll((uint64_t)(x)))
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#include <intrin.h>
#define SM_POPCOUNT64(x) ((int)__popcnt64((unsigned __int64)(x)))
#else
/*
 * SWAR fallback (Sebastiano Vigna, broadword popcount).  Twelve ops,
 * no table, roughly 3-5x slower than a hardware popcnt.
 */
static inline int
sm_swar_popcount64(uint64_t x)
{
	x = x - ((x >> 1) & UINT64_C(0x5555555555555555));
	x = (x & UINT64_C(0x3333333333333333)) +
	    ((x >> 2) & UINT64_C(0x3333333333333333));
	x = (x + (x >> 4)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
	return ((int)((x * UINT64_C(0x0101010101010101)) >> 56));
}
#define SM_POPCOUNT64(x) sm_swar_popcount64((uint64_t)(x))
#endif
#endif /* SM_POPCOUNT64 */

#ifndef SM_CTZ64
#if defined(__GNUC__) || defined(__clang__)
#define SM_CTZ64(x) __builtin_ctzll((uint64_t)(x))
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#include <intrin.h>
static inline int
sm_msvc_ctz64(uint64_t x)
{
	unsigned long idx;
	_BitScanForward64(&idx, (unsigned __int64)x);
	return ((int)idx);
}
#define SM_CTZ64(x) sm_msvc_ctz64((uint64_t)(x))
#else
/* Portable bit-binary-search fallback.  Six branches, no intrinsics. */
static inline int
sm_swar_ctz64(uint64_t x)
{
	int n = 0;
	if (!(x & 0xFFFFFFFFULL)) {
		n += 32;
		x >>= 32;
	}
	if (!(x & 0xFFFFULL)) {
		n += 16;
		x >>= 16;
	}
	if (!(x & 0xFFULL)) {
		n += 8;
		x >>= 8;
	}
	if (!(x & 0xFULL)) {
		n += 4;
		x >>= 4;
	}
	if (!(x & 0x3ULL)) {
		n += 2;
		x >>= 2;
	}
	if (!(x & 0x1ULL)) {
		n += 1;
	}
	return (n);
}
#define SM_CTZ64(x) sm_swar_ctz64((uint64_t)(x))
#endif
#endif /* SM_CTZ64 */

#ifndef SM_CLZ64
#if defined(__GNUC__) || defined(__clang__)
#define SM_CLZ64(x) __builtin_clzll((uint64_t)(x))
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#include <intrin.h>
static inline int
sm_msvc_clz64(uint64_t x)
{
	unsigned long idx;
	_BitScanReverse64(&idx, (unsigned __int64)x);
	return (63 - (int)idx);
}
#define SM_CLZ64(x) sm_msvc_clz64((uint64_t)(x))
#else
static inline int
sm_swar_clz64(uint64_t x)
{
	int n = 0;
	if (!(x & 0xFFFFFFFF00000000ULL)) {
		n += 32;
		x <<= 32;
	}
	if (!(x & 0xFFFF000000000000ULL)) {
		n += 16;
		x <<= 16;
	}
	if (!(x & 0xFF00000000000000ULL)) {
		n += 8;
		x <<= 8;
	}
	if (!(x & 0xF000000000000000ULL)) {
		n += 4;
		x <<= 4;
	}
	if (!(x & 0xC000000000000000ULL)) {
		n += 2;
		x <<= 2;
	}
	if (!(x & 0x8000000000000000ULL)) {
		n += 1;
	}
	return (n);
}
#define SM_CLZ64(x) sm_swar_clz64((uint64_t)(x))
#endif
#endif /* SM_CLZ64 */

/*
 * Diagnostic and assertion hooks.
 *
 * sparsemap reports internal invariant violations through three
 * macros, each independently overridable by the consumer (define it
 * before this translation unit is compiled, e.g. with -D on the
 * command line):
 *
 *	__sm_assert(expr)
 *		Evaluated wherever the library checks an internal
 *		invariant.  The built-in forms below are active only
 *		under SPARSEMAP_DIAGNOSTIC; in a normal build the
 *		default is ((void)0).  A host that wants its own
 *		assertion machinery (PostgreSQL's Assert(), the C
 *		standard assert(), an abort-on-fail check) defines
 *		__sm_assert to route there.
 *
 *	__sm_diag(fmt, ...)
 *		printf-style debug logging.  Default is ((void)0)
 *		outside SPARSEMAP_DIAGNOSTIC.  A host that wants the
 *		messages routed to its logger (PostgreSQL's elog,
 *		syslog, a ring buffer) defines __sm_diag.
 *
 *	__sm_when_diag(stmt)
 *		Guards diagnostic-only statement blocks (chunk dumps
 *		and the like).  Expands to `if (1) stmt` when
 *		diagnostics are on, `if (0) stmt` otherwise, so the
 *		compiler still type-checks the block but drops it.
 *
 * If a consumer overrides __sm_diag but not __sm_assert (or vice
 * versa) the un-overridden macro keeps its default.  When the
 * consumer overrides __sm_diag, the built-in __sm_diag_ sink below
 * is not compiled, so it costs nothing.
 */

#if defined(SPARSEMAP_DIAGNOSTIC) && !defined(__sm_diag)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#define __sm_diag(format, ...) \
	__sm_diag_(__FILE__, __LINE__, __func__, format, ##__VA_ARGS__)
#pragma GCC diagnostic pop
void
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 4, 5)))
#endif
    __sm_diag_(const char *file,
    const int line, const char *func, const char *format, ...)
{
	va_list args = { 0 };
	fprintf(stderr, "%s:%d:%s(): ", file, line, func);
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}
#endif

#if defined(SPARSEMAP_DIAGNOSTIC) && !defined(__sm_assert)
#define __sm_assert(expr)                                               \
	if (!(expr))                                                    \
	fprintf(stderr, "%s:%d:%s(): assertion failed! %s\n", __FILE__, \
	    __LINE__, __func__, #expr)
#endif

#if defined(SPARSEMAP_DIAGNOSTIC) && !defined(__sm_when_diag)
#define __sm_when_diag(expr) \
	if (1)               \
	expr
#endif

/* Defaults for any hook the consumer did not supply and that the
 * diagnostic build did not define above. */
#ifndef __sm_diag
/*
 * Default diagnostic sink: a no-op.  Spelled as a variadic function
 * (C89) rather than a variadic macro (C99) so the strict C90
 * compatibility audit stays clean; in a normal build nothing calls it
 * and the inline stub is elided.
 */
static inline void
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2), unused))
#endif
__sm_diag_noop(const char *format, ...)
{
	(void)format;
}
#define __sm_diag __sm_diag_noop
#endif
#ifndef __sm_assert
#define __sm_assert(expr) ((void)0)
#endif
#ifndef __sm_when_diag
#define __sm_when_diag(expr) \
	if (0)               \
	expr
#endif

#define IS_8_BYTE_ALIGNED(addr) (((uintptr_t)(addr) & 0x7) == 0)

/*
 * Branch-prediction hints.  These are no-ops on compilers that don't
 * understand __builtin_expect; on gcc/clang they let the optimizer
 * lay out the hot path inline and push the cold path off the icache.
 */
#if defined(__GNUC__) || defined(__clang__)
#define SM_LIKELY(cond)   __builtin_expect(!!(cond), 1)
#define SM_UNLIKELY(cond) __builtin_expect(!!(cond), 0)
#else
#define SM_LIKELY(cond)   (cond)
#define SM_UNLIKELY(cond) (cond)
#endif

/*
 * Function-attribute shims (see sm.h for SM_ALIGNED / ssize_t).
 * SM_ALWAYS_INLINE is a complete declaration prefix that replaces the
 * usual "static inline": on gcc/clang it forces inlining, on MSVC it
 * uses __forceinline, elsewhere it degrades to a plain static inline.
 * SM_HOT marks a hot function.
 * SM_WARN_UNUSED makes the compiler complain when a return value that
 * reports failure is discarded; it is how the "someone checked the
 * capacity" invariant is enforced at compile time rather than by an
 * assert that vanishes in release builds.
 */
#if defined(__GNUC__) || defined(__clang__)
#define SM_ALWAYS_INLINE static inline __attribute__((always_inline))
#define SM_HOT __attribute__((hot))
#define SM_WARN_UNUSED __attribute__((warn_unused_result))
#elif defined(_MSC_VER)
#define SM_ALWAYS_INLINE static __forceinline
#define SM_HOT
#define SM_WARN_UNUSED _Check_return_
#else
#define SM_ALWAYS_INLINE static inline
#define SM_HOT
#define SM_WARN_UNUSED
#endif

typedef uint64_t __sm_bitvec_t;

/*
 * __sm_idx_t: the type of a chunk-start offset -- the absolute,
 * chunk-aligned bit index that prefixes every chunk in the serialized
 * buffer.  It is uint64_t so the map addresses the full 64-bit index
 * space the public API advertises (SM_IDX_MAX == UINT64_MAX).
 *
 * This is an internal type, never exposed in <sm.h>; the public API
 * uses uint64_t for every bit location.  It exists as the single point
 * of control for the on-disk index width: SM_SIZEOF_OVERHEAD and the
 * __sm_load_idx / __sm_store_idx helpers all derive their width from
 * sizeof(__sm_idx_t), so the serialized format width is defined in
 * exactly one place.  (A 32-bit __sm_idx_t was the cause of the
 * pre-4.0 truncation bug for indices >= 2^32; keeping the width here,
 * named, makes it a one-line, reviewable decision.)
 */
typedef uint64_t __sm_idx_t;

/*
 * __sm_bitvec_unaligned_t: a 64-bit unsigned alias that the compiler
 * treats as having 1-byte alignment, so loads and stores through a
 * pointer of this type emit unaligned-safe code.
 *
 * The on-disk layout prefixes each chunk with an 8-byte start offset
 * (and the map with an 8-byte chunk-count header), so chunk
 * descriptors are naturally 8-aligned within m_data.  This typedef is
 * retained as defense-in-depth: a caller-supplied wrap() buffer may be
 * arbitrarily aligned, and accessing the descriptor through a plain
 * uint64_t * would then trip UBSan and trap on strict-alignment cpus.
 *
 * gcc and clang lower the unaligned access to whatever the platform
 * requires (a single load on x86_64, two byte-shuffled half-loads on
 * a strict-alignment cpu).  Zero overhead on the common targets.
 */
#if defined(__GNUC__) || defined(__clang__)
typedef uint64_t __attribute__((aligned(1))) __sm_bitvec_unaligned_t;
#elif defined(_MSC_VER)
typedef uint64_t __unaligned __sm_bitvec_unaligned_t;
#else
typedef uint64_t __sm_bitvec_unaligned_t;
#endif

/*
 * __sm_chunk_t holds only a pointer; the unaligned-safe access is a
 * property of the pointee type (__sm_bitvec_unaligned_t), so the
 * struct itself needs no special alignment.
 */
typedef struct {
	__sm_bitvec_unaligned_t *m_data;
} __sm_chunk_t;

typedef struct {
	size_t rem;
	size_t pos;
} __sm_chunk_rank_t;

/*
 * The exclusive end start + n of a chunk or run, saturated at UINT64_MAX.
 * The top chunk [2^64 - 2048, 2^64) ends at 2^64, which wraps to 0 in
 * uint64_t and makes that chunk look as if it ended before everything.
 * Saturating keeps every comparison against a member index exact,
 * because UINT64_MAX is SM_IDX_MAX and never a member.
 */
static inline uint64_t
__sm_end(uint64_t start, uint64_t n)
{
	return (start > UINT64_MAX - n ? UINT64_MAX : start + n);
}

/*
 * Unaligned-safe load and store helpers.
 *
 * sparsemap's on-disk layout places the 8-byte chunk-count header and
 * the 8-byte per-chunk start offsets at 8-byte boundaries within
 * m_data, naturally aligned for the cpu.  A caller-supplied wrap()
 * buffer, however, may be arbitrarily aligned.
 *
 * However, the pre-fix idiom (`*(__sm_idx_t *)p`) is technically UB
 * under the strict-aliasing rule when `p` is `uint8_t *`, and UBSan
 * complains.  More importantly, on strict-alignment cpus (some
 * RISC-V configurations, ARMv5, certain embedded platforms) a
 * misaligned access traps.  The memcpy idiom below is portable
 * across all of these.  Modern compilers (gcc 4.8+, clang 3.x+)
 * lower a `memcpy` of a fixed small size to a single native
 * load/store -- zero overhead on x86_64 and aarch64.
 *
 * See docs/ARCHITECTURE.md for the on-disk layout invariants.
 */
static inline __sm_idx_t
__sm_load_idx(const uint8_t *p)
{
	__sm_idx_t v;
	memcpy(&v, p, sizeof(v));
	return (v);
}

static inline void
__sm_store_idx(uint8_t *p, const __sm_idx_t v)
{
	memcpy(p, &v, sizeof(v));
}

/*
 * The chunk-count header occupies SM_SIZEOF_OVERHEAD (8) bytes at the
 * start of m_data.  Through v5.0 the count was stored as a uint32_t
 * (the low 4 bytes), capping a map at 2^32-1 chunks; the high 4 bytes
 * were always zero (sm_create / sm_clear memset the whole buffer, and
 * sm_deserialize copies a body whose high bytes are likewise zero).
 *
 * v5.1 widens the count to uint64_t using the full 8-byte slot.  This
 * is wire-compatible in both directions: a v5.1 reader sees the same
 * value in a v5.0 stream (high bytes zero), and a v5.1 writer storing
 * a count < 2^32 produces byte-identical output.  The ceiling now
 * exceeds the addressable index space, so the chunk count is no
 * longer a binding limit for any consumer.
 */
static inline uint64_t
__sm_load_u64(const uint8_t *p)
{
	uint64_t v;
	memcpy(&v, p, sizeof(v));
	return (v);
}

static inline void
__sm_store_u64(uint8_t *p, const uint64_t v)
{
	memcpy(p, &v, sizeof(v));
}

enum __SM_CHUNK_INFO {
	/* metadata overhead: sizeof(__sm_idx_t) bytes for the chunk-start
	 * offset / chunk-count header (8 bytes) */
	SM_SIZEOF_OVERHEAD = sizeof(__sm_idx_t),

	/* number of bits that can be stored in a __sm_bitvec_t */
	SM_BITS_PER_VECTOR = sizeof(__sm_bitvec_t) * 8,

	/* number of flags that can be stored in a single index byte */
	SM_FLAGS_PER_INDEX_BYTE = 4,

	/* number of flags that can be stored in the index */
	SM_FLAGS_PER_INDEX = sizeof(__sm_bitvec_t) * SM_FLAGS_PER_INDEX_BYTE,

	/* maximum capacity of a __sm_chunk_t (in bits) */
	SM_CHUNK_MAX_CAPACITY = SM_BITS_PER_VECTOR * SM_FLAGS_PER_INDEX,

	/* minimum capacity of a __sm_chunk_t (in bits) */
	SM_CHUNK_MIN_CAPACITY = SM_BITS_PER_VECTOR - 2,

	/* __sm_bitvec_t payload is all zeros (2#00) */
	SM_PAYLOAD_ZEROS = 0,

	/* __sm_bitvec_t payload is all ones (2#11) */
	SM_PAYLOAD_ONES = 3,

	/* __sm_bitvec_t payload is mixed (2#10) */
	SM_PAYLOAD_MIXED = 2,

	/* __sm_bitvec_t is not used (2#01) */
	SM_PAYLOAD_NONE = 1,

	/* a mask for checking flags (2 bits, 2#11) */
	SM_FLAG_MASK = 3,

	/* return code for set(): ok, no further action required */
	SM_OK = 0,

	/* return code for set(): needs to grow this __sm_chunk_t */
	SM_NEEDS_TO_GROW = 1,

	/* return code for set(): needs to shrink this __sm_chunk_t */
	SM_NEEDS_TO_SHRINK = 2
};

/* Check (f) in sm_validate finds the highest data-bearing slot by masking
 * the HIGH bit of every flag (0xAAAA...).  That is only right while ONES
 * and MIXED are exactly the two flags with the high bit set. */
_Static_assert((SM_PAYLOAD_ONES & 2) != 0 && (SM_PAYLOAD_MIXED & 2) != 0 &&
    (SM_PAYLOAD_NONE & 2) == 0 && (SM_PAYLOAD_ZEROS & 2) == 0,
    "check (f) in sm_validate relies on the flag encoding");

/*
 * SM_ENOUGH_SPACE: if growing m_data_used by `need` bytes would push
 * past m_capacity, return SM_IDX_MAX with errno=ENOSPC.
 *
 * The +SM_SIZEOF_OVERHEAD slack accounts for an off-by-4 read in
 * __sm_insert_data: the memmove length there is `m_data_used -
 * offset`, which over-counts by SM_SIZEOF_OVERHEAD when m_data_used
 * includes the chunk-count header (the post-sm_clear()
 * convention).  Without the slack the over-read writes
 * SM_SIZEOF_OVERHEAD bytes past the buffer end at the boundary.
 * Fixing the off-by-4 in __sm_insert_data directly is preferable
 * but breaks the alternate convention used by
 * sm_wrap()-without-clear callers, where m_data_used does
 * not include the header.
 */
#define SM_ENOUGH_SPACE(need)                                        \
	do {                                                         \
		if (map->m_data_used + (need) + SM_SIZEOF_OVERHEAD > \
		    __sm_cap(map)) {                                  \
			errno = ENOSPC;                              \
			return (SM_IDX_MAX);                         \
		}                                                    \
	} while (0)

#define SM_CHUNK_GET_FLAGS(data, at) \
	((((data)) & ((__sm_bitvec_t)SM_FLAG_MASK << ((at) * 2))) >> ((at) * 2))
#define SM_CHUNK_SET_FLAGS(data, at, to)                                    \
	((data) = ((data) & ~((__sm_bitvec_t)SM_FLAG_MASK << ((at) * 2))) | \
	        ((__sm_bitvec_t)(to) << ((at) * 2)))

/*
 * struct sparsemap is defined in <sm.h> (visible here because this
 * file defines SM_INTERNAL before including it; consumers get it via
 * SM_EXPOSE_STRUCT).  Keeping the single definition in the header
 * lets callers embed an sm_t by value without the layout drifting
 * from the library.
 */

/*
 * Allocation lineage.  Tracked per sm_t so the grow / dispose
 * paths know what they may safely realloc or free.
 *
 * SM_OWNED_CONTIGUOUS  Single calloc(1, sizeof(sm_t) + size).
 *                      Both the struct and m_data live in one heap
 *                      block; m_data sits immediately after the struct.
 *                      Set by sparsemap() and sm_copy().  May be
 *                      grown via realloc, and disposed with free(map).
 *                      Default for zero-initialized memory.
 *
 * SM_WRAPPED           m_data points to a buffer the caller owns.  Set
 *                      by sm_wrap(), sm_init(), and
 *                      sm_open().  Cannot be realloc'd in place;
 *                      sm_set_data_size with data == NULL will
 *                      transparently promote to SM_OWNED_SPLIT by
 *                      allocating a fresh library-owned buffer and
 *                      copying the m_data_used prefix into it.  The
 *                      caller's original buffer is left untouched and
 *                      remains theirs to free.
 *
 * SM_OWNED_SPLIT       The struct is heap-allocated; m_data is
 *                      separately heap-allocated and owned by the
 *                      library (typically the result of promoting an
 *                      SM_WRAPPED map via grow).  Disposed with
 *                      sm_free, which does free(m_data) +
 *                      free(map).
 */
enum sm_alloc_kind {
	SM_OWNED_CONTIGUOUS = 0,
	SM_WRAPPED = 1,
	SM_OWNED_SPLIT = 2
};

/* -------------------------------------------------------------------
 * Allocator hooks
 *
 * Sparsemap routes every malloc/realloc/free through these helpers,
 * which consult a single process-global allocator (CRoaring style).
 * Any individual hook may be NULL; the helper falls back to libc for
 * that operation.  An all-zero allocator therefore means "use libc
 * throughout", which is the default.
 * ------------------------------------------------------------------- */

static sm_allocator_t __sm_g_allocator = { 0 };

void
sm_set_allocator(sm_allocator_t a)
{
	__sm_g_allocator = a;
}

static inline void *
__sm_alloc(size_t n)
{
	if (__sm_g_allocator.malloc != NULL) {
		return (__sm_g_allocator.malloc(n));
	}
	return (malloc(n));
}

static inline void *
__sm_alloc_zero(size_t n)
{
	void *p = __sm_alloc(n);
	if (p != NULL) {
		memset(p, 0, n);
	}
	return (p);
}

static inline void *
__sm_realloc(void *p, size_t n)
{
	if (__sm_g_allocator.realloc != NULL) {
		return (__sm_g_allocator.realloc(p, n));
	}
	return (realloc(p, n));
}

static inline void
__sm_free(void *p)
{
	if (__sm_g_allocator.free != NULL) {
		__sm_g_allocator.free(p);
		return;
	}
	free(p);
}

/* -------------------------------------------------------------------
 * Capacity / lineage accessors
 *
 * The allocation-lineage tag (how m_data was provisioned) is folded
 * into the low 3 bits of m_capacity.  Capacity is always rounded up to
 * an 8-byte boundary before being stored, so those bits are free.
 * Read the byte capacity with __sm_cap() and the lineage with
 * __sm_kind(); never touch m_capacity directly.
 * ------------------------------------------------------------------- */

static inline size_t
__sm_cap(const sm_t *m)
{
	return (m->m_capacity & ~(size_t)7);
}

static inline uint8_t
__sm_kind(const sm_t *m)
{
	return ((uint8_t)(m->m_capacity & 7u));
}

static inline void
__sm_set_cap_kind(sm_t *m, size_t cap, uint8_t kind)
{
	/* Round capacity DOWN to an 8-byte boundary so the tag bits are
	 * free and we never report more usable bytes than the buffer
	 * actually has.  Library-owned allocators round the allocation
	 * size UP before calling here, so for them this is a no-op; for
	 * caller-supplied (wrapped) buffers a non-8-aligned size loses up
	 * to 7 trailing bytes, which is the documented behavior. */
	m->m_capacity = (cap & ~(size_t)7) | (kind & 7u);
}

static inline void
__sm_set_kind(sm_t *m, uint8_t kind)
{
	m->m_capacity = (m->m_capacity & ~(size_t)7) | (kind & 7u);
}

/* -------------------------------------------------------------------
 * Lazy cardinality cache (runtime-only; see the struct comment in sm.h)
 *
 * m_card_plus1 caches sm_cardinality() biased by one so that the
 * all-zero state means "unknown": 0 = invalid, otherwise the cached
 * count is (m_card_plus1 - 1).  The cache is derived purely from
 * m_data, so an invalid cache is always safe -- the next query just
 * recomputes it.  Every membership-changing path calls
 * __sm_card_invalidate(); sm_cardinality fills it on the first query
 * after a mutation.  Never serialized, never sent to the wire.
 * ------------------------------------------------------------------- */

static inline void
__sm_card_invalidate(sm_t *m)
{
	if (m != NULL)
		m->m_card_plus1 = 0;
}

static inline bool
__sm_card_is_valid(const sm_t *m)
{
	return (m->m_card_plus1 != 0);
}

static inline size_t
__sm_card_get(const sm_t *m)
{
	return (m->m_card_plus1 - 1u);
}

static inline void
__sm_card_store(sm_t *m, size_t count)
{
	/* count + 1 cannot wrap in practice (count <= SM_IDX_MAX bits), but
	 * guard: a would-be-wrapping count simply stays uncached. */
	if (m != NULL && count != SIZE_MAX)
		m->m_card_plus1 = count + 1u;
}


/*
 * Internal-invariant check.  No-op in production builds; under
 * SPARSEMAP_TESTING / SPARSEMAP_DIAGNOSTIC it asserts:
 *
 *   - map is non-NULL
 *   - m_data is non-NULL when the byte capacity > 0
 *   - m_data_used <= byte capacity (no buffer overrun)
 *   - m_data is 8-byte aligned (the chunk codec assumes this)
 *   - the lineage tag (low bits of m_capacity) is one of the three
 *     known values
 *
 * The intent is to fail at the moment a corrupted map is touched,
 * rather than three operations later when the libc heap finally
 * notices.  Called at the top of every public mutating or query
 * function in the heisenbug-fix series.
 */
static inline void
__sm_check_invariants(const struct sparsemap *map)
{
	__sm_when_diag({
		__sm_assert(map != NULL);
		if (map == NULL)
			return;
		__sm_assert(__sm_cap(map) == 0 || map->m_data != NULL);
		__sm_assert(map->m_data_used <= __sm_cap(map));
		__sm_assert(IS_8_BYTE_ALIGNED(map->m_data));
		__sm_assert(__sm_kind(map) == SM_OWNED_CONTIGUOUS ||
		    __sm_kind(map) == SM_WRAPPED ||
		    __sm_kind(map) == SM_OWNED_SPLIT);
	});
}

/*
 * Descriptor flag masks.
 *
 * A sparse descriptor packs thirty-two 2-bit flags, flag i in bits
 * [2i, 2i+1].  Splitting the word into the high bit (SM_DESC_HI_BITS)
 * and the low bit (SM_DESC_LO_BITS) of every flag lets one popcount
 * count all flags of a kind at once.  The bit that represents flag i
 * in the masks below is the flag's high bit, 2i+1.
 */
#define SM_DESC_HI_BITS UINT64_C(0xAAAAAAAAAAAAAAAA)
#define SM_DESC_LO_BITS UINT64_C(0x5555555555555555)

/**
 * @brief Marks every SM_PAYLOAD_MIXED (2#10) flag of a sparse descriptor.
 *
 * @param[in] desc A chunk descriptor.
 * @return A word with bit 2i+1 set exactly when flag i is MIXED; every
 *         other bit is clear.
 */
static inline __sm_bitvec_t
__sm_desc_mixed_mask(const __sm_bitvec_t desc)
{
	return ((desc & SM_DESC_HI_BITS) & ~((desc & SM_DESC_LO_BITS) << 1));
}

/**
 * @brief Marks every SM_PAYLOAD_NONE (2#01) flag of a sparse descriptor.
 *
 * @param[in] desc A chunk descriptor.
 * @return A word with bit 2i+1 set exactly when flag i is NONE; every
 *         other bit is clear.
 */
static inline __sm_bitvec_t
__sm_desc_none_mask(const __sm_bitvec_t desc)
{
	return (((desc & SM_DESC_LO_BITS) << 1) & ~(desc & SM_DESC_HI_BITS));
}

/**
 * @brief Extracts flag-byte @a n of a chunk descriptor, endian-neutrally.
 *
 * The sparse descriptor packs thirty-two 2-bit flags into one 64-bit
 * word, four flags per byte, with flag @a i occupying bits
 * [2*i, 2*i+1] of the word.  Flag byte @a n therefore covers flags
 * [4n, 4n+3], i.e. word bits [8n, 8n+7].
 *
 * Reading those bytes by walking a `uint8_t *` over the word only works
 * on a little-endian host: on big-endian, byte 0 of the object is the
 * word's most-significant byte, so a byte walk visits the flags in
 * reverse.  That produced a correct sm_contains (which shifts the word
 * directly) but wrong sm_cardinality / minimum / maximum / rank /
 * select on big-endian hosts.  Shifting the word is correct everywhere
 * and compiles to the same single byte load on little-endian.
 *
 * @param[in] desc The chunk descriptor word.
 * @param[in] n    Flag-byte index in [0, sizeof(__sm_bitvec_t)).
 * @return Byte @a n of the logical flag sequence.
 */
static inline uint8_t
__sm_desc_flag_byte(const __sm_bitvec_t desc, const size_t n)
{
	return ((uint8_t)((desc >> (n * 8)) & 0xFFu));
}

/**
 * @brief Retrieves the position within the chunk corresponding to the specified bit vector index.
 *
 * This function calculates the position in the chunk's data array that
 * corresponds to the given bit vector index.
 *
 * @param[in] chunk The chunk from which to retrieve the position.
 * @param[in] bv The bit vector index within the chunk.
 * @return The position within the chunk's data array corresponding to the specified bit vector index.
 */
SM_ALWAYS_INLINE size_t
__sm_chunk_get_position(const __sm_chunk_t *chunk, size_t bv)
{
	__sm_bitvec_t below;
	/* Defense-in-depth: callers compute `bv` as `idx / SM_BITS_PER_VECTOR`
	 * after subtracting the chunk's start offset; on a corrupt buffer
	 * (sm_open of attacker-controlled bytes) the start offset can be
	 * wildly wrong, making `bv` arbitrarily large.  Clamp to the
	 * physical chunk capacity so the mask shift below stays defined.
	 * Returning 0 here causes the caller to read
	 * chunk->m_data[1] which is also bounded by the chunk_size that
	 * __sm_get_size_impl validated. */
	if (bv >= SM_FLAGS_PER_INDEX || bv == 0) {
		return (0);
	}

	/* The payload word of flag bv follows one stored word per MIXED
	 * flag below it, so count the MIXED flags in [0, bv).  bv < 32
	 * here, so the mask shift is in [2, 64) and well defined. */
	below = ~(__sm_bitvec_t)0 >> (SM_BITS_PER_VECTOR - 2 * bv);
	return ((size_t)SM_POPCOUNT64(
	    __sm_desc_mixed_mask(*chunk->m_data) & below));
}

/**
 * @brief Initializes an __sm_chunk_t structure with the given data.
 *
 * This function sets the m_data member of the provided __sm_chunk_t structure to point
 * to the given data, cast as a pointer to __sm_bitvec_t.
 *
 * @param[in,out] chunk The chunk to initialize.
 * @param[in] data The data to associate with the chunk.
 */
static void
__sm_chunk_init(__sm_chunk_t *chunk, uint8_t *data)
{
	chunk->m_data = (__sm_bitvec_unaligned_t *)data;
}

/**
 * @brief Retrieves the capacity of the given chunk.
 *
 * Every chunk in this build is sparse; the capacity is computed by
 * examining the descriptor's per-vector flags and subtracting the
 * SM_PAYLOAD_NONE (unused) sections from the maximum.
 *
 * @param[in] chunk The chunk whose capacity is to be determined.
 * @return The capacity of the chunk.
 */
SM_ALWAYS_INLINE size_t
__sm_chunk_get_capacity(const __sm_chunk_t *chunk)
{
	/* Every NONE flag removes one vector from the full capacity. */
	return (SM_CHUNK_MAX_CAPACITY - SM_BITS_PER_VECTOR *
	    (size_t)SM_POPCOUNT64(__sm_desc_none_mask(*chunk->m_data)));
}

/*
 * The exclusive end of the index window a chunk starting at `start`
 * owns, saturated at UINT64_MAX.  A chunk owns its whole aligned window
 * [start, start + 2048) even when NONE slots reduce its capacity
 * (sm_offset emits such chunks): the uncovered tail holds no members,
 * but no other chunk may start there, so a lookup for an index in the
 * tail must stop at this chunk.  Its capacity still bounds which bits
 * are members.
 */
static inline uint64_t
__sm_chunk_window_end(uint64_t start)
{
	return (__sm_end(start, SM_CHUNK_MAX_CAPACITY));
}

/**
 * @brief Increases the capacity of a chunk to the specified value.
 *
 * This function adjusts the capacity of a given chunk, ensuring that the new capacity
 * is a multiple of SM_BITS_PER_VECTOR, does not exceed the maximum allowed capacity,
 * and is greater than the current capacity of the chunk. The capacity is increased by
 * marking payload bits in the chunk's data array.
 *
 * @param[in,out] chunk The chunk whose capacity is to be increased.
 * @param[in] capacity The new capacity to set for the chunk.
 */
static void
__sm_chunk_increase_capacity(const __sm_chunk_t *chunk, const size_t capacity)
{
	size_t initial_capacity;
	size_t increased;
	size_t i;
	__sm_assert(capacity % SM_BITS_PER_VECTOR == 0);
	__sm_assert(capacity <= SM_CHUNK_MAX_CAPACITY);
	__sm_assert(capacity > __sm_chunk_get_capacity(chunk));

	initial_capacity = __sm_chunk_get_capacity(chunk);
	if (capacity <= initial_capacity || capacity > SM_CHUNK_MAX_CAPACITY) {
		return;
	}

	increased = 0;
	for (i = 0; i < sizeof(__sm_bitvec_t); i++) {
		int j;
		const uint8_t b = __sm_desc_flag_byte(*chunk->m_data, i);
		if (!b || b == 0xff) {
			continue;
		}
		for (j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
			const size_t flags = SM_CHUNK_GET_FLAGS(b, j);
			if (flags == SM_PAYLOAD_NONE) {
				/* Flag (i * 4 + j) of the descriptor word;
				 * set it word-wise so the update is
				 * endian-neutral. */
				__sm_bitvec_t desc = *chunk->m_data;
				SM_CHUNK_SET_FLAGS(desc,
				    (i * (size_t)SM_FLAGS_PER_INDEX_BYTE) +
				        (size_t)j,
				    SM_PAYLOAD_ZEROS);
				*chunk->m_data = desc;
				increased += SM_BITS_PER_VECTOR;
				if (increased + initial_capacity == capacity) {
					__sm_assert(__sm_chunk_get_capacity(
					                chunk) == capacity);
					return;
				}
			}
		}
	}
	__sm_assert(__sm_chunk_get_capacity(chunk) == capacity);
}

/**
 * @brief Determines if a given chunk is empty.
 *
 * This function checks if all flags within the chunk's data are either
 * SM_PAYLOAD_ZEROS or SM_PAYLOAD_NONE. If any flag doesn't meet these
 * criteria, the chunk is considered not empty.
 *
 * @param[in] chunk The chunk to be evaluated.
 * @return True if the chunk is empty, otherwise false.
 */
static bool
__sm_chunk_is_empty(const __sm_chunk_t *chunk)
{
	if (chunk->m_data[0] != 0) {
		size_t i;
		/* A chunk is considered empty if all flags are SM_PAYLOAD_ZERO or _NONE. */
		const __sm_bitvec_t desc = *chunk->m_data;
		for (i = 0; i < sizeof(__sm_bitvec_t); i++) {
			const uint8_t b = __sm_desc_flag_byte(desc, i);
			if (b) {
				int j;
				for (j = 0; j < SM_FLAGS_PER_INDEX_BYTE;
				     j++) {
					const size_t flags =
					    SM_CHUNK_GET_FLAGS(b, j);
					if (flags != SM_PAYLOAD_NONE &&
					    flags != SM_PAYLOAD_ZEROS) {
						return (false);
					}
				}
			}
		}
	}
	/* The __sm_chunk_t is empty if all flags (in m_data[0]) are zero. */
	return (true);
}

/**
 * @brief Retrieves the size of the specified chunk.
 *
 * Computes the byte size of the chunk.  A chunk's size is the descriptor
 * word plus one payload word per MIXED vector (one masked popcount).  The
 * result is bounded (at most 8 bytes of descriptor plus 32 payload
 * words), so the pre-validate stride walk can never be driven to
 * over-read by a crafted descriptor.
 *
 * @param[in] chunk The chunk whose size is to be determined.
 * @return The size of the chunk in bytes.
 */
SM_ALWAYS_INLINE size_t
__sm_chunk_get_size(const __sm_chunk_t *chunk)
{
	/* The descriptor word plus one payload word per MIXED flag. */
	return (sizeof(__sm_bitvec_t) * ((size_t)1 +
	    (size_t)SM_POPCOUNT64(__sm_desc_mixed_mask(*chunk->m_data))));
}

/**
 * @brief Checks if a specific bit is set in a given chunk.
 *
 * This function determines if a bit at a specific index within a sparse
 * chunk is set.
 *
 * @param[in] chunk The chunk to check.
 * @param[in] idx The index of the bit to check within the chunk.
 * @return True if the bit at the specified index is set, false otherwise.
 */
SM_ALWAYS_INLINE bool
__sm_chunk_is_set(const __sm_chunk_t *chunk, const size_t idx)
{
	size_t bv;
	size_t flags;
	__sm_bitvec_t w;
	/* Defense-in-depth: on a corrupt buffer (attacker-controlled
	 * chunk start offset) the caller's `idx - start` can wrap to a
	 * value way beyond SM_CHUNK_MAX_CAPACITY.  Reject those without
	 * trying to compute `bv`. */
	if (idx >= SM_CHUNK_MAX_CAPACITY) {
		return (false);
	}
	/* in which __sm_bitvec_t is |idx| stored? */
	bv = idx / SM_BITS_PER_VECTOR;
	__sm_assert(bv < SM_FLAGS_PER_INDEX);

	/* now retrieve the flags of that __sm_bitvec_t */
	flags = SM_CHUNK_GET_FLAGS(*chunk->m_data, bv);
	switch (flags) {
	case SM_PAYLOAD_ZEROS:
	case SM_PAYLOAD_NONE:
		return (false);
	case SM_PAYLOAD_ONES:
		return (true);
	default:
		__sm_assert(flags == SM_PAYLOAD_MIXED);
		/* FALLTHROUGH */
	}

	/* get the __sm_bitvec_t at |bv| */
	w = chunk->m_data[1 + __sm_chunk_get_position(chunk, bv)];
	/* and finally check the bit in that __sm_bitvec_t */
	return ((w & (__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR) > 0);
}

/**
 * @brief Clears a specific bit in a chunk.
 *
 * This function attempts to clear a specified bit within a given chunk.
 * Based on the payload flags in the chunk, it will update the position of
 * the bit and handle transitions between different payload states
 * (ZEROS, ONES, MIXED). If the bit is already clear, it performs a no-op.
 * If the bit is set, it updates the relevant data structures accordingly,
 * possibly requiring the chunk to grow or shrink.
 *
 * @param[in] chunk The chunk in which to clear the bit.
 * @param[in] idx The index of the bit to be cleared.
 * @param[out] pos The position of the bit to be cleared; updated internally.
 * @return An integer status code indicating the result:
 *         - SM_OK if the operation was successful,
 *         - SM_NEEDS_TO_GROW if the chunk needs to grow,
 *         - SM_NEEDS_TO_SHRINK if the chunk needs to shrink.
 */
static int
__sm_chunk_clr_bit(const __sm_chunk_t *chunk, const uint64_t idx, size_t *pos)
{
	__sm_bitvec_t w;
	const size_t bv = idx / SM_BITS_PER_VECTOR;

	__sm_assert(bv < SM_FLAGS_PER_INDEX);

	switch (SM_CHUNK_GET_FLAGS(*chunk->m_data, bv)) {
	case SM_PAYLOAD_ZEROS:
		/* The bit is already clear, no-op. */
		*pos = 0;
		return (SM_OK);
		break;
	case SM_PAYLOAD_ONES:
		/* What was all ones transitions to mixed, which requires another vector. */
		if (*pos == 0) {
			*pos = (size_t)1 + __sm_chunk_get_position(chunk, bv);
			return (SM_NEEDS_TO_GROW);
		}
		SM_CHUNK_SET_FLAGS(*chunk->m_data, bv, SM_PAYLOAD_MIXED);
		w = chunk->m_data[*pos];
		w &= ~((__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR);
		/* Update the mixed vector. */
		chunk->m_data[*pos] = w;
		return (SM_OK);
		break;
	case SM_PAYLOAD_MIXED:
		*pos = 1 + __sm_chunk_get_position(chunk, bv);
		w = chunk->m_data[*pos];
		w &= ~((__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR);
		/* Did the vector transition from mixed to all zeros? If so, remove it. */
		if (w == 0) {
			SM_CHUNK_SET_FLAGS(*chunk->m_data, bv,
			    SM_PAYLOAD_ZEROS);
			return (SM_NEEDS_TO_SHRINK);
		}
		/* Update the mixed vector. */
		chunk->m_data[*pos] = w;
		break;
	case SM_PAYLOAD_NONE:
		/* FALLTHROUGH */
	default:
		__sm_assert(!"shouldn't be here");
#ifdef DEBUG
		abort();
#endif
		break;
	}
	return (SM_OK);
}

/**
 * @brief Sets a bit within a chunk at the specified index.
 *
 * This function sets a bit in the given chunk at the location specified by the index.
 * It handles different payload states (all ones, all zeros, and mixed) and updates
 * the chunk's data and flags accordingly.
 *
 * @param[in] chunk The chunk to modify.
 * @param[in] idx The index within the chunk where the bit should be set.
 * @param[out] pos Pointer to a size_t that will be set to the position of the bit.
 * @return An integer indicating the status of the operation. Possible return values are:
 *         - SM_OK: The bit was successfully set.
 *         - SM_NEEDS_TO_GROW: The chunk needs additional space.
 *         - SM_NEEDS_TO_SHRINK: The chunk has excess space that can be reclaimed.
 */
static int
__sm_chunk_set_bit(const __sm_chunk_t *chunk, const uint64_t idx, size_t *pos)
{
	/* Where in the descriptor does this idx fall, which flag should we examine? */
	const size_t bv = idx / SM_BITS_PER_VECTOR;
	__sm_assert(bv < SM_FLAGS_PER_INDEX);

	switch (SM_CHUNK_GET_FLAGS(*chunk->m_data, bv)) {
		__sm_bitvec_t w;
	case SM_PAYLOAD_ONES:
		/* The bit is already set, no-op. */
		*pos = 0;
		return (SM_OK);
		break;
	case SM_PAYLOAD_ZEROS:
		/* What was all zeros transitions to mixed, which requires another vector. */
		if (*pos == 0) {
			*pos = (size_t)1 + __sm_chunk_get_position(chunk, bv);
			return (SM_NEEDS_TO_GROW);
		}
		SM_CHUNK_SET_FLAGS(*chunk->m_data, bv, SM_PAYLOAD_MIXED);
		/* FALLTHROUGH */
	case SM_PAYLOAD_MIXED:
		*pos = 1 + __sm_chunk_get_position(chunk, bv);
		w = chunk->m_data[*pos];
		w |= (__sm_bitvec_t)1 << idx % SM_BITS_PER_VECTOR;
		/* Did the vector transition from mixed to all ones? If so, remove it. */
		if (w == ~(__sm_bitvec_t)0) {
			SM_CHUNK_SET_FLAGS(*chunk->m_data, bv, SM_PAYLOAD_ONES);
			return (SM_NEEDS_TO_SHRINK);
		}
		/* Update the mixed vector. */
		chunk->m_data[*pos] = w;
		break;
	case SM_PAYLOAD_NONE:
		/* FALLTHROUGH */
	default:
#ifdef DEBUG
		abort();
#endif
		break;
	}
	return (SM_OK);
}

/**
 * @brief Counts the set bits of a chunk.
 *
 * A chunk holds 64 bits per ONES flag plus the set bits of every stored
 * MIXED payload word.  The payload words are contiguous after the
 * descriptor, so the count needs no per-slot position lookup.
 *
 * @param[in] chunk The chunk to count; it must be valid (no data-bearing
 *            slot past its payload words).
 * @return The number of set bits in the chunk.
 */
static size_t
__sm_chunk_popcount(const __sm_chunk_t *chunk)
{
	const __sm_bitvec_t desc = chunk->m_data[0];
	size_t nmixed;
	size_t count;
	size_t i;

	/* ONES (2#11) flags: both bits of the flag set. */
	count = SM_BITS_PER_VECTOR * (size_t)SM_POPCOUNT64((desc &
	    SM_DESC_HI_BITS) & ((desc & SM_DESC_LO_BITS) << 1));
	nmixed = (size_t)SM_POPCOUNT64(__sm_desc_mixed_mask(desc));
	for (i = 0; i < nmixed; i++) {
		count += (size_t)SM_POPCOUNT64(chunk->m_data[1 + i]);
	}
	return (count);
}

/**
 * @brief Returns the position of the n-th (0-based) set bit of a word.
 *
 * With BMI2, pdep deposits the single bit 1 << n onto the n-th set bit of
 * @a w and tzcnt reads its position.  The portable form clears the @a n
 * lowest set bits and counts the trailing zeros of what is left; it runs
 * fewer than 64 iterations.
 *
 * NB: the caller guarantees n < popcount(w); the result is meaningless
 * otherwise.
 *
 * @param[in] w The word to search.
 * @param[in] n The 0-based rank of the set bit to find.
 * @return The bit position, in [0, 64).
 */
static inline size_t
__sm_word_select(__sm_bitvec_t w, size_t n)
{
	__sm_assert(n < (size_t)SM_POPCOUNT64(w));
#if defined(SM_HAVE_PDEP64)
	return ((size_t)SM_CTZ64(_pdep_u64(UINT64_C(1) << n, w)));
#else
	while (n > 0) {
		w &= w - 1;
		n--;
	}
	return ((size_t)SM_CTZ64(w));
#endif
}

/**
 * @brief Selects the nth bit with the specified value from a chunk.
 *
 * This function scans a chunk of data to find the nth occurrence of a bit
 * with the specified value (true for 1, false for 0) after skipping offset
 * bits (of any value).
 *
 * The chunk is scanned over its whole 2048-bit window, slot by slot; the
 * positions of a NONE slot count as unset.
 *
 * @param[in] chunk The chunk to scan for the bit.
 * @param[in] n The number of bits of value to count before returning.
 * @param[in,out] offset The number of bits to skip before starting to count.
 * @param[in] value The bit value to search for (true for 1, false for 0).
 * @return The index within this chunk of the bit when found, otherwise the
 * number of bits scanned (at most SM_BITS_PER_VECTOR).
 */
static size_t
__sm_chunk_select(const __sm_chunk_t *chunk, ssize_t n, ssize_t *offset,
    const bool value)
{
	size_t i;
	/*
	 * Sparse encoding path
	 *
	 * Algorithm: Iterate through flag bytes examining 2-bit descriptors for each 64-bit vector.
	 * Skip vectors that can't contain the target value (ZEROS when searching for 1s, ONES when
	 * searching for 0s). For MIXED vectors, use popcount to quickly check if we need to scan
	 * individual bits. Accumulate bit positions until we've found the nth occurrence.
	 */
	size_t ret = 0;
	const __sm_bitvec_t sel_desc = *chunk->m_data;
	for (i = 0; i < sizeof(__sm_bitvec_t); i++) {
		int j;
		const uint8_t b = __sm_desc_flag_byte(sel_desc, i);
		/* Quick skip: if flag byte is 0 (all NONE descriptors) and seeking 1s, skip 4 vectors */
		if (b == 0 && value) {
			ret += (size_t)SM_FLAGS_PER_INDEX_BYTE *
			    SM_BITS_PER_VECTOR;
			continue;
		}

		for (j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
			const size_t flags = SM_CHUNK_GET_FLAGS(b, j);
			if (flags == SM_PAYLOAD_NONE ||
			    flags == SM_PAYLOAD_ZEROS) {
				/* Neither holds a set bit.  A NONE slot has no
				 * payload but still occupies its index range:
				 * __sm_chunk_is_set addresses flags positionally
				 * as flags[idx / 64], so its 64 positions are
				 * unset, exactly like a ZEROS slot's. */
				if (value == true) {
					ret += SM_BITS_PER_VECTOR;
					continue;
				}
				/* This slot supplies exactly
				 * SM_BITS_PER_VECTOR candidates, addressed
				 * n = 0 .. SM_BITS_PER_VECTOR-1.  The guard
				 * must therefore be >=, not >: with > the
				 * n == SM_BITS_PER_VECTOR case returned
				 * ret + 64, one position past the slot,
				 * instead of moving on to the next one. */
				if (n >= SM_BITS_PER_VECTOR) {
					n -= SM_BITS_PER_VECTOR;
					ret += SM_BITS_PER_VECTOR;
					continue;
				}
				*offset = -1;
				return (ret + (size_t)n);
			}
			if (flags == SM_PAYLOAD_ONES) {
				if (value == true) {
					/* Same off-by-one as the ZEROS arm
					 * above: an all-ones slot holds set
					 * bits n = 0 .. 63, so n == 64 belongs
					 * to a later slot.  With > this
					 * returned a position inside this slot
					 * for a bit that lives further on --
					 * e.g. a map with bits [0,128) and
					 * [500,510) answered
					 * sm_select(128, true) = 128 instead of
					 * 500. */
					if (n >= SM_BITS_PER_VECTOR) {
						n -= SM_BITS_PER_VECTOR;
						ret += SM_BITS_PER_VECTOR;
						continue;
					}
					*offset = -1;
					return (ret + (size_t)n);
				}
				ret += SM_BITS_PER_VECTOR;
				continue;
			}
			if (flags == SM_PAYLOAD_MIXED) {
				const __sm_bitvec_t w = chunk->m_data[1 +
				    __sm_chunk_get_position(chunk,
				        (i * SM_FLAGS_PER_INDEX_BYTE) + (size_t)j)];
				const __sm_bitvec_t target_bits = value ? w : ~w;
				const ssize_t pc = SM_POPCOUNT64(target_bits);
				if (n >= pc) {
					n -= pc;
					ret += SM_BITS_PER_VECTOR;
					continue;
				}
				*offset = -1;
				return (ret +
				    __sm_word_select(target_bits, (size_t)n));
			}
		}
	}
	*offset = n;
	return (ret);
}

/**
 * @brief Calculates the rank of a bit in a chunk between specified indices.
 *
 * This function computes the number of bits set to a particular state (true
 * or false) within a chunk of data, starting from a specified index and ending
 * at a specified index.
 *
 * Invoking this function with `from = 0` and `to = 0` (the range [0, 0]), will
 * compare 1 bit at the position 0 against value. The range [0, 9] will examine
 * 10 bits, starting with the 0th and ending with the 9th and return at most a
 * count of 10.
 *
 * @param[out] rank Pointer to the rank data structure to populate.
 * @param[in] value The bit state to calculate the rank for (true or false).
 * @param[in] chunk Pointer to the chunk to be examined.
 * @param[in] from The starting index within the chunk.
 * @param[in] to The ending index within the chunk.
 * @return The number of bits in the specified state between the indices [from, to].
 */
static size_t
__sm_chunk_rank(__sm_chunk_rank_t *rank, const bool value,
    const __sm_chunk_t *chunk, size_t from, size_t to)
{
	size_t amt = 0;
	const size_t cap = __sm_chunk_get_capacity(chunk);

	__sm_assert(to >= from);
	rank->rem = cap;
	rank->pos = 0;

	if (from >= cap) {
		rank->pos = cap;
		rank->rem = 0;
		return (amt);
	}

	{
		size_t i;
		/*
		 * Sparse encoding rank algorithm
		 *
		 * Strategy: Iterate through flag bytes and use popcounts for efficient bit counting.
		 * For ZEROS/ONES payloads, we know the count immediately (0 or 64). For MIXED payloads,
		 * extract the 64-bit vector and use hardware popcount. Apply range masks to only count
		 * bits within [from, to] range. This achieves O(chunks) performance instead of O(bits).
		 */
		const __sm_bitvec_t rank_desc = *chunk->m_data;
		__sm_bitvec_t w, mw;
		uint64_t mask;
		size_t pc;

		for (i = 0; i < sizeof(__sm_bitvec_t); i++) {
			int j;
			const uint8_t vb = __sm_desc_flag_byte(rank_desc, i);
			for (j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
				const size_t flags =
				    SM_CHUNK_GET_FLAGS(vb, j);

				switch (flags) {
				case SM_PAYLOAD_ZEROS:
					rank->rem = 0;
					if (to >= SM_BITS_PER_VECTOR) {
						rank->pos += SM_BITS_PER_VECTOR;
						to -= SM_BITS_PER_VECTOR;
						if (from >=
						    SM_BITS_PER_VECTOR) {
							from = from -
							    SM_BITS_PER_VECTOR;
						} else {
							if (!value) {
								amt +=
								    SM_BITS_PER_VECTOR -
								    from;
							}
							from = 0;
						}
					} else {
						rank->pos += to + 1;
						if (!value) {
							if (from > to) {
								from -= to;
							} else {
								amt += to + 1 -
								    from;
								goto done;
							}
						} else {
							goto done;
						}
					}
					break;

				case SM_PAYLOAD_ONES:
					rank->rem = UINT64_MAX;
					if (to >= SM_BITS_PER_VECTOR) {
						rank->pos += SM_BITS_PER_VECTOR;
						to -= SM_BITS_PER_VECTOR;
						if (from >=
						    SM_BITS_PER_VECTOR) {
							from = from -
							    SM_BITS_PER_VECTOR;
						} else {
							if (value) {
								amt +=
								    SM_BITS_PER_VECTOR -
								    from;
							}
							from = 0;
						}
					} else {
						rank->pos += to + 1;
						if (value) {
							if (from > to) {
								from =
								    from - to;
							} else {
								amt += to + 1 -
								    from;
								goto done;
							}
						} else {
							goto done;
						}
					}
					break;

				case SM_PAYLOAD_MIXED:
					w = chunk->m_data[1 +
					    __sm_chunk_get_position(chunk,
					        (i * SM_FLAGS_PER_INDEX_BYTE) +
					            (size_t)j)];
					if (to >= SM_BITS_PER_VECTOR) {
						rank->pos += SM_BITS_PER_VECTOR;
						to -= SM_BITS_PER_VECTOR;
						mask = from == 0 ?
						    UINT64_MAX :
						    ~(UINT64_MAX >>
						        (SM_BITS_PER_VECTOR -
						            (from >= 64 ?
						                    64 :
						                    from)));
						mw = (value ? w : ~w) & mask;
						pc = (size_t)SM_POPCOUNT64(mw);
						amt += pc;
						from =
						    from > SM_BITS_PER_VECTOR ?
						    from - SM_BITS_PER_VECTOR :
						    0;
					} else {
						uint64_t to_mask;
						uint64_t from_mask;
						rank->pos += to + 1;
						to_mask = (to == 63) ?
						    UINT64_MAX :
						    ((uint64_t)1 << (to + 1)) -
						        1;
						from_mask = from == 0 ?
						    UINT64_MAX :
						    ~(UINT64_MAX >>
						        (SM_BITS_PER_VECTOR -
						            (from >= 64 ?
						                    64 :
						                    from)));
						/* Create a mask for the range [from, to] and use popcount. */
						mask = to_mask & from_mask;
						mw = (value ? w : ~w) & mask;
						pc = (size_t)SM_POPCOUNT64(mw);
						amt += pc;
						rank->rem = mw >>
						    (from > 63 ? 63 : from);
						goto done;
					}
					break;

				case SM_PAYLOAD_NONE:
				default:
					continue;
				}
			}
		}
	}
done:;
	return (amt);
}

/**
 * @brief Scans a chunk allowing the callee to process each vector.
 *
 * This function iterates through a chunk's data and processes these
 * payloads using the provided scanner function.
 *
 * @param[in] chunk The chunk to scan.
 * @param[in] start The starting index for the scan.
 * @param[in] scanner The callback function to process discovered vectors.
 * @param[in] skip The number of vectors to skip before processing.
 * @param[in] aux Auxiliary data to pass to the scanner function.
 * @return The total number of processed vectors.
 */
static size_t
__sm_chunk_scan(const __sm_chunk_t *chunk, const __sm_idx_t start,
    void (*scanner)(uint64_t[], size_t, void *aux), size_t skip, void *aux)
{
	size_t i;
	/* Sparse encoding path.
	 * 'pos' tracks the bit offset within the chunk (each vector = SM_BITS_PER_VECTOR).
	 * 'skip' counts set bits remaining to skip before scanning.
	 * Returns the number of set bits skipped in this chunk. */
	size_t pos = 0;
	size_t skipped = 0;
	uint64_t buffer[SM_BITS_PER_VECTOR];
	const __sm_bitvec_t scan_desc = *chunk->m_data;
	for (i = 0; i < sizeof(__sm_bitvec_t); i++) {
		int j;
		const uint8_t b = __sm_desc_flag_byte(scan_desc, i);
		if (b == 0) {
			/* All 4 flag slots in this byte are ZEROS -- no set bits, advance position. */
			pos += SM_FLAGS_PER_INDEX_BYTE * SM_BITS_PER_VECTOR;
			continue;
		}

		for (j = 0; j < SM_FLAGS_PER_INDEX_BYTE; j++) {
			const size_t flags = SM_CHUNK_GET_FLAGS(b, j);
			if (flags == SM_PAYLOAD_NONE) {
				/* No capacity in this slot, do not advance position. */
			} else if (flags == SM_PAYLOAD_ZEROS) {
				/* All zeroes -- no set bits to skip or scan. */
				pos += SM_BITS_PER_VECTOR;
			} else if (flags == SM_PAYLOAD_ONES) {
				if (skip >= SM_BITS_PER_VECTOR) {
					skip -= SM_BITS_PER_VECTOR;
					skipped += SM_BITS_PER_VECTOR;
					pos += SM_BITS_PER_VECTOR;
				} else if (skip > 0) {
					size_t b;
					size_t n = 0;
					for (b = skip;
					     b < SM_BITS_PER_VECTOR; b++) {
						buffer[n++] = start + pos + b;
					}
					skipped += skip;
					skip = 0;
					scanner(&buffer[0], n, aux);
					pos += SM_BITS_PER_VECTOR;
				} else {
					size_t b;
					for (b = 0;
					     b < SM_BITS_PER_VECTOR; b++) {
						buffer[b] = start + pos + b;
					}
					scanner(&buffer[0], SM_BITS_PER_VECTOR,
					    aux);
					pos += SM_BITS_PER_VECTOR;
				}
			} else if (flags == SM_PAYLOAD_MIXED) {
				__sm_bitvec_t remaining = chunk->m_data[1 +
				    __sm_chunk_get_position(chunk,
				        (i * SM_FLAGS_PER_INDEX_BYTE) + (size_t)j)];
				size_t n = 0;
				while (remaining) {
					int b = SM_CTZ64(remaining);
					if (skip > 0) {
						skip--;
						skipped++;
					} else {
						buffer[n++] = start + pos + (size_t)b;
					}
					remaining &= remaining -
					    1; /* clear lowest set bit */
				}
				if (n > 0) {
					scanner(&buffer[0], n, aux);
				}
				pos += SM_BITS_PER_VECTOR;
			}
		}
	}
	return (skipped);
}

/* -------------------------------------------------------------------
 * Map structure: chunk navigation, the tail cursor, and the
 * byte-level insert/remove/coalesce primitives
 * ------------------------------------------------------------------- */

/**
 * @brief Retrieves the count of chunks in the sparse map.
 *
 * This function reads the first 32-bit integer from the `m_data` array
 * of the given sparse map to determine and return the number of chunks.
 *
 * @param[in] map The sparse map from which to retrieve the chunk count.
 * @return The number of chunks in the sparse map.
 */
static size_t
__sm_get_chunk_count(const sm_t *map)
{
	/*
	 * The chunk-count slot lives in the first SM_SIZEOF_OVERHEAD bytes of
	 * m_data.  When m_data_used == 0 the slot has not been initialized
	 * (e.g. a freshly sm_wrap'd buffer that has not yet been
	 * sm_clear'd or sm_open'd), so reading it would return
	 * whatever happened to be in the caller's buffer.
	 *
	 * Pre-fix, downstream loops in sm_intersection / _union /
	 * _maximum / __sm_rank_vec walked off the end of the buffer when
	 * the slot held garbage; pg_tre carried four "BUG FIX: m_data_used
	 * = 0 but garbage chunk count" patches at every call site.  The
	 * canonical fix is here: an uninitialized chunk-count slot
	 * means "no chunks", full stop.
	 */
	if (map->m_data_used < SM_SIZEOF_OVERHEAD) {
		return (0);
	}
	return ((size_t)__sm_load_u64(&map->m_data[0]));
}

/**
 * @brief Retrieves a pointer to the data at the specified offset within the sparse map.
 *
 * This function calculates the address of the data starting after a predefined
 * overhead and adds the provided offset to this start point. The resulting
 * pointer points to the actual data within the sparse map.
 *
 * @param[in] map A pointer to the sparse map.
 * @param[in] offset The offset within the sparse map where the data starts.
 * @return A pointer to the data at the specified offset within the sparse map.
 */
static uint8_t *
__sm_get_chunk_data(const sm_t *map, const size_t offset)
{
	return (&map->m_data[SM_SIZEOF_OVERHEAD + offset]);
}

/**
 * @brief Computes the end pointer of the chunk data in the sparse map.
 *
 * This function calculates the end of the chunk data by iterating through all
 * the chunks present in the sparse map, taking into account the overhead size
 * and the size of each chunk.
 *
 * @param[in] map The sparse map whose chunk end pointer needs to be calculated.
 * @return A pointer to the end of the chunk data in the sparse map.
 */
static uint8_t *
__sm_get_chunk_end(const sm_t *map)
{
	size_t i;
	uint8_t *p = __sm_get_chunk_data(map, 0);
	const size_t count = __sm_get_chunk_count(map);
	for (i = 0; i < count; i++) {
		__sm_chunk_t chunk;
		size_t chunk_size;
		p += SM_SIZEOF_OVERHEAD;
		__sm_chunk_init(&chunk, p);
		chunk_size = __sm_chunk_get_size(&chunk);
		if (i + 1 < count) {
			SM_PREFETCH(p + chunk_size + SM_SIZEOF_OVERHEAD);
		}
		p += chunk_size;
	}
	return (p);
}

/**
 * @brief Computes the aligned offset for a given index based on chunk capacity.
 *
 * This function calculates the offset for the provided index such that
 * it aligns with the chunk boundaries defined by the maximum chunk capacity.
 *
 * @param[in] idx The index for which the aligned offset is to be computed.
 * @return The aligned offset corresponding to the given index.
 */
static __sm_idx_t
__sm_get_chunk_aligned_offset(const uint64_t idx)
{
	const uint64_t capacity = SM_CHUNK_MAX_CAPACITY;
	return (idx / capacity * capacity);
}

/**
 * @brief Calculates the total size of the sparse map's used data.
 *
 * This function iterates through each chunk in the sparse map and computes
 * the total memory used by the map, including overhead.
 *
 * @param[in] map Pointer to the sparse map.
 * @return Total size of the used data in the sparse map.
 *
 * Bounds-safe: when called on a possibly-corrupt buffer (after
 * sm_open) the walker validates each chunk against m_capacity and
 * truncates the on-disk chunk count if any chunk would extend past
 * the buffer.  The returned size therefore corresponds to the
 * largest valid chunk-stream prefix; if the input is
 * well-formed, behavior is unchanged.
 */
static void __sm_set_chunk_count(const sm_t *map, size_t new_count);

static size_t
__sm_get_size_impl(const sm_t *map)
{
	size_t count;
	size_t valid_count;
	size_t i;
	uint8_t *start = __sm_get_chunk_data(map, 0);
	uint8_t *p = start;
	uint8_t *end = map->m_data + __sm_cap(map);

	/* Defensive: a chunk-data start outside the data buffer means the
	 * map header itself is corrupt.  Return the empty-map size. */
	if (start < map->m_data || start > end) {
		return (SM_SIZEOF_OVERHEAD);
	}

	count = __sm_get_chunk_count(map);
	valid_count = 0;
	for (i = 0; i < count; i++) {
		__sm_chunk_t chunk;
		size_t chunk_size;
		/* Each chunk needs at least SM_SIZEOF_OVERHEAD bytes for its
		 * aligned-offset prefix plus sizeof(__sm_bitvec_t) bytes for the
		 * mandatory chunk header word.  If less remains, the on-disk
		 * count is bogus. */
		if ((size_t)(end - p) <
		    SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t)) {
			break;
		}
		p += SM_SIZEOF_OVERHEAD;
		__sm_chunk_init(&chunk, p);
		chunk_size = __sm_chunk_get_size(&chunk);
		/* __sm_chunk_get_size returns at minimum sizeof(__sm_bitvec_t).
		 * A chunk that claims to extend past `end` indicates corrupt
		 * flags; stop walking. */
		if (chunk_size < sizeof(__sm_bitvec_t) ||
		    (size_t)(end - p) < chunk_size) {
			/* Roll back the SM_SIZEOF_OVERHEAD we just advanced; we want
			 * to report the size up to the last *complete* chunk. */
			p -= SM_SIZEOF_OVERHEAD;
			break;
		}
		if (i + 1 < count) {
			SM_PREFETCH(p + chunk_size + SM_SIZEOF_OVERHEAD);
		}
		p += chunk_size;
		valid_count++;
	}

	/* If the walker truncated, fix up the on-disk chunk count so
	 * subsequent operations see only the valid prefix.  This is the
	 * only place we mutate the map during what is logically a
	 * read; the const cast is intentional and the mutation is safe
	 * (we're correcting attacker-controlled corruption to a
	 * consistent, harmless state). */
	if (valid_count != count) {
		__sm_set_chunk_count((sm_t *)map, valid_count);
	}
	return (SM_SIZEOF_OVERHEAD + (size_t)(p - start));
}

/**
 * @brief Retrieves the offset of a specified chunk within the sparse map.
 *
 * This function iterates through the chunks in the sparse map to find the
 * offset of the chunk that either contains or would logically contain the
 * given index.
 *
 * @param[in] map The sparse map to search within.
 * @param[in] idx The index to find the corresponding chunk offset for.
 * @param[in,out] cur Optional caller-owned cursor; NULL = no acceleration.
 * @return The offset of the chunk if found, otherwise -1 if no appropriate chunk is found.
 *
 * Read-cursor optimization:
 *
 * The naive implementation walks from chunk 0 on every call.  For a
 * map with N chunks this is O(N) per lookup, so a sequence of N
 * ascending lookups is O(N^2).  When the caller threads a cursor
 * (see sm_cursor_t) the walk resumes from the most-recently-located
 * chunk whenever the new idx is at or after that chunk's start,
 * making an ascending sequence O(N) overall.  A lookup that falls in
 * the gap before a chunk caches that chunk's left neighbour instead
 * (see the end of the walk), so the next ascending lookup in the same
 * gap still resumes.  Passing NULL (every mutator does) walks from
 * chunk 0.
 *
 * The walk starts only at chunk 0 or at cur->offset, and only after
 * the start index stored at cur->offset matches cur->start_idx.
 * cur->prev_offset is never a walk start: it is a hint for the
 * coalescing path, which validates it before use.
 *
 * The cursor is caller-owned and purely an in-memory speedup; the
 * on-disk format is unchanged.  ANY mutation of the map invalidates
 * the caller's cursor (the caller must reset it); the library no
 * longer tracks cursor validity in the struct.
 */
static ssize_t
__sm_get_chunk_offset(const sm_t *map, const uint64_t idx, sm_cursor_t *cur)
{
	uint8_t * base;
	uint8_t * p;
	size_t stream_end;
	size_t prev_off;
	size_t prev2_off;
	const size_t count = __sm_get_chunk_count(map);

	if (count == 0) {
		return (-1);
	}

	base = __sm_get_chunk_data(map, 0);
	p = base;
	/* Offsets returned here are relative to `base` (the first chunk);
	 * m_data_used is relative to m_data and includes the
	 * SM_SIZEOF_OVERHEAD chunk-count header, so the chunk stream
	 * occupies [0, stream_end) in base-relative offsets.  Bounding the
	 * walk by stream_end is correct no matter where we resume from
	 * (unlike an ordinal count, which would over-run when resuming
	 * partway through the chunk list). */
	stream_end = (size_t)map->m_data_used - SM_SIZEOF_OVERHEAD;

	/* Byte offset (base-relative) of the chunk immediately BEFORE the
	 * chunk we finally return, or SIZE_MAX if none.  Captured for free
	 * during the forward walk and handed back to the caller so the
	 * coalescing path can find the left neighbor without a head-walk. */
	prev_off = SIZE_MAX;
	/* The chunk before prev_off, when the walk itself passed it (else
	 * SIZE_MAX): recorded when a gap lookup caches prev_off. */
	prev2_off = SIZE_MAX;

	/*
	 * Cursor fast-path.  If the caller passed a valid cursor whose
	 * cached chunk starts at or before idx, resume the walk from the
	 * cached byte offset instead of from the head.  Otherwise walk from
	 * chunk 0.  We never index chunks by ordinal here, so a relocated
	 * buffer is fine as long as the offset is still in range.
	 */
	if (cur != NULL && cur->offset != SIZE_MAX &&
	    cur->offset + sizeof(__sm_idx_t) <= stream_end &&
	    idx >= cur->start_idx) {
		/* Check the cached offset before trusting it: a prior mutation
		 * (a chunk being rewritten, a leftward coalesce, a separate)
		 * can shift or remove the cached chunk without the caller
		 * resetting.  If the word at the cached offset is not the start
		 * we recorded, the cursor is stale -- walk from the head.  This
		 * catches a shifted chunk, not every stale cursor: payload words
		 * can equal the recorded start.  Reset a cursor after mutating
		 * the map (sm_cursor_t). */
		const __sm_idx_t at = __sm_load_idx(base + cur->offset);
		if (at == cur->start_idx) {
			p = base + cur->offset;
			/* A resume that lands in this same chunk (no loop
			 * advance below) must still carry the left-neighbor
			 * hint the caller cached, or it would be lost. */
			prev_off = cur->prev_offset;
		}
	}

	/* Walk to the chunk that contains idx, or the last chunk if idx is
	 * past the end. */
	for (;;) {
		size_t next_off;
		const __sm_idx_t s = __sm_load_idx((const uint8_t *)p);
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		__sm_assert(s == __sm_get_chunk_aligned_offset(s));
		next_off = (size_t)(p - base) +
		    SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		if (idx >= __sm_chunk_window_end(s) &&
		    next_off < stream_end) {
			prev2_off = prev_off;
			prev_off = (size_t)(p - base);
			p = base + next_off;
			continue;
		}
		if (cur != NULL) {
			if (idx < s && prev_off != SIZE_MAX) {
				/* idx is in the gap before this chunk.  Caching
				 * this chunk would make the next ascending lookup
				 * in the same gap (idx < its start) walk from the
				 * head: O(lookups x chunks) over a sparse map (found
				 * by pg_fts).  Cache the left neighbour, which this
				 * walk just stepped over, so that lookup resumes
				 * there and steps once.  Its own neighbour is
				 * prev2_off when the walk passed it, else unknown.
				 * The return value is unchanged.  (5.8.4 instead
				 * resumed from cur->prev_offset, which nothing
				 * verified was a chunk start.) */
				cur->offset = prev_off;
				cur->start_idx =
				    __sm_load_idx(base + prev_off);
				cur->prev_offset = prev2_off;
			} else {
				cur->offset = (size_t)(p - base);
				cur->start_idx = s;
				cur->prev_offset = prev_off;
			}
		}
		return (p - base);
	}
}

/**
 * @brief Sets the chunk count for the sparsemap to a new value.
 *
 * This function updates the chunk count stored in the map's data array
 * to the specified new count.
 *
 * @param[in,out] map The sparsemap in which to set the chunk count.
 * @param[in] new_count The new chunk count to set.
 */
static void
__sm_set_chunk_count(const sm_t *map, const size_t new_count)
{
	__sm_store_u64((uint8_t *)&map->m_data[0], (uint64_t)new_count);
}

/* -------------------------------------------------------------------
 * Small-set mode
 *
 * When every set index is below SM_SMALL_MAX_BITS, the map stores a bare
 * uint64 bitmapword[] from bit 0 (bit i in word i/64) behind an 8-byte
 * header, so a near-zero set pays no per-chunk start or descriptor.
 * Chunk mode is smaller once indices spread.
 *
 * The mode is self-describing in the m_data byte stream: the same
 * 8-byte header word that holds the chunk count in chunk mode has its
 * top bit (SM_SMALL_FLAG) set in small mode, with the bitmapword count
 * in the low 32 bits.  A chunk count never approaches 2^63, so the top
 * bit is free.  __sm_get_chunk_count / the chunk walkers must never run
 * on a small map; callers gate on __sm_is_small() first.
 *
 * Layout (m_data):  [ SM_SMALL_FLAG | nwords : 8 bytes ][ word[0..nwords) ]
 * m_data_used = SM_SIZEOF_OVERHEAD + nwords * 8.
 * ------------------------------------------------------------------- */

#define SM_SMALL_FLAG   ((uint64_t)1 << 63)
#define SM_SMALL_WMASK  (((uint64_t)1 << 32) - 1)
/* Hard span cap: at 16 words the small form is at most 8 + 16*8 = 136
 * bytes.  It is preferred over the chunk form whenever it is not larger
 * and fits in place; in a tightly-sized buffer a growth-to-shrink is
 * skipped, so the small form can occasionally sit up to one word larger
 * than the chunk equivalent.  Above this cap the map is always in chunk
 * mode. */
#define SM_SMALL_MAX_WORDS 16u
#define SM_SMALL_MAX_BITS  (SM_SMALL_MAX_WORDS * 64u)

static inline void
__sm_cursor_reset(sm_cursor_t *c)
{
	c->offset = (size_t)-1;
	c->start_idx = 0;
	c->prev_offset = (size_t)-1;
}

static inline bool
__sm_is_small(const sm_t *map)
{
	if (map == NULL || map->m_data == NULL ||
	    map->m_data_used < SM_SIZEOF_OVERHEAD) {
		return (false);
	}
	return ((__sm_load_u64(&map->m_data[0]) & SM_SMALL_FLAG) != 0);
}

static inline size_t
__sm_small_nwords(const sm_t *map)
{
	return ((size_t)(__sm_load_u64(&map->m_data[0]) & SM_SMALL_WMASK));
}

/*
 * The small-set word array starts 8 bytes into m_data.  m_data is
 * 8-aligned for library-owned maps, but sm_wrap/sm_init/sm_open accept
 * a caller buffer that may be arbitrarily aligned (sm.h documents that
 * this works, with a speed cost on some cpus).  So the words are typed
 * __sm_bitvec_unaligned_t -- like every chunk descriptor -- and the
 * compiler emits alignment-safe loads/stores instead of trapping on a
 * strict-alignment cpu or tripping UBSan.  Same codegen on x86_64 and
 * aarch64; a plain `uint64_t *` here was undefined behaviour on a
 * misaligned buffer.
 */
static inline __sm_bitvec_unaligned_t *
__sm_small_words(const sm_t *map)
{
	return ((__sm_bitvec_unaligned_t *)(void *)&map->m_data[
	    SM_SIZEOF_OVERHEAD]);
}

static inline void
__sm_small_set_header(sm_t *map, size_t nwords)
{
	__sm_store_u64((uint8_t *)&map->m_data[0],
	    SM_SMALL_FLAG | (uint64_t)nwords);
}


/* True if idx is set in a small-mode map. */
static bool
__sm_small_contains(const sm_t *map, uint64_t idx)
{
	const size_t w = (size_t)(idx / 64);
	if (w >= __sm_small_nwords(map)) {
		return (false);
	}
	return ((__sm_small_words(map)[w] >> (idx % 64)) & 1u);
}

/* Population count of a small-mode map. */
static uint64_t
__sm_small_cardinality(const sm_t *map)
{
	size_t i;
	const __sm_bitvec_unaligned_t *w = __sm_small_words(map);
	const size_t n = __sm_small_nwords(map);
	uint64_t c = 0;
	for (i = 0; i < n; i++) {
		c += (uint64_t)SM_POPCOUNT64(w[i]);
	}
	return (c);
}

static bool
__sm_small_is_empty(const sm_t *map)
{
	size_t i;
	const __sm_bitvec_unaligned_t *w = __sm_small_words(map);
	const size_t n = __sm_small_nwords(map);
	for (i = 0; i < n; i++) {
		if (w[i] != 0) {
			return (false);
		}
	}
	return (true);
}

static uint64_t
__sm_small_minimum(const sm_t *map)
{
	size_t i;
	const __sm_bitvec_unaligned_t *w = __sm_small_words(map);
	const size_t n = __sm_small_nwords(map);
	for (i = 0; i < n; i++) {
		if (w[i] != 0) {
			return ((uint64_t)i * 64 + (uint64_t)SM_CTZ64(w[i]));
		}
	}
	return (0);
}

static uint64_t
__sm_small_maximum(const sm_t *map)
{
	size_t i;
	const __sm_bitvec_unaligned_t *w = __sm_small_words(map);
	const size_t n = __sm_small_nwords(map);
	for (i = n; i-- > 0;) {
		if (w[i] != 0) {
			return ((uint64_t)i * 64 + (63 -
			    (uint64_t)SM_CLZ64(w[i])));
		}
	}
	return (0);
}

/*
 * Number of set bits of a small-mode map in the inclusive range
 * [begin, end].  NB: the caller guarantees begin <= end.
 */
static size_t
__sm_small_rank1(const sm_t *map, uint64_t begin, uint64_t end)
{
	const __sm_bitvec_unaligned_t *w = __sm_small_words(map);
	const uint64_t nbits =
	    (uint64_t)__sm_small_nwords(map) * SM_BITS_PER_VECTOR;
	size_t first;
	size_t last;
	size_t count = 0;
	size_t i;

	if (begin >= nbits) {
		return (0);
	}
	if (end >= nbits) {
		end = nbits - 1;
	}
	/* Mask the first and last word to the range, popcount the rest. */
	first = (size_t)(begin / SM_BITS_PER_VECTOR);
	last = (size_t)(end / SM_BITS_PER_VECTOR);
	for (i = first; i <= last; i++) {
		__sm_bitvec_t x = w[i];
		if (i == first) {
			x &= ~(__sm_bitvec_t)0 << (begin % SM_BITS_PER_VECTOR);
		}
		if (i == last) {
			x &= ~(__sm_bitvec_t)0 >>
			    (SM_BITS_PER_VECTOR - 1 - end % SM_BITS_PER_VECTOR);
		}
		count += (size_t)SM_POPCOUNT64(x);
	}
	return (count);
}

/*
 * sm_select on a small-mode map: the position of the n-th (0-based) bit
 * equal to `value` in [0, 2^64 - 1), or SM_IDX_MAX if there is none.
 * Whole words are skipped by popcount and the target word is searched
 * with __sm_word_select.  Every bit past the last word is unset.
 */
static uint64_t
__sm_small_select(const sm_t *map, uint64_t n, bool value)
{
	const __sm_bitvec_unaligned_t *w = __sm_small_words(map);
	const size_t nw = __sm_small_nwords(map);
	const uint64_t nbits = (uint64_t)nw * SM_BITS_PER_VECTOR;
	size_t i;

	for (i = 0; i < nw; i++) {
		const __sm_bitvec_t x = value ? w[i] : ~w[i];
		const uint64_t pc = (uint64_t)SM_POPCOUNT64(x);
		if (n < pc) {
			return ((uint64_t)i * SM_BITS_PER_VECTOR +
			    __sm_word_select(x, (size_t)n));
		}
		n -= pc;
	}
	if (value || n >= SM_IDX_MAX - nbits) {
		return (SM_IDX_MAX);
	}
	return (nbits + n);
}

/*
 * The first position >= x whose bit equals `value` in a small-mode map,
 * or SM_IDX_MAX if there is none.  An unset bit always exists below
 * SM_IDX_MAX past the last word, so for value == false SM_IDX_MAX
 * comes back only for x == SM_IDX_MAX.
 */
static uint64_t
__sm_small_next(const sm_t *map, uint64_t x, bool value)
{
	const __sm_bitvec_unaligned_t *w = __sm_small_words(map);
	const size_t nw = __sm_small_nwords(map);
	const uint64_t nbits = (uint64_t)nw * SM_BITS_PER_VECTOR;
	size_t i;

	if (x >= nbits) {
		return ((value || x == SM_IDX_MAX) ? SM_IDX_MAX : x);
	}
	for (i = (size_t)(x / SM_BITS_PER_VECTOR); i < nw; i++) {
		__sm_bitvec_t b = value ? w[i] : ~w[i];
		if (i == (size_t)(x / SM_BITS_PER_VECTOR)) {
			b &= ~(__sm_bitvec_t)0 << (x % SM_BITS_PER_VECTOR);
		}
		if (b != 0) {
			return ((uint64_t)i * SM_BITS_PER_VECTOR +
			    (uint64_t)SM_CTZ64(b));
		}
	}
	return (value ? SM_IDX_MAX : nbits);
}

/*
 * sm_span on a small-mode map: the first position p >= idx such that
 * the len bits [p, p + len - 1] all equal `value` (len 0 and 1 both
 * accept the first matching bit), or SM_IDX_MAX if there is none.  It
 * steps from stretch to stretch: a stretch of `value` starts at p and
 * ends where the opposite value next appears.  The unset stretch past
 * the last member never ends, so it holds len bits whenever
 * p + len - 1 does not overflow, the same bound the chunk path's rank
 * check applies.
 */
static uint64_t
__sm_small_span(const sm_t *map, uint64_t idx, size_t len, bool value)
{
	uint64_t p = __sm_small_next(map, idx, value);

	while (p != SM_IDX_MAX) {
		const uint64_t q = __sm_small_next(map, p, !value);
		if (q == SM_IDX_MAX) {
			if (len == 0 || (uint64_t)len - 1 <= UINT64_MAX - p) {
				return (p);
			}
			return (SM_IDX_MAX);
		}
		if (q - p >= (uint64_t)len) {
			return (p);
		}
		p = __sm_small_next(map, q, value);
	}
	return (SM_IDX_MAX);
}

/*
 * sm_scan on a small-mode map: deliver every set bit after the first
 * `skip`, in ascending order, one batch per word that has any left.
 */
static void
__sm_small_scan(const sm_t *map,
    void (*scanner)(uint64_t[], size_t, void *aux), size_t skip, void *aux)
{
	const __sm_bitvec_unaligned_t *w = __sm_small_words(map);
	const size_t nw = __sm_small_nwords(map);
	uint64_t buffer[SM_BITS_PER_VECTOR];
	size_t i;

	for (i = 0; i < nw; i++) {
		__sm_bitvec_t b = w[i];
		const size_t pc = (size_t)SM_POPCOUNT64(b);
		size_t n = 0;
		if (skip >= pc) {
			skip -= pc;
			continue;
		}
		/* Fewer than 64 skipped bits remain in this word. */
		while (skip > 0) {
			b &= b - 1;
			skip--;
		}
		while (b != 0) {
			buffer[n++] = (uint64_t)i * SM_BITS_PER_VECTOR +
			    (uint64_t)SM_CTZ64(b);
			b &= b - 1;
		}
		scanner(&buffer[0], n, aux);
	}
}

/* Materialize a small-mode map into a freshly allocated chunk-mode map
 * (adding each set bit).  Returns NULL on allocation failure.  The
 * caller owns the result and disposes it with sm_free().  Used to give
 * the chunk-walking read/algebra paths a uniform view of a small map
 * without mutating the (const) input. */
static sm_t *__sm_materialize(const sm_t *small);


/**
 * @brief Appends data to the sparsemap's internal buffer.
 *
 * Copies @a buffer_size bytes to the end of the map's data region and
 * advances m_data_used.
 *
 * The caller must have established capacity first, because there is no
 * single correct response to "it does not fit" at this level: the
 * library-owned result maps in sm_union() and friends grow (see
 * __sm_ensure_capacity(), which may reallocate and reassign the
 * caller's pointer), while operations on a caller-supplied buffer must
 * instead fail with errno=ENOSPC (see the SM_ENOUGH_SPACE() macro).
 * A callee cannot pick between grow-and-continue and fail-fast, so the
 * policy stays with the caller.
 *
 * What this function *can* do is refuse to perform an overflowing copy
 * and say so.  It previously returned void and only noted the problem
 * through __sm_assert(), which expands to ((void)0) unless
 * SPARSEMAP_DIAGNOSTIC is defined -- so in a release build a caller
 * that forgot its capacity check got a silent heap overflow instead of
 * a diagnostic.  sm_split() did forget, and the corruption surfaced
 * much later as an unrelated glibc "realloc(): invalid next size".
 * Returning bool makes the compiler point at any caller that does not
 * check (warn_unused_result), which is the property the assert was
 * standing in for.
 *
 * @param[in,out] map          Map whose buffer is appended to.
 * @param[in]     buffer       Bytes to append.
 * @param[in]     buffer_size  Number of bytes to append.
 * @return true on success; false without copying anything if the bytes
 *         would not fit, which indicates a missing caller-side check.
 */
SM_WARN_UNUSED static bool
__sm_append_data(sm_t *map, const uint8_t *buffer, const size_t buffer_size)
{
	if (SM_UNLIKELY(map->m_data_used + buffer_size > __sm_cap(map))) {
		__sm_assert(map->m_data_used + buffer_size <= __sm_cap(map));
		errno = ENOSPC;
		return (false);
	}

	memcpy(&map->m_data[map->m_data_used], buffer, buffer_size);
	map->m_data_used += buffer_size;
	return (true);
}

/**
 * @brief Inserts data into the sparse map at the specified offset.
 *
 * This function asserts that there is enough capacity in the map to accommodate
 * the new data, retrieves the appropriate chunk of data from the map, and then
 * inserts the provided buffer at the given offset. The existing data is moved
 * to make space for the new data, and the map's used data size is updated accordingly.
 *
 * @param[in,out] map Pointer to the sparse map where data will be inserted.
 * @param[in] offset Offset in the map where the data should be inserted.
 * @param[in] buffer Pointer to the buffer containing the data to be inserted.
 * @param[in] buffer_size Size of the buffer in bytes.
 */
static void
__sm_insert_data(sm_t *map, const size_t offset, const uint8_t *buffer,
    const size_t buffer_size)
{
	uint8_t * p;
	__sm_assert(map->m_data_used + buffer_size <= __sm_cap(map));
	__sm_assert(offset <= map->m_data_used);

	p = __sm_get_chunk_data(map, offset);
	memmove(p + buffer_size, p, map->m_data_used - offset);
	memcpy(p, buffer, buffer_size);
	map->m_data_used += buffer_size;
}

/**
 * @brief Removes a contiguous block of data from the sparsemap.
 *
 * This function removes a block of data from the sparsemap at the specified offset
 * and reduces the size of the data used accordingly.
 *
 * @param[in,out] map A pointer to the sparsemap from which data will be removed.
 * @param[in] offset The starting position of the block to be removed.
 * @param[in] gap_size The size of the block to be removed.
 */
static void
__sm_remove_data(sm_t *map, const size_t offset, const size_t gap_size)
{
	uint8_t * p;
	size_t len;
	__sm_assert(map->m_data_used >= gap_size);
	p = __sm_get_chunk_data(map, offset);
	/* `offset` is chunk-data-relative; whether m_data_used counts the
	 * header depends on how the map was opened (see SM_ENOUGH_SPACE).
	 * When it does, this length runs SM_SIZEOF_OVERHEAD past the used
	 * end -- past the allocation if the buffer is exactly full.  The
	 * mainline build reached that through its chunk-merging pass
	 * (5.8.0-5.8.3); this build has no such pass and every caller
	 * reserves slack first, but the clamp keeps the helper safe on its
	 * own.  It never cuts real data in either convention: that all lies
	 * within capacity. */
	len = map->m_data_used - offset - gap_size;
	if (SM_SIZEOF_OVERHEAD + offset + gap_size + len > __sm_cap(map))
		len = __sm_cap(map) - SM_SIZEOF_OVERHEAD - offset - gap_size;
	memmove(p, p + gap_size, len);
	map->m_data_used -= gap_size;
}

/**
 * @brief Coalesces the specified chunk with adjacent chunks if conditions are met.
 *
 * This function attempts to merge the provided chunk with its adjacent chunks
 * in a sparse map if they meet certain conditions. The goal is to reduce the
 * number of chunks by combining adjacent ones that form continuous runs.
 *
 * @param[in] map The sparse map that contains the chunk.
 * @param[in] chunk The chunk to be potentially coalesced.
 * @param[in] offset The offset of the chunk in the sparse map.
 * @param[in] start The starting index of the chunk.
 * @param[in,out] p Pointer to the chunk's data.
 * @return The number of chunks that were removed during the coalescing process.
 */
static int
__sm_coalesce_chunk(sm_t *map, __sm_chunk_t *chunk, size_t offset,
    __sm_idx_t start, uint8_t *p, uint64_t idx, bool is_set_op,
    size_t left_hint)
{
	/*
	 * This build stores runs as a stretch of adjacent all-ONES sparse
	 * chunks, which is already a valid, fully general representation --
	 * there is nothing to merge, so this is a deliberate no-op.  The
	 * signature is kept so the many call sites (set/unset/merge/split)
	 * stay unchanged.
	 */
	(void)map;
	(void)chunk;
	(void)offset;
	(void)start;
	(void)p;
	(void)idx;
	(void)is_set_op;
	(void)left_hint;
	return (0);
}

/**
 * @brief Coalesces adjacent chunks in a sparse map (deliberate no-op).
 *
 * Adjacent all-ONES sparse chunks are already a valid, fully general
 * representation of a run, so there is nothing to merge.  Kept as a
 * no-op so every call site is unchanged.
 *
 * @param[in] map The sparse map to coalesce.
 * @return Always 0 (no bytes coalesced).
 */
static size_t
__sm_coalesce_map(sm_t *map)
{
	(void)map;
	return (0);
}

/* -------------------------------------------------------------------
 * Lifecycle: construction, copy, disposal, and buffer resize
 * ------------------------------------------------------------------- */

/**
 * @brief Clears the given sparse map.
 *
 * This function resets the sparse map by setting all its data to zero and updating
 * its metadata to reflect an empty map.
 *
 * @param[in] map The sparse map to clear.
 */
void
sm_clear(sm_t *map)
{
	if (map == NULL) {
		return;
	}
	__sm_card_invalidate(map);
	/* A buffer too short for the chunk-count header (0..7 bytes, or
	 * NULL) is the empty map with m_data_used == 0: write nothing. */
	if (SM_UNLIKELY(__sm_cap(map) < SM_SIZEOF_OVERHEAD)) {
		map->m_data_used = 0;
		return;
	}
	memset(map->m_data, 0, __sm_cap(map));
	map->m_data_used = SM_SIZEOF_OVERHEAD;
	__sm_set_chunk_count(map, 0);
}

/**
 * @brief Allocates and initializes a sparsemap of the given size.
 *
 * This function creates a new sparsemap structure with allocated memory.
 * If the specified size is zero, a default size of 1024 is used. The function
 * ensures that the internal data array is 8-byte aligned and initializes the sparsemap
 * structure.
 *
 * @param[in] size The size of the sparsemap to allocate.
 * @return A pointer to the allocated sparsemap structure, or NULL if allocation fails.
 */
sm_t *
sparsemap(size_t size)
{
	return (sm_create(size));
}

/**
 * @brief Allocates and initializes a sparsemap of the given size.
 *
 * This function creates a new sparsemap structure with allocated memory.
 * If the specified size is zero, a default size of 1024 is used. The function
 * ensures that the internal data array is 8-byte aligned and initializes the sparsemap
 * structure.
 *
 * @param[in] size The size of the sparsemap to allocate.
 * @return A pointer to the allocated sparsemap structure, or NULL if allocation fails.
 */
sm_t *
sm_create(size_t size)
{
	size_t data_size;
	size_t total_size;
	size_t padding;
	sm_t * map;
	if (size == 0) {
		size = 1024;
	}

	/* Round up to an 8-byte boundary so the data region we allocate
	 * and the (low-bit-tagged) stored capacity agree exactly. */
	size = (size + 7u) & ~(size_t)7;

	data_size = size * sizeof(uint8_t);

	/* Ensure that m_data is 8-byte aligned. */
	total_size = sizeof(sm_t) + data_size;
	padding = total_size % 8 == 0 ? 0 : 8 - (total_size % 8);
	total_size += padding;

	map = (sm_t *)__sm_alloc_zero(total_size);
	if (map) {
		uint8_t *data = (uint8_t *)(((uintptr_t)map + sizeof(sm_t)) &
		    ~(uintptr_t)7);
		sm_init(map, data, size);
		/*
		 * sm_init tags the map as SM_WRAPPED (caller-supplied
		 * buffer); override here because the buffer is contiguous with the
		 * struct and we own both.
		 */
		__sm_set_kind(map, SM_OWNED_CONTIGUOUS);
		__sm_when_diag(
		    { __sm_assert(IS_8_BYTE_ALIGNED(map->m_data)); });
	}
	return (map);
}

/**
 * @brief Disposes of a sparsemap, regardless of allocation lineage.
 *
 * SM_OWNED_CONTIGUOUS  free(map) -- the struct and buffer share one block.
 * SM_OWNED_SPLIT       free(map->m_data) + free(map).
 * SM_WRAPPED           free(map) only -- the data buffer is the caller's
 *                      and is left untouched.
 *
 * Calling with NULL is a no-op.
 */
void
sm_free(sm_t *map)
{
	if (map == NULL) {
		return;
	}
	switch (__sm_kind(map)) {
	case SM_OWNED_SPLIT:
		__sm_free(map->m_data);
		/* fallthrough */
	case SM_OWNED_CONTIGUOUS:
	case SM_WRAPPED:
	default:
		__sm_free(map);
		break;
	}
}

/**
 * @brief Returns a guaranteed-owned, guaranteed-growable copy of \a map.
 *
 * The result is always SM_OWNED_CONTIGUOUS (single calloc, struct +
 * buffer in one heap block).  Use this when you have a sparsemap whose
 * lineage you don't trust and need a self-contained copy that's safe to
 * grow and dispose with sm_free() or libc free().
 */
sm_t *
sm_owned_copy(const sm_t *map)
{
	size_t cap;
	sm_t * out;
	if (map == NULL) {
		return (NULL);
	}
	cap = sm_get_capacity(map);
	out = sm_create(cap);
	if (out == NULL) {
		return (NULL);
	}
	out->m_data_used = map->m_data_used;
	/* m_capacity is already cap; lineage is SM_OWNED_CONTIGUOUS. */
	if (cap > 0 && map->m_data != NULL) {
		memcpy(out->m_data, map->m_data, cap);
	}
	return (out);
}

/**
 * @brief Creates a copy of the given sparse map.
 *
 * This function duplicates the provided sparse map, allocating a new sparse
 * map instance with the same capacity and copying over the used data.
 *
 * @param[in] other The sparse map to be copied.
 * @return A pointer to the newly created sparse map that is a copy of the input,
 *         or NULL if the memory allocation fails.
 */
sm_t *
sm_copy(const sm_t *other)
{
	size_t cap;
	sm_t * map;
	if (other == NULL) {
		errno = EINVAL;
		return (NULL);
	}
	cap = sm_get_capacity(other);
	map = sparsemap(cap);
	if (map) {
		__sm_set_cap_kind(map, cap, SM_OWNED_CONTIGUOUS);
		map->m_data_used = other->m_data_used;
		if (cap > 0) {
			memcpy(map->m_data, other->m_data, cap);
		}
	}
	return (map);
}

/**
 * @brief Wraps a given data array into a sparsemap structure.
 *
 * Allocates and initializes a sm_t structure to manage a provided data array.
 * The sparsemap structure will point to the data array and will track its capacity.
 *
 * @param[in] data Pointer to the data array to be managed by the sparsemap.
 * @param[in] size The size of the data array.
 * @return A pointer to the initialized sm_t structure, or NULL if allocation fails.
 */
sm_t *
sm_wrap(uint8_t *data, const size_t size)
{
	/* Wrap allocates only the struct (caller owns the data buffer);
	 * route through the global allocator so sm_free works correctly. */
	sm_t *map = (sm_t *)__sm_alloc_zero(sizeof(sm_t));
	if (map) {
		map->m_data = data;
		map->m_data_used = 0;
		__sm_set_cap_kind(map, size, SM_WRAPPED);
		__sm_card_invalidate(map);
	}
	return (map);
}

/**
 * @brief Initializes a sparsemap with the provided data and size.
 *
 * This function sets up the initial state of a sparsemap by assigning the given
 * data buffer and capacity. It also clears the sparsemap to ensure it starts empty.
 *
 * @param[in] map A pointer to the sparsemap to initialize.
 * @param[in] data A pointer to the data buffer to be used by the sparsemap.
 * @param[in] size The size of the data buffer in bytes.
 */
void
sm_init(sm_t *map, uint8_t *data, const size_t size)
{
	if (map == NULL) {
		errno = EINVAL;
		return;
	}
	map->m_data = data;
	map->m_data_used = 0;
	__sm_set_cap_kind(map, size, SM_WRAPPED);
	__sm_card_invalidate(map);
	/*
	 * Caller-allocated struct + caller-allocated buffer.  The buffer is
	 * not owned by the library; sm_set_data_size will treat any
	 * grow as a wrap-style promotion (allocate fresh, copy, transition
	 * to SM_OWNED_SPLIT).  sparsemap() overrides this to
	 * SM_OWNED_CONTIGUOUS after calling us.
	 */
	sm_clear(map);
}

/*
 * Drop every chunk that holds no member from a chunk-mode map read from
 * untrusted bytes, before it is validated.  The writers never leave one
 * (sm_validate rejects them), but sm_split through 5.8.5 could, and
 * the readers answer inconsistently on one: sm_minimum reports an empty
 * chunk's start.  An empty chunk holds no member, so dropping it keeps
 * the set.  The walk stops at the first chunk that does not fit in
 * m_data_used; sm_validate rejects that map.
 */
static void
__sm_drop_empty_chunks(sm_t *map)
{
	size_t count;
	size_t i;
	size_t off = 0;
	if (map->m_data_used < SM_SIZEOF_OVERHEAD || __sm_is_small(map)) {
		return;
	}
	count = __sm_get_chunk_count(map);
	for (i = 0; i < count; i++) {
		__sm_chunk_t chunk;
		size_t size;
		if (SM_SIZEOF_OVERHEAD + off + SM_SIZEOF_OVERHEAD +
		        sizeof(__sm_bitvec_t) > map->m_data_used) {
			return;
		}
		__sm_chunk_init(&chunk,
		    __sm_get_chunk_data(map, off) + SM_SIZEOF_OVERHEAD);
		size = SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		if (SM_SIZEOF_OVERHEAD + off + size > map->m_data_used) {
			return;
		}
		if ((chunk.m_data[0] & SM_DESC_HI_BITS) == 0) {
			uint8_t *p = __sm_get_chunk_data(map, off);
			memmove(p, p + size, map->m_data_used -
			    (SM_SIZEOF_OVERHEAD + off + size));
			map->m_data_used -= size;
			__sm_set_chunk_count(map,
			    __sm_get_chunk_count(map) - 1);
		} else {
			off += size;
		}
	}
}

/**
 * @brief Initializes a sparse map with given data and size.
 *
 * This function sets up the sparse map by assigning the provided data array and
 * size, and calculates the initial data usage.
 *
 * @param[in,out] map The sparse map to initialize.
 * @param[in] data Pointer to the data array to be used by the sparse map.
 * @param[in] size The capacity of the data array.
 */
void
sm_open(sm_t *map, uint8_t *data, const size_t size)
{
	size_t claimed_count;
	size_t walked_count;
	if (map == NULL) {
		errno = EINVAL;
		return;
	}
	map->m_data = data;
	__sm_card_invalidate(map);
	/*
	 * Set m_capacity and a temporary m_data_used = capacity *before*
	 * calling __sm_get_size_impl.  __sm_get_size_impl walks chunks via
	 * __sm_get_chunk_count, which since v1.0.0 short-circuits to 0
	 * when m_data_used < SM_SIZEOF_OVERHEAD (the empty-map guard for
	 * the heisenbug-related fix).  Without the temporary, sm_open of
	 * a fully-populated buffer reads its chunk count as 0 and produces
	 * a stunt-map with m_data_used = 4 -- which then trips a size_t
	 * underflow downstream when something tries to insert at the
	 * supposed-end of the chunks region.
	 *
	 * sm_open is for deserializing into a caller-supplied
	 * struct + buffer; lineage matches sm_init (SM_WRAPPED).
	 */
	__sm_set_cap_kind(map, size, SM_WRAPPED);
	/* Too short to hold the chunk-count header (size 0..7, including
	 * sm_open(m, NULL, 0)): the empty map.  Read and write nothing --
	 * __sm_cap rounds the size down to 0, but the walk below would
	 * still load an 8-byte header past the caller's buffer. */
	if (SM_UNLIKELY(size < SM_SIZEOF_OVERHEAD)) {
		map->m_data_used = 0;
		return;
	}
	map->m_data_used = __sm_cap(map);
	/* Small-set body: the header word's top bit is set.  Its size is
	 * fixed by the word count; don't run the chunk walk on it. */
	if (__sm_is_small(map)) {
		const size_t nwords = __sm_small_nwords(map);
		map->m_data_used =
		    SM_SIZEOF_OVERHEAD + nwords * sizeof(uint64_t);
		if (map->m_data_used > __sm_cap(map) || !sm_validate(map)) {
			__sm_store_u64(&map->m_data[0], 0);
			map->m_data_used = SM_SIZEOF_OVERHEAD;
		}
		return;
	}
	/* The stored count as the buffer claims it, before __sm_get_size_impl
	 * silently truncates it to the valid prefix.  A mismatch is an S1(e)
	 * violation (stored count disagrees with the walk). */
	claimed_count = __sm_get_chunk_count(map);
	map->m_data_used = __sm_get_size_impl(map);
	walked_count = __sm_get_chunk_count(map);
	/* An untrusted buffer must be structurally valid or it is replaced
	 * with an empty (valid) map -- the same contract sm_deserialize
	 * already enforces.  size 0 is the documented "leave it empty" call
	 * (sm_init/sm_wrap of a fresh buffer), so don't validate that. */
	if (claimed_count == walked_count) {
		__sm_drop_empty_chunks(map);
	}
	if (claimed_count != walked_count || !sm_validate(map)) {
		__sm_store_u64(&map->m_data[0], 0);
		map->m_data_used = SM_SIZEOF_OVERHEAD;
	}
}

sm_t *
sm_open_copy(const uint8_t *data, size_t n, size_t slack)
{
	size_t cap;
	sm_t * m;
	if (data == NULL && n > 0)
		return (NULL);
	/* sm_create needs at least SM_SIZEOF_OVERHEAD bytes; bump up if the
	 * caller asked for less. */
	cap = n + slack;
	if (cap < SM_SIZEOF_OVERHEAD)
		cap = SM_SIZEOF_OVERHEAD;
	m = sm_create(cap);
	if (m == NULL)
		return (NULL);
	if (n > 0) {
		size_t claimed_count;
		size_t walked_count;
		memcpy(sm_get_data(m), data, n);
		/* Small-set body: fixed size, no chunk walk. */
		if (n >= SM_SIZEOF_OVERHEAD && __sm_is_small(m)) {
			const size_t nwords = __sm_small_nwords(m);
			m->m_data_used =
			    SM_SIZEOF_OVERHEAD + nwords * sizeof(uint64_t);
			if (m->m_data_used > n || !sm_validate(m)) {
				sm_free(m);
				return (NULL);
			}
			__sm_set_kind(m, SM_OWNED_CONTIGUOUS);
			return (m);
		}
		/* sm_open re-derives m_data_used from the chunk count + walk;
		 * temporarily set m_data_used = capacity so the empty-map guard
		 * in __sm_get_chunk_count doesn't short-circuit during the walk. */
		m->m_data_used = __sm_cap(m);
		claimed_count = __sm_get_chunk_count(m);
		m->m_data_used = __sm_get_size_impl(m);
		walked_count = __sm_get_chunk_count(m);
		/* Untrusted bytes: reject anything not structurally valid
		 * rather than returning a half-parsed map. */
		if (claimed_count == walked_count) {
			__sm_drop_empty_chunks(m);
		}
		if (claimed_count != walked_count || !sm_validate(m)) {
			sm_free(m);
			return (NULL);
		}
	}
	/* sm_open's regular implementation transitions the lineage to
	 * SM_WRAPPED -- but here the buffer is contiguous with the struct
	 * because we got it from sm_create.  Restore the correct lineage so
	 * sm_free does the right thing and so subsequent grows can use the
	 * single-block realloc path. */
	__sm_set_kind(m, SM_OWNED_CONTIGUOUS);
	return (m);
}

/**
 * @brief Resizes the data buffer of the sparsemap.
 *
 * Behaviour depends on the calling form and the map's allocation
 * lineage:
 *
 *   sm_set_data_size(map, NULL, size)
 *     Library-managed grow / shrink.  Always succeeds (returning a
 *     possibly-relocated map pointer) or returns NULL on allocation
 *     failure.  Never silently no-ops the resize.
 *
 *       SM_OWNED_CONTIGUOUS -- realloc the single struct+buffer block.
 *                             Caller must update all map references to
 *                             the returned pointer.
 *       SM_OWNED_SPLIT      -- realloc m_data; map struct stays put.
 *       SM_WRAPPED          -- if size <= m_capacity, simply update
 *                             m_capacity (caller's buffer is still
 *                             theirs).  If size > m_capacity, allocate
 *                             a fresh library-owned buffer of the
 *                             requested size, memcpy the m_data_used
 *                             prefix into it, redirect m_data, and
 *                             transition lineage to SM_OWNED_SPLIT.
 *                             The caller's original buffer is left
 *                             untouched and remains theirs.
 *
 *   sm_set_data_size(map, data, size)  [data != NULL]
 *     Re-point the map at a caller-supplied buffer.  m_capacity is
 *     updated; copying any existing bits is the caller's
 *     responsibility.  Lineage transitions to SM_WRAPPED -- the library
 *     does not own the new buffer and will not realloc/free it on the
 *     caller's behalf.
 *
 * @param[in,out] map   The sparsemap to resize.  Must be non-NULL.
 * @param[in]     data  Optional caller-supplied buffer; NULL means
 *                      "library decides".
 * @param[in]     size  New buffer size in bytes.
 * @return The (possibly relocated) sparsemap pointer on success,
 *         or NULL on allocation failure.
 */
sm_t *
sm_set_data_size(sm_t *map, uint8_t *data, const size_t size)
{
	size_t asize;
	size_t cur_cap;
	if (map == NULL) {
		return (NULL);
	}

	/* Caller-driven re-point: trust them, transition to SM_WRAPPED. */
	if (data != NULL) {
		if (data != map->m_data) {
			map->m_data = data;
		}
		__sm_set_cap_kind(map, size, SM_WRAPPED);
		return (map);
	}

	/* Library-managed resize.  Round the requested size up to an
	 * 8-byte boundary so the allocated buffer and the stored (low-bit-
	 * tagged) capacity agree exactly; __sm_set_cap_kind rounds down,
	 * so an already-aligned size round-trips unchanged. */
	asize = (size + 7u) & ~(size_t)7;
	cur_cap = __sm_cap(map);
	switch (__sm_kind(map)) {
	case SM_OWNED_CONTIGUOUS: {
		size_t total_size;
		size_t padding;
		size_t old_capacity;
		sm_t * m;
		if (size == cur_cap) {
			return (map);
		}
		/*
		 * Realloc the single block.  Allocate room for the struct + the
		 * new data buffer + alignment padding so m_data lands on an 8-byte
		 * boundary.
		 */
		total_size = sizeof(sm_t) + size;
		padding = total_size % 8 == 0 ? 0 : 8 - (total_size % 8);
		total_size += padding;

		old_capacity = cur_cap;
		m = (sm_t *)__sm_realloc(map, total_size);
		if (!m) {
			/* Original block still valid; leave map untouched. */
			return (NULL);
		}
		m->m_data =
		    (uint8_t *)(((uintptr_t)m + sizeof(sm_t)) & ~(uintptr_t)7);
		if (size > old_capacity) {
			/* Zero the newly-acquired tail so chunk metadata stays clean. */
			memset(m->m_data + old_capacity, 0,
			    size - old_capacity);
		}
		__sm_set_cap_kind(m, size, SM_OWNED_CONTIGUOUS);
		/*
		 * m_data_used does not change on grow; on shrink the caller is
		 * responsible for ensuring m_data_used <= size before calling.
		 */
		if (m->m_data_used > __sm_cap(m)) {
			m->m_data_used = __sm_cap(m);
		}
		__sm_when_diag({ __sm_assert(IS_8_BYTE_ALIGNED(m->m_data)); });
		return (m);
	}

	case SM_OWNED_SPLIT: {
		uint8_t * new_data;
		if (size == cur_cap) {
			return (map);
		}
		new_data = (uint8_t *)__sm_realloc(map->m_data, size);
		if (!new_data) {
			return (NULL);
		}
		if (size > cur_cap) {
			memset(new_data + cur_cap, 0, size - cur_cap);
		}
		map->m_data = new_data;
		__sm_set_cap_kind(map, size, SM_OWNED_SPLIT);
		if (map->m_data_used > __sm_cap(map)) {
			map->m_data_used = __sm_cap(map);
		}
		return (map);
	}

	case SM_WRAPPED: {
		uint8_t * new_data;
		size_t copy_bytes;
		/*
		 * Caller owns m_data.  Two cases:
		 *
		 *   size <= capacity (shrink or same):
		 *     We do not own the buffer, so we cannot realloc/free it.  Just
		 *     update the recorded capacity to "use no more than `size`
		 *     bytes of the caller's buffer".  The caller's buffer is
		 *     unchanged and remains theirs to free.
		 *
		 *   size > capacity (grow):
		 *     Allocate a fresh library-owned buffer of the requested size,
		 *     copy the in-use prefix (m_data_used bytes), redirect m_data,
		 *     transition lineage to SM_OWNED_SPLIT.  The caller's original
		 *     buffer is untouched and remains theirs.
		 *
		 *     This is the path that fixes the heisenbug from
		 *     HEISENBUG_REPORT.md: pre-fix, the function silently set
		 *     the capacity without allocating storage, and the next
		 *     sm_add corrupted the heap.
		 */
		if (size <= cur_cap) {
			__sm_set_cap_kind(map, size, SM_WRAPPED);
			if (map->m_data_used > __sm_cap(map)) {
				map->m_data_used = __sm_cap(map);
			}
			return (map);
		}

		new_data = (uint8_t *)__sm_alloc_zero(asize);
		if (!new_data) {
			return (NULL);
		}
		copy_bytes = map->m_data_used <= cur_cap ?
		    map->m_data_used :
		    cur_cap;
		if (copy_bytes > 0 && map->m_data != NULL) {
			memcpy(new_data, map->m_data, copy_bytes);
		}
		map->m_data = new_data;
		__sm_set_cap_kind(map, asize, SM_OWNED_SPLIT);
		return (map);
	}
	}

	/* Unreachable. */
	__sm_when_diag(
	    { __sm_assert(0 && "unknown sparsemap allocation lineage"); });
	return (NULL);
}

/* -------------------------------------------------------------------
 * Small-set <-> chunk transitions
 * ------------------------------------------------------------------- */

static __sm_idx_t __sm_map_set(sm_t *map, uint64_t idx, bool coalesce,
    sm_cursor_t *cur);
static void __sm_expand_sparse_chunk(const __sm_chunk_t *chunk,
    __sm_bitvec_t words[32], int cap_flags[32]);

/*
 * Exact byte footprint the single low chunk would occupy for the bits
 * currently held in a small-mode map: 8 (chunk-count header) + 8 (chunk
 * start) + 8 (descriptor) + 8 per 64-bit word that is neither all-zero
 * (a ZEROS slot, not stored) nor all-one (a ONES slot, not stored).
 * Every index in a small map is < SM_SMALL_MAX_BITS < one chunk's 2048-
 * bit window, so the whole set is exactly one chunk.
 */
static size_t
__sm_small_chunk_bytes(const sm_t *map)
{
	size_t i;
	const __sm_bitvec_unaligned_t *w = __sm_small_words(map);
	const size_t n = __sm_small_nwords(map);
	size_t mixed = 0;
	for (i = 0; i < n; i++) {
		if (w[i] != 0 && w[i] != ~(uint64_t)0) {
			mixed++;
		}
	}
	return (SM_SIZEOF_OVERHEAD + SM_SIZEOF_OVERHEAD +
	    sizeof(__sm_bitvec_t) + mixed * sizeof(__sm_bitvec_t));
}

/* Byte footprint of a small map's own stored form. */
static size_t
__sm_small_bytes(const sm_t *map)
{
	return (SM_SIZEOF_OVERHEAD + __sm_small_nwords(map) * sizeof(uint64_t));
}

/*
 * Should a set whose maximum index is `maxbit` and whose single low
 * chunk would occupy `chunk_bytes` be stored in small-set mode?  Yes
 * when the index span fits the small cap AND the small form is no
 * larger than the chunk form.  (chunk_bytes == 0 means "not computed";
 * fall back to the span test alone, used when there are no bits yet.)
 */
static inline bool
__sm_small_is_better(uint64_t maxbit, size_t small_bytes, size_t chunk_bytes)
{
	if (maxbit >= SM_SMALL_MAX_BITS) {
		return (false);
	}
	return (chunk_bytes == 0 || small_bytes <= chunk_bytes);
}

/*
 * Convert a small-mode map to chunk mode in place, within the existing
 * buffer capacity.  Returns true on success; false (ENOSPC) if the
 * chunk form would not fit -- the caller (sm_add) then reports ENOSPC
 * and the sm_add_grow wrapper grows and retries.  The map is left
 * unchanged on failure.
 */
static bool
__sm_promote(sm_t *map)
{
	size_t n;
	uint64_t words[SM_SMALL_MAX_WORDS];
	size_t i;
	__sm_assert(__sm_is_small(map));
	/* Snapshot the bits; the buffer is reused for the chunk form. */
	n = __sm_small_nwords(map);
	__sm_assert(n <= SM_SMALL_MAX_WORDS);
	memcpy(words, __sm_small_words(map), n * sizeof(uint64_t));

	/* Reset to an empty chunk-mode map and re-add every set bit.  Each
	 * add stays within [0, SM_SMALL_MAX_BITS) == one chunk, so the peak
	 * footprint is one chunk; if that exceeds capacity, add returns
	 * ENOSPC and we restore the small header. */
	map->m_data_used = SM_SIZEOF_OVERHEAD;
	__sm_set_chunk_count(map, 0);
	for (i = 0; i < n; i++) {
		uint64_t bits = words[i];
		while (bits != 0) {
			uint64_t idx;
			const int b = SM_CTZ64(bits);
			bits &= bits - 1;
			idx = (uint64_t)i * 64 + (uint64_t)b;
			if (__sm_map_set(map, idx, true, NULL) == SM_IDX_MAX) {
				/* Out of space: restore the small form. */
				map->m_data_used =
				    SM_SIZEOF_OVERHEAD + n * sizeof(uint64_t);
				__sm_small_set_header(map, n);
				memcpy(__sm_small_words(map), words,
				    n * sizeof(uint64_t));
				return (false);
			}
		}
	}
	return (true);
}

/*
 * If a chunk-mode map now holds only indices below SM_SMALL_MAX_BITS and
 * the small form would be no larger, convert it to small mode in place.
 * Always fits (small form is smaller than the chunk form it replaces).
 * A no-op for maps that should stay in chunk mode.
 */
static void
__sm_try_demote(sm_t *map)
{
	size_t count;
	uint8_t * p;
	__sm_idx_t start;
	__sm_chunk_t chunk;
	__sm_bitvec_t w32[SM_FLAGS_PER_INDEX];
	int cap[SM_FLAGS_PER_INDEX];
	size_t hi_word;
	bool any;
	uint64_t maxbit;
	size_t nwords;
	size_t small_bytes;
	uint64_t out[SM_SMALL_MAX_WORDS];
	int wi;
	size_t i;
	if (map == NULL || __sm_is_small(map) || map->m_data == NULL) {
		return;
	}
	if (map->m_data_used < SM_SIZEOF_OVERHEAD) {
		return;
	}
	count = __sm_get_chunk_count(map);
	if (count == 0) {
		/* Empty: leave as an empty chunk-mode map (size == overhead,
		 * same as small with zero words). */
		return;
	}
	if (count > 1) {
		return; /* spans >1 chunk => max index >= 2048 > threshold */
	}
	/* Single chunk: it must start at 0 for the small form (from bit 0)
	 * to represent it, and every index must be < SM_SMALL_MAX_BITS. */
	p = __sm_get_chunk_data(map, 0);
	start = __sm_load_idx((const uint8_t *)p);
	if (start != 0) {
		return;
	}
	__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
	__sm_expand_sparse_chunk(&chunk, w32, cap);
	/* Highest set bit within the chunk. */
	hi_word = 0;
	any = false;
	for (wi = 0; wi < (int)SM_FLAGS_PER_INDEX; wi++) {
		if (cap[wi] && w32[wi] != 0) {
			hi_word = (size_t)wi;
			any = true;
		}
	}
	if (!any) {
		/* Chunk with no set bits: collapse to empty chunk-mode. */
		map->m_data_used = SM_SIZEOF_OVERHEAD;
		__sm_set_chunk_count(map, 0);
		return;
	}
	maxbit = (uint64_t)hi_word * 64 +
	    (63 - (uint64_t)SM_CLZ64(w32[hi_word]));
	if (maxbit >= SM_SMALL_MAX_BITS) {
		return;
	}
	nwords = (size_t)(maxbit / 64) + 1;
	small_bytes = SM_SIZEOF_OVERHEAD +
	    nwords * sizeof(uint64_t);
	if (small_bytes > map->m_data_used) {
		return; /* chunk form is already smaller; keep it */
	}
	/* Build the small form from the expanded words.  small_bytes <=
	 * m_data_used <= capacity, so it always fits. */
	memset(out, 0, sizeof(out));
	for (i = 0; i < nwords; i++) {
		out[i] = w32[i];
	}
	__sm_small_set_header(map, nwords);
	memcpy(__sm_small_words(map), out, nwords * sizeof(uint64_t));
	map->m_data_used = small_bytes;
}

/*
 * Materialize a small-mode map into a fresh chunk-mode map so the
 * chunk-walking read/algebra paths get a uniform view without mutating
 * the const input.  Returns NULL on allocation failure.
 */
static sm_t *
__sm_materialize(const sm_t *small)
{
	size_t i;
	const __sm_bitvec_unaligned_t *w = __sm_small_words(small);
	const size_t n = __sm_small_nwords(small);
	sm_t *m = sm_create(256);
	if (m == NULL) {
		return (NULL);
	}
	for (i = 0; i < n; i++) {
		uint64_t bits = w[i];
		while (bits != 0) {
			uint64_t idx;
			const int b = SM_CTZ64(bits);
			bits &= bits - 1;
			idx = (uint64_t)i * 64 + (uint64_t)b;
			if (sm_add_grow(&m, idx) == SM_IDX_MAX) {
				sm_free(m);
				return (NULL);
			}
		}
	}
	/* sm_add keeps a near-zero set in small mode; force chunk mode so
	 * the caller (the chunk-walking read/algebra paths) gets a real
	 * chunk stream and __sm_chunk_view does not recurse forever. */
	if (__sm_is_small(m) && !__sm_small_is_empty(m)) {
		if (!__sm_promote(m)) {
			/* Promote needs the chunk form to fit; grow and retry. */
			sm_t *g = sm_set_data_size(m, NULL,
			    sm_get_capacity(m) * 2 + 4096);
			if (g == NULL) {
				sm_free(m);
				return (NULL);
			}
			m = g;
			if (!__sm_promote(m)) {
				sm_free(m);
				return (NULL);
			}
		}
	}
	return (m);
}

/*
 * Return a chunk-mode view of `map` for the raw-chunk-walking read and
 * set-algebra paths.  If the map is already chunk mode (or NULL) it is
 * returned as-is and *owned is false.  If it is small, a materialized
 * chunk-mode copy is returned and *owned is set true; the caller must
 * sm_free() it.  On materialization failure returns NULL with *owned
 * false (callers treat NULL as an empty operand / allocation failure).
 */
static const sm_t *
__sm_chunk_view(const sm_t *map, bool *owned)
{
	sm_t * m;
	*owned = false;
	if (map == NULL || !__sm_is_small(map)) {
		return (map);
	}
	m = __sm_materialize(map);
	if (m == NULL) {
		return (NULL);
	}
	*owned = true;
	return (m);
}

/**
 * @brief Calculates the remaining capacity of the sparsemap.
 *
 * This function returns the percentage of unused capacity in the sparse map.
 * If the used capacity is equal to or exceeds the total capacity, it returns 0.
 * If the total capacity is 0, it returns 100. Otherwise, it returns the
 * percentage of capacity remaining.
 *
 * @param[in] map The sparsemap for which the remaining capacity is calculated.
 * @return The percentage of remaining capacity in the sparsemap.
 */
double
sm_capacity_remaining(const sm_t *map)
{
	size_t cap;
	if (map == NULL)
		return (0.0);
	cap = __sm_cap(map);
	if (map->m_data_used >= cap) {
		return (0);
	}
	if (cap == 0) {
		return (100.0);
	}
	return ((1.0 - ((double)map->m_data_used / (double)cap)) * 100.0);
}

/**
 * @brief Retrieves the capacity of the sparse map.
 *
 * This function returns the total capacity of the given sparse map, which is
 * the size of the underlying data structure.
 *
 * @param[in] map Pointer to the sparse map.
 * @return The capacity of the sparse map.
 */
size_t
sm_get_capacity(const sm_t *map)
{
	if (map == NULL)
		return (0);
	return (__sm_cap(map));
}

/* -------------------------------------------------------------------
 * Single-bit operations: test, set, and clear
 * ------------------------------------------------------------------- */

/**
 * @brief Checks if a specific bit is set in the sparse map.
 *
 * This function determines whether the bit at the given index is set in the
 * sparse map. It performs various checks and traverses to the appropriate
 * chunk to verify the bit's state.
 *
 * @param[in] map The sparse map to check.
 * @param[in] idx The index of the bit to check.
 * @return True if the bit is set, false otherwise.
 */
SM_HOT bool
sm_contains(const sm_t *map, uint64_t idx, sm_cursor_t *cur)
{
	ssize_t offset;
	uint8_t * p;
	__sm_idx_t start;
	__sm_chunk_t chunk;
	/* Defensive: NULL or empty maps contain nothing.  Accepting NULL is
	 * cheap insurance for consumers that pass the result of
	 * sm_intersection / sm_difference / sm_xor unchecked, which
	 * legitimately return NULL when the result is empty. */
	if (map == NULL || idx == SM_IDX_MAX) {
		return (false);
	}
	if (__sm_is_small(map)) {
		return (__sm_small_contains(map, idx));
	}
	__sm_assert(sm_get_size((sm_t *)map) >= SM_SIZEOF_OVERHEAD);

	/* Get the __sm_chunk_t which manages this index */
	offset = __sm_get_chunk_offset(map, idx, cur);

	/* No __sm_chunk_t's available -> the bit is not set */
	if (offset == -1) {
		return (false);
	}

	/* Otherwise load the __sm_chunk_t */
	p = __sm_get_chunk_data(map, (size_t)offset);
	start = __sm_load_idx((const uint8_t *)p);
	__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);

	/*
	 * Determine if the bit is out of bounds of the __sm_chunk_t; if yes then
	 * the bit is not set.
	 */
	if (idx < start ||
	    (__sm_idx_t)idx - start >= __sm_chunk_get_capacity(&chunk)) {
		return (false);
	}

	/* Otherwise ask the __sm_chunk_t whether the bit is set. */
	return (__sm_chunk_is_set(&chunk, idx - start));
}

/**
 * @brief Unsets a bit at a specified index in the given sparse map.
 *
 * This function clears the bit at the given index in the sparse map. It handles
 * different scenarios, including chunks that do not exist for the specified index
 * and sparse chunks.
 *
 * The function also optionally performs chunk coalescing if the `coalesce` flag is set.
 *
 * @param[in,out] map The sparse map in which the bit needs to be unset.
 * @param[in] idx The index of the bit to be unset.
 * @param[in] coalesce A flag indicating whether to perform chunk coalescing.
 * @return The index of the bit that was unset.
 */
/*
 * Sentinel stored in the size_t byte-offset variable `offset` to gate chunk
 * coalescing off (the chunk was never located or its pointers are now stale).
 * It MUST be size_t-width: a uint64_t sentinel (SM_IDX_MAX == UINT64_MAX)
 * truncates to 0xFFFFFFFF on ILP32 targets, so the `!=` gate test (which
 * promotes offset back to 64 bits) never matches and coalescing runs on an
 * uninitialized chunk -- a 32-bit-only crash.
 */
#define SM_UNSET_NO_COALESCE ((size_t)-1)
static __sm_idx_t
__sm_map_unset(sm_t *map, uint64_t idx, const bool coalesce)
{
	size_t offset;
	size_t chunk_offset;
	uint8_t * p;
	__sm_idx_t start;
	__sm_chunk_t chunk;
	size_t capacity;
	size_t pos;
	__sm_bitvec_t vec;
	const uint64_t ret_idx = idx;
	__sm_assert(sm_get_size(map) >= SM_SIZEOF_OVERHEAD);

	/* Clearing a bit could require an additional vector, let's ensure we have that
	 * space available in the buffer first, or ENOMEM now. */
	SM_ENOUGH_SPACE(SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));

	/* Determine if there is a chunk that could contain this index. */
	offset = (size_t)__sm_get_chunk_offset(map, idx, NULL);
	chunk_offset = offset;

	if ((ssize_t)offset == -1) {
		/* There are no chunks in the map, there is nothing to clear, this is a
		 * no-op. */
		offset =
		    SM_UNSET_NO_COALESCE; /* gate coalesce off; chunk is uninitialized */
		goto done;
	}

	/*
	 * Try to locate a chunk for this idx.  We could find that:
	 * - the first chunk's offset is greater than the index, or
	 * - the index is beyond the end of the last chunk, or
	 * - we found a chunk that can contain this index.
	 */
	p = __sm_get_chunk_data(map, offset);
	start = __sm_load_idx((const uint8_t *)p);
	__sm_assert(start == __sm_get_chunk_aligned_offset(start));

	if (idx < start) {
		/* Our search resulted in the first chunk that starts after the index but
		 * that means there is no chunk that contains this index, so again this is
		 * a no-op. */
		offset =
		    SM_UNSET_NO_COALESCE; /* gate coalesce off; chunk is uninitialized */
		goto done;
	}

	__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
	capacity = __sm_chunk_get_capacity(&chunk);

	if (idx - start >= capacity) {
		/*
		 * Our search resulted in a chunk however it's capacity doesn't encompass
		 * this index, so again a no-op.
		 */
		offset = SM_UNSET_NO_COALESCE; /* gate coalesce off; chunk untouched */
		goto done;
	}

	pos = 0;
	vec = ~(__sm_bitvec_t)0;
	switch (__sm_chunk_clr_bit(&chunk, idx - start, &pos)) {
	case SM_OK:
		break;
	case SM_NEEDS_TO_GROW:
		SM_ENOUGH_SPACE(sizeof(__sm_bitvec_t));
		offset += SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t);
		__sm_insert_data(map, offset, (uint8_t *)&vec,
		    sizeof(__sm_bitvec_t));
		__sm_chunk_clr_bit(&chunk, idx - start, &pos);
		break;
	case SM_NEEDS_TO_SHRINK:
		/* The vector is empty, perhaps the entire chunk is empty? */
		if (__sm_chunk_is_empty(&chunk)) {
			__sm_remove_data(map, offset,
			    SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2));
			__sm_set_chunk_count(map,
			    __sm_get_chunk_count(map) - 1);
		} else {
			offset +=
			    SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t);
			__sm_remove_data(map, offset, sizeof(__sm_bitvec_t));
		}
		break;
	default:
		__sm_assert(!"shouldn't be here");
#ifdef DEBUG
		abort();
#endif
		break;
	}

done:;
	if (coalesce && offset != SM_UNSET_NO_COALESCE) {
		__sm_coalesce_chunk(map, &chunk, chunk_offset, start, p, idx,
		    false, SIZE_MAX);
	}
	return (ret_idx);
}

/**
 * @brief Unsets the value at a specific index in the sparse map.
 *
 * This function calls the internal __sm_map_unset function with the coalesce parameter
 * set to true, which removes an entry at the specified index and attempts to merge adjacent
 * segments to maintain the map's sparsity.
 *
 * @param[in] map The sparse map in which the value will be unset.
 * @param[in] idx The index at which the value will be unset.
 * @return The index that was unset.
 */
SM_HOT uint64_t
sm_remove(sm_t *map, const uint64_t idx)
{
	uint64_t rc;
	if (map == NULL) {
		errno = EINVAL;
		return (SM_IDX_MAX);
	}
	/* SM_IDX_MAX is never a member: nothing to clear, but the return
	 * value is the failure sentinel, so say why. */
	if (idx == SM_IDX_MAX) {
		errno = EINVAL;
		return (SM_IDX_MAX);
	}
	__sm_card_invalidate(map);
	if (__sm_is_small(map)) {
		const size_t w = (size_t)(idx / 64);
		if (w < __sm_small_nwords(map)) {
			size_t n;
			__sm_bitvec_unaligned_t *words;
			__sm_small_words(map)[w] &=
			    ~((uint64_t)1 << (idx % 64));
			/* Shrink the trailing all-zero words so the footprint
			 * tracks the new maximum index. */
			n = __sm_small_nwords(map);
			words = __sm_small_words(map);
			while (n > 0 && words[n - 1] == 0) {
				n--;
			}
			__sm_small_set_header(map, n);
			map->m_data_used = SM_SIZEOF_OVERHEAD +
			    n * sizeof(uint64_t);
		}
		return (idx);
	}
	rc = __sm_map_unset(map, idx, true);
	__sm_try_demote(map);
	return (rc);
}

/**
 * @brief Sets a bit in a chunk within the sparse map and manages chunk resizing.
 *
 * This function sets a bit in the chunk of a sparse map corresponding to the
 * given index. It handles the initialization, setting the bit, and necessary
 * memory adjustments for growing or shrinking chunks, including allocation and
 * deallocation of bit vectors.
 *
 * @param[in,out] map The sparse map where the bit will be set.
 * @param[in] idx The index within the sparse map where the bit will be set.
 * @param[in] p A pointer to the chunk data within the sparse map.
 * @param[in] offset The offset within the sparse map's data where the chunk is located.
 * @param[in] v A bit vector, when non-NULL, indicates that a new chunk has been added.
 *
 * @return The index at which the bit was set.
 */
static __sm_idx_t
__sparsemap_add(sm_t *map, const uint64_t idx, uint8_t *p, size_t offset,
    const void *v)
{
	/*
	 * When v is non-NULL we've just added a new chunk, and we knew in advance that a
	 * new chunk would result in an SM_PAYLOAD_MIXED which in turn requires space to
	 * store the bit pattern, so given that we allocated the space ahead of time we
	 * don't need to allocate it now.
	 */
	size_t pos = v ? (size_t)-1 : 0;
	__sm_chunk_t chunk;
	const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);

	__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);

	switch (__sm_chunk_set_bit(&chunk, idx - start, &pos)) {
	case SM_OK:
		break;
	case SM_NEEDS_TO_GROW:
		if (!v) {
			__sm_bitvec_t vec = 0;
			SM_ENOUGH_SPACE(sizeof(__sm_bitvec_t));
			offset +=
			    SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t);
			__sm_insert_data(map, offset, (uint8_t *)&vec,
			    sizeof(__sm_bitvec_t));
			pos = (size_t)-1;
		}
		__sm_chunk_set_bit(&chunk, idx - start, &pos);
		break;
	case SM_NEEDS_TO_SHRINK:
		/* The vector is empty, perhaps the entire chunk is empty? */
		if (__sm_chunk_is_empty(&chunk)) {
			__sm_remove_data(map, offset,
			    SM_SIZEOF_OVERHEAD + (sizeof(__sm_bitvec_t) * 2));
			__sm_set_chunk_count(map,
			    __sm_get_chunk_count(map) - 1);
		} else {
			offset +=
			    SM_SIZEOF_OVERHEAD + pos * sizeof(__sm_bitvec_t);
			__sm_remove_data(map, offset, sizeof(__sm_bitvec_t));
		}
		break;
	default:
		__sm_assert(!"shouldn't be here");
#ifdef DEBUG
		abort();
#endif
		break;
	}

	return (idx);
}

/**
 * @brief Sets a bit in the sparse bit map.
 *
 * This function sets a bit at the given index in the provided sparse bit map.
 * It performs various internal checks and operations to ensure the data integrity of the map,
 * including initializing, inserting new chunks, and transitioning chunk states when necessary.
 *
 * @param[in,out] map The sparse bit map to be modified.
 * @param[in] idx The index of the bit to set.
 * @param[in] coalesce A flag indicating whether to attempt chunk coalescing.
 * @return Returns the adjusted index within the sparse bit map or the given index.
 */
static __sm_idx_t
__sm_map_set(sm_t *map, uint64_t idx, const bool coalesce, sm_cursor_t *cur)
{
	size_t offset;
	size_t left_hint;
	size_t capacity;
	__sm_chunk_t chunk;
	uint64_t ret_idx = idx;
	__sm_idx_t start;
	uint8_t *p;
	__sm_assert(sm_get_size(map) >= SM_SIZEOF_OVERHEAD);

	/*
	 * Setting a bit could require an additional vector, let's ensure we have that
	 * space available in the buffer first, or ENOMEM now.
	 */
	SM_ENOUGH_SPACE(SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));

	/* Determine if there is a chunk that could contain this index. */
	offset = (size_t)__sm_get_chunk_offset(map, idx, cur);

	/* Free left-neighbor hint for the coalescing path: the forward walk
	 * above already passed over the chunk immediately before the located
	 * chunk and recorded its byte offset.  It stays valid ONLY while the
	 * located chunk keeps its position; every path below that inserts,
	 * separates, or shifts chunk layout at/before `offset` resets it to
	 * SIZE_MAX so a stale hint is never produced.  A SIZE_MAX hint just
	 * makes __sm_coalesce_chunk fall back to a head-walk.  A gap lookup
	 * leaves the cursor on the left neighbour instead of the returned
	 * chunk; its prev_offset is then not this chunk's neighbour. */
	left_hint = (cur != NULL && cur->offset == offset) ?
	    cur->prev_offset : SIZE_MAX;

	if ((ssize_t)offset == -1) {
		const __sm_bitvec_unaligned_t * v;
		/*
		 * No chunks exist, the map is empty, so we must append a new chunk to the
		 * end of the buffer and initialize it so that it can contain this index.
		 */
		const uint8_t buf[SM_SIZEOF_OVERHEAD +
		    (sizeof(__sm_bitvec_t) * 2)] = { 0 };
		/* Capacity was established by the SM_ENOUGH_SPACE() above; a
		 * failure here would mean that check and this size disagree,
		 * so propagate ENOSPC rather than corrupt the buffer. */
		if (SM_UNLIKELY(!__sm_append_data(map, &buf[0], sizeof(buf)))) {
			return (SM_IDX_MAX);
		}
		p = __sm_get_chunk_data(map, 0);
		__sm_store_idx((uint8_t *)p,
		    __sm_get_chunk_aligned_offset(idx));
		__sm_set_chunk_count(map, 1);

		v = (__sm_bitvec_unaligned_t *)((uintptr_t)p +
		        SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
		ret_idx = __sparsemap_add(map, idx, p, 0, v);

		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		start = __sm_load_idx((const uint8_t *)p);
		offset = 0;
		left_hint = SIZE_MAX; /* fresh append; no left neighbor */
		goto done;
	}

	/*
	 * Try to locate a chunk for this idx.  We could find that:
	 *  - the first chunk's offset is greater than the index, or
	 *  - the index is beyond the end of the last chunk, or
	 *  - we found a chunk that can contain this index.
	 */
	p = __sm_get_chunk_data(map, offset);
	start = __sm_load_idx((const uint8_t *)p);
	__sm_assert(start == __sm_get_chunk_aligned_offset(start));

	if (idx < start) {
		const __sm_bitvec_unaligned_t * v;
		/*
		 * Our search resulted in the first chunk, but it starts after the index,
		 * so that means there is no chunk that can contain this index.  We need
		 * to insert a new chunk before this one and initialize it so that it can
		 * contain this index.
		 */
		const uint8_t buf[SM_SIZEOF_OVERHEAD +
		    (sizeof(__sm_bitvec_t) * 2)] = { 0 };
		SM_ENOUGH_SPACE(sizeof(buf));
		__sm_insert_data(map, offset, &buf[0], sizeof(buf));
		__sm_set_chunk_count(map, __sm_get_chunk_count(map) + 1);

		/* NOTE: insert moves the memory over meaning `p` is now the new chunk */
		__sm_store_idx((uint8_t *)p,
		    __sm_get_chunk_aligned_offset(idx));
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);

		v = (__sm_bitvec_unaligned_t *)((uintptr_t)p +
		        SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
		ret_idx = __sparsemap_add(map, idx, p, offset, v);
		left_hint = SIZE_MAX; /* inserted a chunk before this one */
		goto done;
	}

	__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
	capacity = __sm_chunk_get_capacity(&chunk);

	if (capacity < SM_CHUNK_MAX_CAPACITY &&
	    idx - start < SM_CHUNK_MAX_CAPACITY) {
		/*
		 * Special case, we have a sparse chunk with one or more flags set to
		 * SM_PAYLOAD_NONE which reduces the carrying capacity of the chunk. In
		 * this case we should remove those flags and try again.
		 */
		__sm_chunk_increase_capacity(&chunk, SM_CHUNK_MAX_CAPACITY);
		capacity = __sm_chunk_get_capacity(&chunk);
	}

	/*
	 * A run longer than one chunk's sparse capacity is stored as several
	 * adjacent all-ONES sparse chunks.  When this full chunk's next bit
	 * is set (idx - start == SM_CHUNK_MAX_CAPACITY == capacity) the
	 * generic "insert a new chunk after this one" path below handles it.
	 */

	if (idx - start >= capacity) {
		const __sm_bitvec_unaligned_t * v;
		/*
		 * Our search resulted in a chunk however it's capacity doesn't encompass
		 * this index, so we need to insert a new chunk after this one and
		 * initialize it so that it can contain this index.
		 */
		const uint8_t buf[SM_SIZEOF_OVERHEAD +
		    (sizeof(__sm_bitvec_t) * 2)] = { 0 };
		const size_t size = __sm_chunk_get_size(&chunk);
		SM_ENOUGH_SPACE(sizeof(buf));
		offset += SM_SIZEOF_OVERHEAD + size;
		p += SM_SIZEOF_OVERHEAD + size;
		__sm_insert_data(map, offset, &buf[0], sizeof(buf));

		start = __sm_get_chunk_aligned_offset(idx);
		__sm_store_idx((uint8_t *)p, start);
		__sm_assert(start == __sm_get_chunk_aligned_offset(start));
		__sm_set_chunk_count(map, __sm_get_chunk_count(map) + 1);

		v = (__sm_bitvec_unaligned_t *)((uintptr_t)p +
		        SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
		ret_idx = __sparsemap_add(map, idx, p, offset, v);
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		left_hint = SIZE_MAX; /* inserted a new chunk after this one;
		                       * hint pointed at the old chunk's
		                       * predecessor, wrong for the new offset */
		goto done;
	}

	ret_idx = __sparsemap_add(map, idx, p, offset, NULL);
	if (ret_idx != idx) {
		goto done;
	}

done:;
	if (coalesce) {
		__sm_coalesce_chunk(map, &chunk, offset, start, p, idx, true,
		    left_hint);
	}
	/*
	 * Re-seat the caller's cursor at the chunk we just touched so an
	 * ascending bulk insert (sm_add_many / sm_add_many_grow) resumes
	 * the next __sm_get_chunk_offset walk here instead of from the
	 * head -- the difference between O(N) and O(N^2) when a hot
	 * trigram accumulates tens of thousands of TIDs.  We record the
	 * byte offset and the chunk's start index; __sm_get_chunk_offset
	 * self-validates this (re-walking from the head if a later
	 * mutation shifted the chunk), so a stale seat is merely slow,
	 * never wrong.  Coalescing may have moved the chunk, so seat
	 * AFTER it and let the next call's validation sort out any drift.
	 */
	if (cur != NULL) {
		cur->offset = offset;
		cur->start_idx = start;
	}
	return (ret_idx);
}

/*
 * Small-mode bit set.  Grows the word array within the existing buffer
 * capacity; returns SM_IDX_MAX (errno=ENOSPC) if the buffer is too
 * small, so sm_add_grow can grow and retry.  The map stays in small
 * mode.  Caller guarantees idx < SM_SMALL_MAX_BITS.
 */
static uint64_t
__sm_small_add(sm_t *map, uint64_t idx)
{
	const size_t w = (size_t)(idx / 64);
	size_t n = __sm_small_nwords(map);
	if (w >= n) {
		__sm_bitvec_unaligned_t *words;
		size_t i;
		const size_t need = SM_SIZEOF_OVERHEAD +
		    (w + 1) * sizeof(uint64_t);
		if (need > __sm_cap(map)) {
			errno = ENOSPC;
			return (SM_IDX_MAX);
		}
		/* Zero the newly-exposed words. */
		words = __sm_small_words(map);
		for (i = n; i <= w; i++) {
			words[i] = 0;
		}
		n = w + 1;
		__sm_small_set_header(map, n);
		map->m_data_used = need;
	}
	__sm_small_words(map)[w] |= (uint64_t)1 << (idx % 64);
	return (idx);
}

/*
 * Unified add path handling both small and chunk mode.
 *
 *  - A small-mode map (or an empty chunk-mode map) whose new max index
 *    stays below SM_SMALL_MAX_BITS stays/goes small.  If, after the add,
 *    the equivalent single chunk would be strictly smaller, promote --
 *    that always fits, since the chunk form is the smaller one.
 *  - Otherwise (index out of small range, or already a multi-chunk map)
 *    promote any small map to chunk form and use the chunk setter, then
 *    try to demote the result back to small.
 *
 * The cursor accelerates the chunk-mode ascending path only; on any
 * mode transition it is reset (layout changed) so a stale seat is never
 * used.
 */
static uint64_t
__sm_add_dispatch(sm_t *map, uint64_t idx, sm_cursor_t *cur)
{
	uint64_t rc;
	const bool small = __sm_is_small(map);
	const bool empty_chunk = !small &&
	    (map->m_data_used < SM_SIZEOF_OVERHEAD ||
	        __sm_get_chunk_count(map) == 0);

	/* SM_IDX_MAX is the "not found" sentinel, not a member. */
	if (idx == SM_IDX_MAX) {
		errno = EINVAL;
		return (SM_IDX_MAX);
	}
	__sm_card_invalidate(map);

	if ((small || empty_chunk) && idx < SM_SMALL_MAX_BITS) {
		uint64_t rc_s;
		uint64_t maxbit;
		if (empty_chunk) {
			/* Turn the empty chunk-mode buffer into an empty
			 * small-mode map (zero words). */
			if (SM_SIZEOF_OVERHEAD > __sm_cap(map)) {
				errno = ENOSPC;
				return (SM_IDX_MAX);
			}
			__sm_small_set_header(map, 0);
			map->m_data_used = SM_SIZEOF_OVERHEAD;
		}
		rc_s = __sm_small_add(map, idx);
		if (rc_s == SM_IDX_MAX) {
			return (SM_IDX_MAX); /* ENOSPC: caller may grow */
		}
		if (cur != NULL) {
			__sm_cursor_reset(cur);
		}
		/* Keep the smaller of the two forms.  Promote only when the
		 * chunk form is strictly smaller (it then always fits). */
		maxbit = __sm_small_maximum(map);
		if (!__sm_small_is_better(maxbit, __sm_small_bytes(map),
		        __sm_small_chunk_bytes(map))) {
			(void)__sm_promote(map);
		}
		return (idx);
	}

	/* Chunk-mode path (promote first if the map is still small). */
	if (small) {
		if (!__sm_promote(map)) {
			return (SM_IDX_MAX); /* ENOSPC: caller may grow */
		}
		if (cur != NULL) {
			__sm_cursor_reset(cur);
		}
	}
	rc = __sm_map_set(map, idx, true, cur);
	if (rc != SM_IDX_MAX) {
		__sm_try_demote(map);
		if (cur != NULL && __sm_is_small(map)) {
			__sm_cursor_reset(cur);
		}
	}
	return (rc);
}

/**
 * @brief Sets the specified index in the sparsemap.
 *
 * This function marks the given index in the sparsemap as set.
 * Internally, it calls the __sm_map_set function with coalesce set to true.
 *
 * @param[in] map The sparsemap to modify.
 * @param[in] idx The index to set in the sparsemap.
 * @return The index that was set in the sparsemap.
 */
SM_HOT uint64_t
sm_add(sm_t *map, const uint64_t idx)
{
	if (map == NULL) {
		errno = EINVAL;
		return (SM_IDX_MAX);
	}
	return (__sm_add_dispatch(map, idx, NULL));
}

/* Cursor-threading variant of sm_add for O(N) bulk construction.
 * Internal only; the cursor accelerates ascending inserts.  See
 * sm_add_many / sm_add_many_grow.
 *
 * Inserting a new chunk or coalescing existing ones changes the chunk
 * layout at or before the cached chunk, which would leave a recorded
 * cursor offset pointing at the wrong place (or past the end after a
 * coalesce removes trailing chunks).  Detect that by comparing the
 * chunk count before and after: on any change, reset the cursor so the
 * next lookup walks from the head.  Pure in-place updates (the common
 * case in an ascending run) leave the count unchanged and keep the
 * cursor hot, preserving the amortized O(N) build cost. */
static uint64_t
__sm_add_c(sm_t *map, uint64_t idx, sm_cursor_t *cur)
{
	/*
	 * __sm_map_set re-seats *cur at the touched chunk (see its done:
	 * label), so we no longer reset the cursor here on a chunk-count
	 * change -- that blanket reset defeated the ascending-append fast
	 * path (every new chunk forced the next lookup back to the head,
	 * making bulk insert O(N^2)).  __sm_get_chunk_offset self-validates
	 * the seat, so an occasionally-stale cursor is safe.
	 */
	return (__sm_add_dispatch(map, idx, cur));
}

uint64_t
sm_add_grow(sm_t **mapp, uint64_t idx)
{
	sm_t * m;
	uint64_t rc;
	size_t new_cap;
	sm_t * grown;
	if (mapp == NULL || *mapp == NULL)
		return (SM_IDX_MAX);
	m = *mapp;
	rc = sm_add(m, idx);
	if (rc != SM_IDX_MAX || errno != ENOSPC)
		return (rc);

	/* ENOSPC: grow geometrically with a 4 KiB floor. */
	new_cap = sm_get_capacity(m) * 2;
	if (new_cap < 4096)
		new_cap = 4096;
	grown = sm_set_data_size(m, NULL, new_cap);
	if (grown == NULL)
		return (SM_IDX_MAX);
	*mapp = grown;
	return (sm_add(grown, idx));
}

uint64_t
sm_add_grow_cursor(sm_t **mapp, uint64_t idx, sm_cursor_t *cur)
{
	sm_t * m;
	uint64_t rc;
	size_t new_cap;
	sm_t * grown;
	if (mapp == NULL || *mapp == NULL)
		return (SM_IDX_MAX);
	m = *mapp;
	rc = __sm_add_c(m, idx, cur);
	if (rc != SM_IDX_MAX || errno != ENOSPC)
		return (rc);

	/* ENOSPC: grow geometrically with a 4 KiB floor. */
	new_cap = sm_get_capacity(m) * 2;
	if (new_cap < 4096)
		new_cap = 4096;
	grown = sm_set_data_size(m, NULL, new_cap);
	if (grown == NULL)
		return (SM_IDX_MAX);
	*mapp = grown;
	/* The grow relocated the buffer; the cursor's byte offset is stale. */
	if (cur != NULL)
		__sm_cursor_reset(cur);
	return (__sm_add_c(grown, idx, cur));
}

/**
 * @brief Sets or unsets a value in the sparse map at the specified index.
 *
 * This function assigns a value to the sparse map at the given index.
 * It either sets or unsets (clears) the bit at the index based on
 * the provided boolean value.
 *
 * @param[in,out] map Pointer to the sparsemap structure.
 * @param[in] idx The index at which the value should be assigned.
 * @param[in] value Boolean value indicating whether to set (true) or unset (false) the bit.
 * @return The index at which the operation was performed.
 */
uint64_t
sm_assign(sm_t *map, const uint64_t idx, const bool value)
{
	if (map == NULL) {
		errno = EINVAL;
		return (SM_IDX_MAX);
	}
	__sm_check_invariants(map);
	return (value ? sm_add(map, idx) : sm_remove(map, idx));
}

/* -------------------------------------------------------------------
 * Aggregate queries: minimum, maximum, fill factor, cardinality
 * ------------------------------------------------------------------- */

/**
 * @brief Retrieves the starting offset in a sparse map.
 *
 * This function determines the starting offset of a sparse map by analyzing
 * the chunks within the map. It iterates over the chunk data to find the first
 * payload of interest, either `ones` or `mixed`, and returns the corresponding
 * offset.
 *
 * @param[in] map Pointer to the sparse map to analyze.
 * @return The starting offset within the sparse map.
 */
uint64_t
sm_minimum(const sm_t *map)
{
	uint64_t offset;
	size_t count;
	uint8_t * p;
	uint64_t relative_position;
	__sm_chunk_t chunk;
	size_t m;
	if (map == NULL)
		return (0);
	if (__sm_is_small(map))
		return (__sm_small_minimum(map));
	__sm_check_invariants(map);
	offset = 0;
	count = __sm_get_chunk_count(map);
	if (count == 0) {
		return (0);
	}
	p = __sm_get_chunk_data(map, 0);
	relative_position = __sm_load_idx((const uint8_t *)p);
	p += SM_SIZEOF_OVERHEAD;
	__sm_chunk_init(&chunk, p);
	for (m = 0; m < sizeof(__sm_bitvec_t); m++) {
		int n;
		const uint8_t fb = __sm_desc_flag_byte(*chunk.m_data, m);
		for (n = 0; n < SM_FLAGS_PER_INDEX_BYTE; n++) {
			const size_t flags = SM_CHUNK_GET_FLAGS(fb, n);
			if (flags == SM_PAYLOAD_NONE) {
				/* A NONE slot carries no payload, but it still
				 * occupies its index slot: __sm_chunk_is_set
				 * locates a bit positionally as
				 * flags[idx / 64], so bit 64 lives in flag 1
				 * whether or not flag 0 is NONE.  Skipping
				 * without advancing made this report a
				 * position 64 bits too low per leading NONE
				 * slot, contradicting sm_contains and
				 * sm_next_member on the same map. */
				relative_position += SM_BITS_PER_VECTOR;
				continue;
			} else if (flags == SM_PAYLOAD_ZEROS) {
				relative_position += SM_BITS_PER_VECTOR;
			} else if (flags == SM_PAYLOAD_ONES) {
				offset = relative_position;
				goto done;
			} else if (flags == SM_PAYLOAD_MIXED) {
				const __sm_bitvec_t w = chunk.m_data[1 +
				    __sm_chunk_get_position(&chunk,
				        (m * SM_FLAGS_PER_INDEX_BYTE) + (size_t)n)];
				if (w != 0) {
					offset = relative_position +
					    (uint64_t)SM_CTZ64(w);
					goto done;
				}
				relative_position += SM_BITS_PER_VECTOR;
			}
		}
	}
done:;
	return (offset);
}

/**
 * @brief Retrieves the ending offset of a sparse map.
 *
 * This function calculates the ending offset of a sparse map by examining
 * each chunk within the map. If the map is empty, the offset is zero. For
 * maps with chunks, it iterates over the chunks, evaluating their data and
 * calculating the final offset.
 *
 * @param[in] map Pointer to the sparse map structure.
 * @return The calculated ending offset of the map.
 */
uint64_t
sm_maximum(const sm_t *map)
{
	size_t count;
	uint8_t * p;
	__sm_idx_t start;
	__sm_chunk_t chunk;
	uint64_t offset;
	uint64_t relative_position;
	size_t i;
	size_t m;
	if (map == NULL)
		return (0);
	if (__sm_is_small(map))
		return (__sm_small_maximum(map));
	__sm_check_invariants(map);
	count = __sm_get_chunk_count(map);

	/* the ending offset of a map containing zero chunks is zero */
	if (count == 0) {
		return (0);
	}

	/* the ending offset will be the last offset in the last chunk */
	p = __sm_get_chunk_data(map, 0);
	for (i = 0; i < count - 1; i++) {
		__sm_chunk_t chunk_s;
		p += SM_SIZEOF_OVERHEAD;
		__sm_chunk_init(&chunk_s, p);
		p += __sm_chunk_get_size(&chunk_s);
	}

	/* examine the last chunk in the map */
	start = __sm_load_idx((const uint8_t *)p);
	p += SM_SIZEOF_OVERHEAD;
	__sm_chunk_init(&chunk, p);

	/* the last chunk is sparse; examine it to find the highest set bit */
	offset = 0;
	relative_position = start;
	for (m = 0; m < sizeof(__sm_bitvec_t); m++) {
		int n;
		const uint8_t fb = __sm_desc_flag_byte(*chunk.m_data, m);
		for (n = 0; n < SM_FLAGS_PER_INDEX_BYTE; n++) {
			const size_t flags = SM_CHUNK_GET_FLAGS(fb, n);
			switch (flags) {
			case SM_PAYLOAD_ZEROS:
				relative_position += SM_BITS_PER_VECTOR;
				break;
			case SM_PAYLOAD_ONES:
				offset =
				    relative_position + SM_BITS_PER_VECTOR - 1;
				relative_position += SM_BITS_PER_VECTOR;
				break;
			case SM_PAYLOAD_MIXED: {
				int k;
				const __sm_bitvec_t w = chunk.m_data[1 +
				    __sm_chunk_get_position(&chunk,
				        (m * SM_FLAGS_PER_INDEX_BYTE) + (size_t)n)];
				int idx = 0;
				for (k = 0; k < SM_BITS_PER_VECTOR; k++) {
					if (w & (__sm_bitvec_t)1 << k) {
						idx = k;
					}
				}
				offset = relative_position + (uint64_t)idx;
				relative_position += SM_BITS_PER_VECTOR;
				break;
			}
			case SM_PAYLOAD_NONE:
				/* Occupies its index slot without carrying a
				 * payload; advance so positions stay in step
				 * with __sm_chunk_is_set's flags[idx / 64]
				 * addressing (see sm_minimum). */
				relative_position += SM_BITS_PER_VECTOR;
				continue;
			default:
				continue;
			}
		}
	}
	return (offset);
}

/**
 * @brief Calculates the fill factor of a sparse map.
 *
 * This function computes the fill factor of a sparse map by determining
 * the proportion of occupied elements relative to its total offset.
 * The fill factor is expressed as a percentage.
 *
 * @param[in] map A pointer to the sparse map.
 * @return The fill factor of the map as a percentage.
 */
double
sm_fill_factor(sm_t *map)
{
	size_t rank;
	uint64_t lo;
	uint64_t hi;
	uint64_t range;
	if (map == NULL)
		return (0.0);
	__sm_check_invariants(map);
	rank = sm_rank(map, 0, SM_IDX_MAX, true);
	if (rank == 0) {
		return (0.0);
	}
	lo = sm_minimum(map);
	hi = sm_maximum(map);
	/* range = hi - lo + 1 (the inclusive span containing all set bits). */
	range = hi - lo + 1;
	if (range == 0) {
		return (0.0);
	}
	return ((double)rank / (double)range);
}

/**
 * @brief Retrieves the serialized bitmap data from a sparse map.
 *
 * This function returns a pointer to the serialized data contained within
 * a given sparse map.
 *
 * @param[in] map Pointer to the sparse map from which to retrieve the data.
 * @return Pointer to the serialized bitmap data.
 */
void *
sm_get_data(const sm_t *map)
{
	if (map == NULL)
		return (NULL);
	return (map->m_data);
}

/**
 * @brief Retrieves the size of the sparse map.
 *
 * This function calculates the utilized size of the sparse map. If the stored
 * size does not match the calculated size, it updates the stored size.
 *
 * @param[in] map Pointer to the sparse map.
 * @return The size of the sparse map.
 */
size_t
sm_get_size(sm_t *map)
{
	if (map == NULL)
		return (0);
	/* No room for the chunk-count header (a 0..7-byte buffer): report
	 * the empty-map size, but never record it in m_data_used -- that
	 * would make later readers trust a header the buffer cannot hold. */
	if (SM_UNLIKELY(__sm_cap(map) < SM_SIZEOF_OVERHEAD))
		return (SM_SIZEOF_OVERHEAD);
	/* Small-set mode: the stored m_data_used is authoritative; the
	 * chunk-walking size recompute must not run on a small body. */
	if (__sm_is_small(map))
		return (map->m_data_used);
	if (map->m_data_used) {
		const size_t size = __sm_get_size_impl(map);
		if (size != map->m_data_used) {
			map->m_data_used = size;
		}
		__sm_when_diag({
			__sm_assert(
			    map->m_data_used == __sm_get_size_impl(map));
		});
		return (map->m_data_used);
	}
	return (map->m_data_used = __sm_get_size_impl(map));
}

/**
 * @brief Counts the number of elements in a sparse map.
 *
 * This function returns the total count of elements stored in a given
 * sm_t instance by invoking the sm_rank function.
 *
 * @param[in] map A pointer to the sm_t instance to be counted.
 * @return The total number of elements in the sparse map.
 */
size_t
sm_cardinality(sm_t *map)
{
	size_t count;
	if (map == NULL)
		return (sm_rank(map, 0, SM_IDX_MAX, true));
	if (__sm_card_is_valid(map))
		return (__sm_card_get(map));
	if (__sm_is_small(map))
		count = (size_t)__sm_small_cardinality(map);
	else
		count = sm_rank(map, 0, SM_IDX_MAX, true);
	__sm_card_store(map, count);
	return (count);
}

/* -------------------------------------------------------------------
 * Iteration: batched callback scan of set bits
 * ------------------------------------------------------------------- */

/**
 * @brief Scans through each chunk in a sparse map and applies a scanning function to each chunk.
 *
 * This function iterates over all chunks in the provided sparse map, initializing each chunk
 * and applying a user-defined scanning function to it. The scan may optionally skip a specified
 * number of elements before commencing.
 *
 * @param[in] map Pointer to the sparse map to scan.
 * @param[in] scanner User-defined scanning function to be applied to each chunk.
 * @param[in] skip Number of elements to skip before starting the scan.
 * @param[in] aux Auxiliary data to pass to the scanning function.
 */
void
sm_scan(const sm_t *map, void (*scanner)(uint64_t[], size_t, void *aux),
    size_t skip, void *aux)
{
	uint8_t * p;
	size_t count;
	size_t i;
	if (map == NULL)
		return;
	if (__sm_is_small(map)) {
		__sm_small_scan(map, scanner, skip, aux);
		return;
	}
	p = __sm_get_chunk_data(map, 0);
	count = __sm_get_chunk_count(map);

	for (i = 0; i < count; i++) {
		__sm_chunk_t chunk;
		size_t chunk_size;
		size_t skipped;
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);
		p += SM_SIZEOF_OVERHEAD;
		__sm_chunk_init(&chunk, p);
		chunk_size = __sm_chunk_get_size(&chunk);
		if (i + 1 < count) {
			SM_PREFETCH(p + chunk_size + SM_SIZEOF_OVERHEAD);
		}
		skipped = __sm_chunk_scan(&chunk, start, scanner, skip, aux);
		if (skip) {
			__sm_assert(skip >= skipped);
			skip -= skipped;
		}
		p += chunk_size;
	}
}

/**
 * @brief Creates a new sparsemap with all bits shifted by a given offset.
 *
 * Every set bit at position i in the source map appears at position i + offset
 * in the result. Bits shifted below 0 are silently dropped.
 *
 * Uses direct chunk copying and bit-vector shifting for performance.
 *
 * @param[in] map    The source sparsemap.
 * @param[in] offset Signed shift amount (positive = right, negative = left).
 * @return A newly allocated sparsemap (caller must free()), or NULL if all
 *         bits are shifted away or on allocation failure.
 */

/* -------------------------------------------------------------------
 * Set operations: scratch-word codec, append helpers, and the
 * bitwise shift (sm_offset)
 * ------------------------------------------------------------------- */

/**
 * @brief Expand a sparse chunk's descriptor into 32 full 64-bit words.
 *
 * For each of the 32 descriptor flag slots:
 *   ZEROS -> 0x0000000000000000
 *   ONES  -> 0xFFFFFFFFFFFFFFFF
 *   MIXED -> the stored bit-vector word
 *   NONE  -> 0x0000000000000000 (treated as zeros for shifting)
 *
 * @param[in]  chunk     The sparse chunk to expand.
 * @param[out] words     Array of 32 uint64_t to receive expanded words.
 * @param[out] cap_flags Array of 32 flags: 1 if slot contributes to capacity, 0 if NONE.
 */
static void
__sm_expand_sparse_chunk(const __sm_chunk_t *chunk, __sm_bitvec_t words[32],
    int cap_flags[32])
{
	int i;
	const __sm_bitvec_t desc = chunk->m_data[0];

	/* Pass 1: prefix-sum of MIXED flag counts to break serial vec_idx dependency. */
	int vec_offsets[SM_FLAGS_PER_INDEX];
	int running = 0;
	for (i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
		vec_offsets[i] = running;
		running +=
		    (((desc >> (i * 2)) & SM_FLAG_MASK) == SM_PAYLOAD_MIXED);
	}

	/* Pass 2: each slot computed independently using precomputed offsets. */
	for (i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
		const unsigned f = (desc >> (i * 2)) & SM_FLAG_MASK;
		cap_flags[i] = (f != SM_PAYLOAD_NONE);
		words[i] = (f == SM_PAYLOAD_MIXED) ?
		    chunk->m_data[1 + vec_offsets[i]] :
		    (f == SM_PAYLOAD_ONES) ? ~(__sm_bitvec_t)0 :
		                             0;
	}
}

/**
 * @brief Encode 32 expanded words back into a sparse chunk format.
 *
 * Builds a descriptor and vector array from the expanded words.
 * Only slots where cap_flags[i] == 1 contribute to capacity.
 *
 * @param[in]  words      Array of 32 uint64_t words.
 * @param[in]  cap_flags  Array of 32 flags indicating capacity slots.
 * @param[out] out_desc   The output descriptor word.
 * @param[out] out_vecs   Output vector array (up to 32 words).
 * @param[out] out_nvecs  Number of output vectors written.
 * @return true if the chunk has any set bits, false if completely empty.
 */
static bool
__sm_encode_sparse_chunk(__sm_bitvec_t words[32], int cap_flags[32],
    __sm_bitvec_t *out_desc, __sm_bitvec_t out_vecs[32], int *out_nvecs)
{
	__sm_bitvec_t desc;
	bool has_bits;
	unsigned flags[SM_FLAGS_PER_INDEX];
	int nvecs;
	int i;
	/* Slot 31 (the highest) must never be NONE.  NONE in bits 63:62 of
     the descriptor sets the top two bits to 01; keeping slot 31 as
     ZEROS/ONES/MIXED instead guarantees this build's descriptors are
     byte-compatible with the sparse-only wire format the sibling
     variant also reads.  Force it to ZEROS (adding 64 bits of harmless
     zero capacity) when needed. */
	if (!cap_flags[SM_FLAGS_PER_INDEX - 1]) {
		cap_flags[SM_FLAGS_PER_INDEX - 1] = 1;
		words[SM_FLAGS_PER_INDEX - 1] = 0;
	}

	/* Pass 1: compute flags for each slot (no inter-iteration dependency). */
	desc = 0;
	has_bits = false;
	for (i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
		unsigned f;
		if (!cap_flags[i]) {
			f = SM_PAYLOAD_NONE;
		} else if (words[i] == 0) {
			f = SM_PAYLOAD_ZEROS;
		} else if (words[i] == ~(__sm_bitvec_t)0) {
			f = SM_PAYLOAD_ONES;
			has_bits = true;
		} else {
			f = SM_PAYLOAD_MIXED;
			has_bits = true;
		}
		flags[i] = f;
		desc |= (__sm_bitvec_t)f << (i * 2);
	}

	/* Pass 2: compact MIXED vectors (serial but only touches MIXED slots). */
	nvecs = 0;
	for (i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
		if (flags[i] == SM_PAYLOAD_MIXED) {
			out_vecs[nvecs++] = words[i];
		}
	}

	*out_desc = desc;
	*out_nvecs = nvecs;
	return (has_bits);
}


/* ---- SIMD-accelerated word-level operations ---- */

#if defined(__AVX2__)
#include <immintrin.h>

static inline void
__sm_words_or(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	int i;
	for (i = 0; i < 32; i += 4) {
		__m256i va = _mm256_loadu_si256(
		    (const __m256i *)(const void *)&a[i]);
		__m256i vb = _mm256_loadu_si256(
		    (const __m256i *)(const void *)&b[i]);
		_mm256_storeu_si256((__m256i *)(void *)&dst[i],
		    _mm256_or_si256(va, vb));
	}
}

static inline void
__sm_words_and(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	int i;
	for (i = 0; i < 32; i += 4) {
		__m256i va = _mm256_loadu_si256(
		    (const __m256i *)(const void *)&a[i]);
		__m256i vb = _mm256_loadu_si256(
		    (const __m256i *)(const void *)&b[i]);
		_mm256_storeu_si256((__m256i *)(void *)&dst[i],
		    _mm256_and_si256(va, vb));
	}
}

static inline void
__sm_words_andnot(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	/* dst = a & ~b */
	int i;
	for (i = 0; i < 32; i += 4) {
		__m256i va = _mm256_loadu_si256(
		    (const __m256i *)(const void *)&a[i]);
		__m256i vb = _mm256_loadu_si256(
		    (const __m256i *)(const void *)&b[i]);
		_mm256_storeu_si256((__m256i *)(void *)&dst[i],
		    _mm256_andnot_si256(vb, va));
	}
}

#elif defined(__SSE2__)
#include <emmintrin.h>

static inline void
__sm_words_or(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	int i;
	for (i = 0; i < 32; i += 2) {
		__m128i va = _mm_loadu_si128(
		    (const __m128i *)(const void *)&a[i]);
		__m128i vb = _mm_loadu_si128(
		    (const __m128i *)(const void *)&b[i]);
		_mm_storeu_si128((__m128i *)(void *)&dst[i],
		    _mm_or_si128(va, vb));
	}
}

static inline void
__sm_words_and(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	int i;
	for (i = 0; i < 32; i += 2) {
		__m128i va = _mm_loadu_si128(
		    (const __m128i *)(const void *)&a[i]);
		__m128i vb = _mm_loadu_si128(
		    (const __m128i *)(const void *)&b[i]);
		_mm_storeu_si128((__m128i *)(void *)&dst[i],
		    _mm_and_si128(va, vb));
	}
}

static inline void
__sm_words_andnot(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	int i;
	/* dst = a & ~b */
	for (i = 0; i < 32; i += 2) {
		__m128i va = _mm_loadu_si128(
		    (const __m128i *)(const void *)&a[i]);
		__m128i vb = _mm_loadu_si128(
		    (const __m128i *)(const void *)&b[i]);
		_mm_storeu_si128((__m128i *)(void *)&dst[i],
		    _mm_andnot_si128(vb, va));
	}
}

#else

/* Scalar fallback */
static inline void
__sm_words_or(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	for (int i = 0; i < 32; i++)
		dst[i] = a[i] | b[i];
}

static inline void
__sm_words_and(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	for (int i = 0; i < 32; i++)
		dst[i] = a[i] & b[i];
}

static inline void
__sm_words_andnot(__sm_bitvec_t dst[32], const __sm_bitvec_t a[32],
    const __sm_bitvec_t b[32])
{
	for (int i = 0; i < 32; i++)
		dst[i] = a[i] & ~b[i];
}

#endif

/**
 * @brief Ensure the result map has enough capacity, growing if needed.
 *
 * @param[in,out] resultp Pointer to result map pointer (may be reallocated).
 * @param[in]     needed  Number of bytes needed beyond current usage.
 * @return true on success, false on allocation failure.
 */
static bool
__sm_ensure_capacity(sm_t **resultp, size_t needed)
{
	size_t cap;
	size_t new_cap;
	sm_t * grown;
	sm_t *result = *resultp;
	/*
	 * Defense in depth: the only callers of __sm_ensure_capacity are
	 * sm_union / _intersection / _difference, which all allocate
	 * their result via sm_create() (SM_OWNED_CONTIGUOUS).  Any
	 * other lineage at this point indicates an internal API misuse --
	 * fail loudly under SPARSEMAP_TESTING so we catch it now rather
	 * than three operations downstream when the heap finally notices.
	 */
	__sm_when_diag({
		__sm_assert(__sm_kind(result) == SM_OWNED_CONTIGUOUS ||
		    __sm_kind(result) == SM_OWNED_SPLIT);
	});
	if (result->m_data_used + needed <= __sm_cap(result)) {
		return (true);
	}
	cap = __sm_cap(result);
	new_cap = cap + (cap / 2 > needed ? cap / 2 : needed + 256);
	grown = sm_set_data_size(result, NULL, new_cap);
	if (grown == NULL) {
		return (false);
	}
	*resultp = grown;
	return (true);
}

/**
 * @brief Append a sparse chunk (descriptor + vectors) to the result map.
 *
 * @param[in,out] resultp    Pointer to result map pointer (may grow).
 * @param[in]     start      The chunk start offset (__sm_idx_t).
 * @param[in]     desc       The descriptor word.
 * @param[in]     vecs       The vector array.
 * @param[in]     nvecs      Number of vectors.
 * @return true on success, false on allocation failure.
 */
static bool
__sm_append_sparse_chunk(sm_t **resultp, __sm_idx_t start, __sm_bitvec_t desc,
    __sm_bitvec_t vecs[], int nvecs)
{
	sm_t * result;
	int i;
	const size_t chunk_size = SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) +
	    (size_t)nvecs * sizeof(__sm_bitvec_t);
	if (!__sm_ensure_capacity(resultp, chunk_size)) {
		return (false);
	}
	result = *resultp;

	/* Capacity for the whole chunk was reserved above, so these appends
	 * cannot fail; check anyway so the invariant is enforced by the
	 * compiler rather than by a comment. */
	if (SM_UNLIKELY(!__sm_append_data(result, (const uint8_t *)&start,
	        SM_SIZEOF_OVERHEAD))) {
		return (false);
	}
	if (SM_UNLIKELY(!__sm_append_data(result, (const uint8_t *)&desc,
	        sizeof(__sm_bitvec_t)))) {
		return (false);
	}
	for (i = 0; i < nvecs; i++) {
		if (SM_UNLIKELY(!__sm_append_data(result,
		        (const uint8_t *)&vecs[i], sizeof(__sm_bitvec_t)))) {
			return (false);
		}
	}

	__sm_set_chunk_count(result, __sm_get_chunk_count(result) + 1);
	return (true);
}

/**
 * @brief Append a run of all-ONES sparse chunks to the result map.
 *
 * A whole-chunk span of set bits is stored as one or more all-ONES
 * sparse chunks (descriptor ~0, no payload words).  Callers only ever
 * pass a chunk-aligned capacity equal to the length (whole 2048-bit
 * windows); any sub-chunk remainder is emitted through the words path
 * by the caller (see __sm_emit_ones_run).
 *
 * @param[in,out] resultp    Pointer to result map pointer (may grow).
 * @param[in]     start      The (chunk-aligned) start offset.
 * @param[in]     capacity   Span capacity in bits (== length, chunk-aligned).
 * @param[in]     length     Span length in bits (whole 2048-bit windows).
 * @return true on success, false on allocation failure.
 */
static bool
__sm_append_ones_chunks(sm_t **resultp, __sm_idx_t start, size_t capacity,
    size_t length)
{
	__sm_bitvec_t ones;
	size_t off;
	__sm_assert(capacity == length);
	__sm_assert(length % SM_CHUNK_MAX_CAPACITY == 0);
	(void)capacity;

	ones = ~(__sm_bitvec_t)0; /* all 32 slots ONES */
	for (off = 0; off < length; off += SM_CHUNK_MAX_CAPACITY) {
		/* All-ONES chunk: descriptor only, zero MIXED payload words. */
		if (!__sm_append_sparse_chunk(resultp,
		        (__sm_idx_t)((size_t)start + off), ones, NULL, 0)) {
			return (false);
		}
	}
	return (true);
}


/**
 * @brief Ordered, collision-free emitter for sm_offset's output.
 *
 * sm_offset shifts each source chunk into an aligned output chunk, but
 * a shift is not a bijection on chunk boundaries: several source chunks
 * can land in the SAME output chunk.  The function used to have five
 * independent append sites plus a carry buffer, each deciding its own
 * start, so two of them could append chunks with identical start
 * offsets.  That breaks the ascending-start invariant every reader
 * assumes: sm_validate rejects the map, sm_contains misses bits that
 * sm_next_member still yields, and sm_deserialize refuses the map's own
 * serialized bytes.
 *
 * Everything now goes through one emitter that keeps at most one
 * pending sparse output chunk.  Emits for the pending start are merged
 * into it; an emit for a later start flushes the pending chunk first.
 * Since output starts are produced in ascending order, one slot is
 * enough, and each output chunk is appended exactly once.
 *
 * An all-ONES run emit always begins a fresh output chunk, so it just
 * flushes whatever is pending; instrumenting the whole sweep showed the
 * only collisions that ever occur are sparse-into-sparse and
 * sparse-after-run, never anything into a claimed run window.
 */
typedef struct __sm_emitter {
	sm_t **resultp;
	__sm_bitvec_t words[32];
	int cap[32];
	__sm_idx_t start;
	bool pending;
	/* Span of the all-ONES run emitted most recently.  Its capacity is
	 * whole output chunks, so a later sparse emit can target a start
	 * that already lies inside it. */
	__sm_idx_t run_start;
	size_t run_end;
	size_t run_len;
	bool have_run;
} __sm_emitter_t;

static bool
__sm_emit_flush(__sm_emitter_t *e)
{
	__sm_bitvec_t desc;
	__sm_bitvec_t vecs[32];
	int nvecs;
	if (!e->pending) {
		return (true);
	}
	e->pending = false;
	if (!__sm_encode_sparse_chunk(e->words, e->cap, &desc, vecs, &nvecs)) {
		return (true); /* nothing set: emit nothing */
	}
	/* Once a sparse chunk lands after the run, that run is no longer the
	 * tail and its span must not absorb later emits. */
	e->have_run = false;
	return (__sm_append_sparse_chunk(e->resultp, e->start, desc, vecs,
	    nvecs));
}

/* Emit (or merge) a sparse output chunk given as expanded words. */
static bool
__sm_emit_words(__sm_emitter_t *e, __sm_idx_t start,
    const __sm_bitvec_t words[32], const int cap[32])
{
	if (e->pending && e->start == start) {
		int i;
		for (i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
			if (cap[i]) {
				e->words[i] |= words[i];
				e->cap[i] = 1;
			}
		}
		return (true);
	}

	/* A start inside the last run's span must never happen: an all-ONES
	 * run is emitted with capacity == its own (chunk-aligned) length, so
	 * it never advertises indices it does not own.  Assert it rather
	 * than trying to repair it here -- an overlap means an upstream
	 * caller computed the wrong output start. */
	__sm_assert(!(e->have_run && (size_t)start >= (size_t)e->run_start &&
	    (size_t)start < e->run_end));

	if (!__sm_emit_flush(e)) {
		return (false);
	}
	memcpy(e->words, words, sizeof(e->words));
	memcpy(e->cap, cap, sizeof(e->cap));
	e->start = start;
	e->pending = true;
	return (true);
}

/* Emit a whole-chunk run of set bits as one or more all-ONES sparse
 * output chunks.
 *
 * A run of `length` bits starting at `start` is emitted as the whole
 * 2048-bit windows it fills (as all-ONES sparse chunks via
 * __sm_append_ones_chunks) plus a sub-chunk remainder handled through
 * the words path.  The caller passes capacity == length; only the whole
 * output chunks are emitted here, the remainder rolls into a following
 * emit for the same chunk.
 *
 * The span (run_start/run_end) is recorded so __sm_emit_words can
 * assert a later start never lands inside the just-emitted run.
 */
static bool
__sm_emit_ones_run(__sm_emitter_t *e, __sm_idx_t start, size_t capacity,
    size_t length)
{
	size_t full;
	size_t rem;
	__sm_bitvec_t w[32];
	int c[32];
	int i;
	size_t bit;
	/*
	 * Emit only the whole output chunks the run actually fills (as
	 * all-ONES sparse chunks) and pass any sub-chunk remainder to the
	 * words path, where a following emit for the same output chunk merges
	 * with it.  A partial trailing run must not claim indices it has no
	 * bits for, so the whole-chunk part is always a multiple of
	 * SM_CHUNK_MAX_CAPACITY.
	 */
	(void)capacity;

	full = (length / SM_CHUNK_MAX_CAPACITY) *
	    SM_CHUNK_MAX_CAPACITY;
	if (full > 0) {
		if (!__sm_emit_flush(e)) {
			return (false);
		}
		if (!__sm_append_ones_chunks(e->resultp, start, full, full)) {
			return (false);
		}
		e->run_start = start;
		e->run_end = (size_t)__sm_end(start, full);
		e->run_len = full;
		e->have_run = true;
	}

	rem = length - full;
	if (rem == 0) {
		return (true);
	}

	memset(w, 0, sizeof(w));
	/* Full 32-slot capacity (see __sm_emit_run): a partial trailing run
	 * still claims the whole chunk window, encoding the unset tail slots
	 * as ZEROS rather than NONE. */
	for (i = 0; i < 32; i++)
		c[i] = 1;
	for (bit = 0; bit < rem; bit += SM_BITS_PER_VECTOR) {
		const size_t slot = bit / SM_BITS_PER_VECTOR;
		const size_t n = (rem - bit < SM_BITS_PER_VECTOR) ?
		    rem - bit :
		    SM_BITS_PER_VECTOR;
		w[slot] = (n == SM_BITS_PER_VECTOR) ?
		    ~(__sm_bitvec_t)0 :
		    (((__sm_bitvec_t)1 << n) - 1);
	}
	/* The remainder starts exactly where the run part ended, so it is
	 * outside that run's span; clear the marker so the assertion in
	 * __sm_emit_words (which forbids a start *inside* the span) is not
	 * confused by the boundary case. */
	e->have_run = false;
	return (__sm_emit_words(e, (__sm_idx_t)((size_t)start + full), w, c));
}

/*
 * Emit an arbitrary half-open run [lo, hi) of set bits through the
 * ordered emitter in O(output chunks), NOT O(hi-lo).
 *
 * The set-algebra ops (sm_xor, sm_extract_range) consume operand runs
 * with __sm_run_next and used to materialise each survivor one bit at a
 * time via sm_add -- so a single [0, 2^31) run cost 2^31 add calls and
 * the amplification DoS lived here, not in the run reader.  A run is
 * instead split at chunk boundaries: a sub-chunk head goes to the words
 * path, the whole output chunks it fills go out as all-ONES sparse
 * chunks, and the sub-chunk tail is handled by __sm_emit_ones_run's own
 * remainder path.
 *
 * runs are delivered ascending and non-overlapping, so consecutive
 * emits for the same output chunk merge in __sm_emit_words and each
 * output chunk is appended exactly once.  Capacity stays chunk-aligned
 * (see __sm_emit_ones_run) so the coalesce pass never walks off the buffer.
 */
static bool
__sm_emit_run(__sm_emitter_t *e, uint64_t lo, uint64_t hi)
{
	uint64_t body_lo;
	size_t len;
	if (lo >= hi) {
		return (true);
	}

	/* Sub-chunk head: bits from lo up to the next chunk boundary.  The
	 * boundary is measured as a distance from lo, because rounding lo up
	 * wraps to 0 in the top chunk, whose boundary is 2^64. */
	body_lo = lo;
	if (lo % SM_CHUNK_MAX_CAPACITY != 0) {
		int i;
		uint64_t bit;
		const uint64_t room =
		    SM_CHUNK_MAX_CAPACITY - lo % SM_CHUNK_MAX_CAPACITY;
		const uint64_t head_hi = hi - lo <= room ? hi : lo + room;
		const __sm_idx_t chunk_start =
		    (__sm_idx_t)(lo - (lo % SM_CHUNK_MAX_CAPACITY));
		__sm_bitvec_t w[32];
		int c[32];
		memset(w, 0, sizeof(w));
		/* Full 32-slot capacity: a
		 * sparse chunk with front slots marked NONE (cap 0) instead of
		 * ZEROS confuses the reader, which then finds only the first set
		 * bit.  Every emitted sparse chunk claims the whole 2048-bit
		 * window and encodes absent slots as ZEROS. */
		for (i = 0; i < 32; i++)
			c[i] = 1;
		for (bit = lo; bit < head_hi; bit++) {
			const uint64_t off = bit - chunk_start;
			const size_t slot = off / SM_BITS_PER_VECTOR;
			w[slot] |= (__sm_bitvec_t)1
			    << (off % SM_BITS_PER_VECTOR);
		}
		if (!__sm_emit_words(e, chunk_start, w, c)) {
			return (false);
		}
		body_lo = head_hi;
	}

	if (body_lo >= hi) {
		return (true);
	}

	/* body_lo is now chunk-aligned; __sm_emit_ones_run emits the whole
	 * output chunks as all-ONES sparse chunks and forwards its own
	 * sub-chunk tail to the words path.  capacity == length keeps the
	 * run chunk-aligned. */
	len = (size_t)(hi - body_lo);
	return (__sm_emit_ones_run(e, (__sm_idx_t)body_lo, len, len));
}

/*
 * Absolute shifted start of a chunk under sm_offset, computed without a
 * signed-overflow intermediate.
 *
 * sm_offset shifts every bit at position i to i + offset.  A chunk that
 * starts at src_start lands at src_start + offset, which overflows a
 * ssize_t intermediate for a start at or above 2^63 or a large
 * |offset| even though the true value fits (the caller's ERANGE and
 * drop guards already bound the surviving range to [0, SM_IDX_MAX)).
 * Return the shifted start as an unsigned magnitude plus a sign flag:
 * *neg is true when the start falls below bit 0 (only possible for
 * offset < 0), and the result is then how far below 0 it is.
 */
static inline uint64_t
__sm_offset_abs_start(uint64_t src_start, ssize_t offset, bool *neg)
{
	uint64_t down;
	if (offset >= 0) {
		/* The offset > 0 ERANGE guard already proved max + offset <
		 * SM_IDX_MAX and src_start <= max, so this cannot wrap. */
		*neg = false;
		return (src_start + (uint64_t)offset);
	}
	/* offset < 0: |offset| as an unsigned magnitude (SSIZE_MIN-safe). */
	down = (uint64_t)(-(offset + 1)) + 1;
	if (src_start >= down) {
		*neg = false;
		return (src_start - down);
	}
	*neg = true;
	return (down - src_start);
}

sm_t *
sm_offset(const sm_t *map, ssize_t offset)
{
	size_t count;
	size_t chunk_bytes;
	size_t cap;
	sm_t * result;
	__sm_emitter_t em;
	uint8_t * p;
	size_t i;
	__sm_check_invariants(map);
	if (map == NULL) {
		return (NULL);
	}
	if (__sm_is_small(map)) {
		/* This builds a new map anyway, so the chunk-mode copy costs
		 * no more than the result; the query paths read small maps
		 * directly instead. */
		sm_t * r;
		sm_t *m = __sm_materialize(map);
		if (m == NULL)
			return (NULL);
		r = sm_offset(m, offset);
		sm_free(m);
		if (r != NULL)
			__sm_try_demote(r);
		return (r);
	}

	/* offset == 0: just copy */
	if (offset == 0) {
		return (sm_copy(map));
	}

	count = __sm_get_chunk_count(map);
	if (count == 0) {
		return (NULL);
	}

	/* A shift that moves the top member onto SM_IDX_MAX or past it
	 * leaves the member universe [0, 2^64 - 1): ERANGE, as for any
	 * other out-of-range shift. */
	if (offset > 0) {
		uint64_t max = sm_maximum(map);
		if (max >= SM_IDX_MAX - (uint64_t)offset) {
			errno = ERANGE;
			return (NULL);
		}
	}

	/* Check if all bits would be shifted below 0 */
	if (offset < 0) {
		uint64_t max = sm_maximum(map);
		/* Compare in unsigned to avoid the (ssize_t)max cast UB when a
		 * valid map holds a bit above 2^63: the whole map is dropped
		 * iff every bit shifts below 0, i.e. max < |offset|. */
		const uint64_t neg = (uint64_t)(-(offset + 1)) + 1; /* |offset| */
		if (max < neg) {
			return (NULL); /* all bits shifted away */
		}
	}

	/* Allocate result.
	 *
	 * A shift is not a bijection on chunk boundaries: an unaligned
	 * offset splits each source chunk across two output chunks, so the
	 * result can need roughly twice the source's bytes plus a chunk of
	 * slack.  Sizing it at exactly map->m_data_used left the buffer
	 * completely full, and the coalesce pass at the end works in place
	 * on the byte stream -- with zero headroom its memmove read one
	 * chunk past the allocation (ASan heap-buffer-overflow).
	 */
	chunk_bytes = SM_SIZEOF_OVERHEAD +
	    sizeof(__sm_bitvec_t) * (SM_FLAGS_PER_INDEX + 1);
	cap = map->m_data_used * 2 + chunk_bytes;
	result = sparsemap(cap > 1024 ? cap : 1024);
	if (result == NULL) {
		return (NULL);
	}

	/* Single ordered emitter: holds at most one pending output chunk so
	 * that several source pieces landing in the same aligned output
	 * chunk are merged instead of appended twice.  Replaces the old
	 * carry buffer, which only serialised the forward-overflow case. */
	memset(&em, 0, sizeof(em));
	em.resultp = &result;
	em.pending = false;

	/* Walk source chunks */
	p = __sm_get_chunk_data(map, 0);

	for (i = 0; i < count; i++) {
		__sm_chunk_t chunk;
		size_t chunk_size;
		const __sm_idx_t src_start = __sm_load_idx((const uint8_t *)p);
		p += SM_SIZEOF_OVERHEAD;
		__sm_chunk_init(&chunk, p);
		chunk_size = __sm_chunk_get_size(&chunk);

		/* Every chunk is sparse in this build: expand to 32 words,
		 * compute final absolute positions, place into the correct
		 * output chunk(s). */
		{
			bool neg;
			uint64_t mag;
			uint64_t out_aligned;
			ssize_t intra_shift;
			__sm_bitvec_t main_words[32];
			int main_cap[32];
			__sm_bitvec_t overflow_words[32];
			int overflow_cap[32];
			bool has_overflow;
			int w;
			__sm_bitvec_t words[32];
			int cf[32];
			__sm_expand_sparse_chunk(&chunk, words, cf);

			/* Each bit at absolute position src_start + slot*64 + bit_offset
         maps to src_start + offset + slot*64 + bit_offset in the output.

         The output chunk aligned start = align(src_start + offset).
         The intra-chunk shift = (src_start + offset) - aligned_start.

         If intra >= 0: right-shift within the 32-word array, overflow to carry.
         If intra < 0 (new start negative): left-shift, dropping low bits. */

			mag = __sm_offset_abs_start(src_start, offset, &neg);

			/* Compute aligned output chunk start and intra-chunk
			 * shift.  All arithmetic is unsigned; intra_shift for
			 * the non-negative case is a within-chunk remainder
			 * (0 .. SM_CHUNK_MAX_CAPACITY - 1), so it fits ssize_t.
			 * A start below bit 0 (neg) becomes a left shift by
			 * `mag` bits, dropping the low bits; a drop of a whole
			 * source chunk or more leaves nothing. */
			if (!neg) {
				out_aligned = __sm_get_chunk_aligned_offset(mag);
				intra_shift = (ssize_t)(mag - out_aligned);
			} else {
				if (mag >= SM_CHUNK_MAX_CAPACITY) {
					p += chunk_size;
					continue;
				}
				out_aligned = 0;
				intra_shift = -(ssize_t)mag;
			}

			/* Build the shifted 32-word arrays for main output chunk and overflow */
			memset(main_words, 0, sizeof(main_words));
			memset(main_cap, 0, sizeof(main_cap));
			memset(overflow_words, 0, sizeof(overflow_words));
			memset(overflow_cap, 0, sizeof(overflow_cap));

			if (intra_shift >= 0) {
				int wd;
				size_t w_s;
				/* Right-shift by intra_shift bits */
				size_t word_shift =
				    (size_t)intra_shift / SM_BITS_PER_VECTOR;
				size_t bit_rem =
				    (size_t)intra_shift % SM_BITS_PER_VECTOR;

				for (wd = 31; wd >= 0; wd--) {
					size_t dst;
					if (!cf[wd] && words[wd] == 0)
						continue;

					dst = (size_t)wd + word_shift;
					if (bit_rem == 0) {
						if (dst < 32) {
							main_words[dst] |=
							    words[wd];
							main_cap[dst] = 1;
						} else if (dst < 64) {
							overflow_words[dst -
							    32] |= words[wd];
							overflow_cap[dst - 32] =
							    1;
						}
					} else {
						size_t dst1;
						__sm_bitvec_t lo = words[wd]
						    << bit_rem;
						__sm_bitvec_t hi = words[wd] >>
						    (SM_BITS_PER_VECTOR -
						        bit_rem);

						if (dst < 32) {
							main_words[dst] |= lo;
							main_cap[dst] = 1;
						} else if (dst < 64) {
							overflow_words[dst -
							    32] |= lo;
							overflow_cap[dst - 32] =
							    1;
						}

						dst1 = dst + 1;
						if (dst1 < 32) {
							main_words[dst1] |= hi;
							main_cap[dst1] = 1;
						} else if (dst1 < 64) {
							overflow_words[dst1 -
							    32] |= hi;
							overflow_cap[dst1 -
							    32] = 1;
						}
					}
				}

				/* Mark shifted-in zero slots as capacity */
				for (w_s = 0; w_s < word_shift && w_s < 32;
				     w_s++) {
					main_cap[w_s] = 1;
				}
			} else {
				size_t w_s;
				/* intra_shift < 0: left-shift by |intra_shift| bits (dropping low bits) */
				size_t drop = (size_t)(-intra_shift);
				size_t word_drop = drop / SM_BITS_PER_VECTOR;
				size_t bit_drop = drop % SM_BITS_PER_VECTOR;

				for (w_s = 0; w_s < 32; w_s++) {
					size_t src_w = w_s + word_drop;
					if (src_w >= 32)
						break;
					main_cap[w_s] = 1;
					if (bit_drop == 0) {
						main_words[w_s] = words[src_w];
					} else {
						main_words[w_s] =
						    words[src_w] >> bit_drop;
						if (src_w + 1 < 32) {
							main_words[w_s] |=
							    words[src_w + 1]
							    << (SM_BITS_PER_VECTOR -
							           bit_drop);
						}
					}
				}
			}

			/* Emit the main chunk, then any overflow into the
			 * next one.  Both go through the ordered emitter, so a
			 * chunk that another source piece also targets is
			 * merged rather than appended a second time. */
			if (!__sm_emit_words(&em, (__sm_idx_t)out_aligned,
			        main_words, main_cap)) {
				sm_free(result);
				return (NULL);
			}

			has_overflow = false;
			for (w = 0; w < (int)SM_FLAGS_PER_INDEX; w++) {
				if (overflow_cap[w] && overflow_words[w] != 0) {
					has_overflow = true;
					break;
				}
			}
			if (has_overflow) {
				if (!__sm_emit_words(&em,
				        (__sm_idx_t)out_aligned +
				            SM_CHUNK_MAX_CAPACITY,
				        overflow_words, overflow_cap)) {
					sm_free(result);
					return (NULL);
				}
			}
		}

		p += chunk_size;
	}

	/* Flush the last pending output chunk. */
	if (!__sm_emit_flush(&em)) {
		sm_free(result);
		return (NULL);
	}
	result = *em.resultp;

	/* If no chunks were added, return NULL */
	if (__sm_get_chunk_count(result) == 0) {
		sm_free(result);
		return (NULL);
	}

	/* Coalesce adjacent chunks where possible */
	__sm_coalesce_map(result);

	return (result);
}

/* -------------------------------------------------------------------
 * Predicates and member-by-member iteration
 * ------------------------------------------------------------------- */

bool
sm_is_empty(const sm_t *map)
{
	if (map == NULL) {
		return (true);
	}
	if (__sm_is_small(map)) {
		return (__sm_small_is_empty(map));
	}
	__sm_check_invariants(map);
	return (__sm_get_chunk_count(map) == 0);
}

/*
 * Iterate set bits in `chunk` (anchored at absolute `start`),
 * starting strictly after `lower_excl`.  Returns the first set bit
 * found, or SM_IDX_MAX if none.  Pass UINT64_MAX as lower_excl to
 * mean "start before bit 0" (return the first bit at or after start).
 */
static __sm_idx_t
__sm_chunk_next_set(const __sm_chunk_t *chunk, uint64_t start,
    uint64_t lower_excl)
{
	size_t v;
	for (v = 0; v < SM_FLAGS_PER_INDEX; v++) {
		size_t flags;
		__sm_bitvec_t w;
		uint64_t skip;
		__sm_bitvec_t masked;
		const uint64_t vec_lo = start + v * SM_BITS_PER_VECTOR;
		const uint64_t vec_hi = vec_lo + SM_BITS_PER_VECTOR - 1;
		if (lower_excl != UINT64_MAX && vec_hi <= lower_excl) {
			continue;
		}
		flags = SM_CHUNK_GET_FLAGS(chunk->m_data[0], v);
		if (flags == SM_PAYLOAD_NONE || flags == SM_PAYLOAD_ZEROS) {
			continue;
		}
		if (flags == SM_PAYLOAD_ONES) {
			if (lower_excl == UINT64_MAX || lower_excl < vec_lo) {
				return (vec_lo);
			}
			return (lower_excl + 1);
		}
		/* SM_PAYLOAD_MIXED: scan the payload word for a 1-bit > lower_excl. */
		w = chunk->m_data[1 + __sm_chunk_get_position(chunk, v)];
		skip = 0;
		if (lower_excl != UINT64_MAX && lower_excl >= vec_lo) {
			skip = lower_excl - vec_lo + 1;
			if (skip >= SM_BITS_PER_VECTOR)
				continue;
		}
		masked = w & (~(__sm_bitvec_t)0 << skip);
		if (masked == 0) {
			continue;
		}
		return (vec_lo + (uint64_t)SM_CTZ64(masked));
	}
	return (SM_IDX_MAX);
}

/*
 * Iterate set bits in `chunk` (anchored at absolute `start`),
 * looking for the highest set bit strictly less than `upper_excl`.
 */
static __sm_idx_t
__sm_chunk_prev_set(const __sm_chunk_t *chunk, uint64_t start,
    uint64_t upper_excl)
{
	ssize_t v;
	for (v = SM_FLAGS_PER_INDEX - 1; v >= 0; v--) {
		size_t flags;
		uint64_t vec_hi;
		__sm_bitvec_t w;
		const uint64_t vec_lo =
		    start + (uint64_t)v * SM_BITS_PER_VECTOR;
		if (vec_lo >= upper_excl) {
			continue;
		}
		flags = SM_CHUNK_GET_FLAGS(chunk->m_data[0], (size_t)v);
		if (flags == SM_PAYLOAD_NONE || flags == SM_PAYLOAD_ZEROS) {
			continue;
		}
		vec_hi = vec_lo + SM_BITS_PER_VECTOR - 1;
		if (flags == SM_PAYLOAD_ONES) {
			return (
			    upper_excl - 1 < vec_hi ? upper_excl - 1 : vec_hi);
		}
		/* SM_PAYLOAD_MIXED. */
		w = chunk
		        ->m_data[1 + __sm_chunk_get_position(chunk, (size_t)v)];
		if (upper_excl - 1 < vec_hi) {
			const uint64_t bits_to_keep = upper_excl - vec_lo;
			if (bits_to_keep == 0)
				continue;
			w &= (~(__sm_bitvec_t)0) >>
			    (SM_BITS_PER_VECTOR - bits_to_keep);
		}
		if (w == 0)
			continue;
		return (vec_lo +
		    (uint64_t)(SM_BITS_PER_VECTOR - 1 - (size_t)SM_CLZ64(w)));
	}
	return (SM_IDX_MAX);
}

uint64_t
sm_next_member(const sm_t *map, uint64_t prev_idx, sm_cursor_t *cur)
{
	size_t count;
	uint8_t * base;
	uint8_t * p;
	size_t stream_end;
	if (map == NULL)
		return (SM_IDX_MAX);
	if (__sm_is_small(map)) {
		size_t w;
		uint64_t word;
		/* First set bit strictly greater than prev_idx (SM_IDX_MAX
		 * means "from the start"). */
		const __sm_bitvec_unaligned_t *words = __sm_small_words(map);
		const size_t n = __sm_small_nwords(map);
		const uint64_t from = (prev_idx == SM_IDX_MAX) ? 0
		                                               : prev_idx + 1;
		if (prev_idx != SM_IDX_MAX &&
		    prev_idx >= (uint64_t)n * 64)
			return (SM_IDX_MAX);
		w = (size_t)(from / 64);
		if (w >= n)
			return (SM_IDX_MAX);
		word = words[w] &
		    (~(uint64_t)0 << (from % 64));
		for (;;) {
			if (word != 0)
				return ((uint64_t)w * 64 +
				    (uint64_t)SM_CTZ64(word));
			if (++w >= n)
				return (SM_IDX_MAX);
			word = words[w];
		}
	}
	__sm_check_invariants(map);
	count = __sm_get_chunk_count(map);
	if (count == 0)
		return (SM_IDX_MAX);

	base = __sm_get_chunk_data(map, 0);
	p = base;
	stream_end = map->m_data_used - SM_SIZEOF_OVERHEAD;

	/*
	 * Cursor fast-path.  Sequential forward iteration
	 *   while ((i = sm_next_member(map, i, &c)) != SM_IDX_MAX) ...
	 * is the dominant scan-side hot path.  Without a cursor each
	 * call walks from chunk 0 -- O(N) per call, O(N^2) per scan.
	 * Resume from the cached chunk when prev_idx is not earlier than
	 * that chunk's start, after checking that the word at the cached
	 * offset is the start we recorded, as __sm_get_chunk_offset does.
	 * Through 5.8.4 this resumed unchecked, so a cursor left at a
	 * shifted offset walked from the middle of a chunk.
	 */
	if (prev_idx != SM_IDX_MAX && cur != NULL &&
	    cur->offset != SIZE_MAX &&
	    cur->offset + sizeof(__sm_idx_t) <= stream_end &&
	    cur->start_idx <= prev_idx &&
	    __sm_load_idx(base + cur->offset) == cur->start_idx) {
		p = base + cur->offset;
	}

	while ((size_t)(p - base) < stream_end) {
		size_t cap;
		uint64_t hit;
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		cap = __sm_chunk_get_capacity(&chunk);
		/* Skip chunks entirely below the lower bound. */
		if (prev_idx != SM_IDX_MAX &&
		    __sm_end(start, cap - 1) <= prev_idx) {
			p += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
			continue;
		}
		hit = __sm_chunk_next_set(&chunk, start, prev_idx);
		if (hit != SM_IDX_MAX) {
			if (cur != NULL) {
				cur->offset = (size_t)(p - base);
				cur->start_idx = start;
			}
			return (hit);
		}
		p += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
	}
	return (SM_IDX_MAX);
}

/*
 * The chunk that immediately precedes the chunk at `last`, found by a
 * walk from chunk 0 (a walk may start only there or at a verified cursor
 * offset, and cur->prev_offset is never trusted).  *before receives the
 * chunk preceding the result, or NULL.  `last` must be greater than
 * `base`.  If `last` is not a chunk start (a stale cursor whose offset
 * held a matching payload word), the walk stops at the chunk that
 * contains it, so the result is always a real chunk and the loop always
 * ends: every step advances by at least one chunk header.
 */
static uint8_t *
__sm_chunk_before(uint8_t *base, uint8_t *last, uint8_t **before)
{
	uint8_t *q = base;
	*before = NULL;
	for (;;) {
		__sm_chunk_t tmp;
		uint8_t *next;
		__sm_chunk_init(&tmp, q + SM_SIZEOF_OVERHEAD);
		next = q + SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&tmp);
		if (next >= last) {
			return (q);
		}
		*before = q;
		q = next;
	}
}

/*
 * Reverse lookup on a chunk-mode map: the highest set bit strictly below
 * upper_excl, or SM_IDX_MAX.
 *
 * The answer lies in the last chunk that starts below upper_excl, unless
 * that chunk has no member below the bound; then it lies in an earlier
 * chunk.  The forward walk that finds the last chunk starts at chunk 0,
 * or at the cursor's chunk when the start word stored at cur->offset
 * equals cur->start_idx and that chunk starts below upper_excl (the same
 * check sm_next_member makes).  The walk remembers the chunk it passed
 * just before the last one, so one step back costs nothing; a step back
 * past a chunk the walk did not pass (the cursor's own chunk, or an
 * empty chunk) walks from chunk 0 again.  The chunk holding the answer
 * is recorded in the cursor, so a descending scan resumes in place while
 * it stays inside one chunk.
 */
static uint64_t
__sm_chunks_prev_member(const sm_t *map, uint64_t upper_excl,
    sm_cursor_t *cur)
{
	uint8_t *base;
	uint8_t *p;
	uint8_t *last = NULL;
	uint8_t *before = NULL;
	size_t stream_end;
	if (__sm_get_chunk_count(map) == 0) {
		return (SM_IDX_MAX);
	}
	base = __sm_get_chunk_data(map, 0);
	p = base;
	stream_end = map->m_data_used - SM_SIZEOF_OVERHEAD;

	/* Resume at the cursor's chunk only after checking its start word. */
	if (cur != NULL && cur->offset != SIZE_MAX &&
	    cur->offset + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) <=
	        stream_end &&
	    cur->start_idx < upper_excl &&
	    __sm_load_idx(base + cur->offset) == cur->start_idx) {
		p = base + cur->offset;
	}

	/* Find the last chunk that starts below upper_excl.  Every chunk of
	 * a valid map has its start and descriptor inside the stream; the
	 * bound only matters for a stale cursor. */
	while ((size_t)(p - base) + SM_SIZEOF_OVERHEAD +
	        sizeof(__sm_bitvec_t) <= stream_end) {
		__sm_chunk_t tmp;
		if (__sm_load_idx((const uint8_t *)p) >= upper_excl) {
			break;
		}
		before = last;
		last = p;
		__sm_chunk_init(&tmp, p + SM_SIZEOF_OVERHEAD);
		p += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&tmp);
	}
	if (last == NULL) {
		return (SM_IDX_MAX);
	}

	/* Step back chunk by chunk until one has a member below the bound. */
	for (;;) {
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)last);
		__sm_chunk_t chunk;
		uint64_t hit;
		__sm_chunk_init(&chunk, last + SM_SIZEOF_OVERHEAD);
		hit = __sm_chunk_prev_set(&chunk, start, upper_excl);
		if (hit != SM_IDX_MAX) {
			if (cur != NULL) {
				cur->offset = (size_t)(last - base);
				cur->start_idx = start;
				cur->prev_offset = (before == NULL) ? SIZE_MAX :
				    (size_t)(before - base);
			}
			return (hit);
		}
		if (last == base) {
			return (SM_IDX_MAX);
		}
		if (before != NULL) {
			last = before;
			before = NULL;
		} else {
			last = __sm_chunk_before(base, last, &before);
		}
	}
}

uint64_t
sm_prev_member(const sm_t *map, uint64_t prev_idx, sm_cursor_t *cur)
{
	if (map == NULL)
		return (SM_IDX_MAX);
	if (__sm_is_small(map)) {
		uint64_t upper_excl_s;
		uint64_t last_s;
		size_t w;
		uint64_t word;
		/* Highest set bit strictly less than prev_idx (SM_IDX_MAX
		 * means "from the end"). */
		const __sm_bitvec_unaligned_t *words = __sm_small_words(map);
		const size_t n = __sm_small_nwords(map);
		if (n == 0)
			return (SM_IDX_MAX);
		upper_excl_s = (prev_idx == SM_IDX_MAX) ? (uint64_t)n * 64 : prev_idx;
		if (upper_excl_s == 0)
			return (SM_IDX_MAX);
		last_s = upper_excl_s - 1;
		if (last_s >= (uint64_t)n * 64)
			last_s = (uint64_t)n * 64 - 1;
		w = (size_t)(last_s / 64);
		word = words[w] &
		    (~(uint64_t)0 >> (63 - (last_s % 64)));
		for (;;) {
			if (word != 0)
				return ((uint64_t)w * 64 +
				    (63 - (uint64_t)SM_CLZ64(word)));
			if (w == 0)
				return (SM_IDX_MAX);
			w--;
			word = words[w];
		}
	}
	__sm_check_invariants(map);
	/* SM_IDX_MAX as input means "start past the end". */
	return (__sm_chunks_prev_member(map,
	    (prev_idx == SM_IDX_MAX) ? UINT64_MAX : prev_idx, cur));
}

/* Defined with the run iterator below. */
static bool __sm_run_pair_any(const sm_t *a, const sm_t *b,
    bool want_shared);

bool
sm_is_subset(const sm_t *a, const sm_t *b)
{
	return (!__sm_run_pair_any(a, b, false));
}

bool
sm_is_superset(const sm_t *a, const sm_t *b)
{
	return (sm_is_subset(b, a));
}

bool
sm_overlap(const sm_t *a, const sm_t *b)
{
	return (__sm_run_pair_any(a, b, true));
}

sm_membership_t
sm_membership(const sm_t *map)
{
	uint64_t first;
	uint64_t second;
	if (map == NULL || sm_is_empty(map))
		return (SM_EMPTY);
	first = sm_next_member(map, SM_IDX_MAX, NULL);
	if (first == SM_IDX_MAX)
		return (SM_EMPTY);
	second = sm_next_member(map, first, NULL);
	return ((second == SM_IDX_MAX) ? SM_SINGLETON : SM_MULTIPLE);
}

uint64_t
sm_singleton_member(const sm_t *map)
{
	uint64_t first;
	uint64_t second;
	if (map == NULL || sm_is_empty(map))
		return (SM_IDX_MAX);
	first = sm_next_member(map, SM_IDX_MAX, NULL);
	if (first == SM_IDX_MAX)
		return (SM_IDX_MAX);
	second = sm_next_member(map, first, NULL);
	return ((second == SM_IDX_MAX) ? first : SM_IDX_MAX);
}

/* -------------------------------------------------------------------
 * Cardinality without allocation, bulk add, array conversion
 * ------------------------------------------------------------------- */

/*
 * Maximal-run iterator.
 *
 * The set-algebra and hashing helpers below used to walk bit-by-bit via
 * sm_next_member, making them O(cardinality): a run of set bits stored
 * as a stretch of all-ONES sparse chunks (each 8 bytes, declaring a
 * full 2048-bit window) turned sm_xor / sm_hash / the
 * *_cardinality family / sm_jaccard_index / sm_extract_range into
 * multi-second (or, for sm_split, non-terminating) loops on an
 * attacker-sized input.
 *
 * This iterator instead yields half-open runs [lo, hi) of set bits, so
 * its cost tracks the ENCODED size: a stretch of adjacent all-ONES
 * sparse chunks yields one run, and a partial sparse chunk yields at
 * most a chunk's worth of runs (<= 2048 bits, physically present).
 *
 * Runs are decomposed per chunk and are NOT merged across chunk
 * boundaries.  That is deliberate: chunk windows are fixed 2048-aligned
 * spans, so any two maps that compare equal under sm_equals() occupy
 * the same chunks and decompose into the identical run sequence --
 * which is all the content hash and the interval sweeps below require.
 * Merging across chunks would add state for no correctness gain.
 */
typedef struct {
	const sm_t *map;
	size_t count;      /* total chunk count */
	size_t idx;        /* next chunk ordinal to decode */
	uint8_t *p;        /* cursor into the chunk stream */
	/* Runs decoded from the current chunk, not yet yielded.  A sparse
	 * chunk with an alternating bit pattern is the worst case:
	 * SM_CHUNK_MAX_CAPACITY / 2 single-bit runs, so size for that plus
	 * one. */
	uint64_t run_lo[SM_CHUNK_MAX_CAPACITY / 2 + 1];
	uint64_t run_hi[SM_CHUNK_MAX_CAPACITY / 2 + 1];
	size_t nruns;
	size_t next_run;
} __sm_run_iter_t;

static void
__sm_run_iter_init(__sm_run_iter_t *it, const sm_t *map)
{
	memset(it, 0, sizeof(*it));
	it->map = map;
	if (map == NULL || sm_is_empty(map)) {
		it->count = 0;
		return;
	}
	if (__sm_is_small(map)) {
		bool open;
		uint64_t cur_lo;
		uint64_t cur_hi;
		size_t i;
		/* Decode the whole small map into the run buffer up front and
		 * leave count == 0 so __sm_run_next just drains it.  A small
		 * map spans < SM_SMALL_MAX_BITS (<= 1024) bits, whose worst
		 * case (alternating) is 512 runs -- well within the buffer. */
		const __sm_bitvec_unaligned_t *w = __sm_small_words(map);
		const size_t n = __sm_small_nwords(map);
		it->count = 0;
		it->nruns = 0;
		it->next_run = 0;
		open = false;
		cur_lo = 0; cur_hi = 0;
		for (i = 0; i < n; i++) {
			int b;
			const uint64_t base = (uint64_t)i * 64;
			uint64_t word = w[i];
			for (b = 0; b < 64; b++) {
				if ((word >> b) & 1u) {
					const uint64_t bit = base + (uint64_t)b;
					if (open && cur_hi == bit) {
						cur_hi = bit + 1;
					} else {
						if (open) {
							it->run_lo[it->nruns] =
							    cur_lo;
							it->run_hi[it->nruns++] =
							    cur_hi;
						}
						cur_lo = bit;
						cur_hi = bit + 1;
						open = true;
					}
				}
			}
		}
		if (open) {
			it->run_lo[it->nruns] = cur_lo;
			it->run_hi[it->nruns++] = cur_hi;
		}
		return;
	}
	it->count = __sm_get_chunk_count(map);
	it->p = __sm_get_chunk_data(map, 0);
}

/*
 * Decompose one chunk (the one at it->p) into its runs, absolute bit
 * indices, into it->run_lo/run_hi.
 */
static void
__sm_run_decode_chunk(__sm_run_iter_t *it, __sm_idx_t start)
{
	__sm_bitvec_t desc;
	size_t pos;
	bool open;
	uint64_t cur_lo;
	uint64_t cur_hi;
	size_t v;
	__sm_chunk_t chunk;
	__sm_chunk_init(&chunk, it->p + SM_SIZEOF_OVERHEAD);
	it->nruns = 0;
	it->next_run = 0;

	/* Sparse: walk the 32 flags, coalescing adjacent set bits.  ONES is
	 * a full 64-bit run; MIXED decodes its payload word bit-by-bit
	 * (bounded, 64 bits); ZEROS / NONE break any open run. */
	desc = chunk.m_data[0];
	pos = 1; /* payload-word cursor for MIXED slots */
	open = false;
	cur_lo = 0; cur_hi = 0;
	for (v = 0; v < SM_FLAGS_PER_INDEX; v++) {
		const size_t flags = SM_CHUNK_GET_FLAGS(desc, v);
		const uint64_t base = start + (uint64_t)v * SM_BITS_PER_VECTOR;
		if (flags == SM_PAYLOAD_ONES) {
			if (open && cur_hi == base) {
				cur_hi = base + SM_BITS_PER_VECTOR;
			} else {
				if (open) {
					it->run_lo[it->nruns] = cur_lo;
					it->run_hi[it->nruns++] = cur_hi;
				}
				cur_lo = base;
				cur_hi = base + SM_BITS_PER_VECTOR;
				open = true;
			}
		} else if (flags == SM_PAYLOAD_MIXED) {
			int b;
			__sm_bitvec_t w = chunk.m_data[pos++];
			for (b = 0; b < SM_BITS_PER_VECTOR; b++) {
				if ((w >> b) & 1u) {
					const uint64_t bit = base + (uint64_t)b;
					if (open && cur_hi == bit) {
						cur_hi = bit + 1;
					} else {
						if (open) {
							it->run_lo[it->nruns] =
							    cur_lo;
							it->run_hi[it->nruns++] =
							    cur_hi;
						}
						cur_lo = bit;
						cur_hi = bit + 1;
						open = true;
					}
				}
			}
		} else {
			/* ZEROS / NONE: a gap ends any open run. */
			if (open) {
				it->run_lo[it->nruns] = cur_lo;
				it->run_hi[it->nruns++] = cur_hi;
				open = false;
			}
		}
	}
	if (open) {
		it->run_lo[it->nruns] = cur_lo;
		it->run_hi[it->nruns++] = cur_hi;
	}
}

/*
 * Yield the next run.  Returns false when exhausted.
 */
static bool
__sm_run_next(__sm_run_iter_t *it, uint64_t *lo, uint64_t *hi)
{
	for (;;) {
		__sm_idx_t start;
		__sm_chunk_t chunk;
		size_t chunk_bytes;
		/* Drain runs already decoded from the current chunk. */
		if (it->next_run < it->nruns) {
			*lo = it->run_lo[it->next_run];
			*hi = it->run_hi[it->next_run];
			it->next_run++;
			return (true);
		}
		/* Current chunk exhausted; decode the next one. */
		if (it->idx >= it->count) {
			return (false);
		}
		start = __sm_load_idx((const uint8_t *)it->p);
		__sm_chunk_init(&chunk, it->p + SM_SIZEOF_OVERHEAD);
		chunk_bytes = SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		__sm_run_decode_chunk(it, start);
		it->p += chunk_bytes;
		it->idx++;
		/* loop back to drain the freshly-decoded run list */
	}
}

/*
 * The cardinality / set-algebra / hashing helpers below walk maps
 * run-by-run (see __sm_run_iter_t) rather than bit-by-bit, so their
 * cost tracks the encoded size, not the popcount.  A run stored as a
 * stretch of adjacent all-ONES sparse chunks decomposes into a single
 * run.
 */

/*
 * Single lockstep pass over two maps' runs, accumulating the counts
 * every set-algebra cardinality wants: |a|, |b|, |a & b|, |a | b|.
 * Runs are maximal, ascending and non-overlapping within each map, so
 * a classic interval sweep is exact and O(runs_a + runs_b).
 */
static void
__sm_run_pair_counts(const sm_t *a, const sm_t *b, uint64_t *cnt_a,
    uint64_t *cnt_b, uint64_t *inter, uint64_t *uni)
{
	uint64_t alo;
	uint64_t ahi;
	uint64_t blo;
	uint64_t bhi;
	bool have_a;
	bool have_b;
	uint64_t ca;
	uint64_t cb;
	uint64_t ci;
	__sm_run_iter_t ia, ib;
	__sm_run_iter_init(&ia, a);
	__sm_run_iter_init(&ib, b);
	alo = 0; ahi = 0; blo = 0; bhi = 0;
	have_a = __sm_run_next(&ia, &alo, &ahi);
	have_b = __sm_run_next(&ib, &blo, &bhi);
	ca = 0; cb = 0; ci = 0;
	/* Intersection by interval sweep: at each step add the overlap of
	 * the two active runs, then consume whichever ends first so the
	 * other can still overlap the consumed side's later runs.  Runs
	 * are ascending and disjoint within each map, so no overlap is
	 * double-counted.  Union follows from inclusion-exclusion:
	 * |a | b| = |a| + |b| - |a & b|.  Cardinalities are accumulated
	 * once per run as it is consumed. */
	while (have_a || have_b) {
		if (have_a && have_b) {
			const uint64_t ov_lo = alo > blo ? alo : blo;
			const uint64_t ov_hi = ahi < bhi ? ahi : bhi;
			if (ov_lo < ov_hi)
				ci += ov_hi - ov_lo;
		}
		if (have_a && (!have_b || ahi <= bhi)) {
			ca += ahi - alo;
			have_a = __sm_run_next(&ia, &alo, &ahi);
		} else {
			cb += bhi - blo;
			have_b = __sm_run_next(&ib, &blo, &bhi);
		}
	}
	if (cnt_a)
		*cnt_a = ca;
	if (cnt_b)
		*cnt_b = cb;
	if (inter)
		*inter = ci;
	if (uni)
		*uni = ca + cb - ci;
}

/*
 * One early-exit lockstep pass over two maps' runs.  With want_shared
 * it reports whether a and b share a member; otherwise whether a holds
 * a member b lacks.  Either answer is found at the first run of a that
 * proves it, so the cost is O(runs) up to that point, and a search for
 * a shared member stops once b has no run left.  A run stored as
 * all-ONES chunks is one step per chunk, never one per bit.  NULL or
 * empty maps have no runs.
 */
static bool
__sm_run_pair_any(const sm_t *a, const sm_t *b, bool want_shared)
{
	__sm_run_iter_t ia, ib;
	uint64_t alo = 0, ahi = 0, blo = 0, bhi = 0;
	bool have_a, have_b;
	__sm_run_iter_init(&ia, a);
	__sm_run_iter_init(&ib, b);
	have_a = __sm_run_next(&ia, &alo, &ahi);
	have_b = __sm_run_next(&ib, &blo, &bhi);
	while (have_a) {
		/* Drop b's runs that end at or before alo: they cannot meet
		 * this run of a or any later one. */
		while (have_b && bhi <= alo) {
			have_b = __sm_run_next(&ib, &blo, &bhi);
		}
		if (want_shared) {
			if (!have_b) {
				return (false); /* nothing of b is left to meet */
			}
			if (blo < ahi) {
				return (true); /* [alo, ahi) meets [blo, bhi) */
			}
		} else if (!have_b || blo > alo) {
			return (true); /* alo itself is not in b */
		} else if (bhi < ahi) {
			/* b covers [alo, bhi) of this run: test the rest. */
			alo = bhi;
			continue;
		}
		have_a = __sm_run_next(&ia, &alo, &ahi);
	}
	return (false);
}

size_t
sm_union_cardinality(const sm_t *a, const sm_t *b)
{
	uint64_t uni;
	if (sm_is_empty(a))
		return (b ? sm_cardinality((sm_t *)b) : 0);
	if (sm_is_empty(b))
		return (sm_cardinality((sm_t *)a));
	uni = 0;
	__sm_run_pair_counts(a, b, NULL, NULL, NULL, &uni);
	return ((size_t)uni);
}

size_t
sm_intersection_cardinality(const sm_t *a, const sm_t *b)
{
	uint64_t inter;
	if (sm_is_empty(a) || sm_is_empty(b))
		return (0);
	inter = 0;
	__sm_run_pair_counts(a, b, NULL, NULL, &inter, NULL);
	return ((size_t)inter);
}

size_t
sm_difference_cardinality(const sm_t *a, const sm_t *b)
{
	uint64_t ca;
	uint64_t inter;
	if (sm_is_empty(a))
		return (0);
	if (sm_is_empty(b))
		return (sm_cardinality((sm_t *)a));
	ca = 0; inter = 0;
	__sm_run_pair_counts(a, b, &ca, NULL, &inter, NULL);
	return ((size_t)(ca - inter));
}

bool
sm_nonempty_difference(const sm_t *a, const sm_t *b)
{
	return (__sm_run_pair_any(a, b, false));
}

double
sm_jaccard_index(const sm_t *a, const sm_t *b)
{
	uint64_t inter;
	uint64_t uni;
	if (sm_is_empty(a) && sm_is_empty(b))
		return (0.0);
	inter = 0; uni = 0;
	__sm_run_pair_counts(a, b, NULL, NULL, &inter, &uni);
	return (uni == 0 ? 0.0 : (double)inter / (double)uni);
}

/* Ascending uint64_t comparator for the bulk-insert sort below. */
static int
__sm_cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;
	return ((x > y) - (x < y));
}

/* Defined further down with the in-place set ops; forward-declared so the
 * bulk builder can swap its freshly-emitted buffer into an existing map. */
static sm_t *__sm_replace_buffer(sm_t *dst, sm_t *result);

/*
 * Coalesce a sorted (ascending) index array into maximal half-open runs
 * [lo, hi), consecutive integers becoming one run, duplicates collapsing.
 * Writes the runs into run_lo/run_hi (each at most n long) and returns the
 * run count.  n must be > 0 and arr must be sorted ascending.
 */
static size_t
__sm_coalesce_sorted(const uint64_t *arr, size_t n, uint64_t *run_lo,
    uint64_t *run_hi)
{
	size_t nruns = 0;
	uint64_t lo = arr[0];
	uint64_t hi = arr[0] + 1;
	size_t i;
	for (i = 1; i < n; i++) {
		const uint64_t v = arr[i];
		if (v <= hi) {
			/* Duplicate (v < hi) or consecutive (v == hi): extend. */
			if (v == hi)
				hi = v + 1;
			continue;
		}
		run_lo[nruns] = lo;
		run_hi[nruns++] = hi;
		lo = v;
		hi = v + 1;
	}
	run_lo[nruns] = lo;
	run_hi[nruns++] = hi;
	return (nruns);
}

/*
 * Build a fresh map that is the union of an existing map's set bits and a
 * list of ascending, non-overlapping runs, emitting the whole result in one
 * ordered pass through __sm_emitter_t -- the same machinery sm_union and
 * sm_add_range use.  This makes the merge O(#existing chunks + #runs)
 * regardless of where the new bits land.
 *
 * The per-element __sm_add_c path threads a cursor, so an ASCENDING bulk
 * build that only appends new chunks at the tail was already near-linear.
 * The quadratic case it does NOT cover is inserting sparse bits INTO an
 * already-large map: each insert byte-shifts the tail of the buffer
 * (O(#chunks) memmove), so N such inserts cost O(N * #chunks) -- measured
 * at ~37 s for 50k inserts into a 50k-chunk map, ~2750x the ~13 ms this
 * single-pass rebuild takes, and scaling ~4x per doubling (this path ~2x).
 *
 * Interval-union sweep of the map's run stream and the run array (identical
 * to sm_xor's sweep but emitting wherever EITHER side is set).  Returns the
 * new buffer (caller swaps it in) or NULL on allocation failure.
 */
static sm_t *
__sm_union_runs(const sm_t *map, const uint64_t *run_lo,
    const uint64_t *run_hi, size_t nruns)
{
	size_t cap;
	sm_t *r;
	__sm_emitter_t em;
	__sm_run_iter_t ia;
	uint64_t alo = 0, ahi = 0;
	bool have_a;
	size_t bi = 0;
	uint64_t pos = 0;
	bool have_pos = false;

	/* Result upper bound: existing bytes plus ~24 per input run. */
	cap = sm_is_empty(map) ? 0 : sm_get_size((sm_t *)map);
	cap += nruns * 24 + 64;
	if (cap < 1024)
		cap = 1024;
	r = sm_create(cap);
	if (r == NULL)
		return (NULL);

	memset(&em, 0, sizeof(em));
	em.resultp = &r;
	__sm_run_iter_init(&ia, map);
	have_a = __sm_run_next(&ia, &alo, &ahi);
	while (have_a || bi < nruns) {
		const bool have_b = bi < nruns;
		uint64_t blo = have_b ? run_lo[bi] : 0;
		uint64_t bhi = have_b ? run_hi[bi] : 0;
		uint64_t lo = have_a ? alo : blo;
		bool in_a;
		bool in_b;
		uint64_t next = UINT64_MAX;
		if (have_b && blo < lo)
			lo = blo;
		if (!have_pos || pos < lo) {
			pos = lo;
			have_pos = true;
		}
		in_a = have_a && pos >= alo && pos < ahi;
		in_b = have_b && pos >= blo && pos < bhi;
		if (have_a) {
			if (pos < alo && alo < next)
				next = alo;
			if (pos >= alo && ahi < next)
				next = ahi;
		}
		if (have_b) {
			if (pos < blo && blo < next)
				next = blo;
			if (pos >= blo && bhi < next)
				next = bhi;
		}
		/* Union: any span set in either side is emitted.  __sm_emit_run
		 * is the ordered emitter primitive __sm_add_run_grow wraps. */
		if (in_a || in_b) {
			if (!__sm_emit_run(&em, pos, next)) {
				sm_free(r);
				return (NULL);
			}
		}
		pos = next;
		if (have_a && pos >= ahi)
			have_a = __sm_run_next(&ia, &alo, &ahi);
		if (have_b && pos >= bhi)
			bi++;
	}
	if (!__sm_emit_flush(&em)) {
		sm_free(r);
		return (NULL);
	}
	r = *em.resultp;
	__sm_coalesce_map(r);
	return (r);
}

/*
 * Shared bulk-insert core for sm_add_many / sm_add_many_grow.  Sorts a
 * private copy of the input, coalesces it into runs, unions those runs with
 * the map's existing bits via the ordered emitter in one pass, and swaps the
 * result into *mapp with __sm_replace_buffer (which grows, demotes to
 * small-set mode when the result fits, and frees the scratch buffer).
 */
/*
 * True when arr[0..n) holds SM_IDX_MAX, which is not a member.  The bulk
 * builders reject such an input whole, before touching the map, with
 * errno = EINVAL.
 */
static bool
__sm_holds_idx_max(const uint64_t *arr, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		if (arr[i] == SM_IDX_MAX) {
			errno = EINVAL;
			return (true);
		}
	}
	return (false);
}

static bool
__sm_add_many_core(sm_t **mapp, const uint64_t *arr, size_t n, bool may_grow)
{
	uint64_t *sorted;
	uint64_t *run_lo;
	uint64_t *run_hi;
	size_t nruns;
	sm_t *result;
	sm_t *swapped;

	sorted = (uint64_t *)__sm_alloc(n * sizeof(uint64_t));
	run_lo = (uint64_t *)__sm_alloc(n * sizeof(uint64_t));
	run_hi = (uint64_t *)__sm_alloc(n * sizeof(uint64_t));
	if (sorted == NULL || run_lo == NULL || run_hi == NULL) {
		__sm_free(sorted);
		__sm_free(run_lo);
		__sm_free(run_hi);
		return (false);
	}
	memcpy(sorted, arr, n * sizeof(uint64_t));
	qsort(sorted, n, sizeof(uint64_t), __sm_cmp_u64);
	nruns = __sm_coalesce_sorted(sorted, n, run_lo, run_hi);
	__sm_free(sorted);

	result = __sm_union_runs(*mapp, run_lo, run_hi, nruns);
	__sm_free(run_lo);
	__sm_free(run_hi);
	if (result == NULL)
		return (false);

	/*
	 * sm_add_many (may_grow == false) promises not to relocate the
	 * caller's buffer: it takes an sm_t*, not an sm_t**, so it cannot
	 * report a moved pointer.  __sm_replace_buffer grows via
	 * sm_set_data_size, which for an SM_OWNED_CONTIGUOUS map reallocs the
	 * whole struct+buffer block and frees the original -- leaving the
	 * caller holding a dangling pointer.  If the merged result does not
	 * fit in the map's current capacity, free the scratch result and fail
	 * cleanly, leaving *mapp untouched and valid; the caller must retry
	 * with sm_add_many_grow.
	 */
	if (!may_grow && __sm_cap(*mapp) < result->m_data_used) {
		sm_free(result);
		return (false);
	}

	swapped = __sm_replace_buffer(*mapp, result);
	if (swapped == NULL)
		return (false);
	*mapp = swapped;
	return (true);
}

bool
sm_add_many(sm_t *map, const uint64_t *arr, size_t n)
{
	sm_t *m;
	if (map == NULL || (arr == NULL && n > 0))
		return (false);
	if (n == 0)
		return (true);
	if (__sm_holds_idx_max(arr, n))
		return (false);
	if (n == 1)
		return (sm_add(map, arr[0]) != SM_IDX_MAX);

	/*
	 * Bulk build: coalesce the input into runs and union them with the
	 * existing bits in one O(N + #chunks) emitter pass.  The per-element
	 * cursor path is already near-linear for a pure tail-append build, but
	 * degrades to O(N * #chunks) when the new bits land inside an existing
	 * map (each insert memmoves the buffer tail); the single-pass rebuild
	 * is linear either way.
	 *
	 * sm_add_many keeps the sm_t* signature and must not relocate the
	 * caller's buffer: __sm_replace_buffer reuses *map's allocation when
	 * it is large enough and only grows in place otherwise, so *m stays
	 * == map.  A result that outgrows the buffer cannot be reported here
	 * (no sm_t**), so it fails -- callers that need growth use
	 * sm_add_many_grow.
	 */
	m = map;
	if (!__sm_add_many_core(&m, arr, n, false))
		return (false);
	return (m == map);
}

/*
 * Growing bulk insert: like sm_add_many but takes sm_t** so
 * __sm_replace_buffer may relocate the buffer when the emitted result
 * outgrows it, instead of failing.
 */
bool
sm_add_many_grow(sm_t **map, const uint64_t *arr, size_t n)
{
	if (map == NULL || *map == NULL || (arr == NULL && n > 0))
		return (false);
	if (n == 0)
		return (true);
	if (__sm_holds_idx_max(arr, n))
		return (false);
	if (n == 1)
		return (sm_add_grow(map, arr[0]) != SM_IDX_MAX);
	return (__sm_add_many_core(map, arr, n, true));
}

/*
 * Set every bit in [lo, hi) on a result map that is being built through
 * the ordered emitter (__sm_emitter_t).  Runs arrive ascending and
 * non-overlapping, so this is O(output chunks): whole chunks go out as
 * all-ONES sparse chunks and only the sub-chunk head/tail touch the
 * words path.  This is
 * where the S4 amplification lived -- the old body added one bit at a
 * time, so a single [0, 2^31) survivor cost 2^31 sm_add calls.
 */
static bool
__sm_add_run_grow(__sm_emitter_t *e, uint64_t lo, uint64_t hi)
{
	return (__sm_emit_run(e, lo, hi));
}

void
sm_to_array(const sm_t *map, uint64_t *out, size_t *n_out)
{
	size_t cap;
	size_t written;
	uint64_t i;
	if (n_out == NULL)
		return;
	cap = (out == NULL) ? 0 : *n_out;
	written = 0;

	if (out == NULL) {
		/* Query: just count. */
		*n_out = sm_is_empty(map) ? 0 : sm_cardinality((sm_t *)map);
		return;
	}

	i = SM_IDX_MAX;
	while ((i = sm_next_member(map, i, NULL)) != SM_IDX_MAX) {
		if (written >= cap)
			break;
		out[written++] = i;
	}
	*n_out = written;
}

/* -------------------------------------------------------------------
 * Range ops, symmetric difference, set-op synonyms, constructors,
 * hashing and ordering, destructive iteration
 * ------------------------------------------------------------------- */

/*
 * add_range fast path: OR the literal run [lo, hi) into an existing
 * map's run stream and re-emit, instead of touching every bit.
 *
 * The old body was `for (i = lo; i < hi; i++) sm_add(map, i)`, which is
 * O(hi - lo) -- a [0, 1e6) add cost ~14 ms even though the result is a
 * short run of all-ONES sparse chunks.  Here add_range is
 * `map = union(map, [lo,hi))`: we merge the map's existing (ascending,
 * maximal) runs with the single literal run using interval-union
 * semantics, then feed each merged run to the same ordered emitter set
 * ops already use (__sm_add_run_grow -> __sm_emit_run), which lays whole
 * output chunks as all-ONES sparse chunks and only touches the sub-chunk
 * head/tail bit-by-bit.  Cost is O(existing_chunks + run_chunks).
 *
 * The emitter demands runs delivered ascending and non-overlapping, so
 * the interval merge below coalesces the literal run against the map's
 * runs (and against each other across the seam) before emitting.
 *
 * Contract preserved from the old loop: the map is written in place and
 * does NOT relocate (sm_add_range returns bool, so a relocated struct
 * pointer could not reach the caller).  If the merged result does not
 * fit the map's current buffer, we fail with errno = ENOSPC -- exactly
 * as the old per-bit loop's sm_add did on a too-small
 * SM_OWNED_CONTIGUOUS map.
 */
bool
sm_add_range(sm_t *map, uint64_t lo, uint64_t hi)
{
	__sm_emitter_t em;
	__sm_run_iter_t it;
	sm_t *r;
	size_t cap;
	size_t result_size;
	uint64_t rlo = 0, rhi = 0;
	bool have_run;
	/* Pending merged run being accumulated (interval union). */
	uint64_t cur_lo = 0, cur_hi = 0;
	bool have_cur = false;
	/* The single literal run [lo, hi), consumed once when the ascending
	 * merge reaches its position. */
	bool lit_pending;

	if (map == NULL || lo >= hi)
		return (lo >= hi); /* empty range = OK */
	lit_pending = true;

	__sm_check_invariants(map);

	/* Result capacity upper bound: the map's current encoding plus the
	 * new run's worst-case chunk footprint.  A run spanning K output
	 * chunks needs at most K * (SM_SIZEOF_OVERHEAD + one descriptor +
	 * two vectors); most of that collapses, so this is generous. */
	cap = sm_get_size(map) + 128;
	if (cap < 1024)
		cap = 1024;
	r = sm_create(cap);
	if (r == NULL)
		return (false);

	memset(&em, 0, sizeof(em));
	em.resultp = &r;
	__sm_run_iter_init(&it, map);
	have_run = __sm_run_next(&it, &rlo, &rhi);

	/*
	 * Ascending interval-union merge of the map's runs with the single
	 * literal run.  At each step pick the next-starting interval among
	 * {current map run, literal run}, then either extend the pending
	 * merged run (overlap or abut) or flush it and start a new one.
	 */
	for (;;) {
		uint64_t nlo, nhi;
		bool take_lit;
		if (!have_run && !lit_pending)
			break;
		if (have_run && lit_pending)
			take_lit = (lo <= rlo);
		else
			take_lit = lit_pending;
		if (take_lit) {
			nlo = lo;
			nhi = hi;
			lit_pending = false;
		} else {
			nlo = rlo;
			nhi = rhi;
			have_run = __sm_run_next(&it, &rlo, &rhi);
		}
		if (!have_cur) {
			cur_lo = nlo;
			cur_hi = nhi;
			have_cur = true;
		} else if (nlo <= cur_hi) {
			/* Overlap or abut: extend. */
			if (nhi > cur_hi)
				cur_hi = nhi;
		} else {
			/* Gap: flush the pending run and open a new one. */
			if (!__sm_add_run_grow(&em, cur_lo, cur_hi)) {
				sm_free(r);
				return (false);
			}
			cur_lo = nlo;
			cur_hi = nhi;
		}
	}
	if (have_cur) {
		if (!__sm_add_run_grow(&em, cur_lo, cur_hi)) {
			sm_free(r);
			return (false);
		}
	}
	if (!__sm_emit_flush(&em)) {
		sm_free(r);
		return (false);
	}
	r = *em.resultp;
	__sm_coalesce_map(r);

	/*
	 * Copy the merged result into the map's existing buffer in place.
	 * Do NOT use __sm_replace_buffer here: it grows via sm_set_data_size,
	 * which relocates an SM_OWNED_CONTIGUOUS struct and returns a new
	 * pointer that sm_add_range cannot hand back.  Refuse (ENOSPC) if the
	 * result does not fit, matching the old loop's sm_add behaviour.
	 */
	result_size = r->m_data_used;
	if (result_size > __sm_cap(map)) {
		sm_free(r);
		errno = ENOSPC;
		return (false);
	}
	memcpy(map->m_data, r->m_data, result_size);
	map->m_data_used = result_size;
	/* Membership changed: the run-emitter path swaps the buffer in place
	 * (it does NOT route through __sm_add_dispatch or __sm_replace_buffer,
	 * which are the other invalidation points), so invalidate the lazy
	 * cardinality cache here. */
	__sm_card_invalidate(map);
	sm_free(r);
	/* Keep a near-zero result in small mode (footprint parity with the
	 * old loop, whose per-bit sm_add demotes). */
	__sm_try_demote(map);
	return (true);
}

/*
 * Rewrite `map` in place with the bits of [lo, hi) cleared (flip false)
 * or complemented (flip true), lo < hi.  The map's runs are swept once
 * and the result goes out through the ordered emitter, so the cost is
 * O(runs + output chunks), never O(hi - lo): clearing or flipping a
 * 2^31-bit span is a few chunks of work per 2048 bits it touches.
 *
 * NB: like sm_add_range, the map is written in place and never
 * relocates (the callers return bool).  If the result does not fit the
 * map's buffer this fails with ENOSPC and leaves the map unchanged.
 */
static bool
__sm_rewrite_range(sm_t *map, uint64_t lo, uint64_t hi, bool flip)
{
	__sm_emitter_t em;
	__sm_run_iter_t it;
	sm_t *r;
	size_t cap;
	uint64_t rlo = 0, rhi = 0;
	/* The part of [lo, hi) not yet complemented (flip only). */
	uint64_t gap = lo;
	bool ok = true;

	__sm_check_invariants(map);
	/* Worst case: every run splits in two, plus the flipped gaps. */
	cap = sm_get_size(map) * 2 + 1024;
	r = sm_create(cap);
	if (r == NULL)
		return (false);
	memset(&em, 0, sizeof(em));
	em.resultp = &r;
	__sm_run_iter_init(&it, map);

	/* Each run keeps its parts outside [lo, hi).  Under flip, the
	 * unset stretch of [lo, hi) before the run is emitted first, so
	 * the output stays ascending. */
	while (ok && __sm_run_next(&it, &rlo, &rhi)) {
		if (flip && gap < hi && gap < rlo)
			ok = __sm_emit_run(&em, gap, rlo < hi ? rlo : hi);
		if (rhi > gap)
			gap = rhi;
		if (ok && rlo < lo)
			ok = __sm_emit_run(&em, rlo, rhi < lo ? rhi : lo);
		if (ok && rhi > hi)
			ok = __sm_emit_run(&em, rlo > hi ? rlo : hi, rhi);
	}
	if (ok && flip && gap < hi)
		ok = __sm_emit_run(&em, gap, hi);
	if (ok)
		ok = __sm_emit_flush(&em);
	r = *em.resultp;
	if (!ok) {
		sm_free(r);
		return (false);
	}
	__sm_coalesce_map(r);
	/* Demote first: a small-set map in a buffer sized for its words
	 * must not fail just because the chunk form is larger. */
	__sm_try_demote(r);
	if (r->m_data_used > __sm_cap(map)) {
		sm_free(r);
		errno = ENOSPC;
		return (false);
	}
	memcpy(map->m_data, r->m_data, r->m_data_used);
	map->m_data_used = r->m_data_used;
	__sm_card_invalidate(map);
	sm_free(r);
	return (true);
}

bool
sm_remove_range(sm_t *map, uint64_t lo, uint64_t hi)
{
	if (map == NULL || lo >= hi)
		return (lo >= hi);
	return (__sm_rewrite_range(map, lo, hi, false));
}

sm_t *
sm_xor(const sm_t *a, const sm_t *b)
{
	size_t cap;
	sm_t * r;
	__sm_emitter_t em;
	__sm_run_iter_t ia;
	__sm_run_iter_t ib;
	uint64_t alo;
	uint64_t ahi;
	uint64_t blo;
	uint64_t bhi;
	bool have_a;
	bool have_b;
	uint64_t pos;
	bool have_pos;
	if (sm_is_empty(a) && sm_is_empty(b))
		return (NULL);
	if (sm_is_empty(a))
		return (sm_copy(b));
	if (sm_is_empty(b))
		return (sm_copy(a));

	/* Allocate a result big enough for the union (upper bound). */
	cap = sm_get_capacity(a) + sm_get_capacity(b);
	r = sm_create(cap > 1024 ? cap : 1024);
	if (r == NULL)
		return (NULL);

	/* Walk both maps run-by-run and emit the symmetric-difference
	 * runs (bits set in exactly one map).  Cost tracks the encoded
	 * size of the operands, not their popcount, so a 2^31-bit run is
	 * one iteration rather than 2^31.  Each survivor is emitted whole
	 * through the ordered emitter, so a giant run costs O(chunks). */
	memset(&em, 0, sizeof(em));
	em.resultp = &r;
	__sm_run_iter_init(&ia, a);
	__sm_run_iter_init(&ib, b);
	alo = 0; ahi = 0; blo = 0; bhi = 0;
	have_a = __sm_run_next(&ia, &alo, &ahi);
	have_b = __sm_run_next(&ib, &blo, &bhi);
	/* pos = left edge of the not-yet-emitted portion of the current
	 * a/b runs; overlaps cancel, gaps in exactly one survive. */
	pos = 0;
	have_pos = false;
	while (have_a || have_b) {
		bool in_a;
		bool in_b;
		uint64_t next;
		/* The next boundary among the two active runs. */
		uint64_t lo = have_a ? alo : blo;
		if (have_b && blo < lo)
			lo = blo;
		if (!have_pos || pos < lo) {
			pos = lo;
			have_pos = true;
		}
		in_a = have_a && pos >= alo && pos < ahi;
		in_b = have_b && pos >= blo && pos < bhi;
		/* End of the current homogeneous segment. */
		next = UINT64_MAX;
		if (have_a) {
			if (pos < alo && alo < next)
				next = alo;
			if (pos >= alo && ahi < next)
				next = ahi;
		}
		if (have_b) {
			if (pos < blo && blo < next)
				next = blo;
			if (pos >= blo && bhi < next)
				next = bhi;
		}
		if (in_a != in_b) {
			/* Bits [pos, next) are in exactly one map. */
			if (!__sm_add_run_grow(&em, pos, next)) {
				sm_free(r);
				return (NULL);
			}
		}
		pos = next;
		if (have_a && pos >= ahi)
			have_a = __sm_run_next(&ia, &alo, &ahi);
		if (have_b && pos >= bhi)
			have_b = __sm_run_next(&ib, &blo, &bhi);
	}
	if (!__sm_emit_flush(&em)) {
		sm_free(r);
		return (NULL);
	}
	r = *em.resultp;
	__sm_coalesce_map(r);
	if (sm_is_empty(r)) {
		sm_free(r);
		return (NULL);
	}
	__sm_try_demote(r);
	return (r);
}

sm_t *
sm_or(const sm_t *a, const sm_t *b)
{
	return (sm_union(a, b));
}

sm_t *
sm_and(const sm_t *a, const sm_t *b)
{
	return (sm_intersection(a, b));
}

sm_t *
sm_andnot(const sm_t *a, const sm_t *b)
{
	return (sm_difference(a, b));
}

sm_t *
sm_extract_range(const sm_t *map, uint64_t lo, uint64_t hi)
{
	size_t cap;
	sm_t * r;
	__sm_emitter_t em;
	__sm_run_iter_t it;
	uint64_t rlo;
	uint64_t rhi;
	if (map == NULL || sm_is_empty(map) || lo >= hi)
		return (NULL);

	/* Estimate result capacity from the input -- worst case is the same
	 * shape, capped to the requested range size. */
	cap = sm_get_size((sm_t *)map) + 64;
	if (cap < 1024)
		cap = 1024;
	r = sm_create(cap);
	if (r == NULL)
		return (NULL);

	/* Walk set-bit runs and add each run's intersection with [lo, hi).
	 * Run-based, so a 2^31-bit run outside the window costs one
	 * iteration rather than 2^31 bit lookups, and a run inside the
	 * window is emitted whole in O(chunks) via the ordered emitter. */
	memset(&em, 0, sizeof(em));
	em.resultp = &r;
	__sm_run_iter_init(&it, map);
	rlo = 0; rhi = 0;
	while (__sm_run_next(&it, &rlo, &rhi)) {
		uint64_t clip_lo;
		uint64_t clip_hi;
		if (rhi <= lo)
			continue;
		if (rlo >= hi)
			break; /* runs are ascending; nothing more overlaps */
		clip_lo = rlo < lo ? lo : rlo;
		clip_hi = rhi > hi ? hi : rhi;
		if (!__sm_add_run_grow(&em, clip_lo, clip_hi)) {
			sm_free(r);
			return (NULL);
		}
	}
	if (!__sm_emit_flush(&em)) {
		sm_free(r);
		return (NULL);
	}
	r = *em.resultp;
	__sm_coalesce_map(r);

	if (sm_is_empty(r)) {
		sm_free(r);
		return (NULL);
	}
	__sm_try_demote(r);
	return (r);
}

size_t
sm_xor_cardinality(const sm_t *a, const sm_t *b)
{
	uint64_t inter;
	uint64_t uni;
	if (sm_is_empty(a) && sm_is_empty(b))
		return (0);
	if (sm_is_empty(a))
		return (sm_cardinality((sm_t *)b));
	if (sm_is_empty(b))
		return (sm_cardinality((sm_t *)a));
	inter = 0; uni = 0;
	__sm_run_pair_counts(a, b, NULL, NULL, &inter, &uni);
	return ((size_t)(uni - inter));
}

sm_t *
sm_create_singleton(uint64_t idx)
{
	sm_t *m;
	if (idx == SM_IDX_MAX) {
		errno = EINVAL;
		return (NULL);
	}
	m = sm_create(1024);
	if (m && sm_add(m, idx) == SM_IDX_MAX) {
		sm_free(m);
		return (NULL);
	}
	return (m);
}

sm_t *
sm_create_from_range(uint64_t lo, uint64_t hi)
{
	/* Estimate buffer size: each chunk is at most ~24 bytes; range
	 * spans (hi-lo)/2048 chunks plus partial-edge chunks. */
	size_t chunks = (hi - lo) / 2048 + 2;
	size_t bytes = 32 + chunks * 24;
	sm_t *m = sm_create(bytes < 1024 ? 1024 : bytes);
	if (m == NULL)
		return (NULL);
	if (!sm_add_range(m, lo, hi)) {
		/* Try once with a bigger buffer. */
		sm_t *grown = sm_set_data_size(m, NULL, bytes * 4);
		if (grown == NULL) {
			sm_free(m);
			return (NULL);
		}
		sm_clear(grown);
		if (!sm_add_range(grown, lo, hi)) {
			sm_free(grown);
			return (NULL);
		}
		return (grown);
	}
	return (m);
}

sm_t *
sm_create_from_array(const uint64_t *arr, size_t n)
{
	sm_t *m;
	if ((arr == NULL && n > 0) || __sm_holds_idx_max(arr, n))
		return (NULL);
	m = sm_create(1024);
	if (m == NULL)
		return (NULL);
	if (!sm_add_many_grow(&m, arr, n)) {
		sm_free(m);
		return (NULL);
	}
	return (m);
}

uint64_t
sm_hash(const sm_t *map)
{
	__sm_run_iter_t it;
	uint64_t lo;
	uint64_t hi;
	/* FNV-1a 64-bit over the sequence of maximal set-bit runs.
	 * Content-based (encoding-independent): two maps that compare
	 * equal under sm_equals() decompose into the identical run
	 * sequence and so hash to the same value.  Hashing runs rather
	 * than individual bits keeps this O(runs), so a 2^31-bit run costs
	 * one iteration instead of 2^31. */
	uint64_t h = UINT64_C(0xcbf29ce484222325);
	if (sm_is_empty(map))
		return (h);
	__sm_run_iter_init(&it, map);
	lo = 0; hi = 0;
	while (__sm_run_next(&it, &lo, &hi)) {
		int b;
		/* Mix both endpoints of the run (8 bytes each). */
		for (b = 0; b < 8; b++) {
			h ^= (lo >> (b * 8)) & UINT64_C(0xff);
			h *= UINT64_C(0x100000001b3);
		}
		for (b = 0; b < 8; b++) {
			h ^= (hi >> (b * 8)) & UINT64_C(0xff);
			h *= UINT64_C(0x100000001b3);
		}
	}
	return (h);
}

bool
sm_equals(const sm_t *a, const sm_t *b)
{
	__sm_run_iter_t ia;
	__sm_run_iter_t ib;
	uint64_t alo;
	uint64_t ahi;
	uint64_t blo;
	uint64_t bhi;
	bool have_a;
	bool have_b;
	const bool a_empty = (a == NULL) || sm_is_empty(a);
	const bool b_empty = (b == NULL) || sm_is_empty(b);
	if (a_empty && b_empty)
		return (true);
	if (a_empty != b_empty)
		return (false);

	/* Two sets are equal iff their maximal-run decompositions are the
	 * identical interval sequence.  Walk both run streams in lockstep
	 * (see __sm_run_iter_t), so a 2^31-bit run is one comparison rather
	 * than 2^31 bit lookups. */
	__sm_run_iter_init(&ia, a);
	__sm_run_iter_init(&ib, b);
	alo = 0; ahi = 0; blo = 0; bhi = 0;
	have_a = __sm_run_next(&ia, &alo, &ahi);
	have_b = __sm_run_next(&ib, &blo, &bhi);
	while (have_a && have_b) {
		if (alo != blo || ahi != bhi)
			return (false);
		have_a = __sm_run_next(&ia, &alo, &ahi);
		have_b = __sm_run_next(&ib, &blo, &bhi);
	}
	return (have_a == have_b);
}

int
sm_compare(const sm_t *a, const sm_t *b)
{
	uint64_t alo;
	uint64_t ahi;
	uint64_t blo;
	uint64_t bhi;
	bool have_a;
	bool have_b;
	uint64_t pa;
	uint64_t pb;
	/* Lexicographic order on the ascending member sequences: at the
	 * first index where the two sequences differ, the map with the
	 * smaller index sorts first; if one sequence is a proper prefix of
	 * the other, the shorter one sorts first.  Walk both run streams
	 * (see __sm_run_iter_t) with a cursor into the current run of each,
	 * advancing over shared prefixes a whole run at a time, so a
	 * 2^31-bit run costs O(chunks) rather than O(cardinality). */
	__sm_run_iter_t ia, ib;
	__sm_run_iter_init(&ia, a);
	__sm_run_iter_init(&ib, b);
	alo = 0; ahi = 0; blo = 0; bhi = 0;
	have_a = __sm_run_next(&ia, &alo, &ahi);
	have_b = __sm_run_next(&ib, &blo, &bhi);
	/* pa/pb are the next unconsumed member of the current a/b run. */
	pa = alo; pb = blo;
	while (have_a && have_b) {
		uint64_t a_rem;
		uint64_t b_rem;
		uint64_t step;
		if (pa < pb)
			return (-1);
		if (pa > pb)
			return (1);
		/* pa == pb: both runs share consecutive members up to the
		 * shorter run's end; skip that common prefix at once. */
		a_rem = ahi - pa;
		b_rem = bhi - pb;
		step = a_rem < b_rem ? a_rem : b_rem;
		pa += step;
		pb += step;
		if (pa >= ahi) {
			have_a = __sm_run_next(&ia, &alo, &ahi);
			pa = alo;
		}
		if (pb >= bhi) {
			have_b = __sm_run_next(&ib, &blo, &bhi);
			pb = blo;
		}
	}
	if (!have_a && !have_b)
		return (0);
	return (!have_a ? -1 : 1); /* shorter sequence sorts first */
}

sm_subset_relation_t
sm_subset_compare(const sm_t *a, const sm_t *b)
{
	uint64_t alo;
	uint64_t ahi;
	uint64_t blo;
	uint64_t bhi;
	bool have_a;
	bool have_b;
	uint64_t pos;
	bool have_pos;
	bool a_subset_b = true; /* every bit in a is in b */
	bool b_subset_a = true; /* every bit in b is in a */

	/* Interval sweep over the two run streams (see __sm_run_iter_t): a
	 * span present in exactly one map witnesses that map is not a
	 * subset of the other.  Once both witnesses fire the answer is
	 * DIFFERENT.  Run-based, so a 2^31-bit run costs O(chunks) rather
	 * than O(cardinality). */
	__sm_run_iter_t ia, ib;
	__sm_run_iter_init(&ia, a);
	__sm_run_iter_init(&ib, b);
	alo = 0; ahi = 0; blo = 0; bhi = 0;
	have_a = __sm_run_next(&ia, &alo, &ahi);
	have_b = __sm_run_next(&ib, &blo, &bhi);
	/* pos = left edge of the not-yet-classified region. */
	pos = 0;
	have_pos = false;
	while (have_a || have_b) {
		bool in_a;
		bool in_b;
		uint64_t next;
		uint64_t lo = have_a ? alo : blo;
		if (have_b && blo < lo)
			lo = blo;
		if (!have_pos || pos < lo) {
			pos = lo;
			have_pos = true;
		}
		in_a = have_a && pos >= alo && pos < ahi;
		in_b = have_b && pos >= blo && pos < bhi;
		/* End of the current homogeneous segment. */
		next = UINT64_MAX;
		if (have_a) {
			if (pos < alo && alo < next)
				next = alo;
			if (pos >= alo && ahi < next)
				next = ahi;
		}
		if (have_b) {
			if (pos < blo && blo < next)
				next = blo;
			if (pos >= blo && bhi < next)
				next = bhi;
		}
		if (in_a && !in_b)
			a_subset_b = false; /* a has a bit b doesn't */
		else if (in_b && !in_a)
			b_subset_a = false; /* b has a bit a doesn't */
		if (!a_subset_b && !b_subset_a)
			return (SM_REL_DIFFERENT);
		pos = next;
		if (have_a && pos >= ahi)
			have_a = __sm_run_next(&ia, &alo, &ahi);
		if (have_b && pos >= bhi)
			have_b = __sm_run_next(&ib, &blo, &bhi);
	}
	if (a_subset_b && b_subset_a)
		return (SM_REL_EQUAL);
	if (a_subset_b)
		return (SM_REL_SUBSET_A);
	return (SM_REL_SUBSET_B);
}

uint64_t
sm_pop_first(sm_t *map)
{
	uint64_t lowest;
	if (sm_is_empty(map))
		return (SM_IDX_MAX);
	lowest = sm_next_member(map, SM_IDX_MAX, NULL);
	if (lowest == SM_IDX_MAX)
		return (SM_IDX_MAX);
	if (sm_remove(map, lowest) == SM_IDX_MAX) {
		/* Should never happen on a populated map (remove only fails on
		 * ENOSPC for chunk separation, and we're removing not adding). */
		return (SM_IDX_MAX);
	}
	return (lowest);
}

uint64_t
sm_pop_last(sm_t *map)
{
	uint64_t highest;
	if (sm_is_empty(map))
		return (SM_IDX_MAX);
	highest = sm_prev_member(map, SM_IDX_MAX, NULL);
	if (highest == SM_IDX_MAX)
		return (SM_IDX_MAX);
	if (sm_remove(map, highest) == SM_IDX_MAX)
		return (SM_IDX_MAX);
	return (highest);
}

/* -------------------------------------------------------------------
 * In-place set operations.  These mutate `dst` and return it (or a
 * possibly-relocated pointer if dst grew).
 * ------------------------------------------------------------------- */

/*
 * In-place set ops are implemented as "compute via the chunk-pair-walk
 * in sm_union/sm_intersection/sm_difference, then memcpy the result's
 * bytes back into dst's buffer".  This delegates the actual merge to
 * the chunk-aware out-of-place version, paying one allocation for the
 * temporary result.  An alternative would be a two-pointer chunk walk
 * that writes directly into dst's buffer; that's a substantial refactor
 * with minimal speedup over the current approach (sm_union's own walk
 * is already chunk-aware and the memcpy is a single block copy).
 */
static sm_t *
__sm_replace_buffer(sm_t *dst, sm_t *result)
{
	size_t result_size;
	if (result == NULL) {
		/* Empty result -- clear dst. */
		sm_clear(dst);
		return (dst);
	}
	result_size = result->m_data_used;
	if (__sm_cap(dst) < result_size) {
		sm_t *grown = sm_set_data_size(dst, NULL, result_size + 64);
		if (grown == NULL) {
			sm_free(result);
			return (NULL);
		}
		dst = grown;
	}
	memcpy(dst->m_data, result->m_data, result_size);
	dst->m_data_used = result_size;
	__sm_card_invalidate(dst);
	sm_free(result);
	__sm_try_demote(dst);
	return (dst);
}

sm_t *
sm_union_inplace(sm_t *dst, const sm_t *src)
{
	if (dst == NULL)
		return (NULL);
	if (sm_is_empty(src))
		return (dst);
	if (sm_is_empty(dst)) {
		/* dst becomes a copy of src.  Use the chunk-aware copy path. */
		sm_t *copy = sm_copy(src);
		if (copy == NULL)
			return (NULL);
		return (__sm_replace_buffer(dst, copy));
	}
	return (__sm_replace_buffer(dst, sm_union(dst, src)));
}

sm_t *
sm_intersection_inplace(sm_t *dst, const sm_t *src)
{
	if (dst == NULL)
		return (NULL);
	if (sm_is_empty(dst))
		return (dst);
	if (sm_is_empty(src)) {
		sm_clear(dst);
		return (dst);
	}
	return (__sm_replace_buffer(dst, sm_intersection(dst, src)));
}

sm_t *
sm_difference_inplace(sm_t *dst, const sm_t *src)
{
	if (dst == NULL)
		return (NULL);
	if (sm_is_empty(dst) || sm_is_empty(src))
		return (dst);
	return (__sm_replace_buffer(dst, sm_difference(dst, src)));
}

sm_t *
sm_xor_inplace(sm_t *dst, const sm_t *src)
{
	if (dst == NULL)
		return (NULL);
	/* XOR with nothing is a no-op; XOR into nothing is a copy of src. */
	if (sm_is_empty(src))
		return (dst);
	if (sm_is_empty(dst))
		return (__sm_replace_buffer(dst, sm_copy(src)));
	return (__sm_replace_buffer(dst, sm_xor(dst, src)));
}

/* -------------------------------------------------------------------
 * Maintenance and introspection: range flip, validate, statistics,
 * shrink_to_fit
 * ------------------------------------------------------------------- */

bool
sm_flip_range(sm_t *map, uint64_t lo, uint64_t hi)
{
	if (map == NULL || lo >= hi)
		return (lo >= hi);
	return (__sm_rewrite_range(map, lo, hi, true));
}

bool
sm_validate(const sm_t *map)
{
	size_t count;
	uint8_t * p;
	uint8_t * end;
	__sm_idx_t prev_start;
	uint64_t prev_end;
	bool first;
	size_t i;
	if (map == NULL)
		return (true);
	if (map->m_data == NULL && __sm_cap(map) > 0)
		return (false);
	if (map->m_data_used > __sm_cap(map))
		return (false);
	if (map->m_data_used == 0) {
		return (true);
	}
	if (map->m_data_used < SM_SIZEOF_OVERHEAD)
		return (false);

	/* Small-set mode: the header word's top bit is set and the low
	 * bits hold the word count.  Valid iff nwords <= the span cap and
	 * m_data_used exactly covers the header plus that many words. */
	if (__sm_is_small(map)) {
		const size_t nwords = __sm_small_nwords(map);
		if (nwords > SM_SMALL_MAX_WORDS)
			return (false);
		if (map->m_data_used !=
		    SM_SIZEOF_OVERHEAD + nwords * sizeof(uint64_t))
			return (false);
		/* A trailing all-zero word would mean a non-canonical form
		 * (remove trims them); reject so equal sets have one encoding. */
		if (nwords > 0 && __sm_small_words(map)[nwords - 1] == 0)
			return (false);
		return (true);
	}

	count = __sm_get_chunk_count(map);
	if (count == 0) {
		return (map->m_data_used == SM_SIZEOF_OVERHEAD);
	}

	p = __sm_get_chunk_data(map, 0);
	end = map->m_data + map->m_data_used;
	prev_start = 0;
	prev_end = 0; /* start + capacity of the previous chunk */
	first = true;
	for (i = 0; i < count; i++) {
		__sm_idx_t start;
		__sm_chunk_t chunk;
		size_t chunk_size;
		size_t capacity;
		uint64_t chunk_end;
		if (p + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) > end) {
			return (false);
		}
		start = __sm_load_idx((const uint8_t *)p);
		if (!first && start <= prev_start) {
			return (false);
		}
		/* (b) chunk starts are chunk-aligned bit indices. */
		if (start % SM_CHUNK_MAX_CAPACITY != 0) {
			return (false);
		}
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		chunk_size = __sm_chunk_get_size(&chunk);
		if (p + SM_SIZEOF_OVERHEAD + chunk_size > end) {
			return (false);
		}
		/* (a) No chunk shape is special-cased here: every descriptor is
		 * read as a sparse chunk (this build has only one chunk kind).
		 * __sm_chunk_get_size returned a bounded stride above (at most a
		 * descriptor word plus 32 payload words), so the walk did not
		 * over-read; the structural checks below (bounds, chunk-aligned
		 * start, ascending start, non-overlap, exact count) bound every
		 * read and reject any malformed stream.  A stream written by the
		 * sibling variant that happens to set a descriptor's top two bits
		 * to 01 simply denotes a sparse chunk with its highest vector
		 * unused; it is accepted only if it is otherwise structurally
		 * valid and round-trips as an ordinary sparse map.  See the
		 * header note and the design notes under docs/. */
		capacity = __sm_chunk_get_capacity(&chunk);
		/* (h) every chunk holds a member: the writers drop a chunk
		 * once it empties, and the readers disagree on an empty one
		 * (sm_minimum reports an empty chunk's start). */
		if ((chunk.m_data[0] & SM_DESC_HI_BITS) == 0) {
			return (false);
		}
		/* (f) sparse descriptor shape: every data-bearing slot
		 * (SM_PAYLOAD_ONES / SM_PAYLOAD_MIXED) must lie within the
		 * chunk's measured capacity.  __sm_chunk_get_capacity reports
		 * SM_CHUNK_MAX_CAPACITY minus 64 bits per SM_PAYLOAD_NONE flag
		 * wherever that flag sits, but the slot-indexed readers (rank /
		 * cardinality / select / minimum / maximum) place a slot's bits
		 * at its fixed position slot*64 while the capacity-bounded
		 * readers (contains / next_member) stop at start+capacity.  When
		 * a NONE flag sits below a data-bearing slot the two disagree:
		 * sm_cardinality counts the high slot's bits, sm_next_member /
		 * sm_contains skip them.  The encoder never emits such a chunk
		 * (every data-bearing slot it writes fits inside the reduced
		 * capacity), so reject any crafted buffer that violates this --
		 * it is the one sparse shape sm_validate used to accept while
		 * the readers answered inconsistently.  This build has only one
		 * chunk kind, so the check runs on every chunk; a NONE in slot
		 * 31 (the sibling variant reserves bits 63:62 == 01 there)
		 * simply means that highest vector is unused and is not
		 * data-bearing, so it is accepted when the real data still
		 * fits.
		 *
		 * Slot s occupies descriptor bits 2s+1:2s, and ONES (2#11) and
		 * MIXED (2#10) are exactly the flags whose HIGH bit is set (the
		 * _Static_assert after the SM_PAYLOAD_* enum pins this).  So
		 * hi = desc & 0xAAAA... has bit 2s+1 set iff slot s carries
		 * data; hi == 0 means no data slot (accept); otherwise the top
		 * set bit 63 - clz(hi) is odd, = 2s+1 for the highest data slot
		 * s, and halving it gives s (0..31, so (s+1)*64 cannot
		 * overflow).  One mask and one count-leading-zeros per chunk:
		 * the 5.8.1 form scanned all 32 slots and cost ~75% of
		 * sm_open_copy on real maps.  SM_CLZ64 is undefined at 0, hence
		 * the hi != 0 guard. */
		{
			const __sm_bitvec_t hi = chunk.m_data[0] &
			    (__sm_bitvec_t)UINT64_C(0xAAAAAAAAAAAAAAAA);
			if (hi != 0) {
				const size_t highest_data =
				    (size_t)(63 - SM_CLZ64(hi)) / 2;
				if ((highest_data + 1) *
				        (size_t)SM_BITS_PER_VECTOR > capacity)
					return (false);
			}
		}
		/* (c) [start, start + capacity) must not extend past the
		 * addressable index space.  A chunk that ends exactly at 2^64
		 * (start + capacity wraps to 0) is legal -- it holds the top
		 * bits [2^64 - capacity, 2^64).  Only a wrap to a nonzero end
		 * is an overflow. */
		chunk_end = start + capacity;
		if (chunk_end != 0 && chunk_end < start) {
			return (false);
		}
		/* (g) bit 2^64 - 1 is SM_IDX_MAX, not a member, so no chunk
		 * may set it: the writers never do, and every reader assumes
		 * it.  Only a chunk that reaches 2^64 can hold it, as its
		 * last bit. */
		if (chunk_end == 0 && capacity > 0 &&
		    __sm_chunk_is_set(&chunk, capacity - 1)) {
			return (false);
		}
		/* (d) [start, start + capacity) must not overlap the span of
		 * the preceding chunk.  prev_end == 0 means the previous chunk
		 * reached 2^64; ascending starts already forbid a follower. */
		if (!first && prev_end != 0 && start < prev_end) {
			return (false);
		}
		p += SM_SIZEOF_OVERHEAD + chunk_size;
		prev_start = start;
		prev_end = chunk_end;
		first = false;
	}
	/* (e) the stored chunk count must match the walk exactly: the walk
	 * consumed `count` chunks above, so leftover bytes mean the count
	 * disagrees with the encoded stream. */
	return (p == end);
}

/*
 * The chunk breakdown of a small-mode map for sm_statistics: the chunk
 * view its words materialize to, which is what sm_statistics reported
 * through 5.8.5 by building that view.  A nonempty set lies in one
 * sparse chunk at start 0.  bytes_per_set_bit is the view's footprint,
 * __sm_small_chunk_bytes, per set bit.  The caller fills bytes_used and
 * bytes_capacity from the map itself.
 * NB: stats must be zeroed.
 */
static void
__sm_small_statistics(const sm_t *map, sm_stats_t *stats)
{
	const uint64_t card = __sm_small_cardinality(map);

	if (card == 0) {
		return;
	}
	stats->chunks_total = 1;
	stats->chunks_sparse = 1;
	stats->bits_set = card;
	stats->bytes_per_set_bit =
	    (double)__sm_small_chunk_bytes(map) / (double)card;
}

void
sm_statistics(const sm_t *map, sm_stats_t *stats)
{
	size_t count;
	uint8_t * p;
	size_t i;
	if (stats == NULL)
		return;
	memset(stats, 0, sizeof(*stats));
	if (map == NULL)
		return;

	if (__sm_is_small(map)) {
		__sm_small_statistics(map, stats);
		stats->bytes_used = sm_get_size((sm_t *)map);
		stats->bytes_capacity = sm_get_capacity(map);
		return;
	}

	stats->bytes_used = sm_get_size((sm_t *)map);
	stats->bytes_capacity = sm_get_capacity(map);

	count = __sm_get_chunk_count(map);
	stats->chunks_total = count;
	if (count == 0)
		return;

	p = __sm_get_chunk_data(map, 0);
	for (i = 0; i < count; i++) {
		size_t chunk_size;
		__sm_bitvec_t desc;
		size_t pos;
		size_t v;
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		chunk_size = __sm_chunk_get_size(&chunk);
		/* Every chunk is sparse in this build. */
		stats->chunks_sparse++;
		desc = chunk.m_data[0];
		pos = 1;
		for (v = 0; v < SM_FLAGS_PER_INDEX; v++) {
			const size_t flags = SM_CHUNK_GET_FLAGS(desc, v);
			if (flags == SM_PAYLOAD_ONES) {
				stats->bits_set += SM_BITS_PER_VECTOR;
			} else if (flags == SM_PAYLOAD_MIXED) {
				stats->bits_set +=
				    (uint64_t)SM_POPCOUNT64(chunk.m_data[pos]);
				pos++;
			}
		}
		p += SM_SIZEOF_OVERHEAD + chunk_size;
	}
	stats->bytes_per_set_bit = stats->bits_set == 0 ?
	    0.0 :
	    (double)stats->bytes_used / (double)stats->bits_set;
}

sm_t *
sm_shrink_to_fit(sm_t *map)
{
	size_t target;
	if (map == NULL)
		return (NULL);
	if (__sm_kind(map) == SM_WRAPPED)
		return (map);

	target = map->m_data_used > 0 ? map->m_data_used : SM_SIZEOF_OVERHEAD;
	if (target == __sm_cap(map))
		return (map);

	return (sm_set_data_size(map, NULL, target));
}

/* -------------------------------------------------------------------
 * Portable serialization
 * ------------------------------------------------------------------- */

#define SM_WIRE_MAGIC      0x30316d73u /* "sm10" little-endian */
#define SM_WIRE_VERSION    2u
#define SM_WIRE_HEADER_LEN 16u
#define SM_WIRE_FLAG_LE    0x01u
/* Reserved header byte out[6]: a documented mirror of the body's own
 * small-set marker (the body's header top bit is authoritative). */
#define SM_WIRE_FLAG_SMALL 0x01u

static bool
__sm_host_is_little_endian(void)
{
	const uint16_t one = 1;
	return (((const uint8_t *)&one)[0] == 1);
}

/*
 * Reverse the byte order of a 64-bit word.  Portable shifts and masks,
 * no compiler builtin; the reader calls it once per body word of a
 * stream written on a host of the other byte order.
 */
static uint64_t
__sm_bswap64(uint64_t v)
{
	v = ((v & UINT64_C(0x00FF00FF00FF00FF)) << 8) |
	    ((v >> 8) & UINT64_C(0x00FF00FF00FF00FF));
	v = ((v & UINT64_C(0x0000FFFF0000FFFF)) << 16) |
	    ((v >> 16) & UINT64_C(0x0000FFFF0000FFFF));
	return ((v << 32) | (v >> 32));
}

size_t
sm_serialized_size(const sm_t *map)
{
	if (map == NULL)
		return (SM_WIRE_HEADER_LEN + SM_SIZEOF_OVERHEAD);
	return (SM_WIRE_HEADER_LEN + sm_get_size((sm_t *)map));
}

size_t
sm_serialize(const sm_t *map, uint8_t *out, size_t out_size)
{
	size_t needed;
	uint64_t cardinality;
	uint8_t flags;
	uint32_t magic;
	if (out == NULL)
		return (0);
	needed = sm_serialized_size(map);
	if (out_size < needed)
		return (0);

	cardinality = (map == NULL || sm_is_empty(map)) ? 0 : sm_cardinality((sm_t *)map);
	flags = __sm_host_is_little_endian() ? SM_WIRE_FLAG_LE : 0;

	/* Header: writes via memcpy so it works on strict-alignment cpus. */
	magic = SM_WIRE_MAGIC;
	memcpy(out + 0, &magic, 4);
	out[4] = SM_WIRE_VERSION;
	out[5] = flags;
	/* The flag mirrors the body written below, which for an empty map is
	 * the chunk-mode header whichever mode the map is in. */
	out[6] = (map != NULL && !sm_is_empty(map) && __sm_is_small(map)) ?
	    SM_WIRE_FLAG_SMALL : 0;
	out[7] = 0;
	memcpy(out + 8, &cardinality, 8);

	/* Body: existing internal format (or just an SM_SIZEOF_OVERHEAD
	 * zeroed header for NULL/empty maps). */
	if (map == NULL || sm_is_empty(map)) {
		memset(out + SM_WIRE_HEADER_LEN, 0, SM_SIZEOF_OVERHEAD);
	} else {
		memcpy(out + SM_WIRE_HEADER_LEN, sm_get_data((sm_t *)map),
		    sm_get_size((sm_t *)map));
	}
	return (needed);
}

sm_t *
sm_deserialize(const uint8_t *in, size_t n)
{
	/* The magic "sm10" as a little-endian writer emits it. */
	static const uint8_t magic_le[4] = { 0x73, 0x6d, 0x31, 0x30 };
	uint8_t version;
	uint8_t flags;
	bool wire_is_le;
	bool swap;
	size_t body_len;
	sm_t * map;
	size_t i;
	if (in == NULL || n < SM_WIRE_HEADER_LEN + SM_SIZEOF_OVERHEAD) {
		return (NULL);
	}
	version = in[4];
	flags = in[5];
	if (version != SM_WIRE_VERSION)
		return (NULL);

	/*
	 * The writer stored the magic and every body word in its own byte
	 * order and said which in the flags.  The magic bytes must agree
	 * with that flag: little-endian 73 6d 31 30, big-endian 30 31 6d 73.
	 * A stream from the other byte order is accepted and byte-swapped
	 * below; the cardinality hint at offset 8 is not read.
	 */
	wire_is_le = (flags & SM_WIRE_FLAG_LE) != 0;
	for (i = 0; i < 4; i++) {
		if (in[i] != magic_le[wire_is_le ? i : 3 - i])
			return (NULL);
	}
	swap = wire_is_le != __sm_host_is_little_endian();

	/* Body: starts at offset SM_WIRE_HEADER_LEN. */
	body_len = n - SM_WIRE_HEADER_LEN;
	map = sm_create(body_len + 64);
	if (map == NULL)
		return (NULL);

	/* Copy the body into the map's data buffer.  The first SM_SIZEOF_OVERHEAD
	 * bytes are the chunk count; the rest is chunks. */
	memcpy(map->m_data, in + SM_WIRE_HEADER_LEN, body_len);
	/* Every body field is a u64 (chunk count or small-set header, chunk
	 * starts, descriptors, payload and small-set words), so converting a
	 * foreign-order body is one swap per 8-byte word.  A well-formed body
	 * is a whole number of words; anything else is rejected here rather
	 * than half-swapped. */
	if (swap) {
		if (body_len % sizeof(uint64_t) != 0) {
			sm_free(map);
			return (NULL);
		}
		for (i = 0; i < body_len; i += sizeof(uint64_t)) {
			__sm_store_u64(&map->m_data[i],
			    __sm_bswap64(__sm_load_u64(&map->m_data[i])));
		}
	}
	/* Force m_data_used to its expected value: the first 4 bytes contain
	 * chunk_count, then we need to walk to compute total size.
	 * sm_open's pattern handles this. */
	map->m_data_used = body_len;

	/* Validate the result; reject malformed input. */
	__sm_drop_empty_chunks(map);
	if (!sm_validate(map)) {
		sm_free(map);
		return (NULL);
	}
	return (map);
}

/**
 * @brief Copy a raw chunk (start offset + descriptor + vectors) into result.
 */
static bool
__sm_copy_chunk_to_result(sm_t **resultp, const uint8_t *chunk_ptr)
{
	const __sm_chunk_t chunk = { (__sm_bitvec_unaligned_t *)(chunk_ptr +
		                         SM_SIZEOF_OVERHEAD) };
	const size_t chunk_bytes =
	    SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
	if (!__sm_ensure_capacity(resultp, chunk_bytes)) {
		return (false);
	}
	if (SM_UNLIKELY(
	        !__sm_append_data(*resultp, chunk_ptr, chunk_bytes))) {
		return (false);
	}
	__sm_set_chunk_count(*resultp, __sm_get_chunk_count(*resultp) + 1);
	return (true);
}

/* -------------------------------------------------------------------
 * Set operations: chunk-merge intersection, difference, union
 * ------------------------------------------------------------------- */

/**
 * @brief Create a new sparsemap containing the intersection of a and b.
 *
 * Uses a two-pointer chunk merge walk for O(chunks) performance instead
 * of the previous O(cardinality x chunks) bit-by-bit scan+contains.
 */
sm_t *
sm_intersection(const sm_t *a, const sm_t *b)
{
	size_t a_count;
	size_t b_count;
	size_t cap;
	sm_t * result;
	uint8_t * ap;
	uint8_t * bp;
	size_t ai;
	size_t bi;
	__sm_check_invariants(a);
	__sm_check_invariants(b);
	if (a == NULL || b == NULL) {
		return (NULL);
	}
	if (__sm_is_small(a) || __sm_is_small(b)) {
		bool oa, ob;
		const sm_t *va = __sm_chunk_view(a, &oa);
		const sm_t *vb = __sm_chunk_view(b, &ob);
		sm_t *r = (va == NULL || vb == NULL) ? NULL
		                                     : sm_intersection(va, vb);
		if (oa) sm_free((sm_t *)va);
		if (ob) sm_free((sm_t *)vb);
		return (r);
	}

	a_count = __sm_get_chunk_count(a);
	b_count = __sm_get_chunk_count(b);

	if (a_count == 0 || b_count == 0) {
		return (NULL);
	}

	cap = a->m_data_used;
	{
		size_t cap_b = b->m_data_used;
		if (cap_b > cap)
			cap = cap_b;
	}
	if (cap < 1024)
		cap = 1024;

	result = sparsemap(cap);
	if (result == NULL) {
		return (NULL);
	}

	ap = __sm_get_chunk_data(a, 0);
	bp = __sm_get_chunk_data(b, 0);
	ai = 0; bi = 0;

	while (ai < a_count && bi < b_count) {
		size_t a_size;
		size_t a_end;
		__sm_idx_t b_start;
		__sm_chunk_t b_chunk;
		size_t b_size;
		size_t b_end;
		/* Read chunk a metadata */
		const __sm_idx_t a_start = __sm_load_idx((const uint8_t *)ap);
		__sm_chunk_t a_chunk;
		__sm_chunk_init(&a_chunk, ap + SM_SIZEOF_OVERHEAD);
		a_size = __sm_chunk_get_size(&a_chunk);
		a_end = (size_t)__sm_chunk_window_end(a_start);

		/* Read chunk b metadata */
		b_start = __sm_load_idx((const uint8_t *)bp);
		__sm_chunk_init(&b_chunk, bp + SM_SIZEOF_OVERHEAD);
		b_size = __sm_chunk_get_size(&b_chunk);
		b_end = (size_t)__sm_chunk_window_end(b_start);

		/* Prefetch next chunks */
		if (ai + 1 < a_count) {
			SM_PREFETCH(ap + SM_SIZEOF_OVERHEAD + a_size);
		}
		if (bi + 1 < b_count) {
			SM_PREFETCH(bp + SM_SIZEOF_OVERHEAD + b_size);
		}

		/* No overlap: a is entirely before b */
		if (a_end <= b_start) {
			ap += SM_SIZEOF_OVERHEAD + a_size;
			ai++;
			continue;
		}

		/* No overlap: b is entirely before a */
		if (b_end <= a_start) {
			bp += SM_SIZEOF_OVERHEAD + b_size;
			bi++;
			continue;
		}

		/* Chunks overlap.  Every chunk is sparse, so two overlapping
		 * chunks share the same 2048-bit-aligned start: AND them
		 * word-by-word. */
		__sm_assert(a_start == b_start);
		{
			__sm_bitvec_t rw[32];
			int rc[32];
			__sm_bitvec_t desc;
			__sm_bitvec_t vecs[32];
			int nvecs;
			int i;
			/* Word-level AND of two aligned sparse chunks */
			__sm_bitvec_t aw[32], bw[32];
			int ac[32], bc[32];
			__sm_expand_sparse_chunk(&a_chunk, aw, ac);
			__sm_expand_sparse_chunk(&b_chunk, bw, bc);

			__sm_words_and(rw, aw, bw);
			for (i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
				rc[i] = (ac[i] && bc[i]) ? 1 : 0;
				if (!rc[i])
					rw[i] = 0;
			}

			if (__sm_encode_sparse_chunk(rw, rc, &desc, vecs,
			        &nvecs)) {
				if (!__sm_append_sparse_chunk(&result, a_start,
				        desc, vecs, nvecs)) {
					sm_free(result);
					return (NULL);
				}
			}
		}

		/* Advance whichever chunk ends first */
		if (a_end <= b_end) {
			ap += SM_SIZEOF_OVERHEAD + a_size;
			ai++;
		}
		if (b_end <= a_end) {
			bp += SM_SIZEOF_OVERHEAD + b_size;
			bi++;
		}
	}

	if (__sm_get_chunk_count(result) == 0) {
		sm_free(result);
		return (NULL);
	}

	__sm_try_demote(result);
	return (result);
}

/**
 * @brief Emit set bits from a sparse chunk within [from, to) into result.
 *
 * Uses expand-mask-encode for bulk processing.  Every chunk is sparse in
 * this build.
 */
static bool
__sm_emit_chunk_bits(sm_t **resultp, const __sm_chunk_t *chunk,
    __sm_idx_t chunk_start, size_t from, size_t to)
{
	__sm_bitvec_t words[SM_FLAGS_PER_INDEX];
	int cap_flags[SM_FLAGS_PER_INDEX];
	size_t rel_from;
	size_t rel_to;
	int start_word;
	int end_word;
	__sm_bitvec_t desc;
	__sm_bitvec_t vecs[SM_FLAGS_PER_INDEX];
	int nvecs;
	int i;
	if (from >= to)
		return (true);

	/* Sparse: expand, mask to [from, to) range, encode and append */
	__sm_expand_sparse_chunk(chunk, words, cap_flags);

	/* Mask out bits outside [from, to) range relative to chunk_start */
	rel_from = from - (size_t)chunk_start;
	rel_to = to - (size_t)chunk_start;
	start_word = (int)(rel_from / SM_BITS_PER_VECTOR);
	end_word = (int)((rel_to + SM_BITS_PER_VECTOR - 1) / SM_BITS_PER_VECTOR);

	/* Zero words entirely before the range */
	for (i = 0; i < start_word && i < (int)SM_FLAGS_PER_INDEX; i++) {
		words[i] = 0;
		cap_flags[i] = 0;
	}

	/* Mask partial start word */
	if (start_word < (int)SM_FLAGS_PER_INDEX) {
		const size_t start_bit = rel_from % SM_BITS_PER_VECTOR;
		if (start_bit > 0) {
			words[start_word] &= ~((__sm_bitvec_t)0) << start_bit;
		}
	}

	/* Zero words entirely after the range */
	for (i = end_word; i < (int)SM_FLAGS_PER_INDEX; i++) {
		words[i] = 0;
		cap_flags[i] = 0;
	}

	/* Mask partial end word */
	if (end_word > 0 && end_word <= (int)SM_FLAGS_PER_INDEX) {
		const size_t end_bit = rel_to % SM_BITS_PER_VECTOR;
		if (end_bit > 0) {
			words[end_word - 1] &=
			    ((__sm_bitvec_t)1 << end_bit) - 1;
		}
	}

	if (__sm_encode_sparse_chunk(words, cap_flags, &desc, vecs, &nvecs)) {
		if (!__sm_append_sparse_chunk(resultp, chunk_start, desc, vecs,
		        nvecs)) {
			return (false);
		}
	}
	return (true);
}

/**
 * @brief Create a new sparsemap containing the difference a \ b (bits in a but not in b).
 *
 * Uses a two-pointer chunk merge walk with a cursor to track progress
 * through each a chunk, preventing double-counting when one a chunk
 * overlaps with multiple b chunks.
 */
sm_t *
sm_difference(const sm_t *a, const sm_t *b)
{
	size_t a_count;
	size_t b_count;
	size_t cap;
	sm_t * result;
	uint8_t * ap;
	uint8_t * bp;
	size_t ai;
	size_t bi;
	__sm_check_invariants(a);
	__sm_check_invariants(b);
	if (a == NULL) {
		return (NULL);
	}
	if (__sm_is_small(a) || __sm_is_small(b)) {
		bool oa, ob;
		const sm_t *va = __sm_chunk_view(a, &oa);
		const sm_t *vb = __sm_chunk_view(b, &ob);
		sm_t *r = (va == NULL || (b != NULL && vb == NULL))
		    ? NULL
		    : sm_difference(va, vb);
		if (oa) sm_free((sm_t *)va);
		if (ob) sm_free((sm_t *)vb);
		return (r);
	}

	a_count = __sm_get_chunk_count(a);
	if (a_count == 0) {
		return (NULL);
	}

	/* If b is NULL or empty, return a copy of a */
	if (b == NULL || __sm_get_chunk_count(b) == 0) {
		return (sm_copy(a));
	}

	b_count = __sm_get_chunk_count(b);

	cap = a->m_data_used;
	if (cap < 1024)
		cap = 1024;

	result = sparsemap(cap);
	if (result == NULL) {
		return (NULL);
	}

	ap = __sm_get_chunk_data(a, 0);
	bp = __sm_get_chunk_data(b, 0);
	ai = 0; bi = 0;

	while (ai < a_count) {
		size_t a_size;
		size_t a_end;
		size_t a_cursor;
		uint8_t * bp_save;
		size_t bi_save;
		/* Read chunk a metadata */
		const __sm_idx_t a_start = __sm_load_idx((const uint8_t *)ap);
		__sm_chunk_t a_chunk;
		__sm_chunk_init(&a_chunk, ap + SM_SIZEOF_OVERHEAD);
		a_size = __sm_chunk_get_size(&a_chunk);
		a_end = (size_t)__sm_chunk_window_end(a_start);

		/* Prefetch next a chunk */
		if (ai + 1 < a_count) {
			SM_PREFETCH(ap + SM_SIZEOF_OVERHEAD + a_size);
		}

		/* If b is exhausted, copy remaining a chunks */
		if (bi >= b_count) {
			if (!__sm_copy_chunk_to_result(&result, ap)) {
				sm_free(result);
				return (NULL);
			}
			ap += SM_SIZEOF_OVERHEAD + a_size;
			ai++;
			continue;
		}

		/* Cursor: tracks how far into this a chunk we've processed */
		a_cursor = (size_t)a_start;

		/* Save b state so we can iterate b within this a chunk */
		bp_save = bp;
		bi_save = bi;

		/* Process all b chunks that overlap with this a chunk */
		while (bi < b_count) {
			size_t b_size;
			size_t b_end;
			size_t ov_start;
			size_t ov_end;
			const __sm_idx_t b_start =
			    __sm_load_idx((const uint8_t *)bp);
			__sm_chunk_t b_chunk;
			__sm_chunk_init(&b_chunk, bp + SM_SIZEOF_OVERHEAD);
			b_size = __sm_chunk_get_size(&b_chunk);
			b_end = (size_t)__sm_chunk_window_end(b_start);

			/* b is past a: no more overlaps for this a chunk */
			if (a_end <= (size_t)b_start)
				break;

			/* b is entirely before cursor: skip b */
			if (b_end <= a_cursor) {
				bp += SM_SIZEOF_OVERHEAD + b_size;
				bi++;
				continue;
			}

			/* Overlap region */
			ov_start = (size_t)b_start > a_cursor ?
			    (size_t)b_start :
			    a_cursor;
			ov_end = a_end < b_end ? a_end : b_end;
			(void)ov_end; /* overlap fully consumes a below */

			/* Emit a's surviving bits in the gap [a_cursor, ov_start) */
			if (!__sm_emit_chunk_bits(&result, &a_chunk,
			        a_start, a_cursor, ov_start)) {
				sm_free(result);
				return (NULL);
			}

			/* Process overlap: every chunk is sparse and two
			 * overlapping chunks share the same aligned start. */
			__sm_assert(a_start == b_start);
			{
				__sm_bitvec_t rw[32];
				int rc[32];
				__sm_bitvec_t desc;
				__sm_bitvec_t vecs[32];
				int nvecs;
				int i;
				__sm_bitvec_t aw[32], bw[32];
				int ac[32], bc[32];
				__sm_expand_sparse_chunk(&a_chunk, aw, ac);
				__sm_expand_sparse_chunk(&b_chunk, bw, bc);

				__sm_words_andnot(rw, aw, bw);
				for (i = 0; i < (int)SM_FLAGS_PER_INDEX;
				     i++) {
					if (ac[i]) {
						if (!bc[i])
							rw[i] = aw
							    [i]; /* b has no cap: keep a unchanged */
						rc[i] = 1;
					} else {
						rw[i] = 0;
						rc[i] = 0;
					}
				}

				if (__sm_encode_sparse_chunk(rw, rc, &desc,
				        vecs, &nvecs)) {
					if (!__sm_append_sparse_chunk(&result,
					        a_start, desc, vecs, nvecs)) {
						sm_free(result);
						return (NULL);
					}
				}
				a_cursor =
				    a_end; /* entire a chunk handled by word-level op */
			}

			/* Advance b if it ends within or at a's boundary */
			if (b_end <= a_end) {
				bp += SM_SIZEOF_OVERHEAD + b_size;
				bi++;
			}
			/* If a ends within b, we're done with this a chunk */
			if (a_end <= b_end)
				break;
		}

		/* Emit remaining a bits [a_cursor, a_end) that had no b overlap */
		if (a_cursor < a_end) {
			if (!__sm_emit_chunk_bits(&result, &a_chunk,
			        a_start, a_cursor, a_end)) {
				sm_free(result);
				return (NULL);
			}
		}

		/* Restore b pointer: next a chunk may overlap with same b chunks.
       But we only need b chunks that haven't been fully passed yet.
       Keep bi/bp at the furthest b that still overlaps or is ahead. */
		(void)bp_save;
		(void)bi_save;

		ap += SM_SIZEOF_OVERHEAD + a_size;
		ai++;
	}

	if (__sm_get_chunk_count(result) == 0) {
		sm_free(result);
		return (NULL);
	}

	__sm_try_demote(result);
	return (result);
}

/**
 * @brief Create a new sparsemap containing the union of a and b.
 *
 * Uses a two-pointer chunk merge walk for O(chunks) performance instead
 * of the previous O(cardinality x chunks) in-place mutation.  Cursors
 * track partially-consumed chunks when one chunk extends past the other.
 *
 * Fast paths:
 *   - Aligned sparse chunks: word-level OR via expand/encode helpers.
 *   - Non-overlapping chunks: copied straight through.
 *
 * @param[in] a  First input sparsemap.
 * @param[in] b  Second input sparsemap.
 * @returns A newly allocated sparsemap (caller must free()), or NULL on
 *          allocation failure or if both inputs are empty/NULL.
 */
sm_t *
sm_union(const sm_t *a, const sm_t *b)
{
	size_t a_count;
	size_t b_count;
	size_t cap;
	sm_t * result;
	uint8_t * ap;
	uint8_t * bp;
	size_t ai;
	size_t bi;
	size_t a_cursor;
	size_t b_cursor;
	__sm_check_invariants(a);
	__sm_check_invariants(b);
	if (a == NULL && b == NULL) {
		return (NULL);
	}
	if (__sm_is_small(a) || __sm_is_small(b)) {
		bool oa, ob;
		const sm_t *va = __sm_chunk_view(a, &oa);
		const sm_t *vb = __sm_chunk_view(b, &ob);
		sm_t *r = ((a != NULL && va == NULL) ||
		              (b != NULL && vb == NULL))
		    ? NULL
		    : sm_union(va, vb);
		if (oa) sm_free((sm_t *)va);
		if (ob) sm_free((sm_t *)vb);
		return (r);
	}

	a_count = a ? __sm_get_chunk_count(a) : 0;
	b_count = b ? __sm_get_chunk_count(b) : 0;

	if (a_count == 0 && b_count == 0) {
		return (NULL);
	}
	if (a_count == 0) {
		return (sm_copy(b));
	}
	if (b_count == 0) {
		return (sm_copy(a));
	}

	/* Allocate result with combined data size (worst case: no overlap). */
	cap = a->m_data_used + b->m_data_used;
	if (cap < 1024)
		cap = 1024;

	result = sparsemap(cap);
	if (result == NULL) {
		return (NULL);
	}

	ap = __sm_get_chunk_data(a, 0);
	bp = __sm_get_chunk_data(b, 0);
	ai = 0; bi = 0;

	/* Cursors track how far into each current chunk we've already emitted.
     A value of 0 means "fresh chunk" (reset after advancing).  When a
     chunk is partially consumed, the cursor holds the absolute bit
     position up to which bits have been emitted. */
	a_cursor = 0;
	b_cursor = 0;

	while (ai < a_count && bi < b_count) {
		size_t a_size;
		size_t a_end;
		__sm_idx_t b_start;
		__sm_chunk_t b_chunk;
		size_t b_size;
		size_t b_end;
		size_t ov_start;
		size_t ov_end;
		/* ---- Read chunk a metadata ---- */
		const __sm_idx_t a_start = __sm_load_idx((const uint8_t *)ap);
		__sm_chunk_t a_chunk;
		__sm_chunk_init(&a_chunk, ap + SM_SIZEOF_OVERHEAD);
		a_size = __sm_chunk_get_size(&a_chunk);
		a_end = (size_t)__sm_chunk_window_end(a_start);

		/* Ensure cursor is at least at chunk start. */
		if (a_cursor < (size_t)a_start)
			a_cursor = (size_t)a_start;

		/* ---- Read chunk b metadata ---- */
		b_start = __sm_load_idx((const uint8_t *)bp);
		__sm_chunk_init(&b_chunk, bp + SM_SIZEOF_OVERHEAD);
		b_size = __sm_chunk_get_size(&b_chunk);
		b_end = (size_t)__sm_chunk_window_end(b_start);

		if (b_cursor < (size_t)b_start)
			b_cursor = (size_t)b_start;

		/* Prefetch next chunks for the merge loop. */
		if (ai + 1 < a_count)
			SM_PREFETCH(ap + SM_SIZEOF_OVERHEAD + a_size);
		if (bi + 1 < b_count)
			SM_PREFETCH(bp + SM_SIZEOF_OVERHEAD + b_size);

		/* ---- No overlap: a's remaining range ends before b's ---- */
		if (a_end <= b_cursor) {
			if (a_cursor == (size_t)a_start) {
				if (!__sm_copy_chunk_to_result(&result, ap))
					goto fail;
			} else {
				if (!__sm_emit_chunk_bits(&result, &a_chunk,
				        a_start, a_cursor, a_end))
					goto fail;
			}
			ap += SM_SIZEOF_OVERHEAD + a_size;
			ai++;
			a_cursor = 0;
			continue;
		}

		/* ---- No overlap: b's remaining range ends before a's ---- */
		if (b_end <= a_cursor) {
			if (b_cursor == (size_t)b_start) {
				if (!__sm_copy_chunk_to_result(&result, bp))
					goto fail;
			} else {
				if (!__sm_emit_chunk_bits(&result, &b_chunk,
				        b_start, b_cursor, b_end))
					goto fail;
			}
			bp += SM_SIZEOF_OVERHEAD + b_size;
			bi++;
			b_cursor = 0;
			continue;
		}

		/* ---- Chunks overlap.  Compute overlap bounds. ---- */
		ov_start = a_cursor > b_cursor ? a_cursor : b_cursor;
		ov_end = a_end < b_end ? a_end : b_end;
		(void)ov_start;
		(void)ov_end; /* only the aligned-sparse fast path runs here */

		/* ---- Fast path: both sparse, aligned ---- */
		/* When aligned, handle the full chunk with per-cursor masking.
       This avoids creating separate pre-overlap chunks at the same start.
       Every chunk is sparse and two overlapping chunks share the same
       aligned start, so this path always applies. */
		if (a_start == b_start) {
			__sm_bitvec_t rw[SM_FLAGS_PER_INDEX];
			int rc[SM_FLAGS_PER_INDEX];
			__sm_bitvec_t desc;
			__sm_bitvec_t vecs[SM_FLAGS_PER_INDEX];
			int nvecs;
			int i;
			__sm_bitvec_t aw[SM_FLAGS_PER_INDEX],
			    bw[SM_FLAGS_PER_INDEX];
			int ac[SM_FLAGS_PER_INDEX], bc[SM_FLAGS_PER_INDEX];
			__sm_expand_sparse_chunk(&a_chunk, aw, ac);
			__sm_expand_sparse_chunk(&b_chunk, bw, bc);

			/* Mask a's words before a_cursor */
			if (a_cursor > (size_t)a_start) {
				size_t sb;
				int i_s;
				const size_t rel = a_cursor - (size_t)a_start;
				const int sw = (int)(rel / SM_BITS_PER_VECTOR);
				for (i_s = 0;
				     i_s < sw && i_s < (int)SM_FLAGS_PER_INDEX;
				     i_s++) {
					aw[i_s] = 0;
					ac[i_s] = 0;
				}
				sb = rel % SM_BITS_PER_VECTOR;
				if (sb > 0 && sw < (int)SM_FLAGS_PER_INDEX) {
					aw[sw] &= ~((__sm_bitvec_t)0) << sb;
				}
			}

			/* Mask b's words before b_cursor */
			if (b_cursor > (size_t)b_start) {
				size_t sb;
				int i_s;
				const size_t rel = b_cursor - (size_t)b_start;
				const int sw = (int)(rel / SM_BITS_PER_VECTOR);
				for (i_s = 0;
				     i_s < sw && i_s < (int)SM_FLAGS_PER_INDEX;
				     i_s++) {
					bw[i_s] = 0;
					bc[i_s] = 0;
				}
				sb = rel % SM_BITS_PER_VECTOR;
				if (sb > 0 && sw < (int)SM_FLAGS_PER_INDEX) {
					bw[sw] &= ~((__sm_bitvec_t)0) << sb;
				}
			}

			__sm_words_or(rw, aw, bw);
			for (i = 0; i < (int)SM_FLAGS_PER_INDEX; i++) {
				rc[i] = (ac[i] || bc[i]) ? 1 : 0;
			}

			if (__sm_encode_sparse_chunk(rw, rc, &desc, vecs,
			        &nvecs)) {
				if (!__sm_append_sparse_chunk(&result, a_start,
				        desc, vecs, nvecs))
					goto fail;
			}

			/* Both chunks fully consumed. */
			ap += SM_SIZEOF_OVERHEAD + a_size;
			ai++;
			a_cursor = 0;
			bp += SM_SIZEOF_OVERHEAD + b_size;
			bi++;
			b_cursor = 0;

		} else {
			/*
			 * Every chunk is sparse, and two overlapping sparse
			 * chunks share the same aligned start, so the fast path
			 * above always applies.  This arm (mixed / misaligned)
			 * is unreachable.
			 */
			__sm_assert(false && "union: non-aligned overlap in sparse-only build");
			goto fail;
		}
	}

	/* Copy remaining chunks from whichever map is not exhausted. */
	while (ai < a_count) {
		size_t sz;
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)ap);
		__sm_chunk_t c;
		__sm_chunk_init(&c, ap + SM_SIZEOF_OVERHEAD);
		sz = __sm_chunk_get_size(&c);
		if (a_cursor > 0 && a_cursor > (size_t)start) {
			/* Partially consumed: emit only remaining bits. */
			if (!__sm_emit_chunk_bits(&result, &c, start,
			        a_cursor, (size_t)__sm_chunk_window_end(start)))
				goto fail;
		} else {
			if (!__sm_copy_chunk_to_result(&result, ap))
				goto fail;
		}
		ap += SM_SIZEOF_OVERHEAD + sz;
		ai++;
		a_cursor = 0;
	}
	while (bi < b_count) {
		size_t sz;
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)bp);
		__sm_chunk_t c;
		__sm_chunk_init(&c, bp + SM_SIZEOF_OVERHEAD);
		sz = __sm_chunk_get_size(&c);
		if (b_cursor > 0 && b_cursor > (size_t)start) {
			if (!__sm_emit_chunk_bits(&result, &c, start,
			        b_cursor, (size_t)__sm_chunk_window_end(start)))
				goto fail;
		} else {
			if (!__sm_copy_chunk_to_result(&result, bp))
				goto fail;
		}
		bp += SM_SIZEOF_OVERHEAD + sz;
		bi++;
		b_cursor = 0;
	}

	if (__sm_get_chunk_count(result) == 0) {
		sm_free(result);
		return (NULL);
	}

	__sm_try_demote(result);
	return (result);

fail:
	sm_free(result);
	return (NULL);
}

/* -------------------------------------------------------------------
 * Split, select, rank, and span
 * ------------------------------------------------------------------- */

uint64_t
sm_split(sm_t *map, uint64_t idx, sm_t *other)
{
	size_t i;
	size_t count;
	bool in_middle;
	uint8_t * src;
	uint8_t * dst;
	size_t split_offset;
	size_t chunks_to_move;
	uint8_t * map_end;
	size_t j;
	if (map == NULL || other == NULL) {
		errno = EINVAL;
		return (SM_IDX_MAX);
	}
	__sm_card_invalidate(map);
	__sm_card_invalidate(other);
	/* Split walks raw chunks; give it a chunk-mode map and an empty
	 * chunk-mode destination.  Promote in place (within capacity) if
	 * `map` is small; the destination is cleared to chunk-empty. */
	if (__sm_is_small(map)) {
		if (!__sm_promote(map)) {
			errno = ENOSPC;
			return (SM_IDX_MAX);
		}
	}
	if (__sm_is_small(other)) {
		sm_clear(other);
	}
	__sm_check_invariants(map);
	__sm_check_invariants(other);
	count = __sm_get_chunk_count(map);
	in_middle = false;

	__sm_assert(sm_rank(other, 0, SM_IDX_MAX, true) == 0);

	/*
	 * According to the API when idx is SM_IDX_MAX the client is
	 * requesting that we divide the bits in two equal portions, so we
	 * calculate that index here.
	 */
	if (idx == SM_IDX_MAX) {
		const uint64_t begin = sm_minimum(map);
		const uint64_t end = sm_maximum(map);
		if (begin != end) {
			const size_t rank = sm_rank(map, begin, end, true);
			idx = sm_select(map, rank / 2, true);
		} else {
			return (SM_IDX_MAX);
		}
	}

	/* Is the index beyond the last bit set in the source? */
	if (idx > sm_maximum(map)) {
		return (idx);
	}

	/*
	 * Here's how this is going to work, there are three phases.
	 * 1) Skip over any chunks before the idx.
	 * 2) If the idx falls within a chunk, split that sparse chunk.
	 * 3) Keep half in the src and insert the other half into the dst
	 * 4) Move any remaining chunks to dst.
	 */
	src = __sm_get_chunk_data(map, 0);
	dst = __sm_get_chunk_end(other);

	/* (1): skip over chunks that are entirely to the left. */
	for (i = 0; i < count; i++) {
		__sm_chunk_t chunk;
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)src);
		if (start == idx) {
			break;
		}
		__sm_chunk_init(&chunk, src + SM_SIZEOF_OVERHEAD);
		if (start <= idx && idx - start < __sm_chunk_get_capacity(&chunk)) {
			in_middle = true;
			break;
		}
		if (start > idx) {
			/* This chunk begins past idx and the loop already
			 * advanced `src` to it, so `src` and `i` mark where the
			 * moved region starts.  Rolling back to the previous
			 * chunk here moved the last chunk entirely below idx
			 * into `other` whenever idx fell in a gap between two
			 * chunks. */
			break;
		}

		src += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
	}

	/* (2): The idx falls within a chunk then it has to be split. */
	if (in_middle) {
		__sm_idx_t src_start;
		uint8_t buf[SM_SIZEOF_OVERHEAD +
		    (sizeof(__sm_bitvec_t) * 2)];
		size_t j_s;
		size_t mid_off;
		__sm_chunk_t d_chunk;
		__sm_chunk_init(&d_chunk, dst + SM_SIZEOF_OVERHEAD);
		src_start = __sm_load_idx((const uint8_t *)src);

		/*
		 * (2) We're in the middle of a sparse chunk, let's split it.
		 */

		/* The destination is caller-provided and may be too small for
		 * even the single chunk this phase splits off (the review's
		 * mutating harness always passed a large `other`, but a small
		 * one wrote past its buffer here -- an ASan heap-buffer-
		 * overflow in the memcpy below, before any capacity was
		 * checked).  A split-off sparse chunk occupies at most the
		 * overhead word plus a full descriptor and 32 payload words;
		 * refuse up front with the documented ENOSPC if it will not
		 * fit, leaving both maps untouched. */
		{
			const size_t max_chunk = SM_SIZEOF_OVERHEAD +
			    sizeof(__sm_bitvec_t) *
			        (size_t)(1 + SM_FLAGS_PER_INDEX);
			if (other->m_data_used + max_chunk > __sm_cap(other)) {
				errno = ENOSPC;
				return (SM_IDX_MAX);
			}
		}

		/* Zero out the space we'll need at the proper location in dst. */
		memset(buf, 0, sizeof(buf));
		memcpy(dst, &buf, sizeof(buf));

		/* And add a chunk to the other map. */
		__sm_set_chunk_count(other, __sm_get_chunk_count(other) + 1);
		if (other->m_data_used != 0) {
			other->m_data_used +=
			    SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t);
		}

		/* Copy the bits in the sparse chunk, at most
		 * SM_CHUNK_MAX_CAPACITY.  The unset loop mutates `map`: it
		 * shrinks the middle chunk and, if the chunk empties, removes
		 * it and shifts every later chunk down.  So where the moved
		 * region begins is re-derived after the loop from the current
		 * bytes.  Chunks before the middle one are untouched, so its
		 * m_data offset `mid_off` is stable across the mutation. */
		mid_off = (size_t)(src - map->m_data);
		__sm_store_idx((uint8_t *)dst, src_start);
		for (j_s = idx; j_s - src_start < SM_CHUNK_MAX_CAPACITY;
		     j_s++) {
			if (sm_contains(map, j_s, NULL)) {
				__sm_map_set(other, j_s, false, NULL);
				__sm_map_unset(map, j_s, false);
			}
		}
		/* The middle chunk had no member at or above idx: drop the
		 * empty chunk the zeroed header created in `other`.  It held
		 * no bits, but sm_minimum and other slot-indexed readers saw
		 * its start. */
		__sm_chunk_init(&d_chunk, dst + SM_SIZEOF_OVERHEAD);
		if (__sm_chunk_is_empty(&d_chunk)) {
			other->m_data_used = (size_t)(dst - other->m_data);
			__sm_set_chunk_count(other,
			    __sm_get_chunk_count(other) - 1);
		} else {
			dst +=
			    SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&d_chunk);
		}
		i++;

		/* If a chunk still lives at mid_off and it is the middle
		 * chunk (start == src_start), it kept bits [src_start, idx)
		 * and the move starts just past it; otherwise the middle
		 * chunk was removed and the move starts at mid_off. */
		src = map->m_data + mid_off;
		if (mid_off + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) <=
		        map->m_data_used &&
		    __sm_load_idx((const uint8_t *)src) == src_start) {
			__sm_chunk_t mid;
			__sm_chunk_init(&mid, src + SM_SIZEOF_OVERHEAD);
			src += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&mid);
		}
	}

	/* Now continue with all remaining chunks. */
	/* Save the offset where moved chunks start, so we can truncate map later */
	split_offset = (size_t)(src - map->m_data);
	/* Upper bound on chunks to move: the middle phase can have removed
	 * the middle chunk, so `count - i` is unreliable.  The probe below
	 * re-derives the exact movable count by walking the bytes from
	 * `src` to the data end; the current chunk count never undercounts. */
	chunks_to_move = __sm_get_chunk_count(map);
	/* The chunk stream ends here; the move must never read past it.  On
	 * a valid-but-adversarial map the chunk-skip and sparse-split
	 * phases above can leave `i` disagreeing with the bytes actually
	 * present, so `count - i` may claim more chunks than remain -- a
	 * source-side over-read in __sm_append_data (ASan heap-buffer-
	 * overflow READ, then heap corruption on reuse).  Bounding every
	 * walk by map_end keeps the move within the source buffer. */
	map_end = map->m_data + map->m_data_used;

	/*
	 * The destination is caller-provided and may be far smaller than
	 * what we are about to move into it.  __sm_append_data does no
	 * bounds check, so without this the moved chunks ran off the end of
	 * `other`'s buffer (ASan heap-buffer-overflow in memcpy via
	 * __sm_append_data; glibc reported it later as "realloc(): invalid
	 * next size" once the corrupted heap was reused).  The documented
	 * contract is SM_IDX_MAX with errno=ENOSPC when the buffer is too
	 * small, so total the bytes first and refuse up front, leaving both
	 * maps untouched.  The same pass also re-derives how many chunks
	 * actually fit in the source before its data end, so a desynced
	 * count can never drive the move loop past the buffer.
	 */
	{
		size_t j_s;
		uint8_t *probe = src;
		size_t need = 0;
		size_t movable = 0;
		for (j_s = 0; j_s < chunks_to_move; j_s++) {
			__sm_chunk_t c;
			size_t sz;
			if (probe + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t) >
			    map_end) {
				break;
			}
			__sm_chunk_init(&c, probe + SM_SIZEOF_OVERHEAD);
			sz = SM_SIZEOF_OVERHEAD +
			    __sm_chunk_get_size(&c);
			if (probe + sz > map_end) {
				break;
			}
			need += sz;
			probe += sz;
			movable++;
		}
		chunks_to_move = movable;
		if (other->m_data_used + need > __sm_cap(other)) {
			errno = ENOSPC;
			return (SM_IDX_MAX);
		}
	}

	for (j = 0; j < chunks_to_move; j++) {
		size_t chunk_size;
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, src + SM_SIZEOF_OVERHEAD);
		chunk_size = SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);

		/* Copy chunk to other.  The total was reserved before the
		 * loop started, so this cannot fail; if it ever did, the
		 * move would already be half-applied, so treat it as
		 * unreachable rather than pretending it can be unwound. */
		if (SM_UNLIKELY(!__sm_append_data(other, src, chunk_size))) {
			__sm_assert(!"sm_split: capacity check disagreed with "
			             "the move loop");
			errno = ENOSPC;
			return (SM_IDX_MAX);
		}
		__sm_set_chunk_count(other, __sm_get_chunk_count(other) + 1);

		src += chunk_size;
	}

	/* Update chunk counts and force recalculation of data sizes */
	__sm_set_chunk_count(map, __sm_get_chunk_count(map) - chunks_to_move);
	map->m_data_used = split_offset;

	__sm_assert(sm_get_size(map) >= SM_SIZEOF_OVERHEAD);
	__sm_assert(sm_get_size(other) > SM_SIZEOF_OVERHEAD);

	__sm_coalesce_map(map);
	__sm_coalesce_map(other);

	__sm_try_demote(map);
	__sm_try_demote(other);
	return (idx);
}

uint64_t
sm_select(sm_t *map, uint64_t n, bool value)
{
	size_t count;
	uint8_t * p;
	size_t i;
	uint64_t pos = 0;    /* first position not yet counted */
	bool at_end = false; /* a chunk reached 2^64 - 1 */
	if (map == NULL) {
		/* Empty map: no set bits; unset bits are the whole line, so
		 * the n-th unset bit is n.  Matches the count == 0 path. */
		return (value ? SM_IDX_MAX : n);
	}
	if (__sm_is_small(map)) {
		return (__sm_small_select(map, n, value));
	}
	__sm_check_invariants(map);
	__sm_assert(sm_get_size(map) >= SM_SIZEOF_OVERHEAD);
	count = __sm_get_chunk_count(map);

	if (count == 0) {
		return (value ? SM_IDX_MAX : n);
	}

	p = __sm_get_chunk_data(map, 0);

	/*
	 * Walk the chunks, counting the matching positions of each one's
	 * window and of the gap before it.  A chunk's window is its full
	 * 2048 bits, NONE slots included (they are unset), and the next
	 * start is at least 2048 further on.  Unset bits also fill every
	 * gap between windows and the rest of the universe past the last
	 * one.  A whole chunk is skipped by its popcount, so
	 * __sm_chunk_select only runs on the chunk that holds the answer,
	 * with an n below 2048 (n is 64-bit; its ssize_t parameter is not,
	 * on ILP32).
	 */
	for (i = 0; i < count && !at_end; i++) {
		__sm_chunk_t chunk;
		size_t set;
		size_t matching;
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);

		p += SM_SIZEOF_OVERHEAD;
		__sm_chunk_init(&chunk, p);

		if (value == false && start > pos) {
			const uint64_t gap = start - pos;
			if (n < gap) {
				return (pos + n);
			}
			n -= gap;
		}

		set = __sm_chunk_popcount(&chunk);
		if (value) {
			matching = set;
		} else {
			matching = set >= SM_CHUNK_MAX_CAPACITY ?
			    0 :
			    (size_t)SM_CHUNK_MAX_CAPACITY - set;
		}
		if (n < (uint64_t)matching) {
			ssize_t new_n = (ssize_t)n;
			const size_t index = __sm_chunk_select(&chunk,
			    (ssize_t)n, &new_n, value);
			__sm_assert(new_n == -1);
			if (new_n != -1) {
				return (SM_IDX_MAX);
			}
			return (start + index);
		}
		n -= (uint64_t)matching;

		if (UINT64_MAX - start < (uint64_t)SM_CHUNK_MAX_CAPACITY) {
			at_end = true;
		} else {
			pos = start + SM_CHUNK_MAX_CAPACITY;
		}
		p += __sm_chunk_get_size(&chunk);
	}
	/* Past the last window every bit is unset.  Position SM_IDX_MAX
	 * itself is the "not found" sentinel, not an answer. */
	if (value || at_end || n >= SM_IDX_MAX - pos) {
		return (SM_IDX_MAX);
	}
	return (pos + n);
}

static size_t
__sm_rank_vec(sm_t *map, uint64_t begin, uint64_t end, bool value,
    __sm_bitvec_t *vec)
{
	uint64_t span_width;
	size_t width;
	size_t count;
	size_t set;
	uint8_t * p;
	size_t i;
	(void)vec; /* retained for ABI/signature compatibility */
	__sm_assert(sm_get_size(map) >= SM_SIZEOF_OVERHEAD);

	if (begin > end) {
		return (0);
	}

	/*
	 * Range width as a count.  When [begin, end] spans the entire
	 * 64-bit universe (begin == 0, end == UINT64_MAX) the +1
	 * overflows to 0; size_t saturates instead so the derived unset
	 * count below stays meaningful.  A full-universe unset query is
	 * degenerate (the answer is ~2^64) but must not wrap.
	 */
	span_width = end - begin;
	width = (span_width == UINT64_MAX) ? SIZE_MAX : (size_t)(span_width + 1);

	if (__sm_is_small(map)) {
		set = __sm_small_rank1(map, begin, end);
		return (value ? set : width - set);
	}

	/*
	 * Rank is computed from the set-bit count only.  A bit is set
	 * iff some chunk covers it, so the number of set bits in the
	 * inclusive range [begin, end] is the sum, over every chunk, of
	 * the matching bits in the overlap of [begin, end] with that
	 * chunk's covered span [start, start + capacity).  The unset
	 * count is then width - set; there is no cross-chunk gap
	 * bookkeeping to get wrong.
	 *
	 * __sm_chunk_rank does the per-chunk work (it takes from/to
	 * positions relative to the chunk start and is validated by the
	 * get_position property tests).  We only ever ask it for
	 * set bits here; unset is derived once at the end.
	 */
	count = __sm_get_chunk_count(map);
	if (count == 0) {
		return (value ? 0 : width);
	}

	set = 0;
	p = __sm_get_chunk_data(map, 0);
	for (i = 0; i < count; i++) {
		__sm_chunk_t chunk;
		size_t chunk_size;
		size_t cap;
		uint64_t chunk_lo;
		uint64_t span;
		uint64_t chunk_hi_incl;
		uint64_t ov_lo;
		uint64_t ov_hi_incl;
		size_t from;
		size_t to;
		__sm_chunk_rank_t rank;
		const __sm_idx_t start = __sm_load_idx((const uint8_t *)p);
		p += SM_SIZEOF_OVERHEAD;
		__sm_chunk_init(&chunk, p);
		chunk_size = __sm_chunk_get_size(&chunk);
		if (i + 1 < count) {
			SM_PREFETCH(p + chunk_size + SM_SIZEOF_OVERHEAD);
		}
		cap = __sm_chunk_get_capacity(&chunk);
		chunk_lo = start;
		/* Inclusive top of the chunk's covered span.  cap >= 1, and we
		 * form (cap - 1) as a distance so the comparison below never
		 * overflows even when chunk_lo is near UINT64_MAX (the
		 * top-of-universe case). */
		span = (uint64_t)cap - 1;
		chunk_hi_incl = (chunk_lo > UINT64_MAX - span) ? UINT64_MAX
		                                  : chunk_lo + span;

		/* Chunks are ordered ascending.  Once a chunk starts past
		 * `end` no later chunk can overlap [begin, end]. */
		if (chunk_lo > end) {
			p += chunk_size;
			break;
		}
		/* Skip chunks entirely below `begin`. */
		if (chunk_hi_incl < begin) {
			p += chunk_size;
			continue;
		}

		/* Overlap of [begin, end] with [chunk_lo, chunk_hi_incl]. */
		ov_lo = begin > chunk_lo ? begin : chunk_lo;
		ov_hi_incl = (end < chunk_hi_incl) ? end : chunk_hi_incl;
		/* Positions relative to the chunk start. */
		from = (size_t)(ov_lo - chunk_lo);
		to = (size_t)(ov_hi_incl - chunk_lo);

		set += __sm_chunk_rank(&rank, true, &chunk, from, to);
		p += chunk_size;
	}

	if (value) {
		return (set);
	}
	__sm_assert((uint64_t)set <= width);
	return ((size_t)(width - set));
}

size_t
sm_rank(sm_t *map, uint64_t begin, uint64_t end, bool value)
{
	__sm_bitvec_t vec;
	if (map == NULL)
		return (0);
	__sm_check_invariants(map);
	return (__sm_rank_vec(map, begin, end, value, &vec));
}

uint64_t
sm_span(sm_t *map, uint64_t idx, size_t len, bool value)
{
	__sm_bitvec_t vec;
	size_t nth;
	uint64_t offset;
	if (map == NULL)
		return (SM_IDX_MAX);
	if (__sm_is_small(map)) {
		return (__sm_small_span(map, idx, len, value));
	}
	__sm_check_invariants(map);
	vec = 0;

	/* When skipping forward to `idx` offset in the map we can determine how
	 * many selects we can avoid by taking the rank of the range and starting
	 * at that bit. */
	nth = (idx == 0) ? 0 : sm_rank(map, 0, idx - 1, value);
	/* Find the first bit that matches value, then... */
	offset = sm_select(map, nth, value);
	do {
		size_t rank;
		int amt;
		/* A span of len bits from here would pass 2^64, and the start
		 * only moves right.  Without this, an unset span longer than
		 * the rest of the universe stepped through every unset bit. */
		if (len > 0 && (uint64_t)len - 1 > UINT64_MAX - offset) {
			return (SM_IDX_MAX);
		}
		/* See if the rank of the bits in the range starting at offset is equal
		 * to the desired amount. */
		rank = (len == 1) ?
		    1 :
		    __sm_rank_vec(map, offset, offset + len - 1, value, &vec);
		if (rank >= len) {
			/* We've found what we're looking for, return the index of the first
			 * bit in the range. */
			break;
		}
		/* Now we try to jump forward as much as possible before we look for a
		 * new match. We do this by counting the remaining bits in the returned
		 * vec from the call to rank_vec(). */
		amt = 1;
		if (vec > 0) {
			/* The returned vec had some set bits, let's move forward in the map as
			 * much as possible (max: 64 bit positions). */
			const int max = (int)(len > SM_BITS_PER_VECTOR ?
				SM_BITS_PER_VECTOR :
				len);
			while (amt < max && (vec & (UINT64_C(1) << amt))) {
				amt++;
			}
		}
		nth += (size_t)amt;
		offset = sm_select(map, nth, value);
	} while (SM_FOUND(offset));

	return (offset);
}

/* -------------------------------------------------------------------
 * Lazy iteration over a OP b: members of a combined set without
 * building it
 * ------------------------------------------------------------------- */

/*
 * The first index >= x that is NOT a member of the chunk at `chunk`
 * (starting at `start`), or start + capacity (saturated, see __sm_end)
 * when every bit from x to the chunk's end is set.  x must lie in
 * [start, start + capacity).  NONE slots are absent, like every reader
 * treats them.
 */
static uint64_t
__sm_chunk_next_absent(const __sm_chunk_t *chunk, uint64_t start,
    size_t cap, uint64_t x)
{
	size_t v;
	for (v = (size_t)((x - start) / SM_BITS_PER_VECTOR);
	     v < SM_FLAGS_PER_INDEX; v++) {
		const uint64_t vec_lo = start + v * SM_BITS_PER_VECTOR;
		const size_t flags = SM_CHUNK_GET_FLAGS(chunk->m_data[0], v);
		const unsigned skip =
		    (x > vec_lo) ? (unsigned)(x - vec_lo) : 0u;
		__sm_bitvec_t zeros;
		if (flags == SM_PAYLOAD_ONES) {
			continue;
		}
		if (flags != SM_PAYLOAD_MIXED) {
			return (vec_lo + skip);
		}
		zeros = ~(__sm_bitvec_t)chunk->m_data[1 +
		    __sm_chunk_get_position(chunk, v)] &
		    (~(__sm_bitvec_t)0 << skip);
		if (zeros != 0) {
			return (vec_lo + (uint64_t)SM_CTZ64(zeros));
		}
	}
	return (__sm_end(start, cap));
}

/*
 * The first index >= x that is NOT a member of `map`, or SM_IDX_MAX if
 * every index from x to the last member index, 2^64 - 2, is a member.
 * The walk locates x's chunk through __sm_get_chunk_offset (so `cur`
 * accelerates ascending calls exactly as it does for sm_contains), then
 * follows the run of set bits across abutting chunks.  Cost is
 * O(chunks walked), never one step per bit: an all-ONES chunk is one
 * step for its 2048 bits.
 */
static uint64_t
__sm_next_absent(const sm_t *map, uint64_t x, sm_cursor_t *cur)
{
	uint8_t *base;
	uint8_t *p;
	size_t stream_end;
	ssize_t off;
	if (map == NULL) {
		return (x);
	}
	if (__sm_is_small(map)) {
		return (__sm_small_next(map, x, false));
	}
	off = __sm_get_chunk_offset(map, x, cur);
	if (off < 0) {
		return (x);
	}
	base = __sm_get_chunk_data(map, 0);
	p = base + off;
	stream_end = map->m_data_used - SM_SIZEOF_OVERHEAD;
	for (;;) {
		const uint64_t start = __sm_load_idx((const uint8_t *)p);
		__sm_chunk_t chunk;
		size_t cap;
		uint64_t end;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		cap = __sm_chunk_get_capacity(&chunk);
		if (x < start || x - start >= cap) {
			return (x); /* in a gap or past the last chunk */
		}
		end = __sm_chunk_next_absent(&chunk, start, cap, x);
		if (end != __sm_end(start, cap)) {
			return (end);
		}
		/* Set through the last bit of the chunk that ends at 2^64. */
		if (end == UINT64_MAX) {
			return (SM_IDX_MAX);
		}
		p += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		if ((size_t)(p - base) >= stream_end) {
			return (end);
		}
		x = end;
	}
}

/* True when an index in exactly the sets flagged by in_a / in_b belongs
 * to a OP b. */
static bool
__sm_op_keep(sm_op_t op, bool in_a, bool in_b)
{
	switch (op) {
	case SM_OP_AND:
		return (in_a && in_b);
	case SM_OP_OR:
		return (in_a || in_b);
	case SM_OP_ANDNOT:
		return (in_a && !in_b);
	case SM_OP_XOR:
		return (in_a != in_b);
	}
	/* An out-of-range op keeps nothing.  The fallback sits after the
	 * switch, not in a default label, so a new enumerator without a
	 * case draws a -Wswitch warning. */
	return (false);
}

/* AND: leapfrog the two member streams.  Each round either returns or
 * moves the lower bound up to the larger candidate, so a run in one map
 * meeting a sparse other map costs one step per member of the other. */
static uint64_t
__sm_next_and(const sm_t *a, const sm_t *b, uint64_t p, sm_cursor_t *ca,
    sm_cursor_t *cb)
{
	uint64_t x = sm_next_member(a, p, ca);
	for (;;) {
		uint64_t y;
		if (x == SM_IDX_MAX) {
			return (SM_IDX_MAX);
		}
		/* x - 1 wraps to the "from the start" sentinel for x == 0. */
		y = sm_next_member(b, x - 1, cb);
		if (y == x || y == SM_IDX_MAX) {
			return (y);
		}
		x = sm_next_member(a, y - 1, ca);
		if (x == y) {
			return (x);
		}
	}
}

/* ANDNOT: the next member of a that b lacks.  When b holds a's
 * candidate, jump past b's whole run of set bits at once. */
static uint64_t
__sm_next_andnot(const sm_t *a, const sm_t *b, uint64_t p, sm_cursor_t *ca,
    sm_cursor_t *cb)
{
	uint64_t x = sm_next_member(a, p, ca);
	while (x != SM_IDX_MAX && sm_contains(b, x, cb)) {
		const uint64_t gap = __sm_next_absent(b, x, cb);
		if (gap == SM_IDX_MAX) {
			return (SM_IDX_MAX);
		}
		x = sm_next_member(a, gap - 1, ca);
	}
	return (x);
}

/* XOR: the smaller of the two next members, unless both maps hold it.
 * Then both are set from x up to the first index where either map has
 * a hole; skip that common stretch in one step and look again. */
static uint64_t
__sm_next_xor(const sm_t *a, const sm_t *b, uint64_t p, sm_cursor_t *ca,
    sm_cursor_t *cb)
{
	for (;;) {
		const uint64_t x = sm_next_member(a, p, ca);
		const uint64_t y = sm_next_member(b, p, cb);
		uint64_t ea;
		uint64_t eb;
		uint64_t e;
		if (x != y) {
			return (x < y ? x : y);
		}
		if (x == SM_IDX_MAX) {
			return (SM_IDX_MAX);
		}
		ea = __sm_next_absent(a, x, ca);
		eb = __sm_next_absent(b, x, cb);
		if (ea == SM_IDX_MAX && eb == SM_IDX_MAX) {
			return (SM_IDX_MAX);
		}
		e = ea < eb ? ea : eb;
		if (ea != eb) {
			/* e is a member of exactly one of the two maps. */
			return (e);
		}
		p = e - 1;
	}
}

uint64_t
sm_next_member_op(const sm_t *a, const sm_t *b, sm_op_t op, uint64_t prev_idx,
    sm_cursor_t *cur_a, sm_cursor_t *cur_b)
{
	/* UINT64_MAX is both "from the start" and the largest index, so no
	 * member is strictly greater than a real UINT64_MAX bound; the
	 * sentinel meaning wins, exactly as in sm_next_member. */
	switch (op) {
	case SM_OP_AND:
		if (a == NULL || b == NULL) {
			return (SM_IDX_MAX);
		}
		return (__sm_next_and(a, b, prev_idx, cur_a, cur_b));
	case SM_OP_OR: {
		const uint64_t x = sm_next_member(a, prev_idx, cur_a);
		const uint64_t y = sm_next_member(b, prev_idx, cur_b);
		return (x < y ? x : y);
	}
	case SM_OP_ANDNOT:
		return (__sm_next_andnot(a, b, prev_idx, cur_a, cur_b));
	case SM_OP_XOR:
		return (__sm_next_xor(a, b, prev_idx, cur_a, cur_b));
	}
	/* An out-of-range op (after the switch so -Wswitch still checks
	 * the enum is covered). */
	errno = EINVAL;
	return (SM_IDX_MAX);
}

/*
 * Batched delivery for sm_scan_op: members are copied into a 64-entry
 * buffer and handed to the caller's function whenever it fills, and once
 * more at the end.  `skip` counts result members still to drop.
 */
typedef struct {
	void (*fn)(uint64_t[], size_t, void *);
	void *aux;
	size_t skip;
	size_t n;
	uint64_t buf[SM_BITS_PER_VECTOR];
} __sm_scan_out_t;

/* Buffer one result member, flushing a full batch to the caller. */
static void
__sm_scan_out_index(__sm_scan_out_t *o, uint64_t idx)
{
	if (o->skip > 0) {
		o->skip--;
		return;
	}
	o->buf[o->n++] = idx;
	if (o->n == SM_BITS_PER_VECTOR) {
		o->fn(o->buf, o->n, o->aux);
		o->n = 0;
	}
}

/* Deliver every index in [lo, hi), less any still to be skipped.  The
 * skip is taken arithmetically, so a long skipped run costs nothing. */
static void
__sm_scan_out_run(__sm_scan_out_t *o, uint64_t lo, uint64_t hi)
{
	uint64_t i;
	if (o->skip > 0) {
		if ((uint64_t)o->skip >= hi - lo) {
			o->skip -= (size_t)(hi - lo);
			return;
		}
		lo += o->skip;
		o->skip = 0;
	}
	for (i = lo; i < hi; i++) {
		__sm_scan_out_index(o, i);
	}
}

void
sm_scan_op(const sm_t *a, const sm_t *b, sm_op_t op,
    void (*fn)(uint64_t[], size_t, void *), size_t skip, void *aux)
{
	__sm_run_iter_t *ia;
	__sm_run_iter_t *ib;
	__sm_scan_out_t out;
	uint64_t alo = 0, ahi = 0, blo = 0, bhi = 0;
	bool have_a, have_b;
	uint64_t pos = 0;
	bool have_pos = false;
	if (fn == NULL || op > SM_OP_XOR) {
		errno = EINVAL;
		return;
	}
	/* Two run iterators are about 33 KB; keep them off the stack. */
	ia = (__sm_run_iter_t *)__sm_alloc(sizeof(*ia));
	ib = (__sm_run_iter_t *)__sm_alloc(sizeof(*ib));
	if (ia == NULL || ib == NULL) {
		__sm_free(ia);
		__sm_free(ib);
		errno = ENOMEM;
		return;
	}
	out.fn = fn;
	out.aux = aux;
	out.skip = skip;
	out.n = 0;

	/*
	 * The interval sweep sm_xor uses: walk both maps run by run, split
	 * the line at every run edge, and keep each segment [pos, next) whose
	 * membership (in a, in b) satisfies op.  Run ends are exclusive and
	 * never wrap: the last member is at most 2^64 - 2.
	 */
	__sm_run_iter_init(ia, a);
	__sm_run_iter_init(ib, b);
	have_a = __sm_run_next(ia, &alo, &ahi);
	have_b = __sm_run_next(ib, &blo, &bhi);
	while (have_a || have_b) {
		uint64_t lo = have_a ? alo : blo;
		uint64_t next = UINT64_MAX;
		bool in_a;
		bool in_b;
		if (have_b && blo < lo)
			lo = blo;
		if (!have_pos || pos < lo) {
			pos = lo;
			have_pos = true;
		}
		in_a = have_a && pos >= alo && pos < ahi;
		in_b = have_b && pos >= blo && pos < bhi;
		if (have_a) {
			if (pos < alo && alo < next)
				next = alo;
			if (pos >= alo && ahi < next)
				next = ahi;
		}
		if (have_b) {
			if (pos < blo && blo < next)
				next = blo;
			if (pos >= blo && bhi < next)
				next = bhi;
		}
		if (__sm_op_keep(op, in_a, in_b)) {
			__sm_scan_out_run(&out, pos, next);
		}
		pos = next;
		if (have_a && pos >= ahi)
			have_a = __sm_run_next(ia, &alo, &ahi);
		if (have_b && pos >= bhi)
			have_b = __sm_run_next(ib, &blo, &bhi);
	}
	if (out.n > 0) {
		fn(out.buf, out.n, aux);
	}
	__sm_free(ia);
	__sm_free(ib);
}

/* -------------------------------------------------------------------
 * Frozen maps: a read-only Elias-Fano copy for very sparse sets
 * ------------------------------------------------------------------- */

/*
 * Classic Elias-Fano over the n sorted members x[0] < ... < x[n-1].
 * With u = x[n-1] + 1 and L = floor(log2(u / n)) (0 when u < 2n, at most
 * 63), each member splits into a low part x & (2^L - 1), stored packed L
 * bits apiece in `low`, and a high part h = x >> L, stored in unary in the
 * bitvector `high`: member i sets bit h[i] + i.  `high` therefore holds n
 * ones and (x[n-1] >> L) + 1 zeros counted up to its last one, about 2n
 * bits.  Space is n * (L + 2) bits plus the select samples, about 2 + L
 * bits per member: the information-theoretic bound for a sorted set.
 *
 * select(k) is the position of the k-th one in `high` minus k, shifted up
 * L bits, or'd with low[k].  The k-th one is found from a sample of the
 * position of every SM_EF_SAMPLE-th one, then a word scan, so select is
 * O(1) when the ones are not spread far apart (O(gap / 64) worst case).
 * Bucket b (all members whose high part is b) begins right after the b-th
 * zero of `high`; zeros are sampled the same way.  rank, contains, next
 * and prev find a bucket with select0 and then search it, so they cost
 * O(1 + bucket size), and a bucket holds about u / 2^L <= 2 members on
 * average.
 *
 * The structure is immutable and owns one allocation; only the public
 * functions below touch it.
 */
#define SM_EF_SAMPLE 256u
#define SM_EF_MAGIC  UINT64_C(0x3130667a6f72666d) /* "mfrozf01" LE */
#define SM_EF_VERSION 1u
#define SM_EF_HEADER_WORDS 4u
/* The arrays follow the struct in one allocation, at an 8-byte offset
 * (on ILP32 the struct size need not be a multiple of 8). */
#define SM_EF_STRUCT_BYTES ((sizeof(sm_frozen_t) + 7) / 8 * 8)

struct sm_frozen {
	uint64_t n;          /* members */
	uint64_t max;        /* largest member (0 when n == 0) */
	uint32_t l;          /* low bits per member */
	uint64_t high_bits;  /* bits used in high[] */
	size_t low_words;    /* words in low[] */
	size_t high_words;   /* words in high[] */
	size_t s1_count;     /* entries in s1[] */
	size_t s0_count;     /* entries in s0[] */
	uint64_t *low;       /* packed low parts */
	uint64_t *high;      /* unary high parts */
	uint64_t *s1;        /* position of one number SM_EF_SAMPLE * j */
	uint64_t *s0;        /* position of zero number SM_EF_SAMPLE * j */
};

/* Bits [pos, pos + width) of a packed array, width <= 63. */
static uint64_t
__sm_ef_get_bits(const uint64_t *a, uint64_t pos, uint32_t width)
{
	const size_t w = (size_t)(pos / 64);
	const unsigned s = (unsigned)(pos % 64);
	uint64_t v;
	if (width == 0) {
		return (0);
	}
	v = a[w] >> s;
	if (s + width > 64) {
		v |= a[w + 1] << (64 - s);
	}
	return (v & ((UINT64_C(1) << width) - 1));
}

static void
__sm_ef_set_bits(uint64_t *a, uint64_t pos, uint32_t width, uint64_t v)
{
	const size_t w = (size_t)(pos / 64);
	const unsigned s = (unsigned)(pos % 64);
	if (width == 0) {
		return;
	}
	a[w] |= v << s;
	if (s + width > 64) {
		a[w + 1] |= v >> (64 - s);
	}
}

/*
 * Position in high[] of the k-th one (ones == true) or zero, or
 * f->high_bits when there is none.  Starts at the sample for k and scans
 * words; zeros past high_bits do not exist.
 */
static uint64_t
__sm_ef_select(const sm_frozen_t *f, uint64_t k, bool ones)
{
	const uint64_t *s = ones ? f->s1 : f->s0;
	const size_t ns = ones ? f->s1_count : f->s0_count;
	size_t j = (size_t)(k / SM_EF_SAMPLE);
	uint64_t pos;
	uint64_t rem;
	size_t w;
	uint64_t word;
	if (j >= ns) {
		return (f->high_bits);
	}
	pos = s[j];
	rem = k - (uint64_t)j * SM_EF_SAMPLE;
	w = (size_t)(pos / 64);
	word = ones ? f->high[w] : ~f->high[w];
	word &= ~UINT64_C(0) << (pos % 64);
	for (;;) {
		const uint64_t c = (uint64_t)SM_POPCOUNT64(word);
		if (rem < c) {
			pos = (uint64_t)w * 64 +
			    (uint64_t)__sm_word_select(word, (size_t)rem);
			return (pos < f->high_bits ? pos : f->high_bits);
		}
		rem -= c;
		if (++w >= f->high_words) {
			return (f->high_bits);
		}
		word = ones ? f->high[w] : ~f->high[w];
	}
}

/* Member k (0 <= k < n). */
static uint64_t
__sm_ef_get(const sm_frozen_t *f, uint64_t k)
{
	const uint64_t hi = __sm_ef_select(f, k, true) - k;
	return ((hi << f->l) | __sm_ef_get_bits(f->low, k * f->l, f->l));
}

/*
 * The number of members strictly below x.  The members whose high part
 * is h = x >> L (bucket h) occupy indices [k0, k1): bucket h's ones lie
 * between the (h-1)-th and the h-th zero of high[], and the index of a
 * one is its position minus the zeros before it.  Within a bucket the
 * low parts are sorted, so a binary search finds the first one >= x's.
 */
static uint64_t
__sm_ef_rank_lt(const sm_frozen_t *f, uint64_t x)
{
	uint64_t h;
	uint64_t lowx;
	uint64_t k0;
	uint64_t k1;
	if (f->n == 0 || x == 0) {
		return (0);
	}
	if (x > f->max) {
		return (f->n);
	}
	h = x >> f->l;
	lowx = (f->l == 0) ? 0 : (x & ((UINT64_C(1) << f->l) - 1));
	k0 = (h == 0) ? 0 : __sm_ef_select(f, h - 1, false) + 1 - h;
	/* x <= max, so h <= max >> L and the h-th zero exists. */
	k1 = __sm_ef_select(f, h, false) - h;
	while (k0 < k1) {
		const uint64_t mid = k0 + (k1 - k0) / 2;
		if (__sm_ef_get_bits(f->low, mid * f->l, f->l) < lowx) {
			k0 = mid + 1;
		} else {
			k1 = mid;
		}
	}
	return (k0);
}

/* Size in words of the arrays for n members with L low bits. */
static bool
__sm_ef_shape(uint64_t n, uint64_t max, uint32_t l, uint64_t *high_bits,
    size_t *low_words, size_t *high_words, size_t *s1, size_t *s0)
{
	const uint64_t zeros = n == 0 ? 0 : (max >> l) + 1;
	uint64_t low_bits;
	if (n > (UINT64_MAX - 64) / 64 || zeros > UINT64_MAX - n) {
		return (false);
	}
	low_bits = n * l;
	*high_bits = n + zeros;
	if (*high_bits > (uint64_t)SIZE_MAX / 2) {
		return (false);
	}
	{
		/* Word counts in 64 bits (none overflows: n <= 2^58, so
		 * low_bits < 2^64 - 128, and zeros <= high_bits <= 2^63).
		 * Their sum must fit in size_t as a count of words;
		 * __sm_ef_alloc checks the byte size.  Comparing in uint64_t
		 * against a size_t limit is exact on every width, and never a
		 * constant test on LP64. */
		const uint64_t lw = low_bits / 64 + 2;
		const uint64_t hw = *high_bits / 64 + (*high_bits % 64 != 0);
		const uint64_t c1 = (n + SM_EF_SAMPLE - 1) / SM_EF_SAMPLE;
		const uint64_t c0 = (zeros + SM_EF_SAMPLE - 1) / SM_EF_SAMPLE;
		const uint64_t words_max = (uint64_t)SIZE_MAX;
		if (hw > words_max - lw || c1 > words_max - lw - hw ||
		    c0 > words_max - lw - hw - c1) {
			return (false);
		}
		/* One spare word so the two-word read in __sm_ef_get_bits
		 * stays in bounds at the end of low[]. */
		*low_words = (size_t)lw;
		*high_words = (size_t)hw;
		*s1 = (size_t)c1;
		*s0 = (size_t)c0;
	}
	return (true);
}

/* Allocate a frozen map of the given shape with zeroed arrays. */
static sm_frozen_t *
__sm_ef_alloc(uint64_t n, uint64_t max, uint32_t l)
{
	uint64_t high_bits;
	size_t lw, hw, s1, s0, total;
	sm_frozen_t *f;
	if (!__sm_ef_shape(n, max, l, &high_bits, &lw, &hw, &s1, &s0)) {
		return (NULL);
	}
	total = lw + hw + s1 + s0;
	if (total > (SIZE_MAX - SM_EF_STRUCT_BYTES) / sizeof(uint64_t)) {
		return (NULL);
	}
	f = (sm_frozen_t *)__sm_alloc_zero(SM_EF_STRUCT_BYTES +
	    total * sizeof(uint64_t));
	if (f == NULL) {
		return (NULL);
	}
	f->n = n;
	f->max = max;
	f->l = l;
	f->high_bits = high_bits;
	f->low_words = lw;
	f->high_words = hw;
	f->s1_count = s1;
	f->s0_count = s0;
	f->low = (uint64_t *)(void *)((uint8_t *)f + SM_EF_STRUCT_BYTES);
	f->high = f->low + lw;
	f->s1 = f->high + hw;
	f->s0 = f->s1 + s1;
	return (f);
}

/* Fill the select samples from high[]. */
static void
__sm_ef_build_samples(sm_frozen_t *f)
{
	uint64_t ones = 0, zeros = 0, pos;
	for (pos = 0; pos < f->high_bits; pos++) {
		if ((f->high[pos / 64] >> (pos % 64)) & 1) {
			if (ones % SM_EF_SAMPLE == 0) {
				f->s1[ones / SM_EF_SAMPLE] = pos;
			}
			ones++;
		} else {
			if (zeros % SM_EF_SAMPLE == 0) {
				f->s0[zeros / SM_EF_SAMPLE] = pos;
			}
			zeros++;
		}
	}
}

/* L = floor(log2(u / n)) with u = max + 1, clamped to [0, 63], computed
 * without forming u (max may be UINT64_MAX): u / n is max / n, plus one
 * when max % n == n - 1.  That plus one overflows only when u / n is
 * 2^64 (n == 1, max == UINT64_MAX), whose log is 64, clamped to 63. */
static uint32_t
__sm_ef_low_bits(uint64_t n, uint64_t max)
{
	uint64_t q;
	if (n == 0) {
		return (0);
	}
	q = max / n;
	if (max % n == n - 1) {
		if (q == UINT64_MAX) {
			return (63);
		}
		q++;
	}
	if (q <= 1) {
		return (0);
	}
	return ((uint32_t)(63 - SM_CLZ64(q)));
}

typedef struct {
	sm_frozen_t *f;
	uint64_t k;
	uint64_t last;
	bool fail;
} __sm_ef_builder_t;

static void
__sm_ef_add_batch(uint64_t vec[], size_t n, void *aux)
{
	__sm_ef_builder_t *b = (__sm_ef_builder_t *)aux;
	sm_frozen_t *f = b->f;
	size_t i;
	for (i = 0; i < n; i++) {
		const uint64_t x = vec[i];
		uint64_t pos;
		if (b->k >= f->n || (b->k > 0 && x <= b->last)) {
			b->fail = true;
			return;
		}
		pos = (x >> f->l) + b->k;
		f->high[pos / 64] |= UINT64_C(1) << (pos % 64);
		if (f->l > 0) {
			__sm_ef_set_bits(f->low, b->k * f->l, f->l,
			    x & ((UINT64_C(1) << f->l) - 1));
		}
		b->last = x;
		b->k++;
	}
}

sm_frozen_t *
sm_freeze(const sm_t *map)
{
	uint64_t n = 0;
	uint64_t max = 0;
	__sm_ef_builder_t b;
	if (map != NULL && !sm_is_empty(map)) {
		/* Count without sm_cardinality, which fills the cache through
		 * the const map. */
		__sm_run_pair_counts(map, NULL, &n, NULL, NULL, NULL);
		max = sm_maximum(map);
	}
	b.f = __sm_ef_alloc(n, max, __sm_ef_low_bits(n, max));
	if (b.f == NULL) {
		return (NULL);
	}
	b.k = 0;
	b.last = 0;
	b.fail = false;
	if (n > 0) {
		sm_scan_op(map, NULL, SM_OP_OR, __sm_ef_add_batch, 0, &b);
	}
	if (b.fail || b.k != n) {
		__sm_free(b.f);
		return (NULL);
	}
	__sm_ef_build_samples(b.f);
	return (b.f);
}

void
sm_frozen_free(sm_frozen_t *f)
{
	__sm_free(f);
}

size_t
sm_frozen_size(const sm_frozen_t *f)
{
	if (f == NULL) {
		return (0);
	}
	return (SM_EF_STRUCT_BYTES + (f->low_words + f->high_words +
	    f->s1_count + f->s0_count) * sizeof(uint64_t));
}

size_t
sm_frozen_cardinality(const sm_frozen_t *f)
{
	return (f == NULL ? 0 : (size_t)f->n);
}

bool
sm_frozen_contains(const sm_frozen_t *f, uint64_t idx)
{
	uint64_t r;
	if (f == NULL || f->n == 0 || idx > f->max) {
		return (false);
	}
	r = __sm_ef_rank_lt(f, idx);
	return (r < f->n && __sm_ef_get(f, r) == idx);
}

size_t
sm_frozen_rank(const sm_frozen_t *f, uint64_t x, uint64_t y)
{
	uint64_t upto;
	if (f == NULL || x > y) {
		return (0);
	}
	upto = (y >= f->max) ? f->n : __sm_ef_rank_lt(f, y + 1);
	return ((size_t)(upto - __sm_ef_rank_lt(f, x)));
}

uint64_t
sm_frozen_select(const sm_frozen_t *f, uint64_t n)
{
	if (f == NULL || n >= f->n) {
		return (SM_IDX_MAX);
	}
	return (__sm_ef_get(f, n));
}

uint64_t
sm_frozen_next_member(const sm_frozen_t *f, uint64_t prev_idx)
{
	uint64_t r;
	if (f == NULL || f->n == 0) {
		return (SM_IDX_MAX);
	}
	r = (prev_idx == SM_IDX_MAX) ? 0 : __sm_ef_rank_lt(f, prev_idx + 1);
	return (r < f->n ? __sm_ef_get(f, r) : SM_IDX_MAX);
}

uint64_t
sm_frozen_prev_member(const sm_frozen_t *f, uint64_t prev_idx)
{
	uint64_t r;
	if (f == NULL) {
		return (SM_IDX_MAX);
	}
	/* SM_IDX_MAX is the exclusive bound UINT64_MAX, as in sm_prev_member. */
	r = __sm_ef_rank_lt(f, prev_idx);
	return (r > 0 ? __sm_ef_get(f, r - 1) : SM_IDX_MAX);
}

/*
 * Visit the members in order: each one of high[] at position pos is
 * member k = (ones before it) with high part pos - k.  Calls fn(x, aux)
 * and stops early when it returns false.  Returns false on early stop.
 */
static bool
__sm_ef_each(const sm_frozen_t *f, bool (*fn)(uint64_t, void *), void *aux)
{
	uint64_t k = 0;
	size_t w;
	for (w = 0; w < f->high_words; w++) {
		uint64_t bits = f->high[w];
		while (bits != 0) {
			const uint64_t pos =
			    (uint64_t)w * 64 + (uint64_t)SM_CTZ64(bits);
			const uint64_t x = ((pos - k) << f->l) |
			    __sm_ef_get_bits(f->low, k * f->l, f->l);
			bits &= bits - 1;
			if (!fn(x, aux)) {
				return (false);
			}
			k++;
		}
	}
	return (true);
}

typedef struct {
	sm_t *map;
	sm_cursor_t cur;
} __sm_ef_thaw_t;

static bool
__sm_ef_thaw_one(uint64_t x, void *aux)
{
	__sm_ef_thaw_t *t = (__sm_ef_thaw_t *)aux;
	return (sm_add_grow_cursor(&t->map, x, &t->cur) == x);
}

sm_t *
sm_thaw(const sm_frozen_t *f)
{
	__sm_ef_thaw_t t;
	t.map = sm_create(0);
	__sm_cursor_reset(&t.cur);
	if (t.map == NULL || f == NULL) {
		return (t.map);
	}
	if (!__sm_ef_each(f, __sm_ef_thaw_one, &t)) {
		sm_free(t.map);
		return (NULL);
	}
	return (t.map);
}

/* Little-endian u64 load and store, independent of the host order. */
static uint64_t
__sm_ef_load_le(const uint8_t *p)
{
	uint64_t v = 0;
	int i;
	for (i = 7; i >= 0; i--) {
		v = (v << 8) | p[i];
	}
	return (v);
}

static void
__sm_ef_store_le(uint8_t *p, uint64_t v)
{
	int i;
	for (i = 0; i < 8; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

size_t
sm_frozen_serialized_size(const sm_frozen_t *f)
{
	if (f == NULL) {
		return (0);
	}
	return ((SM_EF_HEADER_WORDS + f->low_words + f->high_words) *
	    sizeof(uint64_t));
}

size_t
sm_frozen_serialize(const sm_frozen_t *f, uint8_t *out, size_t out_size)
{
	const size_t need = sm_frozen_serialized_size(f);
	size_t i;
	uint8_t *p = out;
	if (f == NULL || out == NULL || out_size < need) {
		return (0);
	}
	__sm_ef_store_le(p, SM_EF_MAGIC);
	__sm_ef_store_le(p + 8, (uint64_t)SM_EF_VERSION |
	    ((uint64_t)f->l << 32));
	__sm_ef_store_le(p + 16, f->n);
	__sm_ef_store_le(p + 24, f->max);
	p += SM_EF_HEADER_WORDS * sizeof(uint64_t);
	for (i = 0; i < f->low_words + f->high_words; i++) {
		__sm_ef_store_le(p, f->low[i]);
		p += sizeof(uint64_t);
	}
	return (need);
}

typedef struct {
	uint64_t k;
	uint64_t last;
	bool ok;
} __sm_ef_check_t;

static bool
__sm_ef_check_one(uint64_t x, void *aux)
{
	__sm_ef_check_t *c = (__sm_ef_check_t *)aux;
	if (c->k > 0 && x <= c->last) {
		c->ok = false;
		return (false);
	}
	c->last = x;
	c->k++;
	return (true);
}

/*
 * Structural check of a decoded frozen map: high[] has exactly n ones
 * and nothing past high_bits, the members it decodes to strictly
 * increase and end at max, and no bit past the packed low parts is set.
 * Together these make every query stay inside the arrays and return the
 * member set the header claims.
 */
static bool
__sm_ef_valid(sm_frozen_t *f)
{
	__sm_ef_check_t c;
	uint64_t ones = 0;
	const uint64_t low_bits = f->n * f->l;
	size_t w;
	for (w = 0; w < f->high_words; w++) {
		ones += (uint64_t)SM_POPCOUNT64(f->high[w]);
	}
	if (ones != f->n || (f->high_bits % 64 != 0 &&
	    (f->high[f->high_words - 1] >> (f->high_bits % 64)) != 0)) {
		return (false);
	}
	for (w = (size_t)(low_bits / 64); w < f->low_words; w++) {
		const unsigned used = (w == low_bits / 64) ?
		    (unsigned)(low_bits % 64) : 0u;
		if (used == 0 ? f->low[w] != 0 : (f->low[w] >> used) != 0) {
			return (false);
		}
	}
	c.k = 0;
	c.last = 0;
	c.ok = true;
	(void)__sm_ef_each(f, __sm_ef_check_one, &c);
	return (c.ok && c.k == f->n && (f->n == 0 || c.last == f->max));
}

sm_frozen_t *
sm_frozen_deserialize(const uint8_t *in, size_t n)
{
	uint64_t ver;
	uint64_t count;
	uint64_t max;
	uint32_t l;
	uint64_t high_bits;
	size_t lw, hw, s1, s0, i;
	sm_frozen_t *f;
	const uint8_t *p;
	if (in == NULL || n < SM_EF_HEADER_WORDS * sizeof(uint64_t) ||
	    n % sizeof(uint64_t) != 0 || __sm_ef_load_le(in) != SM_EF_MAGIC) {
		return (NULL);
	}
	ver = __sm_ef_load_le(in + 8);
	count = __sm_ef_load_le(in + 16);
	max = __sm_ef_load_le(in + 24);
	l = (uint32_t)(ver >> 32);
	/* The low-bit width is a function of n and max; any other value is
	 * not something sm_freeze writes. */
	/* max == SM_IDX_MAX would be a member that is not one. */
	if ((ver & UINT64_C(0xFFFFFFFF)) != SM_EF_VERSION ||
	    max == SM_IDX_MAX || l != __sm_ef_low_bits(count, max) ||
	    (count == 0 && max != 0)) {
		return (NULL);
	}
	if (!__sm_ef_shape(count, max, l, &high_bits, &lw, &hw, &s1, &s0) ||
	    n / sizeof(uint64_t) - SM_EF_HEADER_WORDS != lw + hw) {
		return (NULL);
	}
	f = __sm_ef_alloc(count, max, l);
	if (f == NULL) {
		return (NULL);
	}
	p = in + SM_EF_HEADER_WORDS * sizeof(uint64_t);
	for (i = 0; i < lw + hw; i++) {
		f->low[i] = __sm_ef_load_le(p);
		p += sizeof(uint64_t);
	}
	if (!__sm_ef_valid(f)) {
		__sm_free(f);
		return (NULL);
	}
	__sm_ef_build_samples(f);
	return (f);
}

/* -------------------------------------------------------------------
 * Point-lookup / rank / select acceleration (Ideas 3, 4, 5)
 *
 * These add caller-owned, transient acceleration state on TOP of the
 * plain O(chunks) path.  None of them grow sm_t or touch the wire
 * format.  Each answers exactly as sm_contains / sm_rank / sm_select do
 * on the map it was built over; once the map is mutated the state is
 * stale and must be reset or rebuilt (see each type's contract in sm.h).
 * ------------------------------------------------------------------- */

/* -------------------------------------------------------------------
 * Idea 5: sm_contains_many -- batched point lookups in one sweep.
 * ------------------------------------------------------------------- */

void
sm_contains_many(const sm_t *map, const uint64_t *idxs, bool *results,
    size_t n)
{
	size_t count;
	uint8_t * base;
	uint8_t * p;
	size_t stream_end;
	size_t q;
	size_t i;
	if (n == 0) {
		return;
	}
	if (idxs == NULL || results == NULL) {
		errno = EINVAL;
		return;
	}
	if (map == NULL) {
		size_t q_s;
		for (q_s = 0; q_s < n; q_s++) {
			results[q_s] = false;
		}
		return;
	}

#ifdef SPARSEMAP_DIAGNOSTIC
	/* Contract: idxs MUST be sorted ascending. */
	for (size_t q = 1; q < n; q++) {
		__sm_assert(idxs[q] >= idxs[q - 1]);
	}
#endif

	if (__sm_is_small(map)) {
		size_t q_s;
		for (q_s = 0; q_s < n; q_s++) {
			results[q_s] = __sm_small_contains(map, idxs[q_s]);
		}
		return;
	}

	count = __sm_get_chunk_count(map);
	if (count == 0) {
		size_t q_s;
		for (q_s = 0; q_s < n; q_s++) {
			results[q_s] = false;
		}
		return;
	}

	base = __sm_get_chunk_data(map, 0);
	p = base;
	stream_end = (size_t)map->m_data_used - SM_SIZEOF_OVERHEAD;
	q = 0;

	/*
	 * One left-to-right sweep.  Walk chunks in order while draining the
	 * query cursor q into idxs[].  For each chunk [start, start+cap):
	 * queries strictly below start fall in a gap (false); queries below
	 * start+cap are answered by the within-chunk test; queries at or
	 * above start+cap belong to a later chunk, so advance the chunk.
	 * O(chunks + n).
	 */
	for (i = 0; i < count && q < n; i++) {
		size_t cap;
		uint64_t hi;
		size_t next_off;
		const __sm_idx_t s = __sm_load_idx((const uint8_t *)p);
		__sm_chunk_t chunk;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		cap = __sm_chunk_get_capacity(&chunk);
		hi = __sm_end(s, cap); /* exclusive top */

		/* Drain queries that fall before this chunk (gap -> false). */
		while (q < n && idxs[q] < (uint64_t)s) {
			results[q] = false;
			q++;
		}
		/* Answer queries that fall inside this chunk's covered span. */
		while (q < n && idxs[q] < hi) {
			results[q] =
			    __sm_chunk_is_set(&chunk, idxs[q] - (uint64_t)s);
			q++;
		}

		/* Advance to the next chunk. */
		next_off = (size_t)(p - base) +
		    SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		if (next_off >= stream_end) {
			break;
		}
		p = base + next_off;
	}

	/* Any queries past the last chunk are not set. */
	for (; q < n; q++) {
		results[q] = false;
	}
}

/* Set bits strictly below idx in a small-set map (sm_rank_many's
 * convention), via the inclusive-range helper. */
static size_t
__sm_batch_small_rank(const sm_t *map, uint64_t idx)
{
	return (idx == 0 ? 0 : __sm_small_rank1(map, 0, idx - 1));
}

/* Position of the k-th (0-based) set bit of a small-set map, or
 * SM_IDX_MAX when it has k or fewer members. */
static uint64_t
__sm_batch_small_select(const sm_t *map, uint64_t k)
{
	return (__sm_small_select(map, k, true));
}

/* -------------------------------------------------------------------
 * Idea 4: sm_locator_t -- transient two-level chunk directory.
 *
 * Chunks are variable-size, so the plain path reaches chunk i only by
 * walking chunks 0 .. i-1.  The locator records where every chunk is and
 * how many set bits precede it, in two levels (the RsVec layout, over
 * chunks rather than bit positions, since the universe is 2^64):
 *
 *   - per group of SM_LOC_GROUP chunks, absolute values for the group's
 *     first chunk: its start, the set bits before it, and its
 *     base-relative byte offset (g_start, g_set, g_offset);
 *   - per chunk, one u64 of deltas from its group's first chunk
 *     (c_rel): bits 0-15 the byte offset (16 chunks of at most 272
 *     bytes always fit), bits 16-31 the start in units of
 *     SM_CHUNK_MAX_CAPACITY, bits 32-63 the set bits before it.
 *
 * A group whose start delta does not fit (it spans more than 2^16
 * windows, 2^27 bits) is "wide": its chunks' absolute starts and set
 * prefixes go to w_start / w_set at slot g_wide[g], and c_rel keeps only
 * the offset.  The set delta of a narrow group is at most its start
 * delta, so it always fits.
 *
 * Every query is a binary search over the group arrays, then one over at
 * most SM_LOC_GROUP entries, then work inside one chunk: O(log chunks).
 * The three search keys are each non-decreasing in the chunk index: the
 * start; the set bits before the chunk; and the unset bits before it,
 * start - set prefix (every position below a chunk's start that is not
 * a set bit of an earlier chunk is unset).
 * ------------------------------------------------------------------- */

enum {
	SM_LOC_GROUP = 16,           /* chunks per absolute record */
	SM_LOC_GROUP_SHIFT = 4,      /* log2(SM_LOC_GROUP) */
	SM_LOC_KEY_START = 0,        /* search by chunk start */
	SM_LOC_KEY_SET = 1,          /* search by set bits before */
	SM_LOC_KEY_ZERO = 2          /* search by unset bits before */
};

#define SM_LOC_OFF_MASK   UINT64_C(0xFFFF)
#define SM_LOC_START_MAX  UINT64_C(0xFFFF)

/* The start of chunk i. */
static inline uint64_t
__sm_loc_start(const sm_locator_t *loc, size_t i)
{
	const size_t g = i >> SM_LOC_GROUP_SHIFT;
	if (loc->g_wide[g] != SIZE_MAX) {
		return (loc->w_start[loc->g_wide[g] * SM_LOC_GROUP +
		    (i & (SM_LOC_GROUP - 1))]);
	}
	return (loc->g_start[g] + ((loc->c_rel[i] >> 16) & SM_LOC_START_MAX) *
	    SM_CHUNK_MAX_CAPACITY);
}

/* The set bits in chunks 0 .. i-1. */
static inline uint64_t
__sm_loc_set(const sm_locator_t *loc, size_t i)
{
	const size_t g = i >> SM_LOC_GROUP_SHIFT;
	if (loc->g_wide[g] != SIZE_MAX) {
		return (loc->w_set[loc->g_wide[g] * SM_LOC_GROUP +
		    (i & (SM_LOC_GROUP - 1))]);
	}
	return (loc->g_set[g] + (loc->c_rel[i] >> 32));
}

/* The base-relative byte offset of chunk i. */
static inline size_t
__sm_loc_offset(const sm_locator_t *loc, size_t i)
{
	return (loc->g_offset[i >> SM_LOC_GROUP_SHIFT] +
	    (size_t)(loc->c_rel[i] & SM_LOC_OFF_MASK));
}

/* Search key `kind` of chunk i, and of group g's first chunk. */
static inline uint64_t
__sm_loc_key(const sm_locator_t *loc, size_t i, int kind)
{
	if (kind == SM_LOC_KEY_START) {
		return (__sm_loc_start(loc, i));
	}
	if (kind == SM_LOC_KEY_SET) {
		return (__sm_loc_set(loc, i));
	}
	return (__sm_loc_start(loc, i) - __sm_loc_set(loc, i));
}

static inline uint64_t
__sm_loc_group_key(const sm_locator_t *loc, size_t g, int kind)
{
	if (kind == SM_LOC_KEY_START) {
		return (loc->g_start[g]);
	}
	if (kind == SM_LOC_KEY_SET) {
		return (loc->g_set[g]);
	}
	return (loc->g_start[g] - loc->g_set[g]);
}

/* The last chunk whose key `kind` is <= x, or SIZE_MAX when even chunk
 * 0's key exceeds x.  Binary search over the groups, then over the at
 * most SM_LOC_GROUP chunks of the chosen group. */
static size_t
__sm_loc_find(const sm_locator_t *loc, uint64_t x, int kind)
{
	size_t lo = 0, hi = loc->n_grp;
	size_t first;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (__sm_loc_group_key(loc, mid, kind) <= x) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if (lo == 0) {
		return (SIZE_MAX);
	}
	first = (lo - 1) << SM_LOC_GROUP_SHIFT;
	lo = first + 1;
	hi = first + SM_LOC_GROUP < loc->count ? first + SM_LOC_GROUP
	                                       : loc->count;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (__sm_loc_key(loc, mid, kind) <= x) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return (lo - 1);
}

/* True when the locator's fingerprint no longer matches its map, i.e. the
 * caller mutated the map without rebuilding.  A stale locator is a usage
 * error.  The check is O(1) and catches any mutation that inserts,
 * removes, or moves a chunk, or that changes the buffer or its used
 * size; it cannot see a bit flipped in place inside an existing chunk
 * (see the sm_locator_t contract in sm.h). */
static bool
__sm_locator_is_stale(const sm_locator_t *loc)
{
	const sm_t *map;
	uint8_t *base;
	__sm_idx_t first;
	__sm_idx_t last;
	if (loc == NULL || loc->map == NULL) {
		return (true);
	}
	map = loc->map;
	if (map->m_data != loc->data || map->m_data_used != loc->used) {
		return (true);
	}
	/* A cardinality cached at build time that is now invalid or
	 * different means a membership-changing call ran since.  The cache
	 * is filled lazily, so an unpopulated one at build proves nothing. */
	if (loc->card_plus1 != 0 && map->m_card_plus1 != loc->card_plus1) {
		return (true);
	}
	if (loc->small) {
		return (!__sm_is_small(map));
	}
	if (__sm_is_small(map) ||
	    __sm_get_chunk_count(map) != loc->count) {
		return (true);
	}
	if (loc->count == 0) {
		return (false); /* still empty: no chunk start to read */
	}
	base = __sm_get_chunk_data(map, 0);
	first = __sm_load_idx((const uint8_t *)base);
	if (first != loc->first_start) {
		return (true);
	}
	if ((size_t)loc->last_offset + sizeof(__sm_idx_t) >
	    (size_t)map->m_data_used - SM_SIZEOF_OVERHEAD) {
		return (true);
	}
	last =
	    __sm_load_idx((const uint8_t *)(base + loc->last_offset));
	if (last != loc->last_start) {
		return (true);
	}
	return (false);
}

/* Write group g's per-chunk records: deltas into c_rel when they fit,
 * otherwise absolute values into the wide arrays (grown here).  start[]
 * and set[] hold the group's n chunks.  Returns false on allocation
 * failure. */
static bool
__sm_loc_put_group(sm_locator_t *loc, size_t g, const uint64_t *start,
    const uint64_t *set, size_t n, size_t *wide_cap)
{
	const size_t first = g << SM_LOC_GROUP_SHIFT;
	bool narrow = true;
	size_t j;
	loc->g_start[g] = start[0];
	loc->g_set[g] = set[0];
	/* Only the start delta can overflow.  The set bits in chunks
	 * first .. first+j-1 all lie in [start[0], start[j]), so the set
	 * delta never exceeds the start delta, under 2^27 when that fits. */
	for (j = 0; j < n; j++) {
		if ((start[j] - start[0]) / SM_CHUNK_MAX_CAPACITY >
		    SM_LOC_START_MAX) {
			narrow = false;
		}
	}
	if (narrow) {
		loc->g_wide[g] = SIZE_MAX;
		for (j = 0; j < n; j++) {
			loc->c_rel[first + j] |=
			    (((start[j] - start[0]) / SM_CHUNK_MAX_CAPACITY)
			        << 16) |
			    ((set[j] - set[0]) << 32);
		}
		return (true);
	}
	if (loc->n_wide == *wide_cap) {
		const size_t cap = *wide_cap ? *wide_cap * 2 : 4;
		uint64_t *ws = (uint64_t *)__sm_realloc(loc->w_start,
		    cap * SM_LOC_GROUP * sizeof(uint64_t));
		uint64_t *wt;
		if (ws == NULL) {
			return (false);
		}
		loc->w_start = ws;
		wt = (uint64_t *)__sm_realloc(loc->w_set,
		    cap * SM_LOC_GROUP * sizeof(uint64_t));
		if (wt == NULL) {
			return (false);
		}
		loc->w_set = wt;
		*wide_cap = cap;
	}
	loc->g_wide[g] = loc->n_wide;
	for (j = 0; j < n; j++) {
		loc->w_start[loc->n_wide * SM_LOC_GROUP + j] = start[j];
		loc->w_set[loc->n_wide * SM_LOC_GROUP + j] = set[j];
	}
	loc->n_wide++;
	return (true);
}

/* Record the fingerprint of map in loc (see __sm_locator_is_stale). */
static void
__sm_loc_fingerprint(sm_locator_t *loc, const sm_t *map)
{
	loc->map = map;
	loc->data = map->m_data;
	loc->used = map->m_data_used;
	loc->card_plus1 = map->m_card_plus1;
}

/* One O(chunk count) walk filling the directory of a chunk-mode map with
 * count > 0 chunks.  Returns false on allocation failure. */
static bool
__sm_loc_fill(sm_locator_t *loc, const sm_t *map, size_t count)
{
	uint64_t start[SM_LOC_GROUP];
	uint64_t set[SM_LOC_GROUP];
	uint8_t *base = __sm_get_chunk_data(map, 0);
	const size_t stream_end =
	    (size_t)map->m_data_used - SM_SIZEOF_OVERHEAD;
	size_t off = 0;
	size_t wide_cap = 0;
	uint64_t running = 0; /* set bits in the chunks before chunk i */
	size_t i;
	for (i = 0; i < count; i++) {
		const size_t g = i >> SM_LOC_GROUP_SHIFT;
		const size_t j = i & (SM_LOC_GROUP - 1);
		__sm_chunk_t chunk;
		__sm_chunk_rank_t rank;
		size_t cap;
		__sm_chunk_init(&chunk, base + off + SM_SIZEOF_OVERHEAD);
		if (j == 0) {
			loc->g_offset[g] = off;
		}
		start[j] = (uint64_t)__sm_load_idx(base + off);
		set[j] = running;
		loc->c_rel[i] = (uint64_t)(off - loc->g_offset[g]);
		loc->last_offset = off;
		loc->last_start = start[j];
		cap = __sm_chunk_get_capacity(&chunk);
		running +=
		    (uint64_t)__sm_chunk_rank(&rank, true, &chunk, 0, cap - 1);
		if ((j == SM_LOC_GROUP - 1 || i + 1 == count) &&
		    !__sm_loc_put_group(loc, g, start, set, j + 1, &wide_cap)) {
			return (false);
		}
		off += SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		if (off >= stream_end && i + 1 < count) {
			return (false); /* count over-claims the stream */
		}
	}
	loc->total = running;
	return (true);
}

sm_locator_t *
sm_locator_build(const sm_t *map)
{
	sm_locator_t *loc;
	size_t count;
	if (map == NULL) {
		return (NULL);
	}
	loc = (sm_locator_t *)__sm_alloc_zero(sizeof(*loc));
	if (loc == NULL) {
		return (NULL);
	}
	/* A small-set map has no chunk stream; its queries run straight on
	 * its at most SM_SMALL_MAX_WORDS words. */
	if (__sm_is_small(map)) {
		__sm_loc_fingerprint(loc, map);
		loc->small = true;
		return (loc);
	}
	count = __sm_get_chunk_count(map);
	if (count == 0) {
		/* No directory: every query finds no chunk, which answers
		 * for the empty map. */
		__sm_loc_fingerprint(loc, map);
		return (loc);
	}
	loc->count = count;
	loc->n_grp = (count + SM_LOC_GROUP - 1) >> SM_LOC_GROUP_SHIFT;
	loc->g_start = (uint64_t *)__sm_alloc(loc->n_grp * sizeof(uint64_t));
	loc->g_set = (uint64_t *)__sm_alloc(loc->n_grp * sizeof(uint64_t));
	loc->g_offset = (size_t *)__sm_alloc(loc->n_grp * sizeof(size_t));
	loc->g_wide = (size_t *)__sm_alloc(loc->n_grp * sizeof(size_t));
	loc->c_rel = (uint64_t *)__sm_alloc(count * sizeof(uint64_t));
	if (loc->g_start == NULL || loc->g_set == NULL ||
	    loc->g_offset == NULL || loc->g_wide == NULL ||
	    loc->c_rel == NULL || !__sm_loc_fill(loc, map, count)) {
		sm_locator_free(loc);
		return (NULL);
	}
	__sm_loc_fingerprint(loc, map);
	loc->first_start = loc->g_start[0];
	return (loc);
}

void
sm_locator_free(sm_locator_t *loc)
{
	if (loc == NULL) {
		return;
	}
	__sm_free(loc->g_start);
	__sm_free(loc->g_set);
	__sm_free(loc->g_offset);
	__sm_free(loc->g_wide);
	__sm_free(loc->c_rel);
	__sm_free(loc->w_start);
	__sm_free(loc->w_set);
	__sm_free(loc);
}

/* Load chunk i into *chunk; returns its start. */
static inline uint64_t
__sm_loc_chunk(const sm_locator_t *loc, size_t i, __sm_chunk_t *chunk)
{
	uint8_t *p = __sm_get_chunk_data(loc->map, __sm_loc_offset(loc, i));
	__sm_chunk_init(chunk, p + SM_SIZEOF_OVERHEAD);
	return (__sm_loc_start(loc, i));
}

/* Set bits in [0, x] of a fresh chunk-mode locator's map: the set bits
 * before the last chunk starting at or below x, plus that chunk's set
 * bits up to x. */
static uint64_t
__sm_loc_rank_upto(const sm_locator_t *loc, uint64_t x)
{
	const size_t i = __sm_loc_find(loc, x, SM_LOC_KEY_START);
	__sm_chunk_t chunk;
	__sm_chunk_rank_t rank;
	uint64_t s;
	size_t cap;
	uint64_t to;
	if (i == SIZE_MAX) {
		return (0);
	}
	s = __sm_loc_chunk(loc, i, &chunk);
	cap = __sm_chunk_get_capacity(&chunk);
	to = x - s < (uint64_t)cap ? x - s : (uint64_t)cap - 1;
	return (__sm_loc_set(loc, i) +
	    (uint64_t)__sm_chunk_rank(&rank, true, &chunk, 0, (size_t)to));
}

/* Set bits in [0, x] of a small-set map. */
static uint64_t
__sm_loc_small_rank_upto(const sm_t *map, uint64_t x)
{
	if (x >= (uint64_t)SM_SMALL_MAX_BITS) {
		return ((uint64_t)__sm_small_cardinality(map));
	}
	return ((uint64_t)__sm_batch_small_rank(map, x + 1));
}

/* Set bits in [0, x], whichever mode the (fresh) locator is in. */
static uint64_t
__sm_loc_upto(const sm_locator_t *loc, uint64_t x)
{
	if (loc->small) {
		return (__sm_loc_small_rank_upto(loc->map, x));
	}
	return (__sm_loc_rank_upto(loc, x));
}

/* The k-th set bit, whichever mode the (fresh) locator is in. */
static uint64_t
__sm_loc_select1(const sm_locator_t *loc, uint64_t k)
{
	size_t i;
	__sm_chunk_t chunk;
	uint64_t s;
	ssize_t rem;
	size_t at;
	if (loc->small) {
		return (__sm_batch_small_select(loc->map, k));
	}
	if (k >= loc->total) {
		return (SM_IDX_MAX);
	}
	/* k < total, so chunk i holds the k-th set bit: the next chunk's
	 * prefix (or the total) exceeds k. */
	i = __sm_loc_find(loc, k, SM_LOC_KEY_SET);
	s = __sm_loc_chunk(loc, i, &chunk);
	rem = (ssize_t)(k - __sm_loc_set(loc, i));
	at = __sm_chunk_select(&chunk, rem, &rem, true);
	return (s + at);
}

/* The rem-th unset position at or after `from`, given that the 64-bit
 * word w holds the set bits of [from, from + 64): rem < 64 - popcount(w)
 * on entry. */
static uint64_t
__sm_loc_word_select0(uint64_t from, __sm_bitvec_t w, uint64_t rem)
{
	return (from + (uint64_t)__sm_word_select(~w, (size_t)rem));
}

/* The rem-th unset position at or after s + n, where every position
 * from there on is unset, or SM_IDX_MAX when that is past 2^64 - 2.
 * Taking the start and length rather than their sum keeps "the span
 * reaches 2^64" (no positions left) apart from an empty span at 0. */
static uint64_t
__sm_loc_tail_select0(uint64_t s, uint64_t n, uint64_t rem)
{
	uint64_t tail;
	if (n > UINT64_MAX - s) {
		return (SM_IDX_MAX);
	}
	tail = s + n;
	if (rem >= UINT64_MAX - tail) {
		return (SM_IDX_MAX);
	}
	return (tail + rem);
}

/* The k-th unset position of a small-set map (see sm_locator_select). */
static uint64_t
__sm_loc_small_select0(const sm_t *map, uint64_t k)
{
	const __sm_bitvec_unaligned_t *w = __sm_small_words(map);
	const size_t nw = __sm_small_nwords(map);
	size_t i;
	for (i = 0; i < nw; i++) {
		const uint64_t z = 64 - (uint64_t)SM_POPCOUNT64(w[i]);
		if (k < z) {
			return (__sm_loc_word_select0((uint64_t)i * 64, w[i],
			    k));
		}
		k -= z;
	}
	return (__sm_loc_tail_select0(0, (uint64_t)nw * 64, k));
}

/* The k-th unset position of a fresh chunk-mode locator's map.  Chunk i,
 * the last whose unset-bits-before key is <= k, starts at or before the
 * answer, and the answer lies below the next chunk's start (whose key
 * exceeds k).  Inside chunk i the bits are positional: slot v covers
 * [s + 64 v, s + 64 v + 64), and a NONE slot holds no set bits. */
static uint64_t
__sm_loc_select0(const sm_locator_t *loc, uint64_t k)
{
	const size_t i = __sm_loc_find(loc, k, SM_LOC_KEY_ZERO);
	__sm_chunk_t chunk;
	__sm_bitvec_t words[SM_FLAGS_PER_INDEX];
	int cap_flags[SM_FLAGS_PER_INDEX];
	uint64_t s;
	uint64_t rem;
	size_t v;
	if (i == SIZE_MAX) {
		return (k); /* in the gap before chunk 0 */
	}
	s = __sm_loc_chunk(loc, i, &chunk);
	rem = k - (s - __sm_loc_set(loc, i));
	__sm_expand_sparse_chunk(&chunk, words, cap_flags);
	for (v = 0; v < SM_FLAGS_PER_INDEX; v++) {
		const uint64_t z = 64 - (uint64_t)SM_POPCOUNT64(words[v]);
		if (rem < z) {
			const uint64_t from =
			    s + (uint64_t)v * SM_BITS_PER_VECTOR;
			return (__sm_loc_word_select0(from, words[v], rem));
		}
		rem -= z;
	}
	return (__sm_loc_tail_select0(s, SM_CHUNK_MAX_CAPACITY, rem));
}

bool
sm_locator_contains(const sm_locator_t *loc, uint64_t idx)
{
	size_t i;
	__sm_chunk_t chunk;
	uint64_t s;
	if (__sm_locator_is_stale(loc)) {
		__sm_assert(false);
		return (sm_contains(loc ? loc->map : NULL, idx, NULL));
	}
	if (loc->small) {
		return (__sm_small_contains(loc->map, idx));
	}
	i = __sm_loc_find(loc, idx, SM_LOC_KEY_START);
	if (i == SIZE_MAX) {
		return (false);
	}
	s = __sm_loc_chunk(loc, i, &chunk);
	/* idx - s < cap, not idx < s + cap: the chunk ending at 2^64 wraps
	 * s + cap to 0. */
	if (idx - s >= (uint64_t)__sm_chunk_get_capacity(&chunk)) {
		return (false);
	}
	return (__sm_chunk_is_set(&chunk, (size_t)(idx - s)));
}

size_t
sm_locator_rank(const sm_locator_t *loc, uint64_t lo, uint64_t hi, bool value)
{
	uint64_t set;
	uint64_t span;
	if (__sm_locator_is_stale(loc)) {
		__sm_assert(false);
		/* sm_rank does not accept a NULL map: answer directly. */
		if (loc == NULL || loc->map == NULL) {
			return (0);
		}
		return (sm_rank((sm_t *)loc->map, lo, hi, value));
	}
	if (lo > hi) {
		return (0);
	}
	set = __sm_loc_upto(loc, hi);
	if (lo > 0) {
		set -= __sm_loc_upto(loc, lo - 1);
	}
	if (value) {
		return ((size_t)set);
	}
	/* Unset = width - set, with the width saturated exactly as sm_rank
	 * saturates it for the full 2^64 range. */
	span = hi - lo;
	return ((span == UINT64_MAX ? SIZE_MAX : (size_t)(span + 1)) -
	    (size_t)set);
}

uint64_t
sm_locator_select(const sm_locator_t *loc, uint64_t n, bool value)
{
	if (__sm_locator_is_stale(loc)) {
		__sm_assert(false);
		/* sm_select does not accept a NULL map: answer directly. */
		if (loc == NULL || loc->map == NULL) {
			return (SM_IDX_MAX);
		}
		return (sm_select((sm_t *)loc->map, n, value));
	}
	if (value) {
		return (__sm_loc_select1(loc, n));
	}
	if (loc->small) {
		return (__sm_loc_small_select0(loc->map, n));
	}
	return (__sm_loc_select0(loc, n));
}

/*
 * sm_locator_next_member: the first set bit above prev_idx is the one
 * whose rank is the number of set bits in [0, prev_idx].  Two directory
 * lookups, so a scan in either direction is O(log chunks) per step even
 * across long stretches of one-member chunks.
 */
uint64_t
sm_locator_next_member(const sm_locator_t *loc, uint64_t prev_idx)
{
	if (__sm_locator_is_stale(loc)) {
		__sm_assert(false);
		return (sm_next_member(loc ? loc->map : NULL, prev_idx, NULL));
	}
	if (prev_idx == SM_IDX_MAX) {
		return (__sm_loc_select1(loc, 0));
	}
	return (__sm_loc_select1(loc, __sm_loc_upto(loc, prev_idx)));
}

/*
 * sm_locator_prev_member: the last set bit below prev_idx is the one
 * whose rank is one less than the number of set bits in
 * [0, prev_idx - 1].  SM_IDX_MAX ("from the end") is the exclusive bound
 * 2^64 - 1, as in sm_prev_member.
 */
uint64_t
sm_locator_prev_member(const sm_locator_t *loc, uint64_t prev_idx)
{
	uint64_t below;
	if (__sm_locator_is_stale(loc)) {
		__sm_assert(false);
		return (sm_prev_member(loc ? loc->map : NULL, prev_idx, NULL));
	}
	if (prev_idx == 0) {
		return (SM_IDX_MAX);
	}
	below = __sm_loc_upto(loc, prev_idx - 1);
	if (below == 0) {
		return (SM_IDX_MAX);
	}
	return (__sm_loc_select1(loc, below - 1));
}

/* -------------------------------------------------------------------
 * Batch rank and select: one left-to-right sweep, like sm_contains_many.
 * ------------------------------------------------------------------- */

/*
 * sm_rank_many: out[q] = set bits strictly below idxs[q], for ascending
 * idxs.  One sweep over the chunks carries the set bits in every chunk
 * already passed; a query below a chunk's start takes that running
 * count, a query inside the chunk adds the chunk's set bits below it,
 * and a query at or past the chunk's end waits for a later chunk.
 * O(chunks + n).
 */
void
sm_rank_many(const sm_t *map, const uint64_t *idxs, size_t *out, size_t n)
{
	size_t count;
	uint8_t *base;
	uint8_t *p;
	size_t stream_end;
	size_t running = 0; /* set bits in the chunks before p */
	size_t q = 0;
	size_t i;
	if (n == 0) {
		return;
	}
	if (idxs == NULL || out == NULL) {
		errno = EINVAL;
		return;
	}
	if (map == NULL) {
		for (q = 0; q < n; q++) {
			out[q] = 0;
		}
		return;
	}

#ifdef SPARSEMAP_DIAGNOSTIC
	/* Contract: idxs MUST be sorted ascending. */
	for (q = 1; q < n; q++) {
		__sm_assert(idxs[q] >= idxs[q - 1]);
	}
#endif

	if (__sm_is_small(map)) {
		for (q = 0; q < n; q++) {
			out[q] = __sm_batch_small_rank(map, idxs[q]);
		}
		return;
	}

	count = __sm_get_chunk_count(map);
	base = count ? __sm_get_chunk_data(map, 0) : NULL;
	p = base;
	stream_end = (size_t)map->m_data_used - SM_SIZEOF_OVERHEAD;
	for (i = 0; i < count && q < n; i++) {
		const __sm_idx_t s = __sm_load_idx((const uint8_t *)p);
		__sm_chunk_t chunk;
		__sm_chunk_rank_t rank;
		size_t cap;
		size_t next_off;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		cap = __sm_chunk_get_capacity(&chunk);

		/* Queries at or below this chunk's start: nothing of it is
		 * below them. */
		while (q < n && idxs[q] <= (uint64_t)s) {
			out[q] = running;
			q++;
		}
		/* Queries inside the chunk.  idx - s < cap rather than
		 * idx < s + cap: the chunk that ends at 2^64 wraps s + cap. */
		while (q < n && idxs[q] - (uint64_t)s < cap) {
			out[q] = running + __sm_chunk_rank(&rank, true, &chunk,
			    0, (size_t)(idxs[q] - (uint64_t)s) - 1);
			q++;
		}
		running += __sm_chunk_rank(&rank, true, &chunk, 0, cap - 1);

		next_off = (size_t)(p - base) +
		    SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		if (next_off >= stream_end) {
			break;
		}
		p = base + next_off;
	}

	/* Queries past the last chunk see every set bit. */
	for (; q < n; q++) {
		out[q] = running;
	}
}

/*
 * sm_select_many: out[q] = sm_select(map, ks[q], true), for ascending
 * ks.  One sweep over the chunks carries the set bits before the
 * current chunk; a k below running + this chunk's set bits is answered
 * by __sm_chunk_select inside the chunk, otherwise the sweep advances.
 * O(chunks + n).
 */
void
sm_select_many(const sm_t *map, const uint64_t *ks, uint64_t *out, size_t n)
{
	size_t count;
	uint8_t *base;
	uint8_t *p;
	size_t stream_end;
	uint64_t running = 0; /* set bits in the chunks before p */
	size_t q = 0;
	size_t i;
	if (n == 0) {
		return;
	}
	if (ks == NULL || out == NULL) {
		errno = EINVAL;
		return;
	}
	if (map == NULL) {
		for (q = 0; q < n; q++) {
			out[q] = SM_IDX_MAX;
		}
		return;
	}

#ifdef SPARSEMAP_DIAGNOSTIC
	/* Contract: ks MUST be sorted ascending. */
	for (q = 1; q < n; q++) {
		__sm_assert(ks[q] >= ks[q - 1]);
	}
#endif

	if (__sm_is_small(map)) {
		for (q = 0; q < n; q++) {
			out[q] = __sm_batch_small_select(map, ks[q]);
		}
		return;
	}

	count = __sm_get_chunk_count(map);
	base = count ? __sm_get_chunk_data(map, 0) : NULL;
	p = base;
	stream_end = (size_t)map->m_data_used - SM_SIZEOF_OVERHEAD;
	for (i = 0; i < count && q < n; i++) {
		const __sm_idx_t s = __sm_load_idx((const uint8_t *)p);
		__sm_chunk_t chunk;
		__sm_chunk_rank_t rank;
		size_t cap;
		uint64_t set;
		size_t next_off;
		__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
		cap = __sm_chunk_get_capacity(&chunk);
		set = (uint64_t)__sm_chunk_rank(&rank, true, &chunk, 0,
		    cap - 1);

		/* Every k in [running, running + set) is in this chunk. */
		while (q < n && ks[q] >= running && ks[q] - running < set) {
			ssize_t rem = (ssize_t)(ks[q] - running);
			const size_t at =
			    __sm_chunk_select(&chunk, rem, &rem, true);
			out[q] = (uint64_t)s + at;
			q++;
		}
		running += set;

		next_off = (size_t)(p - base) +
		    SM_SIZEOF_OVERHEAD + __sm_chunk_get_size(&chunk);
		if (next_off >= stream_end) {
			break;
		}
		p = base + next_off;
	}

	/* Every remaining k is at or past the cardinality. */
	for (; q < n; q++) {
		out[q] = SM_IDX_MAX;
	}
}


/* -------------------------------------------------------------------
 * Idea 3: sm_cursor_cached_t -- fixed 8-way MRU chunk cache.
 * ------------------------------------------------------------------- */

bool
sm_contains_cached(const sm_t *map, uint64_t idx, sm_cursor_cached_t *cache)
{
	ssize_t offset;
	uint8_t * p;
	__sm_idx_t start;
	__sm_chunk_t chunk;
	size_t cap;
	uint8_t slot;
	uint8_t w;
	if (map == NULL) {
		return (false);
	}
	if (cache == NULL || __sm_is_small(map)) {
		return (sm_contains(map, idx, NULL));
	}

	/* 1. Probe the <=8 valid ways for a covering chunk (a hit). */
	for (w = 0; w < SM_CACHE_WAYS; w++) {
		if ((cache->valid & (uint8_t)(1u << w)) == 0) {
			continue;
		}
		if (idx >= cache->start_idx[w] && idx < cache->end_idx[w]) {
			uint8_t *p_s =
			    __sm_get_chunk_data(map, cache->offset[w]);
			__sm_chunk_t chunk_s;
			__sm_chunk_init(&chunk_s, p_s + SM_SIZEOF_OVERHEAD);
			return (__sm_chunk_is_set(&chunk_s,
			    idx - cache->start_idx[w]));
		}
	}

	/* 2. Miss: walk from the head, then insert the located chunk. */
	offset = __sm_get_chunk_offset(map, idx, NULL);
	if (offset == -1) {
		return (false);
	}
	p = __sm_get_chunk_data(map, (size_t)offset);
	start = __sm_load_idx((const uint8_t *)p);
	__sm_chunk_init(&chunk, p + SM_SIZEOF_OVERHEAD);
	cap = __sm_chunk_get_capacity(&chunk);

	/* 3. Insert (start, start+cap, offset) at the round-robin slot. */
	slot = cache->mru;
	cache->start_idx[slot] = (uint64_t)start;
	cache->end_idx[slot] = __sm_end(start, cap);
	cache->offset[slot] = (size_t)offset;
	cache->valid |= (uint8_t)(1u << slot);
	cache->mru = (uint8_t)((slot + 1) % SM_CACHE_WAYS);

	/* Out of bounds of the located chunk -> not set (matches
	 * sm_contains). */
	if (idx < (uint64_t)start || idx - (uint64_t)start >= cap) {
		return (false);
	}
	return (__sm_chunk_is_set(&chunk, idx - (uint64_t)start));
}
