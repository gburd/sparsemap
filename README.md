# Sparsemap

A sparse, compressed bitmap library optimized for consecutive runs of set or unset bits.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

## Features

* **Highly Compressed**: Best case stores 2048 bits in just 8 bytes
* **RLE Encoding**: Efficient run-length encoding for long sequences of set bits
* **Fast Operations**: O(1) select for RLE chunks, efficient scan with batch processing
* **Flexible**: Worst case is uncompressed (2048 bits + 8 bytes overhead)
* **Production Ready**: Comprehensive test suite, Autoconf build system, MIT license

## Overview

Bitsets (bitmaps) are commonly used as fast data structures, but can use too much memory. Sparsemap is a sparse, compressed bitmap that excels when working with consecutive sequences of set or unset bits.

For consecutive 64-bit integers, sparsemap can compress up to 16KB into just 8 bytes!

## Quick Start

### Building from Source

```bash
# Generate build system (if building from git)
./autogen.sh

# Configure
./configure

# Build
make

# Run tests
make check

# Install
sudo make install
```

### Configuration Options

```bash
# Install to custom location
./configure --prefix=/usr/local

# Enable debug mode
./configure --enable-debug

# Enable diagnostic output
./configure --enable-diagnostic

# Disable building tests
./configure --disable-testing
```

## Usage

### Using in Your Project

#### Option 1: Use Installed Library

```bash
# Link against installed library
gcc -o myapp myapp.c -lsparsemap

# With pkg-config
gcc -o myapp myapp.c $(pkg-config --cflags --libs sparsemap)
```

#### Option 2: Copy Source Files

Copy `src/sparsemap.c` and `include/sparsemap.h` into your project and compile directly.

### API Example

There are two ways to create a sparsemap: heap-managed or caller-managed.

**Heap-managed** (single allocation, resizable):

```c
#include <sparsemap.h>

// Create a heap-managed sparsemap (struct + buffer in one allocation)
sparsemap_t *map = sparsemap(4096);

// Set bits
sparsemap_set(map, 100);
sparsemap_set(map, 200);

// Check if bit is set
bool is_set = sparsemap_is_set(map, 100);  // true

// Count set bits
size_t count = sparsemap_count(map);  // 2

// Select nth set bit (0-based)
sparsemap_idx_t pos = sparsemap_select(map, 0, true);  // 100

// Count set bits in a range [100, 200]
size_t rank = sparsemap_rank(map, 100, 200, true);  // 2

// Scan all set bits
void print_bits(uint32_t indices[], size_t n, void *aux) {
    for (size_t i = 0; i < n; i++)
        printf("Bit %u is set\n", indices[i]);
}
sparsemap_scan(map, print_bits, 0, NULL);

// Clean up (single free for struct + buffer)
free(map);
```

**Caller-managed** (stack or custom allocation):

```c
#include <sparsemap.h>

// Both struct and buffer on the stack
sparsemap_t map;
uint8_t buffer[1024];
sparsemap_init(&map, buffer, sizeof(buffer));

sparsemap_set(&map, 42);
assert(sparsemap_is_set(&map, 42));
// No free needed -- everything is on the stack
```

### Split and Merge

```c
sparsemap_t *left = sparsemap(4096);
sparsemap_t *right = sparsemap(4096);

// Populate left with bits 0-9999
for (size_t i = 0; i < 10000; i++)
    sparsemap_set(left, i);

// Split evenly by cardinality
sparsemap_split(left, SPARSEMAP_IDX_MAX, right);
// left  ~ bits [0, 5000)
// right ~ bits [5000, 10000)

// Merge right back into left
sparsemap_merge(left, right);

free(right);
free(left);
```

### Handling ENOSPC

```c
sparsemap_t *map = sparsemap(128);  // small buffer

sparsemap_idx_t r = sparsemap_set(map, 42);
if (SPARSEMAP_NOT_FOUND(r)) {
    // Buffer full -- grow and retry
    map = sparsemap_set_data_size(map, NULL, 4096);
    sparsemap_set(map, 42);
}

free(map);
```

## How It Works

Sparsemap uses a 3-tier hierarchical architecture with two encoding schemes:

### 1. Sparse Encoding (2-bit flags)

Each chunk has a 64-bit descriptor with 2-bit flags for up to 32 bit-vectors (2048 bits total):

| Flag | Meaning | Storage |
|------|---------|---------|
| `00` | all zeros | vector not stored |
| `11` | all ones | vector not stored |
| `10` | mixed bits | 64-bit vector stored after descriptor |
| `01` | unused | reduces chunk capacity |

A sparse chunk takes a minimum of 8 bytes (descriptor only, all vectors uniform) and a maximum of 264 bytes (descriptor + 32 mixed vectors).

### 2. RLE Encoding (Run-Length)

