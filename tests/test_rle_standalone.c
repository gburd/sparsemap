/* SPDX-License-Identifier: MIT */
/*
 * Standalone test for RLE implementation
 * Compile: cc -Wall -Wextra -g -I. -o test_rle test_rle_standalone.c sparsemap.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sm.h>

/* Test counter for scan */
static size_t scan_count = 0;
static uint32_t scan_last_idx = 0;

void
scan_counter(uint64_t v[], size_t n, void *aux)
{
  (void)aux;
  for (size_t i = 0; i < n; i++) {
    scan_count++;
    scan_last_idx = v[i];
  }
}

int
main(void)
{
  printf("Testing RLE implementation...\n");

  /* Allocate buffer for sparsemap */
  uint8_t *buf = calloc(16384, sizeof(uint8_t));
  assert(buf != NULL);

  /* Create sparsemap */
  sm_t *map = sparsemap(0);
  assert(map != NULL);
  sm_init(map, buf, 16384);

  /* Test 1: Create RLE run of 3000 consecutive set bits (exceeds 2048 chunk capacity) */
  printf("Test 1: Creating RLE run of 3000 bits...\n");
  for (size_t i = 0; i < 3000; i++) {
    sm_add(map, i);
  }
  printf("  Count: %zu (expected 3000)\n", sm_cardinality(map));
  assert(sm_cardinality(map) == 3000);

  /* Test 2: is_set boundary check (the bug we fixed) */
  printf("Test 2: is_set boundary check...\n");
  assert(sm_contains(map, 0) == true);
  assert(sm_contains(map, 1500) == true);
  assert(sm_contains(map, 2999) == true);
  assert(sm_contains(map, 3000) == false); /* Should be false, was incorrectly true before fix */
  printf("  PASS: is_set boundary check\n");

  /* Test 3: select operations */
  printf("Test 3: select operations...\n");
  assert(sm_select(map, 0, true) == 0);
  assert(sm_select(map, 1500, true) == 1500);
  assert(sm_select(map, 2999, true) == 2999);
  assert(sm_select(map, 3000, true) == SM_IDX_MAX);
  printf("  PASS: select(true) operations\n");

  assert(sm_select(map, 0, false) == 3000);
  assert(sm_select(map, 1, false) == 3001);
  assert(sm_select(map, 100, false) == 3100);
  printf("  PASS: select(false) operations\n");

  /* Test 4: scan operations */
  printf("Test 4: scan operations...\n");
  printf("  Map count before scan: %zu\n", sm_cardinality(map));
  scan_count = 0;
  scan_last_idx = 0;
  sm_scan(map, scan_counter, 0, NULL);
  assert(scan_count == 3000);
  assert(scan_last_idx == 2999);
  printf("  PASS: scan counted %zu bits, last index %u\n", scan_count, scan_last_idx);

  /* Test 5: scan with skip */
  printf("Test 5: scan with skip=2500...\n");
  sm_clear(map);
  for (size_t i = 0; i < 3000; i++) {
    sm_add(map, i);
  }
  scan_count = 0;
  scan_last_idx = 0;
  sm_scan(map, scan_counter, 2500, NULL);
  printf("  After scan: scan_count=%zu, expected=500\n", scan_count);
  assert(scan_count == 500);
  assert(scan_last_idx == 2999);
  printf("  PASS: scan with skip counted %zu bits, last index %u\n", scan_count, scan_last_idx);

  /* Test 6: rank operations */
  printf("Test 6: rank operations...\n");
  assert(sm_rank(map, 0, 2999, true) == 3000);
  assert(sm_rank(map, 0, 1499, true) == 1500);
  assert(sm_rank(map, 1500, 2999, true) == 1500);
  printf("  PASS: rank operations\n");

  /* Cleanup */
  free(buf);
  free(map);

  printf("\nAll RLE tests PASSED!\n");
  return 0;
}
