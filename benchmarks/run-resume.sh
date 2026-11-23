#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Release
cmake --build build-wsl -j 4
export TMPDIR=/var/tmp
df -T "$TMPDIR"
ctest --test-dir build-wsl --output-on-failure
mkdir -p benchmarks/results
stamp=$(date -u +%Y%m%dT%H%M%SZ)
for run in 1 2 3; do
    stdbuf -oL ./build-wsl/kv_bench 100000 4 33554432 10 |
        tee "benchmarks/results/${stamp}-run-${run}.txt"
done
