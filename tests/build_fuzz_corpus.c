/* SPDX-License-Identifier: MIT
 *
 * tests/build_fuzz_corpus.c -- emit a small corpus of valid
 * sparsemap byte streams for libFuzzer to start from.  Run once,
 * commit the output to tests/fuzz-corpus/.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sm.h>

static void
emit(const char *path, const uint8_t *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fwrite(data, 1, n, f);
    fclose(f);
}

static void
emit_map(const char *path, const sm_t *m)
{
    size_t n = sm_get_size(m);
    emit(path, sm_get_data(m), n);
}

int
main(int argc, char *argv[])
{
    const char *out = argc > 1 ? argv[1] : "tests/fuzz-corpus";
    char buf[1024];

    /* Empty map. */
    {
        sm_t *m = sm_create(1024);
        snprintf(buf, sizeof(buf), "%s/empty", out);
        emit_map(buf, m);
        sm_free(m);
    }

    /* Single bit. */
    {
        sm_t *m = sm_create(1024);
        sm_add(m, 42);
        snprintf(buf, sizeof(buf), "%s/single-bit", out);
        emit_map(buf, m);
        sm_free(m);
    }

    /* Small dense (all bits in first chunk). */
    {
        sm_t *m = sm_create(1024);
        for (uint64_t i = 0; i < 64; i++) sm_add(m, i);
        snprintf(buf, sizeof(buf), "%s/dense-64", out);
        emit_map(buf, m);
        sm_free(m);
    }

    /* Sparse, scattered. */
    {
        sm_t *m = sm_create(4096);
        for (uint64_t i = 0; i < 50; i++) sm_add(m, i * 1000);
        snprintf(buf, sizeof(buf), "%s/sparse-scattered", out);
        emit_map(buf, m);
        sm_free(m);
    }

    /* Run-length encoded zeros. */
    {
        sm_t *m = sm_create(4096);
        sm_add(m, 0);
        sm_add(m, 1000000);
        snprintf(buf, sizeof(buf), "%s/rle-zeros", out);
        emit_map(buf, m);
        sm_free(m);
    }

    /* Run-length encoded ones. */
    {
        sm_t *m = sm_create(8192);
        for (uint64_t i = 0; i < 10000; i++) sm_add(m, i);
        snprintf(buf, sizeof(buf), "%s/rle-ones", out);
        emit_map(buf, m);
        sm_free(m);
    }

    /* sm_serialize output (with header magic). */
    {
        sm_t *m = sm_create(2048);
        for (int i = 0; i < 100; i++) sm_add(m, i * 17 + 3);
        size_t n = sm_serialized_size(m);
        uint8_t *out_buf = malloc(n);
        sm_serialize(m, out_buf, n);
        snprintf(buf, sizeof(buf), "%s/serialized-mixed", out);
        emit(buf, out_buf, n);
        free(out_buf);
        sm_free(m);
    }

    /* Mixed payloads. */
    {
        sm_t *m = sm_create(8192);
        for (uint64_t i = 0; i < 64; i++)   sm_add(m, i);              /* dense */
        for (uint64_t i = 1000; i < 1010; i++) sm_add(m, i);          /* mixed */
        for (uint64_t i = 10000; i < 11000; i++) sm_add(m, i);        /* dense run */
        sm_add(m, 100000);                                             /* sparse */
        snprintf(buf, sizeof(buf), "%s/mixed-payloads", out);
        emit_map(buf, m);
        sm_free(m);
    }

    return 0;
}
