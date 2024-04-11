#include <sys/types.h>

#include <assert.h>
#include <pthread.h>
#include <sparsemap.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "common.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#define __diag(...)                                                \
  do {                                                             \
    fprintf(stderr, "%s:%d:%s(): ", __FILE__, __LINE__, __func__); \
    fprintf(stderr, __VA_ARGS__);                                  \
  } while (0)
#pragma GCC diagnostic pop

uint64_t
tsc(void)
{
  uint32_t low, high;
  __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
  return ((uint64_t)high << 32) | low;
}

static
uint64_t get_tsc_frequency() {
    uint32_t high, low;
    __asm__ volatile("rdtsc" : "=a" (low), "=d" (high));
    __asm__ volatile("rdtsc");
    return ((uint64_t)high << 32) | low;
}

double
tsc_ticks_to_ns(uint64_t tsc_ticks) {
    static uint64_t tsc_freq = 0;
    if (tsc_freq == 0) {
        tsc_freq = get_tsc_frequency();
    }
    return (double)tsc_ticks / (double)tsc_freq * 1e9;
}

void
est_sift_up(uint64_t *heap, int child_index)
{
  while (child_index > 0) {
    int parent_index = (child_index - 1) / 2;
    if (heap[parent_index] > heap[child_index]) {
      // Swap parent and child
      uint64_t temp = heap[parent_index];
      heap[parent_index] = heap[child_index];
      heap[child_index] = temp;
      child_index = parent_index;
    } else {
      break; // Heap property satisfied
    }
  }
}

void
est_sift_down(uint64_t *heap, int heap_size, int parent_index)
{
  int child_index = 2 * parent_index + 1; // Left child
  while (child_index < heap_size) {
    // Right child exists and is smaller than left child
    if (child_index + 1 < heap_size && heap[child_index + 1] < heap[child_index]) {
      child_index++;
    }
    // If the smallest child is smaller than the parent, swap them
    if (heap[child_index] < heap[parent_index]) {
      uint64_t temp = heap[child_index];
      heap[child_index] = heap[parent_index];
      heap[parent_index] = temp;
      parent_index = child_index;
      child_index = 2 * parent_index + 1;
    } else {
      break; // Heap property satisfied
    }
  }
}

void
est_insert_value(uint64_t *heap, int heap_max_size, int *heap_size, uint64_t value)
{
  if (*heap_size < heap_max_size) { // Heap not full, insert value
    heap[*heap_size] = value;
    est_sift_up(heap, *heap_size);
    (*heap_size)++;
  } else {
    // Heap is full, replace root with new value with a certain probability
    // This is a very naive approach to maintain a sample of the input
    if (rand() % 2) {
      heap[0] = value;
      est_sift_down(heap, heap_max_size, 0);
    }
  }
}

int __xorshift32_state = 0;

// Xorshift algorithm for PRNG
uint32_t
xorshift32()
{
  uint32_t x = __xorshift32_state;
  if (x == 0)
    x = 123456789;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  __xorshift32_state = x;
  return x;
}

void
xorshift32_seed()
{
  __xorshift32_state = XORSHIFT_SEED_VALUE;
}

void
shuffle(int *array, size_t n) // TODO working?
{
  for (size_t i = n - 1; i > 0; --i) {
    size_t j = xorshift32() % (i + 1);
    if (i != j) {
      array[i] ^= array[j];
      array[j] ^= array[i];
      array[i] ^= array[j];
    }
  }
}

int
compare_ints(const void *a, const void *b)
{
  return *(const int *)a - *(const int *)b;
}

// Check if there's already a sequence of 'r' sequential integers
int
has_sequential_set(int a[], int l, int r)
{
  // Start with a count of 1 for the first number
  int count = 1;
  for (int i = 1; i < l; ++i) {
    // Check if the current and previous elements are sequential
    if (a[i] - a[i - 1] == 1) {
      count++;
      if (count >= r) {
        // Found a sequential set of length 'r' starting at 'i'
        return i;
      }
    } else {
      // Reset count if the sequence breaks
      count = 1;
    }
  }
  // No sequential set of length 'r' found
  return -1;
}

// Function to ensure an array contains a set of 'r' sequential integers
int
ensure_sequential_set(int a[], int l, int r)
{
  if (!a || l == 0 || r < 1 || r > l) {
    return 0;
  }

  // Sort the array to check for existing sequences
  qsort(a, l, sizeof(int), compare_ints);

  // Check if a sequential set of length 'r' already exists
  int offset = has_sequential_set(a, l, r);
  if (offset >= 0) {
    return offset; // Sequence already exists, no modification needed
  }

  // Find the minimum and maximum values in the array
  int min_value = a[0];
  int max_value = a[l - 1];

  // Generate a random value between min_value and max_value
  int value = random_uint32() % (max_value - min_value - r + 1);
  // Generate a random location between 0 and l - r
  offset = random_uint32() % (l - r - 1);

  // Adjust the array to include a sequential set of 'r' integers at the random offset
  for (int i = 0; i < r; ++i) {
    a[i + offset] = value + i;
  }
  return value;
}

