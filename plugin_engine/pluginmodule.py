"""
plugin_module.py — Advanced Settings plugin for HomRec 2.0

Standalone Python companion module.  Called from Lua via homrec.ui.call_python().
Does NOT import anything from the HomRec core — all app state is accessed through
the app_ref object passed in by the plugin API.

Public functions (called by Lua):
    open_advanced_settings(app)
    open_console(app)
"""

import os
import sys
import gzip
import json
import logging
import tkinter as tk
import tkinter.ttk as ttk
import tkinter.messagebox as messagebox
import tkinter.filedialog as filedialog

log = logging.getLogger("plugin.advanced_settings")

# -- Helpers -------------------------------------------------------------------

def _set_icon(window, app):
    """Try to apply the app icon to a Toplevel."""
    try:
        if hasattr(app, "_set_icon"):
            app._set_icon(window)
            return
    except Exception:
        pass
    try:
        base = os.path.dirname(os.path.abspath(sys.argv[0]))
        for candidate in [
            os.path.join(base, "Assets", "ofc", "main.ico"),
            os.path.join(base, "icons", "main.ico"),
            os.path.join(base, "main.ico"),
        ]:
            if os.path.exists(candidate):
                window.iconbitmap(candidate)
                return
    except Exception:
        pass


def _hrc_magic():
    return b"HRC\x01"


def _hrc_write(path: str, data: dict) -> None:
    raw = gzip.compress(json.dumps(data, ensure_ascii=False).encode("utf-8"))
    with open(path, "wb") as f:
        f.write(_hrc_magic())
        f.write(raw)


def _hrc_read(path: str) -> dict:
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic != _hrc_magic():
            raise ValueError("Not a HomRec profile (.hrc)")
        raw = f.read()
    return json.loads(gzip.decompress(raw).decode("utf-8"))


# -- Single-instance guards ----------------------------------------------------

_adv_window = None
_con_window  = None


# -- Advanced Settings window --------------------------------------------------

def open_advanced_settings(app):
    """Open the Advanced Settings dialog (single instance)."""
    global _adv_window
    try:
        if _adv_window is not None and _adv_window.winfo_exists():
            _adv_window.focus()
            return
    except Exception:
        pass

    _adv_window = _AdvancedSettingsWindow(app)


