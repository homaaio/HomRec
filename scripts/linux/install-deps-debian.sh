#!/usr/bin/env bash
# scripts/linux/install-deps-debian.sh - installs what building HomRec on Debian / Ubuntu needs.
# Other distros: the package names differ, the list of THINGS needed is the same (see
# docs/LINUX_PORT_PLAN.md, "Dependencies"). Packaging per distro lives in scripts/packaging/ later.
#
#   scripts/linux/install-deps-debian.sh            # what the current phase needs
#   scripts/linux/install-deps-debian.sh --future   # + libraries the planned backends will use
set -euo pipefail

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

# Phase 0-1: toolchain + ffmpeg (HomRec drives the ffmpeg binary, it does not link libav*).
PKGS=(build-essential cmake pkg-config ffmpeg)

if [ "${1:-}" = "--future" ]; then
    PKGS+=(
        libx11-dev libxext-dev libxrandr-dev libxfixes-dev libxcomposite-dev libxdamage-dev  # X11 capture (phase 2)
        libpipewire-0.3-dev libdbus-1-dev                                                      # Wayland portal + PipeWire (phases 3-4)
        libpulse-dev                                                                           # PulseAudio fallback (phase 4)
        liblua5.4-dev                                                                          # plugin host (phase 5)
        libwxgtk3.2-dev                                                                        # GUI (phase 7)
    )
fi

$SUDO apt-get update
$SUDO apt-get install -y "${PKGS[@]}"
