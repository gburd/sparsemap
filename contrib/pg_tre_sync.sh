#!/bin/sh
# SPDX-License-Identifier: MIT
#
# contrib/pg_tre_sync.sh — sync the canonical sparsemap into pg_tre's
# vendored layout.
#
# pg_tre vendors two files:
#
#   src/util/sparsemap.c           <- sm.c
#   include/pg_tre/sparsemap.h     <- sm.h
#
# The vendored filenames stay sparsemap.{c,h} so pg_tre's own #include
# lines don't change; only the upstream source filenames moved to
# sm.{c,h}.
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
cp "$SRC/sm.c"  "$DEST/src/util/sparsemap.c"
cp "$SRC/sm.h"  "$DEST/include/pg_tre/sparsemap.h"

# Rewrite the include of sm.h inside the copied implementation to use
# the pg_tre subdirectory layout and the vendored filename.
sed -i 's|#include "sm.h"|#include "pg_tre/sparsemap.h"|' \
    "$DEST/src/util/sparsemap.c"

printf '\nSync complete.  Diff against pg_tre HEAD:\n'
git -C "$DEST" diff --stat -- src/util/sparsemap.c include/pg_tre/

printf '\nNote: sparsemap is now a two-file library (sm.h + sm.c); the\n'
printf 'portability shims that used to live in popcount.h / sm_portability.h\n'
printf 'are folded into the implementation, so no extra header is needed.\n'
