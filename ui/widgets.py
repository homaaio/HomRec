"""ui/widgets.py — Reusable UI widgets"""
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

class AudioLevelMeter(tk.Canvas):
    def __init__(self, parent, width: int = 180, height: int = 24, **kwargs) -> None:
        super().__init__(parent, width=width, height=height, highlightthickness=0, **kwargs)
        self.width = width
        self.height = height
        self.level = 0
        self.draw_meter()
    
    def draw_meter(self) -> None:
        self.delete("all")
        self.create_rectangle(0, 0, self.width, self.height, fill='#45475a', outline='')
        bar_width = int((self.level / 100) * (self.width - 4))
        if bar_width > 0:
            color = '#a6e3a1' if self.level < 70 else '#f9e2af' if self.level < 90 else '#f38ba8'
            self.create_rectangle(2, 2, bar_width, self.height-2, fill=color, outline='')
        for i in range(0, 101, 20):
            x = int((i / 100) * self.width)
            self.create_line(x, 0, x, self.height, fill='#1e1e2e', width=1)
    
    def set_level(self, level: int) -> None:
        self.level = max(0, min(100, level))
        self.draw_meter()

class CustomMessageBox:
    @staticmethod
    def show(app, title_key: str, message_key: str, info_text: str, dont_show_var: tk.BooleanVar) -> bool:
        dialog = tk.Toplevel()
        dialog.title(app.lang[title_key])
        dialog.geometry("500x400")
        dialog.configure(bg=app.colors["bg"])
        dialog.transient()
        dialog.grab_set()
        dialog.resizable(False, False)
        
        dialog.update_idletasks()
        x = (dialog.winfo_screenwidth() // 2) - 250
        y = (dialog.winfo_screenheight() // 2) - 200
        dialog.geometry(f"+{x}+{y}")
        
        icon_label = tk.Label(dialog, text="✅", font=("Segoe UI", 48), bg=app.colors["bg"], fg='#a6e3a1')
        icon_label.pack(pady=(20, 10))
        
        tk.Label(dialog, text=app.lang[message_key], font=("Segoe UI", 14, "bold"),
                bg=app.colors["bg"], fg=app.colors["fg"]).pack(pady=(0, 10))
        
        info_frame = tk.Frame(dialog, bg=app.colors["surface"], relief='flat', bd=2)
        info_frame.pack(pady=10, padx=20, fill='both', expand=True)
        info_label = tk.Label(info_frame, text=info_text, justify='left',
                              bg=app.colors["surface"], fg=app.colors["text"],
                              font=("Consolas", 10))
        info_label.pack(pady=15, padx=15)
        
        check_frame = tk.Frame(dialog, bg=app.colors["bg"])
        check_frame.pack(pady=10)
        dont_show_text = "Don't show again" if app.current_language == "en" else "Больше не показывать"
        dont_show_check = tk.Checkbutton(check_frame, text=dont_show_text,
                                         variable=dont_show_var,
                                         bg=app.colors["bg"], fg=app.colors["fg"],
                                         selectcolor=app.colors["surface"],
                                         font=("Segoe UI", 9))
        dont_show_check.pack()
        
        btn_frame = tk.Frame(dialog, bg=app.colors["bg"])
        btn_frame.pack(pady=15)
        result = {'value': False}
        
        def on_yes() -> None:
            result['value'] = True
            dialog.destroy()
        
        def on_no() -> None:
            result['value'] = False
            dialog.destroy()
        
        tk.Button(btn_frame, text=app.lang["open_folder"], command=on_yes,
                  bg='#a6e3a1', fg=app.colors["bg"],
                  font=("Segoe UI", 10, "bold"),
                  relief='flat', padx=20, pady=8, width=12).pack(side='left', padx=5)
        
        tk.Button(btn_frame, text=app.lang["cancel"], command=on_no,
                  bg=app.colors["surface"], fg=app.colors["text"],
                  font=("Segoe UI", 10),
                  relief='flat', padx=20, pady=8, width=12).pack(side='left', padx=5)
        
        dialog.wait_window()
        return result['value']

class AudioPanel:
    def __init__(self, parent, app) -> None:
        self.app = app
        self.frame = tk.LabelFrame(parent, text=app.lang["audio_mixer"], 
                                   bg=app.colors["surface"], fg=app.colors["accent"],
                                   font=("Segoe UI", 11, "bold"),
                                   padx=10, pady=10)
        self.frame.pack(side='left', fill='both', expand=True, padx=(5, 0))
        
        self.audio_enabled = tk.BooleanVar(value=True)
        self.mic_mute = tk.BooleanVar(value=False)
        self.sys_mute = tk.BooleanVar(value=False)
        self.audio_stream = None
        self.audio_p = None
        
        self.create_mixer_layout()
    
    def create_mic_section(self) -> None:
        pass  # built inside create_mixer_layout

    def create_system_section(self) -> None:
        pass  # built inside create_mixer_layout

    def create_devices_section(self) -> None:
        pass  # built inside create_mixer_layout

    def create_mixer_layout(self) -> None:
        """Horizontal layout: Mic on left, Desktop Audio on right, controls at bottom."""
        c = self.app.colors

        # -- top row: two channel strips side by side ----------------------
        channels = tk.Frame(self.frame, bg=c["surface"])
        channels.pack(fill='x', pady=(0, 4))

        # Mic strip
        mic_strip = tk.Frame(channels, bg=c["surface"],
                             relief='flat', bd=0)
        mic_strip.pack(side='left', fill='both', expand=True, padx=(0, 8))

        mic_header = tk.Frame(mic_strip, bg=c["surface"])
        mic_header.pack(fill='x')
        tk.Label(mic_header, text=self.app.lang["microphone"],
                 bg=c["surface"], fg='#a6e3a1',
                 font=("Segoe UI", 9, 'bold')).pack(side='left')
        self.mic_mute_btn = tk.Button(mic_header, text=self.app.lang["mute"],
                                      command=self.toggle_mic_mute,
                                      bg=c["surface_light"], fg=c["text"],
                                      font=("Segoe UI", 8), relief='flat',
                                      width=5, cursor='hand2')
        self.mic_mute_btn.pack(side='right')

        mic_vol_row = tk.Frame(mic_strip, bg=c["surface"])
        mic_vol_row.pack(fill='x', pady=2)
        tk.Label(mic_vol_row, text=self.app.lang["vol"],
                 bg=c["surface"], fg=c["text"],
                 font=("Segoe UI", 8)).pack(side='left')
        self.mic_volume = tk.Scale(mic_vol_row, from_=0, to=100,
                                   orient='horizontal', length=110,
                                   bg=c["surface_light"], fg=c["text"],
                                   highlightthickness=0,
                                   troughcolor=c["surface"],
                                   command=self.on_mic_volume_change,
                                   showvalue=False)
        self.mic_volume.set(80)
        self.mic_volume.pack(side='left', padx=4)
        self.mic_volume_label = tk.Label(mic_vol_row, text="80%",
                                         bg=c["surface"], fg='#a6e3a1',
                                         font=("Segoe UI", 8, 'bold'), width=4)
        self.mic_volume_label.pack(side='left')

        mic_meter_row = tk.Frame(mic_strip, bg=c["surface"])
        mic_meter_row.pack(fill='x', pady=2)
        tk.Label(mic_meter_row, text=self.app.lang["level"],
                 bg=c["surface"], fg=c["text"],
                 font=("Segoe UI", 8)).pack(side='left')
        self.mic_meter = AudioLevelMeter(mic_meter_row, width=130, height=14,
                                         bg=c["surface"])
        self.mic_meter.pack(side='left', padx=4)

        # Divider
        tk.Frame(channels, bg=c["surface_light"], width=1).pack(side='left', fill='y', padx=4)

        # System audio strip
        sys_strip = tk.Frame(channels, bg=c["surface"])
        sys_strip.pack(side='left', fill='both', expand=True, padx=(8, 0))

        sys_header = tk.Frame(sys_strip, bg=c["surface"])
        sys_header.pack(fill='x')
        tk.Label(sys_header, text=self.app.lang["desktop_audio"],
                 bg=c["surface"], fg='#89b4fa',
                 font=("Segoe UI", 9, 'bold')).pack(side='left')
        self.sys_mute_btn = tk.Button(sys_header, text=self.app.lang["mute"],
                                      command=self.toggle_sys_mute,
                                      bg=c["surface_light"], fg=c["text"],
                                      font=("Segoe UI", 8), relief='flat',
                                      width=5, cursor='hand2')
        self.sys_mute_btn.pack(side='right')

        sys_vol_row = tk.Frame(sys_strip, bg=c["surface"])
        sys_vol_row.pack(fill='x', pady=2)
        tk.Label(sys_vol_row, text=self.app.lang["vol"],
                 bg=c["surface"], fg=c["text"],
                 font=("Segoe UI", 8)).pack(side='left')
        self.sys_volume = tk.Scale(sys_vol_row, from_=0, to=100,
                                   orient='horizontal', length=110,
                                   bg=c["surface_light"], fg=c["text"],
                                   highlightthickness=0,
                                   troughcolor=c["surface"],
                                   command=self.on_sys_volume_change,
                                   showvalue=False)
        self.sys_volume.set(50)
        self.sys_volume.pack(side='left', padx=4)
        self.sys_volume_label = tk.Label(sys_vol_row, text="50%",
                                          bg=c["surface"], fg='#89b4fa',
                                          font=("Segoe UI", 8, 'bold'), width=4)
        self.sys_volume_label.pack(side='left')

        sys_meter_row = tk.Frame(sys_strip, bg=c["surface"])
        sys_meter_row.pack(fill='x', pady=2)
        tk.Label(sys_meter_row, text=self.app.lang["level"],
                 bg=c["surface"], fg=c["text"],
                 font=("Segoe UI", 8)).pack(side='left')
        self.sys_meter = AudioLevelMeter(sys_meter_row, width=130, height=14,
                                          bg=c["surface"])
        self.sys_meter.pack(side='left', padx=4)

        # -- bottom row: enable audio + ffmpeg status -----------------------
        bottom = tk.Frame(self.frame, bg=c["surface"])
        bottom.pack(fill='x', pady=(4, 0))

        self.audio_check = tk.Checkbutton(bottom, text=self.app.lang["enable_audio"],
                                          variable=self.audio_enabled,
                                          bg=c["surface"], fg=c["text"],
                                          selectcolor=c["surface_light"],
                                          font=("Segoe UI", 9, 'bold'))
        self.audio_check.pack(side='left')

        ffmpeg_text = self.app.lang["ffmpeg_found"] if self.app.check_ffmpeg() else self.app.lang["ffmpeg_not_found"]
        ffmpeg_color = '#a6e3a1' if self.app.check_ffmpeg() else '#f38ba8'
        self.ffmpeg_label = tk.Label(bottom, text=ffmpeg_text,
                                      bg=c["surface"], fg=ffmpeg_color,
                                      font=("Segoe UI", 8))
        self.ffmpeg_label.pack(side='right')
    
    def on_mic_volume_change(self, value: str) -> None:
        self.mic_volume_label.config(text=f"{int(float(value))}%")
    
    def on_sys_volume_change(self, value: str) -> None:
        self.sys_volume_label.config(text=f"{int(float(value))}%")
    
    def toggle_mic_mute(self) -> None:
        self.mic_mute.set(not self.mic_mute.get())
        self.mic_mute_btn.config(bg='#f38ba8' if self.mic_mute.get() else self.app.colors["surface_light"],
                                 text=self.app.lang["unmute"] if self.mic_mute.get() else self.app.lang["mute"])
    
    def toggle_sys_mute(self) -> None:
        self.sys_mute.set(not self.sys_mute.get())
        self.sys_mute_btn.config(bg='#f38ba8' if self.sys_mute.get() else self.app.colors["surface_light"],
                                 text=self.app.lang["unmute"] if self.sys_mute.get() else self.app.lang["mute"])
    
    def update_mic_level(self, level: int) -> None:
        self.mic_meter.set_level(level)
    
    def update_sys_level(self, level: int) -> None:
        self.sys_meter.set_level(level)
    
    def update_language(self) -> None:
        self.frame.config(text=self.app.lang["audio_mixer"])
        self.ffmpeg_label.config(
            text=self.app.lang["ffmpeg_found"] if self.app.check_ffmpeg() else self.app.lang["ffmpeg_not_found"]
        )
        self.audio_check.config(text=self.app.lang["enable_audio"])
        self.mic_mute_btn.config(text=self.app.lang["unmute"] if self.mic_mute.get() else self.app.lang["mute"])
        self.sys_mute_btn.config(text=self.app.lang["unmute"] if self.sys_mute.get() else self.app.lang["mute"])


# -- Schema versioning --------------------------------------------------------
# Bump LANG_SCHEMA_VERSION when new language keys are added
# Bump THEME_SCHEMA_VERSION when new theme color keys are added
