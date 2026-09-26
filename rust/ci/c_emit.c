/* SPDX-License-Identifier: MIT */
/* c_emit.c -- emit a serialized sparsemap to stdout using the C library.
 * Used to verify Rust reads C-produced small-mode bytes (C->Rust wire).
 * Prints the bits it put in (one per line, ascending) to stderr, and the
 * raw serialized bytes to stdout.  A companion Rust reader decodes stdin
 * and prints the bits it sees; the two bit lists must match. */
#include "sm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put(sm_t **m, uint64_t b, FILE *ref) {
    sm_add_grow(m, b);
    fprintf(ref, "%llu\n", (unsigned long long)b);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: c_emit <set>\n"); return 2; }
    const char *set = argv[1];
    sm_t *m = sm_create(64);
    FILE *ref = stderr;
    if (!strcmp(set, "zero")) { put(&m, 0, ref); }
    else if (!strcmp(set, "near")) { put(&m,0,ref); put(&m,1,ref); put(&m,5,ref); put(&m,63,ref); }
    else if (!strcmp(set, "scatter")) { put(&m,0,ref); put(&m,5,ref); put(&m,70,ref); }
    else if (!strcmp(set, "two")) { put(&m,5,ref); put(&m,70,ref); }
    else if (!strcmp(set, "dense64")) { for (uint64_t i=0;i<64;i++) put(&m,i,ref); }
    else if (!strcmp(set, "run1000")) { for (uint64_t i=0;i<=1000;i++) put(&m,i,ref); }
    else if (!strcmp(set, "run1023")) { for (uint64_t i=0;i<1024;i++) put(&m,i,ref); }
    else if (!strcmp(set, "empty")) { /* nothing */ }
    else { fprintf(stderr, "unknown set %s\n", set); return 2; }

    /* Report the encoded mode to stderr for the harness log. */
    uint64_t hdr = 0;
    if (sm_get_size(m) >= 8) memcpy(&hdr, sm_get_data(m), 8);
    fprintf(stderr, "#mode=%s size=%zu\n",
        (hdr & ((uint64_t)1<<63)) ? "small" : "chunk", sm_get_size(m));

    size_t sz = sm_serialized_size(m);
    uint8_t *buf = malloc(sz);
    size_t w = sm_serialize(m, buf, sz);
    fwrite(buf, 1, w, stdout);
    free(buf); sm_free(m);
    return 0;
}
