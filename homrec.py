from __future__ import annotations
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import time
import os
import sys
import json
import threading
import queue
import subprocess
import shutil
import platform
import logging
import ctypes
from datetime import datetime
from PIL import Image, ImageTk, ImageDraw

# -- Logging ------------------------------------------------------------------
def _setup_logging() -> None:
    base = os.path.dirname(sys.executable if getattr(sys,"frozen",False)
                           else os.path.abspath(__file__))
    logging.basicConfig(
        level=logging.DEBUG,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
        handlers=[logging.FileHandler(os.path.join(base,"homrec.log"),
                                      encoding="utf-8")],
    )
_setup_logging()
log = logging.getLogger("homrec")

# -- C++ engine bridge --------------------------------------------------------
import engine as _eng
ENGINE_OK = _eng.load_engine()
if ENGINE_OK:
    log.info("C++ engine loaded - using native capture/preview/audio.")
else:
    log.warning("C++ engine NOT loaded - falling back to Python capture.")

# -- Optional deps ------------------------------------------------------------
try:
    import psutil; HAS_PSUTIL = True
except ImportError:
    HAS_PSUTIL = False

try:
    import pystray
    from pystray import MenuItem as TrayItem
    HAS_TRAY = True
except ImportError:
    HAS_TRAY = False

try:
    import mss as _mss; HAS_MSS = True
except ImportError:
    HAS_MSS = False

try:
    import pyaudio, wave, audioop; HAS_PYAUDIO = True
except ImportError:
    HAS_PYAUDIO = False

# -- Constants ----------------------------------------------------------------
APP_VERSION   = "1.4.4 (Legacy)"
SETTINGS_PATH = os.path.join(
    os.path.dirname(sys.executable if getattr(sys,"frozen",False)
                    else os.path.abspath(__file__)),
    "homrec_settings.json"
)
GITHUB_REPO = "homaaio/homrec"

# -- Language strings ----------------------------------------------------------
LANGUAGES = {
    "en": {
        "app_title":       f"HomRec ({APP_VERSION})",
        "live_preview":    "PREVIEW",
        "ready":           "Ready",
        "recording":       "Recording",
        "paused":          "Paused",
        "fps":             "FPS:",
        "resolution":      "Resolution:",
        "start":           "▶ START",
        "pause":           "⏸ PAUSE",
        "stop":            "■ STOP",
        "resume":          "▶ RESUME",
        "recording_btn":   "⏺ RECORDING",
        "audio_mixer":     "Audio Mixer",
        "microphone":      "Microphone",
        "desktop_audio":   "Desktop Audio",
        "mute":            "Mute",
        "unmute":          "Unmute",
        "vol":             "Vol:",
        "level":           "Level:",
        "enable_audio":    "Enable Audio",
        "ffmpeg_found":    "FFmpeg: ✅ Found",
        "ffmpeg_not_found":"FFmpeg: ❌ Not Found",
        "file_menu":       "File",
        "open_recordings": "Open Recordings Folder",
        "exit":            "Exit",
        "view_menu":       "View",
        "always_on_top":   "Always on Top",
        "fullscreen":      "Fullscreen  F11",
        "pc_analytics":    "PC Analytics",
        "help_menu":       "Help",
        "check_updates":   "Check for Updates",
        "report_issue":    "Report Issue",
        "capture_source":  "Capture Source",
        "full_desktop":    "Full Desktop",
        "select_window":   "Select Window...",
        "minimize_tray":   "Minimize to tray on close",
        "language":        "Language",
        "english":         "English",
        "russian":         "Русский",
        "theme":           "Theme",
        "dark":            "Dark",
        "light":           "Light",
        "settings_menu":   "Settings",
        "preferences":     "Preferences...",
        "performance_menu":"Performance",
        "ultra":           "Ultra (60 FPS)",
        "turbo":           "Turbo (30 FPS)",
        "balanced":        "Balanced (15 FPS)",
        "eco":             "Eco (8 FPS)",
        "stats":           "STATS",
        "time":            "TIME",
        "status":          "STATUS",
        "warning":         "Warning",
        "error":           "Error",
        "info":            "Info",
        "folder_not_exist":"Folder doesn't exist!",
        "recording_failed":"Recording failed!",
        "settings_saved":  "Settings saved!",
        "recording_saved": "✅ Recording Saved!",
        "open_folder":     "Open folder?",
        "ffmpeg_not_found_msg": "⚠️ FFmpeg not found",
        "saved":           "✅ Saved: {size:.1f} MB | {duration:.1f}s",
        "recording_status":"Recording: {frames} frames",
        "file":            "📁 File:",
        "size":            "📊 Size:",
        "duration":        "⏱️ Duration:",
        "audio":           "🎤 Audio:",
        "merged":          "Merged",
        "separate":        "Separate",
        "no_audio":        "No",
        "save":            "Save",
        "cancel":          "Cancel",
        "browse":          "Browse",
        "output_folder":   "Output folder:",
        "settings_title":  "Settings",
        "video_settings":  "Video",
        "quality":         "Quality:",
        "mode":            "Mode:",
        "monitor":         "Monitor:",
        "output":          "Output:",
        "countdown":       "Countdown (3s)",
        "timestamp":       "Timestamp",
        "cursor":          "Cursor",
        "notification":    "Show summary",
        "made_by":         "Homa4ella",
        "audio_file":      "🎵 Audio file:",
        "codec":           "Codec:",
        "preset":          "Preset:",
        "crf":             "CRF:",
        "hw_accel":        "HW Accel:",
        "sample_rate":     "Sample rate:",
        "aac_bitrate":     "AAC bitrate:",
        "auto_stop":       "Auto-stop (min):",
        "hotkey_start":    "Hotkey Start/Stop:",
        "hotkey_pause":    "Hotkey Pause:",
        "notify_sound":    "Sound on start",
        "notify_flash":    "Flash border",
    },
    "ru": {
        "app_title":       f"HomRec ({APP_VERSION})",
        "live_preview":    "ПРЕДПРОСМОТР",
        "ready":           "Готов",
        "recording":       "Запись",
        "paused":          "Пауза",
        "fps":             "FPS:",
        "resolution":      "Разрешение:",
        "start":           "▶ СТАРТ",
        "pause":           "⏸ ПАУЗА",
        "stop":            "■ СТОП",
        "resume":          "▶ ПРОДОЛЖИТЬ",
        "recording_btn":   "⏺ ЗАПИСЬ",
        "audio_mixer":     "Аудио Микшер",
        "microphone":      "Микрофон",
        "desktop_audio":   "Системный звук",
        "mute":            "Выкл",
        "unmute":          "Вкл",
        "vol":             "Громк:",
        "level":           "Уровень:",
        "enable_audio":    "Запись звука",
        "ffmpeg_found":    "FFmpeg: ✅ Найден",
        "ffmpeg_not_found":"FFmpeg: ❌ Не найден",
        "file_menu":       "Файл",
        "open_recordings": "Открыть папку",
        "exit":            "Выход",
        "view_menu":       "Вид",
        "always_on_top":   "Поверх окон",
        "fullscreen":      "Полный экран F11",
        "pc_analytics":    "Аналитика",
        "help_menu":       "Справка",
        "check_updates":   "Проверить обновления",
        "report_issue":    "Сообщить об ошибке",
        "capture_source":  "Источник",
        "full_desktop":    "Весь экран",
        "select_window":   "Выбрать окно...",
        "minimize_tray":   "Сворачивать в трей",
        "language":        "Язык",
        "english":         "English",
        "russian":         "Русский",
        "theme":           "Тема",
        "dark":            "Темная",
        "light":           "Светлая",
        "settings_menu":   "Настройки",
        "preferences":     "Параметры...",
        "performance_menu":"Производительность",
        "ultra":           "Ультра (60 FPS)",
        "turbo":           "Турбо (30 FPS)",
        "balanced":        "Средний (15 FPS)",
        "eco":             "Эко (8 FPS)",
        "stats":           "СТАТИСТИКА",
        "time":            "ВРЕМЯ",
        "status":          "СТАТУС",
        "warning":         "Предупреждение",
        "error":           "Ошибка",
        "info":            "Информация",
        "folder_not_exist":"Папка не существует!",
        "recording_failed":"Ошибка записи!",
        "settings_saved":  "Настройки сохранены!",
        "recording_saved": "✅ Запись сохранена!",
        "open_folder":     "Открыть папку?",
        "ffmpeg_not_found_msg": "⚠️ FFmpeg не найден",
        "saved":           "✅ Сохранено: {size:.1f} МБ | {duration:.1f}с",
        "recording_status":"Запись: {frames} кадров",
        "file":            "📁 Файл:",
        "size":            "📊 Размер:",
        "duration":        "⏱️ Длительность:",
        "audio":           "🎤 Аудио:",
        "merged":          "Объединено",
        "separate":        "Отдельно",
        "no_audio":        "Нет",
        "save":            "Сохранить",
        "cancel":          "Отмена",
        "browse":          "Обзор",
        "output_folder":   "Папка записей:",
        "settings_title":  "Настройки",
        "video_settings":  "Видео",
        "quality":         "Качество:",
        "mode":            "Режим:",
        "monitor":         "Монитор:",
        "output":          "Папка:",
        "countdown":       "Отсчет (3с)",
        "timestamp":       "Время",
        "cursor":          "Курсор",
        "notification":    "Показывать сводку",
        "made_by":         "Homa4ella",
        "audio_file":      "🎵 Аудио файл:",
        "codec":           "Кодек:",
        "preset":          "Пресет:",
        "crf":             "CRF:",
        "hw_accel":        "HW Ускорение:",
        "sample_rate":     "Частота дискр.:",
        "aac_bitrate":     "Битрейт AAC:",
        "auto_stop":       "Авто-стоп (мин):",
        "hotkey_start":    "Горячая кнопка старт:",
        "hotkey_pause":    "Горячая кнопка пауза:",
        "notify_sound":    "Звук при старте",
        "notify_flash":    "Мигание рамки",
    },
}

