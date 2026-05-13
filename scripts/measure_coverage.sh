#!/bin/sh
# SPDX-License-Identifier: MIT
#
# scripts/measure_coverage.sh — run the test suite under gcov, then
# emit an lcov HTML report under coverage/.
#
# Usage: scripts/measure_coverage.sh [BUILDDIR]
#
# BUILDDIR defaults to "builddir-coverage" so we don't clobber a
# normal builddir.  The directory is reconfigured (or created) with
# coverage enabled, the test suite is run, and lcov produces
# coverage/index.html.
set -eu

BUILDDIR="${1:-builddir-coverage}"

if ! command -v lcov >/dev/null 2>&1; then
    printf 'measure_coverage: lcov not found; install lcov first\n' >&2
    exit 1
fi

if [ ! -d "$BUILDDIR" ]; then
    meson setup "$BUILDDIR" -Db_coverage=true -Ddiagnostic=true
else
    meson setup --reconfigure "$BUILDDIR" -Db_coverage=true -Ddiagnostic=true
fi

ninja -C "$BUILDDIR"

# Run all tests, but don't fail the script on a single test failure —
# we still want the coverage report.  CI's regular build job is the
# pass/fail gate.
meson test -C "$BUILDDIR" --print-errorlogs || true

# Capture coverage data and produce HTML.
mkdir -p coverage
lcov --capture --directory "$BUILDDIR" \
     --output-file coverage/coverage.info \
     --rc lcov_branch_coverage=1 \
     --ignore-errors source,unused 2>/dev/null

# Strip system / test / generated paths from the report.
lcov --remove coverage/coverage.info \
     '/usr/*' '*/munit.c' '*/roaring.c' '*/tdigest.c' '*/qc.c' '*/midl.c' \
     '*/tests/*' \
     --output-file coverage/coverage.info \
     --rc lcov_branch_coverage=1 2>/dev/null

genhtml coverage/coverage.info \
        --output-directory coverage \
        --branch-coverage \
        --title "sparsemap coverage" 2>/dev/null

printf '\ncoverage report at coverage/index.html\n'

# Print a summary line for CI logs.
lcov --summary coverage/coverage.info 2>&1 | grep -E 'lines|functions|branches' || true
