from __future__ import annotations
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import time
import os
from datetime import datetime
import cv2
import numpy as np
from PIL import Image, ImageTk, ImageDraw
from tkinter import colorchooser as cc
import mss
import threading
import json
import gzip
try:
    from tkinterdnd2 import DND_FILES, TkinterDnD
    _DND_AVAILABLE = True
except ImportError:
    _DND_AVAILABLE = False
import ctypes
import sys
import subprocess
import re
import glob
import warnings
warnings.filterwarnings("ignore", category=DeprecationWarning)
try:
    import pyaudio as _pyaudio_mod
    import audioop as _audioop_mod
    _PYAUDIO_AVAILABLE = True
except ImportError:
    _pyaudio_mod = None
    _audioop_mod = None
    _PYAUDIO_AVAILABLE = False
try:
    import wave
except ImportError:
    wave = None
import shutil
import platform
import webbrowser
import logging
import queue

def setup_logging() -> None:
    if getattr(sys, 'frozen', False):
        log_dir = os.path.dirname(sys.executable)
    else:
        _src = os.path.dirname(os.path.abspath(__file__))
        _parent = os.path.dirname(_src)
        log_dir = _parent if (os.path.isdir(os.path.join(_parent, "src")) or os.path.basename(_src).lower() == "src") else _src
    logging.basicConfig(
        level=logging.DEBUG,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
        handlers=[logging.FileHandler(os.path.join(log_dir, "homrec.log"), encoding="utf-8")]
    )

setup_logging()
log = logging.getLogger("homrec")

try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    HAS_PSUTIL = False

try:
    import pystray
    from pystray import MenuItem as TrayItem
    HAS_TRAY = True
except ImportError:
    HAS_TRAY = False

CURRENT_VERSION = "1.7.0"
GITHUB_REPO = "homaaio/homrec"

def check_for_updates(callback) -> None:
    def _fetch():
        try:
            import urllib.request, json as _json
            url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
            req = urllib.request.Request(url, headers={"User-Agent": "HomRec"})
            with urllib.request.urlopen(req, timeout=5) as resp:
                data = _json.loads(resp.read().decode())
            tag = data.get("tag_name", "").lstrip("v")
            if tag and _version_gt(tag, CURRENT_VERSION):
                callback(tag)
        except Exception as e:
            log.warning(f"Update check failed: {e}")
    threading.Thread(target=_fetch, daemon=True).start()

def _version_gt(a: str, b: str) -> bool:
    try:
        return tuple(int(x) for x in a.split(".")) > tuple(int(x) for x in b.split("."))
    except:
        return False

