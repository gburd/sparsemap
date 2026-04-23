# Quick Start Guide

## Build Without Autotools

If you don't have automake installed or prefer a simpler build process:

```bash
# Build library (debug mode)
make -f Makefile.simple DEBUG=1 DIAGNOSTIC=1

# Build and run RLE standalone test
make -f Makefile.simple DEBUG=1 DIAGNOSTIC=1
./tests/test_rle_standalone

# Install to custom location
make -f Makefile.simple PREFIX=/tmp/sparsemap install
```

## Build With Autotools (Recommended for Production)

If you have autoconf, automake, and libtool installed:

```bash
# Install build tools (if needed)
# macOS: brew install autoconf automake libtool
# Ubuntu: sudo apt-get install autoconf automake libtool

# Generate build system
./autogen.sh

# Configure and build
./configure --prefix=/tmp/sparsemap --enable-debug --enable-diagnostic
make -j$(nproc)

# Run tests
make check

# Install
make install
```

## Build Results

After building, you'll have:

- `libsparsemap.a` - Static library
- `libsparsemap.dylib` (or `.so` on Linux) - Shared library
- `tests/test_rle_standalone` - RLE test executable

## Test Output (Expected)

```
Testing RLE implementation...
Test 1: Creating RLE run of 3000 bits...
  Count: 3000 (expected 3000)
Test 2: is_set boundary check...
  PASS: is_set boundary check
Test 3: select operations...
  PASS: select(true) operations
  PASS: select(false) operations
Test 4: scan operations...
  Map count before scan: 3000
  PASS: scan counted 3000 bits, last index 2999
Test 5: scan with skip=2500...
  After scan: scan_count=500, expected=500
  PASS: scan with skip counted 500 bits, last index 2999
Test 6: rank operations...
  PASS: rank operations

All RLE tests PASSED!
```

## Using the Library

### Include in Your C Project

```c
#include <sparsemap.h>

int main() {
    // Create buffer
    uint8_t buffer[1024];

    // Create sparsemap
    sparsemap_t *map = sparsemap(0);
    sparsemap_init(map, buffer, 1024);

    // Use it
    sparsemap_set(map, 100);
    bool is_set = sparsemap_is_set(map, 100);

    // Clean up
    free(map);
    return 0;
}
```

### Compile Your Program

```bash
# With installed library
cc -o myapp myapp.c -I/tmp/sparsemap/include -L/tmp/sparsemap/lib -lsparsemap

# With local library
cc -o myapp myapp.c -I./include -L. -lsparsemap
```

## Makefile.simple Targets

```bash
make -f Makefile.simple help        # Show all targets
make -f Makefile.simple             # Build library only
make -f Makefile.simple tests       # Build test suite
make -f Makefile.simple examples    # Build examples
make -f Makefile.simple check       # Build and run tests
make -f Makefile.simple install     # Install library
make -f Makefile.simple clean       # Clean build artifacts
```

## Options

```bash
PREFIX=/path    # Installation prefix (default: /usr/local)
DEBUG=1         # Enable debug build (-g -O0)
DIAGNOSTIC=1    # Enable diagnostic output
TESTING=1       # Enable testing support
```

## Troubleshooting

### "aclocal: command not found"
- You need automake: `brew install automake`
- Or use the simple Makefile: `make -f Makefile.simple`

### Homebrew Permission Issues
```bash
# Fix permissions (requires sudo)
sudo chown -R $(whoami) /opt/homebrew/Cellar
brew install automake
```

### Build Warnings in Tests
- Warnings in test code are expected and don't affect library functionality
- The library itself builds with `-Werror` (warnings as errors)

## What's Next?

- See `README.md` for full documentation
- See `doc/RLE_IMPLEMENTATION_SUMMARY.md` for implementation details
- See `examples/` for usage examples
- See `INSTALL` for detailed installation instructions

## Success!

If the RLE standalone test passes, your build is working correctly and the library is ready to use.