# -- Built-in themes -----------------------------------------------------------
BUILTIN_THEMES = {
    "dark": {
        "bg":"#1e1e2e","fg":"#cdd6f4","accent":"#89b4fa",
        "success":"#a6e3a1","warning":"#f9e2af","error":"#f38ba8",
        "surface":"#313244","surface_light":"#45475a",
        "preview_bg":"#11111b","text":"#cdd6f4","text_secondary":"#a6adc8",
    },
    "light": {
        "bg":"#f5f5f5","fg":"#2c3e50","accent":"#3498db",
        "success":"#27ae60","warning":"#f39c12","error":"#e74c3c",
        "surface":"#ecf0f1","surface_light":"#bdc3c7",
        "preview_bg":"#ffffff","text":"#2c3e50","text_secondary":"#7f8c8d",
    },
    "catppuccin": {
        "bg":"#1e1e2e","fg":"#cdd6f4","accent":"#cba6f7",
        "success":"#a6e3a1","warning":"#f9e2af","error":"#f38ba8",
        "surface":"#181825","surface_light":"#313244",
        "preview_bg":"#11111b","text":"#cdd6f4","text_secondary":"#6c7086",
    },
    "nord": {
        "bg":"#2e3440","fg":"#eceff4","accent":"#88c0d0",
        "success":"#a3be8c","warning":"#ebcb8b","error":"#bf616a",
        "surface":"#3b4252","surface_light":"#434c5e",
        "preview_bg":"#242933","text":"#eceff4","text_secondary":"#d8dee9",
    },
    "dracula": {
        "bg":"#282a36","fg":"#f8f8f2","accent":"#bd93f9",
        "success":"#50fa7b","warning":"#f1fa8c","error":"#ff5555",
        "surface":"#44475a","surface_light":"#6272a4",
        "preview_bg":"#21222c","text":"#f8f8f2","text_secondary":"#6272a4",
    },
}

# -- Helpers -------------------------------------------------------------------
def find_ffmpeg() -> str | None:
    base = os.path.dirname(sys.executable if getattr(sys,"frozen",False)
                           else os.path.abspath(__file__))
    for name in ("ffmpeg.exe","ffmpeg"):
        p = os.path.join(base, name)
        if os.path.exists(p): return p
    for name in ("ffmpeg.exe","ffmpeg"):
        if os.path.exists(name): return os.path.abspath(name)
    return shutil.which("ffmpeg")

def _version_gt(a: str, b: str) -> bool:
    try:
        return (tuple(int(x) for x in a.split(".")) >
                tuple(int(x) for x in b.split(".")))
    except Exception:
        return False

# ----------------------- Audio Level Meter -----------------------------------
class AudioLevelMeter(tk.Canvas):
    def __init__(self, parent, width=180, height=20, **kw):
        super().__init__(parent, width=width, height=height,
                         highlightthickness=0, **kw)
        self.w, self.h = width, height
        self.level = 0
        self._draw()

    def _draw(self):
        self.delete("all")
        self.create_rectangle(0,0,self.w,self.h, fill="#45475a", outline="")
        bw = int((self.level/100)*(self.w-4))
        if bw > 0:
            c = "#a6e3a1" if self.level < 70 else "#f9e2af" if self.level < 90 else "#f38ba8"
            self.create_rectangle(2,2,bw,self.h-2, fill=c, outline="")
        for i in range(0,101,25):
            x = int((i/100)*self.w)
            self.create_line(x,0,x,self.h, fill="#1e1e2e", width=1)

    def set_level(self, v: float):
        self.level = max(0, min(100, v))
        self._draw()

