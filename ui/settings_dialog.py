"""ui/settings_dialog.py — Settings dialog"""
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

from ui.advanced_dialog import AdvancedSettingsDialog

class SettingsDialog:
    def __init__(self, parent, app) -> None:
        self.app = app
        self.dialog = tk.Toplevel(parent)
        self.dialog.title(app.lang["settings_title"])
        self.dialog.geometry("500x500")
        self.dialog.configure(bg=app.colors["bg"])
        self.dialog.transient(parent)
        self.dialog.grab_set()
        
        self.dialog.update_idletasks()
        x = (self.dialog.winfo_screenwidth() // 2) - 250
        y = (self.dialog.winfo_screenheight() // 2) - 250
        self.dialog.geometry(f"+{x}+{y}")

        # Apply app icon
        try:
            if getattr(sys, 'frozen', False):
                base_dir = os.path.dirname(sys.executable)
            else:
                base_dir = os.path.dirname(os.path.abspath(__file__))
            ico_path = os.path.join(base_dir, "icons", "main.ico")
            if os.path.exists(ico_path):
                self.dialog.iconbitmap(ico_path)
        except Exception:
            pass
        
        self.create_widgets()
    
    def create_widgets(self) -> None:
        a = self.app
        c = a.colors
        
        notebook = ttk.Notebook(self.dialog)
        notebook.pack(fill="both", expand=True, padx=10, pady=10)
        
        video_tab = ttk.Frame(notebook)
        notebook.add(video_tab, text=a.lang["video_settings"])
        
        video_inner = tk.Frame(video_tab, bg=c["bg"])
        video_inner.pack(fill="both", expand=True, padx=15, pady=15)
        
        quality_frame = tk.Frame(video_inner, bg=c["bg"])
        quality_frame.pack(fill="x", pady=10)
        tk.Label(quality_frame, text=a.lang["quality"], 
                bg=c["bg"], fg=c["text"],
                font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.quality_var = tk.StringVar(value=str(a.quality))
        quality_scale = tk.Scale(quality_frame, from_=10, to=100, 
                                 orient="horizontal", length=250,
                                 variable=self.quality_var, 
                                 command=self.update_quality,
                                 bg=c["surface"], fg=c["text"],
                                 highlightthickness=0, troughcolor=c["surface_light"])
        quality_scale.pack(side="left", padx=5)
        tk.Label(quality_frame, text="%", bg=c["bg"], fg=c["text_secondary"],
                font=("Segoe UI", 10)).pack(side="left")
        
        res_frame = tk.Frame(video_inner, bg=c["bg"])
        res_frame.pack(fill="x", pady=10)
        tk.Label(res_frame, text=a.lang["resolution"], bg=c["bg"], fg=c["text"],
                font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.scale_var = tk.StringVar(value=str(int(a.scale_factor * 100)))
        scale_scale = tk.Scale(res_frame, from_=25, to=100, 
                              orient="horizontal", length=250,
                              variable=self.scale_var,
                              command=self.update_scale,
                              bg=c["surface"], fg=c["text"],
                              highlightthickness=0, troughcolor=c["surface_light"])
        scale_scale.pack(side="left", padx=5)
        tk.Label(res_frame, text="%", bg=c["bg"], fg=c["text_secondary"],
                font=("Segoe UI", 10)).pack(side="left")
        
        mode_frame = tk.Frame(video_inner, bg=c["bg"])
        mode_frame.pack(fill="x", pady=10)
        tk.Label(mode_frame, text=a.lang["mode"], bg=c["bg"], fg=c["text"],
                font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.mode_var = tk.StringVar(value=a.recording_mode)
        mode_combo = ttk.Combobox(mode_frame, textvariable=self.mode_var,
                                  values=["ultra", "turbo", "balanced", "eco"],
                                  width=15, state="readonly", font=("Segoe UI", 10))
        mode_combo.pack(side="left", padx=5)
        mode_combo.bind("<<ComboboxSelected>>", self.on_mode_change)
        
        tk.Label(video_inner, text="Codec and HW Accel settings are in ⚙ Advanced tab.",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI", 9, "italic")).pack(anchor="w", pady=(8, 0))

        lang_tab = ttk.Frame(notebook)
        notebook.add(lang_tab, text=a.lang["language"])
        
        lang_inner = tk.Frame(lang_tab, bg=c["bg"])
        lang_inner.pack(fill="both", expand=True, padx=15, pady=15)
        tk.Label(lang_inner, text="Select language:" if a.current_language == "en" else "Выберите язык:",
                bg=c["bg"], fg=c["text"], font=("Segoe UI", 11, "bold")).pack(anchor="w", pady=10)
        
        self.lang_var = tk.StringVar(value=a.current_language)
        tk.Radiobutton(lang_inner, text="English", variable=self.lang_var, value="en",
                      bg=c["bg"], fg=c["text"], selectcolor=c["surface"],
                      font=("Segoe UI", 10)).pack(anchor="w", pady=2)
        tk.Radiobutton(lang_inner, text="Русский", variable=self.lang_var, value="ru",
                      bg=c["bg"], fg=c["text"], selectcolor=c["surface"],
                      font=("Segoe UI", 10)).pack(anchor="w", pady=2)
        
        adv_tab = ttk.Frame(notebook)
        notebook.add(adv_tab, text=("General" if a.current_language == "en" else "Общие"))
        
        adv_inner = tk.Frame(adv_tab, bg=c["bg"])
        adv_inner.pack(fill="both", expand=True, padx=15, pady=15)
        
        mon_frame = tk.Frame(adv_inner, bg=c["bg"])
        mon_frame.pack(fill="x", pady=10)
        tk.Label(mon_frame, text=a.lang["monitor"], bg=c["bg"], fg=c["text"],
                font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.monitor_var = tk.StringVar(value=str(a.monitor_id))
        monitor_combo = ttk.Combobox(mon_frame, textvariable=self.monitor_var,
                                     values=[str(i) for i in range(1, len(a.sct.monitors))],
                                     width=10, state="readonly", font=("Segoe UI", 10))
        monitor_combo.pack(side="left", padx=5)
        monitor_combo.bind("<<ComboboxSelected>>", self.on_monitor_change)
        
        folder_frame = tk.Frame(adv_inner, bg=c["bg"])
        folder_frame.pack(fill="x", pady=10)
        tk.Label(folder_frame, text=a.lang["output"], bg=c["bg"], fg=c["text"],
                font=("Segoe UI", 10), width=10, anchor="w").pack(side="left")
        self.folder_label = tk.Label(folder_frame, text=os.path.basename(a.output_folder), 
                                     bg=c["surface"], fg=c["accent"],
                                     font=("Consolas", 10), relief="flat", padx=8, pady=4)
        self.folder_label.pack(side="left", padx=5)
        tk.Button(folder_frame, text=a.lang["browse"], command=self.select_folder,
                 bg=c["surface"], fg=c["text"], font=("Segoe UI", 10),
                 relief="flat", padx=12).pack(side="left", padx=5)
        
        features_frame = tk.Frame(adv_inner, bg=c["bg"])
        features_frame.pack(fill="x", pady=10)
        self.countdown_var = tk.BooleanVar(value=a.countdown_var.get())
        tk.Checkbutton(features_frame, text=a.lang["countdown"],
                      variable=self.countdown_var, bg=c["bg"], fg=c["text"],
                      selectcolor=c["surface"], font=("Segoe UI", 10)).pack(anchor="w", pady=2)
        self.timestamp_var = tk.BooleanVar(value=a.timestamp_var.get())
        tk.Checkbutton(features_frame, text=a.lang["timestamp"],
                      variable=self.timestamp_var, bg=c["bg"], fg=c["text"],
                      selectcolor=c["surface"], font=("Segoe UI", 10)).pack(anchor="w", pady=2)
        self.cursor_var = tk.BooleanVar(value=a.cursor_var.get())
        tk.Checkbutton(features_frame, text=a.lang["cursor"],
                      variable=self.cursor_var, bg=c["bg"], fg=c["text"],
                      selectcolor=c["surface"], font=("Segoe UI", 10)).pack(anchor="w", pady=2)
        self.show_summary_var = tk.BooleanVar(value=a.show_summary)
        tk.Checkbutton(features_frame, text=a.lang["notification"],
                      variable=self.show_summary_var, bg=c["bg"], fg=c["text"],
                      selectcolor=c["surface"], font=("Segoe UI", 10)).pack(anchor="w", pady=2)
        self.minimize_tray_var = tk.BooleanVar(value=a.minimize_to_tray.get())
        tk.Checkbutton(features_frame, text=a.lang["minimize_tray"],
                      variable=self.minimize_tray_var, bg=c["bg"], fg=c["text"],
                      selectcolor=c["surface"], font=("Segoe UI", 10)).pack(anchor="w", pady=2)
        
        btn_frame = tk.Frame(self.dialog, bg=c["bg"])
        btn_frame.pack(fill="x", padx=10, pady=10)
        tk.Button(btn_frame, text=a.lang["save"], command=self.save_settings,
                 bg=a.colors["success"], fg=a.colors["bg"],
                 font=("Segoe UI", 10, "bold"), relief="flat", padx=20, pady=8).pack(side="right", padx=5)
        tk.Button(btn_frame, text=a.lang["cancel"], command=self.dialog.destroy,
                 bg=c["surface"], fg=c["text"], font=("Segoe UI", 10),
                 relief="flat", padx=20, pady=8).pack(side="right", padx=5)
    
    def _on_codec_change(self, event=None) -> None:
        codec = self.codec_var.get()
        hints = {
            "libx264":   "CPU · H.264 · universal",
            "libx265":   "CPU · H.265 · smaller files, slower",
            "h264_nvenc":"GPU · H.264 · Nvidia only",
            "hevc_nvenc":"GPU · H.265 · Nvidia only",
            "h264_amf":  "GPU · H.264 · AMD only",
            "hevc_amf":  "GPU · H.265 · AMD only",
            "h264_qsv":  "GPU · H.264 · Intel only",
            "hevc_qsv":  "GPU · H.265 · Intel only",
        }
        self.codec_hint.config(text=hints.get(codec, ""))

    def update_quality(self, event=None) -> None:
        pass
    
    def update_scale(self, event=None) -> None:
        pass
    
    def on_mode_change(self, event=None) -> None:
        pass
    
    def on_monitor_change(self, event=None) -> None:
        pass
    
    def select_folder(self) -> None:
        folder = filedialog.askdirectory(initialdir=self.app.output_folder)
        if folder:
            self.app.output_folder = folder
            self.folder_label.config(text=os.path.basename(folder))
    
    def save_settings(self) -> None:
        new_lang = self.lang_var.get()
        if new_lang != self.app.current_language:
            self.app.current_language = new_lang
            self.app.lang = LANGUAGES[new_lang]
            self.app.update_ui_language()
        
        self.app.quality = int(self.quality_var.get())
        self.app.recording_mode = self.mode_var.get()
        self.app.update_mode_settings()
        self.app.scale_factor = int(self.scale_var.get()) / 100
        self.app.update_monitor_info()
        self.app.monitor_id = int(self.monitor_var.get())
        self.app.update_monitor_info()
        self.app.countdown_var.set(self.countdown_var.get())
        self.app.timestamp_var.set(self.timestamp_var.get())
        self.app.cursor_var.set(self.cursor_var.get())
        self.app.show_summary = self.show_summary_var.get()
        self.app.minimize_to_tray.set(self.minimize_tray_var.get())
        if hasattr(self, "codec_var"):
            self.app.video_codec = self.codec_var.get()
        if hasattr(self, "hw_var"):
            self.app.hw_accel = self.hw_var.get()
        self.app.res_label.config(text=f"{self.app.lang['resolution']} {self.app.record_width}x{self.app.record_height}")
        self.app.save_settings(silent=True)
        self.dialog.destroy()
        messagebox.showinfo(self.app.lang["info"], self.app.lang["settings_saved"])

