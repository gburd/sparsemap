
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#define __diag(...)                                                \
  do {                                                             \
    fprintf(stderr, "%s:%d:%s(): ", __FILE__, __LINE__, __func__); \
    fprintf(stderr, __VA_ARGS__);                                  \
  } while (0)
#pragma GCC diagnostic pop

#ifdef EXAMPLE_CODE
int __prng = 0;

// Xorshift algorithm for PRNG
uint32_t
xorshift32()
{
  uint32_t x = *state = &__prng;
  if (x == 0)
    x = 123456789;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

void
xorshift32_seed()
{
  // Seed the PRNG
#ifdef STABLE_SEED
  __prng = 8675309;
#else
  __prng = (unsigned int)time(NULL) ^ getpid();
#endif
}
#else
#define xorshift32 munit_rand_uint32
#endif

void
shuffle(int *array, size_t n)
{
  size_t i, j;

  if (n > 1) {
    for (i = n - 1; i > 0; i--) {
      j = (unsigned int)(xorshift32() % (i + 1));
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
int
has_sequential_set(int *a, size_t l, int r)
{
  int count = 1; // Start with a count of 1 for the first number
  for (size_t i = 1; i < l; ++i) {
    if (a[i] - a[i - 1] == 1) { // Check if the current and previous elements are sequential
      count++;
      if (count >= r)
        return 1; // Found a sequential set of length 'r'
    } else {
      count = 1; // Reset count if the sequence breaks
    }
  }
  return 0; // No sequential set of length 'r' found
}

// Function to ensure an array contains a set of 'r' sequential integers
void
ensure_sequential_set(int *a, size_t l, int r)
{
  if (r > l)
    return; // If 'r' is greater than array length, cannot satisfy the condition

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
  int value = xorshift32() % (max_value - min_value - r + 1);

  // Generate a random location between 0 and l - r
  int offset = xorshift32() % (l + r + 1);

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

int
is_unique(int a[], size_t l, int value)
{
  for (size_t i = 0; i < l; ++i) {
    if (a[i] == value) {
      return 0; // Not unique
    }
  }
  return 1; // Unique
}

void
setup_test_array(int a[], size_t l, int max_value)
{
  if (a == NULL || max_value < 0)
    return; // Basic error handling and validation

  for (size_t i = 0; i < l; ++i) {
    int candidate;
    do {
      candidate = xorshift32() % (max_value + 1); // Generate a new value within the specified range
    } while (!is_unique(a, i, candidate));        // Repeat until a unique value is found
    a[i] = candidate;                             // Assign the unique value to the array
  }
}