# ----------------------- Settings Dialog (all settings) ----------------------
class SettingsDialog:
    def __init__(self, parent, app):
        self.app = app
        c = app.colors
        d = tk.Toplevel(parent)
        self.d = d
        d.title(app.lang["settings_title"])
        d.geometry("520x640")
        d.resizable(False, True)
        d.configure(bg=c["bg"])
        d.transient(parent)
        d.grab_set()
        app._set_icon(d)
        d.update_idletasks()
        x = parent.winfo_x() + parent.winfo_width()//2 - 260
        y = parent.winfo_y() + parent.winfo_height()//2 - 320
        d.geometry(f"+{x}+{y}")

        nb = ttk.Notebook(d)
        nb.pack(fill="both", expand=True, padx=12, pady=10)

        # -- Video tab ------------------------------------------------
        vt = tk.Frame(nb, bg=c["bg"]); nb.add(vt, text="Video")
        self._mode_v = tk.StringVar(value=app.recording_mode)
        self._row(vt, app.lang["mode"],
                  ttk.Combobox(vt, textvariable=self._mode_v,
                               values=["ultra","turbo","balanced","eco"],
                               width=12, state="readonly"))
        self._mon_v = tk.StringVar(value=str(app.monitor_id))
        mon_count = len(_eng.get_monitors()) if ENGINE_OK else 1
        self._row(vt, app.lang["monitor"],
                  ttk.Combobox(vt, textvariable=self._mon_v,
                               values=[str(i) for i in range(mon_count)],
                               width=6, state="readonly"))
        self._codec_v = tk.StringVar(value=getattr(app,"video_codec","libx264"))
        self._row(vt, app.lang["codec"],
                  ttk.Combobox(vt, textvariable=self._codec_v,
                               values=["libx264","libx265",
                                       "h264_nvenc","hevc_nvenc",
                                       "h264_amf","hevc_amf","h264_qsv"],
                               width=16, state="readonly"))
        self._preset_v = tk.StringVar(value=getattr(app,"enc_preset","ultrafast"))
        self._row(vt, app.lang["preset"],
                  ttk.Combobox(vt, textvariable=self._preset_v,
                               values=["ultrafast","superfast","veryfast",
                                       "faster","fast","medium"],
                               width=12, state="readonly"))
        self._crf_v = tk.IntVar(value=getattr(app,"enc_crf",18))
        self._row(vt, app.lang["crf"],
                  tk.Scale(vt, variable=self._crf_v, from_=0, to=51,
                           orient="horizontal", length=160,
                           bg=c["bg"], fg=c["text"], highlightthickness=0,
                           troughcolor=c["surface"]))
        self._hw_v = tk.StringVar(value=getattr(app,"hw_accel","auto"))
        self._row(vt, app.lang["hw_accel"],
                  ttk.Combobox(vt, textvariable=self._hw_v,
                               values=["auto","none","cuda","dxva2","d3d11va"],
                               width=10, state="readonly"))

        # -- Audio tab ------------------------------------------------
        at = tk.Frame(nb, bg=c["bg"]); nb.add(at, text="Audio")
        self._sr_v = tk.StringVar(value=str(getattr(app,"audio_sample_rate",44100)))
        self._row(at, app.lang["sample_rate"],
                  ttk.Combobox(at, textvariable=self._sr_v,
                               values=["22050","44100","48000"],
                               width=8, state="readonly"))
        self._abr_v = tk.StringVar(value=getattr(app,"audio_aac_bitrate","128k"))
        self._row(at, app.lang["aac_bitrate"],
                  ttk.Combobox(at, textvariable=self._abr_v,
                               values=["96k","128k","192k","256k"],
                               width=8, state="readonly"))

        # -- Recording tab --------------------------------------------
        rt = tk.Frame(nb, bg=c["bg"]); nb.add(rt, text="Recording")
        self._folder_v = tk.StringVar(value=app.output_folder)
        frow = tk.Frame(rt, bg=c["bg"])
        frow.grid(row=0,column=0,columnspan=3,sticky="ew",padx=12,pady=6)
        tk.Label(frow, text=app.lang["output_folder"],
                 bg=c["bg"], fg=c["text"], font=("Segoe UI",10)).pack(anchor="w")
        fe = tk.Frame(frow, bg=c["bg"]); fe.pack(fill="x")
        tk.Entry(fe, textvariable=self._folder_v,
                 bg=c["surface"], fg=c["text"], font=("Segoe UI",9),
                 relief="flat").pack(side="left", fill="x", expand=True)
        tk.Button(fe, text=app.lang["browse"],
                  command=self._browse, bg=c["surface"], fg=c["text"],
                  font=("Segoe UI",9), relief="flat", padx=8).pack(side="left",padx=4)

        self._countdown_v = tk.BooleanVar(value=app.countdown_var.get())
        self._timestamp_v = tk.BooleanVar(value=app.timestamp_var.get())
        self._cursor_v    = tk.BooleanVar(value=app.cursor_var.get())
        self._summary_v   = tk.BooleanVar(value=app.show_summary)
        self._tray_v      = tk.BooleanVar(value=app.minimize_to_tray.get())
        for txt, var in [
            (app.lang["countdown"],    self._countdown_v),
            (app.lang["timestamp"],    self._timestamp_v),
            (app.lang["cursor"],       self._cursor_v),
            (app.lang["notification"], self._summary_v),
            (app.lang["minimize_tray"],self._tray_v),
        ]:
            r = rt.grid_size()[1]
            tk.Checkbutton(rt, text=txt, variable=var,
                           bg=c["bg"], fg=c["text"],
                           selectcolor=c["surface"],
                           font=("Segoe UI",10)).grid(
                row=r, column=0, columnspan=2,
                sticky="w", padx=12, pady=3)

        self._as_v = tk.StringVar(value=str(getattr(app,"auto_stop_min",0)))
        self._row(rt, app.lang["auto_stop"],
                  tk.Spinbox(rt, textvariable=self._as_v,
                             from_=0, to=480, width=6,
                             bg=c["surface"], fg=c["text"], relief="flat"))

        # -- Hotkeys tab ----------------------------------------------
        ht = tk.Frame(nb, bg=c["bg"]); nb.add(ht, text="Hotkeys")
        tk.Label(ht, text="Click a field and press any key combination",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI",9)).grid(row=0,column=0,columnspan=2,
                                           padx=12,pady=(10,4),sticky="w")
        self._hk_ss = tk.StringVar(value=getattr(app,"hotkey_start_stop","F9"))
        self._hk_p  = tk.StringVar(value=getattr(app,"hotkey_pause","F10"))
        for label, var in [(app.lang["hotkey_start"], self._hk_ss),
                           (app.lang["hotkey_pause"],  self._hk_p)]:
            e = tk.Entry(ht, textvariable=var,
                         bg=c["surface"], fg=c["accent"],
                         font=("Consolas",11), relief="flat", width=12,
                         readonlybackground=c["surface"], state="readonly")
            e.bind("<FocusIn>",  lambda ev, v=var, en=e: self._start_key(v,en))
            e.bind("<FocusOut>", lambda ev, en=e: en.config(state="readonly"))
            self._row(ht, label, e)

        # -- Appearance tab -------------------------------------------
        apt = tk.Frame(nb, bg=c["bg"]); nb.add(apt, text="Appearance")
        self._theme_v = tk.StringVar(value=app.current_theme)
        self._row(apt, app.lang["theme"],
                  ttk.Combobox(apt, textvariable=self._theme_v,
                               values=list(BUILTIN_THEMES.keys()),
                               width=14, state="readonly"))
        self._lang_v = tk.StringVar(value=app.current_language)
        self._row(apt, app.lang["language"],
                  ttk.Combobox(apt, textvariable=self._lang_v,
                               values=["en","ru"], width=6, state="readonly"))
        self._ns_v = tk.BooleanVar(value=getattr(app,"notify_sound",True))
        self._nf_v = tk.BooleanVar(value=getattr(app,"notify_flash",True))
        for txt, var in [(app.lang["notify_sound"],self._ns_v),
                         (app.lang["notify_flash"], self._nf_v)]:
            r = apt.grid_size()[1]
            tk.Checkbutton(apt, text=txt, variable=var,
                           bg=c["bg"], fg=c["text"],
                           selectcolor=c["surface"],
                           font=("Segoe UI",10)).grid(
                row=r, column=0, columnspan=2,
                sticky="w", padx=12, pady=3)

        # -- Bottom ---------------------------------------------------
        sep = tk.Frame(d, bg=c["surface"], height=1)
        sep.pack(fill="x", padx=12)
        bot = tk.Frame(d, bg=c["bg"]); bot.pack(fill="x", padx=12, pady=8)
        tk.Button(bot, text=app.lang["cancel"], command=d.destroy,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI",9), relief="flat", padx=12, pady=6).pack(side="right",padx=(4,0))
        tk.Button(bot, text=app.lang["save"], command=self._save,
                  bg=c["success"], fg=c["bg"],
                  font=("Segoe UI",9,"bold"), relief="flat", padx=16, pady=6).pack(side="right")

    def _row(self, parent, label, widget):
        r = parent.grid_size()[1]
        tk.Label(parent, text=label, bg=self.app.colors["bg"],
                 fg=self.app.colors["text"],
                 font=("Segoe UI",10), anchor="w").grid(
            row=r, column=0, sticky="w", padx=(12,8), pady=5)
        widget.grid(row=r, column=1, sticky="w", padx=(0,12), pady=5)
        parent.columnconfigure(1, weight=1)

    def _browse(self):
        f = filedialog.askdirectory(initialdir=self._folder_v.get())
        if f: self._folder_v.set(f)

    def _start_key(self, var, entry):
        entry.config(state="normal"); var.set("Press a key...")
        def on_key(ev):
            parts = []
            if ev.state & 0x4: parts.append("Control")
            if ev.state & 0x1: parts.append("Shift")
            if ev.state & 0x8: parts.append("Alt")
            k = ev.keysym
            if k not in ("Control_L","Control_R","Shift_L","Shift_R","Alt_L","Alt_R"):
                parts.append(k)
            if parts: var.set("+".join(parts))
            entry.config(state="readonly"); entry.unbind("<KeyPress>")
        entry.bind("<KeyPress>", on_key)

    def _save(self):
        a = self.app
        a.recording_mode    = self._mode_v.get()
        a.monitor_id        = int(self._mon_v.get())
        a.video_codec       = self._codec_v.get()
        a.enc_preset        = self._preset_v.get()
        a.enc_crf           = self._crf_v.get()
        a.hw_accel          = self._hw_v.get()
        a.audio_sample_rate = int(self._sr_v.get())
        a.audio_aac_bitrate = self._abr_v.get()
        a.output_folder     = self._folder_v.get()
        a.countdown_var.set(self._countdown_v.get())
        a.timestamp_var.set(self._timestamp_v.get())
        a.cursor_var.set(self._cursor_v.get())
        a.show_summary      = self._summary_v.get()
        a.minimize_to_tray.set(self._tray_v.get())
        a.auto_stop_min     = int(self._as_v.get())
        a.hotkey_start_stop = self._hk_ss.get()
        a.hotkey_pause      = self._hk_p.get()
        a.notify_sound      = self._ns_v.get()
        a.notify_flash      = self._nf_v.get()
        new_lang  = self._lang_v.get()
        new_theme = self._theme_v.get()
        a.update_mode_settings()
        os.makedirs(a.output_folder, exist_ok=True)
        a.save_settings(silent=True)
        if new_lang != a.current_language:
            a.current_language = new_lang
            a.lang = LANGUAGES[new_lang]
        if new_theme != a.current_theme:
            a.current_theme = new_theme
            a.colors = BUILTIN_THEMES.get(new_theme, BUILTIN_THEMES["dark"])
            a.apply_theme(); a.recreate_widgets()
        else:
            a.update_monitor_info()
        self.d.destroy()
        messagebox.showinfo(a.lang["info"], a.lang["settings_saved"])

# ----------------------- Main Application ------------------------------------
class HomRecApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title(f"HomRec ({APP_VERSION})")
        self.root.geometry("960x620")
        self.root.minsize(780, 520)

        # Defaults
        self.recording        = False
        self.paused           = False
        self.stop_flag        = False
        self.frame_count      = 0
        self.start_time       = 0.0
        self.filename         = ""
        self.ffmpeg_proc      = None
        self.audio_recording  = False
        self.tray_icon        = None
        self.capture_mode     = "desktop"
        self.capture_window_title = ""

        # Settings defaults
        self.output_folder    = "recordings"
        self.recording_mode   = "balanced"
        self.target_fps       = 15
        self.quality          = 70
        self.scale_factor     = 0.75
        self.monitor_id       = 0
        self.current_theme    = "dark"
        self.current_language = "en"
        self.video_codec      = "libx264"
        self.enc_preset       = "ultrafast"
        self.enc_crf          = 18
        self.hw_accel         = "auto"
        self.audio_sample_rate= 44100
        self.audio_aac_bitrate= "128k"
        self.auto_stop_min    = 0
        self.hotkey_start_stop= "F9"
        self.hotkey_pause     = "F10"
        self.notify_sound     = True
        self.notify_flash     = True
        self.show_summary     = True

        self.always_on_top    = tk.BooleanVar(value=False)
        self.countdown_var    = tk.BooleanVar(value=True)
        self.timestamp_var    = tk.BooleanVar(value=False)
        self.cursor_var       = tk.BooleanVar(value=False)
        self.minimize_to_tray = tk.BooleanVar(value=True)
        self.language_var     = tk.StringVar(value="en")
        self.theme_var        = tk.StringVar(value="dark")
        self.mic_vol_var      = tk.DoubleVar(value=1.0)
        self.sys_vol_var      = tk.DoubleVar(value=1.0)
        self.mic_muted        = False
        self.sys_muted        = False
        self.audio_enabled    = tk.BooleanVar(value=True)

        # Preview
        self._preview_running = False
        self._preview_queue: queue.Queue = queue.Queue(maxsize=1)
        self._rec_frames: list = []
        self._rec_frame_idx   = 0
        self.preview_width    = 640
        self.preview_height   = 360

        self.load_settings()
        self.lang   = LANGUAGES[self.current_language]
        self.colors = BUILTIN_THEMES.get(self.current_theme, BUILTIN_THEMES["dark"])

        self.ffmpeg_path = find_ffmpeg()

        import mss as _mss_init
        self.sct = _mss_init.mss()
        self.update_monitor_info()   # теперь sct уже существует

        # Restart preview after monitor info is ready

        self.set_app_icon()
        self.apply_theme()
        self.create_menu()
        self.create_widgets()

        self._rec_frames = self._make_rec_frames()
        self._start_preview()
        self.update_preview()

        if HAS_TRAY: self.setup_tray()
        self.root.bind("<F11>", lambda e: self.toggle_fullscreen())
        self.root.bind("<Configure>", self.on_window_resize)
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)

        # Check for updates in background
        threading.Thread(target=self._bg_update_check, daemon=True).start()

    # -- Monitor info ---------------------------------------------------------
    def update_monitor_info(self):
        try:
            monitors = self.sct.monitors  # index 0 = all, 1+ = individual
            idx = self.monitor_id + 1  # mss: 1-based
            if idx >= len(monitors): idx = 1
            m = monitors[idx]
            self.monitor       = m
            self.monitor_left  = m["left"]
            self.monitor_top   = m["top"]
            self.original_width  = m["width"]
            self.original_height = m["height"]
            self.record_width  = int(self.original_width  * self.scale_factor)
            self.record_height = int(self.original_height * self.scale_factor)
            # keep even
            self.record_width  -= self.record_width  % 2
            self.record_height -= self.record_height % 2
        except Exception as e:
            log.warning(f"update_monitor_info: {e}")
            self.monitor_left = self.monitor_top = 0
            self.original_width = self.record_width  = 1920
            self.original_height= self.record_height = 1080

    # -- Theme / icon ----------------------------------------------------------
    def apply_theme(self):
        self.root.configure(bg=self.colors["bg"])
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("TFrame",    background=self.colors["bg"])
        style.configure("TLabel",    background=self.colors["bg"],
                                     foreground=self.colors["fg"])
        style.configure("TNotebook", background=self.colors["bg"])
        style.configure("TNotebook.Tab", background=self.colors["surface"],
                         foreground=self.colors["text"])
        style.configure("TCombobox", fieldbackground=self.colors["surface"],
                         foreground=self.colors["fg"])

    def set_app_icon(self):
        base = os.path.dirname(sys.executable if getattr(sys,"frozen",False)
                               else os.path.abspath(__file__))
        icons = os.path.join(base, "icons")
        self._main_ico = os.path.join(icons, "main.ico")
        self._rec_ico  = os.path.join(icons, "rec.ico")
        try:
            self.root.iconbitmap(self._main_ico)
        except Exception:
            pass
        if sys.platform == "win32":
            try:
                ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(
                    "homrec.144.legacy")
            except Exception:
                pass

    def _set_icon(self, win):
        try:
            if os.path.exists(self._main_ico):
                win.iconbitmap(self._main_ico)
        except Exception:
            pass

    def _set_taskbar_icon(self, recording: bool):
        try:
            ico = self._rec_ico if recording else self._main_ico
            if os.path.exists(ico):
                self.root.iconbitmap(ico)
        except Exception:
            pass

    def _make_rec_frames(self):
        frames = []
        for bright in (True, False):
            w, h = 72, 28
            img = Image.new("RGBA", (w, h), (0,0,0,0))
            d = ImageDraw.Draw(img)
            d.rounded_rectangle([0,0,w-1,h-1], radius=10, fill=(20,20,30,195))
            dot = (232,66,86,255) if bright else (160,40,55,200)
            d.ellipse([8,8,20,20], fill=dot)
            try:
                from PIL import ImageFont
                font = ImageFont.truetype("segoeui.ttf", 13)
            except Exception:
                try:
                    from PIL import ImageFont
                    font = ImageFont.truetype("arial.ttf", 13)
                except Exception:
                    from PIL import ImageFont
                    font = ImageFont.load_default()
            d.text((26,6), "REC", font=font, fill=(220,220,230,255))
            frames.append(img)
        return frames

    # -- Menu ------------------------------------------------------------------
    def create_menu(self):
        c = self.colors
        mb = tk.Menu(self.root, bg=c["surface"], fg=c["fg"])
        self.root.config(menu=mb)

        fm = tk.Menu(mb, tearoff=0, bg=c["surface"], fg=c["fg"])
        mb.add_cascade(label=self.lang["file_menu"], menu=fm)
        fm.add_command(label=self.lang["open_recordings"], command=self.open_recordings)
        fm.add_separator()
        fm.add_command(label=self.lang["exit"], command=self.quit_app)

        vm = tk.Menu(mb, tearoff=0, bg=c["surface"], fg=c["fg"])
        mb.add_cascade(label=self.lang["view_menu"], menu=vm)
        vm.add_checkbutton(label=self.lang["always_on_top"],
                           variable=self.always_on_top,
                           command=self.toggle_always_on_top)
        vm.add_command(label=self.lang["fullscreen"], command=self.toggle_fullscreen)
        if HAS_PSUTIL:
            vm.add_separator()
            vm.add_command(label=self.lang["pc_analytics"], command=self.show_analytics)

        # Theme sub-menu
        tm = tk.Menu(vm, tearoff=0, bg=c["surface"], fg=c["fg"])
        vm.add_cascade(label=self.lang["theme"], menu=tm)
        for tid, tlbl in [("dark","Dark"),("light","Light"),
                           ("catppuccin","Catppuccin"),("nord","Nord"),
                           ("dracula","Dracula")]:
            tm.add_radiobutton(label=tlbl, variable=self.theme_var, value=tid,
                               command=lambda t=tid: self.change_theme(t))

        sm = tk.Menu(mb, tearoff=0, bg=c["surface"], fg=c["fg"])
        mb.add_cascade(label=self.lang["settings_menu"], menu=sm)
        sm.add_command(label=self.lang["preferences"], command=self.open_settings)
        sm.add_separator()
        pm = tk.Menu(sm, tearoff=0, bg=c["surface"], fg=c["fg"])
        sm.add_cascade(label=self.lang["performance_menu"], menu=pm)
        for mode, lbl in [("ultra",self.lang["ultra"]),("turbo",self.lang["turbo"]),
                           ("balanced",self.lang["balanced"]),("eco",self.lang["eco"])]:
            pm.add_command(label=lbl, command=lambda m=mode: self.set_mode(m))
        cap_m = tk.Menu(sm, tearoff=0, bg=c["surface"], fg=c["fg"])
        sm.add_cascade(label=self.lang["capture_source"], menu=cap_m)
        cap_m.add_command(label=self.lang["full_desktop"], command=self.set_capture_desktop)
        cap_m.add_command(label=self.lang["select_window"], command=self.open_window_picker)

        hm = tk.Menu(mb, tearoff=0, bg=c["surface"], fg=c["fg"])
        mb.add_cascade(label=self.lang["help_menu"], menu=hm)
        hm.add_command(label=self.lang["check_updates"], command=self._manual_update_check)
        hm.add_separator()
        hm.add_command(label=self.lang["report_issue"], command=self._open_issues)

    # -- Widgets ---------------------------------------------------------------
    def create_widgets(self):
        c = self.colors
        # Main layout: left panel | preview
        main = tk.Frame(self.root, bg=c["bg"]); main.pack(fill="both", expand=True)
        left = tk.Frame(main, bg=c["bg"], width=260)
        left.pack(side="left", fill="y", padx=(8,0), pady=8)
        left.pack_propagate(False)

        # Preview
        prev_frame = tk.Frame(main, bg=c["preview_bg"])
        prev_frame.pack(side="left", fill="both", expand=True, padx=8, pady=8)
        tk.Label(prev_frame, text=self.lang["live_preview"],
                 bg=c["preview_bg"], fg=c["text_secondary"],
                 font=("Segoe UI",8)).pack()
        self.preview_label = tk.Label(prev_frame, bg=c["preview_bg"])
        self.preview_label.pack(fill="both", expand=True)

        # Status bar
        sb = tk.Frame(left, bg=c["surface"]); sb.pack(fill="x", pady=(0,6))
        self.status_icon  = tk.Label(sb, text="●", fg=c["warning"],
                                     bg=c["surface"], font=("Segoe UI",14))
        self.status_icon.pack(side="left", padx=6)
        self.status_label = tk.Label(sb, text=self.lang["ready"],
                                     bg=c["surface"], fg=c["text"],
                                     font=("Segoe UI",10,"bold"))
        self.status_label.pack(side="left")

        # Stats
        stats = tk.Frame(left, bg=c["surface"]); stats.pack(fill="x", pady=(0,6))
        self.time_label = tk.Label(stats, text="00:00:00",
                                   bg=c["surface"], fg=c["accent"],
                                   font=("Consolas",13,"bold"))
        self.time_label.pack(anchor="center", pady=2)
        self.fps_label  = tk.Label(stats, text=f"{self.lang['fps']} --",
                                   bg=c["surface"], fg=c["text_secondary"],
                                   font=("Segoe UI",9))
        self.fps_label.pack(anchor="center")
        self.res_label  = tk.Label(stats,
                                   text=f"{self.lang['resolution']} {self.record_width}x{self.record_height}",
                                   bg=c["surface"], fg=c["text_secondary"],
                                   font=("Segoe UI",9))
        self.res_label.pack(anchor="center", pady=(0,4))
        self.file_label = tk.Label(stats, text="",
                                   bg=c["surface"], fg=c["text_secondary"],
                                   font=("Segoe UI",8), wraplength=230)
        self.file_label.pack(anchor="center", pady=(0,4))

        # FFmpeg status
        ff_color = c["success"] if self.ffmpeg_path else c["error"]
        ff_text  = self.lang["ffmpeg_found"] if self.ffmpeg_path else self.lang["ffmpeg_not_found"]
        tk.Label(left, text=ff_text, bg=c["bg"], fg=ff_color,
                 font=("Segoe UI",9)).pack(anchor="w", pady=2)

        # Record / Pause / Stop buttons
        btn_frame = tk.Frame(left, bg=c["bg"]); btn_frame.pack(fill="x", pady=4)
        self.record_btn = tk.Button(btn_frame,
                                    text=self.lang["start"],
                                    command=self.start_with_countdown,
                                    bg=c["success"], fg=c["bg"],
                                    font=("Segoe UI",11,"bold"),
                                    relief="flat", pady=8)
        self.record_btn.pack(fill="x", pady=2)
        bf2 = tk.Frame(btn_frame, bg=c["bg"]); bf2.pack(fill="x")
        self.pause_btn = tk.Button(bf2, text=self.lang["pause"],
                                   command=self.toggle_pause,
                                   bg=c["warning"], fg=c["bg"],
                                   font=("Segoe UI",9,"bold"),
                                   relief="flat", pady=6, state="disabled")
        self.pause_btn.pack(side="left", fill="x", expand=True, padx=(0,2))

        # Audio mixer
        audio_lf = tk.LabelFrame(left, text=self.lang["audio_mixer"],
                                  bg=c["bg"], fg=c["accent"],
                                  font=("Segoe UI",9,"bold"))
        audio_lf.pack(fill="x", pady=4)

        # Enable audio checkbox
        tk.Checkbutton(audio_lf, text=self.lang["enable_audio"],
                       variable=self.audio_enabled,
                       bg=c["bg"], fg=c["text"],
                       selectcolor=c["surface"],
                       font=("Segoe UI",9)).pack(anchor="w", padx=6)

        # Mic
        self._build_channel(audio_lf, self.lang["microphone"], "mic")
        # System audio
        self._build_channel(audio_lf, self.lang["desktop_audio"], "sys")

        # Made by
        tk.Label(left, text=f"by {self.lang['made_by']}",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI",8)).pack(side="bottom", pady=2)

        # Start level meter polling
        self._poll_audio_levels()

    def _build_channel(self, parent, label_text, kind):
        c = self.colors
        f = tk.Frame(parent, bg=c["bg"]); f.pack(fill="x", padx=6, pady=2)
        tk.Label(f, text=label_text, bg=c["bg"], fg=c["text"],
                 font=("Segoe UI",9,"bold"), width=14, anchor="w").pack(side="left")
        mute_btn = tk.Button(f, text=self.lang["mute"],
                             bg=c["surface"], fg=c["text"],
                             font=("Segoe UI",8), relief="flat", padx=4,
                             command=lambda: self._toggle_mute(kind, mute_btn))
        mute_btn.pack(side="right")
        vol_var = self.mic_vol_var if kind=="mic" else self.sys_vol_var
        vol_scale = tk.Scale(parent, variable=vol_var,
                             from_=0, to=1, resolution=0.01,
                             orient="horizontal", length=220,
                             bg=c["bg"], fg=c["text"], highlightthickness=0,
                             troughcolor=c["surface"],
                             showvalue=False)
        vol_scale.pack(fill="x", padx=6)
        meter = AudioLevelMeter(parent, bg=c["surface_light"])
        meter.pack(fill="x", padx=6, pady=(0,4))
        if kind == "mic":
            self.mic_meter = meter
        else:
            self.sys_meter = meter

    def _toggle_mute(self, kind, btn):
        if kind == "mic":
            self.mic_muted = not self.mic_muted
            btn.config(text=self.lang["unmute"] if self.mic_muted else self.lang["mute"])
        else:
            self.sys_muted = not self.sys_muted
            btn.config(text=self.lang["unmute"] if self.sys_muted else self.lang["mute"])

    def _poll_audio_levels(self):
        """Update audio level meters from C++ engine or fallback."""
        if ENGINE_OK and self.audio_recording:
            ml = 0.0 if self.mic_muted else _eng.audio_mic_level()
            sl = 0.0 if self.sys_muted else _eng.audio_sys_level()
            self.mic_meter.set_level(ml)
            self.sys_meter.set_level(sl)
        self.root.after(100, self._poll_audio_levels)

    # -- Preview ---------------------------------------------------------------
    def _start_preview(self):
        self._preview_running = True
        if ENGINE_OK:
            _eng.preview_start(
                self.monitor_left, self.monitor_top,
                self.original_width, self.original_height,
                self.preview_width, self.preview_height,
                fps_limit=10
            )
            log.info("C++ preview started")
        else:
            # Python fallback preview
            t = threading.Thread(target=self._py_preview_loop, daemon=True)
            t.start()

    def _py_preview_loop(self):
        """Fallback preview loop using mss (Python) - used if DLL missing."""
        import mss as _mss_loc
        sct = _mss_loc.mss()
        while self._preview_running:
            try:
                mon = getattr(self, "monitor", None)
                pw  = self.preview_width
                ph  = self.preview_height
                if mon is None:
                    time.sleep(0.1); continue
                shot = sct.grab(mon)
                img  = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
                img.thumbnail((pw, ph), Image.Resampling.NEAREST
                              if self.recording else Image.Resampling.BILINEAR)
                if self.recording and not self.paused and self._rec_frames:
                    badge = self._rec_frames[self._rec_frame_idx % 2]
                    self._rec_frame_idx += 1
                    img.paste(badge, (10,10), badge)
                try:
                    self._preview_queue.get_nowait()
                except queue.Empty:
                    pass
                self._preview_queue.put_nowait(img)
            except Exception as e:
                log.debug(f"py_preview_loop: {e}")
            time.sleep(0.2 if self.recording else 0.12)

    def update_preview(self):
        try:
            if ENGINE_OK:
                buf = _eng.preview_get_frame()
                if buf:
                    raw, pw, ph = buf
                    img = Image.frombytes("RGB", (pw, ph), raw)
                    if self.recording and not self.paused and self._rec_frames:
                        badge = self._rec_frames[self._rec_frame_idx % 2]
                        self._rec_frame_idx += 1
                        img.paste(badge, (10,10), badge)
                    photo = ImageTk.PhotoImage(img)
                    self.preview_label.config(image=photo)
                    self.preview_label.image = photo
            else:
                img = self._preview_queue.get_nowait()
                photo = ImageTk.PhotoImage(img)
                self.preview_label.config(image=photo)
                self.preview_label.image = photo
        except queue.Empty:
            pass
        except Exception:
            pass
        self.root.after(100, self.update_preview)

    # -- Recording -------------------------------------------------------------
    def start_with_countdown(self):
        if not self.recording:
            if self.countdown_var.get(): self.show_countdown()
            else:                        self.start_recording()
        else:
            self.stop_recording()

    def show_countdown(self):
        w = tk.Toplevel(self.root)
        self._set_icon(w)
        w.geometry("320x160")
        w.configure(bg=self.colors["bg"])
        w.overrideredirect(True)
        w.update_idletasks()
        w.geometry(f"+{w.winfo_screenwidth()//2-160}+{w.winfo_screenheight()//2-80}")
        lbl = tk.Label(w, text="3", font=("Segoe UI",56,"bold"),
                       bg=self.colors["bg"], fg=self.colors["success"])
        lbl.pack(expand=True)
        def tick(n):
            if n > 0:
                lbl.config(text=str(n))
                w.after(1000, lambda: tick(n-1))
            else:
                lbl.config(text=self.lang["recording_btn"], fg=self.colors["error"])
                w.after(500, w.destroy)
                self.start_recording()
        tick(3)

    def _build_codec_args(self) -> list[str]:
        codec = getattr(self, "video_codec", "libx264")
        preset= getattr(self, "enc_preset",  "ultrafast")
        crf   = getattr(self, "enc_crf",     18)
        if codec in ("h264_nvenc","hevc_nvenc","h264_amf","hevc_amf","h264_qsv","hevc_qsv"):
            return ["-c:v", codec, "-preset", "fast", "-rc", "vbr", "-cq", str(crf)]
        return ["-c:v", codec, "-preset", preset, "-crf", str(crf)]

    def start_recording(self):
        try:
            ts = datetime.now().strftime("%Y%m%d_%H%M%S")
            os.makedirs(self.output_folder, exist_ok=True)
            self.filename  = os.path.join(self.output_folder, f"HomRec_{ts}.mp4")
            self.stop_flag = False
            self.paused    = False
            self.frame_count = 0

            if not self.ffmpeg_path:
                raise RuntimeError("FFmpeg not found. Place ffmpeg.exe next to homrec.py.")

            log.info(f"START recording → {self.filename}")

            if ENGINE_OK:
                # Use C++ engine to launch FFmpeg
                ok = _eng.record_start(
                    self.ffmpeg_path, self.filename,
                    self.monitor_left, self.monitor_top,
                    self.original_width, self.original_height,
                    self.target_fps,
                    getattr(self,"video_codec","libx264"),
                    getattr(self,"enc_preset","ultrafast"),
                    getattr(self,"enc_crf",18),
                    "yuv420p",
                    getattr(self,"hw_accel","auto"),
                    self.capture_mode == "window",
                    self.capture_window_title,
                )
                if not ok:
                    raise RuntimeError("C++ engine failed to start recording.")
            else:
                # Python fallback: spawn ffmpeg directly
                vf = (f"scale={self.record_width}:{self.record_height}"
                      if self.scale_factor != 1.0 else "null")
                cmd = [
                    self.ffmpeg_path, "-y",
                    "-f", "gdigrab", "-framerate", str(self.target_fps),
                    "-offset_x", str(self.monitor_left),
                    "-offset_y", str(self.monitor_top),
                    "-video_size", f"{self.original_width}x{self.original_height}",
                    "-i", "desktop",
                    "-vf", vf, "-r", str(self.target_fps),
                    *self._build_codec_args(),
                    "-pix_fmt", "yuv420p",
                    "-movflags", "+faststart", "-an",
                    self.filename,
                ]
                self.ffmpeg_proc = subprocess.Popen(
                    cmd,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                    stdin=subprocess.PIPE,
                    creationflags=subprocess.CREATE_NO_WINDOW if sys.platform=="win32" else 0
                )
                self.stop_flag = False
                self.ffmpeg_reader_thread = threading.Thread(
                    target=self._ffmpeg_stderr_reader, daemon=True)
                self.ffmpeg_reader_thread.start()

            # Audio
            if self.audio_enabled.get():
                self._start_audio_capture(ts)

            self.recording  = True
            self.start_time = time.time()
            self._set_taskbar_icon(recording=True)
            self.record_btn.config(text=self.lang["stop"],
                                   bg=self.colors["error"],
                                   command=self.stop_recording)
            self.pause_btn.config(state="normal")
            self.stop_btn.config(state="normal")
            self.status_icon.config(fg=self.colors["success"])
            self.status_label.config(text=self.lang["recording"])
            self._update_stats()

            if self.notify_flash:
                self._flash_border()

        except Exception as e:
            log.exception("start_recording failed")
            messagebox.showerror(self.lang["error"],
                                 f"Failed to start recording:\n{e}")

    def _ffmpeg_stderr_reader(self):
        while not self.stop_flag and self.ffmpeg_proc and self.ffmpeg_proc.poll() is None:
            try:
                line = self.ffmpeg_proc.stderr.readline()
                if not line: break
                line = line.decode("utf-8", errors="ignore")
                if "frame=" in line:
                    try:
                        parts = line.split()
                        for i, p in enumerate(parts):
                            if p == "frame=":
                                self.frame_count = int(parts[i+1])
                                break
                    except Exception:
                        pass
            except Exception:
                break

    def _flash_border(self):
        colors = [self.colors["success"], self.colors["bg"]] * 3
        def _flash(idx=0):
            if idx < len(colors):
                self.root.configure(bg=colors[idx])
                self.root.after(120, lambda: _flash(idx+1))
        _flash()

    def _start_audio_capture(self, ts: str):
        mic_path = os.path.join(self.output_folder, f"HomRec_{ts}_mic.wav")
        sys_path = os.path.join(self.output_folder, f"HomRec_{ts}_sys.wav")
        self._audio_mic_path = mic_path
        self._audio_sys_path = sys_path
        mic_vol = 0.0 if self.mic_muted else float(self.mic_vol_var.get())
        sys_vol = 0.0 if self.sys_muted else float(self.sys_vol_var.get())
        if ENGINE_OK:
            _eng.audio_start(mic_path, sys_path,
                             self.audio_sample_rate, 2,
                             mic_vol, sys_vol)
            self.audio_recording = True
        elif HAS_PYAUDIO:
            # fallback Python mic capture
            self.audio_recording = True
            self._py_audio_frames: list = []
            self._py_audio_stop   = threading.Event()
            threading.Thread(target=self._py_mic_capture,
                             args=(mic_path, mic_vol),
                             daemon=True).start()
        else:
            self.audio_recording = False
            log.warning("No audio capture available.")

    def _py_mic_capture(self, path: str, vol: float):
        """Python fallback mic capture via PyAudio."""
        pa = pyaudio.PyAudio()
        sr = self.audio_sample_rate
        frames: list = []
        try:
            stream = pa.open(format=pyaudio.paInt16, channels=1,
                             rate=sr, input=True, frames_per_buffer=1024)
            while not self._py_audio_stop.is_set():
                data = stream.read(1024, exception_on_overflow=False)
                if vol != 1.0:
                    arr = audioop.mul(data, 2, vol)
                    data = arr
                rms = audioop.rms(data, 2)
                self.mic_meter.set_level(min(100, rms / 327))
                frames.append(data)
            stream.stop_stream(); stream.close()
        except Exception as e:
            log.warning(f"py_mic_capture: {e}")
        pa.terminate()
        try:
            wf = wave.open(path, "wb")
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(sr)
            wf.writeframes(b"".join(frames))
            wf.close()
        except Exception as e:
            log.warning(f"py_mic_capture WAV write: {e}")

    def stop_recording(self):
        if not self.recording: return
        log.info("Stopping recording…")
        self.recording  = False
        self.stop_flag  = True
        saved_filename  = self.filename
        saved_start     = self.start_time

        self._set_taskbar_icon(recording=False)
        self.record_btn.config(text=self.lang["start"],
                               bg=self.colors["success"],
                               command=self.start_with_countdown)
        self.pause_btn.config(state="disabled", text=self.lang["pause"])
        self.stop_btn.config(state="disabled")
        self.status_icon.config(fg=self.colors["warning"])
        self.status_label.config(text="Saving…")
        self.time_label.config(text="00:00:00")
        self.file_label.config(text="Processing…")

        def _finalize():
            # Stop C++ engine recording
            if ENGINE_OK:
                _eng.record_stop()
            elif self.ffmpeg_proc and self.ffmpeg_proc.poll() is None:
                try:
                    self.ffmpeg_proc.stdin.write(b"q")
                    self.ffmpeg_proc.stdin.flush()
                    self.ffmpeg_proc.wait(timeout=10)
                except Exception:
                    try: self.ffmpeg_proc.kill()
                    except Exception: pass

            # Stop audio
            audio_mic = None; audio_sys = None
            if self.audio_recording:
                if ENGINE_OK:
                    _eng.audio_stop()
                    audio_mic = getattr(self,"_audio_mic_path","")
                    audio_sys = getattr(self,"_audio_sys_path","")
                else:
                    if hasattr(self,"_py_audio_stop"):
                        self._py_audio_stop.set()
                    time.sleep(0.4)
                    audio_mic = getattr(self,"_audio_mic_path","")
                self.audio_recording = False

            time.sleep(0.3)

            # Merge audio
            audio_merged = False
            if audio_mic and os.path.exists(audio_mic) and self.ffmpeg_path:
                audio_merged = self._merge_audio(saved_filename, audio_mic, audio_sys)

            duration = time.time() - saved_start
            size_mb  = (os.path.getsize(saved_filename)/1024/1024
                        if os.path.exists(saved_filename) else 0)

            self.root.after(0, lambda: self._finalize_ui(
                saved_filename, duration, size_mb,
                audio_mic, audio_merged))

        threading.Thread(target=_finalize, daemon=True).start()

    def _merge_audio(self, video: str, mic: str, sys_wav: str | None) -> bool:
        """Merge WAV audio(s) into the MP4 video using FFmpeg."""
        try:
            merged = video.replace(".mp4","_merged.mp4")
            inputs = ["-i", video, "-i", mic]
            if sys_wav and os.path.exists(sys_wav):
                inputs += ["-i", sys_wav]
                filter_cx = "[1:a][2:a]amix=inputs=2:duration=first[aout]"
                amap      = ["-filter_complex", filter_cx, "-map","0:v","-map","[aout]"]
            else:
                amap = ["-map","0:v","-map","1:a"]
            cmd = [self.ffmpeg_path, "-y",
                   *inputs, *amap,
                   "-c:v","copy","-c:a","aac",
                   "-b:a", getattr(self,"audio_aac_bitrate","128k"),
                   "-movflags","+faststart", merged]
            ret = subprocess.run(cmd,
                                 stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL,
                                 timeout=60,
                                 creationflags=subprocess.CREATE_NO_WINDOW
                                 if sys.platform=="win32" else 0)
            if ret.returncode == 0 and os.path.exists(merged):
                os.replace(merged, video)
                # Clean up WAV files
                for w in (mic, sys_wav):
                    if w and os.path.exists(w):
                        try: os.remove(w)
                        except Exception: pass
                log.info("Audio merged successfully.")
                return True
        except Exception as e:
            log.warning(f"_merge_audio: {e}")
        return False

    def _finalize_ui(self, filename, duration, size_mb, audio_file, audio_merged):
        exists = os.path.exists(filename)
        self.status_icon.config(fg=self.colors["warning"])
        self.status_label.config(text=self.lang["ready"])
        if exists:
            self.file_label.config(
                text=self.lang["saved"].format(size=size_mb, duration=duration))
            if self.show_summary:
                audio_text = (self.lang["merged"] if audio_merged
                              else self.lang["separate"] if audio_file
                              else self.lang["no_audio"])
                info = (f"{self.lang['file']} {os.path.basename(filename)}\n"
                        f"{self.lang['size']} {size_mb:.1f} MB\n"
                        f"{self.lang['duration']} {duration:.1f}s\n"
                        f"{self.lang['audio']} {audio_text}")
                if messagebox.askyesno(self.lang["recording_saved"], info + "\n\n" + self.lang["open_folder"]):
                    self.open_recordings()
        else:
            self.file_label.config(text=self.lang["recording_failed"])
            messagebox.showerror(self.lang["error"], self.lang["recording_failed"])

    def _update_stats(self):
        if not self.recording: return
        try:
            elapsed = time.time() - self.start_time
            h = int(elapsed//3600); m = int((elapsed%3600)//60); s = int(elapsed%60)
            self.time_label.config(text=f"{h:02d}:{m:02d}:{s:02d}")
            fc = _eng.record_frame_count() if ENGINE_OK else self.frame_count
            if elapsed > 0 and fc > 0:
                self.fps_label.config(text=f"{self.lang['fps']} {fc/elapsed:.1f}")
            self.file_label.config(
                text=self.lang["recording_status"].format(frames=fc))
        except Exception:
            pass
        self.root.after(500, self._update_stats)

    def toggle_pause(self):
        if not self.recording: return
        self.paused = not self.paused
        if self.paused:
            self.pause_btn.config(text=self.lang["resume"], bg=self.colors["success"])
            self.status_label.config(text=self.lang["paused"])
        else:
            self.pause_btn.config(text=self.lang["pause"], bg=self.colors["warning"])
            self.status_label.config(text=self.lang["recording"])

    # -- Settings / mode -------------------------------------------------------
    def open_settings(self): SettingsDialog(self.root, self)

    def set_mode(self, mode: str):
        self.recording_mode = mode
        self.update_mode_settings()
        self.save_settings(silent=True)
        self.res_label.config(
            text=f"{self.lang['resolution']} {self.record_width}x{self.record_height}")

    def update_mode_settings(self):
        if   self.recording_mode == "ultra":
            self.target_fps=60; self.quality=95; self.scale_factor=1.0
        elif self.recording_mode == "turbo":
            self.target_fps=30; self.quality=90; self.scale_factor=1.0
        elif self.recording_mode == "balanced":
            self.target_fps=15; self.quality=70; self.scale_factor=0.75
        else:  # eco
            self.target_fps=8;  self.quality=50; self.scale_factor=0.5
        self.update_monitor_info()

    def load_settings(self):
        try:
            if not os.path.exists(SETTINGS_PATH): return
            with open(SETTINGS_PATH, "r") as f:
                s = json.load(f)
            self.output_folder    = s.get("output_folder",    "recordings")
            self.recording_mode   = s.get("mode",             "balanced")
            self.current_theme    = s.get("theme",            "dark")
            self.current_language = s.get("language",         "en")
            self.monitor_id       = s.get("monitor_id",       0)
            self.video_codec      = s.get("video_codec",      "libx264")
            self.enc_preset       = s.get("enc_preset",       "ultrafast")
            self.enc_crf          = s.get("enc_crf",          18)
            self.hw_accel         = s.get("hw_accel",         "auto")
            self.audio_sample_rate= s.get("audio_sample_rate",44100)
            self.audio_aac_bitrate= s.get("audio_aac_bitrate","128k")
            self.auto_stop_min    = s.get("auto_stop_min",    0)
            self.hotkey_start_stop= s.get("hotkey_start_stop","F9")
            self.hotkey_pause     = s.get("hotkey_pause",     "F10")
            self.notify_sound     = s.get("notify_sound",     True)
            self.notify_flash     = s.get("notify_flash",     True)
            self.show_summary     = s.get("show_summary",     True)
            self.always_on_top.set(s.get("always_on_top",    False))
            self.countdown_var.set(s.get("countdown",        True))
            self.timestamp_var.set(s.get("timestamp",        False))
            self.cursor_var.set(   s.get("cursor",           False))
            self.minimize_to_tray.set(s.get("minimize_to_tray", True))
            if self.always_on_top.get():
                self.root.attributes("-topmost", True)
            self.update_mode_settings()
        except Exception as e:
            log.warning(f"load_settings: {e}")

    def save_settings(self, silent=False):
        try:
            s = {
                "output_folder":    self.output_folder,
                "mode":             self.recording_mode,
                "theme":            self.current_theme,
                "language":         self.current_language,
                "monitor_id":       self.monitor_id,
                "video_codec":      getattr(self,"video_codec","libx264"),
                "enc_preset":       getattr(self,"enc_preset","ultrafast"),
                "enc_crf":          getattr(self,"enc_crf",18),
                "hw_accel":         getattr(self,"hw_accel","auto"),
                "audio_sample_rate":getattr(self,"audio_sample_rate",44100),
                "audio_aac_bitrate":getattr(self,"audio_aac_bitrate","128k"),
                "auto_stop_min":    getattr(self,"auto_stop_min",0),
                "hotkey_start_stop":getattr(self,"hotkey_start_stop","F9"),
                "hotkey_pause":     getattr(self,"hotkey_pause","F10"),
                "notify_sound":     getattr(self,"notify_sound",True),
                "notify_flash":     getattr(self,"notify_flash",True),
                "show_summary":     self.show_summary,
                "always_on_top":    self.always_on_top.get(),
                "countdown":        self.countdown_var.get(),
                "timestamp":        self.timestamp_var.get(),
                "cursor":           self.cursor_var.get(),
                "minimize_to_tray": self.minimize_to_tray.get(),
            }
            with open(SETTINGS_PATH, "w") as f:
                json.dump(s, f, indent=2)
        except Exception as e:
            log.warning(f"save_settings: {e}")

    def recreate_widgets(self):
        was_rec = self.recording; was_paused = self.paused
        for w in self.root.winfo_children():
            w.destroy()
        self.create_menu(); self.create_widgets()
        if was_rec:
            self.record_btn.config(text=self.lang["stop"],
                                   bg=self.colors["error"],
                                   command=self.stop_recording)
            self.pause_btn.config(state="normal")
            if was_paused:
                self.pause_btn.config(text=self.lang["resume"],
                                      bg=self.colors["success"])

    def change_theme(self, theme: str):
        self.current_theme = theme
        self.theme_var.set(theme)
        self.colors = BUILTIN_THEMES.get(theme, BUILTIN_THEMES["dark"])
        self.apply_theme(); self.recreate_widgets()
        self.save_settings(silent=True)

    # -- Misc UI ---------------------------------------------------------------
    def toggle_always_on_top(self):
        self.root.attributes("-topmost", self.always_on_top.get())
        self.save_settings(silent=True)

    def toggle_fullscreen(self):
        fs = self.root.attributes("-fullscreen")
        self.root.attributes("-fullscreen", not fs)

    def on_window_resize(self, event):
        if event.widget == self.root:
            pw = max(400, self.root.winfo_width() - 270)
            ph = max(280, self.root.winfo_height() - 80)
            self.preview_width  = min(pw, 1280)
            self.preview_height = min(ph,  720)

    def open_recordings(self):
        if os.path.exists(self.output_folder):
            if sys.platform == "win32":
                os.startfile(self.output_folder)
            else:
                subprocess.Popen(["xdg-open", self.output_folder])
        else:
            messagebox.showwarning(self.lang["warning"],
                                   self.lang["folder_not_exist"])

    # -- PC Analytics ---------------------------------------------------------
    def show_analytics(self):
        if not HAS_PSUTIL:
            messagebox.showinfo("PC Analytics", "psutil not installed.")
            return
        dlg = tk.Toplevel(self.root)
        self._set_icon(dlg)
        dlg.title("PC Analytics"); dlg.geometry("320x360")
        dlg.configure(bg=self.colors["bg"]); dlg.resizable(False,True)
        c = self.colors
        def refresh():
            for w in dlg.winfo_children(): w.destroy()
            tk.Label(dlg, text="PC Analytics", bg=c["bg"], fg=c["accent"],
                     font=("Segoe UI",12,"bold")).pack(pady=(10,4))
            for title, color, items in [
                ("CPU", c["accent"], [
                    ("Cores", str(psutil.cpu_count())),
                    ("Usage", f"{psutil.cpu_percent(interval=0.2):.1f}%"),
                ]),
                ("RAM", c["success"], [
                    ("Total",  f"{psutil.virtual_memory().total/1e9:.1f} GB"),
                    ("Free",   f"{psutil.virtual_memory().available/1e9:.1f} GB"),
                    ("Used",   f"{psutil.virtual_memory().percent}%"),
                ]),
            ]:
                f = tk.Frame(dlg, bg=c["surface"], pady=6, padx=10)
                f.pack(fill="x", padx=12, pady=4)
                tk.Label(f, text=title, bg=c["surface"], fg=color,
                         font=("Segoe UI",10,"bold")).pack(anchor="w")
                for lbl, val in items:
                    r = tk.Frame(f, bg=c["surface"]); r.pack(fill="x")
                    tk.Label(r, text=lbl, bg=c["surface"], fg=c["text_secondary"],
                             font=("Segoe UI",9), width=10, anchor="w").pack(side="left")
                    tk.Label(r, text=val, bg=c["surface"], fg=c["text"],
                             font=("Consolas",9)).pack(side="left")
            tk.Button(dlg, text="Refresh", command=refresh,
                      bg=c["surface"], fg=c["text"],
                      font=("Segoe UI",9), relief="flat",
                      padx=14, pady=4).pack(pady=8)
        refresh()

    # -- Window picker ---------------------------------------------------------
    def set_capture_desktop(self):
        self.capture_mode = "desktop"; self.capture_window_title = ""

    def get_open_windows(self) -> list[str]:
        if sys.platform != "win32": return []
        titles = []
        EnumWindows = ctypes.windll.user32.EnumWindows
        EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool,
                                              ctypes.POINTER(ctypes.c_int),
                                              ctypes.POINTER(ctypes.c_int))
        GetWindowText = ctypes.windll.user32.GetWindowTextW
        GetWindowTextLength = ctypes.windll.user32.GetWindowTextLengthW
        IsWindowVisible = ctypes.windll.user32.IsWindowVisible
        def cb(hwnd, _):
            if IsWindowVisible(hwnd):
                n = GetWindowTextLength(hwnd)
                if n > 0:
                    buf = ctypes.create_unicode_buffer(n+1)
                    GetWindowText(hwnd, buf, n+1)
                    t = buf.value.strip()
                    if t and t not in titles: titles.append(t)
            return True
        EnumWindows(EnumWindowsProc(cb), 0)
        return sorted(titles)

    def open_window_picker(self):
        windows = self.get_open_windows()
        if not windows:
            messagebox.showinfo("Info","No open windows found."); return
        c = self.colors
        dlg = tk.Toplevel(self.root); self._set_icon(dlg)
        dlg.title("Select Window"); dlg.geometry("440x340")
        dlg.configure(bg=c["bg"]); dlg.transient(self.root); dlg.grab_set()
        tk.Label(dlg, text="Select a window to record:",
                 bg=c["bg"], fg=c["text"],
                 font=("Segoe UI",10,"bold")).pack(anchor="w",padx=12,pady=(12,4))
        fr = tk.Frame(dlg,bg=c["bg"]); fr.pack(fill="both",expand=True,padx=12,pady=4)
        sb = tk.Scrollbar(fr); sb.pack(side="right",fill="y")
        lb = tk.Listbox(fr, yscrollcommand=sb.set,
                        bg=c["surface"], fg=c["text"],
                        selectbackground=c["accent"],
                        font=("Segoe UI",9), relief="flat",
                        activestyle="none", borderwidth=0)
        lb.pack(side="left",fill="both",expand=True); sb.config(command=lb.yview)
        for w in windows: lb.insert(tk.END, w)
        bf = tk.Frame(dlg,bg=c["bg"]); bf.pack(fill="x",padx=12,pady=10)
        def on_sel():
            s = lb.curselection()
            if s:
                self.capture_window_title = windows[s[0]]
                self.capture_mode = "window"
                dlg.destroy()
        tk.Button(bf, text="Record this window", command=on_sel,
                  bg=c["accent"], fg=c["bg"],
                  font=("Segoe UI",9,"bold"), relief="flat",
                  padx=12, pady=5).pack(side="left",padx=(0,6))
        tk.Button(bf, text="Full desktop",
                  command=lambda: (self.set_capture_desktop(), dlg.destroy()),
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI",9), relief="flat",
                  padx=12, pady=5).pack(side="left")

    # -- Tray ------------------------------------------------------------------
    def setup_tray(self):
        try:
            # Рисуем иконку 64x64: красный круг с белой буквой R
            img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
            d = ImageDraw.Draw(img)
            # Фон - тёмно-красный круг
            d.ellipse([2, 2, 62, 62], fill="#c0392b", outline="#e74c3c", width=2)
            # Белый круг внутри (имитация кнопки записи)
            d.ellipse([18, 18, 46, 46], fill="#ffffff")
            # Красная точка в центре
            d.ellipse([26, 26, 38, 38], fill="#c0392b")

            menu = pystray.Menu(
                TrayItem("Show HomRec", lambda: self.root.after(0, self.root.deiconify), default=True),
                TrayItem("Start/Stop",  lambda: self.root.after(0, self.toggle_recording)),
                pystray.Menu.SEPARATOR,
                TrayItem("Quit",        lambda: self.root.after(0, self.quit_app)),
            )
            self.tray_icon = pystray.Icon("HomRec", img, f"HomRec {APP_VERSION}", menu)
            threading.Thread(target=self.tray_icon.run, daemon=True).start()
            log.info("Tray icon started")
        except Exception as e:
            log.warning(f"Tray setup failed: {e}")
            self.tray_icon = None

    def toggle_recording(self):
        if not self.recording: self.start_recording()
        else:                  self.stop_recording()

    # -- Update check ---------------------------------------------------------
    def _bg_update_check(self):
        try:
            import urllib.request, json as _json
            url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
            req = urllib.request.Request(url, headers={"User-Agent":"HomRec"})
            with urllib.request.urlopen(req, timeout=5) as r:
                data = _json.loads(r.read().decode())
            tag = data.get("tag_name","").lstrip("v")
            if tag and _version_gt(tag, "1.4.4"):
                self.root.after(0, lambda: self._show_update_btn(tag))
        except Exception:
            pass

    def _show_update_btn(self, ver: str):
        try:
            btn = tk.Button(self.root,
                            text=f"⬇ v{ver} available",
                            bg="#a6e3a1", fg="#1e1e2e",
                            font=("Segoe UI",8,"bold"),
                            relief="flat", padx=8, pady=3,
                            cursor="hand2",
                            command=lambda: self._open_release(ver))
            btn.place(relx=1.0, rely=1.0, anchor="se", x=-10, y=-10)
        except Exception:
            pass

    def _open_release(self, ver: str):
        import webbrowser
        webbrowser.open(f"https://github.com/{GITHUB_REPO}/releases/tag/v{ver}")

    def _manual_update_check(self):
        def _fetch():
            try:
                import urllib.request, json as _json
                url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
                req = urllib.request.Request(url, headers={"User-Agent":"HomRec"})
                with urllib.request.urlopen(req, timeout=5) as r:
                    data = _json.loads(r.read().decode())
                tag = data.get("tag_name","").lstrip("v")
                if tag and _version_gt(tag,"1.4.4"):
                    self.root.after(0, lambda: messagebox.showinfo(
                        "Update available",
                        f"HomRec v{tag} is available!\nhttps://github.com/{GITHUB_REPO}/releases"))
                else:
                    self.root.after(0, lambda: messagebox.showinfo(
                        "No updates", f"You have {APP_VERSION} (latest)."))
            except Exception as e:
                self.root.after(0, lambda: messagebox.showerror("Error", str(e)))
        threading.Thread(target=_fetch, daemon=True).start()

    def _open_issues(self):
        import webbrowser
        webbrowser.open(f"https://github.com/{GITHUB_REPO}/issues")

    # -- Lifecycle -------------------------------------------------------------
    def on_closing(self):
        if HAS_TRAY and self.tray_icon and self.minimize_to_tray.get():
            self.root.withdraw()
        else:
            self.quit_app()

    def quit_app(self):
        if self.recording:
            if not messagebox.askyesno(self.lang["warning"],
                                       "Recording in progress! Stop and exit?"):
                return
            self.stop_recording(); time.sleep(0.6)
        self._preview_running = False
        if ENGINE_OK:
            _eng.preview_stop()
            _eng.record_stop()
            _eng.audio_stop()
        if self.tray_icon:
            try: self.tray_icon.stop()
            except Exception: pass
        self.root.destroy()

# ----------------------- Entry point -----------------------------------------
if __name__ == "__main__":
    if sys.platform == "win32":
        # Single instance mutex
        _mutex = ctypes.windll.kernel32.CreateMutexW(None, False, "HomRec_Legacy_144")
        if ctypes.windll.kernel32.GetLastError() == 183:
            sys.exit(0)

    root = tk.Tk()
    app  = HomRecApp(root)
    root.mainloop()