LANGUAGES = {
    "en": {
        "app_title": "HomRec v1.7.0", "live_preview": "PREVIEW", "ready": "Ready",
        "recording": "Recording", "paused": "Paused", "fps": "FPS:", "resolution": "Resolution:",
        "start": "▶ START", "pause": "⏸ PAUSE", "stop": "■ STOP", "resume": "▶ RESUME",
        "recording_btn": "⏺ RECORDING", "audio_mixer": "Audio Mixer", "microphone": "Microphone",
        "desktop_audio": "Desktop Audio", "mute": "Mute", "unmute": "Unmute", "vol": "Vol:",
        "level": "Level:", "enable_audio": "Enable Audio", "ffmpeg_found": "FFmpeg: ✅ Found",
        "ffmpeg_not_found": "FFmpeg: ❌ Not Found", "file_menu": "File",
        "open_recordings": "Open Recordings Folder", "exit": "Exit", "view_menu": "View",
        "always_on_top": "Always on Top", "fullscreen": "Fullscreen  F11",
        "pc_analytics": "PC Analytics", "cpu_info": "CPU Info", "ram_info": "RAM Info",
        "disk_info": "Disk Info", "help_menu": "Help", "check_updates": "Check for Updates",
        "report_issue": "Report Issue", "capture_source": "Capture Source",
        "full_desktop": "Full Desktop", "select_window": "Select Window...",
        "minimize_tray": "Minimize to tray on close", "language": "Language",
        "english": "English", "russian": "Русский", "theme": "Theme", "dark": "Dark",
        "light": "Light", "settings_menu": "Settings", "preferences": "Preferences...",
        "performance_menu": "Performance", "ultra": "Ultra (60 FPS)", "turbo": "Turbo (30 FPS)",
        "balanced": "Balanced (15 FPS)", "eco": "Eco (8 FPS)", "stats": "STATS",
        "time": "TIME", "status": "STATUS", "warning": "Warning", "error": "Error",
        "info": "Info", "folder_not_exist": "Folder doesn't exist!",
        "recording_failed": "Recording failed!", "settings_saved": "Settings saved!",
        "recording_saved": "✅ Recording Saved!", "open_folder": "Open folder?",
        "ffmpeg_not_found_msg": "⚠️ FFmpeg not found - audio separate",
        "saved": "✅ Saved: {size:.1f} MB | {duration:.1f}s",
        "recording_status": "Recording: {size:.1f} MB | {frames} frames",
        "file": "📁 File:", "size": "📊 Size:", "duration": "⏱️ Duration:",
        "audio": "🎤 Audio:", "merged": "Merged", "separate": "Separate", "no_audio": "No",
        "save": "Save", "cancel": "Cancel", "browse": "Browse",
        "output_folder": "Output folder:", "settings_title": "Settings",
        "video_settings": "Video", "quality": "Quality:", "mode": "Mode:",
        "advanced": "Advanced", "monitor": "Monitor:", "output": "Output:",
        "countdown": "Countdown (3s)", "timestamp": "Timestamp", "cursor": "Cursor",
        "notification": "Show summary", "made_by": "Homa4ella", "audio_file": "🎵 Audio file:",
        "show_log": "Show Log",
    }
}

def find_ffmpeg() -> str | None:
    app_dir = os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) else os.path.dirname(os.path.abspath(__file__))
    for name in ('ffmpeg.exe', 'ffmpeg'):
        c = os.path.join(app_dir, name)
        if os.path.exists(c):
            return c
        if os.path.exists(name):
            return os.path.abspath(name)
    return shutil.which("ffmpeg")

def optimize_for_performance() -> None:
    try:
        import psutil, platform as _plat
        p = psutil.Process()
        p.nice(psutil.HIGH_PRIORITY_CLASS if _plat.system() == "Windows" else -10)
    except Exception:
        pass
    if sys.platform == 'win32':
        try: ctypes.windll.winmm.timeBeginPeriod(1)
        except Exception: pass
        try:
            _io = ctypes.c_ulong(3)
            ctypes.windll.ntdll.NtSetInformationProcess(
                ctypes.windll.kernel32.GetCurrentProcess(), 33,
                ctypes.byref(_io), ctypes.sizeof(_io))
        except Exception: pass
    try:
        cv2.setNumThreads(0)
        cv2.setUseOptimized(True)
    except Exception:
        pass
    import gc
    gc.set_threshold(50000, 200, 200)
    try:
        sys.setswitchinterval(0.005)
    except Exception:
        pass
    try:
        from homrec_native import NATIVE_OK, RINGBUF_OK
        log.info(f"Native extensions: core={NATIVE_OK} ringbuf={RINGBUF_OK}")
    except Exception as _e:
        log.warning(f"Native ext not loaded at startup: {_e}")


def rms_to_level_percent(raw_rms: float, floor_db: float = -55.0) -> int:
    if raw_rms <= 0:
        return 0
    import math as _math
    db = 20.0 * _math.log10(min(raw_rms, 32767) / 32767.0)
    db = max(floor_db, min(0.0, db))
    pct = (db - floor_db) / (0.0 - floor_db) * 100.0
    return max(0, min(100, int(round(pct))))
