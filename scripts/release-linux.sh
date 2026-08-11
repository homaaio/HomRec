#!/usr/bin/env bash
# scripts/release-linux.sh
#
# Placeholder. There is currently NO Linux build of HomRec to release -
# this whole codebase is Windows-only right now (DXGI/D3D11 capture, raw
# Win32 UI, wxWidgets-msw; see the Makefile, which only knows how to
# produce hr.exe via MinGW). This script deliberately does NOT pretend
# otherwise: it explains that and exits, rather than silently "succeeding"
# at packaging nothing, which is worse than an honest failure if this
# ever gets wired into a CI release pipeline.
#
# What has to exist before this script can do anything real:
#   1. Actual Linux capture code, under src/platform/linux/x11/ and/or
#      src/platform/linux/wayland/ (see the README.md in each - capture
#      is split by display server, not by distro).
#   2. A Linux build target - either a new Makefile.linux, or teaching
#      the existing Makefile to branch on `uname`, pulling in GTK/Qt (or
#      whatever replaces wxWidgets-msw on Linux) and the X11/Wayland
#      libs instead of d3d11/dxgi/comctl32/etc.
#   3. THEN packaging: this script would build that, assemble a release
#      folder/tarball the same way scripts/release-windows.sh does for
#      Windows, and - if you want an actual installable .deb, not just a
#      tarball - hand off to `dpkg-deb`/`debuild` using the metadata in
#      scripts/packaging/deb/ (also currently just a placeholder).
#
# Until then, running this just tells you that, instead of doing nothing
# and calling it a success.

set -euo pipefail

echo "HomRec: no Linux build exists yet (Windows-only codebase right now)." >&2
echo "See the comment at the top of this file for what needs to happen first." >&2
exit 1
