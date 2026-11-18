# Sparsemap Performance Analysis

Comparative benchmark results for sparsemap and CRoaring.  All
measurements use p50 (median) latency in nanoseconds unless noted
otherwise.  Cardinality is 10,000 for all patterns.

These numbers were measured on sparsemap 3.0.0 (June 2026).  Later
releases changed several of the paths measured here: the cardinality
count is cached (5.8.0), and point queries, rank and select are faster
(5.9.0; see CHANGELOG.md).  Treat this page as a historical baseline,
not a description of the current release.

An earlier version of this page also compared against PostgreSQL's
`Bitmapset`.  Measured on PostgreSQL workloads (TID sets for bitmap heap
scans, posting lists), sparsemap was neither smaller nor faster than
`Bitmapset`, so that comparison has been removed.

## Test Environment

- **nuc**: FreeBSD 15.0-RELEASE, amd64
- **Compiler**: clang, `-O3 -march=native` (library), `-O2` (bench harness)
- **Commit**: `optimize-set-ops` branch (expand-operate-encode pipeline,
  SIMD word ops, allocation fix)

## Memory Usage (bytes)

| Pattern     | Universe | sparsemap | CRoaring | sm/CR   |
|-------------|----------|-----------|----------|---------|
| dense       | 10,000   | **48**    | 8,209    | 0.006x  |
| block       | 11,000   | **68**    | 8,209    | 0.008x  |
| alternating | 20,096   | **164**   | 8,209    | 0.02x   |
| clustered   | 60,000   | **1,892** | 8,209    | 0.23x   |
| worst       | 20,000   | **2,660** | 8,209    | 0.32x   |
| periodic    | 80,000   | **10,516**| 11,833   | 0.89x   |
| sparse      | 100,000  | **13,080**| 15,123   | 0.87x   |
| powerlaw    | 100,000  | 13,104    | 12,025   | 1.09x   |

Sparsemap uses the least memory in 7 of 8 patterns. RLE encoding of dense
runs produces extreme compression: the dense pattern stores 10,000 bits in
just 48 bytes (a single RLE chunk) versus 8,209 bytes for CRoaring.

## Set Operations (p50 ns)

### Union

| Pattern     | sparsemap | CRoaring | sm vs CR    |
|-------------|-----------|----------|-------------|
| dense       | **92**    | 341      | 3.7x faster |
| block       | **275**   | 342      | 1.2x faster |
| alternating | 1,876     | 341      | 5.5x slower |
| worst       | 2,330     | 341      | 6.8x slower |
| clustered   | 5,409     | 342      | 15.8x slower|
| periodic    | 9,119     | 2,518    | 3.6x slower |
| powerlaw    | 11,277    | 2,596    | 4.3x slower |
| sparse      | 11,437    | 8,384    | 1.4x slower |

### Intersection

| Pattern     | sparsemap | CRoaring | sm vs CR     |
|-------------|-----------|----------|--------------|
| dense       | **94**    | 407      | 4.3x faster  |
| block       | **303**   | 407      | 1.3x faster  |
| alternating | 2,093     | 407      | 5.1x slower  |
| worst       | 2,496     | 407      | 6.1x slower  |
| clustered   | 6,057     | 408      | 14.9x slower |
| periodic    | 9,776     | 916      | 10.7x slower |
| powerlaw    | 12,005    | 912      | 13.2x slower |
| sparse      | 12,059    | 1,314    | 9.2x slower  |

### Difference

| Pattern     | sparsemap | CRoaring | sm vs CR     |
|-------------|-----------|----------|--------------|
| dense       | **131**   | 882      | 6.7x faster  |
| block       | **297**   | 883      | 3.0x faster  |
| worst       | 1,826     | 883      | 2.1x slower  |
| alternating | 1,866     | 883      | 2.1x slower  |
| clustered   | 5,459     | 884      | 6.2x slower  |
| periodic    | 7,059     | 1,440    | 4.9x slower  |
| powerlaw    | 8,618     | 1,432    | 6.0x slower  |
| sparse      | 8,632     | 1,921    | 4.5x slower  |

Sparsemap wins set operations on dense and block patterns where RLE
encoding allows the operations to complete in under 300 ns. For patterns
with many sparse chunks (periodic, powerlaw, sparse), the per-chunk
expand-operate-encode overhead is the bottleneck.

## Iteration and Traversal (p50 ns)

| Pattern     | sparsemap  | CRoaring   | sm vs CR    |
|-------------|------------|------------|-------------|
| alternating | **14,033** | 60,504     | 4.3x faster |
| block       | **14,230** | 59,532     | 4.2x faster |
| dense       | **14,237** | 59,606     | 4.2x faster |
| clustered   | **18,884** | 60,071     | 3.2x faster |
| worst       | **20,517** | 61,724     | 3.0x faster |
| periodic    | **23,435** | 54,467     | 2.3x faster |
| powerlaw    | **35,618** | 56,923     | 1.6x faster |
| sparse      | **41,402** | 58,605     | 1.4x faster |

Sparsemap is the fastest iterator across all patterns, 1.4-4.3x faster
than CRoaring.

## Rank and Select (p50 ns)

### Rank

| Pattern     | sparsemap  | CRoaring | sm vs CR     |
|-------------|------------|----------|--------------|
| dense       | **36**     | 88       | 2.4x faster  |
| block       | **165**    | 96       | 1.7x slower  |
| alternating | 405        | **139**  | 2.9x slower  |
| worst       | 1,242      | **139**  | 8.9x slower  |
| clustered   | 1,791      | **352**  | 5.1x slower  |
| powerlaw    | 3,190      | **313**  | 10.2x slower |
| periodic    | 4,820      | **457**  | 10.5x slower |
| sparse      | 5,986      | **560**  | 10.7x slower |

