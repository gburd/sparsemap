/*-------------------------------------------------------------------------
 *
 * bench_hybrid_wrapper.c
 *	  Unity-build wrapper that compiles bitmapset_hybrid.c with renamed
 *	  function symbols so they do not collide with bitmapset_standalone.o
 *	  at link time.
 *
 *	  The public API is exposed as hybrid_bms_*() functions that bench.c
 *	  calls through the opaque HybridBitmapset handle.
 *
 *	  Strategy: we #define every public bms_* function name to a
 *	  __hybrid_bms_* name *before* including the header and source.
 *	  Type and macro names are also renamed to avoid conflicts.
 *	  The header's own macro redefinitions are harmless since we only
 *	  include the hybrid header here (never the standalone one).
 *
 *-------------------------------------------------------------------------
 */

/*
 * Rename type names that would clash at the ODR / struct-tag level.
 */
#define Bitmapset          __HybridBitmapset
#define bitmapword         __hybrid_bitmapword
#define signedbitmapword   __hybrid_signedbitmapword
#define BMS_Comparison     __HybridBMS_Comparison
#define BMS_Membership     __HybridBMS_Membership

/* Rename enum constants */
#define BMS_EQUAL          __HYBRID_BMS_EQUAL
#define BMS_SUBSET1        __HYBRID_BMS_SUBSET1
#define BMS_SUBSET2        __HYBRID_BMS_SUBSET2
#define BMS_DIFFERENT      __HYBRID_BMS_DIFFERENT
#define BMS_EMPTY_SET      __HYBRID_BMS_EMPTY_SET
#define BMS_SINGLETON      __HYBRID_BMS_SINGLETON
#define BMS_MULTIPLE       __HYBRID_BMS_MULTIPLE

/* Rename every public function symbol */
#define bms_copy             __hybrid_bms_copy
#define bms_equal            __hybrid_bms_equal
#define bms_compare          __hybrid_bms_compare
#define bms_make_singleton   __hybrid_bms_make_singleton
#define bms_free             __hybrid_bms_free
#define bms_union            __hybrid_bms_union
#define bms_intersect        __hybrid_bms_intersect
#define bms_difference       __hybrid_bms_difference
#define bms_offset_members   __hybrid_bms_offset_members
#define bms_is_subset        __hybrid_bms_is_subset
#define bms_subset_compare   __hybrid_bms_subset_compare
#define bms_is_member        __hybrid_bms_is_member
#define bms_member_index     __hybrid_bms_member_index
#define bms_overlap          __hybrid_bms_overlap
#define bms_nonempty_difference __hybrid_bms_nonempty_difference
#define bms_singleton_member __hybrid_bms_singleton_member
#define bms_get_singleton_member __hybrid_bms_get_singleton_member
#define bms_num_members      __hybrid_bms_num_members
#define bms_membership       __hybrid_bms_membership
#define bms_add_member       __hybrid_bms_add_member
#define bms_del_member       __hybrid_bms_del_member
#define bms_add_members      __hybrid_bms_add_members
#define bms_replace_members  __hybrid_bms_replace_members
#define bms_add_range        __hybrid_bms_add_range
#define bms_int_members      __hybrid_bms_int_members
#define bms_del_members      __hybrid_bms_del_members
#define bms_join             __hybrid_bms_join
#define bms_next_member      __hybrid_bms_next_member
#define bms_prev_member      __hybrid_bms_prev_member
#define bms_hash_value       __hybrid_bms_hash_value

/*
 * bms_is_empty is a function-like macro in the header; we do NOT rename it
 * because it expands to ((a) == NULL) and does not produce a linker symbol.
 * The header will define it after our #include and it just works.
 */

/*
 * Include the hybrid header + source. All bms_* symbols inside will
 * be compiled with the __hybrid_bms_* names defined above. The macros
 * (BMS_IS_CHUNKED, BMS_WORDS, etc.) are NOT renamed -- they are local
 * to this translation unit and do not appear at link time.
 */
#ifndef BUILDING_OUTSIDE_POSTGRES
#define BUILDING_OUTSIDE_POSTGRES
#endif
#include "bitmapset.h"
#include "bitmapset.c"

/*
 * HybridBitmapset is the public name for the renamed struct type.
 * We keep the #define Bitmapset -> __HybridBitmapset active so that
 * macros like BITMAPSET_SIZE still resolve correctly.
 */
typedef __HybridBitmapset HybridBitmapset;

/*
 * Public wrapper functions that bench.c calls.
 * These delegate to the renamed __hybrid_bms_* implementations.
 */

HybridBitmapset *
hybrid_bms_add_member(HybridBitmapset *a, int x)
{
	return __hybrid_bms_add_member(a, x);
}

void
hybrid_bms_free(HybridBitmapset *a)
{
	__hybrid_bms_free(a);
}

bool
hybrid_bms_is_member(int x, const HybridBitmapset *a)
{
	return __hybrid_bms_is_member(x, a);
}

int
hybrid_bms_num_members(const HybridBitmapset *a)
{
	return __hybrid_bms_num_members(a);
}

HybridBitmapset *
hybrid_bms_union(const HybridBitmapset *a, const HybridBitmapset *b)
{
	return __hybrid_bms_union(a, b);
}

HybridBitmapset *
hybrid_bms_intersect(const HybridBitmapset *a, const HybridBitmapset *b)
{
	return __hybrid_bms_intersect(a, b);
}

HybridBitmapset *
hybrid_bms_difference(const HybridBitmapset *a, const HybridBitmapset *b)
{
	return __hybrid_bms_difference(a, b);
}

int
hybrid_bms_next_member(const HybridBitmapset *a, int prevbit)
{
	return __hybrid_bms_next_member(a, prevbit);
}

HybridBitmapset *
hybrid_bms_offset_members(const HybridBitmapset *a, int offset)
{
	return __hybrid_bms_offset_members(a, offset);
}

HybridBitmapset *
hybrid_bms_copy(const HybridBitmapset *a)
{
	return __hybrid_bms_copy(a);
}

/*
 * hybrid_bms_memory_bytes - report the total heap bytes used by a hybrid bitmapset.
 *
 * Dense mode:  offsetof(HybridBitmapset, data) + nwords * 8
 * Chunked mode: offsetof(HybridBitmapset, data) + alloc_chunks * 64
 * NULL: 0
 */
size_t
hybrid_bms_memory_bytes(const HybridBitmapset *a)
{
	if (a == NULL)
		return 0;

	if (BMS_IS_CHUNKED(a))
	{
		return offsetof(HybridBitmapset, data) +
			(size_t)BMS_ALLOC_CHUNKS(a) * 64;
	}

	return BITMAPSET_SIZE(a->nwords);
}
