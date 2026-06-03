/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <sm.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#define __diag(...)                                                \
  do {                                                             \
    fprintf(stderr, "%s:%d:%s(): ", __FILE__, __LINE__, __func__); \
    fprintf(stderr, __VA_ARGS__);                                  \
  } while (0)
#pragma GCC diagnostic pop

int
main(void)
{
  int i;

  // disable buffering
  setvbuf(stdout, NULL, _IONBF, 0); // Disable buffering for stdout
  setvbuf(stderr, NULL, _IONBF, 0); // Disable buffering for stdout

  // start with a 1KiB buffer, 1024 bits
  uint8_t *buf = calloc(1024, sizeof(uint8_t));

  // create the sparse bitmap
  sparsemap_t *map = sm_wrap(buf, sizeof(uint8_t) * 1024);

  // Set every other bit (pathologically worst case) to see what happens
  // when the map is full.
  for (i = 0; i < 7744; i++) {
    if (!i % 2) {
      sm_add(map, i);
      assert(sm_contains(map, i) == true);
    }
  }
  // On 1024 KiB of buffer with every other bit set the map holds 7744 bits
  // and then runs out of space.  This next _set() call will fail.
  sm_add(map, ++i);
  assert(sm_contains(map, i) == true);
  return 0;
}
