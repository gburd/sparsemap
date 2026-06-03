/* SPDX-License-Identifier: MIT */
/*
 * c_read.c - read a serialized sparsemap from stdin with the C library
 * and print every set bit, one per line, ascending.  Used by
 * ci/wire_compat.sh to verify the C library reads Rust-produced bytes.
 */
#include "sm.h"
#include <stdio.h>
#include <stdlib.h>

int
main(void)
{
	uint8_t *buf = NULL;
	size_t cap = 0, n = 0, r;
	uint8_t tmp[4096];
	while ((r = fread(tmp, 1, sizeof(tmp), stdin)) > 0) {
		if (n + r > cap) {
			cap = (n + r) * 2;
			buf = realloc(buf, cap);
		}
		for (size_t i = 0; i < r; i++)
			buf[n++] = tmp[i];
	}
	sm_t *m = sm_deserialize(buf, n);
	if (m == NULL) {
		/* Empty maps may decode to NULL; emit nothing. */
		free(buf);
		return (0);
	}
	size_t card = sm_cardinality(m);
	for (size_t i = 0; i < card; i++)
		printf("%llu\n", (unsigned long long)sm_select(m, i, true));
	sm_free(m);
	free(buf);
	return (0);
}
