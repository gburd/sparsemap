/* SPDX-License-Identifier: MIT */
/*
 * test_rle_standalone.c -- RLE-free variant.
 *
 * The RLE variant transitioned a run longer than one chunk into a
 * single run-length-encoded descriptor.  This build has NO RLE: a long
 * run is stored as adjacent all-ONES sparse chunks.  These tests pin
 * that the observable behaviour of a long run (cardinality, is_set,
 * select, scan, rank) is identical to the RLE build, AND that the
 * encoder never emits an RLE descriptor (top two bits 01) -- walk a
 * serialized long-run map and check every chunk.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/*
 * Walk a serialized map's chunk stream and assert no chunk carries the
 * RLE flag (descriptor bits 63:62 == 01).  The wire body starts after
 * the 16-byte portable header: 8-byte chunk count, then per chunk an
 * 8-byte start offset and a descriptor word (+ payload words for MIXED,
 * which an all-ONES run never has).  Returns the number of chunks.
 */
static size_t
assert_no_rle_chunks(const sm_t *map)
{
  size_t n = sm_serialized_size(map);
  uint8_t *buf = malloc(n);
  assert(buf != NULL);
  assert(sm_serialize(map, buf, n) == n);

  const uint8_t *body = buf + 16;
  uint64_t count;
  memcpy(&count, body, 8);
  const uint8_t *p = body + 8;
  for (uint64_t c = 0; c < count; c++) {
    p += 8; /* skip start offset */
    uint64_t desc;
    memcpy(&desc, p, 8);
    /* RLE flag is bits 63:62 == 01. */
    assert(((desc >> 62) & 0x3) != 0x1);
    /* Count MIXED slots to advance past their payload words. */
    size_t payload = 0;
    for (int s = 0; s < 32; s++) {
      if (((desc >> (s * 2)) & 0x3) == 0x2 /* SM_PAYLOAD_MIXED */)
        payload++;
    }
    p += 8 + payload * 8;
  }
  free(buf);
  return ((size_t)count);
}

int
main(void)
{
  printf("Testing RLE-free long-run behaviour...\n");

  /* Allocate buffer for sparsemap */
  uint8_t *buf = calloc(16384, sizeof(uint8_t));
  assert(buf != NULL);

  /* Create sparsemap */
  sm_t *map = sparsemap(0);
  assert(map != NULL);
  sm_init(map, buf, 16384);

  /* Test 1: run of 3000 consecutive set bits (exceeds 2048 chunk cap) */
  printf("Test 1: Creating a run of 3000 bits...\n");
  for (size_t i = 0; i < 3000; i++) {
    sm_add(map, i);
  }
  printf("  Count: %zu (expected 3000)\n", sm_cardinality(map));
  assert(sm_cardinality(map) == 3000);

  /* The encoder must NOT have produced an RLE chunk: the 3000-bit run
   * is stored as sparse chunks (one full all-ONES + one partial). */
  {
    size_t chunks = assert_no_rle_chunks(map);
    printf("  Run stored in %zu sparse chunks, 0 RLE\n", chunks);
    assert(chunks >= 2); /* 3000 bits spans two 2048-bit chunks */
  }

  /* Test 2: is_set boundary check (the bug we fixed) */
  printf("Test 2: is_set boundary check...\n");
  assert(sm_contains(map, 0, NULL) == true);
  assert(sm_contains(map, 1500, NULL) == true);
  assert(sm_contains(map, 2999, NULL) == true);
  assert(sm_contains(map, 3000, NULL) == false); /* Should be false, was incorrectly true before fix */
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

  /* Test 7: a much longer run still emits only sparse chunks. */
  printf("Test 7: 100000-bit run is all sparse...\n");
  sm_clear(map);
  for (size_t i = 0; i < 100000; i++) {
    sm_add(map, i);
  }
  assert(sm_cardinality(map) == 100000);
  {
    size_t chunks = assert_no_rle_chunks(map);
    /* 100000 / 2048 = 48.8 -> 49 sparse chunks, 0 RLE. */
    assert(chunks == 49);
    printf("  PASS: 100000-bit run in %zu sparse chunks, 0 RLE\n", chunks);
  }

  /* Cleanup */
  free(buf);
  free(map);

  printf("\nAll RLE-free tests PASSED!\n");
  return 0;
}

