#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "../include/sparsemap.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#define __diag(...)                                                \
  do {                                                             \
    fprintf(stderr, "%s:%d:%s(): ", __FILE__, __LINE__, __func__); \
    fprintf(stderr, __VA_ARGS__);                                  \
  } while (0)
#pragma GCC diagnostic pop

#define SEED

int
main(void)
{
  int i = 0;

  // disable buffering
  setbuf(stderr, 0);

  // start with a 1KiB buffer, 1024 bits
  uint8_t *buf = calloc(1024, sizeof(uint8_t));

  // create the sparse bitmap
  sparsemap_t *map = sparsemap(buf, sizeof(uint8_t) * 1024);

  // Set every other bit (pathologically worst case) to see what happens
  // when the map is full.
  for (i = 0; i < 7744; i++) {
    if (i % 2)
      continue;
    sparsemap_set(map, i, true);
    assert(sparsemap_is_set(map, i) == true);
  }
  // On 1024 KiB of buffer with every other bit set the map holds 7744 bits
  // and then runs out of space.  This next _set() call will fail/abort.
  sparsemap_set(map, ++i, true);
  assert(sparsemap_is_set(map, i) == true);
  return 0;
}
