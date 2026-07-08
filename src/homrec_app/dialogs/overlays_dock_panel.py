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
from .overlay_manager import OverlayManagerWindow, OverlayPreviewDialog

log = logging.getLogger("homrec")


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
