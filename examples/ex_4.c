#include <assert.h>
#include <ctype.h>
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

uint32_t
xorshift(int *state)
{
   if (!state) {
    return 0;
  }
  // Xorshift algorithm
  uint32_t x = *state; // Dereference state to get the current state value
  if (x == 0) x = 123456789; // Ensure the state is never zero; use a default seed if so
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x; // Update the state
  return x; // Return the new state as the next pseudo-random number
}

void
shuffle(int *array, size_t n, int *prng)
{
  size_t i, j;

  if (n > 1) {
    for (i = n - 1; i > 0; i--) {
      j = (unsigned int)(xorshift(prng) % (i + 1));
      // XOR swap algorithm
      if (i != j) { // avoid self-swap leading to zero-ing the element
        array[i] = array[i] ^ array[j];
        array[j] = array[i] ^ array[j];
        array[i] = array[i] ^ array[j];
      }
    }
  }
}

int
compare_ints(const void *a, const void *b)
{
  return *(const int *)a - *(const int *)b;
}

// Check if there's already a sequence of 'r' sequential integers
int has_sequential_set(int *a, size_t l, int r) {
  int count = 1; // Start with a count of 1 for the first number
  for (size_t i = 1; i < l; ++i) {
    if (a[i] - a[i - 1] == 1) { // Check if the current and previous elements are sequential
      count++;
      if (count >= r) return 1; // Found a sequential set of length 'r'
    } else {
      count = 1; // Reset count if the sequence breaks
    }
  }
  return 0; // No sequential set of length 'r' found
}

// Function to ensure an array contains a set of 'r' sequential integers
void ensure_sequential_set(int *a, size_t l, int r, uint32_t *prng) {
  if (r > l) return; // If 'r' is greater than array length, cannot satisfy the condition

  // Sort the array to check for existing sequences
  qsort(a, l, sizeof(int), compare_ints);

  // Check if a sequential set of length 'r' already exists
  if (has_sequential_set(a, l, r)) {
    return; // Sequence already exists, no modification needed
  }

  // Find the minimum and maximum values in the array
  int min_value = a[0];
  int max_value = a[l - 1];

  // Generate a random value between min_value and max_value
  int value = xorshift(prng) % (max_value - min_value - r + 1);

  // Generate a random location between 0 and l - r
  int offset = xorshift(prng) % (l + r + 1);

  // Adjust the array to include a sequential set of 'r' integers at the random offset
  for (int i = 0; i < r; ++i) {
    a[i + offset] = value + i;
  }
}

void
print_array(int *array, size_t l)
{
  int a[l];
  memcpy(a, array, sizeof(int) * l);
  qsort(a, l, sizeof(int), compare_ints);

  printf("int a[] = {");
  for (int i = 0; i < l; i++) {
    printf("%d", a[i]);
    if (i != l) {
      printf(", ");
    }
  }
  printf("};\n");
}

bool
has_span(sparsemap_t *map, int *array, size_t l, size_t n)
{
  if (n == 0 || l == 0 || n > l) {
    return false;
  }

  int sorted[l];
  memcpy(sorted, array, sizeof(int) * l);
  qsort(sorted, l, sizeof(int), compare_ints);

  for (size_t i = 0; i <= l - n; i++) {
    if (sorted[i] + n - 1 == sorted[i + n - 1]) {
#if 0
      fprintf(stderr, "Found span: ");
      for (size_t j = i; j < i + n; j++) {
        fprintf(stderr, "%d ", sorted[j]);
      }
       fprintf(stderr, "\n");
#endif
      for (size_t j = 0; j < n; j++) {
        size_t pos = sorted[j + i];
        bool set = sparsemap_is_set(map, pos);
        assert(set);
      }
      __diag("Found span: [%d, %d], length: %zu\n", sorted[i], sorted[i + n - 1], n);
      return true;
    }
  }

  return false;
}

bool
is_span(int *array, size_t n, int x, int l)
{
  if (n == 0 || l < 0) {
    return false;
  }

  int a[n];
  memcpy(a, array, sizeof(int) * n);
  qsort(a, n, sizeof(int), compare_ints);

  // Iterate through the array to find a span starting at x of length l
  for (size_t i = 0; i < n; i++) {
    if (a[i] == x) {
      // Check if the span can fit in the array
      if (i + l - 1 < n && a[i + l - 1] == x + l - 1) {
        return true; // Found the span
      }
    }
  }
  return false; // Span not found
}

