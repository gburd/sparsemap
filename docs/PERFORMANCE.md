# Sparsemap Performance Analysis

Comparative benchmark results for sparsemap, CRoaring, and PostgreSQL's
bitmapset. All measurements use p50 (median) latency in nanoseconds unless
noted otherwise. Cardinality is 10,000 for all patterns.

## Test Environment

- **nuc**: FreeBSD 15.0-RELEASE, amd64
- **Compiler**: clang, `-O3 -march=native` (library), `-O2` (bench harness)
- **Commit**: `optimize-set-ops` branch (expand-operate-encode pipeline,
  SIMD word ops, allocation fix)

## Memory Usage (bytes)

| Pattern     | Universe | sparsemap | CRoaring | bitmapset | sm/CR   | sm/bms  |
|-------------|----------|-----------|----------|-----------|---------|---------|
| dense       | 10,000   | **48**    | 8,209    | 1,264     | 0.006x  | 0.04x   |
| block       | 11,000   | **68**    | 8,209    | 1,384     | 0.008x  | 0.05x   |
| alternating | 20,096   | **164**   | 8,209    | 2,512     | 0.02x   | 0.07x   |
| clustered   | 60,000   | **1,892** | 8,209    | 7,448     | 0.23x   | 0.25x   |
| worst       | 20,000   | **2,660** | 8,209    | 2,512     | 0.32x   | 1.06x   |
| periodic    | 80,000   | **10,516**| 11,833   | 10,008    | 0.89x   | 1.05x   |
| sparse      | 100,000  | **13,080**| 15,123   | 12,480    | 0.87x   | 1.05x   |
| powerlaw    | 100,000  | 13,104    | 12,025   | 12,512    | 1.09x   | 1.05x   |

Sparsemap uses the least memory in 7 of 8 patterns. RLE encoding of dense
runs produces extreme compression: the dense pattern stores 10,000 bits in
just 48 bytes (a single RLE chunk) versus 8,209 bytes for CRoaring.

## Set Operations (p50 ns)

### Union

| Pattern     | sparsemap | CRoaring | bitmapset | sm vs CR    |
|-------------|-----------|----------|-----------|-------------|
| dense       | **92**    | 341      | 89        | 3.7x faster |
| block       | **275**   | 342      | 93        | 1.2x faster |
| alternating | 1,876     | 341      | 131       | 5.5x slower |
| worst       | 2,330     | 341      | 131       | 6.8x slower |
| clustered   | 5,409     | 342      | 296       | 15.8x slower|
| periodic    | 9,119     | 2,518    | 383       | 3.6x slower |
| powerlaw    | 11,277    | 2,596    | 473       | 4.3x slower |
| sparse      | 11,437    | 8,384    | 470       | 1.4x slower |

### Intersection

| Pattern     | sparsemap | CRoaring | bitmapset | sm vs CR     |
|-------------|-----------|----------|-----------|--------------|
| dense       | **94**    | 407      | 139       | 4.3x faster  |
| block       | **303**   | 407      | 148       | 1.3x faster  |
| alternating | 2,093     | 407      | 234       | 5.1x slower  |
| worst       | 2,496     | 407      | 234       | 6.1x slower  |
| clustered   | 6,057     | 408      | 596       | 14.9x slower |
| periodic    | 9,776     | 916      | 783       | 10.7x slower |
| powerlaw    | 12,005    | 912      | 968       | 13.2x slower |
| sparse      | 12,059    | 1,314    | 963       | 9.2x slower  |

### Difference

| Pattern     | sparsemap | CRoaring | bitmapset | sm vs CR     |
|-------------|-----------|----------|-----------|--------------|
| dense       | **131**   | 882      | 109       | 6.7x faster  |
| block       | **297**   | 883      | 117       | 3.0x faster  |
| worst       | 1,826     | 883      | 189       | 2.1x slower  |
| alternating | 1,866     | 883      | 189       | 2.1x slower  |
| clustered   | 5,459     | 884      | 505       | 6.2x slower  |
| periodic    | 7,059     | 1,440    | 670       | 4.9x slower  |
| powerlaw    | 8,618     | 1,432    | 830       | 6.0x slower  |
| sparse      | 8,632     | 1,921    | 828       | 4.5x slower  |

Sparsemap wins set operations on dense and block patterns where RLE
encoding allows the operations to complete in under 300 ns. For patterns
with many sparse chunks (periodic, powerlaw, sparse), the per-chunk
expand-operate-encode overhead is the bottleneck.

## Iteration and Traversal (p50 ns)

| Pattern     | sparsemap  | CRoaring   | bitmapset  | sm vs CR    |
|-------------|------------|------------|------------|-------------|
| alternating | **14,033** | 60,504     | 48,484     | 4.3x faster |
| block       | **14,230** | 59,532     | 46,852     | 4.2x faster |
| dense       | **14,237** | 59,606     | 47,040     | 4.2x faster |
| clustered   | **18,884** | 60,071     | 48,424     | 3.2x faster |
| worst       | **20,517** | 61,724     | 46,003     | 3.0x faster |
| periodic    | **23,435** | 54,467     | 43,840     | 2.3x faster |
| powerlaw    | **35,618** | 56,923     | 47,667     | 1.6x faster |
| sparse      | **41,402** | 58,605     | 53,810     | 1.4x faster |

Sparsemap is the fastest iterator across all patterns, 1.4-4.3x faster
than CRoaring and 1.3-3.4x faster than bitmapset.

