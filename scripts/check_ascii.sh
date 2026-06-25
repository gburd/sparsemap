#!/bin/sh
# SPDX-License-Identifier: MIT
#
# scripts/check_ascii.sh -- fail if any tracked C source, header, or
# build file contains a byte outside the 7-bit ASCII range.
#
# sparsemap keeps its sources ASCII-only so that the library compiles
# and reads identically under any locale, editor, or terminal, and so
# that a downstream vendor never has to reason about encoding when it
# copies sm.c / sm.h into its tree.
#
# CI runs this on every push.
set -eu

cd "$(dirname "$0")/.."

# Files to police: C sources/headers, meson build files, shell scripts.
# Use the tracked file list so generated build artifacts are ignored.
files=$(git ls-files \
	'*.c' '*.h' '*.build' 'meson_options.txt' 'scripts/*.sh' \
	2>/dev/null || true)

if [ -z "$files" ]; then
	echo "check_ascii: no files to scan"
	exit 0
fi

fail=0
for f in $files; do
	# grep -P with a non-ASCII class; LC_ALL=C makes the byte range literal.
	if LC_ALL=C grep -nP '[^\x00-\x7F]' "$f" >/dev/null 2>&1; then
		printf 'check_ascii: non-ASCII byte(s) in %s:\n' "$f" >&2
		LC_ALL=C grep -nP '[^\x00-\x7F]' "$f" | head -5 >&2
		fail=1
	fi
done

if [ "$fail" -ne 0 ]; then
	printf '\ncheck_ascii: FAILED -- replace the bytes above with ASCII\n' >&2
	printf '  --  ->  --   (em dash)\n' >&2
	printf '  ... ->  ...  (ellipsis)\n' >&2
	printf '  u   ->  u    (micro sign)\n' >&2
	exit 1
fi

printf 'check_ascii: all tracked sources are ASCII-only\n'
