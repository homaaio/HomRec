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
import webbrowser
from datetime import datetime
import cv2
import numpy as np
from PIL import Image, ImageTk, ImageDraw
import logging

from ..core.optional_deps import (_PYAUDIO_AVAILABLE, _pyaudio_mod, _audioop_mod,
                                   HAS_PSUTIL, HAS_TRAY)
from ..core.constants import (CURRENT_VERSION, ASSETS_DIR, THEMES_DIR, LANGS_DIR, SETTINGS_PATH,
                               THEME_REQUIRED_KEYS, LANG_REQUIRED_KEYS,
                               LANG_SCHEMA_VERSION, THEME_SCHEMA_VERSION,
                               _HRC_MAGIC, _HRL_MAGIC)
from ..core.languages import LANGUAGES
from ..core.profile_io import _hrc_write, _hrc_read, _hrc_detect

log = logging.getLogger("homrec")


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
        tk.Label(tips_frame, text="F9 = Start/Stop   F10 = Pause   F11 = Fullscreen   Ctrl + Shift + T = Console",
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
