#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "sparsemap.h"

// Random number generation (simple LCG)
static uint32_t seed = 0xf3da5dab;  // From failing test

int rand_int_range(int min, int max) {
    seed = seed * 1103515245 + 12345;
    return min + (seed % (max - min + 1));
}

// Matching test's populate_map_rle
size_t populate_map_rle(sparsemap_t *map, size_t loc, size_t num, size_t amount) {
    size_t len = rand_int_range(1, num) * amount;
    printf("Random length: rand_int_range(1, %zu) * %zu = %zu\n", num, amount, len);
    for (size_t i = 0; i < len; i++) {
        sparsemap_add(map, loc + i);
    }
    return len;
}

int main() {
    sparsemap_t *map = sparsemap(100 * 1024);

    // Test parameters from failing test
    size_t start = 13012;
    size_t n = populate_map_rle(map, start, 10, 2718);  // Match test params

    printf("Populated %zu bits starting at %zu\n", n, start);
    printf("Count: %zu\n", sparsemap_cardinality(map));

    sparsemap_idx_t end1 = sparsemap_maximum(map);
    printf("Ending offset after populate: %zu (expected %zu)\n", end1, start + n - 1);

    if (end1 != start + n - 1) {
        printf("ERROR: First assertion would fail!\n");
    }

    // Now set bit 100 positions after the run
    sparsemap_idx_t new_bit = start + n + 100;
    printf("\nSetting bit at %zu\n", new_bit);
    sparsemap_idx_t result = sparsemap_add(map, new_bit);
    printf("sparsemap_set returned: %zu (SPARSEMAP_IDX_MAX=%zu)\n", result, SPARSEMAP_IDX_MAX);

    printf("Count after set: %zu\n", sparsemap_cardinality(map));

    sparsemap_idx_t end2 = sparsemap_maximum(map);
    printf("Ending offset after set: %zu (expected %zu)\n", end2, new_bit);

    if (end2 != new_bit) {
        printf("ERROR: Second assertion fails! Off by %ld\n", (long)(new_bit - end2));

        // Debug: check if the bit is actually set
        if (sparsemap_contains(map, new_bit)) {
            printf("Bit IS set at %zu\n", new_bit);
        } else {
            printf("Bit is NOT set at %zu!\n", new_bit);
        }
    } else {
        printf("SUCCESS!\n");
    }

    free(map);
    return (end2 == new_bit) ? 0 : 1;
}
