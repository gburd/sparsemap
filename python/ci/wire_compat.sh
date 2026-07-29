#!/bin/sh
# checks the write direction since the c library must read python buffers
set -eu
cd "$(dirname "$0")/.."

# reusing the rust port reader keeps a single harness source
cc -O2 -I.. ../sm.c ../rust/ci/c_read.c -o /tmp/sm_c_read_py
fail=0
for set in single scattered run5000 run4w clusters offset; do
  uv run python ci/wire_emit.py emit "$set" | /tmp/sm_c_read_py > /tmp/c_view.txt
  uv run python ci/wire_emit.py describe "$set" > /tmp/py_view.txt
  if diff -q /tmp/py_view.txt /tmp/c_view.txt >/dev/null; then
    echo "ok: $set"
  else
    echo "MISMATCH: $set"; diff /tmp/py_view.txt /tmp/c_view.txt | head; fail=1
  fi
done
exit "$fail"
