<div align="center">

<img src="https://raw.githubusercontent.com/homaaio/homrec/main/icons/hom.png" alt="HomRec Logo" width="100" />

# 🎥 HomRec

**Screen recorder built for weak PCs.**  
No lags. No bloat. No GPU required.

[![Version](https://img.shields.io/badge/version-1.4.3-blue?style=flat-square)](https://github.com/homaaio/homrec/releases)
[![Python](https://img.shields.io/badge/python-3.8+-yellow?style=flat-square&logo=python&logoColor=white)](https://python.org)
[![Platform](https://img.shields.io/badge/platform-Windows-0078d4?style=flat-square&logo=windows)](https://github.com/homaaio/homrec/releases)
[![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)](LICENSE)
[![Telegram](https://img.shields.io/badge/Telegram-@homaexe-2aabee?style=flat-square&logo=telegram)](https://t.me/homaexe)

</div>

---

## 🤔 What is HomRec?

If you've ever tried recording your screen with OBS or Bandicam on an old laptop or office PC — and everything lagged — **HomRec is made for you.**

Written from the ground up to use the minimum possible CPU and RAM. No fancy effects, no GPU requirement, no background services. Just open, record, done.

> Tested on machines where other recorders are completely unusable.

---

## ✨ Features

| Feature | Details |
|---|---|
| 🎞️ **Screen recording** | Full desktop or specific window |
| 🎙️ **Audio capture** | Microphone + Desktop audio (via WASAPI loopback) |
| 🖥️ **Multi-monitor** | Select which monitor to record |
| 📊 **PC Analytics** | CPU, RAM and disk in one combined window |
| ⌨️ **Hotkeys** | F9 start/stop · F10 pause · F11 fullscreen |
| 🌍 **Languages** | English and Russian UI |
| 🎨 **Catppuccin theme** | Dark UI inspired by Catppuccin Macchiato |
| 📁 **Custom output folder** | Choose where recordings are saved |
| 📈 **Recording stats** | Live FPS, duration, frame count in status bar |
| 🔼 **Always on top** | Keep the window above everything else |
| 🔔 **System tray** | Minimise to tray, control recording from tray menu |
| 🔄 **Auto update check** | Notifies you when a new version is out |
| ❓ **Help menu** | Check for updates and report issues directly from the app |

---

## ⚡ Performance modes

| Mode | FPS | Best for |
|---|---|---|
| 🟢 Eco | 8 fps | Very old hardware, office PCs |
| 🔵 Balanced | 15 fps | Default — works on almost anything |
| 🟡 Turbo | 30 fps | Mid-range machines |
| 🔴 Ultra | 60 fps | Modern hardware |

---

## 🚀 Installation

### Option A — .exe (recommended)

**1.** Go to [**Releases**](https://github.com/homaaio/homrec/releases) and download the latest `.zip`

**2.** Unzip anywhere you want — no installer needed

**3.** Download `ffmpeg.exe` from [ffmpeg.org](https://ffmpeg.org/download.html) and place it in the HomRec folder

**4.** Launch `hr.exe`

Your folder should look like this:
```
HomRec/
├── hr.exe          ← main executable
├── ffmpeg.exe      ← required for recording
├── icons/
│   ├── main.ico
│   └── ico.ico
├── config.ini      ← auto-generated
├── homrec.log      ← auto-generated
└── recordings/     ← auto-generated
```

> **Antivirus warning?** Some antiviruses (Kaspersky, Avast) may flag HomRec because it is a new program. It is not a virus — the full source code is on GitHub. Add the HomRec folder to exceptions if needed.

---

### Option B — Run from source

**1. Clone the repo**
```bash
git clone https://github.com/homaaio/homrec.git
cd homrec
```

**2. Install dependencies**
```bash
pip install -r requirements.txt
```

**3. Place ffmpeg**

Download from [ffmpeg.org](https://ffmpeg.org/download.html) and either:
- Place `ffmpeg.exe` in the HomRec folder, **or**
- Add FFmpeg to your system PATH

**4. Run**
```bash
python homrec.py
```

---

## ⌨️ Keyboard shortcuts

| Key | Action |
|---|---|
| `F9` | Start / Stop recording |
| `F10` | Pause / Resume recording |
| `F11` | Toggle fullscreen |

---

## 📦 Dependencies

```
opencv-python
Pillow
mss
pyaudio-wasapi   # fork of pyaudio with WASAPI loopback support (desktop audio)
numpy
psutil           # optional — PC Analytics panel
pystray          # optional — system tray support
```

Install all at once:
```bash
pip install -r requirements.txt
```

> **Desktop audio capture** requires `pyaudio-wasapi` instead of the standard `pyaudio`.  
> Standard `pyaudio` does **not** support WASAPI loopback, so desktop audio will not be recorded.  
> Install it with: `pip install pyaudio-wasapi`

---

## 📋 Changelog

<details>
<summary><b>v1.4.0</b> — 2026-04-24</summary>

**Added**
- Help menu — Check for Updates and Report Issue directly from the app
- Option to disable "minimize to tray on close" in Settings

**Changed**
- PC Analytics combined into one window with CPU, RAM and Disk sections
- Start/Stop merged into one button that changes on recording; Pause and Stop are now compact and side by side

**Fixed**
- FFmpeg console window no longer appears in front of the app after stopping a long recording

</details>

<details>
<summary><b>v1.3.2</b> — 2026-04-23</summary>

**Added**
- System tray — minimise to tray instead of closing, control recording from tray menu
- Window capture — record a specific window (Settings → Capture Source)
- Auto update check — banner appears when a new version is available

**Fixed**
- Microphone stops working after first recording
- 2nd and subsequent recordings were corrupted and unplayable
- Video played faster than real time
- Audio desync on long recordings and after using pause
- FFmpeg not found when running as .exe

**Performance**
- ~30% less CPU usage during capture
- Preview uses fast resampling while recording

</details>

<details>
<summary><b>v1.3.1</b></summary>

- Updated app icons
- Reduced .exe file size by ~2x

</details>

<details>
<summary><b>v1.3.0</b></summary>

- Added logging to homrec.log
- Fixed FFmpeg not found in .exe builds

</details>

<details>
<summary><b>v1.2.0</b> — 2026-03-21</summary>

**Added**
- Separate volume controls for microphone and system audio
- PC Analytics panel (CPU, RAM, disk)
- Multi-language support: English and Russian
- Always on top toggle, fullscreen mode, monitor selection
- Custom output folder, hotkeys F9 / F10 / F11

**Fixed**
- Audio desync on long recordings
- Frame dropping in high-motion scenes
- Settings not saving after restart
- Crash when no microphone is present

</details>

---

## 📣 Stay updated

All news and updates are posted on Telegram first:

**[t.me/homaexe](https://t.me/homaexe)**

---

<div align="center">

Made with ❤️ by **homaaio**

</div>
