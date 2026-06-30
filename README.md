# HomRec (Hardware Optimized Mechanism Recorder)
**Screen recorder built for weak PCs.**  
No lags. No bloat. No GPU required.

[![Version](https://img.shields.io/badge/version-1.6.4-blue?style=flat-square)](https://github.com/homaaio/homrec/releases)
[![Discord](https://img.shields.io/badge/Discord-0047ab?style=flat-square&logo=discord)](https://discord.gg/Gv4t6Xhy7E)
[![Telegram](https://img.shields.io/badge/Telegram-1D77A3?style=flat-square&logo=telegram)](https://t.me/homaexe)

---

## What is HomRec?

If you've ever tried recording your screen with OBS or Bandicam on an old laptop or office PC and everything lagged — **HomRec is made for you.**

Written from the ground up to use the minimum possible CPU and RAM. No fancy effects, no GPU requirement, no background services. Just open, record, done.

---

## Features

| Feature | Details |
|---|---|
| **Screen recording** | Full desktop or specific window |
| **Audio capture** | Microphone + Desktop audio (via WASAPI loopback) |
| **Multi-monitor** | Select which monitor to record |
| **PC Analytics** | CPU, RAM and disk in one combined window |
| **Hotkeys** | F9 start/stop · F10 pause · F11 fullscreen |
| **Custom languages** | Drop a `.hrl` file in Advanced Settings to add any language instantly |
| **Themes** | Dark and Light built-in; plugin system coming soon |
| **Custom output folder** | Choose where recordings are saved |
| **Recording stats** | Live FPS, duration, frame count in status bar |
| **Console window** | Access the most unusual settings directly via the built-in console |
| **Always on top** | Keep the window above everything else |
| **System tray** | Minimise to tray, control recording from tray menu |
| **Auto update check** | Notifies you when a new version is out |
| **Help menu** | Check for updates and report issues directly from the app |

---

## Installation

### Option A — .exe (recommended)

**1.** Go to [**Releases**](https://github.com/homaaio/homrec/releases) and download the latest `.zip`  
**2.** Unzip anywhere you want — no installer needed  
**3.** Launch `hr.exe`

> FFmpeg is already included in the archive — no extra downloads needed.  
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

## Dependencies

```
Pillow
mss
psutil           # optional — PC Analytics panel
pystray          # optional — system tray support
```

Install all at once:
```bash
pip install -r requirements.txt
```

> Audio capture (microphone + desktop) is handled natively via built-in WASAPI C++ libraries — no extra audio packages required.

---

## Stay updated

### **[t.me/homaexe](https://t.me/homaexe)** / **[x.com/homrec_dev](https://x.com/homrec_dev)**

---

<div align="center">
Made with ❤️ by <b>homaaio</b>
</div>