When more than 2048 consecutive bits are set, adjacent chunks coalesce into a
single RLE chunk.  The entire run is represented by one 64-bit descriptor:

```
Bits 63:62 = 01  (RLE marker -- same bit pattern as "unused" flag)
Bits 61:31 = capacity in bits  (31 bits, max ~2 billion)
Bits 30:0  = length in bits    (31 bits, max ~2 billion)
```

Bits `[0, length)` within the chunk are set; bits `[length, capacity)` are unset.
This compresses up to 2^31 consecutive set bits into 12 bytes (4-byte offset + 8-byte descriptor).

#### When does RLE activate?

RLE is **not** triggered simply by setting consecutive bits.  The sequence is:

1. A sparse chunk fills all 32 vectors with ones (2048 bits).
2. Setting the next adjacent bit (bit 2049) causes the two chunks to coalesce.
3. The coalesced result exceeds sparse capacity, so it becomes an RLE chunk.

Setting 1000 consecutive bits uses sparse encoding (with `11` flags).  Setting
3000+ consecutive bits triggers the transition to RLE.

#### What happens when an RLE chunk is modified?

- **Clearing the last bit** in the run shortens the length by one.
- **Clearing a bit in the middle** separates the RLE chunk into up to three
  pieces: an RLE chunk for the left run, a sparse chunk containing the gap,
  and an RLE chunk for the right run.
- **Setting a bit beyond the run** (within capacity) extends the length.

These transitions are automatic and transparent to the caller.

### Chunk Organization

- Each chunk is prefixed with a 4-byte starting offset (`uint32_t`).
- Chunks are stored consecutively in the buffer in ascending offset order.
- The first 4 bytes of the buffer hold the chunk count.
- Adjacent chunks that form contiguous runs are coalesced automatically.

### Serialization

The raw buffer (from `sparsemap_get_data()`, first `sparsemap_get_size()` bytes)
is the serialized format.  To restore:

```c
// Save
size_t sz = sparsemap_get_size(map);
void *blob = sparsemap_get_data(map);
write(fd, blob, sz);

// Restore
sparsemap_t restored;
uint8_t buf[capacity];
read(fd, buf, sz);
sparsemap_open(&restored, buf, capacity);
```

## Performance

### Best Case
- **Consecutive set bits**: 16KB compressed to 8 bytes
- **Consecutive unset bits**: No storage required
- **Select operation**: O(1) for RLE chunks
- **Scan operation**: Batch processing in groups of 64

### Worst Case
- **Random scattered bits**: Same as uncompressed bitmap + 8 bytes overhead
- For such patterns, consider [Roaring Bitmaps](https://github.com/RoaringBitmap/CRoaring)

## Thread Safety

Sparsemap is **not** thread-safe.  Concurrent reads are safe only when no
writer is active.  All mutating operations (`set`, `unset`, `assign`, `merge`,
`split`, `clear`) must be externally synchronized.

## Directory Structure

```
sparsemap/
├── src/              # Library source code
├── include/          # Public header files
├── tests/            # Test suite
├── examples/         # Example programs
├── doc/              # Documentation
├── m4/               # Autoconf macros
└── build-aux/        # Build system auxiliary files
```

## Testing

```bash
# Run all tests
make check

# Run specific test
./tests/test

# Run RLE standalone test
./tests/test_rle_standalone

# Run soak test (comprehensive, slow)
./tests/soak
```

## Documentation

- [RLE Implementation Summary](doc/RLE_IMPLEMENTATION_SUMMARY.md) - Detailed implementation notes
- See `examples/` for usage examples
- API documentation in `include/sparsemap.h`

## Requirements

- C11 compiler (GCC 4.9+, Clang 3.4+, or compatible)
- POSIX-compatible system
- pthread library

For building from git:
- autoconf 2.69+
- automake 1.15+
- libtool 2.4+

## History

Originally created by [Christoph Rupp](https://crupp.de) in C++ (2014), then translated to C and improved by Gregory Burd for use in LMDB and OpenLDAP.

Version 1.0.0 (March 2026) includes:
- Complete RLE implementation with select/scan operations
- Autoconf/Automake build system
- Comprehensive test coverage
- Production-ready code quality

## License

MIT License - see [LICENSE](LICENSE) file for details.

Copyright (c) 2014 Christoph Rupp
Copyright (c) 2024-2026 Gregory Burd

## Contributing

Contributions welcome! Please:
1. Fork the repository
2. Create a feature branch
3. Add tests for new functionality
4. Ensure `make check` passes
5. Submit a pull request

## References

- Roaring Bitmaps: https://roaringbitmap.org/
- Daniel Lemire's Blog: http://lemire.me/blog/
- LMDB: https://www.symas.com/lmdb

## Contact

Gregory Burd <greg@burd.me>

Report issues: https://github.com/gburd/sparsemap/issues
