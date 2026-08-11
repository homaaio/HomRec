#!/usr/bin/env bash
# scripts/release-windows.sh
#
# Builds HomRec and assembles a distributable release FOLDER (not a zip -
# see the note at the bottom for why). Point your archive tool of choice
# at the resulting dist/HomRec-<version>-win64/ folder and publish that.
#
# Must be run the same way build.sh is: from the "MSYS2 MinGW64" terminal
# (not plain MSYS2, not MinGW32/UCRT64 unless that's deliberate) - check
# the window title bar if you're not sure which one you're in.
#
# Usage:
#   cd /path/to/HomRec-main
#   ./scripts/release-windows.sh
#
# What it does:
#   1. Runs ./build.sh (same toolchain/wxWidgets/Lua checks you already
#      get from a normal build - this script doesn't duplicate those,
#      it just calls the thing that already does them).
#   2. Copies hr.exe, hom.exe, and the other files a release actually
#      needs into dist/HomRec-<version>-win64/.
#   3. Prints what's in it and reminds you what it's still missing
#      (ffmpeg.exe - see below for why that's not automated).

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."   # project root, regardless of cwd

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}==>${NC} $*"; }
warn()  { echo -e "${YELLOW}==>${NC} $*"; }
error() { echo -e "${RED}==> ERROR:${NC} $*" >&2; }

# ---------------------------------------------------------------------------
# 1. Build (reuses build.sh's own toolchain/wxWidgets/Lua checks - if any
#    of those are missing, build.sh already explains what to install and
#    exits non-zero, so this script just fails the same way).
# ---------------------------------------------------------------------------
info "Building HomRec (./build.sh)..."
./build.sh

if [[ ! -f hr.exe || ! -f hom.exe ]]; then
    error "Build reported success but hr.exe/hom.exe aren't both present - aborting."
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. Version + output folder
# ---------------------------------------------------------------------------
VERSION="$(grep -oP '(?<=HR_APP_VERSION)\s+"\K[^"]+' src/ui/version.h || echo "unknown")"
DIST_NAME="HomRec-${VERSION}-win64"
DIST_DIR="dist/${DIST_NAME}"

info "Version: ${VERSION}"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

# ---------------------------------------------------------------------------
# 3. Copy release contents.
#
# NOTE on what's deliberately NOT here:
#   - icons/*.ico  - not needed. Every icon HomRec actually shows at
#     runtime (window icon, tray icon) is compiled INTO hr.exe as a Win32
#     resource (see resource.rc / LoadIconW(..., MAKEINTRESOURCEW(1))),
#     not loaded from a folder on disk. Shipping icons/ separately would
#     just be dead weight.
#   - docs/, commands.md - internal/dev docs, not something an end user
#     running the release needs. Add them here if that's wrong.
# ---------------------------------------------------------------------------
info "Assembling ${DIST_DIR}..."

cp hr.exe hom.exe "$DIST_DIR/"
cp LICENSE README.md SUPPORT.md "$DIST_DIR/" 2>/dev/null || true

# Bundled default plugins (bter, input_overlay_presets) - LuaPluginEngine
# auto-creates an empty plugins/ folder next to the exe if none exists,
# but that would silently lose these two default ones, so they're copied
# in explicitly.
if [[ -d plugins ]]; then
    cp -r plugins "$DIST_DIR/"
fi

# ---------------------------------------------------------------------------
# 4. ffmpeg.exe - deliberately NOT auto-downloaded here.
#
# hr_find_ffmpeg() (src/hr_app_logic.cpp) looks for ffmpeg.exe next to
# hr.exe first, so HomRec DOES expect to find it there in a proper
# release - but this script won't reach out to the internet and fetch a
# third-party binary on its own. Two reasons:
#   1. No network access assumption - this should work in an offline/
#      airgapped build environment too.
#   2. Redistribution terms - which FFmpeg build (LGPL vs GPL, which
#      encoders it was compiled with) you're allowed to ship depends on
#      your own build's licensing choices. That's a decision this script
#      shouldn't make silently on your behalf.
#
# If you already have a specific ffmpeg.exe you use for releases, either
# drop it in manually after this script runs, or point FFMPEG_EXE at it:
#   FFMPEG_EXE=/c/tools/ffmpeg/bin/ffmpeg.exe ./scripts/release-windows.sh
# ---------------------------------------------------------------------------
if [[ -n "${FFMPEG_EXE:-}" ]]; then
    if [[ -f "$FFMPEG_EXE" ]]; then
        cp "$FFMPEG_EXE" "$DIST_DIR/ffmpeg.exe"
        info "Bundled ffmpeg.exe from \$FFMPEG_EXE."
    else
        warn "\$FFMPEG_EXE is set but '$FFMPEG_EXE' doesn't exist - skipping."
    fi
else
    warn "ffmpeg.exe NOT bundled (see the comment in this script for why)."
    warn "Either drop your own ffmpeg.exe into '${DIST_DIR}/' before zipping,"
    warn "or re-run with FFMPEG_EXE=/path/to/ffmpeg.exe to have this script do it."
fi

# ---------------------------------------------------------------------------
# 5. Done - list what's there and remind what's left to do by hand.
# ---------------------------------------------------------------------------
echo
info "Release folder ready: ${DIST_DIR}/"
( cd "$DIST_DIR" && find . -maxdepth 2 -type f | sed 's/^/    /' )
echo
info "Next step: zip/7z this folder yourself, e.g."
echo "    cd dist && zip -r ${DIST_NAME}.zip ${DIST_NAME}"
echo "  (left as a manual step on purpose - you mentioned wanting to pick"
echo "   the archive format yourself, and that's one less assumption for"
echo "   this script to get wrong on your behalf)."
