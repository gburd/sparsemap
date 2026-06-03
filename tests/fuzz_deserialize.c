/* SPDX-License-Identifier: MIT
 *
 * tests/fuzz_deserialize.c — libFuzzer harness for the deserialize
 * path.  Two distinct entry points get attacked:
 *
 *   sm_open(map, data, size):
 *     Untrusted bytes interpreted directly as the on-disk chunk
 *     stream.  Vulnerable to any chunk-codec walker that doesn't
 *     bounds-check.
 *
 *   sm_deserialize(buf, n):
 *     Length-prefixed header (magic + version + endianness) then
 *     a chunk stream.  The header is validated; the chunk stream
 *     is not.
 *
 * For each input, we run both deserializers and then exercise a
 * battery of follow-up operations on whatever survives.  Goal:
 * trip ASan / UBSan / heap corruption / infinite-loop hangs.
 *
 * Build:
 *   meson setup builddir-fuzz -Dfuzz=enabled
 *   ninja -C builddir-fuzz
 *
 * Run:
 *   ./builddir-fuzz/tests/fuzz_deserialize tests/fuzz-corpus/ \
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
exercise_readonly(sparsemap_t *m)
{
    /* Cardinality + a popcount walk through every chunk. */
    (void)sm_cardinality(m);
    (void)sm_get_size(m);
    (void)sm_is_empty(m);

    /* Membership at boundaries. */
    (void)sm_contains(m, 0);
    (void)sm_contains(m, 1);
    (void)sm_contains(m, 63);
    (void)sm_contains(m, 64);
    (void)sm_contains(m, 65535);
    (void)sm_contains(m, SM_IDX_MAX - 1);

    /* Iteration: visit up to 256 set bits, then stop.  prev_idx =
     * SM_IDX_MAX is the documented sentinel for "start at first set". */
    uint64_t idx = SM_IDX_MAX;
    for (int i = 0; i < 256; i++) {
        idx = sm_next_member(m, idx);
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
        sparsemap_t *m = sm_create(size + 64);
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
        sparsemap_t *m = sm_open_copy(data, size, 64);
        if (m != NULL) {
            exercise_readonly(m);
            sm_free(m);
        }
    }

    /* --- Attack vector 3: sm_deserialize()
     *
     * The validating deserializer.  Header parsing should reject
     * everything but properly-formed bytes; we want to ensure no
     * malformed header smuggles through to the chunk-stream walker. */
    {
        sparsemap_t *m = sm_deserialize(data, size);
        if (m != NULL) {
            exercise_readonly(m);
            sm_free(m);
        }
    }

    return 0;
}
