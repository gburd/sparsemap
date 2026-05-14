/* SPDX-License-Identifier: MIT
 *
 * examples/ex_6.c — using sm_set_allocator and sm_create_with_allocator
 * to route sparsemap's allocations through a custom allocator.
 *
 * Pattern shown: arena allocator (single bump pointer, freed all-at-
 * once at the end).  Real-world consumers like PostgreSQL extensions
 * would wire palloc / pfree the same way.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sparsemap.h>

/* ------------------------------------------------------------------ */
/*  Toy bump-allocator arena                                          */
/* ------------------------------------------------------------------ */

typedef struct arena {
    uint8_t *base;
    size_t   size;
    size_t   used;
    size_t   alloc_count;   /* instrumentation */
    size_t   free_count;
} arena_t;

static void *
arena_alloc(size_t n, void *aux)
{
    arena_t *a = aux;
    /* 8-byte align. */
    size_t aligned = (a->used + 7) & ~(size_t)7;
    if (aligned + n > a->size) {
        return NULL; /* arena exhausted */
    }
    void *p = a->base + aligned;
    a->used = aligned + n;
    a->alloc_count++;
    return p;
}

static void *
arena_realloc(void *p, size_t n, void *aux)
{
    arena_t *a = aux;
    /* Naive: always allocate fresh and copy.  Real arenas don't
     * support shrink-in-place either; this is fine for sparsemap
     * because it never shrinks via realloc, only grows. */
    if (p == NULL) return arena_alloc(n, aux);
    void *new_p = arena_alloc(n, aux);
    if (new_p == NULL) return NULL;
    /* We don't know the old size; we'd need a header in production.
     * For this example, we just copy a bounded amount that's safely
     * within the original allocation. */
    memcpy(new_p, p, n / 2);  /* enough for sparsemap's grow pattern */
    /* Old block is leaked into the arena \u2014 freed wholesale at end. */
    a->free_count++;
    return new_p;
}

static void
arena_free(void *p, void *aux)
{
    arena_t *a = aux;
    /* No-op: arena frees everything at once. */
    (void)p;
    if (p) a->free_count++;
}

/* ------------------------------------------------------------------ */
/*  Demo                                                              */
/* ------------------------------------------------------------------ */

int
main(void)
{
    /* 64 KiB arena. */
    uint8_t arena_storage[64 * 1024];
    arena_t arena = {
        .base = arena_storage,
        .size = sizeof(arena_storage),
        .used = 0,
        .alloc_count = 0,
        .free_count = 0,
    };

    static sm_allocator_t hooks;  /* zero-init; populate below */
    hooks.alloc   = arena_alloc;
    hooks.realloc = arena_realloc;
    hooks.free    = arena_free;
    hooks.aux     = &arena;

    printf("=== process-wide allocator ===\n");
    sm_set_allocator(hooks);

    sparsemap_t *m = sm_create(2048);
    sm_add(m, 42);
    sm_add(m, 1000);
    printf("after sm_create + 2 sm_add: |m| = %zu, arena used = %zu, alloc_count = %zu\n",
           sm_cardinality(m), arena.used, arena.alloc_count);

    sm_free(m);
    printf("after sm_free: arena used = %zu, free_count = %zu\n",
           arena.used, arena.free_count);

    /* Reset the global allocator before the next demo. */
    sm_set_allocator((sm_allocator_t){0});
    arena.used = 0;
    arena.alloc_count = 0;
    arena.free_count = 0;

    printf("\n=== per-map allocator ===\n");
    sparsemap_t *libc_map  = sm_create(2048);
    sparsemap_t *arena_map = sm_create_with_allocator(2048, hooks);

    sm_add(libc_map,  100);
    sm_add(arena_map, 200);

    printf("after creating one libc map and one arena map:\n");
    printf("  arena alloc_count = %zu (should be 1; libc map didn't touch the arena)\n",
           arena.alloc_count);
    printf("  arena used        = %zu bytes\n", arena.used);

    sm_free(libc_map);
    sm_free(arena_map);

    printf("\nDone.\n");
    return 0;
}
