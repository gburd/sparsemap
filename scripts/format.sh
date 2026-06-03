#!/bin/sh
# SPDX-License-Identifier: MIT
#
# scripts/format.sh - reformat the library to BSD KNF (style(9)).
#
# clang-format gets the tabs, braces, and wrapping right but, with
# ReflowComments disabled (so it never mangles the ASCII tables and
# bit-diagrams in the doc comments), it leaves block-comment
# continuation lines at their old column.  knf_comment_realign.py
# fixes that.  Run both, in order.
#
# Usage:  scripts/format.sh [file ...]   (defaults to sm.c sm.h)
set -eu

cd "$(dirname "$0")/.."

: "${CLANG_FORMAT:=clang-format}"

if [ "$#" -eq 0 ]; then
	set -- sm.c sm.h
fi

"$CLANG_FORMAT" -i "$@"
python3 scripts/knf_comment_realign.py "$@"
