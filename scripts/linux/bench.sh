#!/usr/bin/env bash
# scripts/linux/bench.sh - builds and runs scripts/tests/bench.cpp (CPU hot-spot timings, "[bench] name ms").
#   scripts/linux/bench.sh > before.txt ; (change code) ; scripts/linux/bench.sh > after.txt
#   python tools/bench_compare.py before.txt after.txt
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CXX="${CXX:-g++}"; CC="${CC:-gcc}"
OUT="${OUT:-$ROOT/build-linux/bench}"
mkdir -p "$OUT"; cd "$ROOT"
"$CC"  -O3 -c src/core/encoder_helpers.c -o "$OUT/eh.o"
"$CXX" -std=c++17 -O3 -Isrc scripts/tests/bench.cpp "$OUT/eh.o" -o "$OUT/bench" >&2
"$OUT/bench"
