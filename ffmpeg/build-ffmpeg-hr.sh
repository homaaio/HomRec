#!/usr/bin/env bash
# build-ffmpeg-hr.sh - builds ffmpeg-hr, the static FFmpeg that HomRec uses.
#
# Run it in an MSYS2 MINGW64 shell (or let .github/workflows/ffmpeg.yml run it):
#
#   ./ffmpeg/build-ffmpeg-hr.sh                 # -> dist/ffmpeg-hr.exe
#   ./ffmpeg/build-ffmpeg-hr.sh /path/ffmpeg.exe
#
# Only one file comes out: a static ffmpeg.exe with no DLLs beside it.
#
# This is a NORMAL FFmpeg build: every native component (demuxers, muxers,
# decoders, encoders, filters, parsers, bitstream filters, protocols, devices)
# is kept, exactly like a regular FFmpeg release. Earlier revisions used
# --disable-everything plus a hand-written allow-list, and every missing entry
# became a real bug in the field (the -f s16le / pcm_s16le rename, missing
# filters for custom args, missing decoders for imported files, ...).
# Do not go back to an allow-list: an incomplete build fails in ways that are
# hard to see until a user hits the exact path.
#
# What is still left out, because it is a separate program or not needed to run
# ffmpeg at all: ffplay, ffprobe, documentation, debug symbols. --disable-autodetect
# keeps configure from linking random system libraries; the external libraries
# below are the only ones linked, and they are all static.
#
# Licence: --enable-gpl --enable-version3 (libx264/libx265), so the result is GPLv3. If you
# publish the exe, publish the corresponding source too (this script + the FFmpeg tarball URL
# and configure line below is enough to rebuild it).
set -euo pipefail

say() { printf '%s\n' "$*"; }
die() { printf 'build-ffmpeg-hr: %s\n' "$*" >&2; exit 1; }

# MSYS2's plain bash does not have /mingw64/bin in PATH; the MINGW64 shell does.
case ":$PATH:" in
  *:/mingw64/bin:*) ;;
  *) [ -d /mingw64/bin ] && export PATH=/mingw64/bin:$PATH ;;
esac

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
FFMPEG_VERSION=${FFMPEG_VERSION:-8.0.1}
WORK=${HR_FFMPEG_WORK:-"$ROOT/.ffmpeg-build"}
OUT=${1:-"$ROOT/dist/ffmpeg-hr.exe"}
SRC="$WORK/ffmpeg-$FFMPEG_VERSION"

say "ffmpeg-hr build"
say "  source:  ffmpeg $FFMPEG_VERSION"
say "  work:    $WORK"
say "  output:  $OUT"
say

# ---------------------------------------------------------------- prerequisites
for t in gcc g++ make nasm pkg-config curl tar; do
  command -v "$t" >/dev/null 2>&1 || die "missing '$t' - install the MSYS2 MINGW64 toolchain (pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-nasm)"
done
for m in x264 x265 dav1d opus vorbis vpl; do
  pkg-config --exists "$m" || die "missing pkg-config module '$m' - see the package list in ffmpeg/README.md"
done
[ -f /mingw64/include/ffnvcodec/nvEncodeAPI.h ] || die "missing NVENC headers - pacman -S mingw-w64-x86_64-ffnvcodec-headers"
[ -d /mingw64/include/AMF ] || die "missing AMF headers - pacman -S mingw-w64-x86_64-amf-headers"

mkdir -p "$WORK" "$(dirname "$OUT")"

# ---------------------------------------------------------------- pkg-config tweak
# MSYS2's x265.pc lists "-lgcc_s", which is the import library of libgcc_s_seh-1.dll:
# with it in the link, even a -static build needs that DLL. Use a private copy of the
# .pc file without it; -static-libgcc then pulls the static unwinder (libgcc_eh.a).
PCDIR="$WORK/pkgconfig"
mkdir -p "$PCDIR"
x265_prefix=$(pkg-config --variable=prefix x265)
x265_version=$(pkg-config --modversion x265)
cat > "$PCDIR/x265.pc" <<EOF
prefix=$x265_prefix
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: x265
Description: H.265/HEVC video encoder
Version: $x265_version
Libs: -L\${libdir} -lx265
Libs.private: -lstdc++ -lgcc -lgcc_eh
Cflags: -I\${includedir}
EOF
export PKG_CONFIG_PATH="$PCDIR${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# ---------------------------------------------------------------- source
if [ ! -f "$SRC/configure" ]; then
  say "== downloading ffmpeg $FFMPEG_VERSION"
  tarball="$WORK/ffmpeg-$FFMPEG_VERSION.tar.xz"
  [ -s "$tarball" ] || curl -fL --retry 3 -o "$tarball" \
    "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
  tar -xJf "$tarball" -C "$WORK"
fi

