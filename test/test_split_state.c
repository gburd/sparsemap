#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sparsemap.h>

// Expose internal structure for debugging
struct sparsemap {
  size_t m_capacity;
  size_t m_data_used;
  uint8_t *m_data;
};

void print_map_state(const char *name, sparsemap_t *map) {
    struct sparsemap *m = (struct sparsemap *)map;
    printf("\n%s state:\n", name);
    printf("  capacity: %zu\n", m->m_capacity);
    printf("  data_used: %zu\n", m->m_data_used);
    printf("  chunk_count: %u\n", *(uint32_t *)m->m_data);
    printf("  starting_offset: %lu\n", sparsemap_minimum(map));
    printf("  ending_offset: %lu\n", sparsemap_maximum(map));
    printf("  count: %zu\n", sparsemap_cardinality(map));

    // Print first few bytes of data
    printf("  data[0-15]: ");
    for (int i = 0; i < 16 && i < m->m_data_used; i++) {
        printf("%02x ", m->m_data[i]);
    }
    printf("\n");
}

int main() {
    printf("Creating maps and setting up split scenario...\n");
    sparsemap_t *map = sparsemap(10 * 1024);
    sparsemap_t *other = sparsemap(1024);

    printf("\nSetting bits 2049-4095 in map...\n");
    for (int i = 2049; i < 4096; i++) {
        sparsemap_add(map, i);
    }

    print_map_state("map BEFORE split", map);
    print_map_state("other BEFORE split", other);

    printf("\n========== SPLIT at 2051 ==========\n");
    sparsemap_idx_t split_result = sparsemap_split(map, 2051, other);
    printf("Split returned: %lu\n", split_result);

    print_map_state("map AFTER split", map);
    print_map_state("other AFTER split", other);

    printf("\n========== MERGE ==========\n");
    printf("About to merge...\n");
    fflush(stdout);

    sparsemap_union(map, other);

    printf("Merge completed successfully!\n");
    print_map_state("map AFTER merge", map);

    free(map);
    free(other);
    return 0;
}
