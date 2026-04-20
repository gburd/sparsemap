#!/usr/bin/env bash

target=${1:-Debug}
set targets="Debug Profile Release"
case "$target" in
    $targets*) echo "Building ${target}..." ;;
    *) echo "Unknown target ${target}, exiting." ;;
esac
name=${target,,}
echo $name
rm -rf "./build" && \
    cmake -DCMAKE_BUILD_TYPE=${target} -DCMAKE_MAKE_PROGRAM=ninja -DCMAKE_C_COMPILER=clang -G Ninja -S "${PWD}" -B "${PWD}/build" && \
    (cd "${PWD}/build" && ninja)

