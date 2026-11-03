#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Cross-language wire check (read direction): the Rust crate must read
# buffers produced by the C library.  The counterpart of wire_compat.sh;
# kept out of the published crate, run in CI where a C toolchain and the
# C source are present.
set -u
cd "$(dirname "$0")/.."          # rust/
[ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"
export CARGO_REGISTRY_TOKEN=
cc -O2 -I.. ../sm.c ci/c_emit.c -o /tmp/sm_c_emit
cargo build -q --example wire_emit
fail=0
for set in empty zero two scatter near dense64 run1000 run1023; do
  /tmp/sm_c_emit "$set" 2>/tmp/cref.txt |
    cargo run -q --example wire_emit -- read > /tmp/rustbits.txt || true
  grep -v '^#' /tmp/cref.txt > /tmp/cbits.txt
  mode=$(grep '^#mode' /tmp/cref.txt | head -1)
  if diff -q /tmp/cbits.txt /tmp/rustbits.txt >/dev/null; then
    echo "ok: $set ($mode)"
  else
    echo "MISMATCH: $set ($mode)"; diff /tmp/cbits.txt /tmp/rustbits.txt | head; fail=1
  fi
done
exit "$fail"
