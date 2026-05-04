from __future__ import annotations
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import time
import os
from datetime import datetime
import cv2
import numpy as np
from PIL import Image, ImageTk, ImageDraw
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
import pyaudio
import wave
import audioop
import subprocess
import shutil
import platform

import logging

# ==================== LOGGING SETUP ====================
def setup_logging() -> None:
    log_dir = os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) else os.path.dirname(os.path.abspath(__file__))
    log_path = os.path.join(log_dir, "homrec.log")
    logging.basicConfig(
        level=logging.DEBUG,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
        handlers=[
            logging.FileHandler(log_path, encoding="utf-8"),
        ]
    )

setup_logging()
log = logging.getLogger("homrec")

# For PC Analytics
try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    HAS_PSUTIL = False

# For system tray
try:
    import pystray
    from pystray import MenuItem as TrayItem
    HAS_TRAY = True
except ImportError:
    HAS_TRAY = False

# ==================== VERSION & UPDATE CHECK ====================
CURRENT_VERSION = "1.4.2"
GITHUB_REPO = "homaaio/homrec"

def check_for_updates(callback) -> None:
    """Fetch latest release tag from GitHub in a background thread.
    Calls callback(latest_version: str) if a newer version is found.
    """
    def _fetch():
        try:
            import urllib.request
            import json as _json
            url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
            req = urllib.request.Request(url, headers={"User-Agent": "HomRec"})
            with urllib.request.urlopen(req, timeout=5) as resp:
                data = _json.loads(resp.read().decode())
            tag = data.get("tag_name", "").lstrip("v")
            if tag and _version_gt(tag, CURRENT_VERSION):
                log.info(f"Update available: v{tag}")
                callback(tag)
            else:
                log.info("No updates found")
        except Exception as e:
            log.warning(f"Update check failed: {e}")

    threading.Thread(target=_fetch, daemon=True).start()

def _version_gt(a: str, b: str) -> bool:
    """Return True if version string a is greater than b."""
    try:
        return tuple(int(x) for x in a.split(".")) > tuple(int(x) for x in b.split("."))
    except:
        return False

# ==================== LANGUAGE FILES ====================
LANGUAGES = {
    "en": {
        "app_title": "HomRec (v1.4.0)",
        "live_preview": "PREVIEW",
        "ready": "Ready",
        "recording": "Recording",
        "paused": "Paused",
        "fps": "FPS:",
        "resolution": "Resolution:",
        "start": "▶ START",
        "pause": "⏸ PAUSE",
        "stop": "■ STOP",
        "resume": "▶ RESUME",
        "recording_btn": "⏺ RECORDING",
        "audio_mixer": "Audio Mixer",
        "microphone": "Microphone",
        "desktop_audio": "Desktop Audio",
        "mute": "Mute",
        "unmute": "Unmute",
        "vol": "Vol:",
        "level": "Level:",
        "enable_audio": "Enable Audio",
        "ffmpeg_found": "FFmpeg: ✅ Found",
        "ffmpeg_not_found": "FFmpeg: ❌ Not Found",
        "file_menu": "File",
        "open_recordings": "Open Recordings Folder",
        "exit": "Exit",
        "view_menu": "View",
        "always_on_top": "Always on Top",
        "fullscreen": "Fullscreen  F11",
        "pc_analytics": "PC Analytics",
        "cpu_info": "CPU Info",
        "ram_info": "RAM Info",
        "disk_info": "Disk Info",
        "help_menu": "Help",
        "check_updates": "Check for Updates",
        "report_issue": "Report Issue",
        "capture_source": "Capture Source",
        "full_desktop": "Full Desktop",
        "select_window": "Select Window...",
        "minimize_tray": "Minimize to tray on close",
        "language": "Language",
        "english": "English",
        "russian": "Русский",
        "theme": "Theme",
        "dark": "Dark",
        "light": "Light",
        "settings_menu": "Settings",
        "preferences": "Preferences...",
        "performance_menu": "Performance",
        "ultra": "Ultra (60 FPS)",
        "turbo": "Turbo (30 FPS)",
        "balanced": "Balanced (15 FPS)",
        "eco": "Eco (8 FPS)",
        "stats": "STATS",
        "time": "TIME",
        "status": "STATUS",
        "warning": "Warning",
        "error": "Error",
        "info": "Info",
        "folder_not_exist": "Folder doesn't exist!",
        "recording_failed": "Recording failed!",
        "settings_saved": "Settings saved!",
        "recording_saved": "✅ Recording Saved!",
        "open_folder": "Open folder?",
        "ffmpeg_not_found_msg": "⚠️ FFmpeg not found - audio separate",
        "saved": "✅ Saved: {size:.1f} MB | {duration:.1f}s",
        "recording_status": "Recording: {size:.1f} MB | {frames} frames",
        "file": "📁 File:",
        "size": "📊 Size:",
        "duration": "⏱️ Duration:",
        "audio": "🎤 Audio:",
        "merged": "Merged",
        "separate": "Separate",
        "no_audio": "No",
        "save": "Save",
        "cancel": "Cancel",
        "browse": "Browse",
        "output_folder": "Output folder:",
        "settings_title": "Settings",
        "video_settings": "Video",
        "quality": "Quality:",
        "mode": "Mode:",
        "advanced": "Advanced",
        "monitor": "Monitor:",
        "output": "Output:",
        "countdown": "Countdown (3s)",
        "timestamp": "Timestamp",
        "cursor": "Cursor",
        "notification": "Show summary",
        "made_by": "Homa4ella",
        "audio_file": "🎵 Audio file:",
    },
    "ru": {
        "app_title": "HomRec (v1.4.0)",
        "live_preview": "ПРЕДПРОСМОТР",
        "ready": "Готов",
        "recording": "Запись",
        "paused": "Пауза",
        "fps": "FPS:",
        "resolution": "Разрешение:",
        "start": "▶ СТАРТ",
        "pause": "⏸ ПАУЗА",
        "stop": "■ СТОП",
        "resume": "▶ ПРОДОЛЖИТЬ",
        "recording_btn": "⏺ ЗАПИСЬ",
        "audio_mixer": "Аудио Микшер",
        "microphone": "Микрофон",
        "desktop_audio": "Системный звук",
        "mute": "Выкл",
        "unmute": "Вкл",
        "vol": "Громк:",
        "level": "Уровень:",
        "enable_audio": "Запись звука",
        "ffmpeg_found": "FFmpeg: ✅ Найден",
        "ffmpeg_not_found": "FFmpeg: ❌ Не найден",
        "file_menu": "Файл",
        "open_recordings": "Открыть папку",
        "exit": "Выход",
        "view_menu": "Вид",
        "always_on_top": "Поверх окон",
        "fullscreen": "Полный экран F11",
        "pc_analytics": "Аналитика",
        "cpu_info": "Инфо CPU",
        "ram_info": "Инфо RAM",
        "disk_info": "Инфо диска",
        "language": "Язык",
        "english": "English",
        "russian": "Русский",
        "theme": "Тема",
        "dark": "Темная",
        "light": "Светлая",
        "settings_menu": "Настройки",
        "preferences": "Параметры...",
        "performance_menu": "Производительность",
        "ultra": "Ультра (60 FPS)",
        "turbo": "Турбо (30 FPS)",
        "balanced": "Средний (15 FPS)",
        "eco": "Эко (8 FPS)",
        "stats": "СТАТИСТИКА",
        "time": "ВРЕМЯ",
        "status": "СТАТУС",
        "warning": "Предупреждение",
        "error": "Ошибка",
        "info": "Информация",
        "folder_not_exist": "Папка не существует!",
        "recording_failed": "Ошибка записи!",
        "settings_saved": "Настройки сохранены!",
        "recording_saved": "✅ Запись сохранена!",
        "open_folder": "Открыть папку?",
        "ffmpeg_not_found_msg": "⚠️ FFmpeg не найден - аудио отдельно",
        "saved": "✅ Сохранено: {size:.1f} МБ | {duration:.1f}с",
        "recording_status": "Запись: {size:.1f} МБ | {frames} кадров",
        "file": "📁 Файл:",
        "size": "📊 Размер:",
        "duration": "⏱️ Длительность:",
        "audio": "🎤 Аудио:",
        "merged": "Объединено",
        "separate": "Отдельно",
        "no_audio": "Нет",
        "save": "Сохранить",
        "cancel": "Отмена",
        "browse": "Обзор",
        "output_folder": "Папка записей:",
        "settings_title": "Настройки",
        "video_settings": "Видео",
        "quality": "Качество:",
        "mode": "Режим:",
        "advanced": "Дополнительно",
        "monitor": "Монитор:",
        "output": "Папка:",
        "countdown": "Отсчет (3с)",
        "timestamp": "Время",
        "cursor": "Курсор",
        "notification": "Показывать сводку",
        "made_by": "Homa4ella",
        "audio_file": "🎵 Аудио файл:",
        "help_menu": "Справка",
        "check_updates": "Проверить обновления",
        "report_issue": "Сообщить об ошибке",
        "capture_source": "Источник",
        "full_desktop": "Весь экран",
        "select_window": "Выбрать окно...",
        "minimize_tray": "Сворачивать в трей",
    }
}

# ==================== HELPER FUNCTIONS ====================

def find_ffmpeg() -> str | None:
    """Find FFmpeg in system or in program directory.

    When running as a PyInstaller .exe:
      - __file__ points to the temp _MEIXXXXXX unpack folder, NOT the .exe folder
      - os.getcwd() is wherever the user launched from, NOT the .exe folder
      - sys.executable is always the actual .exe path, so its directory IS the
        folder the user placed ffmpeg.exe next to the app.
    """
    # 1. Folder containing the running .exe (or .py script)
    if getattr(sys, 'frozen', False):
        # PyInstaller sets sys.frozen=True and sys.executable = path to .exe
        app_dir = os.path.dirname(sys.executable)
    else:
        app_dir = os.path.dirname(os.path.abspath(__file__))

    for name in ('ffmpeg.exe', 'ffmpeg'):
        candidate = os.path.join(app_dir, name)
        if os.path.exists(candidate):
            return candidate

    # 2. Same directory as cwd (fallback, works when running .py directly)
    for name in ('ffmpeg.exe', 'ffmpeg'):
        if os.path.exists(name):
            return os.path.abspath(name)

    # 3. System PATH
    ffmpeg_path = shutil.which("ffmpeg")
    if ffmpeg_path:
        return ffmpeg_path

    return None

def optimize_for_performance() -> None:
    """Apply optimizations for low-end PCs"""
    try:
        import psutil
        p = psutil.Process()
        p.nice(psutil.HIGH_PRIORITY_CLASS)
    except:
        pass
    
    cv2.setNumThreads(0)

class AudioLevelMeter(tk.Canvas):
    def __init__(self, parent, width: int = 180, height: int = 24, **kwargs) -> None:
        super().__init__(parent, width=width, height=height, highlightthickness=0, **kwargs)
        self.width = width
        self.height = height
        self.level = 0
        self.draw_meter()
    
    def draw_meter(self) -> None:
        self.delete("all")
        self.create_rectangle(0, 0, self.width, self.height, fill='#45475a', outline='')
        bar_width = int((self.level / 100) * (self.width - 4))
        if bar_width > 0:
            color = '#a6e3a1' if self.level < 70 else '#f9e2af' if self.level < 90 else '#f38ba8'
            self.create_rectangle(2, 2, bar_width, self.height-2, fill=color, outline='')
        for i in range(0, 101, 20):
            x = int((i / 100) * self.width)
            self.create_line(x, 0, x, self.height, fill='#1e1e2e', width=1)
    
    def set_level(self, level: int) -> None:
        self.level = max(0, min(100, level))
        self.draw_meter()

