#!/usr/bin/env bash
# build-ffmpeg-hr.sh - builds ffmpeg-hr, the lean static FFmpeg that HomRec uses.
#
# Run it in an MSYS2 MINGW64 shell (or let .github/workflows/ffmpeg.yml run it):
#
#   ./ffmpeg/build-ffmpeg-hr.sh                 # -> dist/ffmpeg-hr.exe
#   ./ffmpeg/build-ffmpeg-hr.sh /path/ffmpeg.exe
#
# Only one file comes out: a static ffmpeg.exe with no DLLs beside it. It contains what
# hr.exe/homrec.exe call today (see "WHAT HOMREC NEEDS" below) plus the codecs/containers
# that a recorder is likely to need later (AV1, VP9, GIF/PNG output, audio import/export).
#
# WHAT HOMREC NEEDS (checked against the sources, 3.0.x):
#   * encoders: libx264, libx265, h264/hevc/av1 _nvenc/_amf/_qsv, mpeg4, aac, libmp3lame,
#     pcm_s16le (the separate MP3/WAV audio files)
#   * decoders/filters for the audio-file mixer and GIF/image overlays (paths are decoded by
#     HomRec itself, this is for the "Audio file..." track and for future thumbnails)
#   * input: dshow (webcam), lavfi (the GPU encoder/ddagrab probes), rawvideo, NUT, s16le
#   * output: mp4 (fragmented), matroska, mpegts + segment (Instant Replay), nut, wav, mp3
#   * filters: ddagrab, hwdownload, hwmap, scale, overlay, format, fps, setpts
#   * bsfs: setts (the re-time fast path), h264/hevc/av1_metadata (ddagrab colour tags),
#     aac_adtstoasc, h264/hevc_mp4toannexb (mpegts / concat)
#
# Deliberately NOT in the build ("definitely never needed"): ffplay/ffprobe, docs, networking
# (http/rtmp/srt/rist/ssh/zmq), DVD/Blu-ray, subtitles/font shaping, AVISynth, SDL, filters
# that need OpenCL/Vulkan/libplacebo/shaderc, Whisper/FLITE/TTS, VMAF/vidstab/zimg/Lensfun,
# image formats we never write (JPEG XL/WebP/OpenJPEG), and the software AV1 encoders
# (rav1e/SVT-AV1/vvenc/aom/xevd) - hardware AV1 is in.
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
# Keep these lists sorted; duplicates are harmless but make the diff noisy.
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

  # nothing we do not ask for: no ffplay/ffprobe/doc, no autodetected system libraries
  --disable-everything
  --disable-autodetect
  --disable-doc
  --disable-debug
  --disable-network
  --disable-ffplay
  --disable-ffprobe

  # external libraries that matter (GPL: x264/x265; the rest are permissive)
  --enable-gpl
  --enable-version3
  --enable-libx264
  --enable-libx265
  --enable-libdav1d
  --enable-libmp3lame
  --enable-libopus
  --enable-libvorbis

  # hardware: the same backends the full gyan build has
  --enable-d3d11va
  --enable-dxva2
  --enable-d3d12va
  --enable-ffnvcodec
  --enable-nvenc
  --enable-amf
  --enable-libvpl

  # devices and file/pipe I/O only: dshow is the webcam; lavfi is "-f lavfi -i nullsrc"
  # (the GPU/codec probes) - both are input *devices*, not demuxers
  --enable-indev=dshow
  --enable-indev=lavfi
  --enable-protocol=file
  --enable-protocol=pipe

  # -- demuxers
  --enable-demuxer=aac
  --enable-demuxer=ac3
  --enable-demuxer=asf
  --enable-demuxer=avi
  --enable-demuxer=concat
  --enable-demuxer=eac3
  --enable-demuxer=flac
  --enable-demuxer=gif
  --enable-demuxer=h264
  --enable-demuxer=hevc
  --enable-demuxer=image2
  --enable-demuxer=lavfi
  --enable-demuxer=matroska
  --enable-demuxer=mov
  --enable-demuxer=mp3
  --enable-demuxer=mpegts
  --enable-demuxer=nut
  --enable-demuxer=ogg
  --enable-demuxer=rawvideo
  --enable-demuxer=s16le
  --enable-demuxer=s24le
  --enable-demuxer=f32le
  --enable-demuxer=wav

  # -- muxers
  --enable-muxer=adts
  --enable-muxer=apng
  --enable-muxer=avi
  --enable-muxer=flac
  --enable-muxer=gif
  --enable-muxer=image2
  --enable-muxer=ipod
  --enable-muxer=matroska
  --enable-muxer=mov
  --enable-muxer=mp3
  --enable-muxer=mp4
  --enable-muxer=mpegts
  --enable-muxer=null
  --enable-muxer=nut
  --enable-muxer=ogg
  --enable-muxer=rawvideo
  --enable-muxer=segment
  --enable-muxer=s16le
  --enable-muxer=wav
  --enable-muxer=webm

  # -- video encoders
  --enable-encoder=libx264
  --enable-encoder=libx265
  --enable-encoder=h264_nvenc
  --enable-encoder=hevc_nvenc
  --enable-encoder=av1_nvenc
  --enable-encoder=h264_amf
  --enable-encoder=hevc_amf
  --enable-encoder=av1_amf
  --enable-encoder=h264_qsv
  --enable-encoder=hevc_qsv
  --enable-encoder=av1_qsv
  --enable-encoder=mpeg4
  --enable-encoder=mjpeg
  --enable-encoder=png
  --enable-encoder=bmp
  --enable-encoder=gif
  --enable-encoder=apng
  --enable-encoder=rawvideo

  # -- audio encoders
  --enable-encoder=aac
  --enable-encoder=libmp3lame
  --enable-encoder=libopus
  --enable-encoder=libvorbis
  --enable-encoder=pcm_s16le
  --enable-encoder=pcm_s24le
  --enable-encoder=pcm_f32le
  --enable-encoder=flac
  --enable-encoder=ac3
  --enable-encoder=eac3
  --enable-encoder=mp2

  # -- decoders used by the ffmpeg CLI itself
  --enable-decoder=wrapped_avframe

  # -- video decoders (audio-file mixer, future trimming, thumbnails)
  --enable-decoder=h264
  --enable-decoder=hevc
  --enable-decoder=av1
  --enable-decoder=libdav1d
  --enable-decoder=vp8
  --enable-decoder=vp9
  --enable-decoder=mpeg1video
  --enable-decoder=mpeg2video
  --enable-decoder=mpeg4
  --enable-decoder=mjpeg
  --enable-decoder=png
  --enable-decoder=bmp
  --enable-decoder=gif
  --enable-decoder=webp
  --enable-decoder=tiff
  --enable-decoder=wmv1
  --enable-decoder=wmv2
  --enable-decoder=wmv3
  --enable-decoder=vc1
  --enable-decoder=h263
  --enable-decoder=rawvideo
  --enable-decoder=prores
  --enable-decoder=dnxhd
  --enable-decoder=theora

  # -- audio decoders
  --enable-decoder=aac
  --enable-decoder=aac_latm
  --enable-decoder=mp3
  --enable-decoder=mp2
  --enable-decoder=mp1
  --enable-decoder=ac3
  --enable-decoder=eac3
  --enable-decoder=dca
  --enable-decoder=truehd
  --enable-decoder=flac
  --enable-decoder=vorbis
  --enable-decoder=opus
  --enable-decoder=wmav1
  --enable-decoder=wmav2
  --enable-decoder=wmapro
  --enable-decoder=wmalossless
  --enable-decoder=pcm_s16le
  --enable-decoder=pcm_s24le
  --enable-decoder=pcm_s32le
  --enable-decoder=pcm_f32le
  --enable-decoder=pcm_u8
  --enable-decoder=pcm_alaw
  --enable-decoder=pcm_mulaw
  --enable-decoder=alac

  # -- parsers
  --enable-parser=aac
  --enable-parser=aac_latm
  --enable-parser=ac3
  --enable-parser=av1
  --enable-parser=dca
  --enable-parser=flac
  --enable-parser=gif
  --enable-parser=h264
  --enable-parser=hevc
  --enable-parser=mjpeg
  --enable-parser=mpeg4video
  --enable-parser=mpegvideo
  --enable-parser=mpegaudio
  --enable-parser=opus
  --enable-parser=png
  --enable-parser=vc1
  --enable-parser=vorbis
  --enable-parser=vp8
  --enable-parser=vp9

  # -- bitstream filters
  --enable-bsf=aac_adtstoasc
  --enable-bsf=av1_metadata
  --enable-bsf=extract_extradata
  --enable-bsf=h264_metadata
  --enable-bsf=h264_mp4toannexb
  --enable-bsf=hevc_metadata
  --enable-bsf=hevc_mp4toannexb
  --enable-bsf=setts
  --enable-bsf=vp9_metadata
  --enable-bsf=vp9_superframe

  # -- filters (the recording pipeline + the audio mixer; a few for later)
  --enable-filter=abuffersink
  --enable-filter=abuffersrc
  --enable-filter=adelay
  --enable-filter=afade
  --enable-filter=amix
  --enable-filter=amerge
  --enable-filter=anull
  --enable-filter=anullsrc
  --enable-filter=apad
  --enable-filter=aresample
  --enable-filter=asetpts
  --enable-filter=atempo
  --enable-filter=atrim
  --enable-filter=boxblur
  --enable-filter=buffersink
  --enable-filter=buffersrc
  --enable-filter=color
  --enable-filter=concat
  --enable-filter=crop
  --enable-filter=ddagrab
  --enable-filter=delogo
  --enable-filter=fade
  --enable-filter=format
  --enable-filter=fps
  --enable-filter=hflip
  --enable-filter=hwdownload
  --enable-filter=hwmap
  --enable-filter=hwupload
  --enable-filter=null
  --enable-filter=nullsrc
  --enable-filter=overlay
  --enable-filter=pad
  --enable-filter=scale
  --enable-filter=select
  --enable-filter=setpts
  --enable-filter=setsar
  --enable-filter=sine
  --enable-filter=split
  --enable-filter=testsrc
  --enable-filter=testsrc2
  --enable-filter=transpose
  --enable-filter=trim
  --enable-filter=vflip
  --enable-filter=volume

  # hardware decoders (only the d3d11 paths; no CUDA/VAAPI/Vulkan)
  --enable-hwaccel=d3d11va
  --enable-hwaccel=dxva2
  --enable-hwaccel=d3d12va
)

say "== configure"
cd "$SRC"
./configure "${CONFIGURE[@]}"

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
