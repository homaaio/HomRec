"""ui/theme_editor.py — Theme Editor dialog"""
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

class ThemeEditorDialog:
    """Built-in theme editor with live color pickers and preview."""

    THEME_KEYS = [
        ("bg",             "Main background",     "Window background color"),
        ("surface",        "Surface / panels",    "Cards, inputs, panels"),
        ("accent",         "Accent",              "Buttons, highlights, active elements"),
        ("text",           "Text",                "Primary text color"),
        ("text_secondary", "Secondary text",      "Labels, hints, secondary info"),
        ("success",        "Success",             "Recording active, positive state"),
        ("warning",        "Warning",             "Alerts, cautions"),
        ("error",          "Error",               "Errors, stop button, mute"),
    ]

    def __init__(self, parent, app) -> None:
        self.app = app
        self.dialog = tk.Toplevel(parent)
        self.dialog.title("Theme Editor")
        self.dialog.geometry("520x480")
        self.dialog.resizable(False, True)
        self.dialog.configure(bg=app.colors["bg"])
        self.dialog.grab_set()
        self.dialog.after(50, self._set_icon)
        self._vars = {}
        self._swatches = {}
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

        # Title + load
        top = tk.Frame(self.dialog, bg=c["bg"])
        top.pack(fill="x", padx=14, pady=(12, 4))
        tk.Label(top, text="Theme Editor", bg=c["bg"], fg=c["accent"],
                 font=("Segoe UI", 13, "bold")).pack(side="left")

        btn_row = tk.Frame(self.dialog, bg=c["bg"])
        btn_row.pack(fill="x", padx=14, pady=(0, 6))
        for name, code in [("Dark", "dark"), ("Light", "light"),
                            ("Catppuccin", "catppuccin"), ("Nord", "nord"),
                            ("Dracula", "dracula")]:
            tk.Button(btn_row, text=name,
                      command=lambda n=code: self._load_builtin(n),
                      bg=c["surface"], fg=c["text"], font=("Segoe UI", 8),
                      relief="flat", padx=8, pady=3).pack(side="left", padx=2)
        tk.Button(btn_row, text="Open .hrt...", command=self._load_file,
                  bg=c["surface"], fg=c["text"], font=("Segoe UI", 8),
                  relief="flat", padx=8, pady=3).pack(side="left", padx=6)

        # Theme name
        name_row = tk.Frame(self.dialog, bg=c["bg"])
        name_row.pack(fill="x", padx=14, pady=(0, 8))
        tk.Label(name_row, text="Theme name:", bg=c["bg"], fg=c["text"],
                 font=("Segoe UI", 10)).pack(side="left")
        self._name_var = tk.StringVar(value="My Theme")
        tk.Entry(name_row, textvariable=self._name_var,
                 bg=c["surface"], fg=c["text"], font=("Segoe UI", 10),
                 relief="flat", width=22).pack(side="left", padx=8)

        # Color rows
        grid = tk.Frame(self.dialog, bg=c["bg"])
        grid.pack(fill="both", expand=True, padx=14)

        for i, (key, label, desc) in enumerate(self.THEME_KEYS):
            val = c.get(key, "#ffffff")
            var = tk.StringVar(value=val)
            self._vars[key] = var

            tk.Label(grid, text=label, bg=c["bg"], fg=c["text"],
                     font=("Segoe UI", 10), width=18, anchor="w").grid(
                         row=i, column=0, padx=(0,8), pady=5, sticky="w")

            # Color swatch button
            swatch = tk.Button(grid, bg=val, width=3, relief="flat",
                               command=lambda k=key: self._pick_color(k))
            swatch.grid(row=i, column=1, padx=4, pady=5)
            self._swatches[key] = swatch

            # Hex entry
            entry = tk.Entry(grid, textvariable=var, bg=c["surface"], fg=c["text"],
                             font=("Consolas", 10), relief="flat", width=10)
            entry.grid(row=i, column=2, padx=4, pady=5, sticky="w")
            entry.bind("<FocusOut>", lambda e, k=key: self._on_hex_change(k))
            entry.bind("<Return>",   lambda e, k=key: self._on_hex_change(k))

            tk.Label(grid, text=desc, bg=c["bg"], fg=c["text_secondary"],
                     font=("Segoe UI", 8), anchor="w").grid(
                         row=i, column=3, padx=8, pady=5, sticky="w")

        # Bottom
        sep = tk.Frame(self.dialog, bg=c["surface"], height=1)
        sep.pack(fill="x", padx=14, pady=(8, 0))
        bot = tk.Frame(self.dialog, bg=c["bg"])
        bot.pack(fill="x", padx=14, pady=8)
        tk.Button(bot, text="Preview", command=self._preview,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="left")
        tk.Button(bot, text="Cancel", command=self.dialog.destroy,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="right", padx=(6,0))
        tk.Button(bot, text="Save As .hrt", command=self._save,
                  bg=self.app.colors["success"], fg=self.app.colors["bg"],
                  font=("Segoe UI", 9, "bold"), relief="flat", padx=16, pady=6).pack(side="right")

    def _pick_color(self, key: str) -> None:
        from tkinter.colorchooser import askcolor
        current = self._vars[key].get()
        result = askcolor(color=current, title=f"Pick color for {key}",
                          parent=self.dialog)
        if result and result[1]:
            self._vars[key].set(result[1])
            self._swatches[key].config(bg=result[1])

    def _on_hex_change(self, key: str) -> None:
        val = self._vars[key].get().strip()
        if not val.startswith("#"):
            val = "#" + val
        try:
            self.dialog.winfo_rgb(val)  # validates color
            self._vars[key].set(val)
            self._swatches[key].config(bg=val)
        except Exception:
            pass  # invalid hex — ignore

    def _delete_asset(self, name: str, kind: str, combo: ttk.Combobox) -> None:
        """Delete a custom theme or language file."""
        if not name:
            messagebox.showwarning("Nothing selected", f"Select a {kind} to delete.")
            return
        if not messagebox.askyesno("Confirm delete",
                f"Delete {kind} '{name}'?\nThis cannot be undone."):
            return
        base = os.path.dirname(os.path.abspath(__file__))
        if kind == "theme":
            path = os.path.join(base, THEMES_DIR, f"{name}.hrt")
        else:
            path = os.path.join(base, LANGS_DIR, f"{name}.hrl")
        try:
            if os.path.exists(path):
                os.remove(path)
                log.info(f"Deleted {kind}: {path}")
                messagebox.showinfo("Deleted", f"{kind.capitalize()} '{name}' deleted.")
                # Refresh combo
                if kind == "theme":
                    combo.config(values=self.app._scan_custom_themes())
                else:
                    combo.config(values=[c for c, _ in self.app._scan_custom_languages()])
                combo.set("")
            else:
                messagebox.showerror("Not found", f"File not found:\n{path}")
        except Exception as e:
            messagebox.showerror("Delete failed", str(e))

    def _collect(self) -> dict:
        data = {"theme_name": self._name_var.get() or "My Theme",
                "schema_version": THEME_SCHEMA_VERSION}
        for key, var in self._vars.items():
            data[key] = var.get()
        return data

    def _load_builtin(self, name: str) -> None:
        colors = self.app.BUILTIN_THEMES.get(name, self.app.BUILTIN_THEMES["dark"])
        self._name_var.set(name.capitalize())
        for key, var in self._vars.items():
            val = colors.get(key, "#ffffff")
            var.set(val)
            self._swatches[key].config(bg=val)

    def _load_file(self) -> None:
        path = filedialog.askopenfilename(
            filetypes=[("HomRec Theme", "*.hrt"), ("All files", "*.*")],
            title="Open .hrt file"
        )
        if not path:
            return
        try:
            data = _hrc_read(path, _HRT_MAGIC)
            self._name_var.set(data.get("theme_name", "My Theme"))
            for key, var in self._vars.items():
                val = data.get(key, "#ffffff")
                var.set(val)
                self._swatches[key].config(bg=val)
        except Exception as e:
            messagebox.showerror("Load failed", str(e))

    def _preview(self) -> None:
        data = self._collect()
        self.app.colors = {**self.app.BUILTIN_THEMES["dark"], **data}
        self.app.apply_theme()
        messagebox.showinfo("Preview", "Theme applied temporarily.\\nSave to keep it.")

    def _save(self) -> None:
        data = self._collect()
        # Validate hex colors
        bad = []
        for key in THEME_REQUIRED_KEYS:
            try:
                self.dialog.winfo_rgb(data.get(key, ""))
            except Exception:
                bad.append(key)
        if bad:
            messagebox.showerror("Invalid colors",
                f"These colors are invalid: {', '.join(bad)}")
            return

        fname = data["theme_name"].lower().replace(" ", "_") + ".hrt"
        path = filedialog.asksaveasfilename(
            defaultextension=".hrt",
            filetypes=[("HomRec Theme", "*.hrt"), ("All files", "*.*")],
            initialfile=fname,
            title="Save theme as"
        )
        if not path:
            return
        try:
            _hrc_write(path, data, _HRT_MAGIC)
            # Copy to Assets/Themes/ automatically
            themes_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), THEMES_DIR)
            os.makedirs(themes_dir, exist_ok=True)
            import shutil
            shutil.copy2(path, os.path.join(themes_dir, os.path.basename(path)))
            messagebox.showinfo("Saved", f"Theme saved and installed:\\n{path}")
            log.info(f"Theme saved: {path}")
        except Exception as e:
            messagebox.showerror("Save failed", str(e))


