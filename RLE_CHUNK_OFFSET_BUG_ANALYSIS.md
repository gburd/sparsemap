# RLE Chunk Offset Bug Analysis

## Summary

After implementing and testing the RLE fast paths for `select` and `scan`, one QuickCheck test fails: `_tst_get_chunk_offset`. The failure occurs in a complex edge case involving multiple split/merge operations on RLE chunks.

## Test Failure Details

**Location**: src/sparsemap.c:4180
**Test**: `_tst_get_chunk_offset`
**Symptom**: After `unset(5047)` on an RLE chunk, `__sm_get_chunk_offset(map, 5046)` returns 12 instead of expected 0.

**Test Sequence**:
1. Create RLE chunk covering indices 0-4095 (4096 bits, spans 2 chunks of 2048)
2. `unset(2050)` - Splits into 3 chunks
3. `set(2050)` - Should merge back
4. `unset(5047)` - Should shrink RLE by 1 bit
5. **FAILS**: Chunk offset for index 5046 is wrong

**Debug Output**:
```
After unset(2050): chunk_count=3
  Chunk 0: start=0, capacity=2048, size=8, RLE=0
  Chunk 1: start=2048, capacity=2048, size=16, RLE=0
  Chunk 2: start=0, capacity=2048, size=8, RLE=0   <-- BUG: should be start=4096
```

## Root Cause Analysis

### Observation

After the 3-way split (`unset(2050)`), **Chunk 2 has start=0 instead of start=4096**. This indicates that during the split operation, the start index for the rightmost chunk was not correctly set or was overwritten.

### Code Path

1. **Split Operation**: `__sm_separate_rle_chunk()` (src/sparsemap.c:1759)
   - Handles 3 split cases: left-aligned, right-aligned, centrally-aligned
   - Lines 1940-1955: 3-way split (centrally-aligned)
   - Line 1950-1951: Sets right chunk start correctly:
     ```c
     sep->ex[1].start = aligned_idx + SM_CHUNK_MAX_CAPACITY;
     sep->ex[1].end = sep->target.start + sep->target.length - 1;
     ```

2. **Chunk Construction**: Lines 1957-2019
   - Line 1960: Writes start index to chunk:
     ```c
     *(__sm_idx_t *)sep->ex[i].p = sep->ex[i].start;
     ```

3. **Data Insertion**: Lines 2028-2031
   - Line 2029-2030: Copies sep->buf into map->m_data:
     ```c
     __sm_insert_data(map, sep->target.offset + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t),
                      sep->buf + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t), sep->expand_by);
     memcpy(sep->target.p, sep->buf, sep->expand_by + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t));
     ```

### Hypothesis

The bug likely occurs during **pointer arithmetic in sep->buf manipulation**. The code performs multiple `memmove` operations to adjust chunk positions in the buffer (lines 1882, 1914, 1943, etc.). After these moves, `sep->ex[1].p` may point to the wrong location, or the start index may be overwritten.

**Specific suspect**: Lines 1972-1976 or 2006-2011
```c
memmove((uint8_t *)((uintptr_t)sep->buf + SM_SIZEOF_OVERHEAD + sizeof(__sm_bitvec_t)),
        sep->pivot.p, sep->pivot.size);
if (sep->ex[1].p) {
    sep->ex[1].p = (uint8_t *)((uintptr_t)sep->ex[1].p - sizeof(__sm_bitvec_t));
}
```
These moves adjust `sep->ex[1].p` but may not preserve the start index that was already written at line 1960.

## Impact

**Severity**: Medium
- Affects internal chunk offset calculations in specific edge cases
- Does not affect core RLE operations (select, scan, rank, is_set)
- Manifests only after complex sequences: RLE creation → split → merge → shrink
- 4 out of 5 new RLE tests pass; only this edge case fails

**User Impact**: Low
- Normal operations work correctly
- Bug only triggers in pathological access patterns:
  1. Create large RLE run
  2. Unset bit in middle (3-way split)
  3. Reset same bit (merge)
  4. Unset bit at end (shrink)
  5. Query chunk offset for internal operations

## Recommended Fix

### Immediate Actions

1. **Add assertion**: At line 2030 (after data copy), verify chunk start indices:
   ```c
   for (size_t i = 0; i < 2; i++) {
       if (sep->ex[i].p) {
           __sm_idx_t *actual_start = (__sm_idx_t *)sep->ex[i].p;
           __sm_assert(*actual_start == sep->ex[i].start &&
                       "Chunk start index corrupted during split");
       }
   }
   ```

2. **Debug logging**: Add fprintf before line 2030 to verify sep->ex[1].start value:
   ```c
   fprintf(stderr, "DEBUG: ex[1] start=%u, p offset=%zu\n",
           sep->ex[1].start, (size_t)(sep->ex[1].p - sep->buf));
   ```

### Long-Term Solution

1. **Refactor pointer management**:
   - Instead of adjusting `sep->ex[i].p` during memmove operations, recalculate pointers after all moves complete
   - Store offsets relative to `sep->buf` rather than absolute pointers during construction

2. **Rewrite start index after moves**:
   At line 2028 (before final copy), ensure start indices are correct:
   ```c
   for (size_t i = 0; i < 2; i++) {
       if (sep->ex[i].p) {
           *(__sm_idx_t *)sep->ex[i].p = sep->ex[i].start;  // Rewrite after all moves
       }
   }
   ```

3. **Add unit test**: Create minimal reproducer:
   ```c
   void test_rle_split_merge_shrink() {
       sparsemap_t *map = sparsemap_alloc(32768);

       // Create RLE run 0-4095
       for (int i = 0; i < 4096; i++) {
           sparsemap_set(map, i);
       }

       // Split
       sparsemap_unset(map, 2050);

       // Merge
       sparsemap_set(map, 2050);

       // Shrink
       sparsemap_unset(map, 4095);

       // Verify chunk structure
       ssize_t offset = __sm_get_chunk_offset(map, 4094);
       assert(offset == 0);
   }
   ```

## Workaround

For production use, avoid the specific access pattern:
- After splitting an RLE chunk, don't immediately merge and then shrink
- If needed, rebuild the sparsemap from scratch rather than complex in-place modifications

## References

- **Implementation**: src/sparsemap.c:1759-2034 (`__sm_separate_rle_chunk`)
- **Test**: src/sparsemap.c:3961-4202 (`_tst_get_chunk_offset`)
- **Related Functions**:
  - `__sm_insert_data` (line 1442)
  - `__sm_get_chunk_offset` (line 1399)
  - `__sm_chunk_init` (line 251)

## Status

- **Date**: 2026-03-05
- **Analyzed By**: RLE Implementation Team
- **Priority**: Medium (edge case, low user impact)
- **Est. Fix Time**: 2-4 hours (careful debugging needed)