int
create_sequential_set_in_empty_map(sparsemap_t *map, int s, int r)
{
  int placed_at;
  if (s >= r + 1) {
    placed_at = 0;
  } else {
    placed_at = random_uint32() % (s - r - 1);
  }
  for (int i = placed_at; i < placed_at + r; i++) {
    sparsemap_set(map, i, true);
  }
  return placed_at;
}

void
print_array(int *array, int l)
{
  int a[l];
  memcpy(a, array, sizeof(int) * l);
  qsort(a, l, sizeof(int), compare_ints);

  fprintf(stderr, "int a[] = {");
  for (int i = 0; i < l; i++) {
    fprintf(stderr, "%d", a[i]);
    if (i != l - 1) {
      fprintf(stderr, ", ");
    }
  }
  fprintf(stderr, "};\n");
}

bool
has_span(sparsemap_t *map, int *array, int l, int n)
{
  if (n == 0 || l == 0 || n > l) {
    return false;
  }

  int sorted[l];
  memcpy(sorted, array, sizeof(int) * l);
  qsort(sorted, l, sizeof(int), compare_ints);

  for (int i = 0; i <= l - n; i++) {
    if (sorted[i] + n - 1 == sorted[i + n - 1]) {
      for (int j = 0; j < n; j++) {
        size_t pos = sorted[j + i];
        bool set = sparsemap_is_set(map, pos);
        assert(set);
      }
      __diag("Found span: [%d, %d], length: %d\n", sorted[i], sorted[i + n - 1], n);
      return true;
    }
  }

  return false;
}

bool
is_span(int *array, int n, int x, int l)
{
  if (n == 0 || l < 0) {
    return false;
  }

  int a[n];
  memcpy(a, array, sizeof(int) * n);
  qsort(a, n, sizeof(int), compare_ints);

  // Iterate through the array to find a span starting at x of length l
  for (int i = 0; i < n; i++) {
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
print_spans(int *array, int n)
{
  int a[n];
  size_t start = 0, end = 0;

  if (n == 0) {
    fprintf(stderr, "Array is empty\n");
    return;
  }

  memcpy(a, array, sizeof(int) * n);
  qsort(a, n, sizeof(int), compare_ints);

  for (int i = 1; i < n; i++) {
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
is_set(const int array[], int bit)
{
  for (int i = 0; i < 1024; i++) {
    if (array[i] == (int)bit) {
      return true;
    }
  }
  return false;
}

int
is_unique(int a[], int l, int value)
{
  for (int i = 0; i < l; ++i) {
    if (a[i] == value) {
      return 0; // Not unique
    }
  }
  return 1; // Unique
}

void
setup_test_array(int a[], int l, int max_value)
{
  if (a == NULL || max_value < 0)
    return; // Basic error handling and validation

  for (int i = 0; i < l; ++i) {
    int candidate;
    do {
      candidate = random_uint32() % (max_value + 1); // Generate a new value within the specified range
    } while (!is_unique(a, i, candidate));           // Repeat until a unique value is found
    a[i] = candidate;                                // Assign the unique value to the array
  }
}

void
bitmap_from_uint32(sparsemap_t *map, uint32_t number)
{
  for (int i = 0; i < 32; i++) {
    bool bit = number & (1 << i);
    sparsemap_set(map, i, bit);
  }
}

void
bitmap_from_uint64(sparsemap_t *map, uint64_t number)
{
  for (int i = 0; i < 64; i++) {
    bool bit = number & (1 << i);
    sparsemap_set(map, i, bit);
  }
}

uint32_t
rank_uint64(uint64_t number, int n, int p)
{
  if (p < n || p > 63) {
    return 0;
  }

  /* Create a mask for the range between n and p.
     This works by shifting 1 to the left (p+1) times, subtracting 1 to have
     a sequence of p 1's, then shifting n times to the left to position it
     starting at n. Finally, subtracting (1 << n) - 1 removes the bits below
     n from the mask. */
  uint64_t mask = ((uint64_t)1 << (p + 1)) - 1 - (((uint64_t)1 << n) - 1);

  /* Apply the mask and count the set bits in the result. */
  uint64_t maskedNumber = number & mask;

  /* Count the bits set in maskedNumber. */
  uint32_t count = 0;
  while (maskedNumber) {
    count += maskedNumber & 1;
    maskedNumber >>= 1;
  }

  return count;
}

int
whats_set_uint64(uint64_t number, int pos[64])
{
  int length = 0;

  for (int i = 0; i < 64; i++) {
    if (number & ((uint64_t)1 << i)) {
      pos[length++] = i;
    }
  }

  return length;
}

void
whats_set(sparsemap_t *map, int m)
{
  logf("what's set in the range [0, %d): ", m);
  for (int i = 0; i < m; i++) {
    if (sparsemap_is_set(map, i)) {
      logf("%d ", i);
    }
  }
  logf("\n");
}
