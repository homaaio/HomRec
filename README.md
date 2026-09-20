# HomRec (Hardware Optimized Mechanism Recorder)

[![Version](https://img.shields.io/badge/version-2.2.1-blue?style=flat-square)](https://github.com/homaaio/homrec/releases)
[![Discord](https://img.shields.io/badge/Discord-0047ab?style=flat-square&logo=discord)](https://discord.gg/Gv4t6Xhy7E)
[![Telegram](https://img.shields.io/badge/Telegram-1D77A3?style=flat-square&logo=telegram)](https://t.me/homaexe)
[![Boosty](https://img.shields.io/badge/support-boosty-orange?style=flat-square)](https://boosty.to/homa4ella/donate)

A native C/C++ screen recorder for low-spec Windows machines. Single-binary (`hr.exe`), FFmpeg for encoding, no GPU requirement, no bundled runtime.

## Contents

- [Overview](#overview)
- [Features](#features)
- [Installation](#installation)
  - [Installer](#installer)
  - [Portable](#portable)
  - [From source](#from-source)
- [Usage](#usage)
  - [Keyboard shortcuts](#keyboard-shortcuts)
  - [Custom languages](#custom-languages)
  - [Console startup scripts](#console-startup-scripts)
- [Plugins](#plugins)
- [Build dependencies](#build-dependencies)
- [Support & community](#support--community)

## Overview

HomRec targets a specific problem: general-purpose screen recorders (OBS, Bandicam) assume more CPU/GPU headroom than a lot of real-world machines have. HomRec is built to avoid that assumption entirely - no compositor, no GPU dependency, no background services beyond what active recording requires.

The application is 100% native C/C++: UI, capture pipeline, audio, plugin host, and console all compile into a single `hr.exe` with no runtime dependency beyond FFmpeg (bundled in the portable distribution). Note that this refers to the *application* - a small number of Python scripts exist under `tools/` for maintainer use (build packaging, spec-checking, language file conversion) and are not part of `hr.exe` or required to run it.

## Features

- **Video capture** - full desktop, a chosen monitor, or a specific window; output scaled or at an exact resolution
- **Encoding** - automatic hardware-encoder detection (NVENC, AMD AMF, Intel Quick Sync) with software fallback (libx264/libx265); MP4 or MKV output
- **Audio capture** - microphone and/or desktop audio (WASAPI loopback), combined or exported as separate tracks
- **Multi-monitor support** - select which display to record
- **Overlays** - text, image, webcam, and an input overlay (on-screen keypress/mouse-click display)
- **Instant Replay** - background rolling buffer, saved on demand via hotkey, independent of active recording
- **Hotkeys** - start/stop, pause/resume, fullscreen toggle, save-replay, all user-rebindable
- **Custom languages** - `.hrl` translation files, applied without restarting
- **Plugin system** - Lua-scripted, filesystem/network access, lifecycle hooks
- **Console** - built-in command console with optional autorun scripts
- **Performance controls** - live preview can be disabled at the pipeline level (not just hidden in the UI), with independently adjustable preview resolution/FPS when left on
- **System integration** - system tray, always-on-top, desktop shortcut, launch-at-startup, in-app self-update

## Installation

### Installer

1. Download `homrec-setup-*.exe` from [Releases](https://github.com/homaaio/homrec/releases).
2. Run it. Installs per-user, no admin rights required.
3. Optional: enable a desktop shortcut and/or launch-at-startup from the Tasks page (both also configurable later, in Settings > System).

The installer registers a standard uninstaller (Windows Settings > Apps). Subsequent updates can be applied in-app via Help > Check for Updates, without re-running the installer.

### Portable

1. Download the latest `.zip` or `.7z` from [Releases](https://github.com/homaaio/homrec/releases).
2. Extract to any location.
3. Run `hr.exe`. FFmpeg is included in the archive.

**Note on antivirus warnings:** some antivirus products (reported with Kaspersky and Avast) flag `hr.exe` heuristically as a new/unsigned binary. This is a false positive - the source is public in this repository. Add an exception if needed.

### From source

Requirements: a MinGW-w64 toolchain and Lua 5.4 (headers + library).

```bash
git clone https://github.com/homaaio/HomRec.git
cd homrec
```

Install a toolchain (via [MSYS2](https://www.msys2.org/)):
```bash
pacman -S mingw-w64-x86_64-toolchain
```

Obtain Lua 5.4:
```bash
vcpkg install lua:x64-mingw-dynamic
```
(or the amalgamation from [lua.org](https://www.lua.org/download.html) - point the build at it via the Makefile's `LUA_CFLAGS`/`LUA_LDFLAGS`)

Place `ffmpeg.exe` next to the build output, or ensure it's on PATH.

Build and run:
```bash
make
hr.exe
```

`make` produces a single `hr.exe` covering the UI, recording engine, audio, and plugin host - no separate library/packaging step.

To reproduce the installer itself, see [`installer/README.md`](installer/README.md) (`installer/HomRec.iss`, an Inno Setup script - buildable manually or via `tools/homrec_build.py`).

## Usage

### Keyboard shortcuts

| Key (default) | Action |
|---|---|
| `F9` | Start / Stop recording |
| `F10` | Pause / Resume recording |
| `F11` | Toggle HomRec's own window fullscreen |
| `F8` | Save Instant Replay |

All shortcuts are configurable in Settings > Hotkeys.

### Custom languages

Language files use the `.hrl` format (compressed JSON, keyed against the built-in English template).

To install: Settings > General > **Add Language...** > select file > **Save**. Applies immediately, no restart required.

To create one: use `tools/hrl_tool.py` to pack/unpack `.hrl` files against plain JSON. Community translations are commonly shared via Discord.

### Console startup scripts

Two optional, plain-text script files under `cfg/` (auto-created next to `hr.exe` on first run):

| File | Runs when |
|---|---|
| `autoexec.cfg` | Every application launch |
| `startrec.cfg` | Start of every recording |

Format: one console command per line; `//` or `#` for comments. See [`cfg/README.md`](cfg/README.md) for the command reference and example files.

## Plugins

Plugins are written in Lua and packaged as a directory under `plugins/`:

```
plugins/
  my_plugin/
    plugin.json     { "id": "my_plugin", "name": "My Plugin", "version": "1.0", "entry": "main.lua" }
    main.lua
```

Plugin code has full filesystem and network access (`io`, `os`, `homrec.http_get`/`http_post`), lifecycle hooks (`on_load`, `on_recording_start`, `on_recording_stop`), and a `homrec.*` API (toasts, color helpers, cross-plugin events). Given this access level, only install plugins from trusted sources.

A packaged plugin (`.hrp`, a renamed `.zip`) can be installed by:
- placing it in `plugins/` manually and restarting HomRec, or
- **File > Import Plugin (.hrp)...** in the app, which loads it without a restart, or
- `hom install <plugin-name>`, using the CLI plugin manager in `tools/hom/`

See [`plugins/READMEplugins.md`](plugins/READMEplugins.md) for the complete plugin API reference.

## Build dependencies

```
MinGW-w64 toolchain (g++, gcc, windres, make)
Lua 5.4 (headers + library)
```

## Support & community

- Releases: [github.com/homaaio/homrec/releases](https://github.com/homaaio/homrec/releases)
- Telegram: [t.me/homaexe](https://t.me/homaexe)
- X: [x.com/homrec_dev](https://x.com/homrec_dev)
- Discord: [discord.gg/Gv4t6Xhy7E](https://discord.gg/Gv4t6Xhy7E)
- Support (Boosty): [Boosty](https://boosty.to/homa4ella/donate)

<div align="center">
Made with ❤️ by <b>homaaio</b>
</div>
