# HomRec
**Screen recorder built for weak PCs.**  
No lags. No bloat. No GPU required.

[![Version](https://img.shields.io/badge/version-1.6.3-blue?style=flat-square)](https://github.com/homaaio/homrec/releases)
[![Python](https://img.shields.io/badge/python-3.8+-yellow?style=flat-square&logo=python&logoColor=white)](https://python.org)
[![Platform](https://img.shields.io/badge/platform-Windows-0078d4?style=flat-square&logo=windows)](https://github.com/homaaio/homrec/releases)
[![Telegram](https://img.shields.io/badge/Telegram-@homaexe-2aabee?style=flat-square&logo=telegram)](https://t.me/homaexe)

---

## What is HomRec?

If you've ever tried recording your screen with OBS or Bandicam on an old laptop or office PC and everything lagged — **HomRec is made for you.**

Written from the ground up to use the minimum possible CPU and RAM. No fancy effects, no GPU requirement, no background services. Just open, record, done.

---

## ✨ Features

| Feature | Details |
|---|---|
| 🎞️ **Screen recording** | Full desktop or specific window |
| 🎙️ **Audio capture** | Microphone + Desktop audio (via WASAPI loopback) |
| 🖥️ **Multi-monitor** | Select which monitor to record |
| 📊 **PC Analytics** | CPU, RAM and disk in one combined window |
| ⌨️ **Hotkeys** | F9 start/stop · F10 pause · F11 fullscreen |
| 🌍 **Languages** | English and Russian UI (You can add more with .hrl files!) |
| 🎨 **Catppuccin theme** | Dark UI inspired by Catppuccin Macchiato |
| 📁 **Custom output folder** | Choose where recordings are saved |
| 📈 **Recording stats** | Live FPS, duration, frame count in status bar |
| 🔼 **Always on top** | Keep the window above everything else |
| 🔔 **System tray** | Minimise to tray, control recording from tray menu |
| 🔄 **Auto update check** | Notifies you when a new version is out |
| ❓ **Help menu** | Check for updates and report issues directly from the app |

---

## 🚀 Installation

### Option A — .exe (recommended)

**1.** Go to [**Releases**](https://github.com/homaaio/homrec/releases) and download the latest `.zip`

**2.** Unzip anywhere you want — no installer needed

**3.** Launch `!hr.exe`

> ffmpeg is already included in the archive — no extra downloads needed.

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
numpy
psutil           # optional — PC Analytics panel
pystray          # optional — system tray support
```

Install all at once:
```bash
pip install -r requirements.txt
```

> Audio capture (microphone + desktop) is handled natively via built-in WASAPI C++ libraries — no extra audio packages required.

---

## 📣 Stay updated

### **[t.me/homaexe](https://t.me/homaexe)** / **[x.com/homrec_dev](https://x.com/homrec_dev)**

---

<div align="center">
Made with ❤️ by <b>homaaio</b>
</div>
