#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <sparsemap.h>

void sigbus_handler(int sig) {
    fprintf(stderr, "\n*** SIGBUS CAUGHT ***\n");
    fprintf(stderr, "Signal: %d\n", sig);
    exit(138);
}

int main() {
    signal(SIGBUS, sigbus_handler);

    printf("Test 1: Simple split\n");
    sparsemap_t *map = sparsemap(1024);
    sparsemap_t *other = sparsemap(1024);

    printf("Setting bits 2049-4095...\n");
    for (int i = 2049; i < 4096; i++) {
        sparsemap_set(map, i);
    }

    printf("About to split at 2051...\n");
    fflush(stdout);
    sparsemap_idx_t result = sparsemap_split(map, 2051, other);
    printf("Split returned: %u\n", result);

    printf("Verifying split...\n");
    for (int i = 2049; i < 4096; i++) {
        bool in_map = sparsemap_is_set(map, i);
        bool in_other = sparsemap_is_set(other, i);
        if (i < 2051) {
            if (!in_map || in_other) {
                printf("ERROR at i=%d: map=%d other=%d (expected map=1 other=0)\n", i, in_map, in_other);
                return 1;
            }
        } else {
            if (in_map || !in_other) {
                printf("ERROR at i=%d: map=%d other=%d (expected map=0 other=1)\n", i, in_map, in_other);
                return 1;
            }
        }
    }

    printf("Test 1: PASSED\n");

    printf("\nTest 2: Merge after split\n");
    fflush(stdout);
    sparsemap_merge(map, other);
    printf("Merge completed\n");
    fflush(stdout);

    for (int i = 2049; i < 4096; i++) {
        if (!sparsemap_is_set(map, i)) {
            printf("ERROR: bit %d not set after merge\n", i);
            return 1;
        }
    }

    printf("Test 2: PASSED\n");
    printf("\nAll tests passed!\n");

    free(map);
    free(other);
    return 0;
}
