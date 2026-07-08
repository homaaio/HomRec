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
import subprocess
import threading
from datetime import datetime
import cv2
import numpy as np
from PIL import Image, ImageTk, ImageDraw
import logging

from ..core.optional_deps import (_PYAUDIO_AVAILABLE, _pyaudio_mod, _audioop_mod,
                                   HAS_PSUTIL, HAS_TRAY)
from ..core.constants import (ASSETS_DIR, THEMES_DIR, LANGS_DIR, SETTINGS_PATH,
                               THEME_REQUIRED_KEYS, LANG_REQUIRED_KEYS,
                               LANG_SCHEMA_VERSION, THEME_SCHEMA_VERSION,
                               _HRC_MAGIC, _HRL_MAGIC)
from ..core.languages import LANGUAGES
from ..core.profile_io import _hrc_write, _hrc_read, _hrc_detect
from .advanced_settings_dialog import AdvancedSettingsDialog

log = logging.getLogger("homrec")


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
