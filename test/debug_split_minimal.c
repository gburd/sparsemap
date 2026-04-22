#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "sparsemap.h"

int main() {
    uint8_t buf1[1024] = {0};
    uint8_t buf2[1024] = {0};
    sparsemap_t *map = sparsemap_new(buf1, 1024, SPARSEMAP_USE_BUFFER_ALLOCATED_BY_USER);
    sparsemap_t *portion = sparsemap_new(buf2, 1024, SPARSEMAP_USE_BUFFER_ALLOCATED_BY_USER);

    // Populate with RLE data similar to the test
    for (size_t i = 0; i < 100; i++) {
        sparsemap_add(map, i);
    }

    printf("Map populated with %zu bits set\n", sparsemap_cardinality(map));
    printf("Attempting split at index 50...\n");
    fflush(stdout);

    sparsemap_idx_t result = sparsemap_split(map, 50, portion);

    printf("Split completed. Result: %lu\n", result);
    printf("Map count: %zu, Portion count: %zu\n",
           sparsemap_cardinality(map), sparsemap_cardinality(portion));

    sparsemap_free(&map);
    sparsemap_free(&portion);
    return 0;
}
