#!/bin/sh
# SPDX-License-Identifier: MIT
#
# contrib/pg_tre_sync.sh — sync the canonical sparsemap into pg_tre's
# vendored layout.
#
# pg_tre vendors three files:
#
#   src/util/sparsemap.c           <- src/sparsemap.c
#   include/pg_tre/sparsemap.h     <- include/sparsemap.h
#   include/pg_tre/chunk_codec.h   <- include/sparsemap_internal.h
#                                     (renamed; chunk-codec inlines
#                                      will be promoted out of
#                                      sparsemap.c in v1.1)
#
# Phase 3b note: the chunk-codec extraction (sparsemap_internal.h) is
# planned for v1.1; until then this script syncs only sparsemap.{c,h}
# and reminds the operator to keep their existing chunk_codec.h.
#
# Usage:  contrib/pg_tre_sync.sh PATH/TO/pg_tre
#
# The script is idempotent: re-running on top of an existing sync is
# safe and produces a clean diff.
set -eu

if [ "$#" -ne 1 ]; then
    printf 'usage: %s PATH/TO/pg_tre\n' "$0" >&2
    exit 64
fi

DEST="$1"
SRC="$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -d "$DEST/src/util" ] || [ ! -d "$DEST/include/pg_tre" ]; then
    printf 'pg_tre_sync: %s does not look like a pg_tre checkout\n' "$DEST" >&2
    printf '             (expected src/util/ and include/pg_tre/)\n' >&2
    exit 1
fi

printf 'Syncing %s -> %s ...\n' "$SRC" "$DEST"

# Plain copy: pg_tre's sparsemap.c has the same struct layout, same
# function signatures, and uses uint64_t / standard __attribute__.
cp "$SRC/src/sparsemap.c"      "$DEST/src/util/sparsemap.c"
cp "$SRC/include/sparsemap.h"  "$DEST/include/pg_tre/sparsemap.h"

# Rewrite the include of sparsemap.h inside sparsemap.c to use the
# pg_tre subdirectory layout.
sed -i 's|#include "sparsemap.h"|#include "pg_tre/sparsemap.h"|' \
    "$DEST/src/util/sparsemap.c"

# Rewrite popcount.h include path similarly.
sed -i 's|#include "popcount.h"|#include "pg_tre/popcount.h"|' \
    "$DEST/src/util/sparsemap.c"

# Copy popcount.h too.
cp "$SRC/include/popcount.h"   "$DEST/include/pg_tre/popcount.h"

printf '\nSync complete.  Diff against pg_tre HEAD:\n'
git -C "$DEST" diff --stat -- src/util/sparsemap.c include/pg_tre/

printf '\nReminder: the chunk-codec extraction (sparsemap_internal.h)\n'
printf 'is planned for v1.1.  Until then keep your existing\n'
printf 'include/pg_tre/chunk_codec.h.  Once v1.1 ships, this script\n'
printf 'will sync the upstream sparsemap_internal.h into chunk_codec.h\n'
printf 'with the necessary header-guard rewrite.\n'
