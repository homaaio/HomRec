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


class AudioLevelMeter(tk.Canvas):
    def __init__(self, parent, width: int = 180, height: int = 20, dynamics: int = 5, **kwargs) -> None:
        super().__init__(parent, width=width, height=height, highlightthickness=0, **kwargs)
        self.meter_width = width
        self.meter_height = height
        self.level = 0.0          # smoothed display level
        self._raw_level = 0       # latest raw level from audio thread
        self._peak = 0.0
        self._peak_decay = 0
        self._bar_id = None
        self._peak_id = None
        self.dynamics = max(0, min(10, dynamics))  # 0=off, 1=slow…10=instant
        self.enabled = True
        self._init_canvas()

    def _lerp_color(self, t: float) -> str:
        if t < 0.7:
            s = t / 0.7
            r = int(166 + (249 - 166) * s); g = int(227 + (226 - 227) * s); b = int(161 + (175 - 161) * s)
        else:
            s = (t - 0.7) / 0.3
            r = int(249 + (243 - 249) * s); g = int(226 + (56 - 226) * s); b = int(175 + (168 - 175) * s)
        return f'#{r:02x}{g:02x}{b:02x}'

    def _init_canvas(self) -> None:
        self.delete("all")
        self.create_rectangle(0, 0, self.meter_width, self.meter_height, fill='#1e1e2e', outline='#45475a', width=1)
        self._bar_id = self.create_rectangle(2, 2, 2, self.meter_height - 2, fill='#a6e3a1', outline='')
        self._peak_id = self.create_line(2, 2, 2, self.meter_height - 2, fill='#a6e3a1', width=2, state='hidden')

    def draw_meter(self) -> None:
        if not self.enabled:
            self.coords(self._bar_id, 2, 2, 2, self.meter_height - 2)
            self.itemconfig(self._peak_id, state='hidden')
            return
        inner_w = self.meter_width - 4
        bar_x1 = 2 + max(0, int(self.level / 100 * inner_w))
        self.coords(self._bar_id, 2, 2, bar_x1, self.meter_height - 2)
        self.itemconfig(self._bar_id, fill=self._lerp_color(self.level / 100))
        if self._peak > 1:
            px = 2 + int(self._peak / 100 * inner_w)
            pcol = '#f38ba8' if self._peak > 90 else '#f9e2af' if self._peak > 70 else '#a6e3a1'
            self.coords(self._peak_id, px, 2, px, self.meter_height - 2)
            self.itemconfig(self._peak_id, fill=pcol, state='normal')
        else:
            self.itemconfig(self._peak_id, state='hidden')

    def set_level(self, level: int) -> None:
        if not self.enabled:
            return
        self._raw_level = max(0, min(100, level))

        if self.dynamics == 0:
            self.level = float(self._raw_level)
        else:
            alpha = self.dynamics / 10.0
            self.level = alpha * self._raw_level + (1.0 - alpha) * self.level

        decay_speed = max(1, 4 - self.dynamics // 3)  # 1..4 levels per tick
        if self.level > self._peak:
            self._peak = self.level
            self._peak_decay = max(5, 25 - self.dynamics * 2)  # hold frames
        else:
            if self._peak_decay > 0:
                self._peak_decay -= 1
            else:
                self._peak = max(0.0, self._peak - decay_speed)
        self.draw_meter()


class CustomMessageBox:
    @staticmethod
    def show(app, title_key: str, message_key: str, info_text: str, dont_show_var: tk.BooleanVar) -> bool:
        c = app.colors
        dialog = tk.Toplevel()
        dialog.title(app.lang[title_key])
        dialog.geometry("520x420")
        dialog.configure(bg=c["bg"])
        dialog.transient(); dialog.grab_set(); dialog.resizable(False, False)
        dialog.update_idletasks()
        dialog.geometry(f"+{dialog.winfo_screenwidth()//2-260}+{dialog.winfo_screenheight()//2-210}")

        tk.Frame(dialog, bg=c.get("success", "#a6e3a1"), height=6).pack(fill="x")
        top_row = tk.Frame(dialog, bg=c["bg"])
        top_row.pack(fill="x", padx=24, pady=(20, 6))
        tk.Label(top_row, text="✅", font=("Segoe UI", 36), bg=c["bg"], fg=c.get("success", "#a6e3a1")).pack(side="left", padx=(0, 16))
        title_col = tk.Frame(top_row, bg=c["bg"])
        title_col.pack(side="left", fill="y")
        tk.Label(title_col, text=app.lang[message_key], font=("Segoe UI", 14, "bold"), bg=c["bg"], fg=c["text"]).pack(anchor="w")
        tk.Label(title_col, text="Recording complete", font=("Segoe UI", 9), bg=c["bg"], fg=c.get("text_secondary", "#a6adc8")).pack(anchor="w")

        info_frame = tk.Frame(dialog, bg=c.get("surface", "#313244"), relief="flat", padx=16, pady=12)
        info_frame.pack(pady=8, padx=20, fill="both", expand=True)
        tk.Label(info_frame, text=info_text, justify="left", bg=c.get("surface", "#313244"), fg=c["text"], font=("Consolas", 10), anchor="w").pack(anchor="w")

        check_frame = tk.Frame(dialog, bg=c["bg"])
        check_frame.pack(pady=8)
        dont_show_text = "Don't show again"
        tk.Checkbutton(check_frame, text=dont_show_text, variable=dont_show_var,
                       bg=app.colors["bg"], fg=app.colors["fg"], selectcolor=app.colors["surface"],
                       font=("Segoe UI", 9)).pack()

        btn_frame = tk.Frame(dialog, bg=app.colors["bg"])
        btn_frame.pack(pady=15)
        result = {'value': False}

        def on_yes():
            result['value'] = True; dialog.destroy()
        def on_no():
            result['value'] = False; dialog.destroy()

        tk.Button(btn_frame, text=app.lang["open_folder"], command=on_yes,
                  bg='#a6e3a1', fg=app.colors["bg"], font=("Segoe UI", 10, "bold"),
                  relief='flat', padx=20, pady=8, width=12).pack(side='left', padx=5)
        tk.Button(btn_frame, text=app.lang["cancel"], command=on_no,
                  bg=app.colors["surface"], fg=app.colors["text"], font=("Segoe UI", 10),
                  relief='flat', padx=20, pady=8, width=12).pack(side='left', padx=5)
        dialog.wait_window()
        return result['value']


class WelcomeDialog:
    @staticmethod
    def show(app) -> None:
        W, H = 580, 440
        BG = "#0f0f17"; CARD = "#1a1a2e"; ACCENT = "#89b4fa"; ACCENT2 = "#cba6f7"
        GREEN = "#a6e3a1"; GOLD = "#f9e2af"; TEXT = "#cdd6f4"; SUB = "#a6adc8"; DIM = "#45475a"

        dlg = tk.Toplevel()
        dlg.withdraw()
        dlg.title("Welcome to HomRec")
        dlg.geometry(f"{W}x{H}")
        dlg.resizable(False, False)
        dlg.configure(bg=BG)
        try:
            base_dir = os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) else os.path.dirname(os.path.abspath(__file__))
            ico_path = os.path.join(base_dir, "icons", "main.ico")
            if os.path.exists(ico_path):
                dlg.iconbitmap(ico_path)
        except Exception:
            pass
        dlg.update_idletasks()
        dlg.geometry(f"{W}x{H}+{(dlg.winfo_screenwidth()-W)//2}+{(dlg.winfo_screenheight()-H)//2}")

        hdr_canvas = tk.Canvas(dlg, width=W, height=110, bg=CARD, highlightthickness=0)
        hdr_canvas.pack(fill="x")
        for xi in range(0, W, 30):
            hdr_canvas.create_line(xi, 0, xi, 110, fill="#1e1e30", width=1)
        for yi in range(0, 110, 30):
            hdr_canvas.create_line(0, yi, W, yi, fill="#1e1e30", width=1)
        hdr_canvas.create_oval(18, 18, 92, 92, fill="#181830", outline=ACCENT, width=2)
        hdr_canvas.create_oval(28, 28, 82, 82, fill="#0f0f20", outline=ACCENT2, width=1)
        hdr_canvas.create_oval(43, 43, 67, 67, fill="#f38ba8", outline="")
        hdr_canvas.create_text(110, 38, text="HomRec", anchor="w", font=("Segoe UI", 28, "bold"), fill=ACCENT)
        hdr_canvas.create_text(110, 68, text=f"Screen Recorder  v{CURRENT_VERSION}", anchor="w", font=("Segoe UI", 11), fill=SUB)
        hdr_canvas.create_text(110, 88, text="by homaaio", anchor="w", font=("Segoe UI", 9), fill=DIM)

        _pulse_state = [True]
        def _pulse():
            if not dlg.winfo_exists(): return
            hdr_canvas.itemconfig(3, fill="#f38ba8" if _pulse_state[0] else "#a0203a")
            _pulse_state[0] = not _pulse_state[0]
            dlg.after(600, _pulse)
        dlg.after(300, _pulse)

        tk.Frame(dlg, bg=ACCENT, height=2).pack(fill="x")

        pills_frame = tk.Frame(dlg, bg=BG)
        pills_frame.pack(fill="x", padx=24, pady=(14, 6))
        for icon, label, color in [("⚡", "Native C/C++ core", ACCENT), ("🎙", "Audio mixer", ACCENT2), ("🖥", "Multi-monitor", GREEN), ("🎨", "Themes & langs", GOLD)]:
            pill = tk.Frame(pills_frame, bg="#1e1e2e", padx=10, pady=5)
            pill.pack(side="left", padx=(0, 8))
            tk.Label(pill, text=f"{icon} {label}", bg="#1e1e2e", fg=color, font=("Segoe UI", 9, "bold")).pack()

        tk.Frame(dlg, bg=DIM, height=1).pack(fill="x", padx=24, pady=(6, 0))
        body = tk.Frame(dlg, bg=BG)
        body.pack(fill="both", expand=True, padx=28, pady=14)
        tk.Label(body, text="Hello,", font=("Segoe UI", 14, "bold"), bg=BG, fg=TEXT).pack(anchor="w")
        msg = "Welcome to HomRec! If you have any issues, reach out on GitHub.\n\nEnjoy.   homaaio"
        tk.Label(body, text=msg, font=("Segoe UI", 10), bg=BG, fg=SUB, justify="left").pack(anchor="w", pady=(6, 0))

        tips_frame = tk.Frame(dlg, bg=CARD, pady=8)
        tips_frame.pack(fill="x", padx=24, pady=(6, 0))
        tk.Label(tips_frame, text="Quick tips:", bg=CARD, fg=ACCENT, font=("Segoe UI", 9, "bold")).pack(anchor="w", padx=12)
        tk.Label(tips_frame, text="F9 = Start/Stop   F10 = Pause   F11 = Fullscreen",
                 bg=CARD, fg=SUB, font=("Segoe UI", 9)).pack(anchor="w", padx=12)

        btn_row = tk.Frame(dlg, bg=BG)
        btn_row.pack(fill="x", padx=24, pady=14)

        def _lighten(hx):
            try:
                r = min(255, int(hx[1:3],16)+20); g = min(255, int(hx[3:5],16)+20); b = min(255, int(hx[5:7],16)+20)
                return f"#{r:02x}{g:02x}{b:02x}"
            except: return hx

        def _btn(parent, text, cmd, bg, fg, bold=False, side="left", padx=(0,8)):
            b = tk.Button(parent, text=text, command=cmd, bg=bg, fg=fg,
                          font=("Segoe UI", 9, "bold") if bold else ("Segoe UI", 9),
                          relief="flat", padx=12, pady=8, cursor="hand2", bd=0,
                          activebackground=bg, activeforeground=fg)
            b.pack(side=side, padx=padx)
            b.bind("<Enter>", lambda e: b.config(bg=_lighten(bg)))
            b.bind("<Leave>", lambda e: b.config(bg=bg))
            return b

        _btn(btn_row, "📋 Changelog", lambda: webbrowser.open(f"https://github.com/homaaio/HomREC/blob/main/CHANGELOG.txt"), "#313244", TEXT)
        _btn(btn_row, "⭐ GitHub", lambda: webbrowser.open("https://github.com/homaaio/HomREC"), "#313244", GOLD)
        _btn(btn_row, "🌐 Website", lambda: webbrowser.open("https://homaaio.github.io/HomREC/"), "#313244", ACCENT)
        _btn(btn_row, "Get Started →", dlg.destroy, ACCENT, "#1e1e2e", bold=True, side="right", padx=(8, 0))

        dlg.transient(); dlg.grab_set(); dlg.deiconify(); dlg.focus_force(); dlg.wait_window()


class AudioPanel:
    def __init__(self, parent, app) -> None:
        self.app = app
        c = app.colors
        title_bar = tk.Frame(parent, bg=c["surface"])
        self._title_lbl = tk.Label(title_bar, text=app.lang["audio_mixer"], bg=c["surface"], fg=c["accent"],
                 font=("Segoe UI", 11, "bold"))
        self._title_lbl.pack(side="left")
        tk.Button(title_bar, text="✕", command=self._close_panel,
                  bg=c["surface"], fg=c["text_secondary"], font=("Segoe UI", 9),
                  relief="flat", width=3, cursor="hand2").pack(side="right")
        self.frame = tk.LabelFrame(parent, labelwidget=title_bar,
                                   bg=c["surface"], fg=c["accent"],
                                   font=("Segoe UI", 11, "bold"), padx=10, pady=10)
        self.frame.pack(fill='both', expand=True, padx=5, pady=5)
        self.audio_enabled = tk.BooleanVar(value=True)
        self.mic_mute = tk.BooleanVar(value=False)
        self.sys_mute = tk.BooleanVar(value=False)
        self.audio_stream = None
        self.audio_p = None
        self._mic_level_pending: int = 0
        self._sys_level_pending: int = 0
        self._mic_vol_cached: float = 0.80
        self._sys_vol_cached: float = 1.0
        self._mic_mute_cached: bool = False
        self._sys_mute_cached: bool = False
        self.create_mixer_layout()

    def create_mic_section(self) -> None: pass
    def create_system_section(self) -> None: pass
    def create_devices_section(self) -> None: pass

    def create_mixer_layout(self) -> None:
        c = self.app.colors
        channels = tk.Frame(self.frame, bg=c["surface"])
        channels.pack(fill='x', pady=(0, 4))

        mic_strip = tk.Frame(channels, bg=c["surface"], relief='flat', bd=0)
        mic_strip.pack(side='left', fill='both', expand=True, padx=(0, 8))
        mic_header = tk.Frame(mic_strip, bg=c["surface"])
        mic_header.pack(fill='x')
        tk.Label(mic_header, text=self.app.lang["microphone"], bg=c["surface"], fg='#a6e3a1', font=("Segoe UI", 9, 'bold')).pack(side='left')
        self.mic_mute_btn = tk.Button(mic_header, text=self.app.lang["mute"], command=self.toggle_mic_mute,
                                      bg=c["surface_light"], fg=c["text"], font=("Segoe UI", 8), relief='flat', width=5, cursor='hand2')
        self.mic_mute_btn.pack(side='right')
        mic_vol_row = tk.Frame(mic_strip, bg=c["surface"])
        mic_vol_row.pack(fill='x', pady=2)
        tk.Label(mic_vol_row, text=self.app.lang["vol"], bg=c["surface"], fg=c["text"], font=("Segoe UI", 8)).pack(side='left')
        self.mic_volume = tk.Scale(mic_vol_row, from_=0, to=100, orient='horizontal', length=110,
                                   bg=c["surface_light"], fg=c["text"], highlightthickness=0,
                                   troughcolor=c["surface"], command=self.on_mic_volume_change, showvalue=False)
        self.mic_volume.set(80)
        self.mic_volume.pack(side='left', padx=4)
        self.mic_volume_label = tk.Label(mic_vol_row, text="80%", bg=c["surface"], fg='#a6e3a1', font=("Segoe UI", 8, 'bold'), width=4)
        self.mic_volume_label.pack(side='left')
        mic_meter_row = tk.Frame(mic_strip, bg=c["surface"])
        mic_meter_row.pack(fill='x', pady=2)
        tk.Label(mic_meter_row, text=self.app.lang["level"], bg=c["surface"], fg=c["text"], font=("Segoe UI", 8)).pack(side='left')
        self.mic_meter = AudioLevelMeter(mic_meter_row, width=130, height=14, bg=c["surface"])
        self.mic_meter.pack(side='left', padx=4)

        tk.Frame(channels, bg=c["surface_light"], width=1).pack(side='left', fill='y', padx=4)

        sys_strip = tk.Frame(channels, bg=c["surface"])
        sys_strip.pack(side='left', fill='both', expand=True, padx=(8, 0))
        sys_header = tk.Frame(sys_strip, bg=c["surface"])
        sys_header.pack(fill='x')
        tk.Label(sys_header, text=self.app.lang["desktop_audio"], bg=c["surface"], fg='#89b4fa', font=("Segoe UI", 9, 'bold')).pack(side='left')
        self.sys_mute_btn = tk.Button(sys_header, text=self.app.lang["mute"], command=self.toggle_sys_mute,
                                      bg=c["surface_light"], fg=c["text"], font=("Segoe UI", 8), relief='flat', width=5, cursor='hand2')
        self.sys_mute_btn.pack(side='right')
        sys_vol_row = tk.Frame(sys_strip, bg=c["surface"])
        sys_vol_row.pack(fill='x', pady=2)
        tk.Label(sys_vol_row, text=self.app.lang["vol"], bg=c["surface"], fg=c["text"], font=("Segoe UI", 8)).pack(side='left')
        self.sys_volume = tk.Scale(sys_vol_row, from_=0, to=100, orient='horizontal', length=110,
                                   bg=c["surface_light"], fg=c["text"], highlightthickness=0,
                                   troughcolor=c["surface"], command=self.on_sys_volume_change, showvalue=False)
        self.sys_volume.set(100)
        self.sys_volume.pack(side='left', padx=4)
        self.sys_volume_label = tk.Label(sys_vol_row, text="100%", bg=c["surface"], fg='#89b4fa', font=("Segoe UI", 8, 'bold'), width=4)
        self.sys_volume_label.pack(side='left')
        sys_meter_row = tk.Frame(sys_strip, bg=c["surface"])
        sys_meter_row.pack(fill='x', pady=2)
        tk.Label(sys_meter_row, text=self.app.lang["level"], bg=c["surface"], fg=c["text"], font=("Segoe UI", 8)).pack(side='left')
        self.sys_meter = AudioLevelMeter(sys_meter_row, width=130, height=14, bg=c["surface"])
        self.sys_meter.pack(side='left', padx=4)

        bottom = tk.Frame(self.frame, bg=c["surface"])
        bottom.pack(fill='x', pady=(4, 0))
        self.audio_check = tk.Checkbutton(bottom, text=self.app.lang["enable_audio"],
                                          variable=self.audio_enabled, bg=c["surface"], fg=c["text"],
                                          selectcolor=c["surface_light"], font=("Segoe UI", 9, 'bold'))
        self.audio_check.pack(side='left')
        ffmpeg_ok = self.app.check_ffmpeg()
        self.ffmpeg_label = tk.Label(bottom, text=self.app.lang["ffmpeg_found" if ffmpeg_ok else "ffmpeg_not_found"],
                                      bg=c["surface"], fg='#a6e3a1' if ffmpeg_ok else '#f38ba8', font=("Segoe UI", 8))
        self.ffmpeg_label.pack(side='right')

        dyn_row = tk.Frame(self.frame, bg=c["surface"])
        dyn_row.pack(fill='x', pady=(2, 0))
        self._meter_enabled_var = tk.BooleanVar(value=True)
        tk.Checkbutton(dyn_row, text="VU meter",
                       variable=self._meter_enabled_var,
                       command=self._on_meter_toggle,
                       bg=c["surface"], fg=c["text"],
                       selectcolor=c["surface_light"],
                       font=("Segoe UI", 8)).pack(side='left')
        tk.Label(dyn_row, text="dynamics:", bg=c["surface"], fg=c["text_secondary"],
                 font=("Segoe UI", 7)).pack(side='left', padx=(6, 2))
        self._dynamics_var = tk.IntVar(value=5)
        dyn_scale = tk.Scale(dyn_row, from_=0, to=10, orient='horizontal', length=80,
                             variable=self._dynamics_var, showvalue=False,
                             bg=c["surface_light"], fg=c["text"],
                             highlightthickness=0, troughcolor=c["surface"],
                             command=self._on_dynamics_change)
        dyn_scale.pack(side='left')
        self._dyn_val_lbl = tk.Label(dyn_row, text="5", width=2,
                                      bg=c["surface"], fg=c["accent"],
                                      font=("Segoe UI", 7, "bold"))
        self._dyn_val_lbl.pack(side='left')

        self.app.root.after(100, self._poll_audio_levels)
        self.app.root.after(200, self._start_idle_monitor)

    def _on_meter_toggle(self) -> None:
        enabled = self._meter_enabled_var.get()
        for meter in (self.mic_meter, self.sys_meter):
            meter.enabled = enabled
            if not enabled:
                meter.level = 0.0; meter._peak = 0.0
                meter.draw_meter()

    def _on_dynamics_change(self, value=None) -> None:
        d = self._dynamics_var.get()
        self._dyn_val_lbl.config(text=str(d))
        for meter in (self.mic_meter, self.sys_meter):
            meter.dynamics = d

    def on_mic_volume_change(self, value: str) -> None:
        self.mic_volume_label.config(text=f"{int(float(value))}%")
        self.app.save_settings_debounced()

    def on_sys_volume_change(self, value: str) -> None:
        self.sys_volume_label.config(text=f"{int(float(value))}%")
        self.app.save_settings_debounced()

    def toggle_mic_mute(self) -> None:
        self.mic_mute.set(not self.mic_mute.get())
        self.mic_mute_btn.config(bg='#f38ba8' if self.mic_mute.get() else self.app.colors["surface_light"],
                                 text=self.app.lang["unmute" if self.mic_mute.get() else "mute"])

    def toggle_sys_mute(self) -> None:
        self.sys_mute.set(not self.sys_mute.get())
        self.sys_mute_btn.config(bg='#f38ba8' if self.sys_mute.get() else self.app.colors["surface_light"],
                                 text=self.app.lang["unmute" if self.sys_mute.get() else "mute"])

    def update_mic_level(self, level: int) -> None:
        self._mic_level_pending = level

    def update_sys_level(self, level: int) -> None:
        self._sys_level_pending = level

    def _poll_audio_levels(self) -> None:
        try:
            self.mic_meter.set_level(self._mic_level_pending)
            self.sys_meter.set_level(self._sys_level_pending)
            self._mic_vol_cached = self.mic_volume.get() / 100.0
            self._sys_vol_cached = self.sys_volume.get() / 100.0
            self._mic_mute_cached = self.mic_mute.get()
            self._sys_mute_cached = self.sys_mute.get()
            if getattr(self.app, '_using_cpp_audio', False):
                try:
                    from homrec_native import audio_engine as _ae
                    if _ae:
                        _ae.set_volumes(self._mic_vol_cached, self._sys_vol_cached, self._mic_mute_cached, self._sys_mute_cached)
                except Exception:
                    pass
        except Exception:
            pass
        try:
            self.app.root.after(100, self._poll_audio_levels)
        except Exception:
            pass

    def _start_idle_monitor(self) -> None:
        self._idle_monitor_active = True
        self._idle_monitor_thread = threading.Thread(target=self._idle_monitor_worker, daemon=True)
        self._idle_monitor_thread.start()

    def stop_idle_monitor(self) -> None:
        self._idle_monitor_active = False

    def resume_idle_monitor(self) -> None:
        self._idle_monitor_active = True
        if not getattr(self, '_idle_monitor_thread', None) or not self._idle_monitor_thread.is_alive():
            self._idle_monitor_thread = threading.Thread(target=self._idle_monitor_worker, daemon=True)
            self._idle_monitor_thread.start()

    def _idle_monitor_worker(self) -> None:
        import time as _t
        try:
            from homrec_native import audio_engine as _ae, AUDIO_OK as _AOK
        except Exception:
            _ae = None; _AOK = False

        if _AOK and _ae is not None:
            flags = _ae.start(1.0, 1.0, False, False)
            if bool(flags & 0x1):
                try:
                    while self._idle_monitor_active:
                        if getattr(self.app, 'audio_recording', False):
                            _ae.stop(None, None)
                            while getattr(self.app, 'audio_recording', False) and self._idle_monitor_active:
                                _t.sleep(0.1)
                            if not self._idle_monitor_active: return
                            _ae.start(1.0, 1.0, False, False)
                            continue
                        m, _ = _ae.get_levels()
                        self._mic_level_pending = m
                        _t.sleep(0.05)
                finally:
                    try: _ae.stop(None, None)
                    except Exception: pass
                return

        if not _PYAUDIO_AVAILABLE: return
        p = stream = None
        try:
            p = _pyaudio_mod.PyAudio()
            dev_info = p.get_default_input_device_info()
            ch = min(2, max(1, int(dev_info.get('maxInputChannels', 1))))
            stream = p.open(format=_pyaudio_mod.paInt16, channels=ch, rate=44100,
                            input=True, input_device_index=dev_info.get('index', 0), frames_per_buffer=1024)
            while self._idle_monitor_active:
                if getattr(self.app, 'audio_recording', False):
                    try: stream.read(1024, exception_on_overflow=False)
                    except Exception: pass
                    _t.sleep(0.05); continue
                try:
                    data = stream.read(1024, exception_on_overflow=False)
                    raw_rms = _audioop_mod.rms(data, 2)
                    self._mic_level_pending = rms_to_level_percent(raw_rms)
                except Exception:
                    _t.sleep(0.05)
        except Exception as e:
            log.debug(f'idle mic monitor (PyAudio) failed: {e}')
        finally:
            try:
                if stream: stream.stop_stream(); stream.close()
            except Exception: pass
            try:
                if p: p.terminate()
            except Exception: pass

    def update_language(self) -> None:
        self._title_lbl.config(text=self.app.lang["audio_mixer"])
        ffmpeg_ok = self.app.check_ffmpeg()
        self.ffmpeg_label.config(text=self.app.lang["ffmpeg_found" if ffmpeg_ok else "ffmpeg_not_found"])
        self.audio_check.config(text=self.app.lang["enable_audio"])
        self.mic_mute_btn.config(text=self.app.lang["unmute" if self.mic_mute.get() else "mute"])
        self.sys_mute_btn.config(text=self.app.lang["unmute" if self.sys_mute.get() else "mute"])

    def _close_panel(self) -> None:
        self.app.show_audio_panel = False
        self.app.save_settings(silent=True)
        self.app.recreate_widgets()


