"""ui/lang_editor.py — Language Editor dialog"""
import os, sys, json, gzip, time, wave, audioop, threading, platform
import subprocess, logging, re, shutil
import tkinter as tk
import tkinter.ttk as ttk
import tkinter.messagebox as messagebox
import tkinter.filedialog as filedialog
import tkinter.font

from core.constants import *
from core.languages import LANGUAGES
from core.hrc import _hrc_read, _hrc_write, _hrc_detect, _HRC_MAGIC, _HRL_MAGIC, _HRT_MAGIC

log = logging.getLogger("homrec.ui")

try:
    import pyaudio
    _PYAUDIO_OK = True
except ImportError:
    _PYAUDIO_OK = False

try:
    import psutil
    _PSUTIL_OK = True
except ImportError:
    _PSUTIL_OK = False

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


