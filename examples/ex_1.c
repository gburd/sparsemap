/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <sm.h>

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
  size_t m_capacity;
  size_t m_data_used;
  uint8_t *m_data;
  uint8_t m_alloc_kind;
  sm_allocator_t m_allocator;
};

int
main()
{
  size_t size = 4;
  setvbuf(stdout, NULL, _IONBF, 0); // Disable buffering for stdout
  setvbuf(stderr, NULL, _IONBF, 0); // Disable buffering for stdout

  __diag("Please wait a moment...");
  sm_t mmap, *map = &mmap;
  uint8_t buffer[1024];
  uint8_t buffer2[1024];
  sm_init(map, buffer, sizeof(buffer));
  assert(sm_get_size(map) == size);
  sm_add(map, 0);
  assert(sm_get_size(map) == size + 4 + 8 + 8);
  assert(sm_contains(map, 0) == true);
  assert(sm_get_size(map) == size + 4 + 8 + 8);
  assert(sm_contains(map, 1) == false);
  sm_remove(map, 0);
  assert(sm_get_size(map) == size);

  sm_clear(map);
  sm_add(map, 64);
  assert(sm_contains(map, 64) == true);
  assert(sm_get_size(map) == size + 4 + 8 + 8);

  sm_clear(map);
  fprintf(stderr, ".");

  // set [0..100000]
  for (int i = 0; i < 100000; i++) {
    assert(sm_contains(map, i) == false);
    sm_add(map, i);
    if (i > 5) {
      for (int j = i - 5; j <= i; j++) {
        assert(sm_contains(map, j) == true);
      }
    }

    assert(sm_contains(map, i) == true);
  }

  fprintf(stderr, ".");

  for (int i = 0; i < 100000; i++) {
    assert(sm_contains(map, i) == true);
  }

  // unset [0..10000]
  for (int i = 0; i < 10000; i++) {
    assert(sm_contains(map, i) == true);
    sm_remove(map, i);
    assert(sm_contains(map, i) == false);
  }

  for (int i = 0; i < 10000; i++) {
    assert(sm_contains(map, i) == false);
  }

  sm_clear(map);
  fprintf(stderr, ".");

  // set [10000..0]
  for (int i = 10000; i >= 0; i--) {
    assert(sm_contains(map, i) == false);
    sm_add(map, i);
    assert(sm_contains(map, i) == true);
  }

  for (int i = 10000; i >= 0; i--) {
    assert(sm_contains(map, i) == true);
    fprintf(stderr, ".");
  }

  // open and compare
  sm_t _sm3, *sm3 = &_sm3;
  sm_open(sm3, buffer, sizeof(buffer));
  for (int i = 0; i < 10000; i++) {
    assert(sm_contains(sm3, i) == sm_contains(map, i));
  }

  // unset [10000..0]
  for (int i = 10000; i >= 0; i--) {
    assert(sm_contains(map, i) == true);
    sm_remove(map, i);
    assert(sm_contains(map, i) == false);
  }

  for (int i = 10000; i >= 0; i--) {
    assert(sm_contains(map, i) == false);
  }

  fprintf(stderr, ".");
  sm_clear(map);

  sm_add(map, 0);
  sm_add(map, 2048 * 2 + 1);
  assert(sm_contains(map, 0) == true);
  assert(sm_contains(map, 2048 * 2 + 0) == false);
  assert(sm_contains(map, 2048 * 2 + 1) == true);
  assert(sm_contains(map, 2048 * 2 + 2) == false);
  sm_add(map, 2048);
  assert(sm_contains(map, 0) == true);
  assert(sm_contains(map, 2047) == false);
  assert(sm_contains(map, 2048) == true);
  assert(sm_contains(map, 2049) == false);
  assert(sm_contains(map, 2048 * 2 + 2) == false);
  assert(sm_contains(map, 2048 * 2 + 0) == false);
  assert(sm_contains(map, 2048 * 2 + 1) == true);
  assert(sm_contains(map, 2048 * 2 + 2) == false);

  sm_clear(map);
  fprintf(stderr, ".");

  for (int i = 0; i < 100000; i++) {
    sm_add(map, i);
  }
  for (int i = 0; i < 100000; i++) {
    assert(sm_select(map, i, true) == (unsigned)i);
  }

  sm_clear(map);
  fprintf(stderr, ".");

  for (int i = 1; i < 513; i++) {
    sm_add(map, i);
  }
  for (int i = 1; i < 513; i++) {
    assert(sm_select(map, i - 1, true) == (unsigned)i);
  }

  sm_clear(map);
  fprintf(stderr, ".");

  for (size_t i = 0; i < 8; i++) {
    sm_add(map, i * 10);
  }
  for (size_t i = 0; i < 8; i++) {
    assert(sm_select(map, i, true) == (uint64_t)i * 10);
  }

  // split and move, aligned to MiniMap capacity
  sm_t _sm2, *sm2 = &_sm2;
  sm_init(sm2, buffer2, sizeof(buffer2));
  sm_clear(sm2);
  for (int i = 0; i < 2048 * 2; i++) {
    sm_add(map, i);
  }
  sm_split(map, 2048, sm2);
  for (int i = 0; i < 2048; i++) {
    assert(sm_contains(map, i) == true);
    assert(sm_contains(sm2, i) == false);
  }
  for (int i = 2048; i < 2048 * 2; i++) {
    assert(sm_contains(map, i) == false);
    assert(sm_contains(sm2, i) == true);
  }
  fprintf(stderr, ".");

  // split and move, aligned to BitVector capacity
  sm_init(sm2, buffer2, sizeof(buffer2));
  sm_clear(map);
  for (int i = 0; i < 2048 * 3; i++) {
    sm_add(map, i);
    assert(sm_contains(map, i) == true);
  }
  sm_split(map, 64, sm2);
  for (int i = 0; i < 2048 * 3; i++) {
    if (i < 64) {
      assert(sm_contains(map, i) == true);
      assert(sm_contains(sm2, i) == false);
    } else {
      assert(sm_contains(map, i) == false);
      assert(sm_contains(sm2, i) == true);
    }
  }

  fprintf(stderr, " ok\n");
}
