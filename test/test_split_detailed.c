#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sparsemap.h>

// Expose internal structure for debugging
struct sparsemap {
  size_t m_capacity;
  size_t m_data_used;
  uint8_t *m_data;
};

void print_detailed_state(const char *name, sparsemap_t *map) {
    struct sparsemap *m = (struct sparsemap *)map;
    size_t api_size = sparsemap_get_size(map);
    size_t chunk_count = *(uint32_t *)m->m_data;  // First 4 bytes are chunk count

    printf("\n=== %s ===\n", name);
    printf("Metadata:\n");
    printf("  m_capacity: %zu\n", m->m_capacity);
    printf("  m_data_used (cached): %zu\n", m->m_data_used);
    printf("  sparsemap_get_size (API): %zu\n", api_size);
    printf("  MISMATCH: %s\n", m->m_data_used == api_size ? "NO" : "YES!");
    printf("  chunk_count (from m_data[0-3]): %zu\n", chunk_count);
    printf("  m_data pointer: %p\n", (void *)m->m_data);

    printf("Public API:\n");
    printf("  sparsemap_cardinality(): %zu\n", sparsemap_cardinality(map));
    printf("  sparsemap_minimum(): %lu\n", sparsemap_minimum(map));
    printf("  sparsemap_maximum(): %lu\n", sparsemap_maximum(map));
    printf("  sparsemap_get_capacity(): %zu\n", sparsemap_get_capacity(map));

    printf("Raw data bytes [0-31]:\n  ");
    for (int i = 0; i < 32 && i < m->m_capacity; i++) {
        printf("%02x ", m->m_data[i]);
        if (i == 15) printf("\n  ");
    }
    printf("\n");
}

int main() {
    printf("=================================================================\n");
    printf("DETAILED SPLIT STATE ANALYSIS\n");
    printf("=================================================================\n");

    sparsemap_t *map = sparsemap(10 * 1024);
    sparsemap_t *other = sparsemap(1024);

    printf("\n>>> Setting bits 2049-4095 in map...\n");
    for (int i = 2049; i < 4096; i++) {
        sparsemap_add(map, i);
    }

    print_detailed_state("MAP BEFORE SPLIT", map);
    print_detailed_state("OTHER BEFORE SPLIT", other);

    printf("\n=================================================================\n");
    printf(">>> CALLING sparsemap_split(map, 2051, other)\n");
    printf("=================================================================\n");
    fflush(stdout);

    sparsemap_idx_t split_result = sparsemap_split(map, 2051, other);

    printf(">>> Split returned: %lu\n", split_result);
    fflush(stdout);

    print_detailed_state("MAP AFTER SPLIT", map);
    print_detailed_state("OTHER AFTER SPLIT", other);

    printf("\n=================================================================\n");
    printf(">>> CALLING sparsemap_union(map, other)\n");
    printf("=================================================================\n");
    fflush(stdout);

    size_t result = sparsemap_union(map, other);

    printf(">>> Merge returned: %zu (0 = success)\n", result);

    print_detailed_state("MAP AFTER MERGE", map);

    printf("\n=================================================================\n");
    printf("SUCCESS: All operations completed without crash!\n");
    printf("=================================================================\n");

    free(map);
    free(other);
    return 0;
}
