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
  return 0;
}
