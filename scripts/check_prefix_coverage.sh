#!/bin/sh
# check_prefix_coverage.sh -- every exported sm_* symbol must be renameable.
#
# sm.h carries a block of `#define sm_foo SM__P(sm_foo)` lines that
# implement the SPARSEMAP_PREFIX mechanism (BDB --with-uniquename
# style), so a vendoring consumer can link two sparsemap copies into
# one binary without symbol collisions.  Nothing enforces that the list
# is complete: tests/test_prefix.c only exercises the ~20 functions it
# happens to call by hand, so a new public function whose #define is
# missing still compiles, links, and passes the whole suite -- and then
# collides in a consumer's build.
#
# This compares the shared library's exported sm_* symbols against the
# #define list in sm.h and fails on anything missing.

set -e

cd "$(dirname "$0")/.."

lib="${1:-}"
if [ -z "$lib" ]; then
	for cand in build/libsparsemap.so builddir/libsparsemap.so \
	    build-*/libsparsemap.so; do
		[ -f "$cand" ] && lib="$cand" && break
	done
fi

if [ -z "$lib" ] || [ ! -f "$lib" ]; then
	echo "check_prefix_coverage: no libsparsemap.so found; build first" >&2
	echo "  usage: $0 [path/to/libsparsemap.so]" >&2
	exit 77   # skip, not fail: nothing to check yet
fi

if ! command -v nm >/dev/null 2>&1; then
	echo "check_prefix_coverage: nm not available; skipping" >&2
	exit 77
fi

# Exported, defined, global text/data symbols starting with sm_.
nm --dynamic --defined-only --extern-only "$lib" 2>/dev/null |
    awk '{ print $NF }' |
    sed -n 's/^\(sm_[A-Za-z0-9_]*\)$/\1/p' |
    sort -u > /tmp/.sm_exported.$$

# The prefix list: `#define sm_foo SM__P(sm_foo)`.
sed -n 's/^#define[[:space:]]\+\(sm_[A-Za-z0-9_]*\)[[:space:]]\+SM__P(.*/\1/p' \
    sm.h | sort -u > /tmp/.sm_defined.$$

missing="$(comm -23 /tmp/.sm_exported.$$ /tmp/.sm_defined.$$)"
rm -f /tmp/.sm_exported.$$ /tmp/.sm_defined.$$

if [ -n "$missing" ]; then
	echo "ERROR: exported symbols with no SPARSEMAP_PREFIX #define in sm.h:" >&2
	echo "$missing" | sed 's/^/  /' >&2
	echo "" >&2
	echo "Add for each:  #define NAME  SM__P(NAME)" >&2
	echo "Without it, a vendoring consumer that sets SPARSEMAP_PREFIX" >&2
	echo "still exports this symbol unrenamed and can collide." >&2
	exit 1
fi

echo "check_prefix_coverage: ok (all exported sm_* symbols are renameable)"
