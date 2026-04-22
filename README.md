<div align="center">

<img src="https://raw.githubusercontent.com/Homa4ella/homrec/main/assets/logo.png" alt="HomRec Logo" width="120" />

# 🎥 HomRec

**Screen recorder built for weak PCs.**  
No lags. No bloat. No GPU required.

[![Version](https://img.shields.io/badge/version-1.2.0-blue?style=flat-square)](https://github.com/Homa4ella/homrec/releases)
[![Python](https://img.shields.io/badge/python-3.8+-yellow?style=flat-square&logo=python&logoColor=white)](https://python.org)
[![Platform](https://img.shields.io/badge/platform-Windows-0078d4?style=flat-square&logo=windows)](https://github.com/Homa4ella/homrec/releases)
[![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)](LICENSE)
[![Telegram](https://img.shields.io/badge/Telegram-@homaexe-2aabee?style=flat-square&logo=telegram)](https://t.me/homaexe)

</div>

---

## 🤔 What is HomRec?

If you've ever tried recording your screen with OBS or Bandicam on an old laptop or office PC — and everything lagged — **HomRec is made for you.**

The program is written from the ground up to use the minimum possible CPU and RAM. No fancy effects, no GPU requirement, no background services. Just open, record, and done.

> Tested on machines where other recorders are completely unusable.

---

## ✨ Features

| Feature | Details |
|---|---|
| 🎞️ **Screen recording** | Fullscreen or custom area, any monitor |
| 🎙️ **Audio capture** | Microphone with separate volume control |
| 📊 **PC Analytics** | Live CPU, RAM and disk usage panel |
| ⌨️ **Hotkeys** | F9 start/stop · F10 pause · F11 fullscreen |
| 🌍 **Languages** | English and Russian UI |
| 🎨 **Catppuccin theme** | Dark UI inspired by Catppuccin Macchiato |
| 📁 **Custom output folder** | Choose where recordings are saved |
| 🖥️ **Multi-monitor** | Select which monitor to record |
| 📈 **Recording stats** | Live FPS, duration, frame count in status bar |
| 🔼 **Always on top** | Keep the window above everything else |

---

## ⚡ Performance modes

| Mode | FPS | Best for |
|---|---|---|
| 🟢 Eco | 8 fps | Very old hardware, office PCs |
| 🔵 Balanced | 15 fps | Default — works on almost anything |
| 🟡 Turbo | 30 fps | Mid-range machines |
| 🔴 Ultra | 60 fps | Modern hardware |

---

## 🚀 Getting started

### Option A — Download the .exe

Go to [**Releases**](https://github.com/Homa4ella/homrec/releases) and download the latest build.  
Place `ffmpeg.exe` in the **same folder** as `HomRec.exe`, then launch.

### Option B — Run from source

**1. Clone the repo**
```bash
git clone https://github.com/Homa4ella/homrec.git
cd homrec
```

**2. Install dependencies**
```bash
pip install -r requirements.txt
```

**3. Install FFmpeg**

Download from [ffmpeg.org](https://ffmpeg.org/download.html) and either:
- Add to system PATH, **or**
- Place `ffmpeg.exe` next to `homrec.py`

**4. Run**
```bash
python homrec.py
```

---

## 📦 Dependencies

```
opencv-python
Pillow
mss
pyaudio
numpy
psutil        # optional — enables PC Analytics panel
```

---

## ⌨️ Keyboard shortcuts

| Key | Action |
|---|---|
| `F9` | Start / Stop recording |
| `F10` | Pause / Resume recording |
| `F11` | Toggle fullscreen |

---

## 🛡️ Antivirus warning?

Some antiviruses (Kaspersky, Avast) may flag HomRec because it's a new program not yet in their databases. **It is not a virus.**

The full source code is right here — any developer can read and verify every line. If your antivirus blocks the app, add the HomRec folder to exceptions.

---

## 📁 Project structure

```
homrec/
├── homrec.py        # main application
├── config.ini       # saved settings
├── CHANGELOG.txt    # version history
├── requirements.txt
└── README.md
```

---

## 📋 Changelog

<details>
<summary><b>v1.2.0</b> — 2026-03-21</summary>

**Added**
- Separate volume controls for microphone and system audio
- PC Analytics panel (CPU, RAM, disk) — requires `psutil`
- Multi-language support: English and Russian
- "Always on top" toggle
- Fullscreen mode with F11 shortcut
- Monitor selection in advanced settings
- Custom output folder browser
- Hotkey support: F9, F10, F11

**Changed**
- Complete UI redesign with Catppuccin-inspired color scheme
- Improved FFmpeg integration
- Settings dialog now uses tabbed interface (Video / Advanced)
- Rewritten audio recording system for better sync
- Detailed recording stats in status bar

**Fixed**
- Audio desynchronization during long recordings
- Frame dropping in high-motion scenes
- Settings not saving after restart
- Crash when no microphone is present
- Memory leaks in recording stream

**Performance**
- ~30% less CPU usage during capture
- Improved FPS stability across all modes
- Preview updates throttled during recording
- Reduced disk I/O through optimized frame compression

</details>

---

## 📣 Stay updated

All news and updates are posted on Telegram first:

**[t.me/homaexe](https://t.me/homaexe)**

---

<div align="center">

Made with ❤️ by **Homa4ella**

</div>
