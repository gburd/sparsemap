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

```c
#include <sparsemap.h>

// Create a sparsemap
uint8_t buffer[1024];
sparsemap_t *map = sparsemap(0);
sparsemap_init(map, buffer, 1024);

// Set bits
sparsemap_set(map, 100);
sparsemap_set(map, 200);

// Check if bit is set
bool is_set = sparsemap_is_set(map, 100);  // true

// Count set bits
size_t count = sparsemap_count(map);  // 2

// Select nth set bit
size_t pos = sparsemap_select(map, 0, true);  // 100

// Scan all set bits
void callback(uint32_t indices[], size_t n, void *aux) {
    for (size_t i = 0; i < n; i++) {
        printf("Bit %u is set\n", indices[i]);
    }
}
sparsemap_scan(map, callback, 0, NULL);

// Clean up
free(map);
```

## How It Works

Sparsemap uses a 3-tier hierarchical architecture with two encoding schemes:

### 1. Sparse Encoding (2-bit flags)

Each chunk has a descriptor with 2-bit flags indicating the state of each 64-bit vector:
- `00` = all zeros (not stored)
- `11` = all ones (not stored)
- `10` = mixed bits (vector stored)
- `01` = unused/reduced capacity

Example: Instead of storing 16 bytes, only 2 bytes needed:
```
Descriptor: 00 00 00 00 11 00 11 10
Memory:     0000000011001110 0110010101111001
```

### 2. RLE Encoding (Run-Length)

For long runs of consecutive set bits (>2048), a single 64-bit descriptor stores:
- Bits 63:62 = `01` (RLE flag)
- Bits 61:31 = capacity (31 bits)
- Bits 30:0 = length (31 bits)

This can compress up to 2^31 consecutive set bits into 8 bytes!

### Chunk Organization

- Chunks are aligned to their capacity boundaries
- Multiple chunks are stored consecutively in the buffer
- Each chunk has a 4-byte starting offset followed by descriptor(s)

## Performance

### Best Case
- **Consecutive set bits**: 16KB compressed to 8 bytes
- **Consecutive unset bits**: No storage required
- **Select operation**: O(1) for RLE chunks
- **Scan operation**: Batch processing in groups of 64

### Worst Case
- **Random scattered bits**: Same as uncompressed bitmap + 8 bytes overhead
- For such patterns, consider [Roaring Bitmaps](https://github.com/RoaringBitmap/CRoaring)

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