class OverlaysDockPanel:
    def __init__(self, parent, app) -> None:
        self.app = app
        c = app.colors
        self.frame = tk.LabelFrame(parent, text="🎭 Overlays",
                                    bg=c["surface"], fg=c["accent"],
                                    font=("Segoe UI", 11, "bold"), padx=8, pady=8)
        self.frame.pack(fill="both", expand=True, padx=5, pady=5)

        header = tk.Frame(self.frame, bg=c["surface"])
        header.pack(fill="x", pady=(0, 6))
        tk.Button(header, text="＋", command=self._quick_add,
                  bg=c["success"], fg=c["bg"], font=("Segoe UI", 9, "bold"),
                  relief="flat", width=3, cursor="hand2").pack(side="left")
        tk.Button(header, text="👁 Position on Preview", command=self._open_drag_preview,
                  bg=c.get("surface_light", "#45475a"), fg=c["accent"],
                  font=("Segoe UI", 8), relief="flat", padx=8, cursor="hand2").pack(side="left", padx=(6, 0))
        tk.Button(header, text="✕", command=self._close_panel,
                  bg=c["surface"], fg=c["text_secondary"], font=("Segoe UI", 9),
                  relief="flat", width=3, cursor="hand2").pack(side="right")

        list_outer = tk.Frame(self.frame, bg=c["surface"])
        list_outer.pack(fill="both", expand=True)
        canvas = tk.Canvas(list_outer, bg=c["surface"], highlightthickness=0)
        scroll = ttk.Scrollbar(list_outer, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)
        self._list_inner = tk.Frame(canvas, bg=c["surface"])
        canvas.create_window((0, 0), window=self._list_inner, anchor="nw")
        self._list_inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        self._canvas = canvas

        self._menu_widgets: dict = {}
        self.refresh()

    def refresh(self) -> None:
        c = self.app.colors
        for w in self._list_inner.winfo_children():
            w.destroy()
        overlays = getattr(self.app, "overlays", [])
        if not overlays:
            tk.Label(self._list_inner, text="No overlays yet.\nClick ＋ to add one.",
                     bg=c["surface"], fg=c["text_secondary"], font=("Segoe UI", 8, "italic"),
                     justify="center").pack(padx=8, pady=12)
            return
        for i, ov in enumerate(overlays):
            row = tk.Frame(self._list_inner, bg=c.get("surface_light", "#45475a") if i % 2 else c["surface"])
            row.pack(fill="x", padx=2, pady=1)
            icon = {"text": "📝", "webcam": "📷", "image": "🖼"}.get(ov.get("kind", "text"), "?")
            if ov.get("kind") == "text":
                name = ov.get("text", "")[:14] or "(empty text)"
            elif ov.get("kind") == "image":
                name = os.path.basename(ov.get("path", ""))[:14] or "(no file)"
            else:
                name = f"Cam#{ov.get('cam_index', 0)}"
            dot = "●" if ov.get("enabled", True) else "○"
            tk.Label(row, text=f"{dot} {icon} {name}", bg=row["bg"], fg=c["text"],
                     font=("Segoe UI", 9), anchor="w").pack(side="left", fill="x", expand=True, padx=6, pady=4)
            dots_btn = tk.Button(row, text="⋮", bg=row["bg"], fg=c["text_secondary"],
                                  font=("Segoe UI", 10, "bold"), relief="flat", width=2,
                                  cursor="hand2", command=lambda idx=i: self._open_item_menu(idx))
            dots_btn.pack(side="right", padx=2)

    def _quick_add(self) -> None:
        new_ov = {"kind": "text", "text": "New Text", "font_size": 28, "color": "#ffffff",
                   "opacity": 1.0, "x": 0.05, "y": 0.05, "w": 0.25, "h": 0.08,
                   "path": "", "cam_index": 0, "enabled": True}
        overlays = getattr(self.app, "overlays", [])
        overlays.append(new_ov)
        self.app.overlays = overlays
        self.app.save_settings(silent=True)
        self.app._refresh_overlay_badge()
        self.refresh()

    def _open_item_menu(self, idx: int) -> None:
        c = self.app.colors
        menu = tk.Menu(self.frame, tearoff=0, bg=c["surface"], fg=c["text"],
                        activebackground=c["accent"], activeforeground=c["bg"])
        menu.add_command(label="More…", command=lambda: self._open_more(idx))
        menu.add_command(label="Remove", command=lambda: self._remove(idx))
        try:
            x = self.frame.winfo_pointerx(); y = self.frame.winfo_pointery()
            menu.tk_popup(x, y)
        finally:
            try: menu.grab_release()
            except Exception: pass

    def _open_more(self, idx: int) -> None:
        win = OverlayManagerWindow(self.app.root, self.app)
        if 0 <= idx < len(win._overlays):
            win._select(idx)

    def _remove(self, idx: int) -> None:
        overlays = getattr(self.app, "overlays", [])
        if 0 <= idx < len(overlays):
            del overlays[idx]
            self.app.overlays = overlays
            self.app.save_settings(silent=True)
            self.app._refresh_overlay_badge()
            self.refresh()

    def _open_drag_preview(self) -> None:
        OverlayPreviewDialog(self.app.root, self.app, getattr(self.app, "overlays", []),
                              self._on_drag_preview_saved)

    def _on_drag_preview_saved(self, overlays: list) -> None:
        self.app.overlays = overlays
        self.app.save_settings(silent=True)
        self.refresh()

    def _close_panel(self) -> None:
        self.app.show_overlays_panel = False
        self.app.save_settings(silent=True)
        self.app.recreate_widgets()


LANG_SCHEMA_VERSION = 1
THEME_SCHEMA_VERSION = 1
LANG_REQUIRED_KEYS = [
    "app_title","live_preview","ready","recording","paused","fps","resolution",
    "start","pause","stop","resume","recording_btn","audio_mixer","microphone",
    "desktop_audio","mute","unmute","vol","level","enable_audio","ffmpeg_found",
    "ffmpeg_not_found","file_menu","open_recordings","exit","view_menu",
    "always_on_top","fullscreen","pc_analytics","cpu_info","ram_info","disk_info",
    "help_menu","check_updates","report_issue","capture_source","full_desktop",
    "select_window","minimize_tray","language","english","russian","theme","dark",
    "light","settings_menu","preferences","performance_menu","ultra","turbo",
    "balanced","eco","stats","time","status","warning","error","info",
    "folder_not_exist","recording_failed","settings_saved","recording_saved",
    "open_folder","ffmpeg_not_found_msg","saved","recording_status","file","size",
    "duration","audio","merged","separate","no_audio","save","cancel","browse",
    "output_folder","settings_title","video_settings","quality","mode","advanced",
    "monitor","output","countdown","timestamp","cursor","notification","made_by","audio_file",
]
THEME_REQUIRED_KEYS = ["bg","surface","accent","text","text_secondary","success","warning","error"]

def _get_root_dir() -> str:
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    _src = os.path.dirname(os.path.abspath(__file__))
    _parent = os.path.dirname(_src)
    if os.path.isdir(os.path.join(_parent, "src")) or os.path.basename(_src).lower() == "src":
        return _parent
    return _src

_ROOT_DIR     = _get_root_dir()
ASSETS_DIR    = os.path.join(_ROOT_DIR, "Assets")
SETTINGS_PATH = os.path.join(_ROOT_DIR, "homrec_settings.json")
THEMES_DIR    = os.path.join(ASSETS_DIR, "Themes")
LANGS_DIR     = os.path.join(ASSETS_DIR, "L")

_HRC_MAGIC = b'HRC\x01'
_HRL_MAGIC = b'HRL\x01'

def _hrc_write(path: str, data: dict, magic: bytes) -> None:
    body = gzip.compress(json.dumps(data, indent=2, ensure_ascii=False).encode('utf-8'))
    with open(path, 'wb') as f:
        f.write(magic); f.write(body)

def _hrc_read(path: str, expected_magic: bytes) -> dict:
    with open(path, 'rb') as f:
        magic = f.read(4); body = f.read()
    if magic != expected_magic:
        raise ValueError(f"Invalid file format. Expected {expected_magic!r}, got {magic!r}")
    return json.loads(gzip.decompress(body).decode('utf-8'))

def _hrc_detect(path: str) -> str:
    with open(path, 'rb') as f:
        magic = f.read(4)
    if magic == _HRC_MAGIC: return 'hrc'
    if magic == _HRL_MAGIC: return 'hrl'
    raise ValueError(f"Not a HomRec file (magic={magic!r})")



