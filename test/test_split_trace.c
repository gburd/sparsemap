#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sparsemap.h"

struct sparsemap {
    size_t m_capacity;
    size_t m_data_used;
    uint8_t *m_data;
};

int main(void) {
    size_t map_size = 1024 * 1024;
    sparsemap_t *map = sparsemap(map_size);
    uint8_t portion_buf[4096] = { 0 };
    sparsemap_t portion;

    sparsemap_init(&portion, portion_buf, sizeof(portion_buf));

    /* Populate with exactly 24534 bits starting at 0 */
    size_t amt = 24534;
    for (size_t i = 0; i < amt; i++) {
        sparsemap_add(map, i);
    }

    fprintf(stderr, "Total bits set: %zu\n", sparsemap_cardinality(map));

    /* Replicate the test loop */
    for (size_t i = 0; i < amt + 2049; i++) {
        sparsemap_clear(&portion);
        size_t rank = sparsemap_rank(map, 0, i, 1);
        sparsemap_split(map, i + 1, &portion);

        size_t count_map = sparsemap_cardinality(map);
        size_t count_portion = sparsemap_cardinality(&portion);

        if (count_map != rank) {
            fprintf(stderr, "FAIL at i=%zu: map count %zu != rank %zu\n",
                    i, count_map, rank);
            fprintf(stderr, "  portion count=%zu, sum=%zu, expected total=%zu\n",
                    count_portion, count_map + count_portion, amt);
            return 1;
        }
        if (count_portion != amt - rank) {
            fprintf(stderr, "FAIL at i=%zu: portion count %zu != expected %zu\n",
                    i, count_portion, amt - rank);
            fprintf(stderr, "  map count=%zu, sum=%zu, expected total=%zu\n",
                    count_map, count_map + count_portion, amt);
            return 1;
        }

        sparsemap_union(map, &portion);

        size_t count_after = sparsemap_cardinality(map);
        if (count_after != amt) {
            fprintf(stderr, "FAIL merge at i=%zu: count %zu != %zu\n",
                    i, count_after, amt);
            return 1;
        }

        /* Print progress occasionally */
        if (i % 1000 == 0 || (i >= 2100 && i <= 2120)) {
            fprintf(stderr, "i=%zu: OK (rank=%zu)\n", i, rank);
        }
    }

    fprintf(stderr, "All iterations passed!\n");
    free(map);
    return 0;
}