class CustomMessageBox:
    @staticmethod
    def show(app, title_key: str, message_key: str, info_text: str, dont_show_var: tk.BooleanVar) -> bool:
        dialog = tk.Toplevel()
        dialog.title(app.lang[title_key])
        dialog.geometry("500x400")
        dialog.configure(bg=app.colors["bg"])
        dialog.transient()
        dialog.grab_set()
        dialog.resizable(False, False)
        
        dialog.update_idletasks()
        x = (dialog.winfo_screenwidth() // 2) - 250
        y = (dialog.winfo_screenheight() // 2) - 200
        dialog.geometry(f"+{x}+{y}")
        
        icon_label = tk.Label(dialog, text="✅", font=("Segoe UI", 48), bg=app.colors["bg"], fg='#a6e3a1')
        icon_label.pack(pady=(20, 10))
        
        tk.Label(dialog, text=app.lang[message_key], font=("Segoe UI", 14, "bold"),
                bg=app.colors["bg"], fg=app.colors["fg"]).pack(pady=(0, 10))
        
        info_frame = tk.Frame(dialog, bg=app.colors["surface"], relief='flat', bd=2)
        info_frame.pack(pady=10, padx=20, fill='both', expand=True)
        info_label = tk.Label(info_frame, text=info_text, justify='left',
                              bg=app.colors["surface"], fg=app.colors["text"],
                              font=("Consolas", 10))
        info_label.pack(pady=15, padx=15)
        
        check_frame = tk.Frame(dialog, bg=app.colors["bg"])
        check_frame.pack(pady=10)
        dont_show_text = "Don't show again" if app.current_language == "en" else "Больше не показывать"
        dont_show_check = tk.Checkbutton(check_frame, text=dont_show_text,
                                         variable=dont_show_var,
                                         bg=app.colors["bg"], fg=app.colors["fg"],
                                         selectcolor=app.colors["surface"],
                                         font=("Segoe UI", 9))
        dont_show_check.pack()
        
        btn_frame = tk.Frame(dialog, bg=app.colors["bg"])
        btn_frame.pack(pady=15)
        result = {'value': False}
        
        def on_yes() -> None:
            result['value'] = True
            dialog.destroy()
        
        def on_no() -> None:
            result['value'] = False
            dialog.destroy()
        
        tk.Button(btn_frame, text=app.lang["open_folder"], command=on_yes,
                  bg='#a6e3a1', fg=app.colors["bg"],
                  font=("Segoe UI", 10, "bold"),
                  relief='flat', padx=20, pady=8, width=12).pack(side='left', padx=5)
        
        tk.Button(btn_frame, text=app.lang["cancel"], command=on_no,
                  bg=app.colors["surface"], fg=app.colors["text"],
                  font=("Segoe UI", 10),
                  relief='flat', padx=20, pady=8, width=12).pack(side='left', padx=5)
        
        dialog.wait_window()
        return result['value']

class AudioPanel:
    def __init__(self, parent, app) -> None:
        self.app = app
        self.frame = tk.LabelFrame(parent, text=app.lang["audio_mixer"], 
                                   bg=app.colors["surface"], fg=app.colors["accent"],
                                   font=("Segoe UI", 11, "bold"),
                                   padx=10, pady=10)
        self.frame.pack(side='left', fill='both', expand=True, padx=(5, 0))
        
        self.audio_enabled = tk.BooleanVar(value=True)
        self.mic_mute = tk.BooleanVar(value=False)
        self.sys_mute = tk.BooleanVar(value=False)
        self.audio_stream = None
        self.audio_p = None
        
        self.create_mixer_layout()
    
    def create_mic_section(self) -> None:
        pass  # built inside create_mixer_layout

    def create_system_section(self) -> None:
        pass  # built inside create_mixer_layout

    def create_devices_section(self) -> None:
        pass  # built inside create_mixer_layout

    def create_mixer_layout(self) -> None:
        """Horizontal layout: Mic on left, Desktop Audio on right, controls at bottom."""
        c = self.app.colors

        # ── top row: two channel strips side by side ──────────────────────
        channels = tk.Frame(self.frame, bg=c["surface"])
        channels.pack(fill='x', pady=(0, 4))

        # Mic strip
        mic_strip = tk.Frame(channels, bg=c["surface"],
                             relief='flat', bd=0)
        mic_strip.pack(side='left', fill='both', expand=True, padx=(0, 8))

        mic_header = tk.Frame(mic_strip, bg=c["surface"])
        mic_header.pack(fill='x')
        tk.Label(mic_header, text=self.app.lang["microphone"],
                 bg=c["surface"], fg='#a6e3a1',
                 font=("Segoe UI", 9, 'bold')).pack(side='left')
        self.mic_mute_btn = tk.Button(mic_header, text=self.app.lang["mute"],
                                      command=self.toggle_mic_mute,
                                      bg=c["surface_light"], fg=c["text"],
                                      font=("Segoe UI", 8), relief='flat',
                                      width=5, cursor='hand2')
        self.mic_mute_btn.pack(side='right')

        mic_vol_row = tk.Frame(mic_strip, bg=c["surface"])
        mic_vol_row.pack(fill='x', pady=2)
        tk.Label(mic_vol_row, text=self.app.lang["vol"],
                 bg=c["surface"], fg=c["text"],
                 font=("Segoe UI", 8)).pack(side='left')
        self.mic_volume = tk.Scale(mic_vol_row, from_=0, to=100,
                                   orient='horizontal', length=110,
                                   bg=c["surface_light"], fg=c["text"],
                                   highlightthickness=0,
                                   troughcolor=c["surface"],
                                   command=self.on_mic_volume_change,
                                   showvalue=False)
        self.mic_volume.set(80)
        self.mic_volume.pack(side='left', padx=4)
        self.mic_volume_label = tk.Label(mic_vol_row, text="80%",
                                         bg=c["surface"], fg='#a6e3a1',
                                         font=("Segoe UI", 8, 'bold'), width=4)
        self.mic_volume_label.pack(side='left')

        mic_meter_row = tk.Frame(mic_strip, bg=c["surface"])
        mic_meter_row.pack(fill='x', pady=2)
        tk.Label(mic_meter_row, text=self.app.lang["level"],
                 bg=c["surface"], fg=c["text"],
                 font=("Segoe UI", 8)).pack(side='left')
        self.mic_meter = AudioLevelMeter(mic_meter_row, width=130, height=14,
                                         bg=c["surface"])
        self.mic_meter.pack(side='left', padx=4)

        # Divider
        tk.Frame(channels, bg=c["surface_light"], width=1).pack(side='left', fill='y', padx=4)

        # System audio strip
        sys_strip = tk.Frame(channels, bg=c["surface"])
        sys_strip.pack(side='left', fill='both', expand=True, padx=(8, 0))

        sys_header = tk.Frame(sys_strip, bg=c["surface"])
        sys_header.pack(fill='x')
        tk.Label(sys_header, text=self.app.lang["desktop_audio"],
                 bg=c["surface"], fg='#89b4fa',
                 font=("Segoe UI", 9, 'bold')).pack(side='left')
        self.sys_mute_btn = tk.Button(sys_header, text=self.app.lang["mute"],
                                      command=self.toggle_sys_mute,
                                      bg=c["surface_light"], fg=c["text"],
                                      font=("Segoe UI", 8), relief='flat',
                                      width=5, cursor='hand2')
        self.sys_mute_btn.pack(side='right')

        sys_vol_row = tk.Frame(sys_strip, bg=c["surface"])
        sys_vol_row.pack(fill='x', pady=2)
        tk.Label(sys_vol_row, text=self.app.lang["vol"],
                 bg=c["surface"], fg=c["text"],
                 font=("Segoe UI", 8)).pack(side='left')
        self.sys_volume = tk.Scale(sys_vol_row, from_=0, to=100,
                                   orient='horizontal', length=110,
                                   bg=c["surface_light"], fg=c["text"],
                                   highlightthickness=0,
                                   troughcolor=c["surface"],
                                   command=self.on_sys_volume_change,
                                   showvalue=False)
        self.sys_volume.set(50)
        self.sys_volume.pack(side='left', padx=4)
        self.sys_volume_label = tk.Label(sys_vol_row, text="50%",
                                          bg=c["surface"], fg='#89b4fa',
                                          font=("Segoe UI", 8, 'bold'), width=4)
        self.sys_volume_label.pack(side='left')

        sys_meter_row = tk.Frame(sys_strip, bg=c["surface"])
        sys_meter_row.pack(fill='x', pady=2)
        tk.Label(sys_meter_row, text=self.app.lang["level"],
                 bg=c["surface"], fg=c["text"],
                 font=("Segoe UI", 8)).pack(side='left')
        self.sys_meter = AudioLevelMeter(sys_meter_row, width=130, height=14,
                                          bg=c["surface"])
        self.sys_meter.pack(side='left', padx=4)

        # ── bottom row: enable audio + ffmpeg status ───────────────────────
        bottom = tk.Frame(self.frame, bg=c["surface"])
        bottom.pack(fill='x', pady=(4, 0))

        self.audio_check = tk.Checkbutton(bottom, text=self.app.lang["enable_audio"],
                                          variable=self.audio_enabled,
                                          bg=c["surface"], fg=c["text"],
                                          selectcolor=c["surface_light"],
                                          font=("Segoe UI", 9, 'bold'))
        self.audio_check.pack(side='left')

        ffmpeg_text = self.app.lang["ffmpeg_found"] if self.app.check_ffmpeg() else self.app.lang["ffmpeg_not_found"]
        ffmpeg_color = '#a6e3a1' if self.app.check_ffmpeg() else '#f38ba8'
        self.ffmpeg_label = tk.Label(bottom, text=ffmpeg_text,
                                      bg=c["surface"], fg=ffmpeg_color,
                                      font=("Segoe UI", 8))
        self.ffmpeg_label.pack(side='right')
    
    def on_mic_volume_change(self, value: str) -> None:
        self.mic_volume_label.config(text=f"{int(float(value))}%")
    
    def on_sys_volume_change(self, value: str) -> None:
        self.sys_volume_label.config(text=f"{int(float(value))}%")
    
    def toggle_mic_mute(self) -> None:
        self.mic_mute.set(not self.mic_mute.get())
        self.mic_mute_btn.config(bg='#f38ba8' if self.mic_mute.get() else self.app.colors["surface_light"],
                                 text=self.app.lang["unmute"] if self.mic_mute.get() else self.app.lang["mute"])
    
    def toggle_sys_mute(self) -> None:
        self.sys_mute.set(not self.sys_mute.get())
        self.sys_mute_btn.config(bg='#f38ba8' if self.sys_mute.get() else self.app.colors["surface_light"],
                                 text=self.app.lang["unmute"] if self.sys_mute.get() else self.app.lang["mute"])
    
    def update_mic_level(self, level: int) -> None:
        self.mic_meter.set_level(level)
    
    def update_sys_level(self, level: int) -> None:
        self.sys_meter.set_level(level)
    
    def update_language(self) -> None:
        self.frame.config(text=self.app.lang["audio_mixer"])
        self.ffmpeg_label.config(
            text=self.app.lang["ffmpeg_found"] if self.app.check_ffmpeg() else self.app.lang["ffmpeg_not_found"]
        )
        self.audio_check.config(text=self.app.lang["enable_audio"])
        self.mic_mute_btn.config(text=self.app.lang["unmute"] if self.mic_mute.get() else self.app.lang["mute"])
        self.sys_mute_btn.config(text=self.app.lang["unmute"] if self.sys_mute.get() else self.app.lang["mute"])


# ── Schema versioning ────────────────────────────────────────────────────────
# Bump LANG_SCHEMA_VERSION when new language keys are added
# Bump THEME_SCHEMA_VERSION when new theme color keys are added
LANG_SCHEMA_VERSION  = 1
THEME_SCHEMA_VERSION = 1

# Required keys for each schema
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
    "monitor","output","countdown","timestamp","cursor","notification","made_by",
    "audio_file",
]

THEME_REQUIRED_KEYS = ["bg","surface","accent","text","text_secondary",
                        "success","warning","error"]

ASSETS_DIR   = "Assets"
THEMES_DIR   = os.path.join(ASSETS_DIR, "Themes")
LANGS_DIR    = os.path.join(ASSETS_DIR, "L")

# ── HomRec binary file format helpers ────────────────────────────────────────
# Format: 4-byte magic header + gzip-compressed JSON body
# HRC = profile, HRL = language, HRT = theme

_HRC_MAGIC = b'HRC\x01'
_HRL_MAGIC = b'HRL\x01'
_HRT_MAGIC = b'HRT\x01'

def _hrc_write(path: str, data: dict, magic: bytes) -> None:
    """Write a HomRec binary file (magic header + gzip JSON)."""
    body = gzip.compress(json.dumps(data, indent=2, ensure_ascii=False).encode('utf-8'))
    with open(path, 'wb') as f:
        f.write(magic)
        f.write(body)

def _hrc_read(path: str, expected_magic: bytes) -> dict:
    """Read a HomRec binary file. Raises ValueError if magic doesn't match."""
    with open(path, 'rb') as f:
        magic = f.read(4)
        body = f.read()
    if magic != expected_magic:
        raise ValueError(f"Invalid file format. Expected {expected_magic!r}, got {magic!r}")
    return json.loads(gzip.decompress(body).decode('utf-8'))

def _hrc_detect(path: str) -> str:
    """Return 'hrc', 'hrl', 'hrt' or raise ValueError."""
    with open(path, 'rb') as f:
        magic = f.read(4)
    if magic == _HRC_MAGIC: return 'hrc'
    if magic == _HRL_MAGIC: return 'hrl'
    if magic == _HRT_MAGIC: return 'hrt'
    raise ValueError(f"Not a HomRec file (magic={magic!r})")



class LanguageEditorDialog:
    """Built-in language editor. Load eng.hrl or rus.hrl as base, translate, save."""

    def __init__(self, parent, app) -> None:
        self.app = app
        self.dialog = tk.Toplevel(parent)
        self.dialog.title("Language Editor")
        self.dialog.geometry("700x560")
        self.dialog.resizable(True, True)
        self.dialog.configure(bg=app.colors["bg"])
        self.dialog.grab_set()
        self.dialog.after(50, self._set_icon)
        self._data = {}
        self._vars = {}
        self._missing = set()
        self._build_ui()

    def _set_icon(self) -> None:
        try:
            ico = os.path.join(os.path.dirname(os.path.abspath(__file__)), "main.ico")
            if os.path.exists(ico):
                self.dialog.iconbitmap(ico)
        except Exception:
            pass

    def _build_ui(self) -> None:
        a = self.app
        c = a.colors

        # Top bar
        top = tk.Frame(self.dialog, bg=c["bg"])
        top.pack(fill="x", padx=14, pady=(12, 4))
        tk.Label(top, text="Language Editor", bg=c["bg"], fg=c["accent"],
                 font=("Segoe UI", 13, "bold")).pack(side="left")

        # Load buttons
        btn_row = tk.Frame(self.dialog, bg=c["bg"])
        btn_row.pack(fill="x", padx=14, pady=(0, 6))
        tk.Label(btn_row, text="Load base:", bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI", 9)).pack(side="left", padx=(0, 6))
        tk.Button(btn_row, text="English", command=lambda: self._load_builtin("en"),
                  bg=c["surface"], fg=c["text"], font=("Segoe UI", 9),
                  relief="flat", padx=10, pady=4).pack(side="left", padx=2)
        tk.Button(btn_row, text="Russian", command=lambda: self._load_builtin("ru"),
                  bg=c["surface"], fg=c["text"], font=("Segoe UI", 9),
                  relief="flat", padx=10, pady=4).pack(side="left", padx=2)
        tk.Button(btn_row, text="Open .hrl...", command=self._load_file,
                  bg=c["surface"], fg=c["text"], font=("Segoe UI", 9),
                  relief="flat", padx=10, pady=4).pack(side="left", padx=8)
        self._status_lbl = tk.Label(btn_row, text="Load a base language to start.",
                                    bg=c["bg"], fg=c["text_secondary"],
                                    font=("Segoe UI", 8))
        self._status_lbl.pack(side="left", padx=8)

        # Lang name row
        name_row = tk.Frame(self.dialog, bg=c["bg"])
        name_row.pack(fill="x", padx=14, pady=(0, 4))
        tk.Label(name_row, text="Language name:", bg=c["bg"], fg=c["text"],
                 font=("Segoe UI", 10)).pack(side="left")
        self._name_var = tk.StringVar(value="My Language")
        tk.Entry(name_row, textvariable=self._name_var,
                 bg=c["surface"], fg=c["text"], font=("Segoe UI", 10),
                 relief="flat", width=24).pack(side="left", padx=8)

        # Scrollable key grid
        outer = tk.Frame(self.dialog, bg=c["surface"])
        outer.pack(fill="both", expand=True, padx=14, pady=4)

        canvas = tk.Canvas(outer, bg=c["bg"], highlightthickness=0)
        vsb = ttk.Scrollbar(outer, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=vsb.set)
        vsb.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)

        self._grid = tk.Frame(canvas, bg=c["bg"])
        self._canvas_window = canvas.create_window((0, 0), window=self._grid, anchor="nw")
        self._grid.bind("<Configure>",
                        lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>",
                    lambda e: canvas.itemconfig(self._canvas_window, width=e.width))
        self._canvas = canvas
        self._grid.bind("<MouseWheel>",
                        lambda e: canvas.yview_scroll(-1*(e.delta//120), "units"))

        # Headers
        tk.Label(self._grid, text="Key", bg=c["bg"], fg=c["accent"],
                 font=("Segoe UI", 9, "bold"), width=24, anchor="w").grid(
                     row=0, column=0, padx=(8,4), pady=2, sticky="w")
        tk.Label(self._grid, text="English reference", bg=c["bg"], fg=c["accent"],
                 font=("Segoe UI", 9, "bold"), width=30, anchor="w").grid(
                     row=0, column=1, padx=4, pady=2, sticky="w")
        tk.Label(self._grid, text="Your translation", bg=c["bg"], fg=c["accent"],
                 font=("Segoe UI", 9, "bold"), anchor="w").grid(
                     row=0, column=2, padx=4, pady=2, sticky="ew")
        self._grid.columnconfigure(2, weight=1)

        self._field_frame = self._grid

        # Bottom buttons
        sep = tk.Frame(self.dialog, bg=c["surface"], height=1)
        sep.pack(fill="x", padx=14, pady=(4, 0))
        bot = tk.Frame(self.dialog, bg=c["bg"])
        bot.pack(fill="x", padx=14, pady=8)
        tk.Button(bot, text="Validate", command=self._validate,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="left", padx=(0,6))
        tk.Button(bot, text="Cancel", command=self.dialog.destroy,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="right", padx=(6,0))
        tk.Button(bot, text="Save As .hrl", command=self._save,
                  bg=self.app.colors["success"], fg=self.app.colors["bg"],
                  font=("Segoe UI", 9, "bold"), relief="flat", padx=16, pady=6).pack(side="right")

    def _build_fields(self) -> None:
        c = self.app.colors
        en = LANGUAGES["en"]
        # Clear old fields
        for w in self._field_frame.winfo_children():
            if int(w.grid_info().get("row", 0)) > 0:
                w.destroy()
        self._vars = {}

        for i, key in enumerate(LANG_REQUIRED_KEYS):
            row = i + 1
            en_val = en.get(key, "")
            cur_val = self._data.get(key, "")
            is_missing = key not in self._data

            # Colour: yellow if missing, normal otherwise
            fg = c["warning"] if is_missing else c["text"]

            tk.Label(self._field_frame, text=key, bg=c["bg"], fg=c["text_secondary"],
                     font=("Consolas", 8), width=24, anchor="w").grid(
                         row=row, column=0, padx=(8,4), pady=1, sticky="w")
            tk.Label(self._field_frame, text=en_val[:40], bg=c["bg"], fg=c["text_secondary"],
                     font=("Segoe UI", 8), width=30, anchor="w").grid(
                         row=row, column=1, padx=4, pady=1, sticky="w")
            var = tk.StringVar(value=cur_val)
            entry = tk.Entry(self._field_frame, textvariable=var,
                             bg=c["surface"], fg=fg, font=("Segoe UI", 9),
                             relief="flat")
            entry.grid(row=row, column=2, padx=(4,8), pady=1, sticky="ew")
            self._vars[key] = (var, entry)

        self._update_status()

    def _load_builtin(self, code: str) -> None:
        self._data = dict(LANGUAGES[code])
        self._name_var.set(self._data.get("lang_name", code))
        self._build_fields()

    def _load_file(self) -> None:
        path = filedialog.askopenfilename(
            filetypes=[("HomRec Language", "*.hrl"), ("All files", "*.*")],
            title="Open .hrl file"
        )
        if not path:
            return
        try:
            self._data = _hrc_read(path, _HRL_MAGIC)
            self._name_var.set(self._data.get("lang_name", "My Language"))
            self._build_fields()
        except Exception as e:
            messagebox.showerror("Load failed", str(e))

    def _update_status(self) -> None:
        missing = [k for k in LANG_REQUIRED_KEYS if not str(self._vars.get(k, (tk.StringVar(),))[0].get()).strip()]
        total = len(LANG_REQUIRED_KEYS)
        done = total - len(missing)
        c = self.app.colors
        if missing:
            self._status_lbl.config(
                text=f"{done}/{total} translated  ⚠ {len(missing)} missing",
                fg=c["warning"])
        else:
            self._status_lbl.config(
                text=f"✅ All {total} keys translated",
                fg=c["success"])

    def _validate(self) -> None:
        missing = [k for k, (var, entry) in self._vars.items()
                   if not str(var.get()).strip()]
        c = self.app.colors
        for key, (var, entry) in self._vars.items():
            entry.config(fg=c["error"] if not var.get().strip() else c["text"])
        self._update_status()
        if missing:
            messagebox.showwarning("Validation",
                f"{len(missing)} keys are empty:\\n" + ", ".join(missing[:10]) +
                ("..." if len(missing) > 10 else ""))
        else:
            messagebox.showinfo("Validation", "✅ All keys are filled in!")

    def _save(self) -> None:
        if not self._vars:
            messagebox.showwarning("Nothing to save", "Load a base language first.")
            return
        # Collect current values
        data = {key: var.get() for key, (var, _) in self._vars.items()}
        data["lang_name"] = self._name_var.get() or "My Language"
        data["schema_version"] = LANG_SCHEMA_VERSION

        # Validate
        missing = [k for k, v in data.items() if isinstance(v, str) and not v.strip() and k not in ("schema_version", "lang_name")]
        if missing:
            if not messagebox.askyesno("Missing keys",
                f"{len(missing)} keys are empty. Save anyway?\\n"
                "Missing keys will use English as fallback."):
                return

        fname = data["lang_name"].lower().replace(" ", "_") + ".hrl"
        base = os.path.dirname(os.path.abspath(__file__))
        langs_dir = os.path.join(base, LANGS_DIR)
        os.makedirs(langs_dir, exist_ok=True)
        path = filedialog.asksaveasfilename(
            defaultextension=".hrl",
            filetypes=[("HomRec Language", "*.hrl"), ("All files", "*.*")],
            initialfile=fname,
            initialdir=langs_dir,
            title="Save language as"
        )
        if not path:
            return
        try:
            _hrc_write(path, data, _HRL_MAGIC)
            # Auto-copy to Assets/L/ if saved elsewhere
            import shutil
            dst = os.path.join(langs_dir, os.path.basename(path))
            if os.path.abspath(path) != os.path.abspath(dst):
                shutil.copy2(path, dst)
            messagebox.showinfo("Saved",
                f"Language saved and installed:\n{os.path.basename(path)}\n\n"
                f"Restart HomRec to see it in Settings → Language.")
            log.info(f"Language saved: {path}")
        except Exception as e:
            messagebox.showerror("Save failed", str(e))


class ThemeEditorDialog:
    """Built-in theme editor with live color pickers and preview."""

    THEME_KEYS = [
        ("bg",             "Main background",     "Window background color"),
        ("surface",        "Surface / panels",    "Cards, inputs, panels"),
        ("accent",         "Accent",              "Buttons, highlights, active elements"),
        ("text",           "Text",                "Primary text color"),
        ("text_secondary", "Secondary text",      "Labels, hints, secondary info"),
        ("success",        "Success",             "Recording active, positive state"),
        ("warning",        "Warning",             "Alerts, cautions"),
        ("error",          "Error",               "Errors, stop button, mute"),
    ]

    def __init__(self, parent, app) -> None:
        self.app = app
        self.dialog = tk.Toplevel(parent)
        self.dialog.title("Theme Editor")
        self.dialog.geometry("520x480")
        self.dialog.resizable(False, True)
        self.dialog.configure(bg=app.colors["bg"])
        self.dialog.grab_set()
        self.dialog.after(50, self._set_icon)
        self._vars = {}
        self._swatches = {}
        self._build_ui()

    def _set_icon(self) -> None:
        try:
            ico = os.path.join(os.path.dirname(os.path.abspath(__file__)), "main.ico")
            if os.path.exists(ico):
                self.dialog.iconbitmap(ico)
        except Exception:
            pass

    def _build_ui(self) -> None:
        a = self.app
        c = a.colors

        # Title + load
        top = tk.Frame(self.dialog, bg=c["bg"])
        top.pack(fill="x", padx=14, pady=(12, 4))
        tk.Label(top, text="Theme Editor", bg=c["bg"], fg=c["accent"],
                 font=("Segoe UI", 13, "bold")).pack(side="left")

        btn_row = tk.Frame(self.dialog, bg=c["bg"])
        btn_row.pack(fill="x", padx=14, pady=(0, 6))
        for name, code in [("Dark", "dark"), ("Light", "light"),
                            ("Catppuccin", "catppuccin"), ("Nord", "nord"),
                            ("Dracula", "dracula")]:
            tk.Button(btn_row, text=name,
                      command=lambda n=code: self._load_builtin(n),
                      bg=c["surface"], fg=c["text"], font=("Segoe UI", 8),
                      relief="flat", padx=8, pady=3).pack(side="left", padx=2)
        tk.Button(btn_row, text="Open .hrt...", command=self._load_file,
                  bg=c["surface"], fg=c["text"], font=("Segoe UI", 8),
                  relief="flat", padx=8, pady=3).pack(side="left", padx=6)

        # Theme name
        name_row = tk.Frame(self.dialog, bg=c["bg"])
        name_row.pack(fill="x", padx=14, pady=(0, 8))
        tk.Label(name_row, text="Theme name:", bg=c["bg"], fg=c["text"],
                 font=("Segoe UI", 10)).pack(side="left")
        self._name_var = tk.StringVar(value="My Theme")
        tk.Entry(name_row, textvariable=self._name_var,
                 bg=c["surface"], fg=c["text"], font=("Segoe UI", 10),
                 relief="flat", width=22).pack(side="left", padx=8)

        # Color rows
        grid = tk.Frame(self.dialog, bg=c["bg"])
        grid.pack(fill="both", expand=True, padx=14)

        for i, (key, label, desc) in enumerate(self.THEME_KEYS):
            val = c.get(key, "#ffffff")
            var = tk.StringVar(value=val)
            self._vars[key] = var

            tk.Label(grid, text=label, bg=c["bg"], fg=c["text"],
                     font=("Segoe UI", 10), width=18, anchor="w").grid(
                         row=i, column=0, padx=(0,8), pady=5, sticky="w")

            # Color swatch button
            swatch = tk.Button(grid, bg=val, width=3, relief="flat",
                               command=lambda k=key: self._pick_color(k))
            swatch.grid(row=i, column=1, padx=4, pady=5)
            self._swatches[key] = swatch

            # Hex entry
            entry = tk.Entry(grid, textvariable=var, bg=c["surface"], fg=c["text"],
                             font=("Consolas", 10), relief="flat", width=10)
            entry.grid(row=i, column=2, padx=4, pady=5, sticky="w")
            entry.bind("<FocusOut>", lambda e, k=key: self._on_hex_change(k))
            entry.bind("<Return>",   lambda e, k=key: self._on_hex_change(k))

            tk.Label(grid, text=desc, bg=c["bg"], fg=c["text_secondary"],
                     font=("Segoe UI", 8), anchor="w").grid(
                         row=i, column=3, padx=8, pady=5, sticky="w")

        # Bottom
        sep = tk.Frame(self.dialog, bg=c["surface"], height=1)
        sep.pack(fill="x", padx=14, pady=(8, 0))
        bot = tk.Frame(self.dialog, bg=c["bg"])
        bot.pack(fill="x", padx=14, pady=8)
        tk.Button(bot, text="Preview", command=self._preview,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="left")
        tk.Button(bot, text="Cancel", command=self.dialog.destroy,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="right", padx=(6,0))
        tk.Button(bot, text="Save As .hrt", command=self._save,
                  bg=self.app.colors["success"], fg=self.app.colors["bg"],
                  font=("Segoe UI", 9, "bold"), relief="flat", padx=16, pady=6).pack(side="right")

    def _pick_color(self, key: str) -> None:
        from tkinter.colorchooser import askcolor
        current = self._vars[key].get()
        result = askcolor(color=current, title=f"Pick color for {key}",
                          parent=self.dialog)
        if result and result[1]:
            self._vars[key].set(result[1])
            self._swatches[key].config(bg=result[1])

    def _on_hex_change(self, key: str) -> None:
        val = self._vars[key].get().strip()
        if not val.startswith("#"):
            val = "#" + val
        try:
            self.dialog.winfo_rgb(val)  # validates color
            self._vars[key].set(val)
            self._swatches[key].config(bg=val)
        except Exception:
            pass  # invalid hex — ignore

    def _delete_asset(self, name: str, kind: str, combo: ttk.Combobox) -> None:
        """Delete a custom theme or language file."""
        if not name:
            messagebox.showwarning("Nothing selected", f"Select a {kind} to delete.")
            return
        if not messagebox.askyesno("Confirm delete",
                f"Delete {kind} '{name}'?\nThis cannot be undone."):
            return
        base = os.path.dirname(os.path.abspath(__file__))
        if kind == "theme":
            path = os.path.join(base, THEMES_DIR, f"{name}.hrt")
        else:
            path = os.path.join(base, LANGS_DIR, f"{name}.hrl")
        try:
            if os.path.exists(path):
                os.remove(path)
                log.info(f"Deleted {kind}: {path}")
                messagebox.showinfo("Deleted", f"{kind.capitalize()} '{name}' deleted.")
                # Refresh combo
                if kind == "theme":
                    combo.config(values=self.app._scan_custom_themes())
                else:
                    combo.config(values=[c for c, _ in self.app._scan_custom_languages()])
                combo.set("")
            else:
                messagebox.showerror("Not found", f"File not found:\n{path}")
        except Exception as e:
            messagebox.showerror("Delete failed", str(e))

    def _collect(self) -> dict:
        data = {"theme_name": self._name_var.get() or "My Theme",
                "schema_version": THEME_SCHEMA_VERSION}
        for key, var in self._vars.items():
            data[key] = var.get()
        return data

    def _load_builtin(self, name: str) -> None:
        colors = self.app.BUILTIN_THEMES.get(name, self.app.BUILTIN_THEMES["dark"])
        self._name_var.set(name.capitalize())
        for key, var in self._vars.items():
            val = colors.get(key, "#ffffff")
            var.set(val)
            self._swatches[key].config(bg=val)

    def _load_file(self) -> None:
        path = filedialog.askopenfilename(
            filetypes=[("HomRec Theme", "*.hrt"), ("All files", "*.*")],
            title="Open .hrt file"
        )
        if not path:
            return
        try:
            data = _hrc_read(path, _HRT_MAGIC)
            self._name_var.set(data.get("theme_name", "My Theme"))
            for key, var in self._vars.items():
                val = data.get(key, "#ffffff")
                var.set(val)
                self._swatches[key].config(bg=val)
        except Exception as e:
            messagebox.showerror("Load failed", str(e))

    def _preview(self) -> None:
        data = self._collect()
        self.app.colors = {**self.app.BUILTIN_THEMES["dark"], **data}
        self.app.apply_theme()
        messagebox.showinfo("Preview", "Theme applied temporarily.\\nSave to keep it.")

    def _save(self) -> None:
        data = self._collect()
        # Validate hex colors
        bad = []
        for key in THEME_REQUIRED_KEYS:
            try:
                self.dialog.winfo_rgb(data.get(key, ""))
            except Exception:
                bad.append(key)
        if bad:
            messagebox.showerror("Invalid colors",
                f"These colors are invalid: {', '.join(bad)}")
            return

        fname = data["theme_name"].lower().replace(" ", "_") + ".hrt"
        path = filedialog.asksaveasfilename(
            defaultextension=".hrt",
            filetypes=[("HomRec Theme", "*.hrt"), ("All files", "*.*")],
            initialfile=fname,
            title="Save theme as"
        )
        if not path:
            return
        try:
            _hrc_write(path, data, _HRT_MAGIC)
            # Copy to Assets/Themes/ automatically
            themes_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), THEMES_DIR)
            os.makedirs(themes_dir, exist_ok=True)
            import shutil
            shutil.copy2(path, os.path.join(themes_dir, os.path.basename(path)))
            messagebox.showinfo("Saved", f"Theme saved and installed:\\n{path}")
            log.info(f"Theme saved: {path}")
        except Exception as e:
            messagebox.showerror("Save failed", str(e))


class AdvancedSettingsDialog:
    """Power-user settings window with import/export (.hrc)."""

    HRC_VERSION = 1

    def __init__(self, parent: tk.Tk, app) -> None:
        self.app = app
        self.dialog = tk.Toplevel(parent)
        self.dialog.title("Advanced Settings")
        self.dialog.geometry("560x640")
        self.dialog.resizable(False, False)
        self.dialog.configure(bg=app.colors["bg"])
        self.dialog.grab_set()
        try:
            icon_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "main.ico")
            if os.path.exists(icon_path):
                self.dialog.after(50, lambda: self.dialog.iconbitmap(icon_path))
        except Exception:
            pass
        self._build_ui()

    def _build_ui(self) -> None:
        a = self.app
        c = a.colors

        title = tk.Label(self.dialog, text="⚙ Advanced Settings",
                         bg=c["bg"], fg=c["accent"],
                         font=("Segoe UI", 14, "bold"))
        title.pack(pady=(16, 4), padx=20, anchor="w")
        tk.Label(self.dialog, text="For power users. Changes apply on next recording.",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI", 9)).pack(padx=20, anchor="w")

        notebook = ttk.Notebook(self.dialog)
        notebook.pack(fill="both", expand=True, padx=16, pady=12)

        # ── Video tab ────────────────────────────────────────────────
        vt = tk.Frame(notebook, bg=c["bg"])
        notebook.add(vt, text="Video")
        self._cv = tk.StringVar(value=getattr(a, "video_codec", "libx264"))
        self._row(vt, "Codec",
                  ttk.Combobox(vt, textvariable=self._cv,
                               values=["libx264","libx265","h264_nvenc","hevc_nvenc",
                                       "h264_amf","hevc_amf","h264_qsv","hevc_qsv"],
                               width=18, state="readonly"))
        self._hwv = tk.StringVar(value=getattr(a, "hw_accel", "auto"))
        self._row(vt, "HW Accel",
                  ttk.Combobox(vt, textvariable=self._hwv,
                               values=["auto","none","cuda","dxva2","d3d11va"],
                               width=12, state="readonly"))
        self._prev = tk.StringVar(value=getattr(a, "enc_preset", "ultrafast"))
        self._row(vt, "Preset",
                  ttk.Combobox(vt, textvariable=self._prev,
                               values=["ultrafast","superfast","veryfast","faster","fast","medium","slow"],
                               width=12, state="readonly"))
        self._crfv = tk.IntVar(value=getattr(a, "enc_crf", 18))
        self._row(vt, "CRF (quality)",
                  tk.Scale(vt, variable=self._crfv,
                           from_=0, to=51, orient="horizontal", length=180,
                           bg=c["bg"], fg=c["text"], highlightthickness=0,
                           troughcolor=c["surface"]))
        self._pixv = tk.StringVar(value=getattr(a, "pix_fmt", "yuv420p"))
        self._row(vt, "Pixel format",
                  ttk.Combobox(vt, textvariable=self._pixv,
                               values=["yuv420p","yuv444p","rgb24"],
                               width=12, state="readonly"))

        # ── Audio tab ────────────────────────────────────────────────
        at = tk.Frame(notebook, bg=c["bg"])
        notebook.add(at, text="Audio")
        self._srv = tk.StringVar(value=str(getattr(a, "audio_sample_rate", 44100)))
        self._row(at, "Sample rate",
                  ttk.Combobox(at, textvariable=self._srv,
                               values=["44100","48000","96000"],
                               width=10, state="readonly"))
        self._abrv = tk.StringVar(value=getattr(a, "audio_aac_bitrate", "192k"))
        self._row(at, "AAC bitrate",
                  ttk.Combobox(at, textvariable=self._abrv,
                               values=["96k","128k","192k","256k","320k"],
                               width=10, state="readonly"))
        self._achv = tk.StringVar(value=str(getattr(a, "audio_out_channels", 2)))
        self._row(at, "Channels",
                  ttk.Combobox(at, textvariable=self._achv,
                               values=["1","2"],
                               width=6, state="readonly"))

        # ── Interface tab ────────────────────────────────────────────
        it = tk.Frame(notebook, bg=c["bg"])
        notebook.add(it, text="Interface")
        self._thv = tk.StringVar(value=getattr(a, "ui_theme", "dark"))
        self._row(it, "Theme",
                  ttk.Combobox(it, textvariable=self._thv,
                               values=["dark","light","catppuccin","nord","dracula"],
                               width=14, state="readonly"))
        # Editor buttons
        row_te = it.grid_size()[1]
        tk.Button(it, text="🎨 Theme Editor...",
                  command=lambda: ThemeEditorDialog(self.dialog, self.app),
                  bg=c["surface"], fg=c["accent"],
                  font=("Segoe UI", 9), relief="flat", padx=10, pady=5).grid(
                      row=row_te, column=1, sticky="w", padx=(0,20), pady=(8,2))
        row_le = it.grid_size()[1]
        tk.Button(it, text="🌐 Language Editor...",
                  command=lambda: LanguageEditorDialog(self.dialog, self.app),
                  bg=c["surface"], fg=c["accent"],
                  font=("Segoe UI", 9), relief="flat", padx=10, pady=5).grid(
                      row=row_le, column=1, sticky="w", padx=(0,20), pady=2)

        # Separator
        row_sep = it.grid_size()[1]
        tk.Frame(it, bg=c["surface"], height=1).grid(
            row=row_sep, column=0, columnspan=3, sticky="ew", padx=20, pady=(12,4))

        # Delete theme
        row_dt = it.grid_size()[1]
        tk.Label(it, text="Delete theme", bg=c["bg"], fg=c["text"],
                 font=("Segoe UI", 10), anchor="w").grid(
                     row=row_dt, column=0, sticky="w", padx=(20,8), pady=4)
        self._del_theme_var = tk.StringVar()
        theme_files = self.app._scan_custom_themes()
        del_theme_combo = ttk.Combobox(it, textvariable=self._del_theme_var,
                         values=theme_files, width=16, state="readonly")
        del_theme_combo.grid(row=row_dt, column=1, sticky="w", padx=(0,4), pady=4)
        tk.Button(it, text="🗑 Delete",
                  command=lambda: self._delete_asset(
                      self._del_theme_var.get(), "theme", del_theme_combo),
                  bg=c["error"], fg=c["bg"],
                  font=("Segoe UI", 9), relief="flat", padx=8, pady=3).grid(
                      row=row_dt, column=2, sticky="w", pady=4)

        # Delete language
        row_dl = it.grid_size()[1]
        tk.Label(it, text="Delete language", bg=c["bg"], fg=c["text"],
                 font=("Segoe UI", 10), anchor="w").grid(
                     row=row_dl, column=0, sticky="w", padx=(20,8), pady=4)
        self._del_lang_var = tk.StringVar()
        lang_files = [code for code, _ in self.app._scan_custom_languages()]
        del_lang_combo = ttk.Combobox(it, textvariable=self._del_lang_var,
                         values=lang_files, width=16, state="readonly")
        del_lang_combo.grid(row=row_dl, column=1, sticky="w", padx=(0,4), pady=4)
        tk.Button(it, text="🗑 Delete",
                  command=lambda: self._delete_asset(
                      self._del_lang_var.get(), "language", del_lang_combo),
                  bg=c["error"], fg=c["bg"],
                  font=("Segoe UI", 9), relief="flat", padx=8, pady=3).grid(
                      row=row_dl, column=2, sticky="w", pady=4)
        self._uisv = tk.StringVar(value=str(int(getattr(a, "ui_scale", 1.0)*100))+"%")
        self._row(it, "UI scale",
                  ttk.Combobox(it, textvariable=self._uisv,
                               values=["80%","90%","100%","110%","125%"],
                               width=8, state="readonly"))
        self._fontv = tk.StringVar(value=getattr(a, "ui_font", "Segoe UI"))
        self._row(it, "Font",
                  ttk.Combobox(it, textvariable=self._fontv,
                               values=["Segoe UI","Consolas","Arial","Calibri"],
                               width=14, state="readonly"))

        # ── Recording tab ────────────────────────────────────────────
        rt = tk.Frame(notebook, bg=c["bg"])
        notebook.add(rt, text="Recording")
        self._ftv = tk.StringVar(value=getattr(a, "filename_template", "HomRec_{date}_{time}"))
        self._row(rt, "File template",
                  tk.Entry(rt, textvariable=self._ftv,
                           bg=c["surface"], fg=c["text"], font=("Consolas", 10),
                           relief="flat", width=24))
        self._asv = tk.StringVar(value=str(getattr(a, "auto_stop_min", 0)))
        self._row(rt, "Auto-stop (min)",
                  tk.Spinbox(rt, textvariable=self._asv,
                             from_=0, to=480, width=6,
                             bg=c["surface"], fg=c["text"], relief="flat"))
        row = rt.grid_size()[1]
        tk.Label(rt, text="  0 = disabled", bg=c["bg"],
                 fg=c["text_secondary"], font=("Segoe UI", 8)).grid(
                     row=row, column=1, sticky="w", padx=(0, 20))
        self._rbv = tk.StringVar(value=str(getattr(a, "replay_buffer_sec", 0)))
        self._row(rt, "Replay buffer (s)",
                  tk.Spinbox(rt, textvariable=self._rbv,
                             from_=0, to=300, width=6,
                             bg=c["surface"], fg=c["text"], relief="flat"))
        row = rt.grid_size()[1]
        tk.Label(rt, text="  0 = disabled", bg=c["bg"],
                 fg=c["text_secondary"], font=("Segoe UI", 8)).grid(
                     row=row, column=1, sticky="w", padx=(0, 20))

        # ── Hotkeys tab ──────────────────────────────────────────────
        ht = tk.Frame(notebook, bg=c["bg"])
        notebook.add(ht, text="Hotkeys")
        tk.Label(ht, text="Click a field and press any key combination",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI", 9)).grid(row=0, column=0, columnspan=2,
                                             padx=20, pady=(10,4), sticky="w")
        self._hk_ss = tk.StringVar(value=getattr(a, "hotkey_start_stop", "F9"))
        self._hk_p  = tk.StringVar(value=getattr(a, "hotkey_pause", "F10"))
        self._hk_fs = tk.StringVar(value=getattr(a, "hotkey_fullscreen", "F11"))
        for label, var in [("Start / Stop", self._hk_ss),
                            ("Pause / Resume", self._hk_p),
                            ("Fullscreen", self._hk_fs)]:
            entry = tk.Entry(ht, textvariable=var, bg=c["surface"], fg=c["accent"],
                             font=("Consolas", 11), relief="flat", width=12,
                             readonlybackground=c["surface"], state="readonly")
            entry.bind("<FocusIn>",  lambda e, v=var, en=entry: self._start_key_capture(v, en))
            entry.bind("<FocusOut>", lambda e, en=entry: en.config(state="readonly"))
            self._row(ht, label, entry)

        # ── Notifications tab ────────────────────────────────────────
        nt = tk.Frame(notebook, bg=c["bg"])
        notebook.add(nt, text="Notifications")
        self._notif_sound = tk.BooleanVar(value=getattr(a, "notify_sound", True))
        self._notif_flash = tk.BooleanVar(value=getattr(a, "notify_flash", True))
        self._auto_save   = tk.BooleanVar(value=getattr(a, "auto_save_profile", False))
        for text, var in [
            ("Sound beep on recording start", self._notif_sound),
            ("Flash border on recording start", self._notif_flash),
            ("Auto-save profile on exit", self._auto_save),
        ]:
            row = nt.grid_size()[1]
            tk.Checkbutton(nt, text=text, variable=var,
                           bg=c["bg"], fg=c["text"],
                           selectcolor=c["surface"],
                           font=("Segoe UI", 10)).grid(
                               row=row, column=0, columnspan=2,
                               sticky="w", padx=20, pady=4)

        # ── Bottom buttons ───────────────────────────────────────────
        sep = tk.Frame(self.dialog, bg=c["surface"], height=1)
        sep.pack(fill="x", padx=16, pady=(4, 0))

        bot = tk.Frame(self.dialog, bg=c["bg"])
        bot.pack(fill="x", padx=16, pady=10)

        tk.Button(bot, text="⬆ Export .hrc", command=self._export,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="left", padx=(0, 6))
        tk.Button(bot, text="⬇ Import .hrc", command=self._import,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="left")
        tk.Button(bot, text="Cancel", command=self.dialog.destroy,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="right", padx=(6, 0))
        tk.Button(bot, text="Save", command=self._save,
                  bg=c["success"], fg=c["bg"],
                  font=("Segoe UI", 9, "bold"), relief="flat", padx=16, pady=6).pack(side="right")

    def _row(self, parent, label: str, widget) -> None:
        """Add a label+widget row using grid for clean alignment."""
        # Find next available grid row
        row = parent.grid_size()[1]
        tk.Label(parent, text=label, bg=self.app.colors["bg"],
                 fg=self.app.colors["text"],
                 font=("Segoe UI", 10), anchor="w").grid(
                     row=row, column=0, sticky="w", padx=(20, 8), pady=6)
        widget.grid(row=row, column=1, sticky="w", padx=(0, 20), pady=6)
        parent.columnconfigure(1, weight=1)

    def _start_key_capture(self, var: tk.StringVar, entry: tk.Entry) -> None:
        """Let user press a key to set hotkey."""
        entry.config(state="normal")
        var.set("Press a key...")
        def on_key(event):
            parts = []
            if event.state & 0x4:  parts.append("Control")
            if event.state & 0x1:  parts.append("Shift")
            if event.state & 0x8:  parts.append("Alt")
            key = event.keysym
            if key not in ("Control_L","Control_R","Shift_L","Shift_R","Alt_L","Alt_R"):
                parts.append(key)
            if parts:
                var.set("+".join(parts))
            entry.config(state="readonly")
            entry.unbind("<KeyPress>")
        entry.bind("<KeyPress>", on_key)

    def _delete_asset(self, name: str, kind: str, combo: ttk.Combobox) -> None:
        """Delete a custom theme or language file."""
        if not name:
            messagebox.showwarning("Nothing selected", f"Select a {kind} to delete.")
            return
        if not messagebox.askyesno("Confirm delete",
                f"Delete {kind} '{name}'?\nThis cannot be undone."):
            return
        base = os.path.dirname(os.path.abspath(__file__))
        if kind == "theme":
            path = os.path.join(base, THEMES_DIR, f"{name}.hrt")
        else:
            path = os.path.join(base, LANGS_DIR, f"{name}.hrl")
        try:
            if os.path.exists(path):
                os.remove(path)
                log.info(f"Deleted {kind}: {path}")
                messagebox.showinfo("Deleted", f"{kind.capitalize()} '{name}' deleted.")
                # Refresh combo
                if kind == "theme":
                    combo.config(values=self.app._scan_custom_themes())
                else:
                    combo.config(values=[c for c, _ in self.app._scan_custom_languages()])
                combo.set("")
            else:
                messagebox.showerror("Not found", f"File not found:\n{path}")
        except Exception as e:
            messagebox.showerror("Delete failed", str(e))

    def _collect(self) -> dict:
        return {
            "hrc_version": self.HRC_VERSION,
            "video_codec": self._cv.get(),
            "hw_accel": self._hwv.get(),
            "enc_preset": self._prev.get(),
            "enc_crf": self._crfv.get(),
            "pix_fmt": self._pixv.get(),
            "audio_sample_rate": int(self._srv.get()),
            "audio_aac_bitrate": self._abrv.get(),
            "audio_out_channels": int(self._achv.get()),
            "ui_theme": self._thv.get(),
            "ui_scale": int(self._uisv.get().replace("%", "")) / 100,
            "ui_font": self._fontv.get(),
            "filename_template": self._ftv.get(),
            "auto_stop_min": int(self._asv.get() or 0),
            "replay_buffer_sec": int(self._rbv.get() or 0),
            "hotkey_start_stop": self._hk_ss.get(),
            "hotkey_pause": self._hk_p.get(),
            "hotkey_fullscreen": self._hk_fs.get(),
            "notify_sound": self._notif_sound.get(),
            "notify_flash": self._notif_flash.get(),
            "auto_save_profile": self._auto_save.get(),
        }

    def _save(self) -> None:
        data = self._collect()
        a = self.app
        for k, v in data.items():
            if k != "hrc_version":
                setattr(a, k, v)
        # Re-apply hotkeys immediately
        if hasattr(a, '_apply_hotkeys'):
            a._apply_hotkeys()
        # Re-apply theme if changed
        if hasattr(a, 'apply_theme'):
            a.colors = a.get_theme_colors(data["ui_theme"])
            a.apply_theme()
        a.save_settings(silent=True)
        log.info(f"Advanced settings saved: {data}")
        self.dialog.destroy()

    def _export(self) -> None:
        path = filedialog.asksaveasfilename(
            defaultextension=".hrc",
            filetypes=[("HomRec Profile", "*.hrc"), ("All files", "*.*")],
            initialfile="homrec_profile.hrc",
            title="Export profile"
        )
        if not path:
            return
        data = self._collect()
        try:
            _hrc_write(path, data, _HRC_MAGIC)
            messagebox.showinfo("Exported", f"Profile saved to:\n{path}")
            log.info(f"Profile exported (binary): {path}")
        except Exception as e:
            messagebox.showerror("Export failed", str(e))

    def _import(self) -> None:
        path = filedialog.askopenfilename(
            filetypes=[("HomRec Profile", "*.hrc"), ("All files", "*.*")],
            title="Import profile"
        )
        if not path:
            return
        try:
            data = _hrc_read(path, _HRC_MAGIC)
            # Apply to UI vars
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
            # Also load new fields if present
            if hasattr(self, '_hk_ss'):
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
        self.dialog.geometry("500x500")
        self.dialog.configure(bg=app.colors["bg"])
        self.dialog.transient(parent)
        self.dialog.grab_set()
        
        self.dialog.update_idletasks()
        x = (self.dialog.winfo_screenwidth() // 2) - 250
        y = (self.dialog.winfo_screenheight() // 2) - 250
        self.dialog.geometry(f"+{x}+{y}")

        # Apply app icon
        try:
            if getattr(sys, 'frozen', False):
                base_dir = os.path.dirname(sys.executable)
            else:
                base_dir = os.path.dirname(os.path.abspath(__file__))
            ico_path = os.path.join(base_dir, "icons", "main.ico")
            if os.path.exists(ico_path):
                self.dialog.iconbitmap(ico_path)
        except Exception:
            pass
        
        self.create_widgets()
    
    def create_widgets(self) -> None:
        a = self.app
        c = a.colors
        
        notebook = ttk.Notebook(self.dialog)
        notebook.pack(fill="both", expand=True, padx=10, pady=10)
        
        video_tab = ttk.Frame(notebook)
        notebook.add(video_tab, text=a.lang["video_settings"])
        
        video_inner = tk.Frame(video_tab, bg=c["bg"])
        video_inner.pack(fill="both", expand=True, padx=15, pady=15)
        
        quality_frame = tk.Frame(video_inner, bg=c["bg"])
        quality_frame.pack(fill="x", pady=10)
        tk.Label(quality_frame, text=a.lang["quality"], 
                bg=c["bg"], fg=c["text"],
                font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.quality_var = tk.StringVar(value=str(a.quality))
        quality_scale = tk.Scale(quality_frame, from_=10, to=100, 
                                 orient="horizontal", length=250,
                                 variable=self.quality_var, 
                                 command=self.update_quality,
                                 bg=c["surface"], fg=c["text"],
                                 highlightthickness=0, troughcolor=c["surface_light"])
        quality_scale.pack(side="left", padx=5)
        tk.Label(quality_frame, text="%", bg=c["bg"], fg=c["text_secondary"],
                font=("Segoe UI", 10)).pack(side="left")
        
        res_frame = tk.Frame(video_inner, bg=c["bg"])
        res_frame.pack(fill="x", pady=10)
        tk.Label(res_frame, text=a.lang["resolution"], bg=c["bg"], fg=c["text"],
                font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.scale_var = tk.StringVar(value=str(int(a.scale_factor * 100)))
        scale_scale = tk.Scale(res_frame, from_=25, to=100, 
                              orient="horizontal", length=250,
                              variable=self.scale_var,
                              command=self.update_scale,
                              bg=c["surface"], fg=c["text"],
                              highlightthickness=0, troughcolor=c["surface_light"])
        scale_scale.pack(side="left", padx=5)
        tk.Label(res_frame, text="%", bg=c["bg"], fg=c["text_secondary"],
                font=("Segoe UI", 10)).pack(side="left")
        
        mode_frame = tk.Frame(video_inner, bg=c["bg"])
        mode_frame.pack(fill="x", pady=10)
        tk.Label(mode_frame, text=a.lang["mode"], bg=c["bg"], fg=c["text"],
                font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.mode_var = tk.StringVar(value=a.recording_mode)
        mode_combo = ttk.Combobox(mode_frame, textvariable=self.mode_var,
                                  values=["ultra", "turbo", "balanced", "eco"],
                                  width=15, state="readonly", font=("Segoe UI", 10))
        mode_combo.pack(side="left", padx=5)
        mode_combo.bind("<<ComboboxSelected>>", self.on_mode_change)
        
        tk.Label(video_inner, text="Codec and HW Accel settings are in ⚙ Advanced tab.",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI", 9, "italic")).pack(anchor="w", pady=(8, 0))

        lang_tab = ttk.Frame(notebook)
        notebook.add(lang_tab, text=a.lang["language"])
        
        lang_inner = tk.Frame(lang_tab, bg=c["bg"])
        lang_inner.pack(fill="both", expand=True, padx=15, pady=15)
        tk.Label(lang_inner, text="Select language:" if a.current_language == "en" else "Выберите язык:",
                bg=c["bg"], fg=c["text"], font=("Segoe UI", 11, "bold")).pack(anchor="w", pady=10)
        
        self.lang_var = tk.StringVar(value=a.current_language)
        tk.Radiobutton(lang_inner, text="English", variable=self.lang_var, value="en",
                      bg=c["bg"], fg=c["text"], selectcolor=c["surface"],
                      font=("Segoe UI", 10)).pack(anchor="w", pady=2)
        tk.Radiobutton(lang_inner, text="Русский", variable=self.lang_var, value="ru",
                      bg=c["bg"], fg=c["text"], selectcolor=c["surface"],
                      font=("Segoe UI", 10)).pack(anchor="w", pady=2)
        
        adv_tab = ttk.Frame(notebook)
        notebook.add(adv_tab, text=a.lang["advanced"])
        
        adv_inner = tk.Frame(adv_tab, bg=c["bg"])
        adv_inner.pack(fill="both", expand=True, padx=15, pady=15)
        
        mon_frame = tk.Frame(adv_inner, bg=c["bg"])
        mon_frame.pack(fill="x", pady=10)
        tk.Label(mon_frame, text=a.lang["monitor"], bg=c["bg"], fg=c["text"],
                font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.monitor_var = tk.StringVar(value=str(a.monitor_id))
        monitor_combo = ttk.Combobox(mon_frame, textvariable=self.monitor_var,
                                     values=[str(i) for i in range(1, len(a.sct.monitors))],
                                     width=10, state="readonly", font=("Segoe UI", 10))
        monitor_combo.pack(side="left", padx=5)
        monitor_combo.bind("<<ComboboxSelected>>", self.on_monitor_change)
        
        folder_frame = tk.Frame(adv_inner, bg=c["bg"])
        folder_frame.pack(fill="x", pady=10)
        tk.Label(folder_frame, text=a.lang["output"], bg=c["bg"], fg=c["text"],
                font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.folder_label = tk.Label(folder_frame, text=os.path.basename(a.output_folder), 
                                     bg=c["surface"], fg=c["accent"],
                                     font=("Consolas", 10), relief="flat", padx=8, pady=4)
        self.folder_label.pack(side="left", padx=5)
        tk.Button(folder_frame, text=a.lang["browse"], command=self.select_folder,
                 bg=c["surface"], fg=c["text"], font=("Segoe UI", 10),
                 relief="flat", padx=12).pack(side="left", padx=5)
        
        features_frame = tk.Frame(adv_inner, bg=c["bg"])
        features_frame.pack(fill="x", pady=10)
        self.countdown_var = tk.BooleanVar(value=a.countdown_var.get())
        tk.Checkbutton(features_frame, text=a.lang["countdown"],
                      variable=self.countdown_var, bg=c["bg"], fg=c["text"],
                      selectcolor=c["surface"], font=("Segoe UI", 10)).pack(anchor="w", pady=2)
        self.timestamp_var = tk.BooleanVar(value=a.timestamp_var.get())
        tk.Checkbutton(features_frame, text=a.lang["timestamp"],
                      variable=self.timestamp_var, bg=c["bg"], fg=c["text"],
                      selectcolor=c["surface"], font=("Segoe UI", 10)).pack(anchor="w", pady=2)
        self.cursor_var = tk.BooleanVar(value=a.cursor_var.get())
        tk.Checkbutton(features_frame, text=a.lang["cursor"],
                      variable=self.cursor_var, bg=c["bg"], fg=c["text"],
                      selectcolor=c["surface"], font=("Segoe UI", 10)).pack(anchor="w", pady=2)
        self.show_summary_var = tk.BooleanVar(value=a.show_summary)
        tk.Checkbutton(features_frame, text=a.lang["notification"],
                      variable=self.show_summary_var, bg=c["bg"], fg=c["text"],
                      selectcolor=c["surface"], font=("Segoe UI", 10)).pack(anchor="w", pady=2)
        self.minimize_tray_var = tk.BooleanVar(value=a.minimize_to_tray.get())
        tk.Checkbutton(features_frame, text=a.lang["minimize_tray"],
                      variable=self.minimize_tray_var, bg=c["bg"], fg=c["text"],
                      selectcolor=c["surface"], font=("Segoe UI", 10)).pack(anchor="w", pady=2)
        
        # Advanced Settings tab
        advsettings_tab = ttk.Frame(notebook)
        notebook.add(advsettings_tab, text="⚙ Advanced")
        advsettings_inner = tk.Frame(advsettings_tab, bg=c["bg"])
        advsettings_inner.pack(fill="both", expand=True, padx=15, pady=15)
        tk.Label(advsettings_inner,
                 text="Full customization for power users.",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI", 9)).pack(anchor="w", pady=(0, 12))
        tk.Button(advsettings_inner,
                  text="Open Advanced Settings →",
                  command=lambda: AdvancedSettingsDialog(self.dialog, self.app),
                  bg=c["accent"], fg=c["bg"],
                  font=("Segoe UI", 11, "bold"), relief="flat",
                  padx=20, pady=10).pack(anchor="w")
        tk.Label(advsettings_inner,
                 text="Codec · HW Accel · CRF · Preset · Audio bitrate\n"
                      "Theme · UI scale · Font · Auto-stop · Replay buffer\n"
                      "Import / Export profile (.hrc)",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI", 9), justify="left").pack(anchor="w", pady=(12, 0))

        btn_frame = tk.Frame(self.dialog, bg=c["bg"])
        btn_frame.pack(fill="x", padx=10, pady=10)
        tk.Button(btn_frame, text=a.lang["save"], command=self.save_settings,
                 bg=a.colors["success"], fg=a.colors["bg"],
                 font=("Segoe UI", 10, "bold"), relief="flat", padx=20, pady=8).pack(side="right", padx=5)
        tk.Button(btn_frame, text=a.lang["cancel"], command=self.dialog.destroy,
                 bg=c["surface"], fg=c["text"], font=("Segoe UI", 10),
                 relief="flat", padx=20, pady=8).pack(side="right", padx=5)
    
    def _on_codec_change(self, event=None) -> None:
        codec = self.codec_var.get()
        hints = {
            "libx264":   "CPU · H.264 · universal",
            "libx265":   "CPU · H.265 · smaller files, slower",
            "h264_nvenc":"GPU · H.264 · Nvidia only",
            "hevc_nvenc":"GPU · H.265 · Nvidia only",
            "h264_amf":  "GPU · H.264 · AMD only",
            "hevc_amf":  "GPU · H.265 · AMD only",
            "h264_qsv":  "GPU · H.264 · Intel only",
            "hevc_qsv":  "GPU · H.265 · Intel only",
        }
        self.codec_hint.config(text=hints.get(codec, ""))

    def update_quality(self, event=None) -> None:
        pass
    
    def update_scale(self, event=None) -> None:
        pass
    
    def on_mode_change(self, event=None) -> None:
        pass
    
    def on_monitor_change(self, event=None) -> None:
        pass
    
    def select_folder(self) -> None:
        folder = filedialog.askdirectory(initialdir=self.app.output_folder)
        if folder:
            self.app.output_folder = folder
            self.folder_label.config(text=os.path.basename(folder))
    
    def save_settings(self) -> None:
        new_lang = self.lang_var.get()
        if new_lang != self.app.current_language:
            self.app.current_language = new_lang
            self.app.lang = LANGUAGES[new_lang]
            self.app.update_ui_language()
        
        self.app.quality = int(self.quality_var.get())
        self.app.recording_mode = self.mode_var.get()
        self.app.update_mode_settings()
        self.app.scale_factor = int(self.scale_var.get()) / 100
        self.app.update_monitor_info()
        self.app.monitor_id = int(self.monitor_var.get())
        self.app.update_monitor_info()
        self.app.countdown_var.set(self.countdown_var.get())
        self.app.timestamp_var.set(self.timestamp_var.get())
        self.app.cursor_var.set(self.cursor_var.get())
        self.app.show_summary = self.show_summary_var.get()
        self.app.minimize_to_tray.set(self.minimize_tray_var.get())
        self.app.video_codec = self.codec_var.get()
        self.app.hw_accel = self.hw_var.get()
        self.app.res_label.config(text=f"{self.app.lang['resolution']} {self.app.record_width}x{self.app.record_height}")
        self.app.save_settings(silent=True)
        self.dialog.destroy()
        messagebox.showinfo(self.app.lang["info"], self.app.lang["settings_saved"])

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
        
        self.audio_recording = False
        self.audio_thread: threading.Thread | None = None
        self.audio_frames: list = []
        self.audio_stream = None
        self.audio_p = None
        self.audio_channels = 1
        self.sys_audio_recording = False
        self.sys_audio_thread: threading.Thread | None = None
        self.sys_audio_frames: list = []
        self.sys_audio_stream = None
        self.sys_audio_p = None
        self.sys_audio_filename: str | None = None
        self.sys_ffmpeg_proc = None
        
        self.ffmpeg_proc: subprocess.Popen | None = None
        self.ffmpeg_reader_thread: threading.Thread | None = None
        self.stop_ffmpeg_reader = False
        
        self.scale_factor = 0.75
        self.output_folder = "recordings"
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
        self.recording_thread: threading.Thread | None = None
        self.stop_flag = False
        self.last_frame_time = 0.0
        
        self.monitor_id = 1
        self.monitor_left = 0
        self.monitor_top = 0
        self.update_monitor_info()

        # capture mode: "desktop" or "window"
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
        self.update_preview()
        
        self.root.bind('<Configure>', self.on_window_resize)
        self._apply_hotkeys()
        self._setup_drag_drop()
        self._register_file_types()
        
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)
        self.setup_tray()
        log.info("HomRec v1.4.0 started, language: %s", self.current_language)
        # Check for updates 2 seconds after startup (non-blocking)
        self.root.after(2000, self._start_update_check)
    
    def update_ui_language(self) -> None:
        self.root.title(self.lang["app_title"])
        self.recreate_widgets()
    
    def check_ffmpeg(self) -> bool:
        return self.ffmpeg_path is not None
    
    def get_dshow_audio_devices(self) -> list[str]:
        """List available dshow audio input devices via ffmpeg."""
        try:
            result = subprocess.run(
                [self.ffmpeg_path, '-list_devices', 'true', '-f', 'dshow', '-i', 'dummy'],
                capture_output=True, text=True, timeout=5,
                creationflags=subprocess.CREATE_NO_WINDOW if platform.system() == 'Windows' else 0
            )
            devices = []
            for line in result.stderr.split('\n'):
                if '"' in line and ('audio' in line.lower() or 'stereo' in line.lower() or 'mix' in line.lower() or 'что' in line.lower()):
                    start = line.find('"')
                    end = line.find('"', start + 1)
                    if end > start:
                        devices.append(line[start+1:end])
            return devices
        except:
            return []

    def merge_audio_video(self, video_file: str, audio_file: str) -> bool:
        log.info(f"merge_audio_video: video={video_file!r} audio={audio_file!r}")
        if not audio_file or not os.path.exists(audio_file):
            log.warning(f"merge_audio_video: audio file missing: {audio_file!r}")
            return False
        if not os.path.exists(video_file):
            log.warning(f"merge_audio_video: video file missing: {video_file!r}")
            return False
        if not self.ffmpeg_path:
            log.warning("merge_audio_video: no ffmpeg path")
            return False

        audio_size = os.path.getsize(audio_file)
        video_size = os.path.getsize(video_file)
        log.info(f"merge_audio_video: audio_size={audio_size} video_size={video_size}")

        output_file = video_file.replace('.mp4', '_temp.mp4')

        try:
            cmd = [
                self.ffmpeg_path,
                '-i', video_file,
                '-i', audio_file,
                '-c:v', 'copy',
                '-c:a', 'aac',
                '-af', 'aresample=async=1000',
                '-map', '0:v:0',
                '-map', '1:a:0',
                '-shortest',
                '-y',
                output_file
            ]
            log.debug(f"merge cmd: {' '.join(cmd)}")
            result = subprocess.run(cmd, capture_output=True, timeout=120,
                                    creationflags=subprocess.CREATE_NO_WINDOW if platform.system() == 'Windows' else 0)
            stderr = result.stderr.decode('utf-8', errors='replace')
            log.debug(f"merge ffmpeg returncode={result.returncode}")
            if result.returncode != 0:
                log.warning(f"merge ffmpeg failed:\n{stderr[-1000:]}")

            if result.returncode == 0 and os.path.exists(output_file):
                os.remove(video_file)
                os.remove(audio_file)
                os.rename(output_file, video_file)
                log.info(f"merge_audio_video: success → {video_file}")
                return True
            log.warning(f"merge_audio_video: output not created or returncode!=0")
            return False
        except Exception as e:
            log.warning(f"merge_audio_video exception: {e}")
            return False
    
    def set_app_icon(self) -> None:
        # Resolve icons folder relative to exe or script
        if getattr(sys, 'frozen', False):
            base_dir = os.path.dirname(sys.executable)
        else:
            base_dir = os.path.dirname(os.path.abspath(__file__))
        icons_dir = os.path.join(base_dir, "icons")

        # main.ico — icon shown inside the app window (tkinter iconbitmap)
        main_ico = os.path.join(icons_dir, "main.ico")
        try:
            self.root.iconbitmap(main_ico)
        except:
            # fallback: generate a simple icon if file not found
            try:
                icon_image = Image.new('RGBA', (64, 64), (0, 0, 0, 0))
                draw = ImageDraw.Draw(icon_image)
                draw.rectangle([10, 20, 54, 44], fill="#89b4fa", outline="#cdd6f4", width=2)
                draw.ellipse([25, 25, 39, 39], fill="#1e1e2e", outline="#cdd6f4", width=2)
                draw.ellipse([29, 29, 35, 35], fill="#89b4fa")
                icon_photo = ImageTk.PhotoImage(icon_image)
                self.root.iconphoto(True, icon_photo)
            except:
                pass

        if sys.platform == "win32":
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("homrec.1.4.0")
    
    def on_window_resize(self, event: tk.Event) -> None:
        if event.widget == self.root:
            self.update_preview_size()
    
    def update_preview_size(self) -> None:
        try:
            preview_height = self.root.winfo_height() - 200
            preview_width = self.root.winfo_width() - 280
            if preview_width > 0 and preview_height > 0:
                self.preview_width = max(600, min(preview_width - 40, 1280))
                self.preview_height = max(350, min(preview_height - 40, 720))
        except:
            pass
    
    # Built-in themes
    BUILTIN_THEMES = {
        "dark": {
            "bg": "#1e1e2e", "fg": "#cdd6f4", "accent": "#89b4fa",
            "success": "#a6e3a1", "warning": "#f9e2af", "error": "#f38ba8",
            "surface": "#313244", "surface_light": "#45475a",
            "preview_bg": "#11111b", "text": "#cdd6f4", "text_secondary": "#a6adc8"
        },
        "light": {
            "bg": "#f5f5f5", "fg": "#2c3e50", "accent": "#3498db",
            "success": "#27ae60", "warning": "#f39c12", "error": "#e74c3c",
            "surface": "#ecf0f1", "surface_light": "#bdc3c7",
            "preview_bg": "#ffffff", "text": "#2c3e50", "text_secondary": "#7f8c8d"
        },
        "catppuccin": {
            "bg": "#1e1e2e", "fg": "#cdd6f4", "accent": "#cba6f7",
            "success": "#a6e3a1", "warning": "#f9e2af", "error": "#f38ba8",
            "surface": "#181825", "surface_light": "#313244",
            "preview_bg": "#11111b", "text": "#cdd6f4", "text_secondary": "#6c7086"
        },
        "nord": {
            "bg": "#2e3440", "fg": "#eceff4", "accent": "#88c0d0",
            "success": "#a3be8c", "warning": "#ebcb8b", "error": "#bf616a",
            "surface": "#3b4252", "surface_light": "#434c5e",
            "preview_bg": "#242933", "text": "#eceff4", "text_secondary": "#d8dee9"
        },
        "dracula": {
            "bg": "#282a36", "fg": "#f8f8f2", "accent": "#bd93f9",
            "success": "#50fa7b", "warning": "#f1fa8c", "error": "#ff5555",
            "surface": "#44475a", "surface_light": "#6272a4",
            "preview_bg": "#21222c", "text": "#f8f8f2", "text_secondary": "#6272a4"
        },
    }

    def get_theme_colors(self, theme: str) -> dict:
        # Check built-in themes
        if theme in self.BUILTIN_THEMES:
            return self.BUILTIN_THEMES[theme]
        # Try loading .hrt file from themes/ folder
        themes_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), THEMES_DIR)
        hrt_path = os.path.join(themes_dir, f"{theme}.hrt")
        if os.path.exists(hrt_path):
            try:
                data = _hrc_read(hrt_path, _HRT_MAGIC)
                result = dict(self.BUILTIN_THEMES["dark"])
                result.update(data)
                log.info(f"Loaded theme from {hrt_path}")
                return result
            except Exception as e:
                log.warning(f"Failed to load theme {hrt_path}: {e}")
        # Fallback to dark
        return self.BUILTIN_THEMES["dark"]
    
    def _load_language(self, lang_code: str) -> dict:
        """Load language from LANGUAGES dict or from Assets/L/*.hrl file."""
        if lang_code in LANGUAGES:
            return dict(LANGUAGES[lang_code])
        # Try .hrl file in Assets/L/
        base = os.path.dirname(os.path.abspath(__file__))
        hrl_path = os.path.join(base, LANGS_DIR, f"{lang_code}.hrl")
        if os.path.exists(hrl_path):
            try:
                data = _hrc_read(hrl_path, _HRL_MAGIC)
                result = dict(LANGUAGES["en"])
                file_schema = data.get("schema_version", 0)
                result.update(data)
                # Check for missing keys
                missing = [k for k in LANG_REQUIRED_KEYS if k not in data]
                if missing:
                    log.warning(f"Language {lang_code}: {len(missing)} missing keys "
                                f"(schema {file_schema} vs {LANG_SCHEMA_VERSION}). "
                                f"Using English fallback for: {missing[:5]}...")
                log.info(f"Loaded language from {hrl_path} (schema={file_schema})")
                return result
            except Exception as e:
                log.warning(f"Failed to load language {hrl_path}: {e}")
        return dict(LANGUAGES["en"])

    def _scan_custom_languages(self) -> list:
        """Return list of (code, name) for all .hrl files in Assets/L/ folder."""
        langs_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), LANGS_DIR)
        result = []
        os.makedirs(langs_dir, exist_ok=True)
        for fname in os.listdir(langs_dir):
            if fname.endswith(".hrl"):
                code = fname[:-4]
                try:
                    data = _hrc_read(os.path.join(langs_dir, fname), _HRL_MAGIC)
                    name = data.get("lang_name", code)
                    result.append((code, name))
                except Exception as e:
                    log.warning(f"Failed to scan language {fname}: {e}")
        return result

    def _scan_custom_themes(self) -> list:
        """Return list of theme names from Assets/Themes/*.hrt files."""
        themes_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), THEMES_DIR)
        os.makedirs(themes_dir, exist_ok=True)
        result = []
        if not os.path.exists(themes_dir):
            return result
        for fname in os.listdir(themes_dir):
            if fname.endswith(".hrt"):
                result.append(fname[:-4])
        return result

    def apply_theme(self) -> None:
        self.root.configure(bg=self.colors["bg"])
        style = ttk.Style()
        style.theme_use('clam')
        style.configure("TFrame", background=self.colors["bg"])
        style.configure("TLabel", background=self.colors["bg"], foreground=self.colors["fg"])
        style.configure("TLabelframe", background=self.colors["bg"], foreground=self.colors["accent"])
        style.configure("TLabelframe.Label", background=self.colors["bg"], foreground=self.colors["accent"], 
                       font=("Segoe UI", 11, "bold"))
        style.configure("TButton", background=self.colors["surface"], foreground=self.colors["fg"])
        style.configure("TCombobox", fieldbackground=self.colors["surface"], foreground=self.colors["fg"])
    
    def create_menu(self) -> None:
        menubar = tk.Menu(self.root, bg=self.colors["surface"], fg=self.colors["fg"])
        self.root.config(menu=menubar)
        
        file_menu = tk.Menu(menubar, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        menubar.add_cascade(label=self.lang["file_menu"], menu=file_menu)
        file_menu.add_command(label=self.lang["open_recordings"], command=self.open_recordings)
        file_menu.add_separator()
        file_menu.add_command(label=self.lang["exit"], command=self.quit_app)
        
        view_menu = tk.Menu(menubar, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        menubar.add_cascade(label=self.lang["view_menu"], menu=view_menu)
        
        view_menu.add_checkbutton(label=self.lang["always_on_top"],
                                 variable=self.always_on_top,
                                 command=self.toggle_always_on_top)
        view_menu.add_command(label=self.lang["fullscreen"], command=self.toggle_fullscreen)
        view_menu.add_separator()
        
        if HAS_PSUTIL:
            view_menu.add_command(label=self.lang["pc_analytics"], command=self.show_analytics)
            view_menu.add_separator()
        
        lang_menu = tk.Menu(view_menu, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        view_menu.add_cascade(label=self.lang["language"], menu=lang_menu)
        lang_menu.add_radiobutton(label="English", variable=self.language_var, value="en",
                                 command=lambda: self.change_language("en"))
        lang_menu.add_radiobutton(label="Русский", variable=self.language_var, value="ru",
                                 command=lambda: self.change_language("ru"))
        _custom_langs = self._scan_custom_languages()
        if _custom_langs:
            lang_menu.add_separator()
            for _lcode, _lname in _custom_langs:
                lang_menu.add_radiobutton(label=f"★ {_lname}", variable=self.language_var,
                                          value=_lcode,
                                          command=lambda c=_lcode: self.change_language(c))
        
        theme_menu = tk.Menu(view_menu, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        view_menu.add_cascade(label=self.lang["theme"], menu=theme_menu)
        for _tid, _tlabel in [("dark", "Dark"), ("light", "Light"),
                               ("catppuccin", "Catppuccin"), ("nord", "Nord"), ("dracula", "Dracula")]:
            theme_menu.add_radiobutton(label=_tlabel, variable=self.theme_var, value=_tid,
                                       command=lambda t=_tid: self.change_theme(t))
        _custom_themes = self._scan_custom_themes()
        if _custom_themes:
            theme_menu.add_separator()
            for _ct in _custom_themes:
                theme_menu.add_radiobutton(label=f"★ {_ct}", variable=self.theme_var, value=_ct,
                                           command=lambda t=_ct: self.change_theme(t))
        
        settings_menu = tk.Menu(menubar, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        menubar.add_cascade(label=self.lang["settings_menu"], menu=settings_menu)
        settings_menu.add_command(label=self.lang["preferences"], command=self.open_settings)
        settings_menu.add_separator()
        
        perf_menu = tk.Menu(settings_menu, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        settings_menu.add_cascade(label=self.lang["performance_menu"], menu=perf_menu)
        perf_menu.add_command(label=self.lang["ultra"], command=lambda: self.set_mode("ultra"))
        perf_menu.add_command(label=self.lang["turbo"], command=lambda: self.set_mode("turbo"))
        perf_menu.add_command(label=self.lang["balanced"], command=lambda: self.set_mode("balanced"))
        perf_menu.add_command(label=self.lang["eco"], command=lambda: self.set_mode("eco"))

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
        if self.always_on_top.get():
            self.root.attributes('-topmost', True)
        else:
            self.root.attributes('-topmost', False)
        self.save_settings(silent=True)
    
    def toggle_fullscreen(self) -> None:
        if self.root.attributes('-fullscreen'):
            self.root.attributes('-fullscreen', False)
        else:
            self.root.attributes('-fullscreen', True)
    
    def show_cpu_info(self) -> None:
        self.show_analytics()

    def show_ram_info(self) -> None:
        self.show_analytics()

    def show_disk_info(self) -> None:
        self.show_analytics()

    def show_analytics(self) -> None:
        if not HAS_PSUTIL:
            messagebox.showinfo("PC Analytics", "psutil not installed.")
            return

        dlg = tk.Toplevel(self.root)
        self._set_icon(dlg)
        dlg.title("PC Analytics")
        self._set_icon(dlg)
        dlg.geometry("360x440")
        dlg.configure(bg=self.colors["bg"])
        dlg.transient(self.root)
        dlg.resizable(False, True)
        dlg.update_idletasks()
        x = self.root.winfo_x() + self.root.winfo_width() // 2 - 170
        y = self.root.winfo_y() + self.root.winfo_height() // 2 - 150
        dlg.geometry(f"+{x}+{y}")

        def make_section(parent, title, color):
            f = tk.Frame(parent, bg=self.colors["surface"], pady=8, padx=12)
            f.pack(fill="x", padx=12, pady=6)
            tk.Label(f, text=title, bg=self.colors["surface"],
                     fg=color, font=("Segoe UI", 10, "bold")).pack(anchor="w")
            return f

        def row(parent, label, value):
            r = tk.Frame(parent, bg=self.colors["surface"])
            r.pack(fill="x", pady=1)
            tk.Label(r, text=label, bg=self.colors["surface"],
                     fg=self.colors["text_secondary"], font=("Segoe UI", 9), width=14, anchor="w").pack(side="left")
            tk.Label(r, text=value, bg=self.colors["surface"],
                     fg=self.colors["text"], font=("Consolas", 9)).pack(side="left")

        def refresh():
            for w in dlg.winfo_children():
                w.destroy()

            tk.Label(dlg, text="PC Analytics", bg=self.colors["bg"],
                     fg=self.colors["accent"], font=("Segoe UI", 12, "bold")).pack(pady=(12, 4))

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

            tk.Button(dlg, text="Refresh", command=refresh,
                      bg=self.colors["surface_light"], fg=self.colors["text"],
                      font=("Segoe UI", 9), relief="flat", padx=16, pady=4,
                      cursor="hand2").pack(pady=(4, 12))

        refresh()
    
    def change_language(self, lang: str) -> None:
        if lang != self.current_language:
            self.current_language = lang
            self.lang = LANGUAGES[lang]
            self.language_var.set(lang)
            self.update_ui_language()
            self.save_settings(silent=True)
    
    def open_settings(self) -> None:
        SettingsDialog(self.root, self)
    
    def change_theme(self, theme: str) -> None:
        self.current_theme = theme
        self.theme_var.set(theme)
        self.colors = self.get_theme_colors(theme)
        self.apply_theme()
        self.recreate_widgets()
        self.save_settings(silent=True)
    
    def recreate_widgets(self) -> None:
        was_recording = self.recording
        was_paused = self.paused
        for widget in self.root.winfo_children():
            widget.destroy()
        self.create_menu()
        self.create_widgets()
        if was_recording:
            self.record_btn.config(text=self.lang["stop"], bg=self.colors["error"], command=self.stop_recording)
            self.pause_btn.config(state="normal")
            if was_paused:
                self.pause_btn.config(text=self.lang["resume"], bg=self.colors["success"])
    
    def set_mode(self, mode: str) -> None:
        self.recording_mode = mode
        self.update_mode_settings()
        self.save_settings(silent=True)
        self.res_label.config(text=f"{self.lang['resolution']} {self.record_width}x{self.record_height}")
    
    def update_mode_settings(self) -> None:
        if self.recording_mode == "ultra":
            self.target_fps = 60
            self.quality = 95
            self.scale_factor = 1.0
        elif self.recording_mode == "turbo":
            self.target_fps = 30
            self.quality = 90
            self.scale_factor = 1.0
        elif self.recording_mode == "balanced":
            self.target_fps = 15
            self.quality = 70
            self.scale_factor = 0.75
        else:
            self.target_fps = 8
            self.quality = 50
            self.scale_factor = 0.5
        self.update_monitor_info()
    
    def load_settings(self) -> None:
        try:
            if os.path.exists("homrec_settings.json"):
                with open("homrec_settings.json", "r") as f:
                    settings = json.load(f)
                    self.output_folder = settings.get("output_folder", "recordings")
                    self.scale_factor = settings.get("scale_factor", 0.75)
                    self.target_fps = settings.get("target_fps", 15)
                    self.quality = settings.get("quality", 70)
                    self.recording_mode = settings.get("mode", "balanced")
                    self.current_theme = settings.get("theme", "dark")
                    self.current_language = settings.get("language", "en")
                    self.lang = LANGUAGES[self.current_language]
                    self.always_on_top.set(settings.get("always_on_top", False))
                    self.countdown_var.set(settings.get("countdown", True))
                    self.timestamp_var.set(settings.get("timestamp", False))
                    self.cursor_var.set(settings.get("cursor", False))
                    self.show_summary = settings.get("show_summary", True)
                    self.minimize_to_tray.set(settings.get("minimize_to_tray", True))
                    self.video_codec = settings.get("video_codec", "libx264")
                    self.hw_accel = settings.get("hw_accel", "auto")
                    self.enc_preset = settings.get("enc_preset", "ultrafast")
                    self.enc_crf = settings.get("enc_crf", 18)
                    self.pix_fmt = settings.get("pix_fmt", "yuv420p")
                    self.audio_sample_rate = settings.get("audio_sample_rate", 44100)
                    self.audio_aac_bitrate = settings.get("audio_aac_bitrate", "192k")
                    self.audio_out_channels = settings.get("audio_out_channels", 2)
                    self.ui_theme = settings.get("ui_theme", "dark")
                    self.ui_scale = settings.get("ui_scale", 1.0)
                    self.ui_font = settings.get("ui_font", "Segoe UI")
                    self.filename_template = settings.get("filename_template", "HomRec_{date}_{time}")
                    self.auto_stop_min = settings.get("auto_stop_min", 0)
                    self.replay_buffer_sec = settings.get("replay_buffer_sec", 0)
                    self.hotkey_start_stop = settings.get("hotkey_start_stop", "F9")
                    self.hotkey_pause = settings.get("hotkey_pause", "F10")
                    self.hotkey_fullscreen = settings.get("hotkey_fullscreen", "F11")
                    self.notify_sound = settings.get("notify_sound", True)
                    self.notify_flash = settings.get("notify_flash", True)
                    self.auto_save_profile = settings.get("auto_save_profile", False)

                    if self.always_on_top.get():
                        self.root.attributes('-topmost', True)
        except:
            pass
    
    def save_settings(self, silent: bool = False) -> None:
        settings = {
            "output_folder": self.output_folder,
            "scale_factor": self.scale_factor,
            "target_fps": self.target_fps,
            "quality": self.quality,
            "mode": self.recording_mode,
            "theme": self.current_theme,
            "language": self.current_language,
            "always_on_top": self.always_on_top.get(),
            "countdown": self.countdown_var.get(),
            "timestamp": self.timestamp_var.get(),
            "cursor": self.cursor_var.get(),
            "show_summary": self.show_summary,
            "minimize_to_tray": self.minimize_to_tray.get(),
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
            "auto_save_profile": getattr(self, 'auto_save_profile', False)
        }
        with open("homrec_settings.json", "w") as f:
            json.dump(settings, f, indent=2)
        if not silent:
            messagebox.showinfo(self.lang["info"], self.lang["settings_saved"])
    
    def update_monitor_info(self) -> None:
        """Update monitor information including position offsets"""
        if self.monitor_id < len(self.sct.monitors):
            self.monitor = self.sct.monitors[self.monitor_id]
            self.original_width = self.monitor['width']
            self.original_height = self.monitor['height']
            
            # Store monitor position for FFmpeg
            self.monitor_left = self.monitor['left']
            self.monitor_top = self.monitor['top']
            
            self.record_width = int(self.original_width * self.scale_factor)
            self.record_height = int(self.original_height * self.scale_factor)
            
            # Ensure dimensions are even (required for some codecs)
            if self.record_width % 2 != 0:
                self.record_width -= 1
            if self.record_height % 2 != 0:
                self.record_height -= 1
            log.debug(f"Monitor {self.monitor_id}: {self.original_width}x{self.original_height} at ({self.monitor_left}, {self.monitor_top}), record: {self.record_width}x{self.record_height}")
    
    def create_widgets(self) -> None:
        main_container = tk.Frame(self.root, bg=self.colors["bg"])
        main_container.pack(fill="both", expand=True, padx=15, pady=15)
        
        left_panel = tk.Frame(main_container, bg=self.colors["surface"], width=240)
        left_panel.pack(side="left", fill="y", padx=(0, 15))
        left_panel.pack_propagate(False)
        
        title_frame = tk.Frame(left_panel, bg=self.colors["surface"])
        title_frame.pack(pady=20, fill="x")
        tk.Label(title_frame, text="HomRec", 
                font=("Segoe UI", 22, "bold"), 
                bg=self.colors["surface"], 
                fg=self.colors["accent"]).pack()
        tk.Label(title_frame, text="v1.4.0", 
                font=("Segoe UI", 11), 
                bg=self.colors["surface"], 
                fg=self.colors["text_secondary"]).pack()
        
        btn_frame = tk.Frame(left_panel, bg=self.colors["surface"])
        btn_frame.pack(pady=25, padx=15, fill="x")
        
        # Start/Stop — single button that toggles
        self.record_btn = tk.Button(btn_frame, text=self.lang["start"],
                                   command=self.start_with_countdown,
                                   bg=self.colors["success"], fg=self.colors["bg"],
                                   font=("Segoe UI", 11, "bold"),
                                   relief="flat", height=2, cursor="hand2")
        self.record_btn.pack(fill="x", pady=(0, 4))

        self.pause_btn = tk.Button(btn_frame, text=self.lang["pause"],
                                  command=self.toggle_pause,
                                  bg=self.colors["warning"], fg=self.colors["bg"],
                                  font=("Segoe UI", 10, "bold"),
                                  state="disabled", relief="flat", height=1,
                                  cursor="hand2")
        self.pause_btn.pack(fill="x", pady=(4, 0))
        # stop_btn kept as hidden reference for compatibility but not shown
        self.stop_btn = tk.Button(btn_frame)
        
        status_frame = tk.Frame(left_panel, bg=self.colors["surface"])
        status_frame.pack(pady=15, padx=15, fill="x")
        tk.Label(status_frame, text=self.lang["status"], 
                font=("Segoe UI", 11, "bold"),
                bg=self.colors["surface"], fg=self.colors["accent"]).pack(anchor="w")
        
        status_row = tk.Frame(status_frame, bg=self.colors["surface"])
        status_row.pack(fill="x", pady=8)
        self.status_icon = tk.Label(status_row, text="⬤", 
                                   fg=self.colors["error"], 
                                   bg=self.colors["surface"], 
                                   font=("Arial", 18))
        self.status_icon.pack(side="left", padx=(0, 8))
        self.status_label = tk.Label(status_row, text=self.lang["ready"], 
                                    bg=self.colors["surface"], fg=self.colors["text"],
                                    font=("Segoe UI", 11))
        self.status_label.pack(side="left")
        
        timer_frame = tk.Frame(left_panel, bg=self.colors["surface"])
        timer_frame.pack(pady=15, padx=15, fill="x")
        tk.Label(timer_frame, text=self.lang["time"], 
                font=("Segoe UI", 11, "bold"),
                bg=self.colors["surface"], fg=self.colors["accent"]).pack(anchor="w")
        self.time_label = tk.Label(timer_frame, text="00:00:00", 
                                   font=("Consolas", 24, "bold"),
                                   bg=self.colors["surface"], fg=self.colors["accent"])
        self.time_label.pack(pady=8)
        
        stats_frame = tk.Frame(left_panel, bg=self.colors["surface"])
        stats_frame.pack(pady=15, padx=15, fill="x")
        tk.Label(stats_frame, text=self.lang["stats"], 
                font=("Segoe UI", 11, "bold"),
                bg=self.colors["surface"], fg=self.colors["accent"]).pack(anchor="w")
        
        self.fps_label = tk.Label(stats_frame, text=f"{self.lang['fps']} 0", 
                                 bg=self.colors["surface"], fg=self.colors["text"],
                                 font=("Consolas", 11))
        self.fps_label.pack(anchor="w", pady=3)
        
        self.res_label = tk.Label(stats_frame, 
                                 text=f"{self.lang['resolution']} {self.record_width}x{self.record_height}", 
                                 bg=self.colors["surface"], fg=self.colors["text"],
                                 font=("Consolas", 11))
        self.res_label.pack(anchor="w", pady=3)
        
        right_panel = tk.Frame(main_container, bg=self.colors["bg"])
        right_panel.pack(side="right", fill="both", expand=True)
        
        preview_container = tk.Frame(right_panel, bg=self.colors["surface_light"], relief="flat", bd=2)
        preview_container.pack(fill="both", expand=True, pady=(0, 15))
        
        preview_label_title = tk.Label(preview_container, text=self.lang["live_preview"], 
                                      bg=self.colors["surface_light"], 
                                      fg=self.colors["text_secondary"],
                                      font=("Segoe UI", 10, "bold"))
        preview_label_title.pack(anchor="nw", padx=8, pady=5)
        
        preview_frame = tk.Frame(preview_container, bg=self.colors["preview_bg"])
        preview_frame.pack(fill="both", expand=True, padx=8, pady=8)
        
        self.preview_label = tk.Label(preview_frame, bg=self.colors["preview_bg"])
        self.preview_label.pack(fill="both", expand=True)
        
        bottom_panel = tk.Frame(right_panel, bg=self.colors["bg"], height=300)
        bottom_panel.pack(fill="x")
        bottom_panel.pack_propagate(False)
        
        self.audio_panel = AudioPanel(bottom_panel, self)
        
        bottom_bar = tk.Frame(self.root, bg=self.colors["surface"], height=30)
        bottom_bar.pack(side="bottom", fill="x")
        
        self.file_label = tk.Label(bottom_bar, text=self.lang["ready"], 
                                   bg=self.colors["surface"], fg=self.colors["text_secondary"],
                                   font=("Segoe UI", 10))
        self.file_label.pack(side="left", padx=15)
        
        tk.Label(bottom_bar, text=self.lang["made_by"], 
                bg=self.colors["surface"], fg=self.colors["accent"],
                font=("Segoe UI", 10, "bold")).pack(side="right", padx=15)
        
        self.update_preview_size()
    
    def get_audio_channels(self) -> int:
        try:
            p = pyaudio.PyAudio()
            try:
                stream = p.open(
                    format=pyaudio.paInt16,
                    channels=2,
                    rate=44100,
                    input=True,
                    frames_per_buffer=1024
                )
                stream.close()
                return 2
            except:
                try:
                    stream = p.open(
                        format=pyaudio.paInt16,
                        channels=1,
                        rate=44100,
                        input=True,
                        frames_per_buffer=1024
                    )
                    stream.close()
                    return 1
                except:
                    return 1
        except:
            return 1
    
    def _find_wasapi_loopback(self, p: "pyaudio.PyAudio") -> int | None:
        """Find WASAPI loopback device index for system audio capture (Windows only).

        With pyaudio-wasapi, any output device can be opened as loopback via as_loopback=True.
        Priority: explicit loopback/Stereo Mix device → default output device (works with as_loopback).
        Returns None on non-Windows or if WASAPI is not available.
        """
        if sys.platform != 'win32':
            return None
        try:
            wasapi_info = p.get_host_api_info_by_type(pyaudio.paWASAPI)
        except OSError:
            log.warning("WASAPI not available on this system")
            return None

        default_out_idx = None

        for i in range(p.get_device_count()):
            try:
                dev = p.get_device_info_by_index(i)
            except Exception:
                continue
            if dev.get('hostApi') != wasapi_info['index']:
                continue
            name = dev.get('name', '').lower()
            log.debug(f"WASAPI device [{i}]: {dev.get('name')} in={dev.get('maxInputChannels')} out={dev.get('maxOutputChannels')}")
            # Explicit loopback / Stereo Mix device — best option
            if dev.get('maxInputChannels', 0) >= 1:
                if any(k in name for k in ('loopback', 'stereo mix', 'what u hear',
                                           'стерео микшер', 'что слышит')):
                    log.info(f"WASAPI loopback device found: [{i}] {dev.get('name')}")
                    return i
            # Remember default output device as fallback for as_loopback=True
            if dev.get('maxOutputChannels', 0) >= 1:
                if default_out_idx is None:
                    default_out = wasapi_info.get('defaultOutputDevice', -1)
                    if dev.get('index', i) == default_out or default_out < 0:
                        default_out_idx = i

        # Fallback: use default output device with as_loopback=True (pyaudio-wasapi only)
        if default_out_idx is not None:
            log.info(f"No explicit loopback device found; using default output [{default_out_idx}] with as_loopback=True")
            return default_out_idx

        log.warning("No WASAPI loopback device found at all")
        return None

    def _notify_recording_start(self) -> None:
        """Flash the window border and/or play a beep when recording starts."""
        if getattr(self, 'notify_flash', True):
            # Flash red border 3 times
            orig_bg = self.root.cget("bg")
            def _flash(n=0):
                if n >= 6:
                    self.root.configure(bg=orig_bg)
                    return
                color = self.colors.get("error", "#f38ba8") if n % 2 == 0 else orig_bg
                self.root.configure(bg=color)
                self.root.after(120, lambda: _flash(n + 1))
            _flash()
        if getattr(self, 'notify_sound', True):
            try:
                import winsound
                winsound.MessageBeep(winsound.MB_OK)
            except Exception:
                pass


    def _register_file_types(self) -> None:
        """Register .hrc/.hrl/.hrt file associations in Windows registry."""
        if platform.system() != "Windows":
            return
        try:
            import winreg
            base = os.path.dirname(os.path.abspath(__file__))
            icons_dir = os.path.join(base, "icons")

            types = [
                (".hrc", "HomRec.Profile", "HomRec Profile",  "hrc.ico"),
                (".hrl", "HomRec.Language","HomRec Language", "hrl.ico"),
                (".hrt", "HomRec.Theme",   "HomRec Theme",    "hrt.ico"),
            ]

            for ext, prog_id, description, ico_file in types:
                ico_path = os.path.join(icons_dir, ico_file)

                # Register extension → ProgID
                with winreg.CreateKey(winreg.HKEY_CURRENT_USER,
                                      f"Software\\Classes\\{ext}") as k:
                    winreg.SetValue(k, "", winreg.REG_SZ, prog_id)

                # Register ProgID
                with winreg.CreateKey(winreg.HKEY_CURRENT_USER,
                                      f"Software\\Classes\\{prog_id}") as k:
                    winreg.SetValue(k, "", winreg.REG_SZ, description)

                # Set icon (only if file exists)
                if os.path.exists(ico_path):
                    with winreg.CreateKey(winreg.HKEY_CURRENT_USER,
                                          f"Software\\Classes\\{prog_id}\\DefaultIcon") as k:
                        winreg.SetValue(k, "", winreg.REG_SZ, ico_path)

                # Set "open with HomRec" action
                exe_path = os.path.abspath(__file__)
                with winreg.CreateKey(winreg.HKEY_CURRENT_USER,
                                      f"Software\\Classes\\{prog_id}\\shell\\open\\command") as k:
                    winreg.SetValue(k, "", winreg.REG_SZ, f'"{exe_path}" "%1"')

            # Notify Windows Explorer to refresh icons
            try:
                import ctypes
                ctypes.windll.shell32.SHChangeNotify(0x08000000, 0, None, None)
            except Exception:
                pass

            log.info("File type associations registered (.hrc/.hrl/.hrt)")
        except Exception as e:
            log.warning(f"Could not register file types: {e}")

    def _apply_hotkeys(self) -> None:
        """Bind hotkeys from current settings. Unbinds old ones first."""
        for key in getattr(self, '_bound_hotkeys', []):
            try:
                self.root.unbind(key)
            except Exception:
                pass
        self._bound_hotkeys = []
        def _bind(key, cmd):
            k = f'<{key}>'
            self.root.bind(k, cmd)
            self._bound_hotkeys.append(k)
        _bind(self.hotkey_start_stop, lambda e: self.toggle_recording())
        _bind(self.hotkey_pause, lambda e: self.toggle_pause() if self.recording else None)
        _bind(self.hotkey_fullscreen, lambda e: self.toggle_fullscreen())
        log.debug(f"Hotkeys: start/stop={self.hotkey_start_stop} pause={self.hotkey_pause} fullscreen={self.hotkey_fullscreen}")

    def _handle_drop(self, event) -> None:
        """Handle drag-and-drop of .hrc/.hrl/.hrt files onto the main window."""
        raw = event.data.strip()
        paths = []
        if raw.startswith('{'):
            import re
            paths = re.findall(r'{([^}]+)}', raw)
            if not paths:
                paths = [raw.strip('{}')]
        else:
            paths = raw.split()

        for path in paths:
            path = path.strip()
            try:
                kind = _hrc_detect(path)
                if kind == 'hrc':
                    self._import_hrc(path)
                elif kind == 'hrl':
                    self._import_hrl(path)
                elif kind == 'hrt':
                    self._import_hrt(path)
            except ValueError as e:
                messagebox.showerror("Invalid file", str(e))
                log.warning(f"Drop rejected: {path} — {e}")

    def _import_hrc(self, path: str) -> None:
        """Import a .hrc profile file (binary format)."""
        try:
            data = _hrc_read(path, _HRC_MAGIC)
            for k, v in data.items():
                if k != 'hrc_version':
                    setattr(self, k, v)
            self.save_settings(silent=True)
            self.apply_theme()
            messagebox.showinfo("Profile imported", f"Profile loaded:\n{os.path.basename(path)}")
            log.info(f"HRC profile imported (binary): {path}")
        except Exception as e:
            messagebox.showerror("Import failed", str(e))

    def _import_hrl(self, path: str) -> None:
        """Import a .hrl language file — copy to Assets/L/ folder."""
        try:
            langs_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), LANGS_DIR)
            os.makedirs(langs_dir, exist_ok=True)
            dst = os.path.join(langs_dir, os.path.basename(path))
            import shutil
            shutil.copy2(path, dst)
            messagebox.showinfo("Language imported",
                f"Language file installed:\n{os.path.basename(path)}\n\n"
                "Restart HomRec and select it in Settings → Language.")
            log.info(f"HRL language imported: {path}")
        except Exception as e:
            messagebox.showerror("Import failed", str(e))

    def _import_hrt(self, path: str) -> None:
        """Import a .hrt theme file (binary format) — copy to Assets/Themes/ folder and apply."""
        try:
            themes_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), THEMES_DIR)
            os.makedirs(themes_dir, exist_ok=True)
            dst = os.path.join(themes_dir, os.path.basename(path))
            import shutil
            shutil.copy2(path, dst)
            # Get theme name from binary file
            data = _hrc_read(path, _HRT_MAGIC)
            theme_name = data.get('theme_name', os.path.splitext(os.path.basename(path))[0])
            self.ui_theme = os.path.splitext(os.path.basename(path))[0]
            self.colors = self.get_theme_colors(self.ui_theme)
            self.apply_theme()
            self.save_settings(silent=True)
            messagebox.showinfo("Theme imported", f"Theme '{theme_name}' applied!")
            log.info(f"HRT theme imported and applied: {path}")
        except Exception as e:
            messagebox.showerror("Import failed", str(e))

    def _setup_drag_drop(self) -> None:
        """Register drag-and-drop on the main window if tkinterdnd2 is available."""
        if not _DND_AVAILABLE:
            log.debug("tkinterdnd2 not available — drag-and-drop disabled")
            return
        try:
            self.root.drop_target_register(DND_FILES)
            self.root.dnd_bind('<<Drop>>', self._handle_drop)
            log.info("Drag-and-drop enabled for .hrc/.hrl/.hrt files")
        except Exception as e:
            log.warning(f"Drag-and-drop setup failed: {e}")

    def _set_icon(self, window) -> None:
        """Set app icon on any Toplevel window."""
        try:
            ico = os.path.join(os.path.dirname(os.path.abspath(__file__)), "main.ico")
            if os.path.exists(ico):
                window.iconbitmap(ico)
        except Exception:
            pass

    def _build_codec_args(self) -> list:
        """Return ffmpeg codec arguments based on self.video_codec and self.hw_accel."""
        codec = getattr(self, 'video_codec', 'libx264')
        hw = getattr(self, 'hw_accel', 'auto')

        # quality → CRF/QP mapping (quality 10-100 → crf 45-15)
        # Use enc_crf from advanced settings if set, else derive from quality slider
        if getattr(self, 'enc_crf', None) is not None:
            crf = self.enc_crf
        else:
            q = getattr(self, 'quality', 70)
            crf = int(45 - (q / 100) * 30)

        is_hw = codec in ('h264_nvenc', 'hevc_nvenc', 'h264_amf', 'hevc_amf', 'h264_qsv', 'hevc_qsv')
        is_265 = codec in ('libx265', 'hevc_nvenc', 'hevc_amf', 'hevc_qsv')

        args = []

        # Hardware decoder (input side) — only if hw_accel != none
        if hw != 'none' and is_hw:
            actual_hw = hw
            if hw == 'auto':
                if 'nvenc' in codec:
                    actual_hw = 'cuda'
                elif 'amf' in codec:
                    actual_hw = 'd3d11va'
                elif 'qsv' in codec:
                    actual_hw = 'dxva2'
                else:
                    actual_hw = None
            if actual_hw:
                args += ['-hwaccel', actual_hw]

        args += ['-c:v', codec]

        preset = getattr(self, 'enc_preset', 'ultrafast')
        if is_hw:
            args += ['-qp', str(crf)]
            if 'nvenc' in codec:
                args += ['-preset', 'p1', '-tune', 'ull']
            elif 'qsv' in codec:
                args += ['-preset', 'veryfast']
            elif 'amf' in codec:
                args += ['-quality', 'speed']
        else:
            args += ['-preset', preset, '-tune', 'zerolatency', '-crf', str(crf)]
            if is_265:
                args += ['-x265-params', 'log-level=error']

        log.debug(f"codec args: {args}")
        return args

    def start_audio_recording(self) -> None:
        try:
            if self.audio_thread and self.audio_thread.is_alive():
                self.audio_recording = False
                self.audio_thread.join(timeout=2)
            self.audio_thread = None
            self.audio_frames = []
            self.sys_audio_frames = []

            self.audio_channels = self.get_audio_channels()
            silence = b'\x00' * 1024 * 2 * self.audio_channels

            # ── Microphone stream ──────────────────────────────────────────
            self.audio_p = pyaudio.PyAudio()
            self.audio_stream = self.audio_p.open(
                format=pyaudio.paInt16,
                channels=self.audio_channels,
                rate=44100,
                input=True,
                frames_per_buffer=1024
            )
            self.audio_recording = True

            def record_mic() -> None:
                while self.audio_recording and not self.stop_flag:
                    if not self.paused:
                        if not self.audio_panel.mic_mute.get():
                            try:
                                data = self.audio_stream.read(1024, exception_on_overflow=False)
                                # Apply mic volume
                                vol = self.audio_panel.mic_volume.get() / 100.0
                                if vol != 1.0:
                                    data = audioop.mul(data, 2, vol)
                                self.audio_frames.append(data)
                                rms = audioop.rms(data, 2)
                                level = min(100, int(rms / 300))
                                self.audio_panel.update_mic_level(level)
                            except:
                                pass
                        else:
                            try:
                                self.audio_stream.read(1024, exception_on_overflow=False)
                            except:
                                pass
                            self.audio_frames.append(silence)
                    else:
                        self.audio_frames.append(silence)
                        time.sleep(1024 / 44100)

            self.audio_thread = threading.Thread(target=record_mic, daemon=True)
            self.audio_thread.start()
            log.info(f"Mic recording started ({self.audio_channels} channel(s))")

            # ── System audio via WASAPI loopback (PyAudio) ───────────────
            try:
                if not self.audio_panel.sys_mute.get():
                    self.sys_audio_p = pyaudio.PyAudio()
                    loopback_idx = self._find_wasapi_loopback(self.sys_audio_p)

                    if loopback_idx is not None:
                        # Try combinations: as_loopback flag x channel count
                        opened = False
                        sys_channels = 2
                        for use_loopback_flag in (True, False):
                            for ch in (2, 1):
                                try:
                                    kwargs = dict(
                                        format=pyaudio.paInt16,
                                        channels=ch,
                                        rate=44100,
                                        input=True,
                                        input_device_index=loopback_idx,
                                        frames_per_buffer=1024,
                                    )
                                    if use_loopback_flag:
                                        kwargs['as_loopback'] = True
                                    self.sys_audio_stream = self.sys_audio_p.open(**kwargs)
                                    opened = True
                                    sys_channels = ch
                                    log.info(f"System audio stream opened (as_loopback={use_loopback_flag}, ch={ch}, device={loopback_idx})")
                                    break
                                except (TypeError, OSError) as e:
                                    log.warning(f"sys audio open failed (as_loopback={use_loopback_flag}, ch={ch}): {e}")
                            if opened:
                                break

                        if opened:
                            self.sys_audio_recording = True
                            silence_sys = b'\x00' * 1024 * 2 * sys_channels
                            self._sys_channels = sys_channels

                            def record_sys() -> None:
                                while self.sys_audio_recording and not self.stop_flag:
                                    if not self.paused:
                                        try:
                                            data = self.sys_audio_stream.read(1024, exception_on_overflow=False)
                                            vol = self.audio_panel.sys_volume.get() / 100.0
                                            if vol != 1.0:
                                                data = audioop.mul(data, 2, vol)
                                            self.sys_audio_frames.append(data)
                                            rms = audioop.rms(data, 2)
                                            level = min(100, int(rms / 300))
                                            self.audio_panel.update_sys_level(level)
                                        except Exception as e:
                                            log.debug(f"sys audio read error: {e}")
                                    else:
                                        self.sys_audio_frames.append(silence_sys)
                                        time.sleep(1024 / 44100)

                            self.sys_audio_thread = threading.Thread(target=record_sys, daemon=True)
                            self.sys_audio_thread.start()
                            log.info(f"System audio recording started via WASAPI loopback (device index {loopback_idx})")
                        else:
                            log.warning("Could not open WASAPI loopback stream, falling through to dshow")
                            loopback_idx = None

                    if loopback_idx is None:
                        # Step 2: try PyAudio direct with Stereo Mix device
                        log.warning("WASAPI loopback not found, trying PyAudio Stereo Mix")
                        if self.sys_audio_p is None:
                            self.sys_audio_p = pyaudio.PyAudio()

                        stereo_mix_idx = None
                        stereo_mix_ch = 2

                        def _fix_pyaudio_name(name: str) -> str:
                            """PyAudio on Windows often gives UTF-8 bytes decoded as cp1251."""
                            try:
                                return name.encode('cp1251').decode('utf-8')
                            except Exception:
                                return name

                        keywords = [
                            'stereo mix', 'what u hear', 'loopback', 'mixer', 'wave out',
                            'стерео', 'микшер', 'что слышит',
                        ]

                        for i in range(self.sys_audio_p.get_device_count()):
                            try:
                                dev = self.sys_audio_p.get_device_info_by_index(i)
                                raw_name = dev.get('name', '')
                                fixed_name = _fix_pyaudio_name(raw_name)
                                max_in = int(dev.get('maxInputChannels', 0))
                                log.debug(f"PyAudio device [{i}]: {fixed_name!r} in={max_in}")
                                if max_in > 0:
                                    nl = fixed_name.lower()
                                    if any(k in nl for k in keywords):
                                        stereo_mix_idx = i
                                        stereo_mix_ch = min(max_in, 2)
                                        log.info(f"PyAudio Stereo Mix found: [{i}] {fixed_name!r} ch={stereo_mix_ch}")
                                        break
                            except Exception:
                                continue

                        if stereo_mix_idx is None:
                            log.warning("PyAudio Stereo Mix not found by keyword — trying dshow")

                        if stereo_mix_idx is not None:
                            opened2 = False
                            for ch2 in ([stereo_mix_ch] if stereo_mix_ch == 1 else [2, 1]):
                                try:
                                    self.sys_audio_stream = self.sys_audio_p.open(
                                        format=pyaudio.paInt16,
                                        channels=ch2,
                                        rate=44100,
                                        input=True,
                                        input_device_index=stereo_mix_idx,
                                        frames_per_buffer=1024,
                                    )
                                    opened2 = True
                                    stereo_mix_ch = ch2
                                    log.info(f"PyAudio Stereo Mix stream opened (ch={ch2})")
                                    break
                                except Exception as e:
                                    log.warning(f"PyAudio Stereo Mix open failed (ch={ch2}): {e}")

                            if opened2:
                                self.sys_audio_recording = True
                                self._sys_channels = stereo_mix_ch
                                _silence2 = b'\x00' * 1024 * 2 * stereo_mix_ch

                                def record_sys_mix() -> None:
                                    while self.sys_audio_recording and not self.stop_flag:
                                        if not self.paused:
                                            try:
                                                data = self.sys_audio_stream.read(1024, exception_on_overflow=False)
                                                vol = self.audio_panel.sys_volume.get() / 100.0
                                                if vol != 1.0:
                                                    data = audioop.mul(data, 2, vol)
                                                self.sys_audio_frames.append(data)
                                                rms = audioop.rms(data, 2)
                                                self.audio_panel.update_sys_level(min(100, int(rms / 300)))
                                            except Exception as e:
                                                log.debug(f"sys mix read error: {e}")
                                        else:
                                            self.sys_audio_frames.append(_silence2)
                                            time.sleep(1024 / 44100)

                                self.sys_audio_thread = threading.Thread(target=record_sys_mix, daemon=True)
                                self.sys_audio_thread.start()
                                log.info("System audio recording started via PyAudio Stereo Mix")
                                loopback_idx = "pyaudio_mix"  # mark as handled — skip dshow

                    if loopback_idx is None:
                        # Step 3: last resort — dshow ffmpeg
                        log.warning("WASAPI loopback device not found, trying dshow fallback")
                        try:
                            self.sys_audio_p.terminate()
                        except Exception:
                            pass
                        self.sys_audio_p = None

                        self.sys_audio_filename = self.filename.replace('.mp4', '_sys.wav')
                        devices = self.get_dshow_audio_devices()
                        sys_device = None
                        for d in devices:
                            dl = d.lower()
                            if any(k in dl for k in ['stereo mix', 'what u hear', 'loopback',
                                                       'стерео микшер', 'что слышит']):
                                sys_device = d
                                break
                        if not sys_device and devices:
                            sys_device = devices[0]

                        if sys_device and self.ffmpeg_path:
                            vol = self.audio_panel.sys_volume.get() / 100.0
                            vol_filter = f'volume={vol:.2f}' if vol != 1.0 else 'anull'
                            sys_cmd = [
                                self.ffmpeg_path, '-y',
                                '-f', 'dshow',
                                '-i', f'audio={sys_device}',
                                '-af', vol_filter,
                                '-acodec', 'pcm_s16le',
                                '-ar', '44100', '-ac', '2',
                                self.sys_audio_filename
                            ]
                            self.sys_ffmpeg_proc = subprocess.Popen(
                                sys_cmd,
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL,
                                stdin=subprocess.PIPE,
                                creationflags=subprocess.CREATE_NO_WINDOW if platform.system() == 'Windows' else 0
                            )
                            self.sys_audio_recording = True
                            log.info(f"System audio recording started via dshow fallback: {sys_device}")
                        else:
                            log.warning("No system audio device found (neither WASAPI loopback nor dshow Stereo Mix)")
                            self.sys_audio_filename = None
                            self.sys_audio_recording = False

            except Exception as e:
                log.warning(f"System audio unavailable: {e}")
                self.sys_audio_filename = None
                self.sys_audio_recording = False

        except Exception as e:
            log.error(f"Audio error: {e}")
            self.audio_recording = False
    
    def stop_audio_recording(self) -> str | None:
        # ── Stop mic ──────────────────────────────────────────────────────
        self.audio_recording = False
        if self.audio_thread and self.audio_thread.is_alive():
            self.audio_thread.join(timeout=2)
        self.audio_thread = None
        if self.audio_stream:
            try:
                self.audio_stream.stop_stream()
                self.audio_stream.close()
            except:
                pass
        self.audio_stream = None
        if self.audio_p:
            try:
                self.audio_p.terminate()
            except:
                pass
        self.audio_p = None

        # ── Stop system audio (WASAPI loopback thread) ────────────────────
        self.sys_audio_recording = False
        if hasattr(self, 'sys_audio_thread') and self.sys_audio_thread and self.sys_audio_thread.is_alive():
            self.sys_audio_thread.join(timeout=2)
        self.sys_audio_thread = None
        if hasattr(self, 'sys_audio_stream') and self.sys_audio_stream:
            try:
                self.sys_audio_stream.stop_stream()
                self.sys_audio_stream.close()
            except:
                pass
            self.sys_audio_stream = None
        if hasattr(self, 'sys_audio_p') and self.sys_audio_p:
            try:
                self.sys_audio_p.terminate()
            except:
                pass
            self.sys_audio_p = None

        # ── Stop system audio (dshow ffmpeg fallback) ─────────────────────
        if self.sys_ffmpeg_proc and self.sys_ffmpeg_proc.poll() is None:
            try:
                self.sys_ffmpeg_proc.stdin.write(b'q\n')
                self.sys_ffmpeg_proc.stdin.flush()
                self.sys_ffmpeg_proc.wait(timeout=8)
                log.debug("dshow sys audio ffmpeg stopped gracefully")
            except Exception as e:
                log.warning(f"dshow ffmpeg stop error: {e}")
                try:
                    self.sys_ffmpeg_proc.terminate()
                    self.sys_ffmpeg_proc.wait(timeout=3)
                except Exception:
                    try:
                        self.sys_ffmpeg_proc.kill()
                    except Exception:
                        pass
        self.sys_ffmpeg_proc = None
        import time as _t; _t.sleep(0.5)  # let OS flush file to disk

        mic_frames = self.audio_frames
        sys_frames = self.sys_audio_frames
        self.audio_frames = []
        self.sys_audio_frames = []

        has_mic = bool(mic_frames) and not self.audio_panel.mic_mute.get()

        # sys audio: either WASAPI frames or dshow wav file
        sys_wav = self.sys_audio_filename
        self.sys_audio_filename = None

        has_sys_frames = bool(sys_frames) and not self.audio_panel.sys_mute.get()
        has_sys_file = bool(sys_wav) and os.path.exists(sys_wav or '') and not self.audio_panel.sys_mute.get()

        # Write WASAPI frames to a wav so the mix logic below is uniform
        if has_sys_frames and not has_sys_file:
            sys_wav = self.filename.replace('.mp4', '_sys.wav')
            try:
                sys_ch = getattr(self, '_sys_channels', 2)
                wf = wave.open(sys_wav, 'wb')
                wf.setnchannels(sys_ch)
                wf.setsampwidth(2)
                wf.setframerate(44100)
                wf.writeframes(b''.join(sys_frames))
                wf.close()
                has_sys_file = True
                log.info(f"System audio (WASAPI) written: {sys_wav}")
            except Exception as e:
                log.warning(f"Failed to write system audio frames: {e}")
                has_sys_file = False

        has_sys = has_sys_file

        if not has_mic and not has_sys:
            return None

        audio_filename = self.filename.replace('.mp4', '_audio.wav')

        if has_mic and has_sys:
            # Mix mic WAV + sys WAV using ffmpeg amix
            try:
                mic_tmp = audio_filename + '_mic_tmp.wav'
                mix_cmd = [
                    self.ffmpeg_path,
                    '-y',
                    '-i', mic_tmp,
                    '-i', sys_wav,
                    '-filter_complex', 'amix=inputs=2:duration=longest:weights=1 1',
                    '-acodec', 'pcm_s16le',
                    audio_filename
                ]
                wf = wave.open(mic_tmp, 'wb')
                wf.setnchannels(self.audio_channels)
                wf.setsampwidth(2)
                wf.setframerate(44100)
                wf.writeframes(b''.join(mic_frames))
                wf.close()
                subprocess.run(mix_cmd, capture_output=True, timeout=30,
                               creationflags=subprocess.CREATE_NO_WINDOW if platform.system() == 'Windows' else 0)
                try:
                    os.remove(mic_tmp)
                    os.remove(sys_wav)
                except:
                    pass
                log.info(f"Mixed audio saved: {audio_filename}")
                return audio_filename
            except Exception as e:
                log.warning(f"Mix failed, using mic only: {e}")
                has_sys = False

        if has_sys and not has_mic:
            try:
                os.rename(sys_wav, audio_filename)
                log.info(f"System audio saved: {audio_filename}")
                return audio_filename
            except:
                return sys_wav

        # Only mic
        wf = wave.open(audio_filename, 'wb')
        wf.setnchannels(self.audio_channels)
        wf.setsampwidth(2)
        wf.setframerate(44100)
        wf.writeframes(b''.join(mic_frames))
        wf.close()
        log.info(f"Mic audio saved: {audio_filename}")
        return audio_filename  # ← was returning None! audio_filename
    
    def select_folder(self) -> None:
        folder = filedialog.askdirectory(initialdir=self.output_folder)
        if folder:
            self.output_folder = folder
            os.makedirs(folder, exist_ok=True)
            self.save_settings(silent=True)
    
    def open_recordings(self) -> None:
        if os.path.exists(self.output_folder):
            os.startfile(self.output_folder)
        else:
            messagebox.showwarning(self.lang["warning"], self.lang["folder_not_exist"])
    
    def start_with_countdown(self) -> None:
        if not self.recording:
            if self.countdown_var.get():
                self.show_countdown()
            else:
                self.start_recording()
        else:
            self.stop_recording()
    
    def show_countdown(self) -> None:
        w = tk.Toplevel(self.root)
        self._set_icon(w)
        w.geometry("350x180")
        w.configure(bg=self.colors["bg"])
        w.overrideredirect(True)
        w.update_idletasks()
        w.geometry(f"+{w.winfo_screenwidth()//2-175}+{w.winfo_screenheight()//2-90}")
        label = tk.Label(w, text="3", font=("Segoe UI", 56, "bold"),
                        bg=self.colors["bg"], fg=self.colors["success"])
        label.pack(expand=True)
        
        def tick(n: int) -> None:
            if n > 0:
                label.config(text=str(n))
                w.after(1000, lambda: tick(n - 1))
            else:
                label.config(text=self.lang["recording_btn"], fg=self.colors["error"])
                w.after(500, w.destroy)
                self.start_recording()
        tick(3)
    
    def update_preview(self) -> None:
        try:
            screenshot = self.sct.grab(self.monitor)
            img = Image.frombytes("RGB", screenshot.size, screenshot.bgra, "raw", "BGRX")
            resample = Image.Resampling.NEAREST if self.recording else Image.Resampling.BILINEAR
            img.thumbnail((self.preview_width, self.preview_height), resample)
            if self.recording and not self.paused:
                draw = ImageDraw.Draw(img)
                draw.ellipse([10, 10, 35, 35], fill=self.colors["error"])
            photo = ImageTk.PhotoImage(img)
            self.preview_label.config(image=photo)
            self.preview_label.image = photo
        except:
            pass
        delay = 333 if self.recording else 100
        self.root.after(delay, self.update_preview)
    
    def toggle_recording(self) -> None:
        if not self.recording:
            self.start_recording()
        else:
            self.stop_recording()
    
    def start_recording(self) -> None:
        """Start recording with STRICT fps control and correct monitor offsets"""
        try:
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            self.filename = f"{self.output_folder}/HomRec_{timestamp}.mp4"
            log.info("========== STARTING RECORDING ==========")
            self._notify_recording_start()
            log.info(f"File: {self.filename}")
            log.info(f"Audio enabled: {self.audio_panel.audio_enabled.get()}, FFmpeg: {self.check_ffmpeg()}")
            log.info(f"Monitor {self.monitor_id} at ({self.monitor_left}, {self.monitor_top})")
            if not self.ffmpeg_path:
                raise Exception("FFmpeg not found!")
            
            self.stop_flag = False
            self.paused = False
            self.frame_count = 0
            if hasattr(self, 'ffmpeg_reader_thread') and self.ffmpeg_reader_thread and self.ffmpeg_reader_thread.is_alive():
                self.ffmpeg_reader_thread.join(timeout=2)
            
            width, height = self.record_width, self.record_height
            fps = self.target_fps
            
            offset_x = self.monitor_left
            offset_y = self.monitor_top
            
            vf_filter = f'scale={width}:{height}' if (width != self.original_width or height != self.original_height) else 'null'

            # Build codec args based on selected codec
            codec_args = self._build_codec_args()
            log.info(f"Video codec: {self.video_codec} | hw_accel: {self.hw_accel}")

            if self.capture_mode == "window" and self.capture_window_title:
                # Record a specific window by title
                log.info(f"Capture mode: window — '{self.capture_window_title}'")
                cmd = [
                    self.ffmpeg_path,
                    '-y',
                    '-f', 'gdigrab',
                    '-framerate', str(fps),
                    '-i', f'title={self.capture_window_title}',
                    '-vf', vf_filter,
                    '-r', str(fps),
                    *codec_args,
                    '-pix_fmt', getattr(self, 'pix_fmt', 'yuv420p'),
                    '-movflags', '+faststart',
                    '-an',
                    self.filename
                ]
            else:
                # Record full desktop (default)
                log.info(f"Capture mode: desktop — monitor {self.monitor_id}")
                cmd = [
                    self.ffmpeg_path,
                    '-y',
                    '-f', 'gdigrab',
                    '-framerate', str(fps),
                    '-offset_x', str(offset_x),
                    '-offset_y', str(offset_y),
                    '-video_size', f'{self.original_width}x{self.original_height}',
                    '-i', 'desktop',
                    '-vf', vf_filter,
                    '-r', str(fps),
                    *codec_args,
                    '-pix_fmt', getattr(self, 'pix_fmt', 'yuv420p'),
                    '-movflags', '+faststart',
                    '-an',
                    self.filename
                ]
            log.debug(f"FFmpeg command: {chr(32).join(cmd)}")
            self.ffmpeg_proc = subprocess.Popen(
                cmd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                stdin=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW if platform.system() == "Windows" else 0
            )
            
            self.stop_ffmpeg_reader = False
            self.ffmpeg_reader_thread = threading.Thread(target=self._ffmpeg_reader, daemon=True)
            self.ffmpeg_reader_thread.start()
            
            if self.audio_panel.audio_enabled.get():
                self.start_audio_recording()
            
            self.recording = True
            self.start_time = time.time()
            
            self.record_btn.config(text=self.lang["stop"], bg=self.colors["error"], command=self.stop_recording)
            self.pause_btn.config(state="normal")
            self.stop_btn.config(state="normal")
            self.status_icon.config(fg=self.colors["success"])
            self.status_label.config(text=self.lang["recording"])
            
            self._update_stats()
            
        except Exception as e:
            messagebox.showerror(self.lang["error"], f"Failed to start recording:\n{str(e)}")
            log.exception("Failed to start recording")
    
    def _ffmpeg_reader(self) -> None:
        """Read ffmpeg stderr to get frame count"""
        while not self.stop_flag and self.ffmpeg_proc and self.ffmpeg_proc.poll() is None:
            try:
                line = self.ffmpeg_proc.stderr.readline()
                if not line:
                    break
                line = line.decode('utf-8', errors='ignore').strip()
                
                # Parse frame count from ffmpeg output
                if 'frame=' in line:
                    try:
                        parts = line.split()
                        for i, part in enumerate(parts):
                            if part == 'frame=':
                                frame_str = parts[i+1]
                                self.frame_count = int(frame_str)
                                break
                    except:
                        pass
            except:
                break
        log.debug("FFmpeg reader stopped")
    
    def _update_stats(self) -> None:
        if self.recording:
            try:
                elapsed = time.time() - self.start_time
                if elapsed > 0 and self.frame_count > 0:
                    actual_fps = self.frame_count / elapsed
                    self.fps_label.config(text=f"{self.lang['fps']} {actual_fps:.1f}")
                
                h = int(elapsed // 3600)
                m = int((elapsed % 3600) // 60)
                s = int(elapsed % 60)
                self.time_label.config(text=f"{h:02d}:{m:02d}:{s:02d}")
                
                if self.ffmpeg_proc and self.ffmpeg_proc.poll() is None:
                    status_text = self.lang["recording_status"].format(size=0, frames=self.frame_count)
                    self.file_label.config(text=status_text)
            except:
                pass
            
            self.root.after(500, self._update_stats)
    
    def stop_recording(self) -> None:
        """Stop recording"""
        log.info("Stopping recording...")
        self.recording = False
        self.stop_flag = True
        
        # Stop ffmpeg gracefully
        if self.ffmpeg_proc and self.ffmpeg_proc.poll() is None:
            try:
                # Send 'q' to ffmpeg to stop gracefully
                self.ffmpeg_proc.stdin.write(b'q')
                self.ffmpeg_proc.stdin.flush()
                self.ffmpeg_proc.wait(timeout=5)
            except:
                # Force kill if not responding
                try:
                    self.ffmpeg_proc.kill()
                except:
                    pass
        
        # Stop audio recording
        audio_file = None
        if self.audio_recording:
            audio_file = self.stop_audio_recording()
        
        # Wait a moment for files to be written
        time.sleep(0.5)
        
        # Merge audio if available
        has_ffmpeg = self.check_ffmpeg()
        audio_merged = False
        
        if audio_file and os.path.exists(self.filename) and self.audio_panel.audio_enabled.get():
            if has_ffmpeg:
                audio_merged = self.merge_audio_video(self.filename, audio_file)
        
        # Update UI
        self.record_btn.config(text=self.lang["start"], bg=self.colors["success"], command=self.start_with_countdown)
        self.pause_btn.config(state="disabled", text=self.lang["pause"])
        self.stop_btn.config(state="disabled")
        self.status_icon.config(fg=self.colors["error"])
        self.status_label.config(text=self.lang["ready"])
        self.time_label.config(text="00:00:00")
        
        # Show result
        if os.path.exists(self.filename):
            file_size = os.path.getsize(self.filename) / (1024 * 1024)
            duration = time.time() - self.start_time
            
            # Try to get actual video duration from ffmpeg
            try:
                probe_cmd = [self.ffmpeg_path, '-i', self.filename, '-f', 'null', '-']
                probe_result = subprocess.run(probe_cmd, capture_output=True, text=True,
                                            creationflags=subprocess.CREATE_NO_WINDOW if platform.system() == 'Windows' else 0)
                for line in probe_result.stderr.split('\n'):
                    if 'Duration:' in line:
                        import re
                        match = re.search(r'Duration: (\d+):(\d+):([\d.]+)', line)
                        if match:
                            h, m, s = match.groups()
                            duration = int(h) * 3600 + int(m) * 60 + float(s)
                        break
            except:
                pass
            
            self.file_label.config(text=self.lang["saved"].format(size=file_size, duration=duration))
            
            audio_status = self.lang["merged"] if audio_merged else (self.lang["separate"] if audio_file else self.lang["no_audio"])
            
            info_lines = [
                f"{self.lang['file']} {os.path.basename(self.filename)}",
                f"{self.lang['size']} {file_size:.1f} MB",
                f"{self.lang['duration']} {duration:.1f} sec",
                f"{self.lang['resolution']} {self.record_width}x{self.record_height}",
                f"{self.lang['fps']} {self.target_fps}",
                f"{self.lang['audio']} {audio_status}"
            ]
            
            if audio_file and not audio_merged:
                info_lines.append(f"{self.lang['audio_file']} {os.path.basename(audio_file)}")
            
            if not has_ffmpeg and audio_file:
                info_lines.append("")
                info_lines.append(self.lang["ffmpeg_not_found_msg"])
            
            info_text = "\n".join(info_lines)
            
            if self.show_summary:
                dont_show_var = tk.BooleanVar(value=False)
                result = CustomMessageBox.show(
                    self,
                    "recording_saved",
                    "recording_saved",
                    info_text,
                    dont_show_var
                )
                
                if dont_show_var.get():
                    self.show_summary = False
                    self.save_settings(silent=True)
                
                if result:
                    self.open_recordings()
        else:
            self.file_label.config(text=self.lang["recording_failed"])
            messagebox.showerror(self.lang["error"], self.lang["recording_failed"])
    
    def toggle_pause(self) -> None:
        if self.recording:
            self.paused = not self.paused
            if self.paused:
                self.pause_btn.config(text=self.lang["resume"], bg=self.colors["success"])
                self.status_icon.config(fg=self.colors["warning"])
                self.status_label.config(text=self.lang["paused"])
            else:
                self.pause_btn.config(text=self.lang["pause"], bg=self.colors["warning"])
                self.status_icon.config(fg=self.colors["success"])
                self.status_label.config(text=self.lang["recording"])
    
    # ── Update check ──────────────────────────────────────────────────────────

    def _manual_update_check(self) -> None:
        """Triggered from Help menu — shows result in a messagebox."""
        def on_found(latest):
            self.root.after(0, lambda: messagebox.showinfo(
                "Update available",
                f"HomRec v{latest} is available!\n\nhttps://github.com/{GITHUB_REPO}/releases"
            ))
        def no_update():
            pass
        def _fetch():
            import urllib.request, json as _json
            try:
                url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
                req = urllib.request.Request(url, headers={"User-Agent": "HomRec"})
                with urllib.request.urlopen(req, timeout=5) as resp:
                    data = _json.loads(resp.read().decode())
                tag = data.get("tag_name", "").lstrip("v")
                if tag and _version_gt(tag, CURRENT_VERSION):
                    on_found(tag)
                else:
                    self.root.after(0, lambda: messagebox.showinfo("No updates", f"You have the latest version (v{CURRENT_VERSION})."))
            except Exception as e:
                self.root.after(0, lambda: messagebox.showerror("Error", f"Could not check for updates:\n{e}"))
        threading.Thread(target=_fetch, daemon=True).start()

    def _open_issues(self) -> None:
        import webbrowser
        webbrowser.open(f"https://github.com/{GITHUB_REPO}/issues")

    def _start_update_check(self) -> None:
        check_for_updates(self._on_update_found)

    def _on_update_found(self, latest: str) -> None:
        """Called from background thread — schedule UI update on main thread."""
        self.root.after(0, lambda: self._show_update_banner(latest))

    def _show_update_banner(self, latest: str) -> None:
        """Show a subtle update notification at the bottom of the window."""
        try:
            banner = tk.Frame(self.root, bg=self.colors["warning"], height=28)
            banner.pack(side="bottom", fill="x", before=self.root.pack_slaves()[-1])
            banner.pack_propagate(False)

            msg = f"⬆  HomRec v{latest} is available!"
            tk.Label(banner, text=msg,
                     bg=self.colors["warning"], fg=self.colors["bg"],
                     font=("Segoe UI", 9, "bold")).pack(side="left", padx=12)

            def open_release():
                import webbrowser
                webbrowser.open(f"https://github.com/{GITHUB_REPO}/releases/latest")

            tk.Button(banner, text="Download",
                      command=open_release,
                      bg=self.colors["bg"], fg=self.colors["warning"],
                      font=("Segoe UI", 9, "bold"), relief="flat",
                      padx=10, pady=0, cursor="hand2").pack(side="left")

            tk.Button(banner, text="✕",
                      command=banner.destroy,
                      bg=self.colors["warning"], fg=self.colors["bg"],
                      font=("Segoe UI", 9), relief="flat",
                      padx=8, pady=0, cursor="hand2").pack(side="right", padx=4)

            log.info(f"Update banner shown for v{latest}")
        except Exception as e:
            log.warning(f"Failed to show update banner: {e}")

    def on_closing(self) -> None:
        if getattr(self, 'auto_save_profile', False):
            try:
                profile_path = os.path.join(
                    os.path.dirname(os.path.abspath(__file__)), "autosave.hrc")
                data = {
                    "hrc_version": 1,
                    "video_codec": getattr(self, "video_codec", "libx264"),
                    "hw_accel": getattr(self, "hw_accel", "auto"),
                    "enc_preset": getattr(self, "enc_preset", "ultrafast"),
                    "enc_crf": getattr(self, "enc_crf", 18),
                    "pix_fmt": getattr(self, "pix_fmt", "yuv420p"),
                    "audio_sample_rate": getattr(self, "audio_sample_rate", 44100),
                    "audio_aac_bitrate": getattr(self, "audio_aac_bitrate", "192k"),
                    "audio_out_channels": getattr(self, "audio_out_channels", 2),
                    "ui_theme": getattr(self, "ui_theme", "dark"),
                    "ui_scale": getattr(self, "ui_scale", 1.0),
                    "ui_font": getattr(self, "ui_font", "Segoe UI"),
                    "filename_template": getattr(self, "filename_template", "HomRec_{date}_{time}"),
                    "auto_stop_min": getattr(self, "auto_stop_min", 0),
                    "replay_buffer_sec": getattr(self, "replay_buffer_sec", 0),
                    "hotkey_start_stop": getattr(self, "hotkey_start_stop", "F9"),
                    "hotkey_pause": getattr(self, "hotkey_pause", "F10"),
                    "hotkey_fullscreen": getattr(self, "hotkey_fullscreen", "F11"),
                    "notify_sound": getattr(self, "notify_sound", True),
                    "notify_flash": getattr(self, "notify_flash", True),
                }
                with open(profile_path, "w", encoding="utf-8") as f:
                    json.dump(data, f, indent=2)
                log.info(f"Auto-saved profile to {profile_path}")
            except Exception as e:
                log.warning(f"Auto-save profile failed: {e}")

    def on_closing(self) -> None:
        """Minimise to tray on close (if enabled and pystray available), otherwise quit."""
        if HAS_TRAY and self.tray_icon and self.minimize_to_tray.get():
            self.root.withdraw()
            log.info("Minimised to tray")
        else:
            self.quit_app()

    def quit_app(self) -> None:
        """Fully exit the application."""
        if self.recording:
            result = messagebox.askyesno(self.lang["warning"], "Recording in progress! Stop and exit?")
            if result:
                self.stop_recording()
                time.sleep(0.5)
            else:
                return
        
        if self.tray_icon:
            try:
                self.tray_icon.stop()
            except:
                pass

        self.stop_flag = True
        if hasattr(self, 'ffmpeg_reader_thread') and self.ffmpeg_reader_thread and self.ffmpeg_reader_thread.is_alive():
            self.ffmpeg_reader_thread.join(timeout=1)
        
        self.root.destroy()

    # ── Tray ──────────────────────────────────────────────────────────────────

    def setup_tray(self) -> None:
        if not HAS_TRAY:
            return
        try:
            if getattr(sys, 'frozen', False):
                base_dir = os.path.dirname(sys.executable)
            else:
                base_dir = os.path.dirname(os.path.abspath(__file__))
            ico_path = os.path.join(base_dir, "icons", "main.ico")

            if os.path.exists(ico_path):
                img = Image.open(ico_path).convert("RGBA")
                log.info("Tray icon loaded from main.ico")
            else:
                img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
                d = ImageDraw.Draw(img)
                d.ellipse([4, 4, 60, 60], fill="#89b4fa")
                d.ellipse([20, 20, 44, 44], fill="#1e1e2e")
                d.ellipse([28, 28, 36, 36], fill="#f38ba8")
                log.warning("main.ico not found, using generated tray icon")

            menu = pystray.Menu(
                TrayItem("Show HomRec", self._tray_show, default=True),
                TrayItem("Start / Stop", self._tray_toggle),
                pystray.Menu.SEPARATOR,
                TrayItem("Quit", self._tray_quit),
            )
            self.tray_icon = pystray.Icon("HomRec", img, "HomRec", menu)
            t = threading.Thread(target=self.tray_icon.run, daemon=True)
            t.start()
            log.info("Tray icon started")
        except Exception as e:
            log.warning(f"Tray setup failed: {e}")
            self.tray_icon = None

    def _tray_show(self, icon=None, item=None) -> None:
        self.root.after(0, self.root.deiconify)

    def _tray_toggle(self, icon=None, item=None) -> None:
        self.root.after(0, self.toggle_recording)

    def _tray_quit(self, icon=None, item=None) -> None:
        self.root.after(0, self.quit_app)

    # ── Window picker ─────────────────────────────────────────────────────────

    def set_capture_desktop(self) -> None:
        self.capture_mode = "desktop"
        self.capture_window_title = ""
        log.info("Capture source set to: desktop")

    def get_open_windows(self) -> list[str]:
        """Return titles of all visible, non-empty windows (Windows only)."""
        titles = []
        if sys.platform != "win32":
            return titles
        
        import ctypes
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
                    if title and title not in titles:
                        titles.append(title)
            return True

        EnumWindows(EnumWindowsProc(callback), 0)
        return sorted(titles)

    def open_window_picker(self) -> None:
        """Show a dialog to pick a window to record."""
        windows = self.get_open_windows()
        if not windows:
            messagebox.showinfo("Info", "No open windows found.")
            return

        dlg = tk.Toplevel(self.root)
        self._set_icon(dlg)
        dlg.title("Select Window")
        dlg.geometry("480x380")
        dlg.configure(bg=self.colors["bg"])
        dlg.transient(self.root)
        dlg.grab_set()
        dlg.resizable(False, True)
        dlg.update_idletasks()
        x = self.root.winfo_x() + self.root.winfo_width() // 2 - 240
        y = self.root.winfo_y() + self.root.winfo_height() // 2 - 190
        dlg.geometry(f"+{x}+{y}")

        tk.Label(dlg, text="Select a window to record:",
                 bg=self.colors["bg"], fg=self.colors["text"],
                 font=("Segoe UI", 11, "bold")).pack(anchor="w", padx=15, pady=(15, 5))

        frame = tk.Frame(dlg, bg=self.colors["bg"])
        frame.pack(fill="both", expand=True, padx=15, pady=5)

        scrollbar = tk.Scrollbar(frame)
        scrollbar.pack(side="right", fill="y")

        listbox = tk.Listbox(frame, yscrollcommand=scrollbar.set,
                             bg=self.colors["surface"], fg=self.colors["text"],
                             selectbackground=self.colors["accent"],
                             font=("Segoe UI", 10), relief="flat",
                             activestyle="none", borderwidth=0)
        listbox.pack(side="left", fill="both", expand=True)
        scrollbar.config(command=listbox.yview)

        for w in windows:
            listbox.insert(tk.END, w)

        # Pre-select current window if already set
        if self.capture_window_title in windows:
            idx = windows.index(self.capture_window_title)
            listbox.selection_set(idx)
            listbox.see(idx)

        btn_frame = tk.Frame(dlg, bg=self.colors["bg"])
        btn_frame.pack(fill="x", padx=15, pady=12)

        def on_select():
            sel = listbox.curselection()
            if sel:
                self.capture_window_title = windows[sel[0]]
                self.capture_mode = "window"
                log.info(f"Capture source set to window: '{self.capture_window_title}'")
                dlg.destroy()

        def on_desktop():
            self.set_capture_desktop()
            dlg.destroy()

        tk.Button(btn_frame, text="Record this window", command=on_select,
                  bg=self.colors["accent"], fg=self.colors["bg"],
                  font=("Segoe UI", 10, "bold"), relief="flat",
                  padx=16, pady=6).pack(side="left", padx=(0, 8))
        tk.Button(btn_frame, text="Use full desktop", command=on_desktop,
                  bg=self.colors["surface"], fg=self.colors["text"],
                  font=("Segoe UI", 10), relief="flat",
                  padx=16, pady=6).pack(side="left")

if __name__ == "__main__":
    # Prevent duplicate tray icons — use a mutex on Windows
    import platform as _platform
    if _platform.system() == "Windows":
        import ctypes
        _mutex = ctypes.windll.kernel32.CreateMutexW(None, False, "HomRec_SingleInstance_142")
        if ctypes.windll.kernel32.GetLastError() == 183:  # ERROR_ALREADY_EXISTS
            import sys
            sys.exit(0)
    root = tk.Tk()
    app = HomRecScreen(root)
    root.mainloop()