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
