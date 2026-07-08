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
from .overlay_manager import OverlayPreviewDialog

log = logging.getLogger("homrec")


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

        row_cfa = vt.grid_size()[1]
        tk.Frame(vt, bg=c["surface"], height=1).grid(row=row_cfa, column=0, columnspan=3, sticky="ew", padx=20, pady=(10, 4))
        row_cfa2 = vt.grid_size()[1]
        tk.Label(vt, text="Custom FFmpeg Args", bg=c["bg"], fg=c["accent"], font=("Segoe UI", 10, "bold"), anchor="w").grid(row=row_cfa2, column=0, columnspan=3, sticky="w", padx=(20, 8), pady=(0, 4))
        self._cfav = tk.StringVar(value=getattr(a, "custom_ffmpeg_args", ""))
        row_cfa3 = vt.grid_size()[1]
        tk.Entry(vt, textvariable=self._cfav, bg=c["surface"], fg=c["text"], font=("Consolas", 9),
                 relief="flat", width=44).grid(row=row_cfa3, column=0, columnspan=3, sticky="w", padx=20, pady=(0, 2))
        row_cfa4 = vt.grid_size()[1]
        tk.Label(vt, text="Appended to the encode command as-is (e.g. -profile:v main -x264-params ref=4).\nApplies to encoding AND audio/video merging. Invalid flags will break recording — test after changing.",
                 bg=c["bg"], fg=c.get("text_secondary", "#888"), font=("Segoe UI", 8), justify="left").grid(row=row_cfa4, column=0, columnspan=3, sticky="w", padx=(20, 4), pady=(0, 8))

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
            "custom_ffmpeg_args": self._cfav.get(),
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
            self._cfav.set(data.get("custom_ffmpeg_args", ""))
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
