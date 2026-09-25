/* SPDX-License-Identifier: MIT
 *
 * tests/fuzz_mutate.c -- libFuzzer harness for the *write* paths.
 *
 * tests/fuzz_deserialize.c only reads the decoded map, so it can
 * never reach the mutating code where most of the 2026-09 review
 * crashes lived.  This harness decodes an untrusted buffer through
 * sm_open / sm_open_copy / sm_deserialize and then runs the full
 * mutation battery on the survivors: add/remove, add_range/
 * remove_range, all four set operations, offset in both
 * directions, and split.  Goal: trip ASan / UBSan / heap
 * corruption / infinite-loop hangs on write paths.
 *
 * Build:
 *   meson setup builddir-fuzz -Dfuzz=enabled
 *   ninja -C builddir-fuzz
 *
 * Run:
 *   ./builddir-fuzz/tests/fuzz_mutate tests/fuzz-corpus/ \
 *     -max_total_time=300 -print_final_stats=1
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sm.h>

/* Reasonable input cap: refuse buffers that exceed it.  Real
 * sparsemaps in pg_tre top out around 16 KiB; 64 KiB is a generous
 * upper bound that still keeps fuzzing fast. */
#define FUZZ_MAX_INPUT 65536

/* Exercise a deserialized map enough to trip read-side bugs.  We
 * don't add bits here (that would create new SM_IDX_MAX paths the
 * fuzzer can't distinguish from corruption); we only read. */
static void
exercise_readonly(sm_t *m)
{
    /* Cardinality + a popcount walk through every chunk. */
    (void)sm_cardinality(m);
    (void)sm_get_size(m);
    (void)sm_is_empty(m);

    /* Membership at boundaries. */
    (void)sm_contains(m, 0, NULL);
    (void)sm_contains(m, 1, NULL);
    (void)sm_contains(m, 63, NULL);
    (void)sm_contains(m, 64, NULL);
    (void)sm_contains(m, 65535, NULL);
    (void)sm_contains(m, SM_IDX_MAX - 1, NULL);

    /* Iteration: visit up to 256 set bits, then stop.  prev_idx =
     * SM_IDX_MAX is the documented sentinel for "start at first set". */
    uint64_t idx = SM_IDX_MAX;
    for (int i = 0; i < 256; i++) {
        idx = sm_next_member(m, idx, NULL);
        if (idx == SM_IDX_MAX) break;
    }

    /* Validate: must not crash on malformed input. */
    (void)sm_validate(m);

    /* Stats walk. */
    sm_stats_t s;
    sm_statistics(m, &s);

    /* Round-trip: re-serialize and ensure the size is sane. */
    size_t out_n = sm_serialized_size(m);
    if (out_n > 0 && out_n < FUZZ_MAX_INPUT * 2) {
        uint8_t *buf = (uint8_t *)malloc(out_n);
        if (buf) {
            size_t written = sm_serialize(m, buf, out_n);
            (void)written;
            free(buf);
        }
    }
}

/* Mutation battery: the read-only exerciser above leaves every write
 * path untested on hostile input.  Mutate a copy so the read-only pass
 * still sees the original. */
static void
exercise_mutating(const sm_t *orig, const uint8_t *data, size_t size)
{
    sm_t *m = sm_copy(orig);
    if (m == NULL) return;
    uint64_t seed = 0;
    for (size_t i = 0; i < size && i < 16; i++) seed = seed * 131 + data[i];
    for (int k = 0; k < 8; k++) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        uint64_t idx = seed % 200000;
        if (seed & 1) (void)sm_add_grow(&m, idx); else (void)sm_remove(m, idx);
        if (m == NULL) return;
    }
    (void)sm_add_range(m, 10, 90);
    (void)sm_remove_range(m, 30, 40);
    sm_t *a = sm_union(m, orig);
    sm_t *b = sm_intersection(m, orig);
    sm_t *c = sm_difference(m, orig);
    sm_t *d = sm_xor(m, orig);
    sm_t *e = sm_offset(m, 37);
    sm_t *f = sm_offset(m, -37);
    sm_t *g = sm_create(1 << 16);
    if (g != NULL) (void)sm_split(m, 4096, g);
    sm_free(a); sm_free(b); sm_free(c); sm_free(d);
    sm_free(e); sm_free(f); sm_free(g); sm_free(m);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > FUZZ_MAX_INPUT) return 0;

    /* --- Attack vector 1: sm_open()
     *
     * Hand the bytes to an sm_create-allocated map and call sm_open
     * to interpret them as a chunk stream.  This is the path
     * postgres/undo and pg_tre take when they read a serialized
     * sparsemap from disk and don't trust the bytes. */
    {
        sm_t *m = sm_create(size + 64);
        if (m != NULL) {
            uint8_t *buf = sm_get_data(m);
            memcpy(buf, data, size);
            sm_open(m, buf, size + 64);
            exercise_readonly(m);
            sm_free(m);
        }
    }

    /* --- Attack vector 2: sm_open_copy()
     *
     * Convenience wrapper from v2.1; same parsing path but allocates
     * its own buffer.  Fuzz it separately because the slack
     * arithmetic is the kind of thing that integer-overflows. */
    {
        sm_t *m = sm_open_copy(data, size, 64);
        if (m != NULL) {
            exercise_readonly(m);
            exercise_mutating(m, data, size);
            sm_free(m);
        }
    }

    /* --- Attack vector 3: sm_deserialize()
     *
     * The validating deserializer.  Header parsing should reject
     * everything but properly-formed bytes; we want to ensure no
     * malformed header smuggles through to the chunk-stream walker. */
    {
        sm_t *m = sm_deserialize(data, size);
        if (m != NULL) {
            exercise_readonly(m);
            exercise_mutating(m, data, size);
            sm_free(m);
        }
    }

    return 0;
}
