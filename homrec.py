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

# ==================== LANGUAGE FILES ====================
LANGUAGES = {
    "en": {
        "app_title": "HomRec (v1.3.0)",
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
        "app_title": "HomRec (v1.3.0)",
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
        
        self.create_mic_section()
        self.create_system_section()
        self.create_devices_section()
    
    def create_mic_section(self) -> None:
        mic_frame = tk.Frame(self.frame, bg=self.app.colors["surface"])
        mic_frame.pack(fill='x', pady=5)
        
        header = tk.Frame(mic_frame, bg=self.app.colors["surface"])
        header.pack(fill='x')
        tk.Label(header, text=self.app.lang["microphone"], bg=self.app.colors["surface"], 
                fg='#a6e3a1', font=("Segoe UI", 10, 'bold')).pack(side='left')
        
        self.mic_mute_btn = tk.Button(header, text=self.app.lang["mute"], 
                                      command=self.toggle_mic_mute,
                                      bg=self.app.colors["surface_light"], 
                                      fg=self.app.colors["text"],
                                      font=("Segoe UI", 9),
                                      relief='flat', width=6, height=1,
                                      cursor='hand2')
        self.mic_mute_btn.pack(side='right', padx=2)
        
        volume_frame = tk.Frame(mic_frame, bg=self.app.colors["surface"])
        volume_frame.pack(fill='x', pady=5)
        tk.Label(volume_frame, text=self.app.lang["vol"], bg=self.app.colors["surface"], 
                fg=self.app.colors["text"], font=("Segoe UI", 9)).pack(side='left', padx=(0, 10))
        
        self.mic_volume = tk.Scale(volume_frame, from_=0, to=100, orient='horizontal',
                                   length=150, bg=self.app.colors["surface_light"], 
                                   fg=self.app.colors["text"],
                                   highlightthickness=0, troughcolor=self.app.colors["surface"],
                                   command=self.on_mic_volume_change)
        self.mic_volume.set(80)
        self.mic_volume.pack(side='left', padx=5)
        
        self.mic_volume_label = tk.Label(volume_frame, text="80%", 
                                        bg=self.app.colors["surface"], fg='#a6e3a1',
                                        font=("Segoe UI", 9, 'bold'))
        self.mic_volume_label.pack(side='left', padx=5)
        
        meter_frame = tk.Frame(mic_frame, bg=self.app.colors["surface"])
        meter_frame.pack(fill='x', pady=5)
        tk.Label(meter_frame, text=self.app.lang["level"], bg=self.app.colors["surface"], 
                fg=self.app.colors["text"], font=("Segoe UI", 9)).pack(side='left', padx=(0, 10))
        self.mic_meter = AudioLevelMeter(meter_frame, width=180, height=20,
                                        bg=self.app.colors["surface"])
        self.mic_meter.pack(side='left')
    
    def create_system_section(self) -> None:
        sys_frame = tk.Frame(self.frame, bg=self.app.colors["surface"])
        sys_frame.pack(fill='x', pady=5)
        
        header = tk.Frame(sys_frame, bg=self.app.colors["surface"])
        header.pack(fill='x')
        tk.Label(header, text=self.app.lang["desktop_audio"], bg=self.app.colors["surface"], 
                fg='#89b4fa', font=("Segoe UI", 10, 'bold')).pack(side='left')
        
        self.sys_mute_btn = tk.Button(header, text=self.app.lang["mute"],
                                      command=self.toggle_sys_mute,
                                      bg=self.app.colors["surface_light"], 
                                      fg=self.app.colors["text"],
                                      font=("Segoe UI", 9),
                                      relief='flat', width=6, height=1,
                                      cursor='hand2')
        self.sys_mute_btn.pack(side='right', padx=2)
        
        volume_frame = tk.Frame(sys_frame, bg=self.app.colors["surface"])
        volume_frame.pack(fill='x', pady=5)
        tk.Label(volume_frame, text=self.app.lang["vol"], bg=self.app.colors["surface"], 
                fg=self.app.colors["text"], font=("Segoe UI", 9)).pack(side='left', padx=(0, 10))
        
        self.sys_volume = tk.Scale(volume_frame, from_=0, to=100, orient='horizontal',
                                   length=150, bg=self.app.colors["surface_light"], 
                                   fg=self.app.colors["text"],
                                   highlightthickness=0, troughcolor=self.app.colors["surface"],
                                   command=self.on_sys_volume_change)
        self.sys_volume.set(50)
        self.sys_volume.pack(side='left', padx=5)
        
        self.sys_volume_label = tk.Label(volume_frame, text="50%",
                                        bg=self.app.colors["surface"], fg='#89b4fa',
                                        font=("Segoe UI", 9, 'bold'))
        self.sys_volume_label.pack(side='left', padx=5)
        
        meter_frame = tk.Frame(sys_frame, bg=self.app.colors["surface"])
        meter_frame.pack(fill='x', pady=5)
        tk.Label(meter_frame, text=self.app.lang["level"], bg=self.app.colors["surface"], 
                fg=self.app.colors["text"], font=("Segoe UI", 9)).pack(side='left', padx=(0, 10))
        self.sys_meter = AudioLevelMeter(meter_frame, width=180, height=20,
                                        bg=self.app.colors["surface"])
        self.sys_meter.pack(side='left')
    
    def create_devices_section(self) -> None:
        devices_frame = tk.Frame(self.frame, bg=self.app.colors["surface"])
        devices_frame.pack(fill='x', pady=5)
        
        enable_frame = tk.Frame(devices_frame, bg=self.app.colors["surface"])
        enable_frame.pack(fill='x', pady=5)
        self.audio_check = tk.Checkbutton(enable_frame, text=self.app.lang["enable_audio"],
                                         variable=self.audio_enabled,
                                         bg=self.app.colors["surface"], fg=self.app.colors["text"],
                                         selectcolor=self.app.colors["surface_light"],
                                         font=("Segoe UI", 9, 'bold'))
        self.audio_check.pack(side='left')
        
        ffmpeg_text = self.app.lang["ffmpeg_found"] if self.app.check_ffmpeg() else self.app.lang["ffmpeg_not_found"]
        ffmpeg_color = '#a6e3a1' if self.app.check_ffmpeg() else '#f38ba8'
        
        self.ffmpeg_label = tk.Label(devices_frame, text=ffmpeg_text,
                                     bg=self.app.colors["surface"], fg=ffmpeg_color,
                                     font=("Segoe UI", 8))
        self.ffmpeg_label.pack(pady=2)
    
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
        
        btn_frame = tk.Frame(self.dialog, bg=c["bg"])
        btn_frame.pack(fill="x", padx=10, pady=10)
        tk.Button(btn_frame, text=a.lang["save"], command=self.save_settings,
                 bg=a.colors["success"], fg=a.colors["bg"],
                 font=("Segoe UI", 10, "bold"), relief="flat", padx=20, pady=8).pack(side="right", padx=5)
        tk.Button(btn_frame, text=a.lang["cancel"], command=self.dialog.destroy,
                 bg=c["surface"], fg=c["text"], font=("Segoe UI", 10),
                 relief="flat", padx=20, pady=8).pack(side="right", padx=5)
    
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
        self.app.res_label.config(text=f"{self.app.lang['resolution']} {self.app.record_width}x{self.app.record_height}")
        self.app.save_settings(silent=True)
        self.dialog.destroy()
        messagebox.showinfo(self.app.lang["info"], self.app.lang["settings_saved"])

class HomRecScreen:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.current_language = "en"
        self.lang = LANGUAGES[self.current_language]
        
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
        
        self.ffmpeg_proc: subprocess.Popen | None = None
        self.ffmpeg_reader_thread: threading.Thread | None = None
        self.stop_ffmpeg_reader = False
        
        self.scale_factor = 0.75
        self.output_folder = "recordings"
        self.quality = 70
        self.target_fps = 15
        self.recording_mode = "balanced"
        self.show_summary = True
        
        self.always_on_top = tk.BooleanVar(value=False)
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
        self.root.bind('<F9>', lambda e: self.toggle_recording())
        self.root.bind('<F10>', lambda e: self.toggle_pause() if self.recording else None)
        self.root.bind('<F11>', lambda e: self.toggle_fullscreen())
        
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)
        log.info("HomRec v1.3.0 started, language: %s", self.current_language)
    
    def update_ui_language(self) -> None:
        self.root.title(self.lang["app_title"])
        self.recreate_widgets()
    
    def check_ffmpeg(self) -> bool:
        return self.ffmpeg_path is not None
    
    def merge_audio_video(self, video_file: str, audio_file: str) -> bool:
        if not os.path.exists(audio_file) or not self.ffmpeg_path:
            return False
        
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
            
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            
            if result.returncode == 0 and os.path.exists(output_file):
                os.remove(video_file)
                os.remove(audio_file)
                os.rename(output_file, video_file)
                return True
            return False
        except:
            return False
    
    def set_app_icon(self) -> None:
        try:
            icon_size = (64, 64)
            icon_image = Image.new('RGBA', icon_size, (0, 0, 0, 0))
            draw = ImageDraw.Draw(icon_image)
            draw.rectangle([10, 20, 54, 44], fill="#89b4fa", outline="#cdd6f4", width=2)
            draw.ellipse([25, 25, 39, 39], fill="#1e1e2e", outline="#cdd6f4", width=2)
            draw.ellipse([29, 29, 35, 35], fill="#89b4fa")
            draw.rectangle([45, 15, 50, 20], fill="#f9e2af")
            icon_photo = ImageTk.PhotoImage(icon_image)
            self.root.iconphoto(True, icon_photo)
            if sys.platform == "win32":
                ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("homrec.1.3.0")
        except:
            pass
    
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
    
    def get_theme_colors(self, theme: str) -> dict:
        if theme == "dark":
            return {
                "bg": "#1e1e2e",
                "fg": "#cdd6f4",
                "accent": "#89b4fa",
                "success": "#a6e3a1",
                "warning": "#f9e2af",
                "error": "#f38ba8",
                "surface": "#313244",
                "surface_light": "#45475a",
                "preview_bg": "#11111b",
                "text": "#cdd6f4",
                "text_secondary": "#a6adc8"
            }
        else:
            return {
                "bg": "#f5f5f5",
                "fg": "#2c3e50",
                "accent": "#3498db",
                "success": "#27ae60",
                "warning": "#f39c12",
                "error": "#e74c3c",
                "surface": "#ecf0f1",
                "surface_light": "#bdc3c7",
                "preview_bg": "#ffffff",
                "text": "#2c3e50",
                "text_secondary": "#7f8c8d"
            }
    
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
        file_menu.add_command(label=self.lang["exit"], command=self.on_closing)
        
        view_menu = tk.Menu(menubar, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        menubar.add_cascade(label=self.lang["view_menu"], menu=view_menu)
        
        view_menu.add_checkbutton(label=self.lang["always_on_top"],
                                 variable=self.always_on_top,
                                 command=self.toggle_always_on_top)
        view_menu.add_command(label=self.lang["fullscreen"], command=self.toggle_fullscreen)
        view_menu.add_separator()
        
        if HAS_PSUTIL:
            pc_menu = tk.Menu(view_menu, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
            view_menu.add_cascade(label=self.lang["pc_analytics"], menu=pc_menu)
            pc_menu.add_command(label=self.lang["cpu_info"], command=self.show_cpu_info)
            pc_menu.add_command(label=self.lang["ram_info"], command=self.show_ram_info)
            pc_menu.add_command(label=self.lang["disk_info"], command=self.show_disk_info)
            view_menu.add_separator()
        
        lang_menu = tk.Menu(view_menu, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        view_menu.add_cascade(label=self.lang["language"], menu=lang_menu)
        lang_menu.add_radiobutton(label="English", variable=self.language_var, value="en",
                                 command=lambda: self.change_language("en"))
        lang_menu.add_radiobutton(label="Русский", variable=self.language_var, value="ru",
                                 command=lambda: self.change_language("ru"))
        
        theme_menu = tk.Menu(view_menu, tearoff=0, bg=self.colors["surface"], fg=self.colors["fg"])
        view_menu.add_cascade(label=self.lang["theme"], menu=theme_menu)
        theme_menu.add_radiobutton(label=self.lang["dark"], variable=self.theme_var, value="dark",
                                  command=lambda: self.change_theme("dark"))
        theme_menu.add_radiobutton(label=self.lang["light"], variable=self.theme_var, value="light",
                                  command=lambda: self.change_theme("light"))
        
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
        if HAS_PSUTIL:
            cpu_count = psutil.cpu_count()
            cpu_percent = psutil.cpu_percent(interval=0.5)
            info = f"CPU Cores: {cpu_count}\nCurrent Usage: {cpu_percent}%"
            messagebox.showinfo(self.lang["cpu_info"], info)
    
    def show_ram_info(self) -> None:
        if HAS_PSUTIL:
            mem = psutil.virtual_memory()
            total_gb = mem.total / (1024**3)
            available_gb = mem.available / (1024**3)
            info = f"Total RAM: {total_gb:.2f} GB\nAvailable: {available_gb:.2f} GB\nUsed: {mem.percent}%"
            messagebox.showinfo(self.lang["ram_info"], info)
    
    def show_disk_info(self) -> None:
        if HAS_PSUTIL and os.path.exists(self.output_folder):
            disk = psutil.disk_usage(self.output_folder)
            total_gb = disk.total / (1024**3)
            free_gb = disk.free / (1024**3)
            info = f"Drive: {self.output_folder}\nTotal: {total_gb:.2f} GB\nFree: {free_gb:.2f} GB"
            messagebox.showinfo(self.lang["disk_info"], info)
    
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
        for widget in self.root.winfo_children():
            widget.destroy()
        self.create_menu()
        self.create_widgets()
    
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
            "show_summary": self.show_summary
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
        tk.Label(title_frame, text="v1.3.0", 
                font=("Segoe UI", 11), 
                bg=self.colors["surface"], 
                fg=self.colors["text_secondary"]).pack()
        
        btn_frame = tk.Frame(left_panel, bg=self.colors["surface"])
        btn_frame.pack(pady=25, padx=15, fill="x")
        
        self.record_btn = tk.Button(btn_frame, text=self.lang["start"], 
                                   command=self.start_with_countdown,
                                   bg=self.colors["success"], fg=self.colors["bg"],
                                   font=("Segoe UI", 13, "bold"),
                                   relief="flat", height=2, cursor="hand2")
        self.record_btn.pack(fill="x", pady=5)
        
        self.pause_btn = tk.Button(btn_frame, text=self.lang["pause"], 
                                  command=self.toggle_pause,
                                  bg=self.colors["warning"], fg=self.colors["bg"],
                                  font=("Segoe UI", 13, "bold"),
                                  state="disabled", relief="flat", height=2, cursor="hand2")
        self.pause_btn.pack(fill="x", pady=5)
        
        self.stop_btn = tk.Button(btn_frame, text=self.lang["stop"], 
                                 command=self.stop_recording,
                                 bg=self.colors["error"], fg=self.colors["bg"],
                                 font=("Segoe UI", 13, "bold"),
                                 state="disabled", relief="flat", height=2, cursor="hand2")
        self.stop_btn.pack(fill="x", pady=5)
        
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
    
    def start_audio_recording(self) -> None:
        try:
            if self.audio_thread and self.audio_thread.is_alive():
                self.audio_recording = False
                self.audio_thread.join(timeout=2)
            self.audio_thread = None
            self.audio_frames = []

            self.audio_channels = self.get_audio_channels()

            self.audio_p = pyaudio.PyAudio()
            self.audio_stream = self.audio_p.open(
                format=pyaudio.paInt16,
                channels=self.audio_channels,
                rate=44100,
                input=True,
                frames_per_buffer=1024
            )
            
            self.audio_recording = True
            
            def record_audio() -> None:
                silence = b'\x00' * 1024 * 2 * self.audio_channels  # 1024 samples of silence
                while self.audio_recording and not self.stop_flag:
                    if not self.paused:
                        if not self.audio_panel.mic_mute.get():
                            try:
                                data = self.audio_stream.read(1024, exception_on_overflow=False)
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
            self.audio_thread = threading.Thread(target=record_audio, daemon=True)
            self.audio_thread.start()
            log.info(f"Audio recording started ({self.audio_channels} channel(s))")
            
        except Exception as e:
            log.error(f"Audio error: {e}")
            self.audio_recording = False
    
    def stop_audio_recording(self) -> str | None:
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
        
        frames = self.audio_frames
        self.audio_frames = []
        
        if frames and not self.audio_panel.mic_mute.get():
            audio_filename = self.filename.replace('.mp4', '_audio.wav')
            wf = wave.open(audio_filename, 'wb')
            wf.setnchannels(self.audio_channels)
            wf.setsampwidth(2)
            wf.setframerate(44100)
            wf.writeframes(b''.join(frames))
            wf.close()
            log.info(f"Audio saved: {audio_filename}")
            return audio_filename
        return None
    
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
                '-c:v', 'libx264',
                '-preset', 'ultrafast',
                '-tune', 'zerolatency',
                '-crf', '28',
                '-pix_fmt', 'yuv420p',
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
            
            self.record_btn.config(text=self.lang["recording_btn"], bg=self.colors["error"])
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
        self.record_btn.config(text=self.lang["start"], bg=self.colors["success"])
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
                probe_result = subprocess.run(probe_cmd, capture_output=True, text=True)
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
    
    def on_closing(self) -> None:
        if self.recording:
            result = messagebox.askyesno(self.lang["warning"], "Recording in progress! Stop and exit?")
            if result:
                self.stop_recording()
                time.sleep(0.5)
            else:
                return
        
        self.stop_flag = True
        if hasattr(self, 'ffmpeg_reader_thread') and self.ffmpeg_reader_thread and self.ffmpeg_reader_thread.is_alive():
            self.ffmpeg_reader_thread.join(timeout=1)
        
        self.root.destroy()

if __name__ == "__main__":
    root = tk.Tk()
    app = HomRecScreen(root)
    root.mainloop()