/* SPDX-License-Identifier: MIT
 *
 * examples/ex_6.c -- using sm_set_allocator to route sparsemap's
 * allocations through a custom, process-wide allocator (CRoaring style).
 *
 * Pattern shown: arena allocator (single bump pointer, freed all-at-
 * once at the end).  Real-world consumers like PostgreSQL extensions
 * would wire palloc / pfree the same way.  The hooks are process-global
 * and take no aux pointer, so the arena lives in a file-static.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sm.h>

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

/* The CRoaring-style hooks are process-global and pass no aux pointer,
 * so the arena they bump against is a file-static. */
static uint8_t g_arena_storage[64 * 1024];
static arena_t g_arena = {
    .base = g_arena_storage,
    .size = sizeof(g_arena_storage),
    .used = 0,
    .alloc_count = 0,
    .free_count = 0,
};

static void *
arena_alloc(size_t n)
{
    arena_t *a = &g_arena;
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
arena_realloc(void *p, size_t n)
{
    arena_t *a = &g_arena;
    /* Naive: always allocate fresh and copy.  Real arenas don't
     * support shrink-in-place either; this is fine for sparsemap
     * because it never shrinks via realloc, only grows. */
    if (p == NULL) return arena_alloc(n);
    void *new_p = arena_alloc(n);
    if (new_p == NULL) return NULL;
    /* We don't know the old size; we'd need a header in production.
     * For this example, we just copy a bounded amount that's safely
     * within the original allocation. */
    memcpy(new_p, p, n / 2);  /* enough for sparsemap's grow pattern */
    /* Old block is leaked into the arena -- freed wholesale at end. */
    a->free_count++;
    return new_p;
}

static void
arena_free(void *p)
{
    arena_t *a = &g_arena;
    /* No-op: arena frees everything at once. */
    if (p) a->free_count++;
}

/* ------------------------------------------------------------------ */
/*  Demo                                                              */
/* ------------------------------------------------------------------ */

int
main(void)
{
    static sm_allocator_t hooks;  /* zero-init; populate below */
    hooks.malloc  = arena_alloc;
    hooks.realloc = arena_realloc;
    hooks.free    = arena_free;

    printf("=== process-wide allocator ===\n");
    sm_set_allocator(hooks);

    sm_t *m = sm_create(2048);
    sm_add(m, 42);
    sm_add(m, 1000);
    printf("after sm_create + 2 sm_add: |m| = %zu, arena used = %zu, alloc_count = %zu\n",
           sm_cardinality(m), g_arena.used, g_arena.alloc_count);

    sm_free(m);
    printf("after sm_free: arena used = %zu, free_count = %zu\n",
           g_arena.used, g_arena.free_count);

    /* Reset the global allocator and verify a libc map bypasses the arena. */
    sm_set_allocator((sm_allocator_t){0});
    const size_t alloc_count_before = g_arena.alloc_count;

    printf("\n=== libc allocator (after reset) ===\n");
    sm_t *libc_map = sm_create(2048);
    sm_add(libc_map, 100);
    printf("after creating one libc map:\n");
    printf("  arena alloc_count = %zu (unchanged; libc map didn't touch the arena)\n",
           g_arena.alloc_count);
    assert(g_arena.alloc_count == alloc_count_before);

    sm_free(libc_map);

    printf("\nDone.\n");
    return 0;
}
