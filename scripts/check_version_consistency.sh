#!/bin/sh
# SPDX-License-Identifier: MIT
#
# scripts/check_version_consistency.sh -- fail if the sparsemap version
# string disagrees across its sources of truth.  A version bump that
# forgets one source surfaces immediately in CI.
#
# Sources (each checked only when the file is present, so this runs
# unchanged on the C-only `main` and on `ports/rust`, which also carries
# the Rust crate and the Python binding):
#
#   meson.build               project(version: '...')          [C, required]
#   sm.h                      SM_VERSION_STRING define          [C, required]
#   sm.h                      SM_VERSION_MAJOR/MINOR/PATCH       [C, if present]
#   rust/Cargo.toml           [package] version                 [Rust port]
#   python/Cargo.toml         [package] version                 [Python ext crate]
#   python/pyproject.toml     [project] version                 [Python wheel]
#
# The Rust port and Python binding intentionally track the C library
# version (the binding is a thin PyO3 layer over the Rust crate, which
# mirrors the C wire format), so all present sources must be equal.
set -eu

cd "$(dirname "$0")/.."

fail=0
ref=""        # the canonical version (meson.build) everything compares to

# Extract the [package]/[project]-section version from a TOML file: the
# first `version = "x"` that appears after the given section header.
toml_section_version() {
    # $1 = file, $2 = section header (e.g. "[package]")
    awk -v section="$2" '
        $0 == section { in_section = 1; next }
        /^\[/         { in_section = 0 }
        in_section && /^[[:space:]]*version[[:space:]]*=/ {
            gsub(/.*=[[:space:]]*"/, ""); gsub(/".*/, ""); print; exit
        }
    ' "$1"
}

report() { printf '%-26s %s\n' "$1" "${2:-MISSING}"; }

# --- 1. meson.build (canonical, required) ---
MESON=$(grep -E "^[[:space:]]*version[[:space:]]*:" meson.build 2>/dev/null \
        | head -1 \
        | sed -E "s/.*version[[:space:]]*:[[:space:]]*'([^']+)'.*/\1/")
report "meson.build:" "$MESON"
if [ -z "$MESON" ]; then
    printf 'check_version: meson.build version not found\n' >&2
    exit 1
fi
ref="$MESON"

# Compare a labeled value against the canonical ref (empty = skip).
check() {
    # $1 = label, $2 = value
    report "$1" "$2"
    if [ -n "$2" ] && [ "$2" != "$ref" ]; then
        printf 'check_version: %s %s != meson %s\n' "$1" "$2" "$ref" >&2
        fail=1
    fi
}

# --- 2. sm.h SM_VERSION_STRING (required) ---
HEADER=$(grep -E '^#define[[:space:]]+SM_VERSION_STRING' sm.h 2>/dev/null \
         | head -1 | sed -E 's/.*"([^"]+)".*/\1/')
check "sm.h SM_VERSION_STRING:" "$HEADER"
if [ -z "$HEADER" ]; then
    printf 'check_version: sm.h SM_VERSION_STRING not found\n' >&2
    fail=1
fi

# --- 3. sm.h SM_VERSION_MAJOR/MINOR/PATCH must compose to the string ---
if [ -f sm.h ]; then
    MAJ=$(grep -E '^#define[[:space:]]+SM_VERSION_MAJOR' sm.h | head -1 | grep -oE '[0-9]+$' || true)
    MIN=$(grep -E '^#define[[:space:]]+SM_VERSION_MINOR' sm.h | head -1 | grep -oE '[0-9]+$' || true)
    PAT=$(grep -E '^#define[[:space:]]+SM_VERSION_PATCH' sm.h | head -1 | grep -oE '[0-9]+$' || true)
    if [ -n "$MAJ" ] && [ -n "$MIN" ] && [ -n "$PAT" ]; then
        check "sm.h MAJOR.MINOR.PATCH:" "$MAJ.$MIN.$PAT"
    fi
fi

# --- 4. rust/Cargo.toml (Rust port, if present) ---
if [ -f rust/Cargo.toml ]; then
    check "rust/Cargo.toml:" "$(toml_section_version rust/Cargo.toml '[package]')"
fi

# --- 5. python/Cargo.toml (Python ext crate, if present) ---
if [ -f python/Cargo.toml ]; then
    check "python/Cargo.toml:" "$(toml_section_version python/Cargo.toml '[package]')"
fi

# --- 6. python/pyproject.toml (Python wheel, if present) ---
if [ -f python/pyproject.toml ]; then
    check "python/pyproject.toml:" "$(toml_section_version python/pyproject.toml '[project]')"
fi

if [ "$fail" -ne 0 ]; then
    printf '\ncheck_version: FAILED -- sources disagree\n' >&2
    exit 1
fi

printf '\nall version sources agree: %s\n' "$ref"
