"""ui/advanced_dialog.py — Advanced Settings dialog"""
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

from ui.lang_editor import LanguageEditorDialog
from ui.theme_editor import ThemeEditorDialog

class AdvancedSettingsDialog:
    """Power-user settings window with import/export (.hrc)."""

    HRC_VERSION = 1

    def __init__(self, parent: tk.Tk, app) -> None:
        self.app = app
        self.dialog = tk.Toplevel(parent)
        self.dialog.title("Advanced Settings")
        self.dialog.geometry("560x640")
        self.dialog.resizable(False, False)
        self.dialog.configure(bg=app.colors["bg"])
        self.dialog.grab_set()
        try:
            icon_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "main.ico")
            if os.path.exists(icon_path):
                self.dialog.after(50, lambda: self.dialog.iconbitmap(icon_path))
        except Exception:
            pass
        self._build_ui()

    def _build_ui(self) -> None:
        a = self.app
        c = a.colors

        title = tk.Label(self.dialog, text="⚙ Advanced Settings",
                         bg=c["bg"], fg=c["accent"],
                         font=("Segoe UI", 14, "bold"))
        title.pack(pady=(16, 4), padx=20, anchor="w")
        tk.Label(self.dialog, text="For power users. Changes apply on next recording.",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI", 9)).pack(padx=20, anchor="w")

        notebook = ttk.Notebook(self.dialog)
        notebook.pack(fill="both", expand=True, padx=16, pady=12)

        # -- Video tab ------------------------------------------------
        vt = tk.Frame(notebook, bg=c["bg"])
        notebook.add(vt, text="Video")
        self._cv = tk.StringVar(value=getattr(a, "video_codec", "libx264"))
        self._row(vt, "Codec",
                  ttk.Combobox(vt, textvariable=self._cv,
                               values=["libx264","libx265","h264_nvenc","hevc_nvenc",
                                       "h264_amf","hevc_amf","h264_qsv","hevc_qsv"],
                               width=18, state="readonly"))
        self._hwv = tk.StringVar(value=getattr(a, "hw_accel", "auto"))
        self._row(vt, "HW Accel",
                  ttk.Combobox(vt, textvariable=self._hwv,
                               values=["auto","none","cuda","dxva2","d3d11va"],
                               width=12, state="readonly"))
        self._prev = tk.StringVar(value=getattr(a, "enc_preset", "ultrafast"))
        self._row(vt, "Preset",
                  ttk.Combobox(vt, textvariable=self._prev,
                               values=["ultrafast","superfast","veryfast","faster","fast","medium","slow"],
                               width=12, state="readonly"))
        self._crfv = tk.IntVar(value=getattr(a, "enc_crf", 18))
        self._row(vt, "CRF (quality)",
                  tk.Scale(vt, variable=self._crfv,
                           from_=0, to=51, orient="horizontal", length=180,
                           bg=c["bg"], fg=c["text"], highlightthickness=0,
                           troughcolor=c["surface"]))
        self._pixv = tk.StringVar(value=getattr(a, "pix_fmt", "yuv420p"))
        self._row(vt, "Pixel format",
                  ttk.Combobox(vt, textvariable=self._pixv,
                               values=["yuv420p","yuv444p","rgb24"],
                               width=12, state="readonly"))

        # -- Audio tab ------------------------------------------------
        at = tk.Frame(notebook, bg=c["bg"])
        notebook.add(at, text="Audio")
        self._srv = tk.StringVar(value=str(getattr(a, "audio_sample_rate", 44100)))
        self._row(at, "Sample rate",
                  ttk.Combobox(at, textvariable=self._srv,
                               values=["44100","48000","96000"],
                               width=10, state="readonly"))
        self._abrv = tk.StringVar(value=getattr(a, "audio_aac_bitrate", "192k"))
        self._row(at, "AAC bitrate",
                  ttk.Combobox(at, textvariable=self._abrv,
                               values=["96k","128k","192k","256k","320k"],
                               width=10, state="readonly"))
        self._achv = tk.StringVar(value=str(getattr(a, "audio_out_channels", 2)))
        self._row(at, "Channels",
                  ttk.Combobox(at, textvariable=self._achv,
                               values=["1","2"],
                               width=6, state="readonly"))

        # -- Interface tab --------------------------------------------
        it = tk.Frame(notebook, bg=c["bg"])
        notebook.add(it, text="Interface")
        self._thv = tk.StringVar(value=getattr(a, "ui_theme", "dark"))
        self._row(it, "Theme",
                  ttk.Combobox(it, textvariable=self._thv,
                               values=["dark","light","catppuccin","nord","dracula"],
                               width=14, state="readonly"))
        # Editor buttons
        row_te = it.grid_size()[1]
        tk.Button(it, text="🎨 Theme Editor...",
                  command=lambda: ThemeEditorDialog(self.dialog, self.app),
                  bg=c["surface"], fg=c["accent"],
                  font=("Segoe UI", 9), relief="flat", padx=10, pady=5).grid(
                      row=row_te, column=1, sticky="w", padx=(0,20), pady=(8,2))
        row_le = it.grid_size()[1]
        tk.Button(it, text="🌐 Language Editor...",
                  command=lambda: LanguageEditorDialog(self.dialog, self.app),
                  bg=c["surface"], fg=c["accent"],
                  font=("Segoe UI", 9), relief="flat", padx=10, pady=5).grid(
                      row=row_le, column=1, sticky="w", padx=(0,20), pady=2)

        # Separator
        row_sep = it.grid_size()[1]
        tk.Frame(it, bg=c["surface"], height=1).grid(
            row=row_sep, column=0, columnspan=3, sticky="ew", padx=20, pady=(12,4))

        # Delete theme
        row_dt = it.grid_size()[1]
        tk.Label(it, text="Delete theme", bg=c["bg"], fg=c["text"],
                 font=("Segoe UI", 10), anchor="w").grid(
                     row=row_dt, column=0, sticky="w", padx=(20,8), pady=4)
        self._del_theme_var = tk.StringVar()
        theme_files = self.app._scan_custom_themes()
        del_theme_combo = ttk.Combobox(it, textvariable=self._del_theme_var,
                         values=theme_files, width=16, state="readonly")
        del_theme_combo.grid(row=row_dt, column=1, sticky="w", padx=(0,4), pady=4)
        tk.Button(it, text="🗑 Delete",
                  command=lambda: self._delete_asset(
                      self._del_theme_var.get(), "theme", del_theme_combo),
                  bg=c["error"], fg=c["bg"],
                  font=("Segoe UI", 9), relief="flat", padx=8, pady=3).grid(
                      row=row_dt, column=2, sticky="w", pady=4)

        # Delete language
        row_dl = it.grid_size()[1]
        tk.Label(it, text="Delete language", bg=c["bg"], fg=c["text"],
                 font=("Segoe UI", 10), anchor="w").grid(
                     row=row_dl, column=0, sticky="w", padx=(20,8), pady=4)
        self._del_lang_var = tk.StringVar()
        lang_files = [code for code, _ in self.app._scan_custom_languages()]
        del_lang_combo = ttk.Combobox(it, textvariable=self._del_lang_var,
                         values=lang_files, width=16, state="readonly")
        del_lang_combo.grid(row=row_dl, column=1, sticky="w", padx=(0,4), pady=4)
        tk.Button(it, text="🗑 Delete",
                  command=lambda: self._delete_asset(
                      self._del_lang_var.get(), "language", del_lang_combo),
                  bg=c["error"], fg=c["bg"],
                  font=("Segoe UI", 9), relief="flat", padx=8, pady=3).grid(
                      row=row_dl, column=2, sticky="w", pady=4)
        self._uisv = tk.StringVar(value=str(int(getattr(a, "ui_scale", 1.0)*100))+"%")
        self._row(it, "UI scale",
                  ttk.Combobox(it, textvariable=self._uisv,
                               values=["80%","90%","100%","110%","125%"],
                               width=8, state="readonly"))
        self._fontv = tk.StringVar(value=getattr(a, "ui_font", "Segoe UI"))
        self._row(it, "Font",
                  ttk.Combobox(it, textvariable=self._fontv,
                               values=["Segoe UI","Consolas","Arial","Calibri"],
                               width=14, state="readonly"))

        self._statuscmdv = tk.BooleanVar(value=getattr(a, "status_cmd_enabled", False))
        row_sc = it.grid_size()[1]
        tk.Label(it, text="Status command line", bg=c["bg"], fg=c["text"],
                 font=("Segoe UI", 10), anchor="w").grid(
                     row=row_sc, column=0, sticky="w", padx=(20, 8), pady=(10, 4))
        tk.Checkbutton(it, text="Enable (type commands in status bar)",
                       variable=self._statuscmdv,
                       bg=c["bg"], fg=c["text"],
                       selectcolor=c["surface"],
                       font=("Segoe UI", 10)).grid(
                           row=row_sc, column=1, sticky="w", padx=(0, 20), pady=(10, 4))

        # -- Recording tab --------------------------------------------
        rt = tk.Frame(notebook, bg=c["bg"])
        notebook.add(rt, text="Recording")
        self._ftv = tk.StringVar(value=getattr(a, "filename_template", "HomRec_{date}_{time}"))
        self._row(rt, "File template",
                  tk.Entry(rt, textvariable=self._ftv,
                           bg=c["surface"], fg=c["text"], font=("Consolas", 10),
                           relief="flat", width=24))
        self._asv = tk.StringVar(value=str(getattr(a, "auto_stop_min", 0)))
        self._row(rt, "Auto-stop (min)",
                  tk.Spinbox(rt, textvariable=self._asv,
                             from_=0, to=480, width=6,
                             bg=c["surface"], fg=c["text"], relief="flat"))
        row = rt.grid_size()[1]
        tk.Label(rt, text="  0 = disabled", bg=c["bg"],
                 fg=c["text_secondary"], font=("Segoe UI", 8)).grid(
                     row=row, column=1, sticky="w", padx=(0, 20))
        self._rbv = tk.StringVar(value=str(getattr(a, "replay_buffer_sec", 0)))
        self._row(rt, "Replay buffer (s)",
                  tk.Spinbox(rt, textvariable=self._rbv,
                             from_=0, to=300, width=6,
                             bg=c["surface"], fg=c["text"], relief="flat"))
        row = rt.grid_size()[1]
        tk.Label(rt, text="  0 = disabled", bg=c["bg"],
                 fg=c["text_secondary"], font=("Segoe UI", 8)).grid(
                     row=row, column=1, sticky="w", padx=(0, 20))

        # -- Hotkeys tab ----------------------------------------------
        ht = tk.Frame(notebook, bg=c["bg"])
        notebook.add(ht, text="Hotkeys")
        tk.Label(ht, text="Click a field and press any key combination",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI", 9)).grid(row=0, column=0, columnspan=2,
                                             padx=20, pady=(10,4), sticky="w")
        self._hk_ss = tk.StringVar(value=getattr(a, "hotkey_start_stop", "F9"))
        self._hk_p  = tk.StringVar(value=getattr(a, "hotkey_pause", "F10"))
        self._hk_fs = tk.StringVar(value=getattr(a, "hotkey_fullscreen", "F11"))
        for label, var in [("Start / Stop", self._hk_ss),
                            ("Pause / Resume", self._hk_p),
                            ("Fullscreen", self._hk_fs)]:
            entry = tk.Entry(ht, textvariable=var, bg=c["surface"], fg=c["accent"],
                             font=("Consolas", 11), relief="flat", width=12,
                             readonlybackground=c["surface"], state="readonly")
            entry.bind("<FocusIn>",  lambda e, v=var, en=entry: self._start_key_capture(v, en))
            entry.bind("<FocusOut>", lambda e, en=entry: en.config(state="readonly"))
            self._row(ht, label, entry)

        # -- Notifications tab ----------------------------------------
        nt = tk.Frame(notebook, bg=c["bg"])
        notebook.add(nt, text="Notifications")
        self._notif_sound = tk.BooleanVar(value=getattr(a, "notify_sound", True))
        self._notif_flash = tk.BooleanVar(value=getattr(a, "notify_flash", True))
        self._auto_save   = tk.BooleanVar(value=getattr(a, "auto_save_profile", False))
        for text, var in [
            ("Sound beep on recording start", self._notif_sound),
            ("Flash border on recording start", self._notif_flash),
            ("Auto-save profile on exit", self._auto_save),
        ]:
            row = nt.grid_size()[1]
            tk.Checkbutton(nt, text=text, variable=var,
                           bg=c["bg"], fg=c["text"],
                           selectcolor=c["surface"],
                           font=("Segoe UI", 10)).grid(
                               row=row, column=0, columnspan=2,
                               sticky="w", padx=20, pady=4)

        # -- Bottom buttons -------------------------------------------
        sep = tk.Frame(self.dialog, bg=c["surface"], height=1)
        sep.pack(fill="x", padx=16, pady=(4, 0))

        bot = tk.Frame(self.dialog, bg=c["bg"])
        bot.pack(fill="x", padx=16, pady=10)

        tk.Button(bot, text="⬆ Export .hrc", command=self._export,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="left", padx=(0, 6))
        tk.Button(bot, text="⬇ Import .hrc", command=self._import,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="left")
        tk.Button(bot, text="Cancel", command=self.dialog.destroy,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=6).pack(side="right", padx=(6, 0))
        tk.Button(bot, text="Save", command=self._save,
                  bg=c["success"], fg=c["bg"],
                  font=("Segoe UI", 9, "bold"), relief="flat", padx=16, pady=6).pack(side="right")

    def _row(self, parent, label: str, widget) -> None:
        """Add a label+widget row using grid for clean alignment."""
        # Find next available grid row
        row = parent.grid_size()[1]
        tk.Label(parent, text=label, bg=self.app.colors["bg"],
                 fg=self.app.colors["text"],
                 font=("Segoe UI", 10), anchor="w").grid(
                     row=row, column=0, sticky="w", padx=(20, 8), pady=6)
        widget.grid(row=row, column=1, sticky="w", padx=(0, 20), pady=6)
        parent.columnconfigure(1, weight=1)

    def _start_key_capture(self, var: tk.StringVar, entry: tk.Entry) -> None:
        """Let user press a key to set hotkey."""
        entry.config(state="normal")
        var.set("Press a key...")
        def on_key(event):
            parts = []
            if event.state & 0x4:  parts.append("Control")
            if event.state & 0x1:  parts.append("Shift")
            if event.state & 0x8:  parts.append("Alt")
            key = event.keysym
            if key not in ("Control_L","Control_R","Shift_L","Shift_R","Alt_L","Alt_R"):
                parts.append(key)
            if parts:
                var.set("+".join(parts))
            entry.config(state="readonly")
            entry.unbind("<KeyPress>")
        entry.bind("<KeyPress>", on_key)

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
        return {
            "hrc_version": self.HRC_VERSION,
            "video_codec": self._cv.get(),
            "hw_accel": self._hwv.get(),
            "enc_preset": self._prev.get(),
            "enc_crf": self._crfv.get(),
            "pix_fmt": self._pixv.get(),
            "audio_sample_rate": int(self._srv.get()),
            "audio_aac_bitrate": self._abrv.get(),
            "audio_out_channels": int(self._achv.get()),
            "ui_theme": self._thv.get(),
            "ui_scale": int(self._uisv.get().replace("%", "")) / 100,
            "ui_font": self._fontv.get(),
            "status_cmd_enabled": self._statuscmdv.get(),
            "filename_template": self._ftv.get(),
            "auto_stop_min": int(self._asv.get() or 0),
            "replay_buffer_sec": int(self._rbv.get() or 0),
            "hotkey_start_stop": self._hk_ss.get(),
            "hotkey_pause": self._hk_p.get(),
            "hotkey_fullscreen": self._hk_fs.get(),
            "notify_sound": self._notif_sound.get(),
            "notify_flash": self._notif_flash.get(),
            "auto_save_profile": self._auto_save.get(),
        }

    def _save(self) -> None:
        data = self._collect()
        a = self.app
        for k, v in data.items():
            if k != "hrc_version":
                setattr(a, k, v)
        # Re-apply hotkeys immediately
        if hasattr(a, '_apply_hotkeys'):
            a._apply_hotkeys()
        # Re-apply theme if changed
        if hasattr(a, 'apply_theme'):
            a.colors = a.get_theme_colors(data["ui_theme"])
            a.apply_theme()
        # Toggle status command line visibility immediately
        if hasattr(a, "_set_status_cmd_enabled"):
            a._set_status_cmd_enabled(bool(data.get("status_cmd_enabled")))
        a.save_settings(silent=True)
        log.info(f"Advanced settings saved: {data}")
        self.dialog.destroy()

    def _export(self) -> None:
        path = filedialog.asksaveasfilename(
            defaultextension=".hrc",
            filetypes=[("HomRec Profile", "*.hrc"), ("All files", "*.*")],
            initialfile="homrec_profile.hrc",
            title="Export profile"
        )
        if not path:
            return
        data = self._collect()
        try:
            _hrc_write(path, data, _HRC_MAGIC)
            messagebox.showinfo("Exported", f"Profile saved to:\n{path}")
            log.info(f"Profile exported (binary): {path}")
        except Exception as e:
            messagebox.showerror("Export failed", str(e))

    def _import(self) -> None:
        path = filedialog.askopenfilename(
            filetypes=[("HomRec Profile", "*.hrc"), ("All files", "*.*")],
            title="Import profile"
        )
        if not path:
            return
        try:
            data = _hrc_read(path, _HRC_MAGIC)
            # Apply to UI vars
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
            # Also load new fields if present
            if hasattr(self, '_hk_ss'):
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


