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

/* https://burtleburtle.net/bob/rand/smallprng.html */
typedef struct rnd_ctx {
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
} rnd_ctx_t;
#define __rot(x, k) (((x) << (k)) | ((x) >> (32 - (k))))
uint32_t
__random(rnd_ctx_t *x)
{
  uint32_t e = x->a - __rot(x->b, 27);
  x->a = x->b ^ __rot(x->c, 17);
  x->b = x->c + x->d;
  x->c = x->d + e;
  x->d = e + x->a;
  return x->d;
}

void
__random_seed(rnd_ctx_t *x, uint32_t seed)
{
  uint32_t i;
  x->a = 0xf1ea5eed, x->b = x->c = x->d = seed;
  for (i = 0; i < 20; ++i) {
    (void)__random(x);
  }
}

void
shuffle(rnd_ctx_t *prng, int *array, size_t n)
{
  size_t i, j;

  if (n > 1) {
    for (i = n - 1; i > 0; i--) {
      j = (unsigned int)(__random(prng) % (i + 1));
      // XOR swap algorithm
      if (i != j) { // avoid self-swap leading to zero-ing the element
        array[i] = array[i] ^ array[j];
        array[j] = array[i] ^ array[j];
        array[i] = array[i] ^ array[j];
      }
    }
  }
}

bool
was_set(size_t bit, int array[])
{
  for (int i = 0; i < 1024; i++) {
    if (array[i] == bit)
      return true;
  }
  return false;
}

int
main(void)
{
  int i = 0;
  rnd_ctx_t prng;
  int array[1024];

  // disable buffering
  setbuf(stderr, 0);

  // seed the PRNG
#ifdef SEED
  __random_seed(&prng, 8675309);
#else
  __random_seed(&prng, (unsigned int)time(NULL) ^ getpid());
#endif

  for (i = 0; i < 1024; i++) {
    array[i] = (int)__random(&prng) % 7000 + 1;
    if (array[i] < 0)
      i--;
  }
  // randomize setting the bits on
  shuffle(&prng, array, 1024);

  // start with a 1KiB buffer, 1024 bits
  uint8_t *buf = calloc(1024, sizeof(uint8_t));

  // create the sparse bitmap
  sparsemap_t *map = sparsemap(buf, sizeof(uint8_t) * 1024, 0);

  // set all the bits on in a random order
  for (i = 0; i < 1024; i++) {
    //__diag("set %d\n", array[i]);
    sparsemap_set(map, array[i], true);
    assert(sparsemap_is_set(map, array[i]) == true);
  }

  size_t l = sparsemap_span(map, 0, 8);
  __diag("found span of 8 at %lu starting from 0\n", l);
  for (i = l; i < l + 8; i++) {
    bool set = sparsemap_is_set(map, l + i);
    if (set)
      __diag("verified %lu was set\n", l + i);
    else
      __diag("darn, %lu was not really set, %s\n", l + i, was_set(l + i, array) ? "but we thought it was" : "because it wasn't");
  }

  return 0;
}
