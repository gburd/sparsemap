#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Cross-language wire check (write direction): the C library must read
# buffers produced by the Rust crate.  Kept out of the published crate;
# run in CI where a C toolchain and the C source are present.
set -eu
cd "$(dirname "$0")/.."          # rust/
cc -O2 -I.. ../sm.c ci/c_read.c -o /tmp/sm_c_read
fail=0
for set in single scattered run5000 run4w clusters offset; do
	cargo run -q --example wire_emit -- emit "$set" | /tmp/sm_c_read > /tmp/c_view.txt
	cargo run -q --example wire_emit -- describe "$set"            > /tmp/rust_view.txt
	if diff -q /tmp/rust_view.txt /tmp/c_view.txt >/dev/null; then
		echo "ok: $set"
	else
		echo "MISMATCH: $set"; diff /tmp/rust_view.txt /tmp/c_view.txt | head; fail=1
	fi
done
exit "$fail"