# ---------------------------------------------------------------- configure
CONFIGURE=(
  --prefix="$WORK/out"
  --target-os=mingw32
  --arch=x86_64
  --enable-static
  --disable-shared
  --pkg-config-flags=--static
  # link everything in, including libgcc/libstdc++/winpthread and the .a (not .dll.a)
  # builds of dav1d/x264/x265/... so the exe needs no DLLs beside it
  --extra-ldflags=-static
  # x265.pc's Libs.private asks for -lgcc_s, which -static alone keeps as the DLL import
  # library; these two force the static runtimes in
  --extra-ldflags=-static-libgcc
  --extra-ldflags=-static-libstdc++
  # libvpl.a contains C++ code (configure probes it with the C compiler), and MSYS2's
  # static libmp3lame.a contains an object that calls mpg123 - name both here so the
  # probe links and the final link is complete.
  # (each --extra-libs is prepended, so these come out as -lmpg123 -lshlwapi -lstdc++)
  --extra-libs=-lstdc++
  --extra-libs=-lshlwapi
  --extra-libs=-lmpg123
  --extra-version=hr

  # No autodetected system libraries: everything external is named below.
  --disable-autodetect
  # Separate programs / docs, not part of ffmpeg itself.
  --disable-doc
  --disable-debug
  --disable-ffplay
  --disable-ffprobe

  # A few things that have nothing to do with recording: HomRec only ever reads
  # and writes local files and pipes, so every network protocol (http, rtmp,
  # udp, srt, rist, ...) is dead weight, and nothing here builds an ASCII-art
  # video (caca). Nothing else is removed - every codec, filter, muxer and
  # demuxer stays, so custom ffmpeg args and imports keep working.
  --disable-network
  --disable-muxer=caca

  # External libraries (GPL: x264/x265; the rest are permissive).
  --enable-gpl
  --enable-version3
  --enable-libx264
  --enable-libx265
  --enable-libdav1d
  --enable-libmp3lame
  --enable-libopus
  --enable-libvorbis
  --enable-zlib

  # Hardware encoding backends used by HomRec.
  --enable-d3d11va
  --enable-dxva2
  --enable-d3d12va
  --enable-ffnvcodec
  --enable-nvenc
  --enable-amf
  --enable-libvpl

  # A normal build: all native components stay enabled. Do not add
  # --disable-everything here unless every single component HomRec (and the
  # user's custom ffmpeg args) can ever need is listed and tested.
)

say "== configure"
cd "$SRC"
./configure "${CONFIGURE[@]}"

# ---------------------------------------------------------------- verify
# A typo or a renamed component does not fail configure - it is silently dropped
# and the build succeeds with a missing piece (this is how `-f s16le` once went
# missing after FFmpeg renamed it to pcm_s16le). Check the components HomRec
# actually calls against the generated config_components.h.
say "== verify the components HomRec uses"
missing=0
check() { # kind name
  local kind=$1 name=$2
  local U K
  U=$(printf '%s' "$name" | sed 's/[^A-Za-z0-9]/_/g' | tr '[:lower:]' '[:upper:]')
  K=$(printf '%s' "$kind" | tr '[:lower:]' '[:upper:]')
  if ! grep -q "^#define CONFIG_${U}_${K} 1$" config_components.h; then
    printf '   NOT ENABLED: %s %s\n' "$kind" "$name"
    missing=1
  fi
}
check indev   dshow
check indev   lavfi
check demuxer nut
check demuxer rawvideo
check demuxer pcm_s16le
check demuxer matroska
check demuxer mov
check demuxer mpegts
check demuxer concat
check demuxer mp3
check demuxer wav
check muxer   mp4
check muxer   matroska
check muxer   mpegts
check muxer   segment
check muxer   nut
check muxer   mp3
check muxer   wav
check muxer   null
check encoder libx264
check encoder libx265
check encoder h264_nvenc
check encoder hevc_nvenc
check encoder h264_amf
check encoder hevc_amf
check encoder h264_qsv
check encoder hevc_qsv
check encoder aac
check encoder libmp3lame
check encoder pcm_s16le
check encoder wrapped_avframe
check decoder rawvideo
check decoder wrapped_avframe
check filter  ddagrab
check filter  hwdownload
check filter  hwmap
check filter  overlay
check filter  scale
check filter  fps
check filter  setpts
check filter  format
check filter  aresample
check filter  apad
check bsf     setts
check bsf     h264_metadata
check bsf     hevc_metadata
check bsf     av1_metadata
[ "$missing" -eq 0 ] || die "components above are missing from the build (renamed in this FFmpeg version? see config_components.h)"
say "   all checked components are enabled"

# ---------------------------------------------------------------- build
JOBS=$(nproc 2>/dev/null || echo 4)
say "== make -j$JOBS (this takes a while)"
make -j"$JOBS"

say "== strip + copy"
strip -o "$OUT" ffmpeg.exe
"$OUT" -hide_banner -version | head -n 3
say
say "built $OUT ($(stat -c %s "$OUT") bytes)"
say "sha256: $(sha256sum "$OUT" | cut -d' ' -f1)"
