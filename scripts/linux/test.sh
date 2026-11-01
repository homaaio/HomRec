#!/usr/bin/env bash
# scripts/linux/test.sh - builds and runs the platform-independent unit tests (scripts/tests/)
# with plain g++/gcc - no CMake, no wxWidgets, no Windows API needed.
#
#   scripts/linux/test.sh              # all tests, with AddressSanitizer + UBSan
#   SANITIZE=0 scripts/linux/test.sh   # without sanitizers (faster, or when libasan is missing)
#   CXX=clang++ CC=clang scripts/linux/test.sh
#
# test_replay_ring.cpp skips its ffmpeg-based cases by itself when ffmpeg is not on PATH.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CXX="${CXX:-g++}"
CC="${CC:-gcc}"
OUT="${OUT:-$ROOT/build-linux/tests}"
SAN=()
if [ "${SANITIZE:-1}" != "0" ]; then SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi

mkdir -p "$OUT"
cd "$ROOT"

# encoder_helpers.c is C - build it once, link it into the test that needs it.
"$CC" -O2 -Wall "${SAN[@]}" -c src/core/encoder_helpers.c -o "$OUT/encoder_helpers.o"

fail=0
for t in wav_util audio_mixer overlay_canvas replay_ring encoder_helpers settings_registry; do
    extra=()
    [ "$t" = "encoder_helpers" ] && extra=("$OUT/encoder_helpers.o")
    [ "$t" = "settings_registry" ] && extra=(src/core/settings_registry.cpp)
    echo "=== test_$t"
    "$CXX" -std=c++17 -O1 -Wall -Wextra "${SAN[@]}" \
        -Isrc -Isrc/core -Isrc/utils -Iscripts/tests \
        "scripts/tests/test_$t.cpp" "${extra[@]}" -o "$OUT/test_$t"
    if ! "$OUT/test_$t"; then fail=1; echo "!!! test_$t FAILED"; fi
done

if [ "$fail" -ne 0 ]; then echo "Some tests failed."; exit 1; fi
echo "All tests passed."
