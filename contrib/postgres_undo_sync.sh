#!/bin/sh
# SPDX-License-Identifier: MIT
#
# contrib/postgres_undo_sync.sh — sync the canonical sparsemap into
# the postgres/undo PostgreSQL fork's vendored layout.
#
# postgres/undo vendors:
#
#   src/backend/lib/sparsemap.c             <- src/sparsemap.c
#   src/include/lib/sparsemap.h             <- include/sparsemap.h
#   src/include/lib/sparsemap_internal.h    <- include/sparsemap_internal.h
#                                              (planned for v1.1)
#
# Postgres has its own typedefs (uint64, not uint64_t) and macros
# (pg_attribute_always_inline instead of __attribute__((always_inline))),
# so this script does the necessary sed translations.
#
# Usage:  contrib/postgres_undo_sync.sh PATH/TO/postgres
#
# The script is idempotent.
set -eu

if [ "$#" -ne 1 ]; then
    printf 'usage: %s PATH/TO/postgres\n' "$0" >&2
    exit 64
fi

DEST="$1"
SRC="$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -d "$DEST/src/backend/lib" ] || [ ! -d "$DEST/src/include/lib" ]; then
    printf 'postgres_undo_sync: %s does not look like a postgres tree\n' "$DEST" >&2
    printf '                    (expected src/backend/lib/ and src/include/lib/)\n' >&2
    exit 1
fi

DEST_C="$DEST/src/backend/lib/sparsemap.c"
DEST_H="$DEST/src/include/lib/sparsemap.h"

printf 'Syncing %s -> postgres tree at %s\n' "$SRC" "$DEST"

cp "$SRC/src/sparsemap.c"     "$DEST_C"
cp "$SRC/include/sparsemap.h" "$DEST_H"

# postgres/undo uses postgres.h instead of <stdint.h> for the integer
# typedefs.
for f in "$DEST_C" "$DEST_H"; do
    # Switch <stdint.h> + <inttypes.h> for "postgres.h".
    sed -i 's|#include <stdint.h>|#include "postgres.h"|' "$f"
    sed -i '/#include <inttypes.h>/d' "$f"

    # Postgres style: uint64 (not uint64_t), uint32, uint8.
    sed -i 's/\buint64_t\b/uint64/g' "$f"
    sed -i 's/\buint32_t\b/uint32/g' "$f"
    sed -i 's/\buint8_t\b/uint8/g' "$f"

    # Postgres always-inline macro.
    sed -i 's|__attribute__((always_inline))|pg_attribute_always_inline|g' "$f"

    # Switch include path for sparsemap.h within sparsemap.c.
    sed -i 's|#include "sparsemap.h"|#include "lib/sparsemap.h"|' "$f"
    sed -i 's|#include "popcount.h"|#include "port/pg_bitutils.h"|' "$f"
done

# Postgres tree carries its own popcount in port/pg_bitutils.h, so we
# don't ship include/popcount.h.

printf '\nSync complete.  Diff against postgres HEAD:\n'
git -C "$DEST" diff --stat -- "${DEST_C#$DEST/}" "${DEST_H#$DEST/}"

printf '\nReminder: postgres/undo also vendors src/include/lib/sparsemap_internal.h.\n'
printf 'That will be synced via the chunk-codec promotion in v1.1.\n'