void
print_spans(int *array, size_t n)
{
  int a[n];
  size_t start = 0, end = 0;

  if (n == 0) {
    fprintf(stderr, "Array is empty\n");
    return;
  }

  memcpy(a, array, sizeof(int) * n);
  qsort(a, n, sizeof(int), compare_ints);

  for (size_t i = 1; i < n; i++) {
    if (a[i] == a[i - 1] + 1) {
      end = i; // Extend the span
    } else {
      // Print the current span
      if (start == end) {
        fprintf(stderr, "[%d] ", a[start]);
      } else {
        fprintf(stderr, "[%d, %d] ", a[start], a[end]);
      }
      // Move to the next span
      start = i;
      end = i;
    }
  }

  // Print the last span if needed
  if (start == end) {
    fprintf(stderr, "[%d]\n", a[start]);
  } else {
    fprintf(stderr, "[%d, %d]\n", a[start], a[end]);
  }
}

bool
was_set(size_t bit, const int array[])
{
  for (int i = 0; i < 1024; i++) {
    if (array[i] == (int)bit) {
      return true;
    }
  }
  return false;
}

#define TEST_ARRAY_SIZE 1024

int
is_unique(int a[], size_t l, int value) {
  for (size_t i = 0; i < l; ++i) {
    if (a[i] == value) {
      return 0; // Not unique
    }
  }
  return 1; // Unique
}

void
setup_test_array(int a[], size_t l, int max_value, int *prng)
{
  if (a == NULL || prng == NULL || max_value < 0) return; // Basic error handling and validation

  for (size_t i = 0; i < l; ++i) {
    int candidate;
    do {
      candidate = xorshift(prng) % (max_value + 1); // Generate a new value within the specified range
    } while (!is_unique(a, i, candidate)); // Repeat until a unique value is found
    a[i] = candidate; // Assign the unique value to the array
  }
}

int
main(void)
{
  int i = 0;
  size_t rank;
  int array[TEST_ARRAY_SIZE];
  int prng;

// seed the PRNG
#ifdef SEED
  prng = 8675309;
#else
  prng = (unsigned int)time(NULL) ^ getpid();
#endif

  // disable buffering
  setvbuf(stdout, NULL, _IONBF, 0); // Disable buffering for stdout
  setvbuf(stderr, NULL, _IONBF, 0); // Disable buffering for stdout

  // start with a 3KiB buffer, TEST_ARRAY_SIZE bits
  uint8_t *buf = calloc(3 * 1024, sizeof(uint8_t));

  // create the sparse bitmap
  sparsemap_t *map = sparsemap(buf, sizeof(uint8_t) * 3 * 1024, 0);

  // create an array of ints
  setup_test_array(array, TEST_ARRAY_SIZE, 1024 * 3, &prng);

  // randomize setting the bits on
  shuffle(array, TEST_ARRAY_SIZE, &prng);
  //print_array(array, TEST_ARRAY_SIZE);
  //print_spans(array, TEST_ARRAY_SIZE);

  // set all the bits on in a random order
  for (i = 0; i < TEST_ARRAY_SIZE; i++) {
    sparsemap_set(map, array[i], true);
    assert(sparsemap_is_set(map, array[i]) == true);
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
    ensure_sequential_set(array, TEST_ARRAY_SIZE, len, &prng);
    shuffle(array, TEST_ARRAY_SIZE, &prng);
    print_spans(array, TEST_ARRAY_SIZE);
    for (i = 0; i < TEST_ARRAY_SIZE; i++) {
      sparsemap_set(map, array[i], true);
      assert(sparsemap_is_set(map, array[i]) == true);
    }
    has_span(map, array, TEST_ARRAY_SIZE, len);
    size_t l = sparsemap_span(map, 0, len);
    if (l != (size_t)-1) {
      __diag("Found span in map starting at %lu of length %lu\n", l, len);
      __diag("is_span(%lu, %lu) == %s\n", l, len, is_span(array, TEST_ARRAY_SIZE, l, len) ? "yes" : "no");
      i = (int)l;
      do {
        bool set = sparsemap_is_set(map, i);
        if (set) {
          __diag("verified %d was set\n", i);
        } else {
          __diag("darn, %d was not really set, %s\n", i, was_set(i, array) ? "but we thought it was" : "because it wasn't");
        }
      } while (++i < l + len);
    } else {
      __diag("UNABLE TO FIND SPAN in map of length %lu\n", len);
    }
  }
  return 0;
}
