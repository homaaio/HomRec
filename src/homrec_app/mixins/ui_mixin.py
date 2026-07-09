from __future__ import annotations

import tkinter as tk
from tkinter import ttk, messagebox, filedialog
from tkinter import colorchooser as cc
import time
import os
import sys
import re
import glob
import json
import gzip
import shutil
import platform
import webbrowser
import subprocess
import threading
import queue
import ctypes
import logging
from datetime import datetime
import cv2
import numpy as np
from PIL import Image, ImageTk, ImageDraw
import mss

from ..core.optional_deps import (_DND_AVAILABLE, _PYAUDIO_AVAILABLE, _pyaudio_mod,
                                   _audioop_mod, wave, HAS_PSUTIL, HAS_TRAY, pystray, TrayItem)
if HAS_PSUTIL:
    import psutil
if _DND_AVAILABLE:
    from tkinterdnd2 import DND_FILES
from ..core.constants import (CURRENT_VERSION, GITHUB_REPO, ASSETS_DIR, THEMES_DIR, SRC_DIR,
                               LANGS_DIR, SETTINGS_PATH, THEME_REQUIRED_KEYS,
                               LANG_REQUIRED_KEYS, LANG_SCHEMA_VERSION,
                               THEME_SCHEMA_VERSION, _HRC_MAGIC, _HRL_MAGIC, _ROOT_DIR)
from ..core.languages import LANGUAGES
from ..core.profile_io import _hrc_write, _hrc_read, _hrc_detect
from ..core.system_utils import find_ffmpeg, optimize_for_performance, rms_to_level_percent
from ..core.updates import check_for_updates, _version_gt

from ..dialogs.welcome_dialog import WelcomeDialog
from ..dialogs.settings_dialog import SettingsDialog
from ..dialogs.advanced_settings_dialog import AdvancedSettingsDialog
from ..dialogs.overlay_manager import OverlayManagerWindow, OverlayPreviewDialog
from ..dialogs.overlays_dock_panel import OverlaysDockPanel
from ..dialogs.audio_panel import AudioPanel
from ..dialogs.audio_level_meter import AudioLevelMeter
from ..dialogs.custom_messagebox import CustomMessageBox

log = logging.getLogger("homrec")


class UIMixin:

    BUILTIN_THEMES = {
        "dark": {"bg":"#1e1e2e","fg":"#cdd6f4","accent":"#89b4fa","success":"#a6e3a1","warning":"#f9e2af","error":"#f38ba8","surface":"#313244","surface_light":"#45475a","preview_bg":"#11111b","text":"#cdd6f4","text_secondary":"#a6adc8"},
        "light": {"bg":"#f5f5f5","fg":"#2c3e50","accent":"#3498db","success":"#27ae60","warning":"#f39c12","error":"#e74c3c","surface":"#ecf0f1","surface_light":"#bdc3c7","preview_bg":"#ffffff","text":"#2c3e50","text_secondary":"#7f8c8d"},
    }


    def update_ui_language(self) -> None:
        self.root.title(self.lang["app_title"]); self.recreate_widgets()

    def check_ffmpeg(self) -> bool:
        return self.ffmpeg_path is not None

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
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("homrec.1.7.1")
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
                self.custom_ffmpeg_args = s.get("custom_ffmpeg_args", "")
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
            "custom_ffmpeg_args": getattr(self, 'custom_ffmpeg_args', ''),
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
        tk.Label(title_frame, text="v1.7.1", font=("Segoe UI", 11), bg=self.colors["surface"], fg=self.colors["text_secondary"]).pack()

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

    def _register_file_types(self) -> None:
        if platform.system() != "Windows": return
        try:
            import winreg
            base = SRC_DIR
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
                exe_path = os.path.join(SRC_DIR, "homrec.py")
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
        self._sync_webcam_captures(set())  # release any open webcam devices
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

