#!/usr/bin/env bash
# build.sh - HomRec build wrapper for the MSYS2 MinGW64 terminal.
#
# What you've been doing manually (open "MSYS2 MinGW64", cd into the repo,
# run `make`) still works exactly the same - this script just wraps that
# with checks for the things that make `make` fail with a wall of errors
# if they're missing (wxWidgets, Lua), and tells you in plain language
# what's missing and how to get it, instead of a raw compiler error.
#
# Usage (from the MSYS2 MinGW64 terminal, NOT plain MSYS2/MSYS):
#   cd /path/to/HomRec-main
#   ./build.sh            # normal build
#   ./build.sh clean      # remove build output
#   ./build.sh -j8         # extra args are passed straight through to make
#
# IMPORTANT: this must be run from "MSYS2 MinGW64" (not the plain "MSYS2"
# shortcut, and not "MSYS2 MinGW32"/"MSYS2 UCRT64" unless you know that's
# what you want) - that's the one that puts the mingw-w64-x86_64-* compiler
# toolchain on PATH. The window title bar tells you which one you're in.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}==>${NC} $*"; }
warn()  { echo -e "${YELLOW}==>${NC} $*"; }
error() { echo -e "${RED}==> ERROR:${NC} $*" >&2; }

# ---------------------------------------------------------------------------
# 0. Handle `clean` as a plain passthrough - no dependency checks needed.
# ---------------------------------------------------------------------------
if [[ "${1:-}" == "clean" ]]; then
    info "Cleaning build output..."
    make clean
    exit 0
fi

# ---------------------------------------------------------------------------
# 1. Compiler toolchain check
# ---------------------------------------------------------------------------
if ! command -v g++ >/dev/null 2>&1; then
    error "g++ not found on PATH."
    echo "  You're probably not in the right terminal. This needs to be run from"
    echo "  the 'MSYS2 MinGW64' shortcut specifically (not 'MSYS2', not 'MSYS2 MinGW32'"
    echo "  / 'MSYS2 UCRT64' unless that's deliberate) - check the window title bar."
    echo "  If you ARE in the right terminal, install the toolchain with:"
    echo "    pacman -S mingw-w64-x86_64-toolchain"
    exit 1
fi
if ! command -v windres >/dev/null 2>&1; then
    error "windres not found on PATH (should ship with the mingw-w64 toolchain)."
    echo "    pacman -S mingw-w64-x86_64-toolchain"
    exit 1
fi
info "Compiler toolchain OK: $(g++ --version | head -1)"

# ---------------------------------------------------------------------------
# 2. wxWidgets check (wx-config must be able to report flags)
# ---------------------------------------------------------------------------
if ! command -v wx-config >/dev/null 2>&1; then
    error "wx-config not found - wxWidgets isn't installed."
    echo "  Install it with:"
    echo "    pacman -S mingw-w64-x86_64-wxwidgets3.2-msw"
    exit 1
fi
info "wxWidgets OK: $(wx-config --version)"

# ---------------------------------------------------------------------------
# 3. Lua check - NOT vendored in this repo, and not an MSYS2 package
#    requirement either, since HomRec links Lua 5.4 statically for plugin
#    scripting (src/plugins/lua_engine.cpp, src/plugins/lua_api.cpp).
#    Recognized locations, in order:
#      a) LUA_CFLAGS/LUA_LDFLAGS already set in the environment -> use as-is
#      b) pkg-config (lua5.4 or lua54), if some package provided one
#      c) ./include + ./lib at the project root (this is the "include folder
#         I made myself" setup - put lua.h/lauxlib.h/lualib.h in ./include
#         and liblua54.a (or liblua.a) in ./lib)
# ---------------------------------------------------------------------------
LUA_CFLAGS="${LUA_CFLAGS:-}"
LUA_LDFLAGS="${LUA_LDFLAGS:-}"

if [[ -n "$LUA_CFLAGS" || -n "$LUA_LDFLAGS" ]]; then
    info "Using LUA_CFLAGS/LUA_LDFLAGS already set in the environment."
elif pkg-config --exists lua5.4 2>/dev/null; then
    LUA_CFLAGS="$(pkg-config --cflags lua5.4)"
    LUA_LDFLAGS="$(pkg-config --libs lua5.4)"
    info "Found Lua via pkg-config (lua5.4)."
elif pkg-config --exists lua54 2>/dev/null; then
    LUA_CFLAGS="$(pkg-config --cflags lua54)"
    LUA_LDFLAGS="$(pkg-config --libs lua54)"
    info "Found Lua via pkg-config (lua54)."
elif [[ -f include/lua.h && -d lib ]]; then
    LUA_CFLAGS="-Iinclude"
    LUA_LDFLAGS="-Llib -llua"
    if [[ -f lib/liblua54.a ]]; then LUA_LDFLAGS="-Llib -llua54"; fi
    info "Found Lua headers in ./include and lib in ./lib."
else
    error "Lua 5.4 headers/library not found anywhere I know to look."
    echo "  HomRec needs lua.h/lauxlib.h/lualib.h + a Lua 5.4 static library to build"
    echo "  (used for plugin scripting: src/plugins/lua_engine.cpp)."
    echo
    echo "  Easiest fix: download the Lua 5.4 amalgamation/prebuilt from lua.org,"
    echo "  then put:"
    echo "    - lua.h, lauxlib.h, lualib.h, luaconf.h  ->  ./include/"
    echo "    - liblua.a (or liblua54.a)                ->  ./lib/"
    echo "  and re-run this script."
    echo
    echo "  Or install via pacman if a package is available:"
    echo "    pacman -Ss lua | grep mingw-w64-x86_64"
    echo
    echo "  Or point at wherever you already have it installed:"
    echo "    LUA_CFLAGS=\"-Ic:/path/to/lua/include\" LUA_LDFLAGS=\"-Lc:/path/to/lua/lib -llua54\" ./build.sh"
    exit 1
fi

export LUA_CFLAGS LUA_LDFLAGS

# ---------------------------------------------------------------------------
# 4. Build
# ---------------------------------------------------------------------------
JOBS="$(nproc 2>/dev/null || echo 4)"
info "Building with 'make -j$JOBS' (LUA_CFLAGS=\"$LUA_CFLAGS\" LUA_LDFLAGS=\"$LUA_LDFLAGS\")..."
if make -j"$JOBS" LUA_CFLAGS="$LUA_CFLAGS" LUA_LDFLAGS="$LUA_LDFLAGS" "$@"; then
    echo
    info "Build succeeded: $(pwd)/hr.exe"
else
    echo
    error "Build failed - see the compiler errors above."
    exit 1
fi
