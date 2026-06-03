#!/bin/sh
# SPDX-License-Identifier: MIT
#
# scripts/check_version_consistency.sh — fail if the sparsemap
# version string disagrees between its two sources of truth:
#
#   meson.build               (project(version: '...'))
#   sm.h       (SM_VERSION_STRING define)
#
# CI runs this on every push so a version bump that forgets one
# source surfaces immediately.
set -eu

cd "$(dirname "$0")/.."

# 1. meson.build
MESON=$(grep -E "^[[:space:]]*version[[:space:]]*:" meson.build \
        | head -1 \
        | sed -E "s/.*version[[:space:]]*:[[:space:]]*'([^']+)'.*/\1/")

# 2. sm.h — look for SM_VERSION_STRING.
HEADER=$(grep -E '^#define[[:space:]]+SM_VERSION_STRING' \
         sm.h 2>/dev/null \
         | head -1 \
         | sed -E 's/.*"([^"]+)".*/\1/')

printf 'meson.build:               %s\n' "${MESON:-MISSING}"
printf 'sm.h:       %s\n' "${HEADER:-MISSING}"

fail=0

if [ -z "$MESON" ]; then
    printf 'check_version: meson.build version not found\n' >&2
    fail=1
fi

if [ -n "$HEADER" ] && [ "$HEADER" != "$MESON" ]; then
    printf 'check_version: sm.h SM_VERSION_STRING %s != meson %s\n' \
           "$HEADER" "$MESON" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

printf '\nversion strings agree: %s\n' "$MESON"
