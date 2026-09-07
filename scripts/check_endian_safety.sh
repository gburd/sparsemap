#!/bin/sh
# check_endian_safety.sh -- reject byte-pointer walks over a chunk descriptor.
#
# The sparse chunk descriptor packs thirty-two 2-bit flags into one
# 64-bit word, flag i at bits [2i, 2i+1].  Flag byte n is therefore
# bits [8n, 8n+7] of the *value*.  Reading those bytes by aliasing the
# word with a `uint8_t *` and incrementing gives the right answer only
# on a little-endian host; on big-endian it walks the flags in reverse,
# which silently corrupted sm_cardinality / sm_minimum / sm_maximum /
# sm_rank / sm_select / sm_scan (fixed 2026-07; 6 of 8 test suites
# failed on sparcv9 before the fix).
#
# No runtime test on a little-endian host can catch a regression here,
# because a byte walk and a shift are identical on little-endian.  The
# only cheap guard is structural: forbid the pattern outright and make
# every descriptor read go through __sm_desc_flag_byte(), which shifts.
#
# Big-endian CI (the s390x cross job) is the behavioural backstop.

set -e

cd "$(dirname "$0")/.."

status=0

# Byte-pointer aliases of a chunk descriptor word.  m_data is a
# __sm_bitvec_unaligned_t *; casting it to uint8_t * to step through
# flag bytes is the banned pattern.  Assigning such a cast to a
# variable is what enables the walk, so that is what we reject --
# passing the cast straight to __sm_store_u64 / __sm_load_u64 is fine,
# since those serialize with explicit shifts.
if grep -nE '^[[:space:]]*(register[[:space:]]+)?(const[[:space:]]+)?uint8_t[[:space:]]*\*[[:space:]]*[a-z_]+[[:space:]]*=[[:space:]]*\(uint8_t[[:space:]]*\*\)[[:space:]]*&?[a-z_]+(->|\.)m_data' sm.c; then
	echo "ERROR: byte-pointer alias of a chunk descriptor (endian-unsafe)." >&2
	echo "       Use __sm_desc_flag_byte(desc, n) instead; it shifts the" >&2
	echo "       value and so reads the same flags on every byte order." >&2
	status=1
fi

# SM_CHUNK_GET_FLAGS / __sm_chunk_calc_vector_size applied to a
# dereferenced byte pointer (the other half of the same mistake).
if grep -nE '(SM_CHUNK_GET_FLAGS|__sm_chunk_calc_vector_size)\([[:space:]]*\*[a-z_]+[[:space:]]*[,)]' sm.c |
    grep -vE '\*[a-z_]+(->|\.)?m_data'; then
	echo "ERROR: flag extraction from a dereferenced byte pointer." >&2
	echo "       Use __sm_desc_flag_byte(desc, n)." >&2
	status=1
fi

if [ "$status" -eq 0 ]; then
	echo "check_endian_safety: ok (no descriptor byte walks in sm.c)"
fi

exit "$status"
