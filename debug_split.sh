#!/bin/bash
lldb tests/test <<EOF
breakpoint set -n sparsemap_split
run /api/split
continue
continue
continue
quit
EOF