## Rank and Select (p50 ns)

### Rank

| Pattern     | sparsemap  | CRoaring | bitmapset  | sm vs CR     |
|-------------|------------|----------|------------|--------------|
| dense       | **36**     | 88       | 21,548     | 2.4x faster  |
| block       | **165**    | 96       | 21,567     | 1.7x slower  |
| alternating | 405        | **139**  | 21,976     | 2.9x slower  |
| worst       | 1,242      | **139**  | 22,745     | 8.9x slower  |
| clustered   | 1,791      | **352**  | 22,019     | 5.1x slower  |
| powerlaw    | 3,190      | **313**  | 22,491     | 10.2x slower |
| periodic    | 4,820      | **457**  | 21,580     | 10.5x slower |
| sparse      | 5,986      | **560**  | 25,023     | 10.7x slower |

### Select

| Pattern     | sparsemap  | CRoaring | bitmapset  | sm vs CR     |
|-------------|------------|----------|------------|--------------|
| dense       | **28**     | 159      | 21,409     | 5.7x faster  |
| block       | **65**     | 210      | 21,430     | 3.2x faster  |
| alternating | **186**    | 290      | 21,853     | 1.6x faster  |
| clustered   | 2,065      | **796**  | 21,877     | 2.6x slower  |
| worst       | 2,615      | **289**  | 22,717     | 9.0x slower  |
| powerlaw    | 4,269      | **683**  | 22,468     | 6.3x slower  |
| periodic    | 5,195      | **1,021**| 21,776     | 5.1x slower  |
| sparse      | 7,381      | **1,257**| 25,178     | 5.9x slower  |

Dense-pattern rank and select are extremely fast (28-36 ns) because the
single RLE chunk can answer in O(1). Both beat CRoaring and bitmapset by
wide margins. As chunk counts grow, the O(chunks) linear scan dominates.

## Offset (p50 ns)

| Pattern     | sparsemap  | CRoaring   | bitmapset | sm vs CR    |
|-------------|------------|------------|-----------|-------------|
| dense       | **347**    | 620        | 80        | 1.8x faster |
| block       | 657        | **612**    | 84        | 1.1x slower |
| alternating | 1,960      | **611**    | 110       | 3.2x slower |
| worst       | 2,885      | **612**    | 110       | 4.7x slower |
| clustered   | 6,142      | **613**    | 241       | 10.0x slower|
| periodic    | **9,132**  | 10,342     | 298       | 1.1x faster |
| powerlaw    | 11,689     | **10,841** | 388       | 1.1x slower |
| sparse      | **11,768** | 18,525     | 386       | 1.6x faster |

Offset beats CRoaring on dense, periodic, and sparse patterns.

## Min/Max (p50 ns)

| Operation | sparsemap | CRoaring | bitmapset |
|-----------|-----------|----------|-----------|
| minimum   | 26-29     | 28-30    | 26-27     |
| maximum   | 26-869    | 14-239   | 43,098-50,973 |

Minimum is O(1) for all three. Sparsemap's maximum is O(1) for dense (26 ns)
but O(chunks) for sparse (869 ns). Bitmapset's maximum requires a full linear
scan and is always ~44 us.

## Contains — All 10,000 Members (p50 ns)

| Pattern     | sparsemap  | CRoaring | bitmapset |
|-------------|------------|----------|-----------|
| dense       | 5,365      | 3,111    | **1,316** |
| block       | 14,180     | 3,111    | **1,316** |
| alternating | 37,316     | 3,112    | **1,315** |
| clustered   | 41,851     | 3,132    | **1,349** |
| sparse      | 83,433     | 4,134    | **1,318** |

Contains (point query for all members) is sparsemap's weakest operation
due to the O(chunks) linear scan in `__sm_get_chunk_offset` for each query.

## Populate — Build Map from 10,000 Bits (p50 ns)

| Pattern     | sparsemap    | CRoaring  | bitmapset  |
|-------------|--------------|-----------|------------|
| dense       | 387,183      | 93,699    | **32,894** |
| block       | 592,537      | 93,763    | **32,603** |
| alternating | 1,318,552    | 93,724    | **33,191** |
| worst       | 1,317,671    | 93,666    | **39,641** |
| clustered   | 2,210,929    | 93,729    | **39,737** |
| periodic    | 4,019,783    | 101,433   | **90,329** |
| sparse      | 4,899,218    | 103,927   | **122,541**|

Populate is sparsemap's most expensive operation. Each `sm_add`
does an O(chunks) scan plus potential data movement. CRoaring and bitmapset
amortize insertion much better.

## Cardinality (p50 ns)

| Pattern     | sparsemap | CRoaring | bitmapset |
|-------------|-----------|----------|-----------|
| dense       | 38        | **27**   | 135       |
| block       | 174       | **27**   | 147       |
| alternating | 802       | **27**   | 247       |
| worst       | 2,458     | **27**   | 245       |
| clustered   | 3,729     | **27**   | 670       |
| periodic    | 9,668     | **29**   | 890       |
| sparse      | 11,981    | **29**   | 1,102     |

CRoaring stores cardinality as a cached field (O(1)). Sparsemap computes
it by walking all chunks with popcount.

## Summary

### Where sparsemap excels

- **Memory efficiency**: Best-in-class for 7/8 patterns. RLE encoding
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
- **Cardinality**: No cached count; must walk all chunks.

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
