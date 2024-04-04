#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

#include "../include/sparsemap.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#define __diag(format, ...) \
  __diag_(__FILE__, __LINE__, __func__, format, ##__VA_ARGS__)
#pragma GCC diagnostic pop
void __attribute__((format(printf, 4, 5))) __diag_(const char *file,
  int line, const char *func, const char *format, ...)
{
  va_list args;
  va_start(args, format);
  fprintf(stderr, "%s:%d:%s(): ", file, line, func);
  vfprintf(stderr, format, args);
  va_end(args);
}

// NOTE: currently, this code serves as a sample and unittest.

int main() {
  int size = 4;
  __diag("Please wait a moment...");
#if 1
  uint8_t buffer[1024];
  uint8_t buffer2[1024];
  sparsemap_t *map = sparsemap(buffer, sizeof(buffer), 0);
  assert(sparsemap_get_size(map) == size);
  sparsemap_set(map, 0, true);
  assert(sparsemap_get_size(map) == size + 4 + 8 + 8);
  assert(sparsemap_is_set(map, 0) == true);
  assert(sparsemap_get_size(map) == size + 4 + 8 + 8);
  assert(sparsemap_is_set(map, 1) == false);
  sparsemap_set(map, 0, false);
  assert(sparsemap_get_size(map) == size);

  sparsemap_clear(map);
  sparsemap_set(map, 64, true);
  assert(sparsemap_is_set(map, 64) == true);
  assert(sparsemap_get_size(map) == size + 4 + 8 + 8);

  sparsemap_clear(map);
  __diag(".");

  // set [0..100000]
  for (int i = 0; i < 100000; i++) {
    assert(sparsemap_is_set(map, i) == false);
    sparsemap_set(map, i, true);
    if (i > 5) {
      for (int j = i - 5; j <= i; j++) {
        assert(sparsemap_is_set(map, j) == true);
      }
    }

    assert(sparsemap_is_set(map, i) == true);
  }

  __diag(".");

  for (int i = 0; i < 100000; i++) {
    assert(sparsemap_is_set(map, i) == true);
  }

  // unset [0..10000]
  for (int i = 0; i < 10000; i++) {
    assert(sparsemap_is_set(map, i) == true);
    sparsemap_set(map, i, false);
    assert(sparsemap_is_set(map, i) == false);
  }

  for (int i = 0; i < 10000; i++) {
    assert(sparsemap_is_set(map, i) == false);
  }

  sparsemap_clear(map);
  __diag(".");

  // set [10000..0]
  for (int i = 10000; i >= 0; i--) {
    assert(sparsemap_is_set(map, i) == false);
    sparsemap_set(map, i, true);
    assert(sparsemap_is_set(map, i) == true);
  }

  for (int i = 10000; i >= 0; i--) {
    assert(sparsemap_is_set(map, i) == true);
    __diag(".");
  }

  // open and compare
  sparsemap_t *sm2 = sparsemap_open(buffer, sizeof(buffer));
  for (int i = 0; i < 10000; i++) {
    assert(sparsemap_is_set(sm2, i) == sparsemap_is_set(map, i));
  }

  // unset [10000..0]
  for (int i = 10000; i >= 0; i--) {
    assert(sparsemap_is_set(map, i) == true);
    sparsemap_set(map, i, false);
    assert(sparsemap_is_set(map, i) == false);
  }

  for (int i = 10000; i >= 0; i--) {
    assert(sparsemap_is_set(map, i) == false);
  }

  __diag(".");
  sparsemap_clear(map);

  sparsemap_set(map, 0, true);
  sparsemap_set(map, 2048 * 2 + 1, true);
  assert(sparsemap_is_set(map, 0) == true);
  assert(sparsemap_is_set(map, 2048 * 2 + 0) == false);
  assert(sparsemap_is_set(map, 2048 * 2 + 1) == true);
  assert(sparsemap_is_set(map, 2048 * 2 + 2) == false);
  sparsemap_set(map, 2048, true);
  assert(sparsemap_is_set(map, 0) == true);
  assert(sparsemap_is_set(map, 2047) == false);
  assert(sparsemap_is_set(map, 2048) == true);
  assert(sparsemap_is_set(map, 2049) == false);
  assert(sparsemap_is_set(map, 2048 * 2 + 2) == false);
  assert(sparsemap_is_set(map, 2048 * 2 + 0) == false);
  assert(sparsemap_is_set(map, 2048 * 2 + 1) == true);
  assert(sparsemap_is_set(map, 2048 * 2 + 2) == false);

  sparsemap_clear(map);
  __diag(".");

  for (int i = 0; i < 100000; i++) {
    sparsemap_set(map, i, true);
  }
  for (int i = 0; i < 100000; i++) {
    assert(sparsemap_select(map, i) == (unsigned)i);
  }

  sparsemap_clear(map);
  __diag(".");

  for (int i = 1; i < 513; i++) {
    sparsemap_set(map, i, true);
  }
  for (int i = 1; i < 513; i++) {
    assert(sparsemap_select(map, i - 1) == (unsigned)i);
  }

  sparsemap_clear(map);
  __diag(".");

  for (size_t i = 0; i < 8; i++) {
    sparsemap_set(map, i * 10, true);
  }
  for (size_t i = 0; i < 8; i++) {
    assert(sparsemap_select(map, i) == i * 10);
  }

  // split and move, aligned to MiniMap capacity
  sm2 = sparsemap(buffer2, sizeof(buffer2), 0);
  sparsemap_clear(sm2);
  for (int i = 0; i < 2048 * 2; i++) {
    sparsemap_set(map, i, true);
  }
  sparsemap_split(map, 2048, sm2);
  for (int i = 0; i < 2048; i++) {
    assert(sparsemap_is_set(map, i) == true);
    assert(sparsemap_is_set(sm2, i) == false);
  }
  for (int i = 2048; i < 2048 * 2; i++) {
    assert(sparsemap_is_set(map, i) == false);
    assert(sparsemap_is_set(sm2, i) == true);
  }
  __diag(".");

  // split and move, aligned to BitVector capacity
  sm2 = sparsemap(buffer2, sizeof(buffer2), 0);
  sparsemap_clear(map);
  for (int i = 0; i < 2048 * 3; i++) {
    sparsemap_set(map, i, true);
  }
  sparsemap_split(map, 64, sm2);
  for (int i = 0; i < 64; i++) {
    assert(sparsemap_is_set(map, i) == true);
    assert(sparsemap_is_set(sm2, i) == false);
  }
  for (int i = 64; i < 2048 * 3; i++) {
    assert(sparsemap_is_set(map, i) == false);
    assert(sparsemap_is_set(sm2, i) == true);
  }

  __diag("ok\n");
#else
  //
  // This code was used to create the lookup table for
  // sparsemap::MiniMap<>::calc_vector_size()
  //
  __diag("   ");
  for (unsigned int ch = 0; ch <= 0xff; ch++) {
    if (ch > 0 && ch % 16 == 0)
      __diag("\n   ");

    /*
    // check if value is invalid (contains 2#01)
    if ((ch & (0x3 << 0)) >> 0 == 1
        || (ch & (0x3 << 2)) >> 2 == 1
        || (ch & (0x3 << 4)) >> 4 == 1
        || (ch & (0x3 << 6)) >> 6 == 1) {
      //__diag("%d: -1\n", (int)ch);
      __diag(" -1,");
      continue;
    }
    */

    // count all occurrences of 2#10
    int size = 0;
    if ((ch & (0x3 << 0)) >> 0 == 2)
      size++;
    if ((ch & (0x3 << 2)) >> 2 == 2)
      size++;
    if ((ch & (0x3 << 4)) >> 4 == 2)
      size++;
    if ((ch & (0x3 << 6)) >> 6 == 2)
      size++;
    //__diag("%u: %d\n", (unsigned int)ch, size);
    __diag("  %d,", size);
  }
#endif
}
