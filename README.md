# HomRec 1.4.4 (Legacy)

## Features

- Screen recording via FFmpeg (gdigrab)
- C++ native engine for capture, preview and audio — minimal CPU usage
- WASAPI system audio + microphone capture
- Audio level meters with mute/volume controls
- Live preview (disabled during recording to save CPU)
- 5 built-in themes: Dark, Light, Catppuccin, Nord, Dracula
- 2 languages: English, Русский
- Performance modes: Ultra (60fps) / Turbo (30fps) / Balanced (15fps) / Eco (8fps)
- Hardware acceleration: NVENC, AMF, QSV
- Window capture or full desktop
- Minimize to system tray
- Single settings window — no hidden menus
- Auto-stop timer, countdown, hotkeys
- Single instance (mutex)

---

## Requirements

- Windows 7 / 8 / 10 / 11 (64-bit)
- Python 3.11+
- FFmpeg (place `ffmpeg.exe` next to `homrec.py`)
- MinGW-w64 GCC 13+ (to build the C++ engine DLL)

> **Linux is not supported.** The engine uses gdigrab and WASAPI which are Windows-only.

---

## Quick Start

### 1. Install Python dependencies

```
pip install pillow mss psutil pystray
```

### 2. Build the C++ engine

```
g++ -O2 -shared -o homrec_engine.dll src/capture_engine.cpp -lgdi32 -luser32 -lwinmm -lole32 -static-libgcc -static-libstdc++ -std=c++17
```

> If you skip this step the app will still work using a Python fallback (higher CPU usage).

### 3. Place FFmpeg

Download from https://ffmpeg.org and put `ffmpeg.exe` in the project folder.

### 4. Run

```
python homrec.py
```

---

## Build .exe

```
pip install pyinstaller

pyinstaller --onefile --windowed --name "HomRec_Legacy" --add-binary "homrec_engine.dll;." --add-data "icons;icons" --icon "icons/main.ico" homrec.py
```

Output: `dist/HomRec_Legacy.exe`  
Copy `ffmpeg.exe` next to the `.exe` before distributing.

---

## Project Structure

```
HomRec_Legacy/
├── src/
│   ├── capture_engine.hpp   -- C++ engine header
│   └── capture_engine.cpp   -- C++ engine (capture / record / audio)
├── icons/
│   ├── main.ico
│   └── rec.ico
├── homrec.py                -- main application
├── engine.py                -- Python ctypes bridge to DLL
├── CMakeLists.txt           -- optional CMake build
├── requirements.txt
└── README.md
```

---

## Performance

| Mode | FPS | Scale | Recommended for |
|------|-----|-------|-----------------|
| Ultra | 60 | 100% | Modern PC |
| Turbo | 30 | 100% | Mid-range PC |
| Balanced | 15 | 75% | Old PC (default) |
| Eco | 8 | 50% | Very weak PC |

**CPU usage on i3-1005G1:**

| State | CPU |
|-------|-----|
| Idle (window open) | ~1-2% |
| Window minimised | ~0% |
| Recording (Balanced) | ~11% |

---

## Settings

All settings are in one window (`Settings → Preferences`):

| Tab | Options |
|-----|---------|
| Video | Mode, Monitor, Codec, Preset, CRF, HW Accel |
| Audio | Sample rate, AAC bitrate |
| Recording | Output folder, Countdown, Cursor, Auto-stop |
| Hotkeys | Start/Stop, Pause |
| Appearance | Theme, Language, Notifications |

---

## Changelog

### 1.4.4 (Legacy)
- C++ engine for capture, preview and audio
- Removed Advanced Settings — everything merged into main Settings
- Removed custom themes (.hrt) and custom languages (.hrl / .hrc)
- Preview disabled during recording to reduce CPU load
- Integer-only bilinear scaling in C++ (no floating point)
- 1ms Windows timer resolution via timeBeginPeriod
- Static linking — no MSVC runtime dependency
- Version string updated to `1.4.4 (Legacy)`

---

## License

MIT — see LICENSE file.

---

*by Homa4ella*
