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
    /* out[6] mirrors the body small-set marker; report the mode. */
    const char *mode = (w > 6 && buf[6]) ? "small" : "chunk";
    printf("/// C `sm_serialize` output for the %s set (%zu bits, %zu bytes, %s-mode).\n",
           name, (size_t)sm_cardinality(m), w, mode);
    printf("const %s: &[u8] = &[", name);
    for (size_t i = 0; i < w; i++) printf("%u%s", buf[i], i+1<w?", ":"");
    printf("];\n");
    free(buf); sm_free(m);
}
int main(void) {
    /* --- original fixtures (regenerated against the fixed C) --- */
    { uint64_t b[1]; emit("EMPTY", b, 0); }
    { uint64_t b[] = {42}; emit("SINGLE", b, 1); }
    { uint64_t b[] = {1,2,3,2047,2048,4096,100000}; emit("SCATTERED", b, 7); }
    { uint64_t *b=malloc(sizeof(uint64_t)*5000); for (int i=0;i<5000;i++) b[i]=i; emit("RUN_5000", b, 5000); free(b);}
    { uint64_t *b=malloc(sizeof(uint64_t)*2048*4); for (int i=0;i<2048*4;i++) b[i]=i; emit("RUN_4WINDOWS", b, 2048*4); free(b);}
    { uint64_t b[150]; int k=0; for (int i=0;i<100;i++) b[k++]=i; for (int i=10000;i<10050;i++) b[k++]=i; emit("TWO_CLUSTERS", b, 150); }
    { uint64_t *b=malloc(sizeof(uint64_t)*6000); for (int i=0;i<6000;i++) b[i]=1000+i; emit("OFFSET_RUN", b, 6000); free(b);}

    /* --- small-mode fixtures (header out[6] set) --- */
    { uint64_t b[] = {0}; emit("SMALL_ZERO", b, 1); }                     /* {0} */
    { uint64_t b[] = {0,1,5,63}; emit("SMALL_WORD0", b, 4); }             /* word 0 near-zero */
    { uint64_t b[64]; for (int i=0;i<64;i++) b[i]=i; emit("SMALL_FULLWORD", b, 64); } /* {0..63} full word */
    { uint64_t b[] = {5,70}; emit("SMALL_TWOWORDS", b, 2); }              /* two words {5,70} */
    { uint64_t b[] = {3,17,88,200,511,900,1023}; emit("SMALL_SCATTER", b, 7); } /* sparse scatter under 1024 */
    { uint64_t b[] = {0}; emit("SMALL_EMPTYTHENSINGLE", b, 1); }          /* empty-then-single (built via remove below in Rust; here just {0}) */

    /* --- RLE-chunk fixtures --- */
    { uint64_t *b=malloc(sizeof(uint64_t)*1001); for (int i=0;i<=1000;i++) b[i]=i; emit("RLE_RUN_0_1000", b, 1001); free(b);}   /* {0..1000} */
    { uint64_t *b=malloc(sizeof(uint64_t)*1024); for (int i=0;i<1024;i++) b[i]=i; emit("RLE_RUN_0_1023", b, 1024); free(b);}     /* {0..1023} */
    { uint64_t *b=malloc(sizeof(uint64_t)*5001); for (int i=0;i<=5000;i++) b[i]=i; emit("RLE_RUN_0_5000", b, 5001); free(b);}    /* {0..5000} multi-chunk */
    { uint64_t *b=malloc(sizeof(uint64_t)*2048); for (int i=0;i<2048;i++) b[i]=2048+i; emit("RLE_RUN_2048_4095", b, 2048); free(b);} /* {2048..4095} not starting at 0 */

    /* --- mixed fixtures --- */
    { /* low cluster promoted to chunk + a high sparse chunk */
      uint64_t *b=malloc(sizeof(uint64_t)*(1201+3)); int k=0;
      for (int i=0;i<=1200;i++) b[k++]=i;         /* dense low run -> RLE/chunk */
      b[k++]=50000; b[k++]=50003; b[k++]=123456;  /* high sparse chunk */
      emit("MIXED_LOWRUN_HIGHSPARSE", b, k); free(b);}
    { /* straddle the 1024 small/chunk boundary: a set that crosses out of small mode */
      uint64_t b[] = {0,1,1023,1024,1025,2050}; emit("MIXED_STRADDLE_1024", b, 6); }
    return 0;
}
