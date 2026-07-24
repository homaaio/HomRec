# HomRec (Hardware Optimized Mechanism Recorder)

[![Version](https://img.shields.io/badge/version-1.7.2-blue?style=flat-square)](https://github.com/homaaio/homrec/releases)
[![Discord](https://img.shields.io/badge/Discord-0047ab?style=flat-square&logo=discord)](https://discord.gg/Gv4t6Xhy7E)
[![Telegram](https://img.shields.io/badge/Telegram-1D77A3?style=flat-square&logo=telegram)](https://t.me/homaexe)

**Screen recorder built for weak PCs.**
No lags. No bloat. No GPU required.

---

## What is HomRec?

If you've ever tried recording your screen with OBS or Bandicam on an old laptop or office PC and everything lagged — **HomRec is made for you.**

Written from the ground up to use the minimum possible CPU and RAM. No fancy effects, no GPU requirement, no background services. Just open, record, done.

**HomRec is 100% native C++/C.** There is no Python anywhere in this project — the entire app (UI, recording pipeline, audio, plugins, console) compiles directly into a single `hr.exe` with no runtime dependencies beyond FFmpeg.

---

## Features

| Feature | Details |
|---|---|
| **Screen recording** | Full desktop or specific window |
| **Audio capture** | Microphone + Desktop audio (via WASAPI loopback) |
| **Multi-monitor** | Select which monitor to record |
| **Hotkeys** | F9 start/stop · F10 pause · F11 fullscreen |
| **Custom languages** | Drop a `.hrl` file in Advanced Settings to add any language instantly |
| **Overlays** | Add and position text/image/webcam overlays on your recording |
| **Custom output folder** | Choose where recordings are saved |
| **Recording stats** | Live FPS, duration, frame count in status bar |
| **Console window** | Access advanced commands directly via the built-in console |
| **Plugins** | Lua-scripted plugins with full filesystem/network access |
| **Always on top** | Keep the window above everything else |
| **System tray** | Minimise to tray, control recording from tray menu |
| **Help menu** | Check for updates and report issues directly from the app |

---

## Installation

### Option A — .exe (recommended)

**1.** Go to [**Releases**](https://github.com/homaaio/homrec/releases) and download the latest `.zip` or `.7z`         
**2.** Unzip anywhere you want — no installer needed     
**3.** Launch `hr.exe`     

> FFmpeg is already included in the archive — no extra downloads needed.
> **Antivirus warning?** Some antiviruses (Kaspersky, Avast) may flag HomRec because it is a new program. It is not a virus — the full source code is on GitHub. Add the HomRec folder to exceptions if needed.

---

### Option B — Build from source

HomRec is a native C++/C project — building it means compiling, not installing a Python environment.

**1. Clone the repo**
```bash
git clone https://github.com/homaaio/HomRec.git
cd homrec
```

**2. Get a C++ toolchain**
Windows needs a MinGW-w64 toolchain (g++, gcc, windres, make). The easiest way: install [MSYS2](https://www.msys2.org/), then inside its terminal:
```bash
pacman -S mingw-w64-x86_64-toolchain
```

**3. Get Lua 5.4**
Plugins are Lua-scripted, so the build needs Lua's headers/library. Either:
```bash
vcpkg install lua:x64-mingw-dynamic
```
or download the amalgamation from [lua.org](https://www.lua.org/download.html) and point the build at it (see the Makefile's `LUA_CFLAGS`/`LUA_LDFLAGS` variables).

**4. Place ffmpeg**
Download from [ffmpeg.org](https://ffmpeg.org/download.html) and either:
- Place `ffmpeg.exe` in the HomRec folder, **or**
- Add FFmpeg to your system PATH

**5. Build**
```bash
make
```
This compiles everything — UI, recording engine, audio, plugin host — directly into one `hr.exe`. No separate DLL step, no packaging step.

**6. Run**
```bash
hr.exe
```

---

## Keyboard shortcuts

| Key | Action |
|---|---|
| `F9` | Start / Stop recording |
| `F10` | Pause / Resume recording |
| `F11` | Toggle fullscreen |

---

## Custom languages (.hrl)

HomRec supports custom language files in the `.hrl` format (HomRec Language).
To install one: **Advanced Settings → Interface → 📥 Install .hrl...**

The language applies instantly — no restart needed. You can also drop `.hrl` files directly onto the HomRec window if drag-and-drop is available on your system.

Want to create your own? Each `.hrl` file is a compressed JSON with all UI strings based on the English template. Community-made language files can be shared on the Discord server.

---

## Plugins

Plugins are written in **Lua**, not Python. Drop a folder into `plugins/`:
```
plugins/
  my_plugin/
    plugin.json     { "id": "my_plugin", "name": "My Plugin", "version": "1.0", "entry": "main.lua" }
    main.lua
```
Plugins get full filesystem and network access (`io`, `os`, and `homrec.http_get`/`http_post` are all available), plus hooks like `on_load`, `on_recording_start`, `on_recording_stop`, and a `homrec.*` API for toasts, colors, and cross-plugin events.

---

## Build dependencies

```
MinGW-w64 toolchain (g++, gcc, windres, make)
Lua 5.4 (headers + library)
```

That's it — no `pip install`, no `requirements.txt`. Everything else (capture, encoding, audio) is built-in native code with no third-party runtime dependencies.

---

## Stay updated

### **[t.me/homaexe](https://t.me/homaexe)** / **[x.com/homrec_dev](https://x.com/homrec_dev)** / [discord.gg](https://discord.gg/Gv4t6Xhy7E)

---

<div align="center">
Made with ❤️ by <b>homaaio</b>
</div>
