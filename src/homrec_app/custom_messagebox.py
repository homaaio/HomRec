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

log = logging.getLogger("homrec")


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