class _AdvancedSettingsWindow:
    HRC_VERSION = 1

    def __init__(self, app):
        self.app = app
        c = app.colors

        self.win = tk.Toplevel(app.root)
        _set_icon(self.win, app)
        self.win.title("Advanced Settings")
        self.win.geometry("560x640")
        self.win.resizable(False, False)
        self.win.configure(bg=c["bg"])
        self.win.grab_set()
        self.win.protocol("WM_DELETE_WINDOW", self.win.destroy)

        self._build_ui()

    def _build_ui(self):
        a = self.app
        c = a.colors

        tk.Label(self.win, text="⚙ Advanced Settings",
                 bg=c["bg"], fg=c["accent"],
                 font=("Segoe UI", 14, "bold")).pack(pady=(16, 4), padx=20, anchor="w")
        tk.Label(self.win,
                 text="For power users. Changes apply on next recording.",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI", 9)).pack(padx=20, anchor="w")

        nb = ttk.Notebook(self.win)
        nb.pack(fill="both", expand=True, padx=16, pady=12)

        # -- Video ------------------------------------------------------
        vt = tk.Frame(nb, bg=c["bg"])
        nb.add(vt, text="Video")

        self._cv   = tk.StringVar(value=getattr(a, "video_codec", "libx264"))
        self._hwv  = tk.StringVar(value=getattr(a, "hw_accel", "auto"))
        self._prev = tk.StringVar(value=getattr(a, "enc_preset", "ultrafast"))
        self._crfv = tk.IntVar(value=getattr(a, "enc_crf", 18))
        self._pixv = tk.StringVar(value=getattr(a, "pix_fmt", "yuv420p"))

        self._row(vt, "Codec",
                  ttk.Combobox(vt, textvariable=self._cv, state="readonly", width=18,
                               values=["libx264","libx265","h264_nvenc","hevc_nvenc",
                                       "h264_amf","hevc_amf","h264_qsv","hevc_qsv"]))
        self._row(vt, "HW Accel",
                  ttk.Combobox(vt, textvariable=self._hwv, state="readonly", width=12,
                               values=["auto","none","cuda","dxva2","d3d11va"]))
        self._row(vt, "Preset",
                  ttk.Combobox(vt, textvariable=self._prev, state="readonly", width=12,
                               values=["ultrafast","superfast","veryfast","faster",
                                       "fast","medium","slow"]))
        self._row(vt, "CRF (quality)",
                  tk.Scale(vt, variable=self._crfv, from_=0, to=51,
                           orient="horizontal", length=180,
                           bg=c["bg"], fg=c["text"], highlightthickness=0,
                           troughcolor=c["surface"]))
        self._row(vt, "Pixel format",
                  ttk.Combobox(vt, textvariable=self._pixv, state="readonly", width=12,
                               values=["yuv420p","yuv444p","rgb24"]))

        # -- Audio ------------------------------------------------------
        at = tk.Frame(nb, bg=c["bg"])
        nb.add(at, text="Audio")

        self._srv  = tk.StringVar(value=str(getattr(a, "audio_sample_rate", 44100)))
        self._abrv = tk.StringVar(value=getattr(a, "audio_aac_bitrate", "192k"))
        self._achv = tk.StringVar(value=str(getattr(a, "audio_out_channels", 2)))

        self._row(at, "Sample rate",
                  ttk.Combobox(at, textvariable=self._srv, state="readonly", width=10,
                               values=["44100","48000","96000"]))
        self._row(at, "AAC bitrate",
                  ttk.Combobox(at, textvariable=self._abrv, state="readonly", width=10,
                               values=["96k","128k","192k","256k","320k"]))
        self._row(at, "Channels",
                  ttk.Combobox(at, textvariable=self._achv, state="readonly", width=6,
                               values=["1","2"]))

        # -- Interface --------------------------------------------------
        it = tk.Frame(nb, bg=c["bg"])
        nb.add(it, text="Interface")

        self._thv       = tk.StringVar(value=getattr(a, "ui_theme", "dark"))
        self._uisv      = tk.StringVar(value=str(int(getattr(a, "ui_scale", 1.0)*100)) + "%")
        self._fontv     = tk.StringVar(value=getattr(a, "ui_font", "Segoe UI"))
        self._statuscmdv = tk.BooleanVar(value=getattr(a, "status_cmd_enabled", False))

        self._row(it, "Theme",
                  ttk.Combobox(it, textvariable=self._thv, state="readonly", width=14,
                               values=["dark","light","catppuccin","nord","dracula"]))

        # Theme / Language editor buttons
        row_te = it.grid_size()[1]
        tk.Button(it, text="🎨 Theme Editor...",
                  command=self._open_theme_editor,
                  bg=c["surface"], fg=c["accent"],
                  font=("Segoe UI", 9), relief="flat",
                  padx=10, pady=5).grid(row=row_te, column=1, sticky="w",
                                        padx=(0, 20), pady=(8, 2))
        row_le = it.grid_size()[1]
        tk.Button(it, text="🌐 Language Editor...",
                  command=self._open_lang_editor,
                  bg=c["surface"], fg=c["accent"],
                  font=("Segoe UI", 9), relief="flat",
                  padx=10, pady=5).grid(row=row_le, column=1, sticky="w",
                                        padx=(0, 20), pady=2)

        # Separator
        row_sep = it.grid_size()[1]
        tk.Frame(it, bg=c["surface"], height=1).grid(
            row=row_sep, column=0, columnspan=3, sticky="ew", padx=20, pady=(12, 4))

        # Delete custom theme
        row_dt = it.grid_size()[1]
        tk.Label(it, text="Delete theme", bg=c["bg"], fg=c["text"],
                 font=("Segoe UI", 10), anchor="w").grid(
                     row=row_dt, column=0, sticky="w", padx=(20, 8), pady=4)
        self._del_theme_var = tk.StringVar()
        theme_files = a._scan_custom_themes() if hasattr(a, "_scan_custom_themes") else []
        del_theme_cb = ttk.Combobox(it, textvariable=self._del_theme_var,
                                    values=theme_files, width=16, state="readonly")
        del_theme_cb.grid(row=row_dt, column=1, sticky="w", padx=(0, 4), pady=4)
        tk.Button(it, text="🗑 Delete",
                  command=lambda: self._delete_asset(
                      self._del_theme_var.get(), "theme", del_theme_cb),
                  bg=c["error"], fg=c["bg"],
                  font=("Segoe UI", 9), relief="flat",
                  padx=8, pady=3).grid(row=row_dt, column=2, sticky="w", pady=4)

        # Delete custom language
        row_dl = it.grid_size()[1]
        tk.Label(it, text="Delete language", bg=c["bg"], fg=c["text"],
                 font=("Segoe UI", 10), anchor="w").grid(
                     row=row_dl, column=0, sticky="w", padx=(20, 8), pady=4)
        self._del_lang_var = tk.StringVar()
        lang_codes = ([code for code, _ in a._scan_custom_languages()]
                      if hasattr(a, "_scan_custom_languages") else [])
        del_lang_cb = ttk.Combobox(it, textvariable=self._del_lang_var,
                                   values=lang_codes, width=16, state="readonly")
        del_lang_cb.grid(row=row_dl, column=1, sticky="w", padx=(0, 4), pady=4)
        tk.Button(it, text="🗑 Delete",
                  command=lambda: self._delete_asset(
                      self._del_lang_var.get(), "language", del_lang_cb),
                  bg=c["error"], fg=c["bg"],
                  font=("Segoe UI", 9), relief="flat",
                  padx=8, pady=3).grid(row=row_dl, column=2, sticky="w", pady=4)

        self._row(it, "UI scale",
                  ttk.Combobox(it, textvariable=self._uisv, state="readonly", width=8,
                               values=["80%","90%","100%","110%","125%"]))
        self._row(it, "Font",
                  ttk.Combobox(it, textvariable=self._fontv, state="readonly", width=14,
                               values=["Segoe UI","Consolas","Arial","Calibri"]))

        row_sc = it.grid_size()[1]
        tk.Label(it, text="Status command line", bg=c["bg"], fg=c["text"],
                 font=("Segoe UI", 10), anchor="w").grid(
                     row=row_sc, column=0, sticky="w", padx=(20, 8), pady=(10, 4))
        tk.Checkbutton(it, text="Enable (type commands in status bar)",
                       variable=self._statuscmdv,
                       bg=c["bg"], fg=c["text"], selectcolor=c["surface"],
                       font=("Segoe UI", 10)).grid(
                           row=row_sc, column=1, sticky="w", padx=(0, 20), pady=(10, 4))

        # -- Recording --------------------------------------------------
        rt = tk.Frame(nb, bg=c["bg"])
        nb.add(rt, text="Recording")

        self._ftv = tk.StringVar(value=getattr(a, "filename_template", "HomRec_{date}_{time}"))
        self._asv = tk.StringVar(value=str(getattr(a, "auto_stop_min", 0)))
        self._rbv = tk.StringVar(value=str(getattr(a, "replay_buffer_sec", 0)))

        self._row(rt, "File template",
                  tk.Entry(rt, textvariable=self._ftv,
                           bg=c["surface"], fg=c["text"], font=("Consolas", 10),
                           relief="flat", width=24))
        self._row(rt, "Auto-stop (min)",
                  tk.Spinbox(rt, textvariable=self._asv, from_=0, to=480, width=6,
                             bg=c["surface"], fg=c["text"], relief="flat"))
        row = rt.grid_size()[1]
        tk.Label(rt, text="  0 = disabled", bg=c["bg"],
                 fg=c["text_secondary"], font=("Segoe UI", 8)).grid(
                     row=row, column=1, sticky="w", padx=(0, 20))
        self._row(rt, "Replay buffer (s)",
                  tk.Spinbox(rt, textvariable=self._rbv, from_=0, to=300, width=6,
                             bg=c["surface"], fg=c["text"], relief="flat"))
        row = rt.grid_size()[1]
        tk.Label(rt, text="  0 = disabled", bg=c["bg"],
                 fg=c["text_secondary"], font=("Segoe UI", 8)).grid(
                     row=row, column=1, sticky="w", padx=(0, 20))

        # -- Hotkeys ----------------------------------------------------
        ht = tk.Frame(nb, bg=c["bg"])
        nb.add(ht, text="Hotkeys")

        tk.Label(ht, text="Click a field and press any key combination",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI", 9)).grid(
                     row=0, column=0, columnspan=2, padx=20, pady=(10, 4), sticky="w")

        self._hk_ss = tk.StringVar(value=getattr(a, "hotkey_start_stop", "F9"))
        self._hk_p  = tk.StringVar(value=getattr(a, "hotkey_pause", "F10"))
        self._hk_fs = tk.StringVar(value=getattr(a, "hotkey_fullscreen", "F11"))

        for label, var in [("Start / Stop",   self._hk_ss),
                            ("Pause / Resume", self._hk_p),
                            ("Fullscreen",     self._hk_fs)]:
            entry = tk.Entry(ht, textvariable=var,
                             bg=c["surface"], fg=c["accent"],
                             font=("Consolas", 11), relief="flat", width=12,
                             readonlybackground=c["surface"], state="readonly")
            entry.bind("<FocusIn>",
                       lambda e, v=var, en=entry: self._start_key_capture(v, en))
            entry.bind("<FocusOut>",
                       lambda e, en=entry: en.config(state="readonly"))
            self._row(ht, label, entry)

        # -- Notifications ----------------------------------------------
        nt = tk.Frame(nb, bg=c["bg"])
        nb.add(nt, text="Notifications")

        self._notif_sound = tk.BooleanVar(value=getattr(a, "notify_sound", True))
        self._notif_flash = tk.BooleanVar(value=getattr(a, "notify_flash", True))
        self._auto_save   = tk.BooleanVar(value=getattr(a, "auto_save_profile", False))

        for text, var in [
            ("Sound beep on recording start",  self._notif_sound),
            ("Flash border on recording start", self._notif_flash),
            ("Auto-save profile on exit",       self._auto_save),
        ]:
            row = nt.grid_size()[1]
            tk.Checkbutton(nt, text=text, variable=var,
                           bg=c["bg"], fg=c["text"], selectcolor=c["surface"],
                           font=("Segoe UI", 10)).grid(
                               row=row, column=0, columnspan=2,
                               sticky="w", padx=20, pady=4)

        # -- Bottom buttons ---------------------------------------------
        tk.Frame(self.win, bg=c["surface"], height=1).pack(fill="x", padx=16, pady=(4, 0))
        bot = tk.Frame(self.win, bg=c["bg"])
        bot.pack(fill="x", padx=16, pady=10)

        tk.Button(bot, text="⬆ Export .hrc", command=self._export,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 9), relief="flat",
                  padx=12, pady=6).pack(side="left", padx=(0, 6))
        tk.Button(bot, text="⬇ Import .hrc", command=self._import,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 9), relief="flat",
                  padx=12, pady=6).pack(side="left")
        tk.Button(bot, text="Cancel", command=self.win.destroy,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 9), relief="flat",
                  padx=12, pady=6).pack(side="right", padx=(6, 0))
        tk.Button(bot, text="Save", command=self._save,
                  bg=c["success"], fg=c["bg"],
                  font=("Segoe UI", 9, "bold"), relief="flat",
                  padx=16, pady=6).pack(side="right")

    # -- Helpers -------------------------------------------------------

    def _row(self, parent, label, widget):
        row = parent.grid_size()[1]
        tk.Label(parent, text=label,
                 bg=self.app.colors["bg"], fg=self.app.colors["text"],
                 font=("Segoe UI", 10), anchor="w").grid(
                     row=row, column=0, sticky="w", padx=(20, 8), pady=6)
        widget.grid(row=row, column=1, sticky="w", padx=(0, 20), pady=6)
        parent.columnconfigure(1, weight=1)

    def _start_key_capture(self, var, entry):
        entry.config(state="normal")
        var.set("Press a key...")
        def on_key(event):
            parts = []
            if event.state & 0x4: parts.append("Control")
            if event.state & 0x1: parts.append("Shift")
            if event.state & 0x8: parts.append("Alt")
            key = event.keysym
            if key not in ("Control_L","Control_R","Shift_L","Shift_R","Alt_L","Alt_R"):
                parts.append(key)
            if parts:
                var.set("+".join(parts))
            entry.config(state="readonly")
            entry.unbind("<KeyPress>")
        entry.bind("<KeyPress>", on_key)

    def _open_theme_editor(self):
        try:
            from ui.theme_editor import ThemeEditorDialog
            ThemeEditorDialog(self.win, self.app)
        except Exception as e:
            messagebox.showerror("Theme Editor", str(e))

    def _open_lang_editor(self):
        try:
            from ui.lang_editor import LanguageEditorDialog
            LanguageEditorDialog(self.win, self.app)
        except Exception as e:
            messagebox.showerror("Language Editor", str(e))

    def _delete_asset(self, name, kind, combo):
        if not name:
            messagebox.showwarning("Nothing selected", f"Select a {kind} to delete.")
            return
        if not messagebox.askyesno("Confirm", f"Delete {kind} '{name}'?"):
            return
        a = self.app
        try:
            from core.constants import THEMES_DIR, LANGS_DIR
            base = os.path.dirname(os.path.abspath(
                getattr(a, "__file__", sys.argv[0])))
            path = (os.path.join(base, THEMES_DIR, f"{name}.hrt") if kind == "theme"
                    else os.path.join(base, LANGS_DIR, f"{name}.hrl"))
            if os.path.exists(path):
                os.remove(path)
                messagebox.showinfo("Deleted", f"{kind.capitalize()} '{name}' deleted.")
                if kind == "theme":
                    combo.config(values=a._scan_custom_themes())
                else:
                    combo.config(values=[c for c, _ in a._scan_custom_languages()])
                combo.set("")
            else:
                messagebox.showerror("Not found", f"File not found:\n{path}")
        except Exception as e:
            messagebox.showerror("Delete failed", str(e))

    def _collect(self):
        return {
            "hrc_version":       self.HRC_VERSION,
            "video_codec":       self._cv.get(),
            "hw_accel":          self._hwv.get(),
            "enc_preset":        self._prev.get(),
            "enc_crf":           self._crfv.get(),
            "pix_fmt":           self._pixv.get(),
            "audio_sample_rate": int(self._srv.get()),
            "audio_aac_bitrate": self._abrv.get(),
            "audio_out_channels":int(self._achv.get()),
            "ui_theme":          self._thv.get(),
            "ui_scale":          int(self._uisv.get().replace("%","")) / 100,
            "ui_font":           self._fontv.get(),
            "status_cmd_enabled":self._statuscmdv.get(),
            "filename_template": self._ftv.get(),
            "auto_stop_min":     int(self._asv.get() or 0),
            "replay_buffer_sec": int(self._rbv.get() or 0),
            "hotkey_start_stop": self._hk_ss.get(),
            "hotkey_pause":      self._hk_p.get(),
            "hotkey_fullscreen": self._hk_fs.get(),
            "notify_sound":      self._notif_sound.get(),
            "notify_flash":      self._notif_flash.get(),
            "auto_save_profile": self._auto_save.get(),
        }

    def _save(self):
        data = self._collect()
        a = self.app
        for k, v in data.items():
            if k != "hrc_version":
                setattr(a, k, v)
        if hasattr(a, "_apply_hotkeys"):
            a._apply_hotkeys()
        if hasattr(a, "apply_theme"):
            a.colors = a.get_theme_colors(data["ui_theme"])
            a.apply_theme()
        if hasattr(a, "_set_status_cmd_enabled"):
            a._set_status_cmd_enabled(bool(data.get("status_cmd_enabled")))
        if hasattr(a, "save_settings"):
            a.save_settings(silent=True)
        log.info(f"Advanced settings saved")
        self.win.destroy()

    def _export(self):
        path = filedialog.asksaveasfilename(
            defaultextension=".hrc",
            filetypes=[("HomRec Profile", "*.hrc"), ("All files", "*.*")],
            initialfile="homrec_profile.hrc",
            title="Export profile")
        if not path:
            return
        try:
            _hrc_write(path, self._collect())
            messagebox.showinfo("Exported", f"Profile saved to:\n{path}")
        except Exception as e:
            messagebox.showerror("Export failed", str(e))

    def _import(self):
        path = filedialog.askopenfilename(
            filetypes=[("HomRec Profile", "*.hrc"), ("All files", "*.*")],
            title="Import profile")
        if not path:
            return
        try:
            data = _hrc_read(path)
            self._cv.set(data.get("video_codec", "libx264"))
            self._hwv.set(data.get("hw_accel", "auto"))
            self._prev.set(data.get("enc_preset", "ultrafast"))
            self._crfv.set(data.get("enc_crf", 18))
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
        except Exception as e:
            messagebox.showerror("Import failed", str(e))


# -- Plugin Console window -----------------------------------------------------

def open_console(app):
    """Open the Plugin Console (single instance, opens Library on Console tab)."""
    global _con_window
    try:
        if _con_window is not None and _con_window.winfo_exists():
            _con_window.focus()
            return
    except Exception:
        pass

    # The console lives in Library — open it there
    if hasattr(app, "_open_library"):
        app._open_library()
    else:
        messagebox.showinfo("Console", "Library not available.")