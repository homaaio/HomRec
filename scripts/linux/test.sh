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
for t in wav_util audio_mixer overlay_canvas replay_ring pic_pool encoder_helpers settings_registry idle_detect process_linux x11_record x11_services; do
    extra=()
    libs=()
    [ "$t" = "encoder_helpers" ] && extra=("$OUT/encoder_helpers.o")
    [ "$t" = "settings_registry" ] && extra=(src/core/settings_registry.cpp)
    [ "$t" = "x11_services" ] && extra=(src/platform/linux/x11/x11_windows.cpp src/platform/linux/x11/x11_hotkeys.cpp src/platform/linux/process.cpp) && libs=(-lX11 -ldl -pthread)
    [ "$t" = "process_linux" ] && extra=(src/platform/linux/process.cpp)
    [ "$t" = "x11_record" ] && extra=(src/platform/linux/x11/x11_record.cpp src/platform/linux/paths.cpp src/platform/linux/process.cpp)
    echo "=== test_$t"
    "$CXX" -std=c++17 -O1 -Wall -Wextra "${SAN[@]}" \
        -Isrc -Isrc/core -Isrc/utils -Iscripts/tests \
        "scripts/tests/test_$t.cpp" "${extra[@]}" "${libs[@]}" -o "$OUT/test_$t"
    if ! "$OUT/test_$t"; then fail=1; echo "!!! test_$t FAILED"; fi
done

if [ "$fail" -ne 0 ]; then echo "Some tests failed."; exit 1; fi
echo "All tests passed."
