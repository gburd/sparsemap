#include "sm.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
static void emit(const char *name, uint64_t *bits, size_t n) {
    sm_t *m = sm_create(8192);
    for (size_t i = 0; i < n; i++) sm_add(m, bits[i]);
    size_t sz = sm_serialized_size(m);
    uint8_t *buf = malloc(sz);
    size_t w = sm_serialize(m, buf, sz);
    printf("/// C `sm_serialize` output for the %s set (%zu bits, %zu bytes).\n",
           name, (size_t)sm_cardinality(m), w);
    printf("const %s: &[u8] = &[", name);
    for (size_t i = 0; i < w; i++) printf("%u%s", buf[i], i+1<w?", ":"");
    printf("];\n");
    free(buf); sm_free(m);
}
int main(void) {
    { uint64_t b[1]; emit("EMPTY", b, 0); }
    { uint64_t b[] = {42}; emit("SINGLE", b, 1); }
    { uint64_t b[] = {1,2,3,2047,2048,4096,100000}; emit("SCATTERED", b, 7); }
    { uint64_t b[5000]; for (int i=0;i<5000;i++) b[i]=i; emit("RUN_5000", b, 5000); }
    { uint64_t b[2048*4]; for (int i=0;i<2048*4;i++) b[i]=i; emit("RUN_4WINDOWS", b, 2048*4); }
    { uint64_t b[150]; int k=0; for (int i=0;i<100;i++) b[k++]=i; for (int i=10000;i<10050;i++) b[k++]=i; emit("TWO_CLUSTERS", b, 150); }
    { uint64_t b[6000]; for (int i=0;i<6000;i++) b[i]=1000+i; emit("OFFSET_RUN", b, 6000); }
    return 0;
}
