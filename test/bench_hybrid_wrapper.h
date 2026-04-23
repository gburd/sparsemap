/*-------------------------------------------------------------------------
 *
 * bench_hybrid_wrapper.h
 *	  Wrapper providing hybrid bitmapset functions with hybrid_ prefix
 *	  so bench.c can include both bitmapset_standalone.h and this header
 *	  without symbol conflicts.
 *
 *-------------------------------------------------------------------------
 */
#ifndef BENCH_HYBRID_WRAPPER_H
#define BENCH_HYBRID_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Opaque handle -- the actual struct lives in bench_hybrid_wrapper.c */
typedef struct HybridBitmapset HybridBitmapset;

/* Wrapper functions with hybrid_ prefix */
HybridBitmapset *hybrid_bms_add_member(HybridBitmapset *a, int64_t x);
void hybrid_bms_free(HybridBitmapset *a);
bool hybrid_bms_is_member(int64_t x, const HybridBitmapset *a);
int64_t hybrid_bms_num_members(const HybridBitmapset *a);
HybridBitmapset *hybrid_bms_union(const HybridBitmapset *a, const HybridBitmapset *b);
HybridBitmapset *hybrid_bms_intersect(const HybridBitmapset *a, const HybridBitmapset *b);
HybridBitmapset *hybrid_bms_difference(const HybridBitmapset *a, const HybridBitmapset *b);
int64_t hybrid_bms_next_member(const HybridBitmapset *a, int64_t prevbit);
HybridBitmapset *hybrid_bms_offset_members(const HybridBitmapset *a, int64_t offset);
HybridBitmapset *hybrid_bms_copy(const HybridBitmapset *a);

/* Memory size reporting */
size_t hybrid_bms_memory_bytes(const HybridBitmapset *a);

#endif							/* BENCH_HYBRID_WRAPPER_H */
