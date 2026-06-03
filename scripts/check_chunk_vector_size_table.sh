#!/bin/sh
# SPDX-License-Identifier: MIT
#
# scripts/check_chunk_vector_size_table.sh — fail if the static lookup
# table inlined at sm.c:__sm_chunk_calc_vector_size has
# drifted from what scripts/gen_chunk_vector_size_table.py produces.
#
# We compare the integer sequences only; whitespace and indent differ.
set -eu

cd "$(dirname "$0")/.."

regen=$(python3 scripts/gen_chunk_vector_size_table.py \
        | tr -dc '0-9, ' \
        | tr -s ' ,' ',' \
        | sed 's/^,*//; s/,*$//')

inlined=$(awk '/static int lookup\[\] = \{/,/};/' sm.c \
          | tr -dc '0-9, ' \
          | tr -s ' ,' ',' \
          | sed 's/^,*//; s/,*$//')

if [ "$regen" = "$inlined" ]; then
    printf 'chunk_vector_size_table: in sync\n'
    exit 0
fi

printf 'chunk_vector_size_table: DRIFT\n' >&2
printf '  scripts output:   %s\n' "$regen"   >&2
printf '  inlined in .c:    %s\n' "$inlined" >&2
exit 1
