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


from .audio_level_meter import AudioLevelMeter
from ..core.system_utils import rms_to_level_percent


class AudioPanel:
    def __init__(self, parent, app) -> None:
        self.app = app
        c = app.colors
        title_bar = tk.Frame(parent, bg=c["surface"])
        self._title_lbl = tk.Label(title_bar, text=app.lang["audio_mixer"], bg=c["surface"], fg=c["accent"],
                 font=("Segoe UI", 11, "bold"))
        self._title_lbl.pack(side="left")
        tk.Button(title_bar, text="✕", command=self._close_panel,
                  bg=c["surface"], fg=c["text_secondary"], font=("Segoe UI", 9),
                  relief="flat", width=3, cursor="hand2").pack(side="right")
        self.frame = tk.LabelFrame(parent, labelwidget=title_bar,
                                   bg=c["surface"], fg=c["accent"],
                                   font=("Segoe UI", 11, "bold"), padx=10, pady=10)
        self.frame.pack(fill='both', expand=True, padx=5, pady=5)
        self.audio_enabled = tk.BooleanVar(value=True)
        self.mic_mute = tk.BooleanVar(value=False)
        self.sys_mute = tk.BooleanVar(value=False)
        self.audio_stream = None
        self.audio_p = None
        self._mic_level_pending: int = 0
        self._sys_level_pending: int = 0
        self._mic_vol_cached: float = 0.80
        self._sys_vol_cached: float = 1.0
        self._mic_mute_cached: bool = False
        self._sys_mute_cached: bool = False
        self.create_mixer_layout()

    def create_mic_section(self) -> None: pass
    def create_system_section(self) -> None: pass
    def create_devices_section(self) -> None: pass

    def create_mixer_layout(self) -> None:
        c = self.app.colors
        channels = tk.Frame(self.frame, bg=c["surface"])
        channels.pack(fill='x', pady=(0, 4))

        mic_strip = tk.Frame(channels, bg=c["surface"], relief='flat', bd=0)
        mic_strip.pack(side='left', fill='both', expand=True, padx=(0, 8))
        mic_header = tk.Frame(mic_strip, bg=c["surface"])
        mic_header.pack(fill='x')
        tk.Label(mic_header, text=self.app.lang["microphone"], bg=c["surface"], fg='#a6e3a1', font=("Segoe UI", 9, 'bold')).pack(side='left')
        self.mic_mute_btn = tk.Button(mic_header, text=self.app.lang["mute"], command=self.toggle_mic_mute,
                                      bg=c["surface_light"], fg=c["text"], font=("Segoe UI", 8), relief='flat', width=5, cursor='hand2')
        self.mic_mute_btn.pack(side='right')
        mic_vol_row = tk.Frame(mic_strip, bg=c["surface"])
        mic_vol_row.pack(fill='x', pady=2)
        tk.Label(mic_vol_row, text=self.app.lang["vol"], bg=c["surface"], fg=c["text"], font=("Segoe UI", 8)).pack(side='left')
        self.mic_volume = tk.Scale(mic_vol_row, from_=0, to=100, orient='horizontal', length=110,
                                   bg=c["surface_light"], fg=c["text"], highlightthickness=0,
                                   troughcolor=c["surface"], command=self.on_mic_volume_change, showvalue=False)
        self.mic_volume.set(80)
        self.mic_volume.pack(side='left', padx=4)
        self.mic_volume_label = tk.Label(mic_vol_row, text="80%", bg=c["surface"], fg='#a6e3a1', font=("Segoe UI", 8, 'bold'), width=4)
        self.mic_volume_label.pack(side='left')
        mic_meter_row = tk.Frame(mic_strip, bg=c["surface"])
        mic_meter_row.pack(fill='x', pady=2)
        tk.Label(mic_meter_row, text=self.app.lang["level"], bg=c["surface"], fg=c["text"], font=("Segoe UI", 8)).pack(side='left')
        self.mic_meter = AudioLevelMeter(mic_meter_row, width=130, height=14, bg=c["surface"])
        self.mic_meter.pack(side='left', padx=4)

        tk.Frame(channels, bg=c["surface_light"], width=1).pack(side='left', fill='y', padx=4)

        sys_strip = tk.Frame(channels, bg=c["surface"])
        sys_strip.pack(side='left', fill='both', expand=True, padx=(8, 0))
        sys_header = tk.Frame(sys_strip, bg=c["surface"])
        sys_header.pack(fill='x')
        tk.Label(sys_header, text=self.app.lang["desktop_audio"], bg=c["surface"], fg='#89b4fa', font=("Segoe UI", 9, 'bold')).pack(side='left')
        self.sys_mute_btn = tk.Button(sys_header, text=self.app.lang["mute"], command=self.toggle_sys_mute,
                                      bg=c["surface_light"], fg=c["text"], font=("Segoe UI", 8), relief='flat', width=5, cursor='hand2')
        self.sys_mute_btn.pack(side='right')
        sys_vol_row = tk.Frame(sys_strip, bg=c["surface"])
        sys_vol_row.pack(fill='x', pady=2)
        tk.Label(sys_vol_row, text=self.app.lang["vol"], bg=c["surface"], fg=c["text"], font=("Segoe UI", 8)).pack(side='left')
        self.sys_volume = tk.Scale(sys_vol_row, from_=0, to=100, orient='horizontal', length=110,
                                   bg=c["surface_light"], fg=c["text"], highlightthickness=0,
                                   troughcolor=c["surface"], command=self.on_sys_volume_change, showvalue=False)
        self.sys_volume.set(100)
        self.sys_volume.pack(side='left', padx=4)
        self.sys_volume_label = tk.Label(sys_vol_row, text="100%", bg=c["surface"], fg='#89b4fa', font=("Segoe UI", 8, 'bold'), width=4)
        self.sys_volume_label.pack(side='left')
        sys_meter_row = tk.Frame(sys_strip, bg=c["surface"])
        sys_meter_row.pack(fill='x', pady=2)
        tk.Label(sys_meter_row, text=self.app.lang["level"], bg=c["surface"], fg=c["text"], font=("Segoe UI", 8)).pack(side='left')
        self.sys_meter = AudioLevelMeter(sys_meter_row, width=130, height=14, bg=c["surface"])
        self.sys_meter.pack(side='left', padx=4)

        bottom = tk.Frame(self.frame, bg=c["surface"])
        bottom.pack(fill='x', pady=(4, 0))
        self.audio_check = tk.Checkbutton(bottom, text=self.app.lang["enable_audio"],
                                          variable=self.audio_enabled, bg=c["surface"], fg=c["text"],
                                          selectcolor=c["surface_light"], font=("Segoe UI", 9, 'bold'))
        self.audio_check.pack(side='left')
        ffmpeg_ok = self.app.check_ffmpeg()
        self.ffmpeg_label = tk.Label(bottom, text=self.app.lang["ffmpeg_found" if ffmpeg_ok else "ffmpeg_not_found"],
                                      bg=c["surface"], fg='#a6e3a1' if ffmpeg_ok else '#f38ba8', font=("Segoe UI", 8))
        self.ffmpeg_label.pack(side='right')

        dyn_row = tk.Frame(self.frame, bg=c["surface"])
        dyn_row.pack(fill='x', pady=(2, 0))
        self._meter_enabled_var = tk.BooleanVar(value=True)
        tk.Checkbutton(dyn_row, text="VU meter",
                       variable=self._meter_enabled_var,
                       command=self._on_meter_toggle,
                       bg=c["surface"], fg=c["text"],
                       selectcolor=c["surface_light"],
                       font=("Segoe UI", 8)).pack(side='left')
        tk.Label(dyn_row, text="dynamics:", bg=c["surface"], fg=c["text_secondary"],
                 font=("Segoe UI", 7)).pack(side='left', padx=(6, 2))
        self._dynamics_var = tk.IntVar(value=5)
        dyn_scale = tk.Scale(dyn_row, from_=0, to=10, orient='horizontal', length=80,
                             variable=self._dynamics_var, showvalue=False,
                             bg=c["surface_light"], fg=c["text"],
                             highlightthickness=0, troughcolor=c["surface"],
                             command=self._on_dynamics_change)
        dyn_scale.pack(side='left')
        self._dyn_val_lbl = tk.Label(dyn_row, text="5", width=2,
                                      bg=c["surface"], fg=c["accent"],
                                      font=("Segoe UI", 7, "bold"))
        self._dyn_val_lbl.pack(side='left')

        self.app.root.after(100, self._poll_audio_levels)
        self.app.root.after(200, self._start_idle_monitor)

    def _on_meter_toggle(self) -> None:
        enabled = self._meter_enabled_var.get()
        for meter in (self.mic_meter, self.sys_meter):
            meter.enabled = enabled
            if not enabled:
                meter.level = 0.0; meter._peak = 0.0
                meter.draw_meter()

    def _on_dynamics_change(self, value=None) -> None:
        d = self._dynamics_var.get()
        self._dyn_val_lbl.config(text=str(d))
        for meter in (self.mic_meter, self.sys_meter):
            meter.dynamics = d

    def on_mic_volume_change(self, value: str) -> None:
        self.mic_volume_label.config(text=f"{int(float(value))}%")
        self.app.save_settings_debounced()

    def on_sys_volume_change(self, value: str) -> None:
        self.sys_volume_label.config(text=f"{int(float(value))}%")
        self.app.save_settings_debounced()

    def toggle_mic_mute(self) -> None:
        self.mic_mute.set(not self.mic_mute.get())
        self.mic_mute_btn.config(bg='#f38ba8' if self.mic_mute.get() else self.app.colors["surface_light"],
                                 text=self.app.lang["unmute" if self.mic_mute.get() else "mute"])

    def toggle_sys_mute(self) -> None:
        self.sys_mute.set(not self.sys_mute.get())
        self.sys_mute_btn.config(bg='#f38ba8' if self.sys_mute.get() else self.app.colors["surface_light"],
                                 text=self.app.lang["unmute" if self.sys_mute.get() else "mute"])

    def update_mic_level(self, level: int) -> None:
        self._mic_level_pending = level

    def update_sys_level(self, level: int) -> None:
        self._sys_level_pending = level

    def _poll_audio_levels(self) -> None:
        try:
            self.mic_meter.set_level(self._mic_level_pending)
            self.sys_meter.set_level(self._sys_level_pending)
            self._mic_vol_cached = self.mic_volume.get() / 100.0
            self._sys_vol_cached = self.sys_volume.get() / 100.0
            self._mic_mute_cached = self.mic_mute.get()
            self._sys_mute_cached = self.sys_mute.get()
            if getattr(self.app, '_using_cpp_audio', False):
                try:
                    from homrec_native import audio_engine as _ae
                    if _ae:
                        _ae.set_volumes(self._mic_vol_cached, self._sys_vol_cached, self._mic_mute_cached, self._sys_mute_cached)
                except Exception:
                    pass
        except Exception:
            pass
        try:
            self.app.root.after(100, self._poll_audio_levels)
        except Exception:
            pass

    def _start_idle_monitor(self) -> None:
        self._idle_monitor_active = True
        self._idle_monitor_thread = threading.Thread(target=self._idle_monitor_worker, daemon=True)
        self._idle_monitor_thread.start()

    def stop_idle_monitor(self) -> None:
        self._idle_monitor_active = False

    def resume_idle_monitor(self) -> None:
        self._idle_monitor_active = True
        if not getattr(self, '_idle_monitor_thread', None) or not self._idle_monitor_thread.is_alive():
            self._idle_monitor_thread = threading.Thread(target=self._idle_monitor_worker, daemon=True)
            self._idle_monitor_thread.start()

    def _idle_monitor_worker(self) -> None:
        import time as _t
        try:
            from homrec_native import audio_engine as _ae, AUDIO_OK as _AOK
        except Exception:
            _ae = None; _AOK = False

        if _AOK and _ae is not None:
            flags = _ae.start(1.0, 1.0, False, False)
            if bool(flags & 0x1):
                try:
                    while self._idle_monitor_active:
                        if getattr(self.app, 'audio_recording', False):
                            _ae.stop(None, None)
                            while getattr(self.app, 'audio_recording', False) and self._idle_monitor_active:
                                _t.sleep(0.1)
                            if not self._idle_monitor_active: return
                            _ae.start(1.0, 1.0, False, False)
                            continue
                        m, _ = _ae.get_levels()
                        self._mic_level_pending = m
                        _t.sleep(0.05)
                finally:
                    try: _ae.stop(None, None)
                    except Exception: pass
                return

        if not _PYAUDIO_AVAILABLE: return
        p = stream = None
        try:
            p = _pyaudio_mod.PyAudio()
            dev_info = p.get_default_input_device_info()
            ch = min(2, max(1, int(dev_info.get('maxInputChannels', 1))))
            stream = p.open(format=_pyaudio_mod.paInt16, channels=ch, rate=44100,
                            input=True, input_device_index=dev_info.get('index', 0), frames_per_buffer=1024)
            while self._idle_monitor_active:
                if getattr(self.app, 'audio_recording', False):
                    try: stream.read(1024, exception_on_overflow=False)
                    except Exception: pass
                    _t.sleep(0.05); continue
                try:
                    data = stream.read(1024, exception_on_overflow=False)
                    raw_rms = _audioop_mod.rms(data, 2)
                    self._mic_level_pending = rms_to_level_percent(raw_rms)
                except Exception:
                    _t.sleep(0.05)
        except Exception as e:
            log.debug(f'idle mic monitor (PyAudio) failed: {e}')
        finally:
            try:
                if stream: stream.stop_stream(); stream.close()
            except Exception: pass
            try:
                if p: p.terminate()
            except Exception: pass

    def update_language(self) -> None:
        self._title_lbl.config(text=self.app.lang["audio_mixer"])
        ffmpeg_ok = self.app.check_ffmpeg()
        self.ffmpeg_label.config(text=self.app.lang["ffmpeg_found" if ffmpeg_ok else "ffmpeg_not_found"])
        self.audio_check.config(text=self.app.lang["enable_audio"])
        self.mic_mute_btn.config(text=self.app.lang["unmute" if self.mic_mute.get() else "mute"])
        self.sys_mute_btn.config(text=self.app.lang["unmute" if self.sys_mute.get() else "mute"])

    def _close_panel(self) -> None:
        self.app.show_audio_panel = False
        self.app.save_settings(silent=True)
        self.app.recreate_widgets()
