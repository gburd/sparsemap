#!/bin/sh
# SPDX-License-Identifier: MIT
#
# scripts/check_memory.sh — run the sparsemap test suite under
# valgrind and fail if any leaks or invalid accesses are reported.
#
# Usage: scripts/check_memory.sh [BUILDDIR]
#
# BUILDDIR defaults to "builddir".  Assumes meson has already
# configured and built the tree.
set -eu

BUILDDIR="${1:-builddir}"

if [ ! -d "$BUILDDIR" ]; then
    printf "check_memory: %s does not exist; run \`meson setup %s\` first\n" \
           "$BUILDDIR" "$BUILDDIR" >&2
    exit 1
fi

# Build everything first so we don't valgrind a partially-built tree.
ninja -C "$BUILDDIR"

# meson test --wrap=valgrind runs every test under valgrind with an
# error exit on any leak / invalid access.  Long timeouts because
# valgrind is ~30x slower than native.
exec meson test -C "$BUILDDIR" \
    --print-errorlogs \
    --wrap='valgrind --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=all --track-origins=yes' \
    --timeout-multiplier=30