class OverlayManagerWindow:
    def __init__(self, parent, app) -> None:
        self.app = app
        self.c = app.colors
        self._overlays: list[dict] = [dict(o) for o in getattr(app, 'overlays', [])]

        win = tk.Toplevel(parent)
        win.title("Overlays")
        win.geometry("820x620")
        win.configure(bg=self.c["bg"])
        win.resizable(True, True)
        win.minsize(640, 480)
        self.win = win

        hdr = tk.Frame(win, bg=self.c["surface"], pady=0)
        hdr.pack(fill="x")
        tk.Label(hdr, text="🎭  Overlay Manager", bg=self.c["surface"],
                 fg=self.c["accent"], font=("Segoe UI", 13, "bold")).pack(side="left", padx=16, pady=10)
        tk.Button(hdr, text="👁  Preview & Position", command=self._open_preview,
                  bg=self.c.get("surface_light","#45475a"), fg=self.c["accent"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="right", padx=8, pady=8)
        tk.Button(hdr, text="＋  Add Overlay", command=self._add_overlay,
                  bg=self.c["success"], fg=self.c["bg"],
                  font=("Segoe UI", 9, "bold"), relief="flat", padx=12, pady=6).pack(side="right", padx=(0,4), pady=8)

        ftr = tk.Frame(win, bg=self.c["bg"])
        ftr.pack(side="bottom", fill="x", padx=12, pady=8)
        tk.Button(ftr, text="Save & Apply", command=self._save_apply,
                  bg=self.c["accent"], fg=self.c["bg"],
                  font=("Segoe UI", 10, "bold"), relief="flat", padx=16, pady=6).pack(side="right")
        tk.Button(ftr, text="Cancel", command=win.destroy,
                  bg=self.c["surface"], fg=self.c["text"],
                  font=("Segoe UI", 10), relief="flat", padx=12, pady=6).pack(side="right", padx=(0,6))
        self._status_lbl = tk.Label(ftr, text="", bg=self.c["bg"],
                                     fg=self.c["text_secondary"], font=("Segoe UI", 9))
        self._status_lbl.pack(side="left")

        body = tk.Frame(win, bg=self.c["bg"])
        body.pack(fill="both", expand=True, padx=12, pady=(8, 0))

        list_frame = tk.Frame(body, bg=self.c["surface"], width=280)
        list_frame.pack(side="left", fill="y", padx=(0, 8))
        list_frame.pack_propagate(False)
        tk.Label(list_frame, text="Overlays", bg=self.c["surface"],
                 fg=self.c["text_secondary"], font=("Segoe UI", 9, "bold")).pack(anchor="w", padx=10, pady=(8,4))

        canvas = tk.Canvas(list_frame, bg=self.c["surface"], highlightthickness=0)
        scroll = ttk.Scrollbar(list_frame, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)
        self._list_inner = tk.Frame(canvas, bg=self.c["surface"])
        canvas.create_window((0,0), window=self._list_inner, anchor="nw")
        self._list_inner.bind("<Configure>",
            lambda e: canvas.configure(scrollregion=canvas.bbox("all")))

        # Editor area is scrollable so tall per-type forms never push the footer
        # off-screen or get clipped by a short window.
        editor_outer = tk.Frame(body, bg=self.c["bg"])
        editor_outer.pack(side="left", fill="both", expand=True)
        editor_canvas = tk.Canvas(editor_outer, bg=self.c["bg"], highlightthickness=0)
        editor_scroll = ttk.Scrollbar(editor_outer, orient="vertical", command=editor_canvas.yview)
        editor_canvas.configure(yscrollcommand=editor_scroll.set)
        editor_scroll.pack(side="right", fill="y")
        editor_canvas.pack(side="left", fill="both", expand=True)
        self._editor_frame = tk.Frame(editor_canvas, bg=self.c["bg"])
        self._editor_canvas_window = editor_canvas.create_window((0, 0), window=self._editor_frame, anchor="nw")
        self._editor_frame.bind("<Configure>",
            lambda e: editor_canvas.configure(scrollregion=editor_canvas.bbox("all")))
        editor_canvas.bind("<Configure>",
            lambda e: editor_canvas.itemconfigure(self._editor_canvas_window, width=e.width))
        def _on_editor_wheel(event):
            editor_canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
        editor_canvas.bind("<Enter>", lambda e: editor_canvas.bind_all("<MouseWheel>", _on_editor_wheel))
        editor_canvas.bind("<Leave>", lambda e: editor_canvas.unbind_all("<MouseWheel>"))

        self._sel_idx: int | None = None
        self._editor_widgets: dict = {}
        self._refresh_list()
        win.protocol("WM_DELETE_WINDOW", win.destroy)

    # List ------------------------------------------------------------------
    def _refresh_list(self) -> None:
        for w in self._list_inner.winfo_children():
            w.destroy()
        if not self._overlays:
            tk.Label(self._list_inner, text="No overlays yet. Click + Add Overlay to start.",
                     bg=self.c["surface"], fg=self.c["text_secondary"],
                     font=("Segoe UI", 9, "italic"), justify="center").pack(padx=12, pady=20)
            return
        for i, ov in enumerate(self._overlays):
            sel = (i == self._sel_idx)
            bg = self.c["accent"] if sel else (self.c.get("surface_light","#45475a") if i%2 else self.c["surface"])
            fg = self.c["bg"] if sel else self.c["text"]
            row = tk.Frame(self._list_inner, bg=bg, cursor="hand2")
            row.pack(fill="x", padx=4, pady=1)
            icon = {"text":"📝","webcam":"📷","image":"🖼"}.get(ov.get("kind","text"),"?")
            name = ov.get("text","")[:16] if ov.get("kind")=="text" else                    os.path.basename(ov.get("path",""))[:16] if ov.get("kind")=="image" else                    f"Cam#{ov.get('cam_index',0)}"
            enabled_dot = "●" if ov.get("enabled", True) else "○"
            lbl = tk.Label(row, text=f"  {enabled_dot} {icon}  {name}",
                           bg=bg, fg=fg, font=("Segoe UI", 9), anchor="w")
            lbl.pack(side="left", fill="x", expand=True, pady=5, padx=4)
            del_btn = tk.Button(row, text="✕", command=lambda idx=i: self._delete(idx),
                                bg=self.c["error"], fg=self.c["bg"],
                                font=("Segoe UI", 8, "bold"), relief="flat", padx=5, pady=2)
            del_btn.pack(side="right", padx=4, pady=4)
            row.bind("<Button-1>", lambda e, idx=i: self._select(idx))
            lbl.bind("<Button-1>", lambda e, idx=i: self._select(idx))

    def _select(self, idx: int) -> None:
        self._sel_idx = idx
        self._refresh_list()
        self._build_editor(idx)

    def _delete(self, idx: int) -> None:
        del self._overlays[idx]
        if self._sel_idx is not None and self._sel_idx >= len(self._overlays):
            self._sel_idx = len(self._overlays) - 1 if self._overlays else None
        self._refresh_list()
        self._clear_editor()
        self._set_status(f"Overlay deleted.")

    def _add_overlay(self) -> None:
        self._pick_overlay_kind(self._add_overlay_of_kind)

    def _pick_overlay_kind(self, on_chosen) -> None:
        """Small popup asking which overlay type to create. Calls on_chosen(kind)."""
        c = self.c
        dlg = tk.Toplevel(self.win)
        dlg.title("Add Overlay")
        dlg.configure(bg=c["bg"])
        dlg.resizable(False, False)
        dlg.transient(self.win)
        dlg.grab_set()
        tk.Label(dlg, text="What kind of overlay?", bg=c["bg"], fg=c["accent"],
                 font=("Segoe UI", 11, "bold")).pack(padx=20, pady=(18, 12))
        btn_row = tk.Frame(dlg, bg=c["bg"])
        btn_row.pack(padx=20, pady=(0, 18))
        def _choose(kind):
            dlg.destroy()
            on_chosen(kind)
        for kind, label in [("text", "📝  Text"), ("webcam", "📷  Webcam"), ("image", "🖼  Image")]:
            tk.Button(btn_row, text=label, command=lambda k=kind: _choose(k),
                      bg=c.get("surface_light", "#45475a"), fg=c["text"],
                      font=("Segoe UI", 10), relief="flat", padx=16, pady=10, width=10
                      ).pack(side="left", padx=6)
        dlg.update_idletasks()
        x = self.win.winfo_rootx() + (self.win.winfo_width() - dlg.winfo_width()) // 2
        y = self.win.winfo_rooty() + (self.win.winfo_height() - dlg.winfo_height()) // 2
        dlg.geometry(f"+{max(0,x)}+{max(0,y)}")

    def _add_overlay_of_kind(self, kind: str) -> None:
        defaults_by_kind = {
            "text":   {"text": "New Text", "font_size": 28, "color": "#ffffff"},
            "webcam": {"cam_index": 0},
            "image":  {"path": ""},
        }
        new_ov = {"kind": kind, "opacity": 1.0, "x": 0.05, "y": 0.05, "w": 0.25, "h": 0.08,
                   "enabled": True}
        new_ov.update(defaults_by_kind.get(kind, {}))
        self._overlays.append(new_ov)
        self._sel_idx = len(self._overlays) - 1
        self._refresh_list()
        self._build_editor(self._sel_idx)

    # Editor ----------------------------------------------------------------
    def _clear_editor(self) -> None:
        for w in self._editor_frame.winfo_children():
            w.destroy()
        self._editor_widgets = {}

    def _build_editor(self, idx: int) -> None:
        """Show only fields relevant to the overlay type - no clutter."""
        self._clear_editor()
        ov   = self._overlays[idx]
        c    = self.c
        ef   = self._editor_frame
        w    = self._editor_widgets
        kind = ov.get("kind", "text")

        kind_icon = {"text": "📝", "webcam": "📷", "image": "🖼"}.get(kind, "?")
        tk.Label(ef, text=f"{kind_icon}  Edit {kind.capitalize()} Overlay",
                 bg=c["bg"], fg=c["accent"],
                 font=("Segoe UI", 11, "bold")).grid(
                 row=0, column=0, columnspan=4, sticky="w", padx=8, pady=(4, 10))

        row = [1]
        def _lbl(text):
            tk.Label(ef, text=text, bg=c["bg"], fg=c["text"],
                     font=("Segoe UI", 9), anchor="w", width=13).grid(
                     row=row[0], column=0, sticky="w", padx=(8,4), pady=4)
        def _next():
            row[0] += 1

        # Type selector
        _lbl("Type:")
        kind_var = tk.StringVar(value=kind)
        kf = tk.Frame(ef, bg=c["bg"])
        kf.grid(row=row[0], column=1, columnspan=3, sticky="w", pady=4)
        for kv, kl in [("text","📝 Text"),("webcam","📷 Webcam"),("image","🖼 Image")]:
            tk.Radiobutton(kf, text=kl, variable=kind_var, value=kv,
                           bg=c["bg"], fg=c["text"], selectcolor=c["surface"],
                           activebackground=c["bg"], font=("Segoe UI", 9),
                           command=lambda: (self._apply_editor(idx), self._build_editor(idx))
                           ).pack(side="left", padx=6)
        w["kind"] = kind_var
        _next()

        # Enabled
        _lbl("Enabled:")
        en_var = tk.BooleanVar(value=ov.get("enabled", True))
        tk.Checkbutton(ef, variable=en_var, bg=c["bg"], fg=c["text"],
                       selectcolor=c["surface"], activebackground=c["bg"]).grid(
                       row=row[0], column=1, sticky="w", pady=4)
        w["enabled"] = en_var
        _next()

        # Set defaults for all keys so _apply_editor never KeyErrors
        w["text"]      = tk.StringVar(value=ov.get("text", ""))
        w["font_size"] = tk.IntVar(value=ov.get("font_size", 28))
        w["color"]     = tk.StringVar(value=ov.get("color", "#ffffff"))
        w["path"]      = tk.StringVar(value=ov.get("path", ""))
        w["cam_index"] = tk.IntVar(value=ov.get("cam_index", 0))

        if kind == "text":
            _lbl("Text:")
            tk.Entry(ef, textvariable=w["text"], bg=c["surface"], fg=c["text"],
                     font=("Segoe UI", 10), relief="flat", width=28).grid(
                     row=row[0], column=1, columnspan=3, sticky="ew", padx=4, pady=4)
            _next()

            _lbl("Font size:")
            tk.Scale(ef, variable=w["font_size"], from_=8, to=120,
                     orient="horizontal", length=220, bg=c["bg"], fg=c["text"],
                     troughcolor=c["surface"], highlightthickness=0,
                     showvalue=True).grid(row=row[0], column=1, columnspan=3,
                     sticky="ew", pady=4)
            _next()

            _lbl("Color:")
            col_row = tk.Frame(ef, bg=c["bg"])
            col_row.grid(row=row[0], column=1, columnspan=3, sticky="w", pady=4)
            col_prev = tk.Label(col_row, bg=w["color"].get(), width=4,
                                relief="flat", cursor="hand2")
            col_prev.pack(side="left")
            def _pick_color():
                
                res = cc.askcolor(color=w["color"].get(), parent=self.win)
                if res[1]:
                    w["color"].set(res[1]); col_prev.config(bg=res[1])
            col_prev.bind("<Button-1>", lambda e: _pick_color())
            tk.Label(col_row, textvariable=w["color"], bg=c["bg"],
                     fg=c["text_secondary"], font=("Consolas", 9)).pack(side="left", padx=8)
            _next()

        elif kind == "image":
            _lbl("Image file:")
            pf = tk.Frame(ef, bg=c["bg"])
            pf.grid(row=row[0], column=1, columnspan=3, sticky="ew", pady=4)
            tk.Entry(pf, textvariable=w["path"], bg=c["surface"], fg=c["text"],
                     font=("Segoe UI", 9), relief="flat", width=20).pack(side="left")
            tk.Button(pf, text="Browse…",
                      command=lambda: w["path"].set(
                          filedialog.askopenfilename(
                              filetypes=[("Images", "*.png *.jpg *.jpeg *.bmp *.gif"),
                                         ("All", "*.*")]) or w["path"].get()),
                      bg=c["surface"], fg=c["accent"],
                      font=("Segoe UI", 9), relief="flat", padx=8).pack(side="left", padx=6)
            _next()

        elif kind == "webcam":
            _lbl("Webcam:")
            cam_names = self.app.list_webcams()
            cur_idx = ov.get("cam_index", 0)
            cur_name = cam_names[cur_idx] if 0 <= cur_idx < len(cam_names) else (cam_names[0] if cam_names else "Integrated Webcam")
            cam_name_var = tk.StringVar(value=cur_name)
            cam_row = tk.Frame(ef, bg=c["bg"])
            cam_row.grid(row=row[0], column=1, columnspan=3, sticky="ew", pady=4)
            cam_combo = ttk.Combobox(cam_row, textvariable=cam_name_var, values=cam_names,
                                      width=24, state="readonly")
            cam_combo.pack(side="left")
            tk.Button(cam_row, text="⟳", command=lambda: (
                        setattr(self.app, '_dshow_cam_names_cache', None),
                        cam_combo.configure(values=self.app.list_webcams())
                      ), bg=c["surface"], fg=c["accent"], font=("Segoe UI", 9),
                      relief="flat", padx=6).pack(side="left", padx=4)
            w["_cam_names"] = cam_names
            w["cam_index"] = cam_name_var  # overridden: holds the selected name, resolved to index on apply
            _next()

        # Opacity (always shown)
        tk.Frame(ef, bg=c["surface"], height=1).grid(
            row=row[0], column=0, columnspan=4, sticky="ew", padx=8, pady=(8,4))
        _next()
        _lbl("Opacity:")
        op_var = tk.DoubleVar(value=ov.get("opacity", 1.0))
        tk.Scale(ef, variable=op_var, from_=0.0, to=1.0, resolution=0.05,
                 orient="horizontal", length=220, bg=c["bg"], fg=c["text"],
                 troughcolor=c["surface"], highlightthickness=0,
                 showvalue=True).grid(row=row[0], column=1, columnspan=3,
                 sticky="ew", pady=4)
        w["opacity"] = op_var
        _next()

        # Position & Size
        tk.Label(ef, text="Position & Size  (0.0–1.0, fraction of screen)",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI", 8, "italic")).grid(
                 row=row[0], column=0, columnspan=4, sticky="w", padx=8, pady=(8,2))
        _next()
        for var_key, label, def_val, lo, hi in [
            ("x", "X:", ov.get("x",0.05), 0.0, 0.95),
            ("y", "Y:", ov.get("y",0.05), 0.0, 0.95),
            ("w", "W:", ov.get("w",0.25), 0.02, 1.0),
            ("h", "H:", ov.get("h",0.12), 0.02, 1.0),
        ]:
            _lbl(label)
            var = tk.DoubleVar(value=def_val)
            tk.Scale(ef, variable=var, from_=lo, to=hi, resolution=0.01,
                     orient="horizontal", length=240, bg=c["bg"], fg=c["text"],
                     troughcolor=c["surface"], highlightthickness=0,
                     showvalue=True).grid(row=row[0], column=1, columnspan=3,
                     sticky="ew", pady=2)
            w[var_key] = var
            _next()

        tk.Button(ef, text="✔  Apply", command=lambda: self._apply_editor(idx),
                  bg=c["success"], fg=c["bg"],
                  font=("Segoe UI", 10, "bold"), relief="flat",
                  padx=10, pady=6).grid(
                  row=row[0], column=0, columnspan=4,
                  pady=(14,4), padx=8, sticky="ew")

        ef.columnconfigure(1, weight=1)

    def _apply_editor(self, idx: int) -> None:
        w = self._editor_widgets
        if not w:
            return
        ov = self._overlays[idx]
        ov["kind"]      = w["kind"].get()
        ov["enabled"]   = w["enabled"].get()
        ov["text"]      = w["text"].get()
        ov["font_size"] = w["font_size"].get()
        ov["color"]     = w["color"].get()
        ov["path"]      = w["path"].get()
        if "_cam_names" in w:
            cam_names = w["_cam_names"]
            chosen_name = w["cam_index"].get()
            ov["cam_index"] = cam_names.index(chosen_name) if chosen_name in cam_names else 0
        else:
            ov["cam_index"] = w["cam_index"].get()
        ov["opacity"]   = round(w["opacity"].get(), 2)
        ov["x"] = round(w["x"].get(), 3)
        ov["y"] = round(w["y"].get(), 3)
        ov["w"] = round(w["w"].get(), 3)
        ov["h"] = round(w["h"].get(), 3)
        self._refresh_list()
        self._set_status("Applied.")

    def _open_preview(self) -> None:
        # Sync current editor first
        if self._sel_idx is not None and self._editor_widgets:
            self._apply_editor(self._sel_idx)
        OverlayPreviewDialog(self.win, self.app, self._overlays, self._on_preview_updated)

    def _on_preview_updated(self, overlays: list) -> None:
        self._overlays = overlays
        self._refresh_list()
        if self._sel_idx is not None and self._sel_idx < len(self._overlays):
            self._build_editor(self._sel_idx)

    def _save_apply(self) -> None:
        if self._sel_idx is not None and self._editor_widgets:
            self._apply_editor(self._sel_idx)
        self.app.overlays = self._overlays
        self.app.save_settings(silent=True)
        self.app._refresh_overlay_badge()
        if getattr(self.app, 'overlays_panel', None):
            try: self.app.overlays_panel.refresh()
            except Exception: pass
        self._set_status("Saved & applied.")
        self.win.after(800, self.win.destroy)

    def _set_status(self, msg: str) -> None:
        self._status_lbl.config(text=msg)
        self.win.after(3000, lambda: self._status_lbl.config(text=""))

class OverlayPreviewDialog:
    _HANDLE = 12  # resize handle size px

    def __init__(self, parent, app, overlays: list[dict], callback) -> None:
        self.app = app
        self.callback = callback
        self._overlays = [dict(o) for o in overlays]
        self._sel = None
        self._drag_mode = None
        self._drag_origin = (0, 0)
        self._drag_ov_origin = (0, 0, 0, 0)
        # cached render state
        self._render_ox = self._render_oy = 0
        self._render_bw = self._render_bh = 1

        c = app.colors
        self.dlg = tk.Toplevel(parent)
        self.dlg.title("Overlay Preview — drag to position")
        # Pick window size matching 16:9 + header/footer
        self.dlg.geometry("1024x620")
        self.dlg.configure(bg=c["bg"])
        self.dlg.grab_set()
        self.dlg.resizable(True, True)
        self._c = c

        top = tk.Frame(self.dlg, bg=c["bg"])
        top.pack(fill="x", padx=10, pady=(8, 4))
        tk.Label(top, text="Click an overlay to select it. Drag to move. Drag bottom-right corner to resize.",
                 bg=c["bg"], fg=c["text_secondary"], font=("Segoe UI", 9)).pack(side="left")
        self._sel_lbl = tk.Label(top, text="", bg=c["bg"],
                                  fg=c["accent"], font=("Segoe UI", 9, "bold"))
        self._sel_lbl.pack(side="right", padx=8)

        canvas_frame = tk.Frame(self.dlg, bg="#000000", bd=0)
        canvas_frame.pack(fill="both", expand=True, padx=10, pady=(0, 4))
        self.canvas = tk.Canvas(canvas_frame, bg="#111111",
                                highlightthickness=0, cursor="crosshair")
        self.canvas.pack(fill="both", expand=True)

        bot = tk.Frame(self.dlg, bg=c["bg"])
        bot.pack(fill="x", padx=10, pady=(0, 8))
        tk.Button(bot, text="OK — apply positions", command=self._ok,
                  bg=c["success"], fg=c["bg"], font=("Segoe UI", 10, "bold"),
                  relief="flat", padx=16, pady=6).pack(side="right", padx=(6, 0))
        tk.Button(bot, text="Cancel", command=self.dlg.destroy,
                  bg=c["surface"], fg=c["text"], font=("Segoe UI", 10),
                  relief="flat", padx=12, pady=6).pack(side="right")

        self.canvas.bind("<Configure>", lambda e: self._render())
        self.canvas.bind("<ButtonPress-1>",  self._on_press)
        self.canvas.bind("<B1-Motion>",       self._on_drag)
        self.canvas.bind("<ButtonRelease-1>", self._on_release)
        self._bg_source = None
        self._bg_tk = None
        self.dlg.after(120, self._grab_bg)

    # Screenshot ------------------------------------------------------------
    def _grab_bg(self) -> None:
        """Grab a screenshot of the selected monitor, store as PIL Image."""
        try:
            import mss as _mss
            with _mss.mss() as sct:
                mon_id = getattr(self.app, "monitor_id", 1)
                monitors = sct.monitors
                mon = monitors[min(mon_id, len(monitors) - 1)]
                shot = sct.grab(mon)
                self._bg_source = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
                self._bg_orig_w, self._bg_orig_h = shot.size
        except Exception:
            self._bg_source = None
            self._bg_orig_w = 1920
            self._bg_orig_h = 1080
        self.dlg.after(0, self._render)

    # Render ----------------------------------------------------------------
    def _render(self) -> None:
        cw = self.canvas.winfo_width()
        ch = self.canvas.winfo_height()
        if cw < 4 or ch < 4:
            return
        self.canvas.delete("all")

        # --- Background: letterbox preserving source aspect ratio ---
        src_w = getattr(self, "_bg_orig_w", 1920)
        src_h = getattr(self, "_bg_orig_h", 1080)
        scale = min(cw / src_w, ch / src_h)
        bw = int(src_w * scale)
        bh = int(src_h * scale)
        ox = (cw - bw) // 2
        oy = (ch - bh) // 2

        # Draw black bars
        self.canvas.create_rectangle(0, 0, cw, ch, fill="#000000", outline="")

        if self._bg_source is not None:
            resized = self._bg_source.resize((bw, bh), Image.BILINEAR)
            self._bg_tk = ImageTk.PhotoImage(resized)
            self.canvas.create_image(ox, oy, anchor="nw", image=self._bg_tk)
        else:
            # Placeholder grid
            self.canvas.create_rectangle(ox, oy, ox+bw, oy+bh, fill="#1e1e2e", outline="#313244")
            self.canvas.create_text(ox + bw//2, oy + bh//2,
                                     text="Screen preview (mss not installed)",
                                     fill="#6c7086", font=("Segoe UI", 11), justify="center")

        # Store render coords for hit-test and drag
        self._render_ox = ox
        self._render_oy = oy
        self._render_bw = bw
        self._render_bh = bh

        # --- Draw each overlay as a visible tinted box ---
        KIND_COLOR = {"text": "#cba6f7", "webcam": "#89dceb", "image": "#a6e3a1"}
        for i, ov in enumerate(self._overlays):
            if not ov.get("enabled", True):
                continue
            x1, y1, x2, y2 = self._ov_canvas_rect(ov)
            selected = (i == self._sel)
            kind = ov.get("kind", "text")
            base_col = KIND_COLOR.get(kind, "#cdd6f4")
            border_w = 2 if selected else 1

            # Filled semi-opaque box — drawn with a stipple so it doesn't hide bg
            self.canvas.create_rectangle(
                x1, y1, x2, y2,
                fill=base_col, outline=base_col,
                stipple="gray25", width=0
            )
            # Solid border
            self.canvas.create_rectangle(
                x1, y1, x2, y2,
                fill="", outline=base_col,
                width=border_w,
                dash=(6, 3) if not selected else None
            )

            # Label
            icon = {"text": "T", "webcam": "CAM", "image": "IMG"}.get(kind, "?")
            if kind == "text":
                name = ov.get("text", "")[:20]
            elif kind == "image":
                name = os.path.basename(ov.get("path", ""))[:18]
            else:
                name = f"Cam#{ov.get('cam_index', 0)}"
            op_str = f"{int(ov.get('opacity', 1.0) * 100)}%"
            label = f"[{icon}] {name}  {op_str}"

            # Shadow
            self.canvas.create_text(
                (x1 + x2) // 2 + 1, (y1 + y2) // 2 + 1,
                text=label, fill="#000000", font=("Segoe UI", 9, "bold")
            )
            self.canvas.create_text(
                (x1 + x2) // 2, (y1 + y2) // 2,
                text=label, fill="#ffffff", font=("Segoe UI", 9, "bold")
            )

            # Resize handle (bottom-right corner)
            h = self._HANDLE
            self.canvas.create_rectangle(
                x2 - h, y2 - h, x2, y2,
                fill=base_col, outline="#ffffff", width=1
            )
            # Selection ring
            if selected:
                self.canvas.create_rectangle(
                    x1 - 2, y1 - 2, x2 + 2, y2 + 2,
                    fill="", outline="#ffffff", width=1, dash=(3, 3)
                )

    # Geometry helpers ------------------------------------------------------
    def _ov_canvas_rect(self, ov: dict) -> tuple:
        bw = max(self._render_bw, 1)
        bh = max(self._render_bh, 1)
        ox = self._render_ox
        oy = self._render_oy
        x1 = int(ox + ov.get("x", 0.05) * bw)
        y1 = int(oy + ov.get("y", 0.05) * bh)
        x2 = int(x1 + max(0.02, ov.get("w", 0.2)) * bw)
        y2 = int(y1 + max(0.02, ov.get("h", 0.1)) * bh)
        return x1, y1, x2, y2

    # Interaction -----------------------------------------------------------
    def _hit_test(self, mx: int, my: int) -> tuple:
        h = self._HANDLE
        for i, ov in enumerate(self._overlays):
            if not ov.get("enabled", True):
                continue
            x1, y1, x2, y2 = self._ov_canvas_rect(ov)
            if x2 - h <= mx <= x2 and y2 - h <= my <= y2:
                return i, "resize"
            if x1 <= mx <= x2 and y1 <= my <= y2:
                return i, "move"
        return None, ""

    def _on_press(self, event) -> None:
        idx, mode = self._hit_test(event.x, event.y)
        self._sel = idx
        self._drag_mode = mode
        self._drag_origin = (event.x, event.y)
        if idx is not None:
            ov = self._overlays[idx]
            self._drag_ov_origin = (
                ov.get("x", 0.05), ov.get("y", 0.05),
                ov.get("w", 0.2),  ov.get("h", 0.1)
            )
            kind = ov.get("kind", "text")
            name = ov.get("text","") if kind=="text" else                    os.path.basename(ov.get("path","")) if kind=="image" else                    f"Cam#{ov.get('cam_index',0)}"
            self._sel_lbl.config(text=f"Selected: [{kind}] {name[:24]}")
        else:
            self._sel_lbl.config(text="")
        self._render()

    def _on_drag(self, event) -> None:
        if self._sel is None or self._drag_mode is None:
            return
        dx = event.x - self._drag_origin[0]
        dy = event.y - self._drag_origin[1]
        bw = max(self._render_bw, 1)
        bh = max(self._render_bh, 1)
        ox0, oy0, ow0, oh0 = self._drag_ov_origin
        ov = self._overlays[self._sel]
        if self._drag_mode == "move":
            ov["x"] = round(max(0.0, min(0.95, ox0 + dx / bw)), 3)
            ov["y"] = round(max(0.0, min(0.95, oy0 + dy / bh)), 3)
        elif self._drag_mode == "resize":
            ov["w"] = round(max(0.02, min(1.0, ow0 + dx / bw)), 3)
            ov["h"] = round(max(0.02, min(1.0, oh0 + dy / bh)), 3)
        self._render()

    def _on_release(self, event) -> None:
        self._drag_mode = None

    def _ok(self) -> None:
        self.callback(self._overlays)
        self.dlg.destroy()


class AdvancedSettingsDialog:
    HRC_VERSION = 1

    def __init__(self, parent: tk.Tk, app) -> None:
        self.app = app
        self.dialog = tk.Toplevel(parent)
        self.dialog.title("⚙ Advanced Settings")
        self.dialog.geometry("600x680")
        self.dialog.resizable(True, True)
        self.dialog.minsize(560, 600)
        self.dialog.configure(bg=app.colors["bg"])
        self.dialog.grab_set()
        try:
            icon_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "main.ico")
            if os.path.exists(icon_path):
                self.dialog.after(50, lambda: self.dialog.iconbitmap(icon_path))
        except Exception: pass
        self._build_ui()

    def _build_ui(self) -> None:
        a = self.app; c = a.colors
        tk.Label(self.dialog, text="⚙ Advanced Settings", bg=c["bg"], fg=c["accent"], font=("Segoe UI", 14, "bold")).pack(pady=(16, 4), padx=20, anchor="w")
        tk.Label(self.dialog, text="For power users. Changes apply on next recording.", bg=c["bg"], fg=c["text_secondary"], font=("Segoe UI", 9)).pack(padx=20, anchor="w")

        notebook = ttk.Notebook(self.dialog)
        notebook.pack(fill="both", expand=True, padx=16, pady=12)

        vt = tk.Frame(notebook, bg=c["bg"]); notebook.add(vt, text="Video")
        self._cv = tk.StringVar(value=getattr(a, "video_codec", "libx264"))
        self._row(vt, "Codec", ttk.Combobox(vt, textvariable=self._cv, values=["libx264","libx265","h264_nvenc","hevc_nvenc","h264_amf","hevc_amf","h264_qsv","hevc_qsv"], width=18, state="readonly"))
        self._hwv = tk.StringVar(value=getattr(a, "hw_accel", "auto"))
        self._row(vt, "HW Accel", ttk.Combobox(vt, textvariable=self._hwv, values=["auto","none","cuda","dxva2","d3d11va"], width=12, state="readonly"))
        self._prev = tk.StringVar(value=getattr(a, "enc_preset", "ultrafast"))
        self._row(vt, "Preset", ttk.Combobox(vt, textvariable=self._prev, values=["ultrafast","superfast","veryfast","faster","fast","medium","slow"], width=12, state="readonly"))
        self._crfv = tk.IntVar(value=getattr(a, "enc_crf", 18))
        self._row(vt, "CRF (quality)", tk.Scale(vt, variable=self._crfv, from_=0, to=51, orient="horizontal", length=180, bg=c["bg"], fg=c["text"], highlightthickness=0, troughcolor=c["surface"]))
        self._pixv = tk.StringVar(value="yuv420p")
        self._row(vt, "Pixel format", ttk.Combobox(vt, textvariable=self._pixv, values=["yuv420p"], width=12, state="disabled"))
        _row_note = vt.grid_size()[1]
        tk.Label(vt, text="Locked to yuv420p for player compatibility (yuv444p/rgb24 broke playback)",
                 bg=c["bg"], fg=c.get("text_secondary", "#888"), font=("Segoe UI", 8)).grid(row=_row_note, column=0, columnspan=3, sticky="w", padx=(20, 4), pady=(0, 6))

        at = tk.Frame(notebook, bg=c["bg"]); notebook.add(at, text="Audio")
        self._srv = tk.StringVar(value=str(getattr(a, "audio_sample_rate", 44100)))
        self._row(at, "Sample rate", ttk.Combobox(at, textvariable=self._srv, values=["44100","48000","96000"], width=10, state="readonly"))
        self._abrv = tk.StringVar(value=getattr(a, "audio_aac_bitrate", "192k"))
        self._row(at, "AAC bitrate", ttk.Combobox(at, textvariable=self._abrv, values=["96k","128k","192k","256k","320k"], width=10, state="readonly"))
        self._achv = tk.StringVar(value=str(getattr(a, "audio_out_channels", 2)))
        self._row(at, "Channels", ttk.Combobox(at, textvariable=self._achv, values=["1","2"], width=6, state="readonly"))
        tk.Frame(at, bg=a.colors["surface"], height=1).grid(row=at.grid_size()[1], column=0, columnspan=3, sticky="ew", padx=20, pady=(10,4))
        row_sep_a = at.grid_size()[1]
        tk.Label(at, text="Separate Audio Export", bg=a.colors["bg"], fg=a.colors["accent"], font=("Segoe UI", 10, "bold"), anchor="w").grid(row=row_sep_a, column=0, columnspan=3, sticky="w", padx=(20,8), pady=(0,4))
        self._sep_mp3v = tk.BooleanVar(value=getattr(a, "separate_audio_mp3", False))
        row_mp3 = at.grid_size()[1]
        tk.Checkbutton(at, text="Save audio as separate .mp3  (next to video file)", variable=self._sep_mp3v,
                       bg=a.colors["bg"], fg=a.colors["text"], selectcolor=a.colors["surface"],
                       font=("Segoe UI", 10), activebackground=a.colors["bg"]).grid(row=row_mp3, column=0, columnspan=3, sticky="w", padx=20, pady=4)

        it = tk.Frame(notebook, bg=c["bg"]); notebook.add(it, text="Interface")
        self._thv = tk.StringVar(value=getattr(a, "ui_theme", "dark"))
        self._row(it, "Theme", ttk.Combobox(it, textvariable=self._thv, values=["dark","light"], width=10, state="readonly"))

        row_sep = it.grid_size()[1]
        tk.Frame(it, bg=c["surface"], height=1).grid(row=row_sep, column=0, columnspan=3, sticky="ew", padx=20, pady=(12,4))

        row_hrl = it.grid_size()[1]
        tk.Label(it, text="Language", bg=c["bg"], fg=c["text"], font=("Segoe UI", 10), anchor="w").grid(row=row_hrl, column=0, sticky="w", padx=(20,8), pady=4)
        tk.Button(it, text="📥 Install .hrl...", command=self._install_hrl, bg=c["surface"], fg=c["accent"], font=("Segoe UI", 9), relief="flat", padx=10, pady=5).grid(row=row_hrl, column=1, sticky="w", pady=4)

        row_dl = it.grid_size()[1]
        tk.Label(it, text="Delete language", bg=c["bg"], fg=c["text"], font=("Segoe UI", 10), anchor="w").grid(row=row_dl, column=0, sticky="w", padx=(20,8), pady=4)
        self._del_lang_var = tk.StringVar()
        del_lang_combo = ttk.Combobox(it, textvariable=self._del_lang_var, values=[code for code, _ in self.app._scan_custom_languages()], width=16, state="readonly")
        del_lang_combo.grid(row=row_dl, column=1, sticky="w", padx=(0,4), pady=4)
        tk.Button(it, text="🗑 Delete", command=lambda: self._delete_asset(self._del_lang_var.get(), "language", del_lang_combo), bg=c["error"], fg=c["bg"], font=("Segoe UI", 9), relief="flat", padx=8, pady=3).grid(row=row_dl, column=2, sticky="w", pady=4)

        self._uisv = tk.StringVar(value=str(int(getattr(a, "ui_scale", 1.0)*100))+"%")
        self._row(it, "UI scale", ttk.Combobox(it, textvariable=self._uisv, values=["80%","90%","100%","110%","125%"], width=8, state="readonly"))
        self._fontv = tk.StringVar(value=getattr(a, "ui_font", "Segoe UI"))
        self._row(it, "Font", ttk.Combobox(it, textvariable=self._fontv, values=["Segoe UI","Consolas","Arial","Calibri"], width=14, state="readonly"))

        rt = tk.Frame(notebook, bg=c["bg"]); notebook.add(rt, text="Recording")
        self._ftv = tk.StringVar(value=getattr(a, "filename_template", "HomRec_{date}_{time}"))
        self._row(rt, "File template", tk.Entry(rt, textvariable=self._ftv, bg=c["surface"], fg=c["text"], font=("Consolas", 10), relief="flat", width=24))
        self._asv = tk.StringVar(value=str(getattr(a, "auto_stop_min", 0)))
        self._row(rt, "Auto-stop (min)", tk.Spinbox(rt, textvariable=self._asv, from_=0, to=480, width=6, bg=c["surface"], fg=c["text"], relief="flat"))
        tk.Label(rt, text="  0 = disabled", bg=c["bg"], fg=c["text_secondary"], font=("Segoe UI", 8)).grid(row=rt.grid_size()[1], column=1, sticky="w", padx=(0, 20))
        self._rbv = tk.StringVar(value=str(getattr(a, "replay_buffer_sec", 0)))
        self._row(rt, "Replay buffer (s)", tk.Spinbox(rt, textvariable=self._rbv, from_=0, to=300, width=6, bg=c["surface"], fg=c["text"], relief="flat"))
        tk.Label(rt, text="  0 = disabled", bg=c["bg"], fg=c["text_secondary"], font=("Segoe UI", 8)).grid(row=rt.grid_size()[1], column=1, sticky="w", padx=(0, 20))

        ht = tk.Frame(notebook, bg=c["bg"]); notebook.add(ht, text="Hotkeys")
        tk.Label(ht, text="Click a field and press any key combination", bg=c["bg"], fg=c["text_secondary"], font=("Segoe UI", 9)).grid(row=0, column=0, columnspan=2, padx=20, pady=(10,4), sticky="w")
        self._hk_ss = tk.StringVar(value=getattr(a, "hotkey_start_stop", "F9"))
        self._hk_p  = tk.StringVar(value=getattr(a, "hotkey_pause", "F10"))
        self._hk_fs = tk.StringVar(value=getattr(a, "hotkey_fullscreen", "F11"))
        for label, var in [("Start / Stop", self._hk_ss), ("Pause / Resume", self._hk_p), ("Fullscreen", self._hk_fs)]:
            entry = tk.Entry(ht, textvariable=var, bg=c["surface"], fg=c["accent"], font=("Consolas", 11), relief="flat", width=12, readonlybackground=c["surface"], state="readonly")
            entry.bind("<FocusIn>",  lambda e, v=var, en=entry: self._start_key_capture(v, en))
            entry.bind("<FocusOut>", lambda e, en=entry: en.config(state="readonly"))
            self._row(ht, label, entry)

        nt = tk.Frame(notebook, bg=c["bg"]); notebook.add(nt, text="Notifications")
        self._notif_sound = tk.BooleanVar(value=getattr(a, "notify_sound", True))
        self._notif_flash = tk.BooleanVar(value=getattr(a, "notify_flash", True))
        self._auto_save   = tk.BooleanVar(value=getattr(a, "auto_save_profile", False))
        for text, var in [("Sound beep on recording start", self._notif_sound), ("Flash border on recording start", self._notif_flash), ("Auto-save profile on exit", self._auto_save)]:
            row = nt.grid_size()[1]
            tk.Checkbutton(nt, text=text, variable=var, bg=c["bg"], fg=c["text"], selectcolor=c["surface"], font=("Segoe UI", 10)).grid(row=row, column=0, columnspan=2, sticky="w", padx=20, pady=4)

        ot = tk.Frame(notebook, bg=c["bg"]); notebook.add(ot, text="Overlays")
        self._build_overlays_tab(ot)

        sep = tk.Frame(self.dialog, bg=c["surface"], height=1)
        sep.pack(fill="x", padx=16, pady=(4, 0))
        bot = tk.Frame(self.dialog, bg=c["bg"])
        bot.pack(fill="x", padx=16, pady=10)
        tk.Button(bot, text="⬆ Export .hrc", command=self._export, bg=c["surface"], fg=c["text"], font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="left", padx=(0, 6))
        tk.Button(bot, text="⬇ Import .hrc", command=self._import, bg=c["surface"], fg=c["text"], font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="left")
        tk.Button(bot, text="Cancel", command=self.dialog.destroy, bg=c["surface"], fg=c["text"], font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="right", padx=(6, 0))
        tk.Button(bot, text="Save", command=self._save, bg=c["success"], fg=c["bg"], font=("Segoe UI", 9, "bold"), relief="flat", padx=16, pady=6).pack(side="right")

    def _install_hrl(self) -> None:
        path = filedialog.askopenfilename(
            filetypes=[("HomRec Language", "*.hrl"), ("All files", "*.*")],
            title="Install language (.hrl)")
        if not path: return
        self.app._import_hrl(path)

    def _build_overlays_tab(self, parent: tk.Frame) -> None:
        c = self.app.colors
        self._overlays: list[dict] = [dict(o) for o in getattr(self.app, "overlays", [])]

        top = tk.Frame(parent, bg=c["bg"])
        top.pack(fill="x", padx=12, pady=(10, 4))
        tk.Label(top, text="Overlays", bg=c["bg"], fg=c["accent"],
                 font=("Segoe UI", 11, "bold")).pack(side="left")
        tk.Button(top, text="＋ Add", command=self._overlay_add,
                  bg=c["success"], fg=c["bg"], font=("Segoe UI", 9, "bold"),
                  relief="flat", padx=10, pady=4).pack(side="right")
        tk.Button(top, text="👁 Preview", command=self._overlay_preview,
                  bg=c["surface"], fg=c["accent"], font=("Segoe UI", 9),
                  relief="flat", padx=10, pady=4).pack(side="right", padx=(0, 6))

        list_outer = tk.Frame(parent, bg=c["surface"], relief="flat", bd=1)
        list_outer.pack(fill="both", expand=True, padx=12, pady=(0, 8))
        self._ov_canvas = tk.Canvas(list_outer, bg=c["bg"], highlightthickness=0)
        ov_scroll = ttk.Scrollbar(list_outer, orient="vertical", command=self._ov_canvas.yview)
        self._ov_canvas.configure(yscrollcommand=ov_scroll.set)
        ov_scroll.pack(side="right", fill="y")
        self._ov_canvas.pack(side="left", fill="both", expand=True)
        self._ov_list_frame = tk.Frame(self._ov_canvas, bg=c["bg"])
        self._ov_canvas.create_window((0, 0), window=self._ov_list_frame, anchor="nw")
        self._ov_list_frame.bind("<Configure>",
            lambda e: self._ov_canvas.configure(scrollregion=self._ov_canvas.bbox("all")))
        self._refresh_overlay_list()

    def _refresh_overlay_list(self) -> None:
        c = self.app.colors
        for w in self._ov_list_frame.winfo_children():
            w.destroy()
        if not self._overlays:
            tk.Label(self._ov_list_frame, text="No overlays — click  ＋ Add",
                     bg=c["bg"], fg=c["text_secondary"], font=("Segoe UI", 9, "italic")).pack(padx=16, pady=16)
            return
        for i, ov in enumerate(self._overlays):
            row = tk.Frame(self._ov_list_frame, bg=c["surface_light"] if i % 2 else c["bg"])
            row.pack(fill="x", padx=4, pady=2)
            kind_icon = {"text": "📝", "webcam": "📷", "image": "🖼"}.get(ov.get("kind","text"), "?")
            label_text = f"{kind_icon}  {ov.get('kind','').capitalize()}  — opacity {int(ov.get('opacity',1.0)*100)}%  @ ({int(ov.get('x',0.1)*100)}%, {int(ov.get('y',0.1)*100)}%)"
            if ov.get("kind") == "text":
                label_text += '  "' + ov.get('text','')[:24] + '"'
            elif ov.get("kind") == "image":
                label_text += f"  {os.path.basename(ov.get('path',''))}"
            tk.Label(row, text=label_text, bg=row["bg"], fg=c["text"],
                     font=("Segoe UI", 9), anchor="w").pack(side="left", padx=8, pady=4, fill="x", expand=True)
            tk.Button(row, text="✎", command=lambda idx=i: self._overlay_edit(idx),
                      bg=c["surface"], fg=c["accent"], font=("Segoe UI", 9),
                      relief="flat", padx=6, pady=2).pack(side="right", padx=2)
            tk.Button(row, text="✕", command=lambda idx=i: self._overlay_delete(idx),
                      bg=c["error"], fg=c["bg"], font=("Segoe UI", 9, "bold"),
                      relief="flat", padx=6, pady=2).pack(side="right", padx=2)

    def _overlay_add(self) -> None:
        self._overlay_edit(None)

    def _overlay_delete(self, idx: int) -> None:
        del self._overlays[idx]
        self._refresh_overlay_list()

    def _overlay_edit(self, idx) -> None:
        c = self.app.colors
        existing = self._overlays[idx] if idx is not None else None
        dlg = tk.Toplevel(self.dialog)
        dlg.title("Edit Overlay" if existing else "Add Overlay")
        dlg.geometry("460x480")
        dlg.configure(bg=c["bg"])
        dlg.grab_set()
        dlg.resizable(False, False)

        kind_var    = tk.StringVar(value=existing.get("kind", "text") if existing else "text")
        text_var    = tk.StringVar(value=existing.get("text", "Sample text") if existing else "Sample text")
        font_size_v = tk.IntVar(value=existing.get("font_size", 24) if existing else 24)
        color_var   = tk.StringVar(value=existing.get("color", "#ffffff") if existing else "#ffffff")
        opacity_v   = tk.DoubleVar(value=existing.get("opacity", 1.0) if existing else 1.0)
        x_pct_v     = tk.DoubleVar(value=existing.get("x", 0.05) if existing else 0.05)
        y_pct_v     = tk.DoubleVar(value=existing.get("y", 0.05) if existing else 0.05)
        w_pct_v     = tk.DoubleVar(value=existing.get("w", 0.2)  if existing else 0.2)
        h_pct_v     = tk.DoubleVar(value=existing.get("h", 0.1)  if existing else 0.1)
        path_var    = tk.StringVar(value=existing.get("path", "") if existing else "")
        cam_idx_v   = tk.IntVar(value=existing.get("cam_index", 0) if existing else 0)

        tk.Label(dlg, text="Type:", bg=c["bg"], fg=c["text"], font=("Segoe UI", 10)).grid(row=0, column=0, sticky="w", padx=16, pady=(14,4))
        kind_frame = tk.Frame(dlg, bg=c["bg"]); kind_frame.grid(row=0, column=1, sticky="w", padx=8, pady=(14,4))
        for kv, kl in [("text","Text"), ("webcam","Webcam"), ("image","Image")]:
            tk.Radiobutton(kind_frame, text=kl, variable=kind_var, value=kv,
                           bg=c["bg"], fg=c["text"], selectcolor=c["surface"],
                           activebackground=c["bg"]).pack(side="left", padx=6)

        def _row_lbl(r, label, widget_col):
            tk.Label(dlg, text=label, bg=c["bg"], fg=c["text"], font=("Segoe UI", 10),
                     anchor="w", width=12).grid(row=r, column=0, sticky="w", padx=16, pady=4)
            widget_col.grid(row=r, column=1, sticky="ew", padx=(8, 16), pady=4)
            dlg.columnconfigure(1, weight=1)

        r = 1
        text_entry = tk.Entry(dlg, textvariable=text_var, bg=c["surface"], fg=c["text"], font=("Segoe UI", 10), relief="flat")
        _row_lbl(r, "Text:", text_entry); r+=1

        font_scale = tk.Scale(dlg, variable=font_size_v, from_=8, to=96, orient="horizontal",
                              bg=c["bg"], fg=c["text"], troughcolor=c["surface"], highlightthickness=0)
        _row_lbl(r, "Font size:", font_scale); r+=1

        color_frame = tk.Frame(dlg, bg=c["bg"]); _row_lbl(r, "Color:", color_frame); r+=1
        color_preview = tk.Label(color_frame, bg=color_var.get(), width=4, relief="flat")
        color_preview.pack(side="left")
        def pick_color():
            from tkinter import colorchooser
            col = colorchooser.askcolor(color=color_var.get(), parent=dlg)
            if col[1]:
                color_var.set(col[1]); color_preview.config(bg=col[1])
        tk.Button(color_frame, text=color_var.get(), textvariable=color_var,
                  command=pick_color, bg=c["surface"], fg=c["text"],
                  font=("Consolas", 9), relief="flat", padx=8).pack(side="left", padx=6)

        path_frame = tk.Frame(dlg, bg=c["bg"]); _row_lbl(r, "Image path:", path_frame); r+=1
        tk.Entry(path_frame, textvariable=path_var, bg=c["surface"], fg=c["text"],
                 font=("Segoe UI", 9), relief="flat", width=26).pack(side="left")
        tk.Button(path_frame, text="…", command=lambda: path_var.set(
            filedialog.askopenfilename(filetypes=[("Images","*.png *.jpg *.jpeg *.bmp *.gif"),("All","*.*")]) or path_var.get()),
                  bg=c["surface"], fg=c["text"], font=("Segoe UI", 9), relief="flat", padx=6).pack(side="left", padx=4)

        cam_frame = tk.Frame(dlg, bg=c["bg"]); _row_lbl(r, "Camera index:", cam_frame); r+=1
        tk.Spinbox(cam_frame, textvariable=cam_idx_v, from_=0, to=9, width=5,
                   bg=c["surface"], fg=c["text"], relief="flat").pack(side="left")

        tk.Label(dlg, text="Opacity:", bg=c["bg"], fg=c["text"], font=("Segoe UI", 10)).grid(row=r, column=0, sticky="w", padx=16, pady=4)
        op_frame = tk.Frame(dlg, bg=c["bg"]); op_frame.grid(row=r, column=1, sticky="ew", padx=8, pady=4); r+=1
        tk.Scale(op_frame, variable=opacity_v, from_=0.0, to=1.0, resolution=0.05,
                 orient="horizontal", length=200, bg=c["bg"], fg=c["text"],
                 troughcolor=c["surface"], highlightthickness=0).pack(side="left")

        tk.Label(dlg, text="Position (%):", bg=c["bg"], fg=c["text"], font=("Segoe UI", 10)).grid(row=r, column=0, sticky="w", padx=16, pady=(8,2))
        pos_frame = tk.Frame(dlg, bg=c["bg"]); pos_frame.grid(row=r, column=1, sticky="ew", padx=8, pady=(8,2)); r+=1
        tk.Label(pos_frame, text="X:", bg=c["bg"], fg=c["text"], font=("Segoe UI", 9)).pack(side="left")
        tk.Scale(pos_frame, variable=x_pct_v, from_=0.0, to=0.95, resolution=0.01,
                 orient="horizontal", length=100, bg=c["bg"], fg=c["text"],
                 troughcolor=c["surface"], highlightthickness=0).pack(side="left")
        tk.Label(pos_frame, text=" Y:", bg=c["bg"], fg=c["text"], font=("Segoe UI", 9)).pack(side="left")
        tk.Scale(pos_frame, variable=y_pct_v, from_=0.0, to=0.95, resolution=0.01,
                 orient="horizontal", length=100, bg=c["bg"], fg=c["text"],
                 troughcolor=c["surface"], highlightthickness=0).pack(side="left")

        tk.Label(dlg, text="Size (%):", bg=c["bg"], fg=c["text"], font=("Segoe UI", 10)).grid(row=r, column=0, sticky="w", padx=16, pady=2)
        sz_frame = tk.Frame(dlg, bg=c["bg"]); sz_frame.grid(row=r, column=1, sticky="ew", padx=8, pady=2); r+=1
        tk.Label(sz_frame, text="W:", bg=c["bg"], fg=c["text"], font=("Segoe UI", 9)).pack(side="left")
        tk.Scale(sz_frame, variable=w_pct_v, from_=0.02, to=1.0, resolution=0.01,
                 orient="horizontal", length=100, bg=c["bg"], fg=c["text"],
                 troughcolor=c["surface"], highlightthickness=0).pack(side="left")
        tk.Label(sz_frame, text=" H:", bg=c["bg"], fg=c["text"], font=("Segoe UI", 9)).pack(side="left")
        tk.Scale(sz_frame, variable=h_pct_v, from_=0.02, to=1.0, resolution=0.01,
                 orient="horizontal", length=100, bg=c["bg"], fg=c["text"],
                 troughcolor=c["surface"], highlightthickness=0).pack(side="left")

        hint_lbl = tk.Label(dlg, text="", bg=c["bg"], fg=c["text_secondary"], font=("Segoe UI", 8, "italic"))
        hint_lbl.grid(row=r, column=0, columnspan=2, padx=16, pady=(4,0)); r+=1

        def _update_fields(*_):
            k = kind_var.get()
            text_entry.config(state="normal" if k=="text" else "disabled")
            font_scale.config(state="normal" if k=="text" else "disabled")

        kind_var.trace_add("write", _update_fields)
        _update_fields()

        btn_row = tk.Frame(dlg, bg=c["bg"]); btn_row.grid(row=r, column=0, columnspan=2, pady=(12,8), padx=16, sticky="e")
        def _save():
            ov = {
                "kind": kind_var.get(),
                "text": text_var.get(),
                "font_size": font_size_v.get(),
                "color": color_var.get(),
                "opacity": round(opacity_v.get(), 2),
                "x": round(x_pct_v.get(), 3),
                "y": round(y_pct_v.get(), 3),
                "w": round(w_pct_v.get(), 3),
                "h": round(h_pct_v.get(), 3),
                "path": path_var.get(),
                "cam_index": cam_idx_v.get(),
                "enabled": True,
            }
            if idx is None:
                self._overlays.append(ov)
            else:
                self._overlays[idx] = ov
            self._refresh_overlay_list()
            dlg.destroy()

        tk.Button(btn_row, text="Save", command=_save,
                  bg=c["success"], fg=c["bg"], font=("Segoe UI", 10, "bold"),
                  relief="flat", padx=16, pady=6).pack(side="right", padx=(6,0))
        tk.Button(btn_row, text="Cancel", command=dlg.destroy,
                  bg=c["surface"], fg=c["text"], font=("Segoe UI", 10),
                  relief="flat", padx=12, pady=6).pack(side="right")

    def _overlay_preview(self) -> None:
        OverlayPreviewDialog(self.dialog, self.app, self._overlays, self._on_overlays_updated)

    def _on_overlays_updated(self, overlays: list) -> None:
        self._overlays = overlays
        self._refresh_overlay_list()


    def _row(self, parent, label: str, widget) -> None:
        row = parent.grid_size()[1]
        tk.Label(parent, text=label, bg=self.app.colors["bg"], fg=self.app.colors["text"], font=("Segoe UI", 10), anchor="w").grid(row=row, column=0, sticky="w", padx=(20, 8), pady=6)
        widget.grid(row=row, column=1, sticky="w", padx=(0, 20), pady=6)
        parent.columnconfigure(1, weight=1)

    def _start_key_capture(self, var: tk.StringVar, entry: tk.Entry) -> None:
        entry.config(state="normal")
        _prev_value = var.get()
        var.set("Press a key...")
        _captured = [False]

        def on_key(event):
            parts = []
            if event.state & 0x4: parts.append("Control")
            if event.state & 0x1: parts.append("Shift")
            if event.state & 0x8: parts.append("Alt")
            key = event.keysym
            if key not in ("Control_L","Control_R","Shift_L","Shift_R","Alt_L","Alt_R"):
                parts.append(key)
            if parts:
                hotkey = "+".join(parts)
                if " " not in hotkey and hotkey != "Press a key...":
                    var.set(hotkey); _captured[0] = True
            entry.config(state="readonly"); entry.unbind("<KeyPress>")

        def on_focusout(event):
            if not _captured[0]: var.set(_prev_value)
            entry.config(state="readonly"); entry.unbind("<KeyPress>")

        entry.bind("<KeyPress>", on_key)
        entry.bind("<FocusOut>", on_focusout, add="+")

    def _delete_asset(self, name: str, kind: str, combo: ttk.Combobox) -> None:
        if not name:
            messagebox.showwarning("Nothing selected", f"Select a {kind} to delete."); return
        if not messagebox.askyesno("Confirm delete", f"Delete {kind} '{name}'?\nThis cannot be undone."): return
        base = os.path.dirname(os.path.abspath(__file__))
        path = os.path.join(base, LANGS_DIR, f"{name}.hrl")
        try:
            if os.path.exists(path):
                os.remove(path); log.info(f"Deleted {kind}: {path}")
                messagebox.showinfo("Deleted", f"{kind.capitalize()} '{name}' deleted.")
                combo.config(values=[c for c, _ in self.app._scan_custom_languages()])
                combo.set("")
            else:
                messagebox.showerror("Not found", f"File not found:\n{path}")
        except Exception as e:
            messagebox.showerror("Delete failed", str(e))

    def _collect(self) -> dict:
        return {
            "hrc_version": self.HRC_VERSION,
            "video_codec": self._cv.get(), "hw_accel": self._hwv.get(),
            "enc_preset": self._prev.get(), "enc_crf": self._crfv.get(),
            "pix_fmt": self._pixv.get(), "audio_sample_rate": int(self._srv.get()),
            "audio_aac_bitrate": self._abrv.get(), "audio_out_channels": int(self._achv.get()),
            "ui_theme": self._thv.get(), "ui_scale": int(self._uisv.get().replace("%", "")) / 100,
            "ui_font": self._fontv.get(), "filename_template": self._ftv.get(),
            "auto_stop_min": int(self._asv.get() or 0), "replay_buffer_sec": int(self._rbv.get() or 0),
            "separate_audio_mp3": self._sep_mp3v.get(),
            "overlays": self._overlays,
            "hotkey_start_stop": self._hk_ss.get(), "hotkey_pause": self._hk_p.get(),
            "hotkey_fullscreen": self._hk_fs.get(), "notify_sound": self._notif_sound.get(),
            "notify_flash": self._notif_flash.get(), "auto_save_profile": self._auto_save.get(),
        }

    def _save(self) -> None:
        data = self._collect(); a = self.app
        for k, v in data.items():
            if k != "hrc_version": setattr(a, k, v)
        a.separate_audio_mp3 = data.get("separate_audio_mp3", False)
        if hasattr(a, '_apply_hotkeys'): a._apply_hotkeys()
        if hasattr(a, 'apply_theme'):
            a.colors = a.get_theme_colors(data["ui_theme"]); a.apply_theme()
        a.save_settings(silent=True)
        log.info(f"Advanced settings saved: {data}")
        self.dialog.destroy()

    def _export(self) -> None:
        path = filedialog.asksaveasfilename(defaultextension=".hrc", filetypes=[("HomRec Profile", "*.hrc"), ("All files", "*.*")], initialfile="homrec_profile.hrc", title="Export profile")
        if not path: return
        try:
            _hrc_write(path, self._collect(), _HRC_MAGIC)
            messagebox.showinfo("Exported", f"Profile saved to:\n{path}")
            log.info(f"Profile exported: {path}")
        except Exception as e:
            messagebox.showerror("Export failed", str(e))

    def _import(self) -> None:
        path = filedialog.askopenfilename(filetypes=[("HomRec Profile", "*.hrc"), ("All files", "*.*")], title="Import profile")
        if not path: return
        try:
            data = _hrc_read(path, _HRC_MAGIC)
            self._cv.set(data.get("video_codec", "libx264"))
            self._hwv.set(data.get("hw_accel", "auto"))
            self._prev.set(data.get("enc_preset", "ultrafast"))
            self._crfv.set(data.get("enc_crf", 18))
            self._pixv.set(data.get("pix_fmt", "yuv420p"))
            self._srv.set(str(data.get("audio_sample_rate", 44100)))
            self._abrv.set(data.get("audio_aac_bitrate", "192k"))
            self._achv.set(str(data.get("audio_out_channels", 2)))
            self._thv.set(data.get("ui_theme", "dark"))
            self._uisv.set(str(int(data.get("ui_scale", 1.0)*100)) + "%")
            self._fontv.set(data.get("ui_font", "Segoe UI"))
            self._ftv.set(data.get("filename_template", "HomRec_{date}_{time}"))
            self._asv.set(str(data.get("auto_stop_min", 0)))
            self._rbv.set(str(data.get("replay_buffer_sec", 0)))
            self._hk_ss.set(data.get("hotkey_start_stop", "F9"))
            self._hk_p.set(data.get("hotkey_pause", "F10"))
            self._hk_fs.set(data.get("hotkey_fullscreen", "F11"))
            self._notif_sound.set(data.get("notify_sound", True))
            self._notif_flash.set(data.get("notify_flash", True))
            self._auto_save.set(data.get("auto_save_profile", False))
            messagebox.showinfo("Imported", f"Profile loaded from:\n{path}")
            log.info(f"Profile imported: {path}")
        except Exception as e:
            messagebox.showerror("Import failed", str(e))


