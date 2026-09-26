/* regenerate fixtures by compiling this against sm.c then running the binary */

#include "sm.h"
#include <stdio.h>
#include <stdlib.h>

/* sets here must mirror those in wire_emit.py plus the empty one */
static void
emit(const char *name, const uint64_t *bits, size_t n)
{
  char path[256];
  sm_t *m = sm_create(65536);
  for (size_t i = 0; i < n; i++)
    sm_add(m, bits[i]);
  size_t sz = sm_serialized_size(m);
  uint8_t *buf = malloc(sz);
  size_t w = sm_serialize(m, buf, sz);
  snprintf(path, sizeof(path), "../tests/fixtures/%s.bin", name);
  FILE *f = fopen(path, "wb");
  if (f == NULL || fwrite(buf, 1, w, f) != w) {
    fprintf(stderr, "write failed: %s\n", path);
    exit(1);
  }
  fclose(f);
  free(buf);
  sm_free(m);
}

int
main(void)
{
  { uint64_t b[1]; emit("empty", b, 0); }
  { uint64_t b[] = { 42 }; emit("single", b, 1); }
  { uint64_t b[] = { 1, 2, 3, 2047, 2048, 4096, 100000 }; emit("scattered", b, 7); }
  { static uint64_t b[5000]; for (int i = 0; i < 5000; i++) b[i] = i; emit("run5000", b, 5000); }
  { static uint64_t b[8192]; for (int i = 0; i < 8192; i++) b[i] = i; emit("run4w", b, 8192); }
  { uint64_t b[150]; int k = 0; for (int i = 0; i < 100; i++) b[k++] = i;
    for (int i = 10000; i < 10050; i++) b[k++] = i; emit("clusters", b, 150); }
  { static uint64_t b[6000]; for (int i = 0; i < 6000; i++) b[i] = 1000 + i; emit("offset", b, 6000); }
  /* small-mode shapes (C body top bit set) */
  { uint64_t b[] = { 0 }; emit("small_zero", b, 1); }
  { uint64_t b[] = { 0, 1, 5, 63 }; emit("small_word0", b, 4); }
  { uint64_t b[64]; for (int i = 0; i < 64; i++) b[i] = i; emit("small_fullword", b, 64); }
  { uint64_t b[] = { 5, 70 }; emit("small_twowords", b, 2); }
  { uint64_t b[] = { 3, 17, 88, 200, 511, 900, 1023 }; emit("small_scatter", b, 7); }
  /* RLE-chunk shapes */
  { static uint64_t b[1001]; for (int i = 0; i <= 1000; i++) b[i] = i; emit("rle_run_0_1000", b, 1001); }
  { static uint64_t b[1024]; for (int i = 0; i < 1024; i++) b[i] = i; emit("rle_run_0_1023", b, 1024); }
  { static uint64_t b[5001]; for (int i = 0; i <= 5000; i++) b[i] = i; emit("rle_run_0_5000", b, 5001); }
  { static uint64_t b[2048]; for (int i = 0; i < 2048; i++) b[i] = 2048 + i; emit("rle_run_2048_4095", b, 2048); }
  /* mixed */
  { static uint64_t b[1204]; int k = 0; for (int i = 0; i <= 1200; i++) b[k++] = i;
    b[k++] = 50000; b[k++] = 50003; b[k++] = 123456; emit("mixed_lowrun_highsparse", b, k); }
  { uint64_t b[] = { 0, 1, 1023, 1024, 1025, 2050 }; emit("mixed_straddle_1024", b, 6); }
  return 0;
}
