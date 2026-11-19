# ffmpeg-hr - the FFmpeg build HomRec uses

`ffmpeg-hr` is a lean, single-file FFmpeg for HomRec. Users install it with

```
inwid hom install ffmpeg-hr
```

in HomRec's console (`Ctrl+Shift+T`). `hom` downloads it and puts it at
`<HomRec>\ffmpeg\ffmpeg.exe`, which is one of the places HomRec already searches
(see `hr_check_ffmpeg()` in `src/core/tools.cpp`).

The package entry lives in the hom repo, `pkgs/index.json`, and uses the `into` key:

```json
{
  "name": "ffmpeg-hr",
  "type": "file",
  "file": "ffmpeg.exe",
  "dest": "ffmpeg",
  "into": "homrec",
  "url": "https://github.com/homaaio/hom/releases/download/pkgs/ffmpeg-hr-8.0.1.exe",
  "version": "8.0.1",
  "sha256": "..."
}
```

`file` is the name it gets after installing (`ffmpeg.exe`); `url` points at the versioned asset, so the
release can keep `ffmpeg-hr-8.0.1.exe`, `ffmpeg-hr-8.0.2.exe`, ... and the installed name stays the same.

`into: homrec` needs hom **0.8.1** or newer (or hom 2.x).

## Build it

You need [MSYS2](https://www.msys2.org/) and its **MINGW64** toolchain. In an MSYS2 MINGW64 shell:

```bash
pacman -Syu --needed --noconfirm \
  mingw-w64-x86_64-gcc mingw-w64-x86_64-make mingw-w64-x86_64-nasm \
  mingw-w64-x86_64-pkgconf \
  mingw-w64-x86_64-x264 mingw-w64-x86_64-x265 mingw-w64-x86_64-dav1d \
  mingw-w64-x86_64-lame mingw-w64-x86_64-mpg123 mingw-w64-x86_64-opus \
  mingw-w64-x86_64-libvorbis \
  mingw-w64-x86_64-libvpl \
  mingw-w64-x86_64-ffnvcodec-headers mingw-w64-x86_64-amf-headers

cd /path/to/HomRec
./ffmpeg/build-ffmpeg-hr.sh                 # -> dist/ffmpeg-hr.exe
./ffmpeg/build-ffmpeg-hr.sh D:/ffmpeg.exe   # or choose the output path
```

The first run downloads `ffmpeg-8.0.1.tar.xz` into `.ffmpeg-build/` and builds in the same folder;
later runs re-use the objects. Set `HR_FFMPEG_WORK` to build somewhere else, and `FFMPEG_VERSION` to
build a different FFmpeg version.

The script ends with the version line, the size and the sha256. The result is fully static: no
`.dll` file is needed next to it, and it runs from any folder.

CI: `.github/workflows/ffmpeg.yml` (Actions > ffmpeg-hr > Run workflow) builds the same file on
`windows-latest` and uploads it as an artifact.

## What is inside

Built from FFmpeg **8.0.1** with `--disable-everything` plus only the parts below. The full list is in
`build-ffmpeg-hr.sh`; in short:

- **Encoders:** libx264, libx265, dav1d (decode), H.264/HEVC/AV1 through NVENC, AMF and QSV,
  mpeg4, MJPEG, PNG/BMP/GIF/APNG, AAC, libmp3lame, libopus, libvorbis, FLAC, AC-3/E-AC-3, PCM
  (s16/s24/f32) - what HomRec records and what its audio tools write.
- **Containers:** MP4/MOV (fragmented too), Matroska/WebM, NUT, MPEG-TS, `segment` (Instant Replay),
  WAV, MP3, Ogg/FLAC/ADTS, `concat`, `image2`, `null`.
- **Inputs/devices:** `dshow` (webcam, `-list_devices`), `lavfi` (`-f lavfi -i nullsrc`, the GPU/codec
  probes), `rawvideo`, NUT, `s16le`. The CLI's own `wrapped_avframe` decoder is included, which the
  `lavfi` sources need.
- **Filters:** `ddagrab`, `hwdownload`, `hwmap`, `hwupload`, `scale`, `overlay`, `format`, `fps`,
  `setpts`, `crop`, `pad`, `split`, `concat`, `trim`, `select`, `fade`, `volume`, `adelay`, `amix`,
  `aresample`, `atempo`, plus the buffer sources/sinks.
- **Bitstream filters:** `setts` (the fast re-time path), `h264_metadata`, `hevc_metadata`,
  `av1_metadata`, `aac_adtstoasc`, `h264_mp4toannexb`, `hevc_mp4toannexb`, `extract_extradata`.
- **Hardware:** `d3d11va`, `dxva2`, `d3d12va` decoding; `-ffnvcodec`/`-nvenc`, `-amf`, `-libvpl`.

Left out on purpose: `ffplay`, `ffprobe`, documentation, networking protocols (HTTP/RTMP/SRT/...),
DVD/Blu-ray, subtitles and font shaping, AVISynth, SDL, OpenCL/Vulkan/libplacebo/shaderc, Whisper,
TTS, VMAF/vidstab/zimg, image formats HomRec never writes (JPEG XL, WebP output, OpenJPEG) and the
software AV1 encoders (rav1e, SVT-AV1, vvenc, aom, xevd). Hardware AV1 encoding **is** included.

The script checks the pieces HomRec calls today: run `ffmpeg-hr.exe -hide_banner -filters` for
`ddagrab`, `-encoders` for `libx264`/`nvenc`/`amf`/`qsv`, `-muxers` for `mp4`/`nut`/`mpegts`/`segment`,
and `-devices` for `dshow`.

## Licence and source

`--enable-gpl --enable-version3` (libx264, libx265) make the result **GPLv3**. If you publish the
`.exe`, publish the corresponding source as well:

1. this folder (`build-ffmpeg-hr.sh` is the exact configuration), and
2. <https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz>, plus
3. the MSYS2 source packages for libx264, libx265, dav1d, lame, mpg123, opus, libvorbis, libvpl,
   ffnvcodec-headers and amf-headers (`pacman -Q` prints the installed versions).

The exact configure line is printed by `ffmpeg-hr.exe -version`; keep it with the download so anyone
can rebuild the binary. HomRec's own licence and the FFmpeg LGPL/GPL notices are in the HomRec
repository.

## Publishing a new build

1. Build and test it as above.
2. Upload `dist/ffmpeg-hr.exe` to the `pkgs` release of the hom repository **as `ffmpeg-hr-<version>.exe`**
   (keep the version in the asset name; the installed name comes from the index's `file`).
3. In the hom repo, add or update the entry (this fills in the sha256 of that exact upload):

   ```
   python tools/pub.py add /path/to/ffmpeg-hr.exe --name ffmpeg-hr --type file \
     --file ffmpeg.exe --version 8.0.1 --into homrec --dest ffmpeg --author homaaio \
     --desc "Lean FFmpeg 8.0.1 for HomRec (x264/x265 + NVENC/AMF/QSV, ddagrab)" \
     --url https://github.com/homaaio/hom/releases/download/pkgs/ffmpeg-hr-8.0.1.exe
   python tools/pub.py check
   ```

   Do not copy a hash from a different build: a CI rebuild produces a different byte sequence, so
   upload **that** file and let `pub.py` hash it (or re-run `pub.py add` after the rebuild).
4. Commit `pkgs/index.json` and push. `hom` reads it from `main`.
