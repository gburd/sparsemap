#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../include/sparsemap.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#define __diag(...)                                                \
  do {                                                             \
    fprintf(stderr, "%s:%d:%s(): ", __FILE__, __LINE__, __func__); \
    fprintf(stderr, __VA_ARGS__);                                  \
  } while (0)
#pragma GCC diagnostic pop

/* !!! Duplicated here for testing purposes. Keep in sync, or suffer. !!! */
struct sparsemap {
#ifdef REENTRENT_SPARSEMAP
  pthread_mutex_t m_mutex;
#endif
  size_t m_capacity;
  size_t m_data_used;
  uint8_t *m_data;
};

int
main()
{
  size_t size = 4;
  setvbuf(stdout, NULL, _IONBF, 0); // Disable buffering for stdout
  setvbuf(stderr, NULL, _IONBF, 0); // Disable buffering for stdout

  __diag("Please wait a moment...");
  sparsemap_t mmap, *map = &mmap;
  uint8_t buffer[1024];
  uint8_t buffer2[1024];
  sparsemap_init(map, buffer, sizeof(buffer));
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
  fprintf(stderr, ".");

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

  fprintf(stderr, ".");

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
  fprintf(stderr, ".");

  // set [10000..0]
  for (int i = 10000; i >= 0; i--) {
    assert(sparsemap_is_set(map, i) == false);
    sparsemap_set(map, i, true);
    assert(sparsemap_is_set(map, i) == true);
  }

  for (int i = 10000; i >= 0; i--) {
    assert(sparsemap_is_set(map, i) == true);
    fprintf(stderr, ".");
  }

  // open and compare
  sparsemap_t _sm3, *sm3 = &_sm3;
  sparsemap_open(sm3, buffer, sizeof(buffer));
  for (int i = 0; i < 10000; i++) {
    assert(sparsemap_is_set(sm3, i) == sparsemap_is_set(map, i));
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

  fprintf(stderr, ".");
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
  fprintf(stderr, ".");

  for (int i = 0; i < 100000; i++) {
    sparsemap_set(map, i, true);
  }
  for (int i = 0; i < 100000; i++) {
    assert(sparsemap_select(map, i, true) == (unsigned)i);
  }

  sparsemap_clear(map);
  fprintf(stderr, ".");

  for (int i = 1; i < 513; i++) {
    sparsemap_set(map, i, true);
  }
  for (int i = 1; i < 513; i++) {
    assert(sparsemap_select(map, i - 1, true) == (unsigned)i);
  }

  sparsemap_clear(map);
  fprintf(stderr, ".");

  for (size_t i = 0; i < 8; i++) {
    sparsemap_set(map, i * 10, true);
  }
  for (size_t i = 0; i < 8; i++) {
    assert(sparsemap_select(map, i, true) == (sparsemap_idx_t)i * 10);
  }

  // split and move, aligned to MiniMap capacity
  sparsemap_t _sm2, *sm2 = &_sm2;
  sparsemap_init(sm2, buffer2, sizeof(buffer2));
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
  fprintf(stderr, ".");

  // split and move, aligned to BitVector capacity
  sparsemap_init(sm2, buffer2, sizeof(buffer2));
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

  fprintf(stderr, " ok\n");
}
