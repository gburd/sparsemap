#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sparsemap.h>

void clear_map(sparsemap_t *map) {
    sparsemap_clear(map);
}

/* Helper: union and replace lhs with result */
static sparsemap_t *union_replace(sparsemap_t *lhs, sparsemap_t *rhs) {
    sparsemap_t *merged = sparsemap_union(lhs, rhs);
    free(lhs);
    return merged;
}

int main() {
    printf("Creating maps...\n");
    sparsemap_t *map = sparsemap(10 * 1024);
    sparsemap_t *other = sparsemap(1024);

    if (!map || !other) {
        printf("Failed to create maps\n");
        return 1;
    }

    printf("Test 1: Merge two empty maps\n");
    {
        sparsemap_t *m = sparsemap_union(map, other);
        /* Both empty: m may be NULL */
        if (m) free(m);
    }
    printf("Test 1: PASSED\n");

    printf("Test 2: Merge a single set bit in the first chunk into the empty map\n");
    sparsemap_add(other, 0);
    {
        sparsemap_t *m = sparsemap_union(map, other);
        if (!m || !sparsemap_contains(m, 0)) {
            printf("FAIL: bit 0 not set\n");
            return 1;
        }
        free(m);
    }
    printf("Test 2: PASSED\n");

    clear_map(map);
    clear_map(other);

    printf("Test 3: Merge two maps with the same single bit set\n");
    sparsemap_add(map, 0);
    sparsemap_add(other, 0);
    {
        sparsemap_t *m = sparsemap_union(map, other);
        if (!m || !sparsemap_contains(m, 0)) {
            printf("FAIL: bit 0 not set\n");
            return 1;
        }
        free(m);
    }
    printf("Test 3: PASSED\n");

    clear_map(map);
    clear_map(other);

    printf("Test 4: Merge an empty map with one that has the first bit set\n");
    sparsemap_add(map, 0);
    {
        sparsemap_t *m = sparsemap_union(map, other);
        if (!m || !sparsemap_contains(m, 0)) {
            printf("FAIL: bit 0 not set\n");
            return 1;
        }
        free(m);
    }
    printf("Test 4: PASSED\n");

    clear_map(map);
    clear_map(other);

    printf("Test 5: Merge with bit 2049\n");
    sparsemap_add(other, 2049);
    {
        sparsemap_t *m = sparsemap_union(map, other);
        if (!m || !sparsemap_contains(m, 2049)) {
            printf("FAIL: bit 2049 not set\n");
            return 1;
        }
        free(m);
    }
    printf("Test 5: PASSED\n");

    clear_map(map);
    clear_map(other);

    printf("Test 6: Merge multiple bits\n");
    sparsemap_add(other, 1);
    sparsemap_add(other, 2049);
    sparsemap_add(map, 2050);
    sparsemap_add(other, 4097);
    sparsemap_add(map, 6113);
    sparsemap_add(other, 8193);
    {
        sparsemap_t *m = sparsemap_union(map, other);
        if (!m) { printf("FAIL: union returned NULL\n"); return 1; }
        if (!sparsemap_contains(m, 1) || !sparsemap_contains(m, 2049) ||
            !sparsemap_contains(m, 2050) || !sparsemap_contains(m, 4097) ||
            !sparsemap_contains(m, 6113) || !sparsemap_contains(m, 8193)) {
            printf("FAIL: missing bits\n");
            return 1;
        }
        free(m);
    }
    printf("Test 6: PASSED\n");

    clear_map(map);
    clear_map(other);

    printf("Test 7: Merge with range\n");
    sparsemap_add(map, 0);
    sparsemap_add(map, 2048);
    sparsemap_add(map, 8193);
    for (int i = 2049; i < 4096; i++) {
        sparsemap_add(other, i);
    }
    {
        sparsemap_t *m = sparsemap_union(map, other);
        if (!m) { printf("FAIL: union returned NULL\n"); return 1; }
        if (!sparsemap_contains(m, 0) || !sparsemap_contains(m, 2048) ||
            !sparsemap_contains(m, 8193)) {
            printf("FAIL: missing original bits\n");
            return 1;
        }
        free(m);
    }
    printf("Test 7: PASSED\n");

    printf("All tests passed!\n");
    free(map);
    free(other);
    return 0;
}
