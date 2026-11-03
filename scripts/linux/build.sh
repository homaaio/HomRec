#!/usr/bin/env bash
# scripts/linux/build.sh - configures and builds the Linux target (homrec + unit tests)
# into ./build-linux with CMake. Root CMakeLists.txt hands over to cmake/LinuxPort.cmake on
# anything that is not Windows, so the Windows build files are not involved.
#
#   scripts/linux/build.sh                 # Release
#   BUILD_TYPE=Debug scripts/linux/build.sh
#   scripts/linux/build.sh --test          # build, then run ctest
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-linux}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

if [ "$(uname -s)" != "Linux" ]; then
    echo "scripts/linux/build.sh is for Linux. On Windows use the root CMakeLists.txt (see README.md)." >&2
    exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake not found. On Debian/Ubuntu: scripts/linux/install-deps-debian.sh" >&2
    exit 1
fi

cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" --parallel

if [ "${1:-}" = "--test" ]; then
    ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

echo
echo "Built: $BUILD_DIR/homrec   (try: $BUILD_DIR/homrec doctor)"