### Select

| Pattern     | sparsemap  | CRoaring | sm vs CR     |
|-------------|------------|----------|--------------|
| dense       | **28**     | 159      | 5.7x faster  |
| block       | **65**     | 210      | 3.2x faster  |
| alternating | **186**    | 290      | 1.6x faster  |
| clustered   | 2,065      | **796**  | 2.6x slower  |
| worst       | 2,615      | **289**  | 9.0x slower  |
| powerlaw    | 4,269      | **683**  | 6.3x slower  |
| periodic    | 5,195      | **1,021**| 5.1x slower  |
| sparse      | 7,381      | **1,257**| 5.9x slower  |

Dense-pattern rank and select are extremely fast (28-36 ns) because the
single RLE chunk can answer in O(1). Both beat CRoaring by wide
margins. As chunk counts grow, the O(chunks) linear scan dominates.

## Offset (p50 ns)

| Pattern     | sparsemap  | CRoaring   | sm vs CR    |
|-------------|------------|------------|-------------|
| dense       | **347**    | 620        | 1.8x faster |
| block       | 657        | **612**    | 1.1x slower |
| alternating | 1,960      | **611**    | 3.2x slower |
| worst       | 2,885      | **612**    | 4.7x slower |
| clustered   | 6,142      | **613**    | 10.0x slower|
| periodic    | **9,132**  | 10,342     | 1.1x faster |
| powerlaw    | 11,689     | **10,841** | 1.1x slower |
| sparse      | **11,768** | 18,525     | 1.6x faster |

Offset beats CRoaring on dense, periodic, and sparse patterns.

## Min/Max (p50 ns)

| Operation | sparsemap | CRoaring |
|-----------|-----------|----------|
| minimum   | 26-29     | 28-30    |
| maximum   | 26-869    | 14-239   |

Minimum is O(1) for both. Sparsemap's maximum is O(1) for dense (26 ns)
but O(chunks) for sparse (869 ns).

## Contains — All 10,000 Members (p50 ns)

| Pattern     | sparsemap  | CRoaring |
|-------------|------------|----------|
| dense       | 5,365      | 3,111    |
| block       | 14,180     | 3,111    |
| alternating | 37,316     | 3,112    |
| clustered   | 41,851     | 3,132    |
| sparse      | 83,433     | 4,134    |

Contains (point query for all members) is sparsemap's weakest operation
due to the O(chunks) linear scan in `__sm_get_chunk_offset` for each query.

## Populate — Build Map from 10,000 Bits (p50 ns)

| Pattern     | sparsemap    | CRoaring  |
|-------------|--------------|-----------|
| dense       | 387,183      | 93,699    |
| block       | 592,537      | 93,763    |
| alternating | 1,318,552    | 93,724    |
| worst       | 1,317,671    | 93,666    |
| clustered   | 2,210,929    | 93,729    |
| periodic    | 4,019,783    | 101,433   |
| sparse      | 4,899,218    | 103,927   |

Populate is sparsemap's most expensive operation. Each `sm_add`
does an O(chunks) scan plus potential data movement. CRoaring amortizes
insertion much better.

## Cardinality (p50 ns)

| Pattern     | sparsemap | CRoaring |
|-------------|-----------|----------|
| dense       | 38        | **27**   |
| block       | 174       | **27**   |
| alternating | 802       | **27**   |
| worst       | 2,458     | **27**   |
| clustered   | 3,729     | **27**   |
| periodic    | 9,668     | **29**   |
| sparse      | 11,981    | **29**   |

CRoaring stores cardinality as a cached field (O(1)). Sparsemap computes
it by walking all chunks with popcount.

## Summary

### Where sparsemap excels

- **Memory efficiency**: smaller than CRoaring for 7/8 patterns. RLE encoding
  achieves 48-68 bytes for dense/block patterns (100-170x smaller than
  CRoaring).
- **Iteration**: Fastest across all patterns (1.4-4.3x faster than CRoaring).
- **Dense-pattern operations**: Set ops, rank, select, and offset all
  outperform CRoaring on dense and block data.
- **Select on dense/block/alternating**: 1.6-5.7x faster than CRoaring.

### Where sparsemap is competitive

- **Union on sparse data**: 1.4x slower than CRoaring (11.4k vs 8.4k ns).
- **Offset on periodic/sparse**: Faster than CRoaring.
- **Minimum**: Tied with CRoaring at ~28 ns.

### Where sparsemap needs improvement

- **Populate**: 4-48x slower than CRoaring due to O(chunks) linear scan
  per insertion.
- **Contains**: 2-20x slower due to same O(chunks) scan.
- **Intersection/difference on sparse data**: 4-15x slower than CRoaring.
- **Cardinality**: no cached count in 3.0.0; since 5.8.0 the count is
  cached and sm_cardinality is O(1) after the first call.

### Architectural trade-offs

Sparsemap's compact variable-length encoding (4-byte offset + 64-bit
descriptor + variable vectors per chunk, or compact RLE) gives it
excellent memory efficiency and iteration speed. The trade-off is that
random access requires an O(chunks) linear scan, making point operations
slower as chunk count grows. CRoaring's fixed-size containers with a
sorted top-level index give it O(log N) random access at the cost of
higher memory overhead.

For workloads dominated by bulk operations (iteration, set algebra on
dense data) and memory-constrained environments, sparsemap is a strong
choice. For workloads with frequent point queries or mutations on sparse
data, CRoaring is faster.
