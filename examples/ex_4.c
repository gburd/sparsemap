#include <assert.h>
#include <common.h>
#include <sparsemap.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define TEST_ARRAY_SIZE 1024

int
main(void)
{
  int i;
  int array[TEST_ARRAY_SIZE];

  xorshift32_seed();

  // disable buffering
  setvbuf(stdout, NULL, _IONBF, 0); // Disable buffering for stdout
  setvbuf(stderr, NULL, _IONBF, 0); // Disable buffering for stdout

  // start with a 3KiB buffer, TEST_ARRAY_SIZE bits
  uint8_t *buf = calloc((size_t)3 * 1024, sizeof(uint8_t));

  // create the sparse bitmap
  sparsemap_t *map = sparsemap_wrap(buf, sizeof(uint8_t) * 3 * 1024);

  // create an array of ints
  setup_test_array(array, TEST_ARRAY_SIZE, 1024 * 3);

  // randomize setting the bits on
  shuffle(array, TEST_ARRAY_SIZE);
  // print_array(array, TEST_ARRAY_SIZE);
  // print_spans(array, TEST_ARRAY_SIZE);

  // set all the bits on in a random order
  for (i = 0; i < TEST_ARRAY_SIZE; i++) {
    sparsemap_add(map, array[i]);
    assert(sparsemap_contains(map, array[i]) == true);
  }

  // for (size_t len = 1; len < 20; len++) {
  // for (size_t len = 1; len < TEST_ARRAY_SIZE - 1; len++) {
  // for (size_t len = 1; len <= 1; len++) {
  // for (size_t len = 2; len <= 2; len++) {
  // for (size_t len = 3; len <= 3; len++) {
  // for (size_t len = 4; len <= 4; len++) {
  // for (size_t len = 5; len <= 5; len++) {
  // for (size_t len = 8; len <= 8; len++) {
  for (size_t len = 372; len <= 372; len++) {
    __diag("================> %lu\n", len);
    sparsemap_clear(map);
    // set all the bits on in a random order
    ensure_sequential_set(array, TEST_ARRAY_SIZE, (int)len);
    shuffle(array, TEST_ARRAY_SIZE);
    print_spans(array, TEST_ARRAY_SIZE);
    for (i = 0; i < TEST_ARRAY_SIZE; i++) {
      sparsemap_add(map, array[i]);
      assert(sparsemap_contains(map, array[i]) == true);
    }
    has_span(map, array, TEST_ARRAY_SIZE, (int)len);
    size_t l = sparsemap_span(map, 0, len, true);
    if (l != (size_t)-1) {
      __diag("Found span in map starting at %lu of length %lu\n", l, len);
      __diag("is_span(%lu, %lu) == %s\n", l, len, is_span(array, TEST_ARRAY_SIZE, l, len) ? "yes" : "no");
      i = (int)l;
      do {
        bool set = sparsemap_contains(map, i);
        if (set) {
          __diag("verified %d was set\n", i);
        } else {
          __diag("darn, %d was not really set, %s\n", i, is_set(array, i) ? "but we thought it was" : "because it wasn't");
        }
      } while (++i < (int)(l + len));
    } else {
      __diag("UNABLE TO FIND SPAN in map of length %lu\n", len);
    }
  }
  return 0;
}
