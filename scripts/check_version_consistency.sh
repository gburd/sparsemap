#!/bin/sh
# SPDX-License-Identifier: MIT
#
# scripts/check_version_consistency.sh — fail if the sparsemap
# version string disagrees across its three sources of truth:
#
#   meson.build               (project(version: '...'))
#   include/sparsemap.h       (SPARSEMAP_VERSION_STRING define)
#   CHANGELOG.md              (most recent ## section header)
#
# CI runs this on every push so a version bump that forgets one
# source surfaces immediately.
set -eu

cd "$(dirname "$0")/.."

# 1. meson.build
MESON=$(grep -E "^[[:space:]]*version[[:space:]]*:" meson.build \
        | head -1 \
        | sed -E "s/.*version[[:space:]]*:[[:space:]]*'([^']+)'.*/\1/")

# 2. include/sparsemap.h — look for SPARSEMAP_VERSION_STRING.
HEADER=$(grep -E '^#define[[:space:]]+SPARSEMAP_VERSION_STRING' \
         include/sparsemap.h 2>/dev/null \
         | head -1 \
         | sed -E 's/.*"([^"]+)".*/\1/')

# 3. CHANGELOG.md — first non-Unreleased version header.
if [ -f CHANGELOG.md ]; then
    CHANGELOG=$(grep -E '^## (\[)?[0-9]+\.[0-9]+\.[0-9]+' CHANGELOG.md \
                | head -1 \
                | sed -E 's/^## \[?([0-9]+\.[0-9]+\.[0-9]+).*/\1/')
else
    CHANGELOG=''
fi

printf 'meson.build:               %s\n' "${MESON:-MISSING}"
printf 'include/sparsemap.h:       %s\n' "${HEADER:-MISSING}"
printf 'CHANGELOG.md:              %s\n' "${CHANGELOG:-MISSING}"

fail=0

if [ -z "$MESON" ]; then
    printf 'check_version: meson.build version not found\n' >&2
    fail=1
fi

if [ -n "$HEADER" ] && [ "$HEADER" != "$MESON" ]; then
    printf 'check_version: include/sparsemap.h SPARSEMAP_VERSION_STRING %s != meson %s\n' \
           "$HEADER" "$MESON" >&2
    fail=1
fi

if [ -n "$CHANGELOG" ] && [ "$CHANGELOG" != "$MESON" ]; then
    printf 'check_version: CHANGELOG.md %s != meson %s\n' \
           "$CHANGELOG" "$MESON" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

printf '\nversion strings agree: %s\n' "$MESON"