class SettingsDialog:
    def __init__(self, parent, app) -> None:
        self.app = app
        self.dialog = tk.Toplevel(parent)
        self.dialog.title(app.lang["settings_title"])
        self.dialog.geometry("560x560")
        self.dialog.configure(bg=app.colors["bg"])
        self.dialog.transient(parent); self.dialog.grab_set(); self.dialog.resizable(False, False)
        self.dialog.update_idletasks()
        self.dialog.geometry(f"+{self.dialog.winfo_screenwidth()//2-280}+{self.dialog.winfo_screenheight()//2-280}")
        try:
            base_dir = os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) else os.path.dirname(os.path.abspath(__file__))
            ico_path = os.path.join(base_dir, "icons", "main.ico")
            if os.path.exists(ico_path): self.dialog.iconbitmap(ico_path)
        except Exception: pass
        self.create_widgets()

    def create_widgets(self) -> None:
        a = self.app; c = a.colors
        notebook = ttk.Notebook(self.dialog)
        notebook.pack(fill="both", expand=True, padx=10, pady=10)

        video_tab = ttk.Frame(notebook); notebook.add(video_tab, text=a.lang["video_settings"])
        video_inner = tk.Frame(video_tab, bg=c["bg"])
        video_inner.pack(fill="both", expand=True, padx=15, pady=15)

        quality_frame = tk.Frame(video_inner, bg=c["bg"])
        quality_frame.pack(fill="x", pady=10)
        tk.Label(quality_frame, text=a.lang["quality"], bg=c["bg"], fg=c["text"], font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.quality_var = tk.StringVar(value=str(a.quality))
        tk.Scale(quality_frame, from_=10, to=100, orient="horizontal", length=250, variable=self.quality_var, command=self.update_quality, bg=c["surface"], fg=c["text"], highlightthickness=0, troughcolor=c["surface_light"]).pack(side="left", padx=5)
        tk.Label(quality_frame, text="%", bg=c["bg"], fg=c["text_secondary"], font=("Segoe UI", 10)).pack(side="left")

        res_frame = tk.Frame(video_inner, bg=c["bg"])
        res_frame.pack(fill="x", pady=10)
        tk.Label(res_frame, text=a.lang["resolution"], bg=c["bg"], fg=c["text"], font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.scale_var = tk.StringVar(value=str(int(a.scale_factor * 100)))
        tk.Scale(res_frame, from_=25, to=100, orient="horizontal", length=250, variable=self.scale_var, command=self.update_scale, bg=c["surface"], fg=c["text"], highlightthickness=0, troughcolor=c["surface_light"]).pack(side="left", padx=5)
        tk.Label(res_frame, text="%", bg=c["bg"], fg=c["text_secondary"], font=("Segoe UI", 10)).pack(side="left")

        fps_frame = tk.Frame(video_inner, bg=c["bg"])
        fps_frame.pack(fill="x", pady=10)
        tk.Label(fps_frame, text="FPS:", bg=c["bg"], fg=c["text"], font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.fps_slider_var = tk.IntVar(value=a.target_fps)
        tk.Scale(fps_frame, from_=1, to=60, orient="horizontal", length=200, variable=self.fps_slider_var, bg=c["surface"], fg=c["text"], highlightthickness=0, troughcolor=c["surface_light"], command=self._on_fps_change).pack(side="left", padx=5)
        self._fps_val_label = tk.Label(fps_frame, text=f"{a.target_fps} fps", bg=c["bg"], fg=c["accent"], font=("Segoe UI", 10, "bold"), width=7)
        self._fps_val_label.pack(side="left")

        fmt_frame = tk.Frame(video_inner, bg=c["bg"])
        fmt_frame.pack(fill="x", pady=10)
        tk.Label(fmt_frame, text="Format:", bg=c["bg"], fg=c["text"], font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.video_format_var = tk.StringVar(value=getattr(a, "video_format", "mp4"))
        for fmt_val, fmt_lbl in [("mp4", ".mp4"), ("mkv", ".mkv")]:
            tk.Radiobutton(fmt_frame, text=fmt_lbl, variable=self.video_format_var, value=fmt_val,
                           bg=c["bg"], fg=c["text"], selectcolor=c["surface"], font=("Segoe UI", 10),
                           activebackground=c["bg"], activeforeground=c["accent"]).pack(side="left", padx=8)
        tk.Label(video_inner, text="Codec and HW Accel settings are in ⚙ Advanced tab.", bg=c["bg"], fg=c["text_secondary"], font=("Segoe UI", 9, "italic")).pack(anchor="w", pady=(8, 0))


        adv_tab = ttk.Frame(notebook); notebook.add(adv_tab, text=a.lang["advanced"])
        adv_inner = tk.Frame(adv_tab, bg=c["bg"])
        adv_inner.pack(fill="both", expand=True, padx=15, pady=15)

        mon_frame = tk.Frame(adv_inner, bg=c["bg"])
        mon_frame.pack(fill="x", pady=10)
        tk.Label(mon_frame, text=a.lang["monitor"], bg=c["bg"], fg=c["text"], font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.monitor_var = tk.StringVar(value=str(a.monitor_id))
        monitor_combo = ttk.Combobox(mon_frame, textvariable=self.monitor_var, values=[str(i) for i in range(1, len(a.sct.monitors))], width=10, state="readonly", font=("Segoe UI", 10))
        monitor_combo.pack(side="left", padx=5)
        monitor_combo.bind("<<ComboboxSelected>>", self.on_monitor_change)

        folder_frame = tk.Frame(adv_inner, bg=c["bg"])
        folder_frame.pack(fill="x", pady=10)
        tk.Label(folder_frame, text=a.lang["output"], bg=c["bg"], fg=c["text"], font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.folder_label = tk.Label(folder_frame, text=os.path.basename(a.output_folder), bg=c["surface"], fg=c["accent"], font=("Consolas", 10), relief="flat", padx=8, pady=4)
        self.folder_label.pack(side="left", padx=5)
        tk.Button(folder_frame, text=a.lang["browse"], command=self.select_folder, bg=c["surface"], fg=c["text"], font=("Segoe UI", 10), relief="flat", padx=12).pack(side="left", padx=5)

        features_frame = tk.Frame(adv_inner, bg=c["bg"])
        features_frame.pack(fill="x", pady=10)
        self.countdown_var = tk.BooleanVar(value=a.countdown_var.get())
        self.timestamp_var = tk.BooleanVar(value=a.timestamp_var.get())
        self.cursor_var = tk.BooleanVar(value=a.cursor_var.get())
        self.show_summary_var = tk.BooleanVar(value=a.show_summary)
        self.minimize_tray_var = tk.BooleanVar(value=a.minimize_to_tray.get())
        for text, var in [(a.lang["countdown"], self.countdown_var), (a.lang["timestamp"], self.timestamp_var), (a.lang["cursor"], self.cursor_var), (a.lang["notification"], self.show_summary_var), (a.lang["minimize_tray"], self.minimize_tray_var)]:
            tk.Checkbutton(features_frame, text=text, variable=var, bg=c["bg"], fg=c["text"], selectcolor=c["surface"], font=("Segoe UI", 10)).pack(anchor="w", pady=2)

        tk.Frame(adv_inner, bg=c["surface"], height=1).pack(fill="x", pady=(10, 6))
        tk.Label(adv_inner, text="⚡ Performance", bg=c["bg"], fg=c["accent"], font=("Segoe UI", 10, "bold")).pack(anchor="w", pady=(0, 4))
        self.disable_preview_var = tk.BooleanVar(value=getattr(a, "disable_preview", False))
        tk.Checkbutton(adv_inner, text="Disable Preview  (shows HomRec logo, saves CPU)", variable=self.disable_preview_var, bg=c["bg"], fg=c["text"], selectcolor=c["surface"], font=("Segoe UI", 10), command=self._toggle_preview_hint).pack(anchor="w", pady=2)
        self._dp_hint = tk.Label(adv_inner, text="Preview will be replaced with a blue HomRec screen.", bg=c["bg"], fg=c["text_secondary"], font=("Segoe UI", 8, "italic"))
        self._dp_hint.pack(anchor="w", padx=(20, 0))
        self._toggle_preview_hint()

        advsettings_tab = ttk.Frame(notebook); notebook.add(advsettings_tab, text="⚙ Advanced")
        advsettings_inner = tk.Frame(advsettings_tab, bg=c["bg"])
        advsettings_inner.pack(fill="both", expand=True, padx=15, pady=15)
        tk.Label(advsettings_inner, text="Full customization for power users.", bg=c["bg"], fg=c["text_secondary"], font=("Segoe UI", 9)).pack(anchor="w", pady=(0, 12))
        tk.Button(advsettings_inner, text="Open Advanced Settings →", command=lambda: AdvancedSettingsDialog(self.dialog, self.app), bg=c["accent"], fg=c["bg"], font=("Segoe UI", 11, "bold"), relief="flat", padx=20, pady=10).pack(anchor="w")
        tk.Label(advsettings_inner, text="Codec · HW Accel · CRF · Preset · Audio bitrate\nTheme · UI scale · Font · Auto-stop · Replay buffer\nImport / Export profile (.hrc)", bg=c["bg"], fg=c["text_secondary"], font=("Segoe UI", 9), justify="left").pack(anchor="w", pady=(12, 0))

        btn_frame = tk.Frame(self.dialog, bg=c["bg"])
        btn_frame.pack(fill="x", padx=10, pady=10)
        tk.Button(btn_frame, text=a.lang["save"], command=self.save_settings, bg=a.colors["success"], fg=a.colors["bg"], font=("Segoe UI", 10, "bold"), relief="flat", padx=20, pady=8).pack(side="right", padx=5)
        tk.Button(btn_frame, text=a.lang["cancel"], command=self.dialog.destroy, bg=c["surface"], fg=c["text"], font=("Segoe UI", 10), relief="flat", padx=20, pady=8).pack(side="right", padx=5)

    def update_quality(self, event=None) -> None: pass
    def update_scale(self, event=None) -> None: pass
    def _on_fps_change(self, val=None) -> None:
        self._fps_val_label.config(text=f"{self.fps_slider_var.get()} fps")
    def on_mode_change(self, event=None) -> None: pass
    def on_monitor_change(self, event=None) -> None: pass

    def _toggle_preview_hint(self) -> None:
        if hasattr(self, '_dp_hint'):
            self._dp_hint.config(fg=self.app.colors.get("warning" if self.disable_preview_var.get() else "text_secondary", "#a6adc8"))

    def select_folder(self) -> None:
        folder = filedialog.askdirectory(initialdir=self.app.output_folder)
        if folder:
            self.app.output_folder = folder
            self.folder_label.config(text=os.path.basename(folder))

    def save_settings(self) -> None:
        self.app.quality = int(self.quality_var.get())
        new_fps = int(self.fps_slider_var.get())
        self.app.target_fps = new_fps
        self.app.recording_mode = "ultra" if new_fps >= 60 else "turbo" if new_fps >= 30 else "balanced" if new_fps >= 15 else "eco"
        self.app.scale_factor = int(self.scale_var.get()) / 100
        self.app.monitor_id = int(self.monitor_var.get())
        self.app.update_monitor_info()
        self.app.video_format = self.video_format_var.get()
        self.app.countdown_var.set(self.countdown_var.get())
        self.app.timestamp_var.set(self.timestamp_var.get())
        self.app.cursor_var.set(self.cursor_var.get())
        self.app.show_summary = self.show_summary_var.get()
        self.app.minimize_to_tray.set(self.minimize_tray_var.get())
        if hasattr(self, 'disable_preview_var'):
            self.app.disable_preview = self.disable_preview_var.get()
        if hasattr(self, 'codec_var'): self.app.video_codec = self.codec_var.get()
        if hasattr(self, 'hw_var'): self.app.hw_accel = self.hw_var.get()
        self.app.res_label.config(text=f"{self.app.lang['resolution']} {self.app.record_width}x{self.app.record_height}")
        self.app.save_settings(silent=True)
        self.dialog.destroy()
        messagebox.showinfo(self.app.lang["info"], self.app.lang["settings_saved"])


from hr_console_bridge import NativeConsole


class HomRecScreen:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.current_language = "en"
        self.lang = self._load_language(self.current_language)
        self.root.title(self.lang["app_title"])
        self.root.geometry("1300x750")
        self.root.minsize(1200, 650)
        optimize_for_performance()
        self.set_app_icon()
        self.current_theme = "dark"
        self.colors = self.get_theme_colors("dark")
        self.apply_theme()
        self.sct = mss.mss()
        # C++ pipeline — заменяет mss+_capture_loop при наличии hr_pipeline.dll
        self.cpp_pipeline = None
        self._cpp_pipe_read_fd  = -1   # читающий конец pipe → ffmpeg stdin
        self._cpp_pipe_write_fd = -1   # пишущий конец pipe → C++ pipeline
        self._preview_queue = queue.Queue(maxsize=1)
        self._preview_running = True
        self.audio_recording = False
        self.audio_thread = None
        self.audio_frames = []
        self.audio_stream = None
        self.audio_p = None
        self.audio_channels = 1
        self.sys_audio_recording = False
        self.sys_audio_thread = None
        self.sys_audio_frames = []
        self.sys_audio_stream = None
        self.sys_audio_p = None
        self.sys_audio_filename = None
        self.sys_ffmpeg_proc = None
        self.ffmpeg_proc = None
        self.ffmpeg_reader_thread = None
        self.stop_ffmpeg_reader = False
        self.scale_factor = 0.75
        self.output_folder = os.path.join(_ROOT_DIR, "recordings")
        self.quality = 70
        self.target_fps = 15
        self.recording_mode = "balanced"
        self.show_summary = True
        self.hotkey_start_stop = "F9"
        self.hotkey_pause = "F10"
        self.hotkey_fullscreen = "F11"
        self.notify_sound = True
        self.notify_flash = True
        self.auto_save_profile = False
        self.video_codec = "libx264"
        self.hw_accel = "auto"
        self.enc_preset = "ultrafast"
        self.enc_crf = 18
        self.pix_fmt = "yuv420p"
        self.audio_sample_rate = 44100
        self.audio_aac_bitrate = "192k"
        self.audio_out_channels = 2
        self.ui_theme = "dark"
        self.ui_scale = 1.0
        self.ui_font = "Segoe UI"
        self.filename_template = "HomRec_{date}_{time}"
        self.auto_stop_min = 0
        self.replay_buffer_sec = 0
        self.video_format = "mp4"       # "mp4" or "mkv"
        self.separate_audio_mp3 = False  # save audio as separate .mp3
        self.core_version = CURRENT_VERSION
        self.ui_registry: dict = {}      # logical name -> widget, built by _build_ui_registry()
        self._hide_geo_cache: dict = {}  # !hide bookkeeping (geometry-manager info per hidden widget)
        self.overlays: list[dict] = []   # overlay definitions
        self.overlays_panel = None       # set once the OverlaysDockPanel is built
        self.show_audio_panel = True     # Audio Mixer panel visible (View -> Show)
        self.show_overlays_panel = True  # Overlays dock panel visible (View -> Show)
        self.always_on_top = tk.BooleanVar(value=False)
        self.minimize_to_tray = tk.BooleanVar(value=True)
        self.language_var = tk.StringVar(value="en")
        self.theme_var = tk.StringVar(value="dark")
        self.countdown_var = tk.BooleanVar(value=True)
        self.timestamp_var = tk.BooleanVar(value=False)
        self.cursor_var = tk.BooleanVar(value=False)
        self.preview_width = 900
        self.preview_height = 500
        self.load_settings()
        self.recording = False
        self.paused = False
        self.out = None
        self.frame_count = 0
        self.start_time = 0.0
        self.recording_thread = None
        self.stop_flag = False
        self.last_frame_time = 0.0
        self.monitor_id = 1
        self.monitor_left = 0
        self.monitor_top = 0
        self.update_monitor_info()
        self.capture_mode = "desktop"
        self.capture_window_title = ""
        self.tray_icon = None
        os.makedirs(self.output_folder, exist_ok=True)
        self.ffmpeg_path = find_ffmpeg()
        if self.ffmpeg_path:
            log.info(f"FFmpeg found: {self.ffmpeg_path}")
        else:
            log.warning("FFmpeg NOT found!")
        self.create_menu()
        self.create_widgets()
        self._capture_thread = threading.Thread(target=self._capture_loop, daemon=True)
        self._capture_thread.start()
        self.update_preview()
        self.root.after(3000, self._warm_up_gpu_probe)
        self._console = NativeConsole(self)
        self.root.bind("<Control-Shift-T>", lambda e: self._console.toggle())
        self.root.bind("<Control-Shift-t>", lambda e: self._console.toggle())
        self.root.bind('<Configure>', self.on_window_resize)
        self._apply_hotkeys()
        self._setup_drag_drop()
        self._register_file_types()
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)
        self.setup_tray()
        log.info("HomRec v1.7.0 started, language: %s", self.current_language)
        self.root.after(2000, self._start_update_check)
        if getattr(self, '_first_launch', False):
            self.root.after(400, self._show_welcome_and_save)

    def update_ui_language(self) -> None:
        self.root.title(self.lang["app_title"]); self.recreate_widgets()

    def check_ffmpeg(self) -> bool:
        return self.ffmpeg_path is not None

    def get_dshow_audio_devices(self) -> list[str]:
        if not self.ffmpeg_path: return []
        try:
            from homrec_native import tools_engine as _te, TOOLS_OK as _TOK
            if _TOK and _te: return _te.get_dshow_devices(self.ffmpeg_path)
        except Exception: pass
        return []

    def merge_audio_video(self, video_file: str, audio_file: str) -> bool:
        log.info(f"merge_audio_video: video={video_file!r} audio={audio_file!r}")
        if not audio_file or not os.path.exists(audio_file):
            log.warning(f"merge_audio_video: audio missing: {audio_file!r}"); return False
        if not os.path.exists(video_file):
            log.warning(f"merge_audio_video: video missing: {video_file!r}"); return False
        if not self.ffmpeg_path:
            log.warning("merge_audio_video: no ffmpeg path"); return False
        try:
            from homrec_native import tools_engine as _te, TOOLS_OK as _TOK
            if _TOK and _te:
                ok = _te.merge_av(self.ffmpeg_path, video_file, audio_file)
                if ok:
                    log.info(f"merge_audio_video: C++ success → {video_file}"); return True
        except Exception as e:
            log.warning(f"merge_audio_video: C++ path error: {e}")
        ext = os.path.splitext(video_file)[1] or '.mp4'
        tmp = video_file.replace(ext, f'_merge_tmp{ext}')
        try:
            cmd = [self.ffmpeg_path, '-i', video_file, '-i', audio_file, '-c:v', 'copy', '-c:a', 'aac', '-af', 'aresample=async=1000', '-map', '0:v:0', '-map', '1:a:0', '-shortest', '-y', tmp]
            result = subprocess.run(cmd, capture_output=True, timeout=120, creationflags=subprocess.CREATE_NO_WINDOW if platform.system()=='Windows' else 0)
            if result.returncode == 0 and os.path.exists(tmp):
                os.remove(video_file); os.remove(audio_file); os.rename(tmp, video_file)
                log.info(f"merge_audio_video: fallback success → {video_file}"); return True
        except Exception as e:
            log.warning(f"merge_audio_video: fallback exception: {e}")
        return False

    def set_app_icon(self) -> None:
        self._icons_dir = os.path.join(_ROOT_DIR, "icons")
        self._main_ico = os.path.join(self._icons_dir, "main.ico")
        self._rec_ico  = os.path.join(self._icons_dir, "rec.ico")
        try:
            self.root.iconbitmap(self._main_ico)
        except:
            try:
                icon_image = Image.new('RGBA', (64, 64), (0, 0, 0, 0))
                draw = ImageDraw.Draw(icon_image)
                draw.rectangle([10, 20, 54, 44], fill="#89b4fa", outline="#cdd6f4", width=2)
                draw.ellipse([25, 25, 39, 39], fill="#1e1e2e", outline="#cdd6f4", width=2)
                draw.ellipse([29, 29, 35, 35], fill="#89b4fa")
                self.root.iconphoto(True, ImageTk.PhotoImage(icon_image))
            except: pass
        if sys.platform == "win32":
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("homrec.1.7.0")
        self._rec_icon_img = None
        self._rec_frames = self._make_rec_frames()
        self._rec_frame_idx = 0

    def _set_taskbar_icon(self, recording: bool) -> None:
        try:
            self.root.iconbitmap(self._rec_ico if recording and os.path.exists(self._rec_ico) else self._main_ico)
        except Exception as e:
            log.debug(f"Taskbar icon switch failed: {e}")

    def on_window_resize(self, event: tk.Event) -> None:
        if event.widget == self.root: self.update_preview_size()

    def update_preview_size(self) -> None:
        try:
            max_w = max(600, min(self.root.winfo_width() - 320, 1280))
            max_h = max(350, min(self.root.winfo_height() - 240, 720))
            src_w = getattr(self, 'original_width', 0) or 1
            src_h = getattr(self, 'original_height', 0) or 1
            src_ratio = src_w / src_h
            if max_w / max_h > src_ratio:
                ph = max_h; pw = max(1, int(round(ph * src_ratio)))
            else:
                pw = max_w; ph = max(1, int(round(pw / src_ratio)))
            self.preview_width = pw; self.preview_height = ph
        except: pass

    BUILTIN_THEMES = {
        "dark": {"bg":"#1e1e2e","fg":"#cdd6f4","accent":"#89b4fa","success":"#a6e3a1","warning":"#f9e2af","error":"#f38ba8","surface":"#313244","surface_light":"#45475a","preview_bg":"#11111b","text":"#cdd6f4","text_secondary":"#a6adc8"},
        "light": {"bg":"#f5f5f5","fg":"#2c3e50","accent":"#3498db","success":"#27ae60","warning":"#f39c12","error":"#e74c3c","surface":"#ecf0f1","surface_light":"#bdc3c7","preview_bg":"#ffffff","text":"#2c3e50","text_secondary":"#7f8c8d"},
    }

    def get_theme_colors(self, theme: str) -> dict:
        if theme in self.BUILTIN_THEMES: return self.BUILTIN_THEMES[theme]
        return self.BUILTIN_THEMES["dark"]

    def _load_language(self, lang_code: str) -> dict:
        if lang_code in LANGUAGES: return dict(LANGUAGES[lang_code])
        hrl_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), LANGS_DIR, f"{lang_code}.hrl")
        if os.path.exists(hrl_path):
            try:
                data = _hrc_read(hrl_path, _HRL_MAGIC)
                result = dict(LANGUAGES["en"]); result.update(data)
                missing = [k for k in LANG_REQUIRED_KEYS if k not in data]
                if missing: log.warning(f"Language {lang_code}: {len(missing)} missing keys")
                return result
            except Exception as e:
                log.warning(f"Failed to load language {hrl_path}: {e}")
        return dict(LANGUAGES["en"])

    def _scan_custom_languages(self) -> list:
        langs_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), LANGS_DIR)
        result = []; os.makedirs(langs_dir, exist_ok=True)
        for fname in os.listdir(langs_dir):
            if fname.endswith(".hrl"):
                code = fname[:-4]
                try:
                    data = _hrc_read(os.path.join(langs_dir, fname), _HRL_MAGIC)
                    result.append((code, data.get("lang_name", code)))
                except Exception as e:
                    log.warning(f"Failed to scan language {fname}: {e}")
        return result


    def apply_theme(self) -> None:
        self.root.configure(bg=self.colors["bg"])
        style = ttk.Style(); style.theme_use('clam')
        style.configure("TFrame", background=self.colors["bg"])
        style.configure("TLabel", background=self.colors["bg"], foreground=self.colors["fg"])
        style.configure("TLabelframe", background=self.colors["bg"], foreground=self.colors["accent"])
        style.configure("TLabelframe.Label", background=self.colors["bg"], foreground=self.colors["accent"], font=("Segoe UI", 11, "bold"))
        style.configure("TButton", background=self.colors["surface"], foreground=self.colors["fg"])
        style.configure("TCombobox", fieldbackground=self.colors["surface"], foreground=self.colors["fg"])

    def create_menu(self) -> None:
        menubar = tk.Menu(self.root, bg=self.colors["surface"], fg=self.colors["fg"])
        self.menubar = menubar
        self.root.config(menu=menubar)

        file_menu = tk.Menu(menubar, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        menubar.add_cascade(label=self.lang["file_menu"], menu=file_menu)
        file_menu.add_command(label=self.lang["open_recordings"], command=self.open_recordings)
        file_menu.add_separator()
        file_menu.add_command(label=self.lang["exit"], command=self.quit_app)

        view_menu = tk.Menu(menubar, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        menubar.add_cascade(label=self.lang["view_menu"], menu=view_menu)
        view_menu.add_checkbutton(label=self.lang["always_on_top"], variable=self.always_on_top, command=self.toggle_always_on_top)
        view_menu.add_command(label=self.lang["fullscreen"], command=self.toggle_fullscreen)
        view_menu.add_separator()

        self._show_audio_var = tk.BooleanVar(value=getattr(self, 'show_audio_panel', True))
        self._show_overlays_var = tk.BooleanVar(value=getattr(self, 'show_overlays_panel', True))
        show_menu = tk.Menu(view_menu, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        view_menu.add_cascade(label="Show", menu=show_menu)
        show_menu.add_checkbutton(label="Audio Mixer", variable=self._show_audio_var,
                                   command=lambda: self._toggle_panel_visibility('show_audio_panel', self._show_audio_var))
        show_menu.add_checkbutton(label="Overlays", variable=self._show_overlays_var,
                                   command=lambda: self._toggle_panel_visibility('show_overlays_panel', self._show_overlays_var))
        view_menu.add_separator()
        if HAS_PSUTIL:
            view_menu.add_command(label=self.lang["pc_analytics"], command=self.show_analytics)
            view_menu.add_separator()

        view_menu.add_command(label=self.lang["show_log"], command=self.show_log)
        view_menu.add_separator()

        theme_menu = tk.Menu(view_menu, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        view_menu.add_cascade(label=self.lang["theme"], menu=theme_menu)
        for _tid, _tlabel in [("dark","Dark"),("light","Light")]:
            theme_menu.add_radiobutton(label=_tlabel, variable=self.theme_var, value=_tid, command=lambda t=_tid: self.change_theme(t))

        settings_menu = tk.Menu(menubar, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        menubar.add_cascade(label=self.lang["settings_menu"], menu=settings_menu)
        settings_menu.add_command(label=self.lang["preferences"], command=self.open_settings)
        settings_menu.add_separator()
        capture_menu = tk.Menu(settings_menu, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        settings_menu.add_cascade(label=self.lang["capture_source"], menu=capture_menu)
        capture_menu.add_command(label=self.lang["full_desktop"], command=self.set_capture_desktop)
        capture_menu.add_command(label=self.lang["select_window"], command=self.open_window_picker)

        help_menu = tk.Menu(menubar, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        menubar.add_cascade(label=self.lang["help_menu"], menu=help_menu)
        help_menu.add_command(label=self.lang["check_updates"], command=self._manual_update_check)
        help_menu.add_separator()
        help_menu.add_command(label=self.lang["report_issue"], command=self._open_issues)

    def toggle_always_on_top(self) -> None:
        self.root.attributes('-topmost', self.always_on_top.get())
        self.save_settings(silent=True)

    def toggle_fullscreen(self) -> None:
        self.root.attributes('-fullscreen', not self.root.attributes('-fullscreen'))

    def show_cpu_info(self) -> None: self.show_analytics()
    def show_ram_info(self) -> None: self.show_analytics()
    def show_disk_info(self) -> None: self.show_analytics()

    def show_analytics(self) -> None:
        if not HAS_PSUTIL:
            messagebox.showinfo("PC Analytics", "psutil not installed."); return
        dlg = tk.Toplevel(self.root)
        self._set_icon(dlg)
        dlg.title("PC Analytics"); dlg.geometry("360x440"); dlg.configure(bg=self.colors["bg"])
        dlg.transient(self.root); dlg.resizable(False, True)
        dlg.update_idletasks()
        dlg.geometry(f"+{self.root.winfo_x()+self.root.winfo_width()//2-170}+{self.root.winfo_y()+self.root.winfo_height()//2-150}")

        def make_section(parent, title, color):
            f = tk.Frame(parent, bg=self.colors["surface"], pady=8, padx=12)
            f.pack(fill="x", padx=12, pady=6)
            tk.Label(f, text=title, bg=self.colors["surface"], fg=color, font=("Segoe UI", 10, "bold")).pack(anchor="w")
            return f

        def row(parent, label, value):
            r = tk.Frame(parent, bg=self.colors["surface"])
            r.pack(fill="x", pady=1)
            tk.Label(r, text=label, bg=self.colors["surface"], fg=self.colors["text_secondary"], font=("Segoe UI", 9), width=14, anchor="w").pack(side="left")
            tk.Label(r, text=value, bg=self.colors["surface"], fg=self.colors["text"], font=("Consolas", 9)).pack(side="left")

        def refresh():
            for w in dlg.winfo_children(): w.destroy()
            tk.Label(dlg, text="PC Analytics", bg=self.colors["bg"], fg=self.colors["accent"], font=("Segoe UI", 12, "bold")).pack(pady=(12, 4))
            cpu_f = make_section(dlg, "CPU", self.colors["accent"])
            row(cpu_f, "Cores:", str(psutil.cpu_count()))
            row(cpu_f, "Usage:", f"{psutil.cpu_percent(interval=0.3):.1f}%")
            mem = psutil.virtual_memory()
            ram_f = make_section(dlg, "RAM", self.colors["success"])
            row(ram_f, "Total:", f"{mem.total/1024**3:.1f} GB")
            row(ram_f, "Available:", f"{mem.available/1024**3:.1f} GB")
            row(ram_f, "Used:", f"{mem.percent}%")
            if os.path.exists(self.output_folder):
                disk = psutil.disk_usage(self.output_folder)
                dsk_f = make_section(dlg, "Disk", self.colors["warning"])
                row(dsk_f, "Total:", f"{disk.total/1024**3:.1f} GB")
                row(dsk_f, "Free:", f"{disk.free/1024**3:.1f} GB")
                row(dsk_f, "Used:", f"{disk.percent}%")
            tk.Button(dlg, text="Refresh", command=refresh, bg=self.colors["surface_light"], fg=self.colors["text"], font=("Segoe UI", 9), relief="flat", padx=16, pady=4, cursor="hand2").pack(pady=(4, 12))
        refresh()

    def show_log(self) -> None:
        log_dir = _ROOT_DIR
        log_path = os.path.join(log_dir, "homrec.log")
        dlg = tk.Toplevel(self.root)
        self._set_icon(dlg)
        dlg.title("HomRec Log"); dlg.geometry("720x460"); dlg.configure(bg=self.colors["bg"])
        dlg.transient(self.root); dlg.resizable(True, True)
        dlg.update_idletasks()
        dlg.geometry(f"+{self.root.winfo_x()+self.root.winfo_width()//2-360}+{self.root.winfo_y()+self.root.winfo_height()//2-230}")

        hdr = tk.Frame(dlg, bg=self.colors.get("surface", "#313244"), pady=10)
        hdr.pack(fill="x")
        tk.Label(hdr, text="📋  HomRec Log", bg=self.colors.get("surface", "#313244"), fg=self.colors["accent"], font=("Segoe UI", 11, "bold")).pack(side="left", padx=14)
        tk.Label(hdr, text=log_path, bg=self.colors.get("surface", "#313244"), fg=self.colors.get("text_secondary", "#a6adc8"), font=("Segoe UI", 8)).pack(side="left", padx=6)
        tk.Frame(dlg, bg=self.colors["accent"], height=2).pack(fill="x")

        txt_frame = tk.Frame(dlg, bg=self.colors["bg"])
        txt_frame.pack(fill="both", expand=True, padx=8, pady=8)
        vsb = tk.Scrollbar(txt_frame); vsb.pack(side="right", fill="y")
        hsb = tk.Scrollbar(txt_frame, orient="horizontal"); hsb.pack(side="bottom", fill="x")
        txt = tk.Text(txt_frame, bg=self.colors.get("surface", "#1e1e2e"), fg=self.colors.get("text", "#cdd6f4"), font=("Consolas", 9), relief="flat", wrap="none", state="disabled", yscrollcommand=vsb.set, xscrollcommand=hsb.set)
        txt.pack(side="left", fill="both", expand=True)
        vsb.config(command=txt.yview); hsb.config(command=txt.xview)

        def _load():
            txt.config(state="normal"); txt.delete("1.0", "end")
            try:
                with open(log_path, "r", encoding="utf-8", errors="replace") as f:
                    txt.insert("end", f.read())
            except FileNotFoundError:
                txt.insert("end", f"Log file not found:\n{log_path}")
            except Exception as e:
                txt.insert("end", f"Error reading log: {e}")
            txt.config(state="disabled"); txt.see("end")
        _load()

        btn_frame = tk.Frame(dlg, bg=self.colors["bg"])
        btn_frame.pack(fill="x", padx=8, pady=(0, 8))

        def _open_folder():
            try:
                if sys.platform == "win32": os.startfile(log_dir)
                elif sys.platform == "darwin": subprocess.Popen(["open", log_dir])
                else: subprocess.Popen(["xdg-open", log_dir])
            except Exception as e: log.warning(f"Failed to open log folder: {e}")

        tk.Button(btn_frame, text="🔄 Refresh", command=_load, bg=self.colors.get("surface_light","#45475a"), fg=self.colors["text"], font=("Segoe UI", 9), relief="flat", padx=14, pady=5, cursor="hand2").pack(side="left", padx=(0,6))
        tk.Button(btn_frame, text="📂 Open Folder", command=_open_folder, bg=self.colors.get("surface_light","#45475a"), fg=self.colors["text"], font=("Segoe UI", 9), relief="flat", padx=14, pady=5, cursor="hand2").pack(side="left")
        tk.Button(btn_frame, text="Close", command=dlg.destroy, bg=self.colors.get("surface","#313244"), fg=self.colors["text"], font=("Segoe UI", 9), relief="flat", padx=14, pady=5, cursor="hand2").pack(side="right")

    def change_language(self, lang: str) -> None:
        if lang != self.current_language:
            self.current_language = lang
            self.lang = self._load_language(lang)   # BUG FIX: was LANGUAGES[lang] — crashed for custom .hrl languages
            self.language_var.set(lang)
            self.update_ui_language()
            self.save_settings(silent=True)

    def open_settings(self) -> None: SettingsDialog(self.root, self)

    def change_theme(self, theme: str) -> None:
        self.current_theme = theme; self.theme_var.set(theme)
        self.colors = self.get_theme_colors(theme)
        self.apply_theme(); self.recreate_widgets()
        self.save_settings(silent=True)

    def recreate_widgets(self) -> None:
        was_recording = self.recording; was_paused = self.paused
        # Останавливаем текущий цикл update_preview — он держит ссылку на старые виджеты.
        self._preview_active = False
        for widget in self.root.winfo_children(): widget.destroy()
        self.create_menu(); self.create_widgets()
        # Запускаем новый цикл для только что созданного preview_label.
        self._preview_active = True
        self.update_preview()
        if was_recording:
            self.record_btn.config(text=self.lang["stop"], bg=self.colors["error"], command=self.stop_recording)
            self.pause_btn.config(state="normal")
            if was_paused:
                self.pause_btn.config(text=self.lang["resume"], bg=self.colors["success"])

    def _toggle_panel_visibility(self, attr_name: str, var: tk.BooleanVar) -> None:
        setattr(self, attr_name, var.get())
        self.save_settings(silent=True)
        self.recreate_widgets()

    def set_mode(self, mode: str) -> None:
        self.recording_mode = mode; self.update_mode_settings()
        self.save_settings(silent=True)
        self.res_label.config(text=f"{self.lang['resolution']} {self.record_width}x{self.record_height}")

    def update_mode_settings(self) -> None:
        modes = {"ultra": (60, 95, 1.0), "turbo": (30, 90, 1.0), "balanced": (15, 70, 0.75), "eco": (8, 50, 0.5)}
        self.target_fps, self.quality, self.scale_factor = modes.get(self.recording_mode, (15, 70, 0.75))
        self.update_monitor_info()

    def load_settings(self) -> None:
        try:
            if os.path.exists(SETTINGS_PATH):
                self._first_launch = False
                with open(SETTINGS_PATH, "r") as f:
                    s = json.load(f)
                self.output_folder = s.get("output_folder", os.path.join(_ROOT_DIR, "recordings"))
                self.scale_factor = s.get("scale_factor", 0.75)
                self.target_fps = s.get("target_fps", 15)
                self.quality = max(50, min(100, int(s.get("quality", 70))))
                self.recording_mode = s.get("mode", "balanced")
                self.current_theme = s.get("theme", "dark")
                self.current_language = s.get("language", "en")
                self.lang = self._load_language(self.current_language)  # BUG FIX: was LANGUAGES[...] — crashed if saved language was a custom .hrl that's now missing
                self.always_on_top.set(s.get("always_on_top", False))
                self.countdown_var.set(s.get("countdown", True))
                self.timestamp_var.set(s.get("timestamp", False))
                self.cursor_var.set(s.get("cursor", False))
                self.show_summary = s.get("show_summary", True)
                self.minimize_to_tray.set(s.get("minimize_to_tray", True))
                self.video_codec = s.get("video_codec", "libx264")
                self.hw_accel = s.get("hw_accel", "auto")
                self.enc_preset = s.get("enc_preset", "ultrafast")
                self.enc_crf = s.get("enc_crf", 18)
                self.pix_fmt = s.get("pix_fmt", "yuv420p")
                self.audio_sample_rate = s.get("audio_sample_rate", 44100)
                self.audio_aac_bitrate = s.get("audio_aac_bitrate", "192k")
                self.audio_out_channels = s.get("audio_out_channels", 2)
                self.ui_theme = s.get("ui_theme", "dark")
                self.ui_scale = s.get("ui_scale", 1.0)
                self.ui_font = s.get("ui_font", "Segoe UI")
                self.filename_template = s.get("filename_template", "HomRec_{date}_{time}")
                self.auto_stop_min = s.get("auto_stop_min", 0)
                self.replay_buffer_sec = s.get("replay_buffer_sec", 0)
                self.hotkey_start_stop = s.get("hotkey_start_stop", "F9")
                self.hotkey_pause = s.get("hotkey_pause", "F10")
                self.hotkey_fullscreen = s.get("hotkey_fullscreen", "F11")
                self.notify_sound = s.get("notify_sound", True)
                self.notify_flash = s.get("notify_flash", True)
                self.auto_save_profile = s.get("auto_save_profile", False)
                self.disable_preview = s.get("disable_preview", False)
                self.video_format = s.get("video_format", "mp4")
                self.separate_audio_mp3 = s.get("separate_audio_mp3", False)
                self.overlays = s.get("overlays", [])
                self.show_audio_panel = s.get("show_audio_panel", True)
                self.show_overlays_panel = s.get("show_overlays_panel", True)
                self._saved_mic_volume = s.get("mic_volume", 80)
                self._saved_sys_volume = s.get("sys_volume", 100)
                self._saved_mic_mute = s.get("mic_mute", False)
                self._saved_sys_mute = s.get("sys_mute", False)
                self._saved_audio_enabled = s.get("audio_enabled", True)
                if self.always_on_top.get(): self.root.attributes('-topmost', True)
        except: pass
        if not hasattr(self, '_first_launch'): self._first_launch = True

    def save_settings(self, silent: bool = False) -> None:
        settings = {
            "output_folder": self.output_folder, "scale_factor": self.scale_factor,
            "target_fps": self.target_fps, "quality": self.quality, "mode": self.recording_mode,
            "theme": self.current_theme, "language": self.current_language,
            "always_on_top": self.always_on_top.get(), "countdown": self.countdown_var.get(),
            "timestamp": self.timestamp_var.get(), "cursor": self.cursor_var.get(),
            "show_summary": self.show_summary, "minimize_to_tray": self.minimize_to_tray.get(),
            "video_codec": getattr(self, 'video_codec', 'libx264'),
            "hw_accel": getattr(self, 'hw_accel', 'auto'),
            "enc_preset": getattr(self, 'enc_preset', 'ultrafast'),
            "enc_crf": getattr(self, 'enc_crf', 18),
            "pix_fmt": getattr(self, 'pix_fmt', 'yuv420p'),
            "audio_sample_rate": getattr(self, 'audio_sample_rate', 44100),
            "audio_aac_bitrate": getattr(self, 'audio_aac_bitrate', '192k'),
            "audio_out_channels": getattr(self, 'audio_out_channels', 2),
            "ui_theme": getattr(self, 'ui_theme', 'dark'),
            "ui_scale": getattr(self, 'ui_scale', 1.0),
            "ui_font": getattr(self, 'ui_font', 'Segoe UI'),
            "filename_template": getattr(self, 'filename_template', 'HomRec_{date}_{time}'),
            "auto_stop_min": getattr(self, 'auto_stop_min', 0),
            "replay_buffer_sec": getattr(self, 'replay_buffer_sec', 0),
            "hotkey_start_stop": getattr(self, 'hotkey_start_stop', 'F9'),
            "hotkey_pause": getattr(self, 'hotkey_pause', 'F10'),
            "hotkey_fullscreen": getattr(self, 'hotkey_fullscreen', 'F11'),
            "notify_sound": getattr(self, 'notify_sound', True),
            "notify_flash": getattr(self, 'notify_flash', True),
            "auto_save_profile": getattr(self, 'auto_save_profile', False),
            "disable_preview": getattr(self, 'disable_preview', False),
            "video_format": getattr(self, 'video_format', 'mp4'),
            "separate_audio_mp3": getattr(self, 'separate_audio_mp3', False),
            "overlays": getattr(self, 'overlays', []),
            "show_audio_panel": getattr(self, 'show_audio_panel', True),
            "show_overlays_panel": getattr(self, 'show_overlays_panel', True),
            "mic_volume": int(self.audio_panel.mic_volume.get()) if hasattr(self, 'audio_panel') else 80,
            "sys_volume": int(self.audio_panel.sys_volume.get()) if hasattr(self, 'audio_panel') else 100,
            "mic_mute": bool(self.audio_panel.mic_mute.get()) if hasattr(self, 'audio_panel') else False,
            "sys_mute": bool(self.audio_panel.sys_mute.get()) if hasattr(self, 'audio_panel') else False,
            "audio_enabled": bool(self.audio_panel.audio_enabled.get()) if hasattr(self, 'audio_panel') else True,
        }
        try:
            with open(SETTINGS_PATH, "w") as f:
                json.dump(settings, f, indent=2)
        except Exception as e:
            log.error(f"save_settings failed: {e}")
            if not silent:
                messagebox.showerror(self.lang["error"], f"Could not save settings:\n{e}")
            return
        if not silent:
            messagebox.showinfo(self.lang["info"], self.lang["settings_saved"])

    def save_settings_debounced(self, delay_ms: int = 400) -> None:
        existing = getattr(self, '_pending_save_after_id', None)
        if existing is not None:
            try: self.root.after_cancel(existing)
            except Exception: pass
        self._pending_save_after_id = self.root.after(
            delay_ms, lambda: (self.save_settings(silent=True), setattr(self, '_pending_save_after_id', None)))

    def update_monitor_info(self) -> None:
        if self.monitor_id < len(self.sct.monitors):
            self.monitor = self.sct.monitors[self.monitor_id]
            self.original_width = self.monitor['width']
            self.original_height = self.monitor['height']
            self.monitor_left = self.monitor['left']
            self.monitor_top = self.monitor['top']
            self.record_width = int(self.original_width * self.scale_factor)
            self.record_height = int(self.original_height * self.scale_factor)
            if self.record_width % 2 != 0: self.record_width -= 1
            if self.record_height % 2 != 0: self.record_height -= 1

    def create_widgets(self) -> None:
        main_container = tk.Frame(self.root, bg=self.colors["bg"])
        main_container.pack(fill="both", expand=True, padx=15, pady=15)

        left_panel = tk.Frame(main_container, bg=self.colors["surface"], width=240)
        self.left_panel = left_panel
        left_panel.pack(side="left", fill="y", padx=(0, 15))
        left_panel.pack_propagate(False)

        title_frame = tk.Frame(left_panel, bg=self.colors["surface"])
        title_frame.pack(pady=20, fill="x")
        tk.Label(title_frame, text="HomRec", font=("Segoe UI", 22, "bold"), bg=self.colors["surface"], fg=self.colors["accent"]).pack()
        tk.Label(title_frame, text="v1.7.0", font=("Segoe UI", 11), bg=self.colors["surface"], fg=self.colors["text_secondary"]).pack()

        btn_frame = tk.Frame(left_panel, bg=self.colors["surface"])
        btn_frame.pack(pady=25, padx=15, fill="x")
        self.record_btn = tk.Button(btn_frame, text=self.lang["start"], command=self.start_with_countdown,
                                    bg=self.colors["success"], fg=self.colors["bg"], font=("Segoe UI", 11, "bold"),
                                    relief="flat", height=2, cursor="hand2")
        self.record_btn.pack(fill="x", pady=(0, 4))
        self.pause_btn = tk.Button(btn_frame, text=self.lang["pause"], command=self.toggle_pause,
                                   bg=self.colors["warning"], fg=self.colors["bg"], font=("Segoe UI", 10, "bold"),
                                   state="disabled", relief="flat", height=1, cursor="hand2")
        self.pause_btn.pack(fill="x", pady=(4, 0))
        self.stop_btn = tk.Button(btn_frame)

        status_frame = tk.Frame(left_panel, bg=self.colors["surface"])
        status_frame.pack(pady=15, padx=15, fill="x")
        tk.Label(status_frame, text=self.lang["status"], font=("Segoe UI", 11, "bold"), bg=self.colors["surface"], fg=self.colors["accent"]).pack(anchor="w")
        status_row = tk.Frame(status_frame, bg=self.colors["surface"])
        status_row.pack(fill="x", pady=8)
        self.status_icon = tk.Label(status_row, text="⬤", fg=self.colors["error"], bg=self.colors["surface"], font=("Arial", 18))
        self.status_icon.pack(side="left", padx=(0, 8))
        self.status_label = tk.Label(status_row, text=self.lang["ready"], bg=self.colors["surface"], fg=self.colors["text"], font=("Segoe UI", 11))
        self.status_label.pack(side="left")

        timer_frame = tk.Frame(left_panel, bg=self.colors["surface"])
        timer_frame.pack(pady=15, padx=15, fill="x")
        tk.Label(timer_frame, text=self.lang["time"], font=("Segoe UI", 11, "bold"), bg=self.colors["surface"], fg=self.colors["accent"]).pack(anchor="w")
        self.time_label = tk.Label(timer_frame, text="00:00:00", font=("Consolas", 24, "bold"), bg=self.colors["surface"], fg=self.colors["accent"])
        self.time_label.pack(pady=8)

        stats_frame = tk.Frame(left_panel, bg=self.colors["surface"])
        stats_frame.pack(pady=15, padx=15, fill="x")
        tk.Label(stats_frame, text=self.lang["stats"], font=("Segoe UI", 11, "bold"), bg=self.colors["surface"], fg=self.colors["accent"]).pack(anchor="w")
        self.fps_label = tk.Label(stats_frame, text=f"{self.lang['fps']} 0", bg=self.colors["surface"], fg=self.colors["text"], font=("Consolas", 11))
        self.fps_label.pack(anchor="w", pady=3)
        self.res_label = tk.Label(stats_frame, text=f"{self.lang['resolution']} {self.record_width}x{self.record_height}", bg=self.colors["surface"], fg=self.colors["text"], font=("Consolas", 11))
        self.res_label.pack(anchor="w", pady=3)

        right_panel = tk.Frame(main_container, bg=self.colors["bg"])
        right_panel.pack(side="right", fill="both", expand=True)

        preview_container = tk.Frame(right_panel, bg=self.colors["surface_light"], relief="flat", bd=2)
        self.preview_container = preview_container
        preview_container.pack(fill="both", expand=True, pady=(0, 15))

        preview_header = tk.Frame(preview_container, bg=self.colors.get("surface_light","#45475a"), height=30)
        preview_header.pack(fill="x"); preview_header.pack_propagate(False)
        tk.Label(preview_header, text="● " + self.lang["live_preview"], bg=self.colors.get("surface_light","#45475a"), fg=self.colors["accent"], font=("Segoe UI", 9, "bold")).pack(side="left", padx=10, pady=5)
        self._preview_fps_lbl = tk.Label(preview_header, text="", bg=self.colors.get("surface_light","#45475a"), fg=self.colors.get("text_secondary","#a6adc8"), font=("Segoe UI", 8))
        self._preview_fps_lbl.pack(side="right", padx=10, pady=5)

        preview_frame = tk.Frame(preview_container, bg=self.colors["preview_bg"])
        preview_frame.pack(fill="both", expand=True, padx=8, pady=8)
        self.preview_label = tk.Label(preview_frame, bg=self.colors["preview_bg"])
        self.preview_label.pack(fill="both", expand=True)

        show_audio = getattr(self, 'show_audio_panel', True)
        show_overlays = getattr(self, 'show_overlays_panel', True)

        if show_audio or show_overlays:
            bottom_panel = tk.Frame(right_panel, bg=self.colors["bg"], height=300)
            bottom_panel.pack(fill="x"); bottom_panel.pack_propagate(False)

            if show_audio and show_overlays:
                paned = ttk.PanedWindow(bottom_panel, orient="horizontal")
                paned.pack(fill="both", expand=True)
                audio_host = tk.Frame(paned, bg=self.colors["bg"])
                overlays_host = tk.Frame(paned, bg=self.colors["bg"])
                paned.add(audio_host, weight=3)
                paned.add(overlays_host, weight=1)
                self.audio_panel = AudioPanel(audio_host, self)
                self.overlays_panel = OverlaysDockPanel(overlays_host, self)
            elif show_audio:
                self.audio_panel = AudioPanel(bottom_panel, self)
                self.overlays_panel = None
            else:
                hidden_audio_host = tk.Frame(right_panel, bg=self.colors["bg"])
                self.audio_panel = AudioPanel(hidden_audio_host, self)
                self.overlays_panel = OverlaysDockPanel(bottom_panel, self)
        else:
            hidden_host = tk.Frame(right_panel, bg=self.colors["bg"])
            self.audio_panel = AudioPanel(hidden_host, self)
            self.overlays_panel = None

        if hasattr(self, '_saved_mic_volume'):
            self.audio_panel.mic_volume.set(self._saved_mic_volume)
            self.audio_panel.mic_volume_label.config(text=f"{self._saved_mic_volume}%")
        if hasattr(self, '_saved_sys_volume'):
            self.audio_panel.sys_volume.set(self._saved_sys_volume)
            self.audio_panel.sys_volume_label.config(text=f"{self._saved_sys_volume}%")
        if hasattr(self, '_saved_mic_mute') and self._saved_mic_mute:
            self.audio_panel.mic_mute.set(True)
            self.audio_panel.mic_mute_btn.config(text=self.lang.get('unmute', 'Unmute'))
        if hasattr(self, '_saved_sys_mute') and self._saved_sys_mute:
            self.audio_panel.sys_mute.set(True)
            self.audio_panel.sys_mute_btn.config(text=self.lang.get('unmute', 'Unmute'))
        if hasattr(self, '_saved_audio_enabled'):
            self.audio_panel.audio_enabled.set(self._saved_audio_enabled)

        bottom_bar = tk.Frame(self.root, bg=self.colors["surface"], height=32)
        self.bottom_bar = bottom_bar
        bottom_bar.pack(side="bottom", fill="x"); bottom_bar.pack_propagate(False)
        self._status_dot = tk.Label(bottom_bar, text="●", bg=self.colors["surface"], fg=self.colors.get("success","#a6e3a1"), font=("Segoe UI", 9))
        self._status_dot.pack(side="left", padx=(10, 2), pady=6)
        self.file_label = tk.Label(bottom_bar, text=self.lang["ready"], bg=self.colors["surface"], fg=self.colors["text_secondary"], font=("Segoe UI", 9))
        self.file_label.pack(side="left", padx=(0, 10), pady=6)
        try:
            from homrec_native import NATIVE_OK, ENCODER_OK
            native_txt = "⚡ Native" if NATIVE_OK else "🐍 Python"
            native_col = self.colors.get("success","#a6e3a1") if NATIVE_OK else self.colors.get("warning","#f9e2af")
        except Exception:
            native_txt = "🐍 Python"; native_col = self.colors.get("text_secondary","#a6adc8")
        tk.Label(bottom_bar, text=native_txt, bg=self.colors["surface"], fg=native_col, font=("Segoe UI", 8)).pack(side="left", padx=4, pady=6)
        tk.Label(bottom_bar, text=self.lang["made_by"], bg=self.colors["surface"], fg=self.colors["accent"], font=("Segoe UI", 9, "bold")).pack(side="right", padx=12, pady=6)
        tk.Label(bottom_bar, text=f"v{CURRENT_VERSION}", bg=self.colors["surface"], fg=self.colors.get("text_secondary","#6c7086"), font=("Segoe UI", 8)).pack(side="right", padx=(0, 4), pady=6)
        self.update_preview_size()
        self._build_ui_registry()

    def _build_ui_registry(self) -> None:
        """
        Logical name -> widget map used by console commands (`!hide`, `$rm --ui`).
        Curated at panel / key-button granularity, not every single label —
        extend by adding more entries here.
        """
        reg: dict = {}

        def add(name: str, widget) -> None:
            if widget is not None:
                reg[name] = widget

        add("start_button", getattr(self, "record_btn", None))
        add("pause_button", getattr(self, "pause_btn", None))
        add("stop_button", getattr(self, "stop_btn", None))
        add("left_panel", getattr(self, "left_panel", None))
        add("preview_panel", getattr(self, "preview_container", None))
        add("bottom_bar", getattr(self, "bottom_bar", None))
        if hasattr(self, "audio_panel"):
            add("audio_mixer", getattr(self.audio_panel, "frame", None))
            add("mic_mute_button", getattr(self.audio_panel, "mic_mute_btn", None))
            add("sys_mute_button", getattr(self.audio_panel, "sys_mute_btn", None))
        if getattr(self, "overlays_panel", None):
            add("overlays_panel", getattr(self.overlays_panel, "frame", None))
        add("menu", getattr(self, "menubar", None))
        # alias kept for the example in the spec — "settings_window" really means
        # "keep the menu bar (so Settings is still reachable)"
        add("settings_window", getattr(self, "menubar", None))

        self.ui_registry = reg

    def get_audio_channels(self) -> int:
        try:
            from homrec_native import AUDIO_OK
            if AUDIO_OK: return 2
        except Exception: pass
        if not _PYAUDIO_AVAILABLE: return 2
        try:
            p = _pyaudio_mod.PyAudio()
            try:
                for ch in (2, 1):
                    try:
                        s = p.open(format=_pyaudio_mod.paInt16, channels=ch, rate=44100, input=True, frames_per_buffer=1024)
                        s.close(); return ch
                    except Exception: pass
                return 1
            finally:
                try: p.terminate()
                except: pass
        except Exception: return 2

    @staticmethod
    def _pyaudio_supports_loopback(p) -> bool:
        try:
            p.open(format=_pyaudio_mod.paInt16, channels=1, rate=44100, input=True, input_device_index=99999, frames_per_buffer=512, as_loopback=True)
        except TypeError: return False
        except Exception: return True
        return True

    def _find_wasapi_loopback(self, p, require_input: bool = False):
        if sys.platform != 'win32': return None
        try: wasapi_info = p.get_host_api_info_by_type(_pyaudio_mod.paWASAPI)
        except OSError: log.warning("WASAPI not available"); return None

        default_out_idx = first_wasapi_out_idx = None
        wasapi_default_dev = wasapi_info.get('defaultOutputDevice', -1)

        for i in range(p.get_device_count()):
            try: dev = p.get_device_info_by_index(i)
            except Exception: continue
            if dev.get('hostApi') != wasapi_info['index']: continue
            name = dev.get('name', '').lower()
            if dev.get('maxInputChannels', 0) >= 1:
                if any(k in name for k in ('loopback','stereo mix','what u hear','стерео микшер','что слышит')):
                    return i
            if not require_input and dev.get('maxOutputChannels', 0) >= 1:
                if default_out_idx is None and wasapi_default_dev >= 0 and i == wasapi_default_dev:
                    default_out_idx = i
                if first_wasapi_out_idx is None:
                    first_wasapi_out_idx = i

        chosen = default_out_idx if default_out_idx is not None else first_wasapi_out_idx
        if chosen is not None: return chosen
        log.warning("No WASAPI loopback device found"); return None

    def _notify_recording_start(self) -> None:
        if getattr(self, 'notify_flash', True):
            orig_bg = self.root.cget("bg")
            def _flash(n=0):
                if n >= 6: self.root.configure(bg=orig_bg); return
                self.root.configure(bg=self.colors.get("error","#f38ba8") if n % 2 == 0 else orig_bg)
                self.root.after(120, lambda: _flash(n + 1))
            _flash()
        if getattr(self, 'notify_sound', True):
            try: import winsound; winsound.MessageBeep(winsound.MB_OK)
            except Exception: pass

    def _register_file_types(self) -> None:
        if platform.system() != "Windows": return
        try:
            import winreg
            base = os.path.dirname(os.path.abspath(__file__))
            icons_dir = os.path.join(base, "icons")
            types = [(".hrc","HomRec.Profile","HomRec Profile","hrc.ico"), (".hrl","HomRec.Language","HomRec Language","hrl.ico")]
            for ext, prog_id, description, ico_file in types:
                ico_path = os.path.join(icons_dir, ico_file)
                with winreg.CreateKey(winreg.HKEY_CURRENT_USER, f"Software\\Classes\\{ext}") as k:
                    winreg.SetValue(k, "", winreg.REG_SZ, prog_id)
                with winreg.CreateKey(winreg.HKEY_CURRENT_USER, f"Software\\Classes\\{prog_id}") as k:
                    winreg.SetValue(k, "", winreg.REG_SZ, description)
                if os.path.exists(ico_path):
                    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, f"Software\\Classes\\{prog_id}\\DefaultIcon") as k:
                        winreg.SetValue(k, "", winreg.REG_SZ, ico_path)
                exe_path = os.path.abspath(__file__)
                with winreg.CreateKey(winreg.HKEY_CURRENT_USER, f"Software\\Classes\\{prog_id}\\shell\\open\\command") as k:
                    winreg.SetValue(k, "", winreg.REG_SZ, f'"{exe_path}" "%1"')
            try: ctypes.windll.shell32.SHChangeNotify(0x08000000, 0, None, None)
            except Exception: pass
        except Exception as e: log.warning(f"Could not register file types: {e}")

    def _apply_hotkeys(self) -> None:
        for key in getattr(self, '_bound_hotkeys', []):
            try: self.root.unbind(key)
            except Exception: pass
        self._bound_hotkeys = []

        def _bind(key, cmd):
            if not key or " " in key or key == "Press a key...":
                log.warning(f"Skipping invalid hotkey: {key!r}"); return
            k = f'<{key}>'
            try: self.root.bind(k, cmd); self._bound_hotkeys.append(k)
            except Exception as e: log.warning(f"Failed to bind hotkey {k!r}: {e}")

        _bind(self.hotkey_start_stop, lambda e: self.toggle_recording())
        _bind(self.hotkey_pause, lambda e: self.toggle_pause() if self.recording else None)
        _bind(self.hotkey_fullscreen, lambda e: self.toggle_fullscreen())

    def _handle_drop(self, event) -> None:
        raw = event.data.strip()
        paths = []
        if raw.startswith('{'):
            paths = re.findall(r'{([^}]+)}', raw) or [raw.strip('{}')]
        else:
            paths = raw.split()
        for path in paths:
            path = path.strip()
            try:
                kind = _hrc_detect(path)
                if kind == 'hrc': self._import_hrc(path)
                elif kind == 'hrl': self._import_hrl(path)
            except ValueError as e:
                messagebox.showerror("Invalid file", str(e))

    def _import_hrc(self, path: str) -> None:
        try:
            data = _hrc_read(path, _HRC_MAGIC)
            for k, v in data.items():
                if k != 'hrc_version': setattr(self, k, v)
            self.save_settings(silent=True); self.apply_theme()
            messagebox.showinfo("Profile imported", f"Profile loaded:\n{os.path.basename(path)}")
            log.info(f"HRC profile imported: {path}")
        except Exception as e: messagebox.showerror("Import failed", str(e))

    def _import_hrl(self, path: str) -> None:
        try:
            langs_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), LANGS_DIR)
            os.makedirs(langs_dir, exist_ok=True)
            shutil.copy2(path, os.path.join(langs_dir, os.path.basename(path)))
            data = _hrc_read(path, _HRL_MAGIC)
            lang_code = os.path.splitext(os.path.basename(path))[0]
            self.current_language = lang_code
            self.lang = dict(LANGUAGES["en"]); self.lang.update(data)
            self.language_var.set(lang_code)
            self.save_settings(silent=True)
            self.update_ui_language()
            messagebox.showinfo("Language installed", f"Language '{data.get('lang_name', lang_code)}' applied!")
            log.info(f"HRL language imported and applied: {path}")
        except Exception as e: messagebox.showerror("Import failed", str(e))


    def _setup_drag_drop(self) -> None:
        if not _DND_AVAILABLE: return
        try:
            self.root.drop_target_register(DND_FILES)
            self.root.dnd_bind('<<Drop>>', self._handle_drop)
        except Exception as e: log.warning(f"Drag-and-drop setup failed: {e}")

    def _set_icon(self, window) -> None:
        try:
            ico = os.path.join(_ROOT_DIR, "icons", "main.ico")
            if os.path.exists(ico): window.iconbitmap(ico)
        except Exception: pass

    def _detect_gpu_encoder(self) -> str | None:
        return getattr(self, '_gpu_encoder_cache', None)

    def _warm_up_gpu_probe(self) -> None:
        if not self.ffmpeg_path or hasattr(self, '_gpu_encoder_cache'): return
        ffpath = self.ffmpeg_path

        def _probe():
            try:
                from homrec_native import tools_engine as _te, TOOLS_OK as _TOK
                if _TOK and _te:
                    enc = _te.probe_gpu(ffpath)
                    self._gpu_encoder_cache = enc
                    log.info(f"GPU encoder: {enc or 'none'}")
                    return
            except Exception as e: log.debug(f"C++ GPU probe error: {e}")
            _cf = subprocess.CREATE_NO_WINDOW if platform.system()=='Windows' else 0
            for name, args in [
                ('h264_nvenc',['-f','lavfi','-i','nullsrc=s=8x8:d=0.01','-c:v','h264_nvenc','-f','null','-']),
                ('h264_amf',  ['-f','lavfi','-i','nullsrc=s=8x8:d=0.01','-c:v','h264_amf',  '-f','null','-']),
                ('h264_qsv',  ['-f','lavfi','-i','nullsrc=s=8x8:d=0.01','-c:v','h264_qsv',  '-f','null','-']),
            ]:
                try:
                    r = subprocess.run([ffpath, '-y', *args],
                        capture_output=True, timeout=4, creationflags=_cf)
                    if r.returncode == 0:
                        log.info(f"GPU encoder detected: {name}")
                        self._gpu_encoder_cache = name; return
                except Exception: pass
            self._gpu_encoder_cache = None

        threading.Thread(target=_probe, daemon=True).start()

    def _safe_pix_fmt(self) -> str:
        return 'yuv420p'

    def _ddagrab_vf(self) -> str:
        """
        Конвертация GPU-текстуры (ddagrab) → CPU-фрейм для любого энкодера.
        hwdownload — копирует DXGI-текстуру из VRAM в RAM.
        Работает с QSV, NVENC, AMF, libx264. hwmap намеренно не используется
        (требует одного GPU-контекста у источника и энкодера — на практике даёт -40).
        """
        return 'hwdownload,format=bgra,format=yuv420p'

    def _build_codec_args(self) -> list:
        codec = getattr(self, 'video_codec', 'libx264')
        hw    = getattr(self, 'hw_accel', 'auto')
        if codec == 'libx264' and hw == 'auto':
            gpu = self._detect_gpu_encoder()
            if gpu:
                codec = gpu; log.info(f"Auto-upgraded codec → {codec}")
        quality = getattr(self, 'quality', 70)
        fps = getattr(self, 'target_fps', 30)
        cpu_count = os.cpu_count() or 4
        try:
            from homrec_native import tools_engine as _te, TOOLS_OK as _TOK
            if _TOK and _te:
                args = _te.build_codec_args(codec, quality, fps, cpu_count)
                return args
        except Exception as e: log.debug(f"C++ build_codec_args error: {e}")
        qp = max(18, min(34, int(34 - (quality / 100) * 16)))
        gop = fps * 2
        is_nvenc = 'nvenc' in codec
        is_qsv   = 'qsv'   in codec
        is_amf   = 'amf'   in codec
        is_265   = codec == 'libx265' or 'hevc' in codec
        args = ['-c:v', codec]
        if is_nvenc:
            args += ['-preset','p1','-tune','ull','-rc','constqp',
                     '-qp',str(qp),'-g',str(gop),
                     '-spatial-aq','1','-aq-strength','8',
                     '-bf','0','-profile:v','high']
        elif is_qsv:
            args += ['-preset','veryfast','-look_ahead','0','-low_power','1',
                     '-global_quality',str(qp),'-g',str(gop),'-profile:v','high']
        elif is_amf:
            args += ['-quality','speed','-rc','cqp',
                     '-qp_i',str(qp),'-qp_p',str(qp),
                     '-g',str(gop),'-profile:v','high']
        else:
            thr = max(1, (cpu_count or 4) // 2)
            # OPTIMIZED: Lower CPU usage with restricted threads
            args += ['-preset','superfast','-tune','zerolatency',
         '-crf','28','-g',str(gop),'-threads','2']
            if not is_265: args += ['-profile:v','high','-level','4.2']
            if is_265: args += ['-x265-params','log-level=error:no-open-gop=1']
        return args

    def start_audio_recording(self) -> None:
        self.audio_thread = None; self.audio_frames = []; self.sys_audio_frames = []
        self.sys_audio_recording = False; self.sys_audio_filename = None; self.sys_ffmpeg_proc = None
        try:
            from homrec_native import audio_engine as _ae, AUDIO_OK as _AOK
        except Exception:
            _ae = None; _AOK = False
        if not (_AOK and _ae):
            log.warning("hr_audio.dll not available — audio recording disabled")
            self.audio_recording = False; self._using_cpp_audio = False; return

        mic_vol = self.audio_panel.mic_volume.get() / 100.0
        sys_vol = self.audio_panel.sys_volume.get() / 100.0
        flags = _ae.start(mic_vol, sys_vol, self.audio_panel.mic_mute.get(), self.audio_panel.sys_mute.get())
        mic_ok = bool(flags & 0x1); sys_ok = bool(flags & 0x2)
        log.info(f"C++ AudioEngine: mic={'OK' if mic_ok else 'FAIL'} sys={'OK' if sys_ok else 'FAIL'}")
        self._using_cpp_audio = mic_ok or sys_ok
        self.audio_recording = mic_ok; self.sys_audio_recording = sys_ok; self.audio_channels = 2

        if self._using_cpp_audio:
            def _cpp_vu_poll():
                while getattr(self, '_using_cpp_audio', False) and (self.audio_recording or self.sys_audio_recording):
                    m, s = _ae.get_levels()
                    self.audio_panel.update_mic_level(m); self.audio_panel.update_sys_level(s)
                    time.sleep(0.05)
            threading.Thread(target=_cpp_vu_poll, daemon=True).start()
        else:
            log.warning("C++ AudioEngine: no streams opened")
            self.audio_recording = False

    def stop_audio_recording(self) -> str | None:
        if not getattr(self, '_using_cpp_audio', False):
            self.audio_recording = False; self.sys_audio_recording = False; return None
        self._using_cpp_audio = False; self.audio_recording = False; self.sys_audio_recording = False
        try:
            from homrec_native import audio_engine as _ae, AUDIO_OK as _AOK
        except Exception:
            _ae = None; _AOK = False
        if not (_AOK and _ae): return None

        _base = os.path.splitext(self.filename)[0]
        audio_filename = _base + '_audio.wav'
        mic_wav = _base + '_mic_tmp.wav'
        sys_wav = _base + '_sys.wav'
        has_mic = not self.audio_panel.mic_mute.get()
        has_sys = not self.audio_panel.sys_mute.get()
        flags = _ae.stop(mic_wav if has_mic else None, sys_wav if has_sys else None)
        mic_written = bool(flags & 0x1); sys_written = bool(flags & 0x2)
        log.info(f"C++ AudioEngine stopped: mic={mic_written} sys={sys_written}")

        if mic_written and sys_written:
            if _ae.mix_wav(mic_wav, sys_wav, audio_filename):
                for f in (mic_wav, sys_wav):
                    try: os.remove(f)
                    except: pass
                return audio_filename
            try: os.rename(mic_wav, audio_filename)
            except: pass
            return audio_filename
        if mic_written:
            try: os.rename(mic_wav, audio_filename)
            except: pass
            return audio_filename
        if sys_written:
            try: os.rename(sys_wav, audio_filename)
            except: pass
            return audio_filename
        return None

    def select_folder(self) -> None:
        folder = filedialog.askdirectory(initialdir=self.output_folder)
        if folder:
            self.output_folder = folder; os.makedirs(folder, exist_ok=True); self.save_settings(silent=True)

    def open_recordings(self) -> None:
        if os.path.exists(self.output_folder):
            # BUG FIX: os.startfile is Windows-only; use platform-aware open
            try:
                if sys.platform == "win32":
                    os.startfile(self.output_folder)
                elif sys.platform == "darwin":
                    subprocess.Popen(["open", self.output_folder])
                else:
                    subprocess.Popen(["xdg-open", self.output_folder])
            except Exception as e:
                log.warning(f"Failed to open recordings folder: {e}")
        else:
            messagebox.showwarning(self.lang["warning"], self.lang["folder_not_exist"])

    def start_with_countdown(self) -> None:
        if not self.recording:
            self.show_countdown() if self.countdown_var.get() else self.start_recording()
        else:
            self.stop_recording()

    def show_countdown(self) -> None:
        w = tk.Toplevel(self.root); self._set_icon(w)
        W, H = 300, 200
        w.geometry(f"{W}x{H}"); w.configure(bg="#0f0f17"); w.overrideredirect(True)
        try: w.attributes("-alpha", 0.92)
        except: pass
        w.update_idletasks()
        w.geometry(f"{W}x{H}+{(w.winfo_screenwidth()-W)//2}+{(w.winfo_screenheight()-H)//2}")
        w.lift(); w.attributes("-topmost", True)

        cv = tk.Canvas(w, width=W, height=H, bg="#0f0f17", highlightthickness=0)
        cv.pack(fill="both", expand=True)
        cx, cy, r = W//2, H//2 - 10, 60
        cv.create_oval(cx-r, cy-r, cx+r, cy+r, outline="#313244", width=6)
        arc_id = cv.create_arc(cx-r, cy-r, cx+r, cy+r, start=90, extent=0, outline=self.colors.get("success","#a6e3a1"), width=6, style="arc")
        num_id = cv.create_text(cx, cy, text="3", font=("Segoe UI", 42, "bold"), fill=self.colors.get("success","#a6e3a1"))
        hint_id = cv.create_text(cx, cy+r+22, text="Starting recording…", font=("Segoe UI", 10), fill="#6c7086")

        def tick(n: int) -> None:
            if n > 0:
                cv.itemconfig(arc_id, extent=-(n/3)*360, outline=self.colors.get("success","#a6e3a1"))
                cv.itemconfig(num_id, text=str(n), fill=self.colors.get("success","#a6e3a1"))
                w.after(1000, lambda: tick(n - 1))
            else:
                cv.itemconfig(arc_id, extent=-360, outline=self.colors.get("error","#f38ba8"))
                cv.itemconfig(num_id, text="●", fill=self.colors.get("error","#f38ba8"))
                cv.itemconfig(hint_id, text=self.lang["recording_btn"], fill=self.colors.get("error","#f38ba8"))
                w.after(400, w.destroy); self.start_recording()
        tick(3)

    def _make_rec_frames(self) -> list:
        from PIL import ImageFont
        frames = []
        for bright in (True, False):
            w, h = 72, 28
            img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
            d = ImageDraw.Draw(img)
            d.rounded_rectangle([0, 0, w-1, h-1], radius=10, fill=(20, 20, 30, 195))
            d.ellipse([8, 8, 20, 20], fill=(232, 66, 86, 255) if bright else (160, 40, 55, 200))
            try: font = ImageFont.truetype("segoeui.ttf", 13)
            except:
                try: font = ImageFont.truetype("arial.ttf", 13)
                except: font = ImageFont.load_default()
            d.text((26, 6), "REC", font=font, fill=(220, 220, 230, 255))
            frames.append(img)
        return frames

    def _composite_overlays_on_preview(self, img: "Image.Image", pw: int, ph: int) -> "Image.Image":
        overlays = getattr(self, 'overlays', [])
        if not overlays:
            return img
        try:
            img = img.convert("RGBA")
            for ov in overlays:
                if not ov.get('enabled', True):
                    continue
                kind = ov.get('kind', 'text')
                if kind == 'text':
                    text = ov.get('text', '')
                    if not text:
                        continue
                    x = int(ov.get('x', 0.05) * pw)
                    y = int(ov.get('y', 0.05) * ph)
                    rw = getattr(self, 'record_width', 0) or getattr(self, 'original_width', 0) or pw
                    fs_full = int(ov.get('font_size', 24))
                    fs = max(6, int(fs_full * (pw / max(1, rw))))
                    col = ov.get('color', '#ffffff')
                    opacity = ov.get('opacity', 1.0)
                    try:
                        alpha = max(0, min(255, int(opacity * 255)))
                        r = int(col[1:3], 16); g = int(col[3:5], 16); b = int(col[5:7], 16)
                    except Exception:
                        r, g, b, alpha = 255, 255, 255, 255
                    from PIL import ImageFont
                    font = getattr(self, '_preview_overlay_font_cache', {}).get(fs)
                    if font is None:
                        try: font = ImageFont.truetype("segoeui.ttf", fs)
                        except Exception:
                            try: font = ImageFont.truetype("arial.ttf", fs)
                            except Exception: font = ImageFont.load_default()
                        cache = getattr(self, '_preview_overlay_font_cache', {})
                        cache[fs] = font
                        self._preview_overlay_font_cache = cache
                    d = ImageDraw.Draw(img)
                    d.text((x + 1, y + 1), text, font=font, fill=(0, 0, 0, alpha))  # cheap drop shadow
                    d.text((x, y), text, font=font, fill=(r, g, b, alpha))
                elif kind == 'image':
                    path = ov.get('path', '')
                    if not path or not os.path.exists(path):
                        continue
                    ow = max(1, int(ov.get('w', 0.25) * pw))
                    oh = max(1, int(ov.get('h', 0.08) * ph))
                    ox = int(ov.get('x', 0.05) * pw)
                    oy = int(ov.get('y', 0.05) * ph)
                    opacity = ov.get('opacity', 1.0)
                    cache = getattr(self, '_preview_overlay_img_cache', {})
                    cache_key = (path, ow, oh)
                    thumb = cache.get(cache_key)
                    if thumb is None:
                        try:
                            src = Image.open(path).convert("RGBA")
                            src = src.resize((ow, oh), Image.Resampling.BILINEAR)
                            if len(cache) > 16:  # simple cap so this can't grow unbounded
                                cache.clear()
                            cache[cache_key] = src
                            self._preview_overlay_img_cache = cache
                            thumb = src
                        except Exception:
                            continue
                    if opacity < 1.0:
                        a = thumb.split()[-1].point(lambda p: int(p * opacity))
                        thumb = thumb.copy(); thumb.putalpha(a)
                    img.paste(thumb, (ox, oy), thumb)
                elif kind == 'webcam':
                    ow = max(1, int(ov.get('w', 0.25) * pw))
                    oh = max(1, int(ov.get('h', 0.25) * ph))
                    ox = int(ov.get('x', 0.05) * pw)
                    oy = int(ov.get('y', 0.05) * ph)
                    d = ImageDraw.Draw(img)
                    d.rectangle([ox, oy, ox + ow, oy + oh], outline=(137, 180, 250, 255), width=2,
                                fill=(30, 30, 46, 160))
                    d.text((ox + 4, oy + 4), f"📷 Cam {ov.get('cam_index', 0)}", fill=(205, 214, 244, 255))
            return img.convert("RGB")
        except Exception as e:
            log.debug(f"overlay preview composite error: {e}")
            return img.convert("RGB") if img.mode != "RGB" else img

    def _capture_loop(self) -> None:
        # C++ pipeline path ----------------------------------------------
        try:
            from homrec_native import CppPipeline, PIPELINE_OK as _PLK
        except ImportError:
            _PLK = False

        if _PLK and getattr(self, 'cpp_pipeline', None) is None:
            pw = getattr(self, 'preview_width', 640)
            ph = getattr(self, 'preview_height', 360)
            pl = CppPipeline()
            # pipe_write_fd=0 → только превью, без записи (запись стартует отдельно)
            if pl.create(0, 0, getattr(self, 'target_fps', 30), 0, pw, ph):
                if pl.start():
                    self.cpp_pipeline = pl
                    log.info("C++ pipeline started for preview")
                else:
                    pl.destroy()

        if getattr(self, 'cpp_pipeline', None) is not None:
            self._capture_loop_cpp()
            return

        # Python fallback (mss) -----------------------------------------
        import mss as _mss
        sct = _mss.mss()
        try:
            from homrec_native import core as _native_core; _have_native = True
        except Exception:
            _native_core = None; _have_native = False

        while self._preview_running:
            try:
                monitor = getattr(self, 'monitor', None)
                pw = getattr(self, 'preview_width', 640)
                ph = getattr(self, 'preview_height', 360)
                recording = getattr(self, 'recording', False)

                if monitor is None: time.sleep(0.1); continue

                if getattr(self, 'disable_preview', False):
                    try: self._preview_queue.get_nowait()
                    except queue.Empty: pass
                    self._preview_queue.put_nowait(None); time.sleep(0.5); continue

                if recording:
                    # BUG FIX: when paused, skip new screenshots entirely — ffmpeg will duplicate
                    # the last frame automatically.  This saves CPU and avoids stale frames.
                    if getattr(self, 'paused', False):
                        time.sleep(0.05)
                        continue
                    _now = time.monotonic()
                    if _now - getattr(self,'_rec_pv_last_t',0.0) >= 0.25:
                        self._rec_pv_last_t = _now
                        try:
                            screenshot = sct.grab(monitor)
                            sw2, sh2 = screenshot.size
                            if _have_native and _native_core:
                                rgb_np2 = _native_core.bgrx_to_rgb_np(screenshot.bgra, sw2, sh2)
                                small_np2 = _native_core.resize_nearest_np(rgb_np2, sw2, sh2, pw, ph)
                                rec_img = Image.frombuffer("RGB", (pw, ph), small_np2, "raw", "RGB", 0, 1)
                            else:
                                rec_img = Image.frombytes("RGB", screenshot.size, screenshot.bgra, "raw", "BGRX")
                                rec_img = rec_img.resize((pw, ph), Image.Resampling.NEAREST)
                            try: self._preview_queue.get_nowait()
                            except: pass
                            rec_img = self._composite_overlays_on_preview(rec_img, pw, ph)
                            self._preview_queue.put_nowait(rec_img)
                        except Exception: pass
                    time.sleep(0.05); continue

                screenshot = sct.grab(monitor); sw, sh = screenshot.size
                if _have_native and _native_core:
                    rgb_np = _native_core.bgrx_to_rgb_np(screenshot.bgra, sw, sh)
                    small_np = _native_core.resize_bilinear_np(rgb_np, sw, sh, pw, ph)
                    img = Image.frombuffer("RGB", (pw, ph), small_np, "raw", "RGB", 0, 1)
                else:
                    img = Image.frombytes("RGB", screenshot.size, screenshot.bgra, "raw", "BGRX")
                    img.thumbnail((pw, ph), Image.Resampling.BILINEAR)

                try: self._preview_queue.get_nowait()
                except queue.Empty: pass
                img = self._composite_overlays_on_preview(img, pw, ph)
                self._preview_queue.put_nowait(img)

                _now = time.time()
                if not hasattr(self, '_pv_last_t'): self._pv_last_t = _now; self._pv_frame_acc = 0
                self._pv_frame_acc += 1
                if _now - self._pv_last_t >= 2.0:
                    fps_val = self._pv_frame_acc / (_now - self._pv_last_t)
                    self._pv_last_t = _now; self._pv_frame_acc = 0
                    if hasattr(self, '_preview_fps_lbl'):
                        try: self._preview_fps_lbl.config(text=f"{fps_val:.0f} fps")
                        except: pass
            except Exception as e: log.debug(f"_capture_loop error: {e}")
            time.sleep(0.1)  # BUG FIX: was 0.083 (~12 fps) but preview only redraws at 100 ms; align to avoid wasted captures

    def _capture_loop_cpp(self) -> None:
        """
        Цикл превью через C++ pipeline.
        C++ поток захватывает экран с THREAD_PRIORITY_TIME_CRITICAL,
        конвертирует BGRA→RGB thumbnail внутри DLL.
        Python только копирует готовый буфер в PIL Image.
        """
        import time as _t
        pl = self.cpp_pipeline
        while self._preview_running:
            try:
                pw = getattr(self, 'preview_width', 640)
                ph = getattr(self, 'preview_height', 360)

                # Обновляем размер превью если окно изменилось
                if pl._pv_w.value != pw or pl._pv_h.value != ph:
                    pl._pv_buf = __import__('ctypes').create_string_buffer(pw * ph * 3)
                    _pipeline_mod = __import__('homrec_native', fromlist=['_pipeline'])
                    if hasattr(_pipeline_mod, '_pipeline') and _pipeline_mod._pipeline:
                        _pipeline_mod._pipeline.hr_pl_set_preview_size(pl._handle, pw, ph)

                rgb_bytes, w, h = pl.get_preview()
                if rgb_bytes and w > 0 and h > 0:
                    img = Image.frombuffer("RGB", (w, h), rgb_bytes, "raw", "RGB", 0, 1)
                    img = self._composite_overlays_on_preview(img, w, h)
                    try: self._preview_queue.get_nowait()
                    except Exception: pass
                    self._preview_queue.put_nowait(img)

                # Обновляем счётчик кадров из C++ stats
                fc, fd, fps_act = pl.stats()
                self.frame_count = int(fc)
                if fd > 0:
                    pass  # drops уже логирует C++ pipeline

                # Пауза
                if getattr(self, 'paused', False):
                    pl.pause(True)
                else:
                    pl.pause(False)

            except Exception as _e:
                log.debug(f"_capture_loop_cpp error: {_e}")

            _t.sleep(0.033)  # ~30 Hz опрос буфера — C++ захват идёт независимо

    def update_preview(self) -> None:
        # Если виджеты были пересозданы (recreate_widgets сбросил флаг) — останавливаем
        # старый цикл; новый уже запущен recreate_widgets.
        if not getattr(self, '_preview_active', True):
            return
        try:
            img = self._preview_queue.get_nowait()
            if img is None:
                self._show_preview_placeholder()
            else:
                photo = ImageTk.PhotoImage(img)
                self.preview_label.config(image=photo, text="")
                self.preview_label.image = photo
        except queue.Empty: pass
        except Exception: pass
        # BUG FIX: during recording the preview was rescheduled at 250 ms, but the capture
        # loop already throttles to ~250 ms, so the label was always one cycle stale.
        # Idle polling at 80 ms (12.5 fps) was unnecessarily burning CPU for a preview.
        # Use 100 ms idle (10 fps preview is plenty) and 150 ms while recording.
        self.root.after(150 if getattr(self, 'recording', False) else 100, self.update_preview)

    def _show_preview_placeholder(self) -> None:
        try:
            pw = getattr(self, 'preview_width', 640)
            ph = getattr(self, 'preview_height', 360)
            cache_key = (pw, ph)
            if getattr(self, '_placeholder_key', None) != cache_key:
                img = Image.new("RGB", (pw, ph), color="#181825")
                draw = ImageDraw.Draw(img)
                for x in range(0, pw, 20):
                    draw.rectangle([x, 0, x+10, 2], fill="#45475a")
                    draw.rectangle([x, ph-2, x+10, ph], fill="#45475a")
                for y in range(0, ph, 20):
                    draw.rectangle([0, y, 2, y+10], fill="#45475a")
                    draw.rectangle([pw-2, y, pw, y+10], fill="#45475a")
                cx, cy, r = pw//2, ph//2 - 20, 40
                draw.ellipse([cx-r, cy-r, cx+r, cy+r], outline="#45475a", width=3)
                draw.ellipse([cx-15, cy-15, cx+15, cy+15], outline="#45475a", width=2)
                try:
                    from PIL import ImageFont; font = ImageFont.truetype("segoeui.ttf", 16)
                except: font = None
                msg = "Preview disabled"
                try:
                    bbox = draw.textbbox((0,0), msg, font=font); tw = bbox[2]-bbox[0]
                except: tw = len(msg)*8
                draw.text((pw//2 - tw//2, cy+r+16), msg, fill="#6c7086", font=font)
                self._placeholder_photo = ImageTk.PhotoImage(img)
                self._placeholder_key = cache_key
            self.preview_label.config(image=self._placeholder_photo, text="")
            self.preview_label.image = self._placeholder_photo
        except Exception: pass

    def _probe_ddagrab_support(self) -> bool:
        if platform.system() != "Windows":
            return False
        if not getattr(self, 'ffmpeg_path', None) or not os.path.exists(self.ffmpeg_path):
            return False
        try:
            r = subprocess.run(
                [self.ffmpeg_path, '-filters'],
                capture_output=True, text=True, timeout=8,
                creationflags=subprocess.CREATE_NO_WINDOW
            )
            supported = 'ddagrab' in (r.stdout + r.stderr)
            log.info("ddagrab support: %s", supported)
            return supported
        except Exception as e:
            log.debug("ddagrab probe failed: %s", e)
            return False

    def _refresh_overlay_badge(self) -> None:
        if not hasattr(self, '_overlay_badge'):
            return
        enabled = [o for o in getattr(self, 'overlays', []) if o.get('enabled', True)]
        if enabled:
            self._overlay_badge.config(text=f"{len(enabled)} active")
        else:
            self._overlay_badge.config(text="none active")

    def _open_overlays_panel(self) -> None:
        OverlayManagerWindow(self.root, self)

    def _build_overlay_vf(self) -> str:
        filters = []
        w = self.record_width or self.original_width or 1920
        h = self.record_height or self.original_height or 1080
        for ov in getattr(self, 'overlays', []):
            if not ov.get('enabled', True):
                continue
            if ov.get('kind') != 'text':
                continue
            text = ov.get('text', '').replace("'", "\\'").replace(':', '\\:').replace(',', '\\,')
            if not text:
                continue
            x_px = int(ov.get('x', 0.05) * w)
            y_px = int(ov.get('y', 0.05) * h)
            fs   = int(ov.get('font_size', 24))
            col  = ov.get('color', '#ffffff').lstrip('#')
            opacity = ov.get('opacity', 1.0)
            alpha_hex = f"{int(opacity * 255):02x}"
            color_ff = f"0x{col}@0x{alpha_hex}"
            filters.append(
                f"drawtext=text='{text}':x={x_px}:y={y_px}:fontsize={fs}:"
                f"fontcolor={color_ff}:shadowcolor=0x00000080:shadowx=1:shadowy=1"
            )
        return ','.join(filters)

    def _build_filter_graph(self, base_label_in: str = "0:v") -> tuple[list, str, str | None]:
        extra_inputs: list = []
        graph_parts: list = []
        next_input_idx = 1

        needs_scale = (self.record_width != self.original_width or
                       self.record_height != self.original_height)
        cur_label = base_label_in
        if needs_scale:
            graph_parts.append(f"[{cur_label}]scale={self.record_width}:{self.record_height}:flags=fast_bilinear[scaled]")
            cur_label = "scaled"

        text_vf = self._build_overlay_vf()
        if text_vf:
            graph_parts.append(f"[{cur_label}]{text_vf}[txt]")
            cur_label = "txt"

        w = self.record_width or 1920
        h = self.record_height or 1080

        for ov in getattr(self, 'overlays', []):
            if not ov.get('enabled', True):
                continue
            kind = ov.get('kind')

            if kind == 'image':
                path = ov.get('path', '')
                if not path or not os.path.exists(path):
                    continue
                ow = max(2, int(ov.get('w', 0.25) * w))
                oh = max(2, int(ov.get('h', 0.08) * h))
                ox = int(ov.get('x', 0.05) * w)
                oy = int(ov.get('y', 0.05) * h)
                opacity = ov.get('opacity', 1.0)

                extra_inputs += ['-i', path]
                in_label = f"{next_input_idx}:v"
                scaled_label = f"img{next_input_idx}"
                if opacity < 1.0:
                    graph_parts.append(
                        f"[{in_label}]scale={ow}:{oh},format=rgba,"
                        f"colorchannelmixer=aa={opacity:.2f}[{scaled_label}]"
                    )
                else:
                    graph_parts.append(f"[{in_label}]scale={ow}:{oh}[{scaled_label}]")

                out_label = f"ov{next_input_idx}"
                graph_parts.append(
                    f"[{cur_label}][{scaled_label}]overlay={ox}:{oy}[{out_label}]"
                )
                cur_label = out_label
                next_input_idx += 1

            elif kind == 'webcam':
                cam_idx = ov.get('cam_index', 0)
                ow = max(2, int(ov.get('w', 0.25) * w))
                oh = max(2, int(ov.get('h', 0.25) * h))
                ox = int(ov.get('x', 0.05) * w)
                oy = int(ov.get('y', 0.05) * h)

                if platform.system() == 'Windows':
                    cam_args = ['-f', 'dshow', '-video_size', f'{ow}x{oh}',
                                '-i', f'video={self._dshow_cam_name(cam_idx)}']
                else:
                    cam_args = ['-f', 'v4l2', '-video_size', f'{ow}x{oh}',
                                '-i', f'/dev/video{cam_idx}']
                extra_inputs += cam_args
                in_label = f"{next_input_idx}:v"
                scaled_label = f"cam{next_input_idx}"
                graph_parts.append(f"[{in_label}]scale={ow}:{oh}[{scaled_label}]")

                out_label = f"ov{next_input_idx}"
                graph_parts.append(
                    f"[{cur_label}][{scaled_label}]overlay={ox}:{oy}[{out_label}]"
                )
                cur_label = out_label
                next_input_idx += 1

        if not graph_parts:
            return [], "", None

        filter_complex = ';'.join(graph_parts)
        return extra_inputs, filter_complex, cur_label

    def list_webcams(self) -> list[str]:
        """Return a list of available webcam display names (Windows: DirectShow video devices)."""
        try:
            if platform.system() == 'Windows':
                cached = getattr(self, '_dshow_cam_names_cache', None)
                if cached is None:
                    result = subprocess.run(
                        [self.ffmpeg_path, '-list_devices', 'true', '-f', 'dshow', '-i', 'dummy'],
                        capture_output=True, text=True, timeout=8,
                        creationflags=subprocess.CREATE_NO_WINDOW)
                    names = []
                    for line in result.stderr.splitlines():
                        if '(video)' in line and '"' in line:
                            names.append(line.split('"')[1])
                    self._dshow_cam_names_cache = names
                    cached = names
                return cached or ["Integrated Webcam"]
            else:
                # On Linux, enumerate /dev/video* nodes as a fallback.
                found = sorted(glob.glob('/dev/video*'))
                return found or ["/dev/video0"]
        except Exception as e:
            log.debug(f"webcam list probe failed: {e}")
            return ["Integrated Webcam"]

    def _dshow_cam_name(self, cam_index: int) -> str:
        try:
            cached = getattr(self, '_dshow_cam_names_cache', None)
            if cached is None:
                result = subprocess.run(
                    [self.ffmpeg_path, '-list_devices', 'true', '-f', 'dshow', '-i', 'dummy'],
                    capture_output=True, text=True, timeout=8,
                    creationflags=subprocess.CREATE_NO_WINDOW)
                names = []
                for line in result.stderr.splitlines():
                    if '(video)' in line and '"' in line:
                        names.append(line.split('"')[1])
                self._dshow_cam_names_cache = names
                cached = names
            if cached and 0 <= cam_index < len(cached):
                return cached[cam_index]
        except Exception as e:
            log.debug(f"dshow cam name probe failed: {e}")
        return "Integrated Webcam"

    def toggle_recording(self) -> None:
        if not self.recording: self.start_recording()
        else: self.stop_recording()

    def start_recording(self) -> None:
        try:
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            fmt = getattr(self, 'video_format', 'mp4')
            # BUG FIX: filename_template was stored/loaded but never actually used; apply it now.
            template = getattr(self, 'filename_template', 'HomRec_{date}_{time}')
            date_str = datetime.now().strftime("%Y%m%d")
            time_str = datetime.now().strftime("%H%M%S")
            base_name = template.replace('{date}', date_str).replace('{time}', time_str)
            # Strip characters that are invalid in filenames on Windows/Linux
            import re as _re
            base_name = _re.sub(r'[\\/:*?"<>|]', '_', base_name)
            self.filename = f"{self.output_folder}/{base_name}.{fmt}"
            log.info(f"Starting recording: {self.filename}")
            self._notify_recording_start()
            if not self.ffmpeg_path: raise Exception("FFmpeg not found!")

            self.stop_flag = False; self.paused = False; self.frame_count = 0
            if hasattr(self, 'ffmpeg_reader_thread') and self.ffmpeg_reader_thread and self.ffmpeg_reader_thread.is_alive():
                self.ffmpeg_reader_thread.join(timeout=2)

            fps = self.target_fps
            codec_args = self._build_codec_args()
            draw_mouse = '1' if getattr(self, 'cursor_var', None) and self.cursor_var.get() else '0'

            gdi_flags = ['-thread_queue_size','512','-probesize','32',
                         '-analyzeduration','0',
                         '-fflags','nobuffer+genpts','-rtbufsize','256M']
            out_flags = ['-vsync','0','-flush_packets','1','-max_muxing_queue_size','4096']
            if fmt == 'mp4':
                out_flags += ['-movflags', '+faststart']
            elif fmt == 'mkv':
                out_flags += ['-cluster_size_limit', '2M']
            safe_pix_fmt = self._safe_pix_fmt()

            extra_inputs, filter_complex, out_pad = self._build_filter_graph()
            if out_pad:
                graph_args = ['-filter_complex', filter_complex, '-map', f'[{out_pad}]']
            else:
                graph_args = []

            use_ddagrab = getattr(self, 'use_ddagrab', None)
            if use_ddagrab is None:
                use_ddagrab = self._probe_ddagrab_support()
                self.use_ddagrab = use_ddagrab

            if use_ddagrab and self.capture_mode != "window":
                dda_flags = ['-thread_queue_size', '512', '-fflags', 'nobuffer']
                mon_idx = max(0, getattr(self, 'monitor_id', 1) - 1)
                dda_src = (
                    f'ddagrab=output_idx={mon_idx}'
                    f':framerate={fps}:draw_mouse={draw_mouse}:dup_frames=1'
                )
                dda_input = ['-f', 'lavfi', '-i', dda_src]
                dda_vf = ['-vf', self._ddagrab_vf()]
                if graph_args:
                    cmd = [self.ffmpeg_path, '-y', *dda_flags, *dda_input,
                           *dda_vf, *extra_inputs, *graph_args,
                           *codec_args, '-pix_fmt', 'yuv420p', *out_flags, '-an', self.filename]
                else:
                    cmd = [self.ffmpeg_path, '-y', *dda_flags, *dda_input,
                           *dda_vf, *codec_args,
                           '-pix_fmt', 'yuv420p', *out_flags, '-an', self.filename]
            elif self.capture_mode == "window" and self.capture_window_title:
                cmd = [self.ffmpeg_path, '-y', *gdi_flags,
                       '-f', 'gdigrab', '-framerate', str(fps),
                       '-draw_mouse', draw_mouse,
                       '-i', f'title={self.capture_window_title}',
                       *extra_inputs, *graph_args, *codec_args,
                       '-pix_fmt', safe_pix_fmt, *out_flags, '-an', self.filename]
            else:
                cmd = [self.ffmpeg_path, '-y', *gdi_flags,
                       '-f', 'gdigrab', '-framerate', str(fps),
                       '-draw_mouse', draw_mouse,
                       '-offset_x', str(self.monitor_left),
                       '-offset_y', str(self.monitor_top),
                       '-video_size', f'{self.original_width}x{self.original_height}',
                       '-i', 'desktop',
                       *extra_inputs, *graph_args, *codec_args,
                       '-pix_fmt', safe_pix_fmt, *out_flags, '-an', self.filename]

            log.debug(f"FFmpeg cmd: {' '.join(cmd)}")

            # Выбор пути запуска ffmpeg ---------------------------------
            _pl = getattr(self, 'cpp_pipeline', None)
            _use_cpp_pipe = (
                _pl is not None
                and use_ddagrab  # C++ pipeline использует DXGI, как и ddagrab
                and not graph_args  # с overlays пока только ffmpeg-путь
            )

            if _use_cpp_pipe:
                # C++ pipeline path -------------------------------------
                # Open pipe: C++ → YUV420p → ffmpeg stdin
                import os as _os
                r_fd, w_fd = _os.pipe()
                self._cpp_pipe_read_fd  = r_fd
                self._cpp_pipe_write_fd = w_fd

                # Переключаем pipeline на режим записи
                _pl.set_recording(True, w_fd)
                log.info("C++ pipeline: recording via pipe fd=%d", w_fd)

                # Строим команду ffmpeg для чтения из pipe (YUV420p)
                w_src = self.record_width  or self.original_width  or 1920
                h_src = self.record_height or self.original_height or 1080
                pipe_input = [
                    '-f',            'rawvideo',
                    '-pixel_format', 'yuv420p',
                    '-video_size',   f'{w_src}x{h_src}',
                    '-framerate',    str(fps),
                    '-i',            f'pipe:{r_fd}',
                ]
                pipe_cmd = [
                    self.ffmpeg_path, '-y',
                    *pipe_input,
                    *codec_args,
                    '-pix_fmt', 'yuv420p',
                    *out_flags, '-an', self.filename,
                ]
                log.debug(f"FFmpeg pipe cmd: {' '.join(pipe_cmd)}")

                try:
                    from homrec_native import FfmpegProcess as _FP
                    self._native_ffmpeg = _FP(pipe_cmd)
                    # subprocess.Popen-совместимый stub для stop_recording
                    self.ffmpeg_proc = None
                    self._using_native_ffmpeg = True
                    log.info("C++ FfmpegProcess started (pipe mode)")
                except Exception as _e:
                    log.warning(f"Native FfmpegProcess failed ({_e}), falling back")
                    _pl.set_recording(False, 0)
                    _os.close(r_fd); _os.close(w_fd)
                    self._cpp_pipe_read_fd = self._cpp_pipe_write_fd = -1
                    self._using_native_ffmpeg = False
                    # Fallback: обычный subprocess
                    self.ffmpeg_proc = subprocess.Popen(
                        cmd, stdout=subprocess.DEVNULL,
                        stderr=subprocess.PIPE, stdin=subprocess.PIPE,
                        creationflags=subprocess.CREATE_NO_WINDOW if platform.system()=="Windows" else 0
                    )
            else:
                # Python / ddagrab путь (без C++ pipeline) -------------
                self._using_native_ffmpeg = False
                self.ffmpeg_proc = subprocess.Popen(
                    cmd,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                    stdin=subprocess.PIPE,
                    creationflags=subprocess.CREATE_NO_WINDOW if platform.system()=="Windows" else 0
                )
                try:
                    import psutil as _ps; _fp = _ps.Process(self.ffmpeg_proc.pid)
                    _fp.nice(_ps.HIGH_PRIORITY_CLASS if platform.system()=="Windows" else -10)
                except Exception: pass

            # Stderr reader (только для subprocess-пути)
            self.stop_ffmpeg_reader = False
            if self.ffmpeg_proc is not None:
                self.ffmpeg_reader_thread = threading.Thread(
                    target=self._ffmpeg_reader, daemon=True, name='ffmpeg-stderr'
                )
                self.ffmpeg_reader_thread.start()
            else:
                self.ffmpeg_reader_thread = None

            if self.audio_panel.audio_enabled.get(): self.start_audio_recording()
            self.recording = True; self.start_time = time.time()
            self._set_taskbar_icon(recording=True)
            self.record_btn.config(text=self.lang["stop"], bg=self.colors["error"], command=self.stop_recording)
            self.pause_btn.config(state="normal"); self.stop_btn.config(state="normal")
            self.status_icon.config(fg=self.colors["success"])
            self.status_label.config(text=self.lang["recording"])
            self._update_stats()
        except Exception as e:
            messagebox.showerror(self.lang["error"], f"Failed to start recording:\n{str(e)}")
            log.exception("Failed to start recording")

    def _ffmpeg_reader(self) -> None:
        while not self.stop_flag and not self.stop_ffmpeg_reader and self.ffmpeg_proc and self.ffmpeg_proc.poll() is None:
            try:
                line = self.ffmpeg_proc.stderr.readline()
                if not line: break
                line = line.decode('utf-8', errors='ignore').strip()
                if 'frame=' in line:
                    import re as _re
                    m = _re.search(r'frame=\s*(\d+)', line)
                    if m: self.frame_count = int(m.group(1))
            except: break

    def _update_stats(self) -> None:
        if self.recording:
            try:
                elapsed = time.time() - self.start_time
                if elapsed > 0 and self.frame_count > 0:
                    self.fps_label.config(text=f"{self.lang['fps']} {self.frame_count/elapsed:.1f}")
                h = int(elapsed // 3600); m = int((elapsed % 3600) // 60); s = int(elapsed % 60)
                self.time_label.config(text=f"{h:02d}:{m:02d}:{s:02d}")
                # BUG FIX: auto_stop_min was stored/loaded but never checked — auto-stop never fired.
                auto_stop = getattr(self, 'auto_stop_min', 0)
                if auto_stop > 0 and elapsed >= auto_stop * 60:
                    log.info(f"Auto-stop triggered after {auto_stop} min")
                    self.stop_recording()
                    return
                proc_alive = (
                    (self.ffmpeg_proc and self.ffmpeg_proc.poll() is None)
                    or getattr(self, '_using_native_ffmpeg', False)
                )
                if proc_alive:
                    # BUG FIX: _last_ffmpeg_size_kb was read but never written, so the status
                    # bar always showed "0 MB".  Read the actual file size from disk instead.
                    try:
                        fn = getattr(self, 'filename', '')
                        if fn and os.path.exists(fn):
                            _kb = os.path.getsize(fn) // 1024
                            self._last_ffmpeg_size_kb = _kb
                        else:
                            _kb = getattr(self, '_last_ffmpeg_size_kb', 0)
                    except OSError:
                        _kb = getattr(self, '_last_ffmpeg_size_kb', 0)
                    _mb = _kb / 1024.0
                    _sz = f"{_mb:.1f} MB" if _mb >= 1.0 else f"{_kb} KB"
                    try:
                        self.file_label.config(
                            text=self.lang['recording_status'].format(
                                size=_mb, frames=self.frame_count))
                    except Exception:
                        self.file_label.config(text=f"{_sz}  {self.frame_count} кадров")
            except: pass
            self.root.after(1000, self._update_stats)

    def stop_recording(self) -> None:
        log.info("Stopping recording...")
        self.recording = False; self.stop_flag = True
        saved_filename = self.filename; saved_start_time = self.start_time
        saved_record_width = self.record_width; saved_record_height = self.record_height; saved_target_fps = self.target_fps

        self._set_taskbar_icon(recording=False)
        self.record_btn.config(text=self.lang["start"], bg=self.colors["success"], command=self.start_with_countdown)
        self.pause_btn.config(state="disabled", text=self.lang["pause"])
        self.stop_btn.config(state="disabled")
        self.status_icon.config(fg=self.colors["warning"])
        self.status_label.config(text="Saving…")
        self.time_label.config(text="00:00:00")
        self.file_label.config(text="Processing…")

        def _finalize():
            self.stop_ffmpeg_reader = True

            if getattr(self, '_using_native_ffmpeg', False):
                # C++ pipeline остановка --------------------------------
                _pl = getattr(self, 'cpp_pipeline', None)
                if _pl:
                    # Сначала останавливаем запись в pipe (C++ перестаёт писать YUV)
                    _pl.set_recording(False, 0)
                    log.info("C++ pipeline: recording stopped")

                # Закрываем write-конец pipe → ffmpeg получает EOF → финализирует файл
                import os as _os
                wfd = getattr(self, '_cpp_pipe_write_fd', -1)
                rfd = getattr(self, '_cpp_pipe_read_fd',  -1)
                if wfd >= 0:
                    try: _os.close(wfd)
                    except OSError: pass
                    self._cpp_pipe_write_fd = -1

                # Ждём завершения ffmpeg (30 с — время финализации moov-атома)
                nff = getattr(self, '_native_ffmpeg', None)
                if nff:
                    clean = nff.stop(timeout_ms=30000)
                    log.info(f"Native ffmpeg stopped cleanly: {clean}")
                    del nff; self._native_ffmpeg = None

                if rfd >= 0:
                    try: _os.close(rfd)
                    except OSError: pass
                    self._cpp_pipe_read_fd = -1
                self._using_native_ffmpeg = False

            elif self.ffmpeg_proc and self.ffmpeg_proc.poll() is None:
                # subprocess путь ---------------------------------------
                try: 
                    self.ffmpeg_proc.stdin.write(b'q')
                    self.ffmpeg_proc.stdin.flush()
                except: 
                    pass
                
                # Wait max 3 seconds for graceful exit (was 30 seconds)
                try:
                    self.ffmpeg_proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    # Force kill if stuck
                    try:
                        self.ffmpeg_proc.terminate()
                        self.ffmpeg_proc.wait(timeout=1)
                    except:
                        try:
                            self.ffmpeg_proc.kill()
                        except:
                            pass

            audio_file = None
            if self.audio_recording: audio_file = self.stop_audio_recording()
            time.sleep(0.25)

            has_ffmpeg = self.check_ffmpeg(); audio_merged = False
            mp3_file = None
            if audio_file and os.path.exists(audio_file) and getattr(self, 'separate_audio_mp3', False) and has_ffmpeg:
                mp3_path = os.path.splitext(saved_filename)[0] + '.mp3'
                try:
                    mp3_cmd = [self.ffmpeg_path, '-y', '-i', audio_file, '-codec:a', 'libmp3lame',
                               '-q:a', '2', '-ar', str(getattr(self, 'audio_sample_rate', 44100)), mp3_path]
                    subprocess.run(mp3_cmd, capture_output=True, timeout=60,
                                   creationflags=subprocess.CREATE_NO_WINDOW if platform.system()=='Windows' else 0)
                    if os.path.exists(mp3_path):
                        mp3_file = mp3_path
                        log.info(f"Separate MP3 saved: {mp3_path}")
                except Exception as e:
                    log.warning(f"MP3 export failed: {e}")

            if audio_file and os.path.exists(saved_filename) and self.audio_panel.audio_enabled.get():
                if has_ffmpeg:
                    self.root.after(0, lambda: self.file_label.config(text="Merging audio…"))
                    audio_merged = self.merge_audio_video(saved_filename, audio_file)

            self.root.after(0, lambda: self._finalize_ui(saved_filename, saved_start_time, saved_record_width, saved_record_height, saved_target_fps, audio_file, audio_merged, has_ffmpeg, mp3_file))

        threading.Thread(target=_finalize, daemon=True).start()

    def _finalize_ui(self, filename, start_time, rec_width, rec_height, target_fps, audio_file, audio_merged, has_ffmpeg, mp3_file=None) -> None:
        self.status_icon.config(fg=self.colors["error"])
        self.status_label.config(text=self.lang["ready"])

        if os.path.exists(filename):
            file_size = os.path.getsize(filename) / (1024 * 1024)
            duration = time.time() - start_time
            try:
                probe_cmd = [self.ffmpeg_path, '-i', filename, '-f', 'null', '-']
                probe_result = subprocess.run(probe_cmd, capture_output=True, text=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW if platform.system()=='Windows' else 0)
                import re
                for line in probe_result.stderr.split('\n'):
                    if 'Duration:' in line:
                        match = re.search(r'Duration: (\d+):(\d+):([\d.]+)', line)
                        if match:
                            h, m, s = match.groups()
                            duration = int(h)*3600 + int(m)*60 + float(s)
                        break
            except Exception: pass

            self.file_label.config(text=self.lang["saved"].format(size=file_size, duration=duration))
            audio_status = self.lang["merged"] if audio_merged else (self.lang["separate"] if audio_file else self.lang["no_audio"])
            info_lines = [
                f"{self.lang['file']} {os.path.basename(filename)}",
                f"{self.lang['size']} {file_size:.1f} MB",
                f"{self.lang['duration']} {duration:.1f} sec",
                f"{self.lang['resolution']} {rec_width}x{rec_height}",
                f"{self.lang['fps']} {target_fps}",
                f"{self.lang['audio']} {audio_status}",
            ]
            if audio_file and not audio_merged:
                info_lines.append(f"{self.lang['audio_file']} {os.path.basename(audio_file)}")
            if mp3_file and os.path.exists(mp3_file):
                info_lines.append(f"🎵 MP3: {os.path.basename(mp3_file)}")
            if not has_ffmpeg and audio_file:
                info_lines.extend(["", self.lang["ffmpeg_not_found_msg"]])

            if self.show_summary:
                dont_show_var = tk.BooleanVar(value=False)
                result = CustomMessageBox.show(self, "recording_saved", "recording_saved", "\n".join(info_lines), dont_show_var)
                if dont_show_var.get():
                    self.show_summary = False; self.save_settings(silent=True)
                if result: self.open_recordings()
        else:
            self.file_label.config(text=self.lang["recording_failed"])
            messagebox.showerror(self.lang["error"], self.lang["recording_failed"])

    def toggle_pause(self) -> None:
        if self.recording:
            self.paused = not self.paused
            if self.paused:
                # Pause audio too
                if self.audio_recording and hasattr(self, '_ae') and self._ae:
                    try:
                        self._ae.pause()
                    except Exception as e:
                        log.warning(f"Audio pause failed: {e}")
                
                self.pause_btn.config(text=self.lang["resume"], bg=self.colors["success"])
                self.status_icon.config(fg=self.colors["warning"])
                self.status_label.config(text=self.lang["paused"])
                self._pause_start = time.time()
            else:
                # Resume audio
                if self.audio_recording and hasattr(self, '_ae') and self._ae:
                    try:
                        self._ae.resume()
                    except Exception as e:
                        log.warning(f"Audio resume failed: {e}")
                
                self.pause_btn.config(text=self.lang["pause"], bg=self.colors["warning"])
                self.status_icon.config(fg=self.colors["success"])
                self.status_label.config(text=self.lang["recording"])
                if hasattr(self, '_pause_start'):
                    self.start_time += time.time() - self._pause_start
                    del self._pause_start

    def _show_welcome_and_save(self) -> None:
        self.save_settings(silent=True); WelcomeDialog.show(self)

    def _manual_update_check(self) -> None:
        def _fetch():
            import urllib.request, json as _json
            try:
                url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
                req = urllib.request.Request(url, headers={"User-Agent": "HomRec"})
                with urllib.request.urlopen(req, timeout=5) as resp:
                    data = _json.loads(resp.read().decode())
                tag = data.get("tag_name", "").lstrip("v")
                if tag and _version_gt(tag, CURRENT_VERSION):
                    self.root.after(0, lambda: messagebox.showinfo("Update available", f"HomRec v{tag} is available!\n\nhttps://github.com/{GITHUB_REPO}/releases"))
                else:
                    self.root.after(0, lambda: messagebox.showinfo("No updates", f"You have the latest version (v{CURRENT_VERSION})."))
            except Exception as e:
                self.root.after(0, lambda: messagebox.showerror("Error", f"Could not check for updates:\n{e}"))
        threading.Thread(target=_fetch, daemon=True).start()

    def _open_issues(self) -> None:
        webbrowser.open(f"https://github.com/{GITHUB_REPO}/issues")

    def _start_update_check(self) -> None:
        check_for_updates(self._on_update_found)

    def _on_update_found(self, latest: str) -> None:
        self.root.after(0, lambda: self._show_update_banner(latest))

    def _show_update_banner(self, latest: str) -> None:
        try:
            if hasattr(self, '_update_btn') and self._update_btn.winfo_exists():
                self._update_btn.destroy()

            def _do_download():
                self._update_btn.config(text="⬇ Downloading…", state="disabled", bg="#f9e2af")
                def _fetch():
                    try:
                        import urllib.request, json as _json, tempfile as _tmp
                        url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
                        req = urllib.request.Request(url, headers={"User-Agent": "HomRec"})
                        with urllib.request.urlopen(req, timeout=10) as resp:
                            data = _json.loads(resp.read().decode())
                        exe_url = exe_name = None
                        for asset in data.get("assets", []):
                            name = asset.get("name", "").lower()
                            if name.endswith(".exe") or name.endswith(".zip"):
                                exe_url = asset.get("browser_download_url"); exe_name = asset.get("name", "HomRec_setup.exe"); break
                        if exe_url:
                            dest = os.path.join(_tmp.gettempdir(), exe_name)
                            urllib.request.urlretrieve(exe_url, dest)
                            self.root.after(0, lambda: self._update_btn.config(text="✅ Downloaded!", bg="#a6e3a1", state="normal", command=lambda: os.startfile(dest)))
                        else:
                            self.root.after(0, lambda: (webbrowser.open(f"https://github.com/{GITHUB_REPO}/releases/latest"), self._update_btn.config(text="⬇ Download", bg="#a6e3a1", state="normal")))
                    except Exception as e:
                        log.warning(f"Auto-download failed: {e}")
                        self.root.after(0, lambda: (webbrowser.open(f"https://github.com/{GITHUB_REPO}/releases/latest"), self._update_btn.config(text="⬇ Download", bg="#a6e3a1", state="normal")))
                threading.Thread(target=_fetch, daemon=True).start()

            self._update_btn = tk.Button(self.root, text=f"⬇ v{latest} available", command=_do_download,
                                          bg="#a6e3a1", fg="#1e1e2e", font=("Segoe UI", 9, "bold"),
                                          relief="flat", padx=10, pady=4, cursor="hand2", bd=0)
            self._update_btn.place(relx=1.0, rely=1.0, anchor="se", x=-12, y=-12)
        except Exception as e: log.warning(f"Failed to show update button: {e}")

    def on_closing(self) -> None:
        if HAS_TRAY and self.tray_icon and self.minimize_to_tray.get():
            self.root.withdraw()
        else:
            self.quit_app()

    def quit_app(self) -> None:
        if self.recording:
            if not messagebox.askyesno(self.lang["warning"], "Recording in progress! Stop and exit?"): return
            if self.ffmpeg_proc and self.ffmpeg_proc.poll() is None:
                try: self.ffmpeg_proc.kill()
                except: pass
            self.recording = False; self.audio_recording = False; self.sys_audio_recording = False
        self._preview_running = False
        # Уничтожаем C++ pipeline — освобождает DXGI и D3D ресурсы
        _pl = getattr(self, 'cpp_pipeline', None)
        if _pl:
            try: _pl.destroy()
            except Exception: pass
            self.cpp_pipeline = None
        if self.tray_icon:
            try: self.tray_icon.stop()
            except: pass
        self.stop_flag = True
        self.root.after(100, self.root.destroy)

    def setup_tray(self) -> None:
        if not HAS_TRAY: return
        try:
            icons_dir = os.path.join(_ROOT_DIR, "icons")
            tray_ico = os.path.join(icons_dir, "tray.ico")
            main_ico = os.path.join(icons_dir, "main.ico")
            if os.path.exists(tray_ico):
                img = Image.open(tray_ico).convert("RGBA")
            elif os.path.exists(main_ico):
                img = Image.open(main_ico).convert("RGBA")
            else:
                img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
                d = ImageDraw.Draw(img)
                d.ellipse([4, 4, 60, 60], fill="#89b4fa"); d.ellipse([20, 20, 44, 44], fill="#1e1e2e"); d.ellipse([28, 28, 36, 36], fill="#f38ba8")

            menu = pystray.Menu(TrayItem("Show HomRec", self._tray_show, default=True), TrayItem("Start / Stop", self._tray_toggle), pystray.Menu.SEPARATOR, TrayItem("Quit", self._tray_quit))
            self.tray_icon = pystray.Icon("HomRec", img, "HomRec", menu)
            threading.Thread(target=self.tray_icon.run, daemon=True).start()
        except Exception as e:
            log.warning(f"Tray setup failed: {e}"); self.tray_icon = None

    def _tray_show(self, icon=None, item=None) -> None: self.root.after(0, self.root.deiconify)
    def _tray_toggle(self, icon=None, item=None) -> None: self.root.after(0, self.toggle_recording)
    def _tray_quit(self, icon=None, item=None) -> None: self.root.after(0, self.quit_app)

    def set_capture_desktop(self) -> None:
        self.capture_mode = "desktop"; self.capture_window_title = ""

    def get_open_windows(self) -> list[str]:
        if sys.platform != "win32": return []
        titles = []
        EnumWindows = ctypes.windll.user32.EnumWindows
        EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int))
        GetWindowText = ctypes.windll.user32.GetWindowTextW
        GetWindowTextLength = ctypes.windll.user32.GetWindowTextLengthW
        IsWindowVisible = ctypes.windll.user32.IsWindowVisible

        def callback(hwnd, lparam):
            if IsWindowVisible(hwnd):
                length = GetWindowTextLength(hwnd)
                if length > 0:
                    buf = ctypes.create_unicode_buffer(length + 1)
                    GetWindowText(hwnd, buf, length + 1)
                    title = buf.value.strip()
                    if title and title not in titles: titles.append(title)
            return True

        EnumWindows(EnumWindowsProc(callback), 0)
        return sorted(titles)

    def open_window_picker(self) -> None:
        windows = self.get_open_windows()
        if not windows: messagebox.showinfo("Info", "No open windows found."); return

        dlg = tk.Toplevel(self.root); self._set_icon(dlg)
        dlg.title("🖥  Select Window to Record"); dlg.geometry("520x420")
        dlg.configure(bg=self.colors["bg"]); dlg.transient(self.root); dlg.grab_set()
        dlg.resizable(False, True); dlg.minsize(480, 360)
        dlg.update_idletasks()
        dlg.geometry(f"+{self.root.winfo_x()+self.root.winfo_width()//2-260}+{self.root.winfo_y()+self.root.winfo_height()//2-210}")

        hdr = tk.Frame(dlg, bg=self.colors.get("surface","#313244"), pady=12)
        hdr.pack(fill="x")
        tk.Label(hdr, text="🖥  Select a window to record", bg=self.colors.get("surface","#313244"), fg=self.colors["accent"], font=("Segoe UI", 12, "bold")).pack(anchor="w", padx=16)
        tk.Label(hdr, text=f"{len(windows)} windows found", bg=self.colors.get("surface","#313244"), fg=self.colors.get("text_secondary","#a6adc8"), font=("Segoe UI", 9)).pack(anchor="w", padx=16)
        tk.Frame(dlg, bg=self.colors["accent"], height=2).pack(fill="x")

        frame = tk.Frame(dlg, bg=self.colors["bg"])
        frame.pack(fill="both", expand=True, padx=15, pady=5)
        scrollbar = tk.Scrollbar(frame); scrollbar.pack(side="right", fill="y")
        listbox = tk.Listbox(frame, yscrollcommand=scrollbar.set, bg=self.colors["surface"], fg=self.colors["text"], selectbackground=self.colors["accent"], font=("Segoe UI", 10), relief="flat", activestyle="none", borderwidth=0)
        listbox.pack(side="left", fill="both", expand=True)
        scrollbar.config(command=listbox.yview)
        for w in windows: listbox.insert(tk.END, w)
        if self.capture_window_title in windows:
            idx = windows.index(self.capture_window_title)
            listbox.selection_set(idx); listbox.see(idx)

        btn_frame = tk.Frame(dlg, bg=self.colors["bg"])
        btn_frame.pack(fill="x", padx=15, pady=12)

        def on_select():
            sel = listbox.curselection()
            if sel:
                self.capture_window_title = windows[sel[0]]; self.capture_mode = "window"; dlg.destroy()

        tk.Button(btn_frame, text="Record this window", command=on_select, bg=self.colors["accent"], fg=self.colors["bg"], font=("Segoe UI", 10, "bold"), relief="flat", padx=16, pady=6).pack(side="left", padx=(0, 8))
        tk.Button(btn_frame, text="Use full desktop", command=lambda: (self.set_capture_desktop(), dlg.destroy()), bg=self.colors["surface"], fg=self.colors["text"], font=("Segoe UI", 10), relief="flat", padx=16, pady=6).pack(side="left")


if __name__ == "__main__":
    import platform as _platform
    if _platform.system() == "Windows":
        _mutex = ctypes.windll.kernel32.CreateMutexW(None, False, "HomRec_SingleInstance_150")
        if ctypes.windll.kernel32.GetLastError() == 183:
            sys.exit(0)
    root = tk.Tk()
    app = HomRecScreen(root)

    try:
        from hr_plugin_engine import init_plugin_engine
        app._plugins_dir = os.path.join(_ROOT_DIR, 'plugins')
        app.plugin_engine = init_plugin_engine(app)
    except Exception as _pe:
        log.warning(f"Plugin engine failed to load: {_pe}")
    root.mainloop()