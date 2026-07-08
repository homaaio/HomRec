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
import gzip
import shutil
import platform
import webbrowser
import subprocess
import threading
import queue
import ctypes
import logging
from datetime import datetime
import cv2
import numpy as np
from PIL import Image, ImageTk, ImageDraw
import mss

from ..core.optional_deps import (_DND_AVAILABLE, _PYAUDIO_AVAILABLE, _pyaudio_mod,
                                   _audioop_mod, wave, HAS_PSUTIL, HAS_TRAY, pystray, TrayItem)
from ..core.constants import (CURRENT_VERSION, GITHUB_REPO, ASSETS_DIR, THEMES_DIR,
                               LANGS_DIR, SETTINGS_PATH, THEME_REQUIRED_KEYS,
                               LANG_REQUIRED_KEYS, LANG_SCHEMA_VERSION,
                               THEME_SCHEMA_VERSION, _HRC_MAGIC, _HRL_MAGIC, _ROOT_DIR)
from ..core.languages import LANGUAGES
from ..core.profile_io import _hrc_write, _hrc_read, _hrc_detect
from ..core.system_utils import find_ffmpeg, optimize_for_performance, rms_to_level_percent
from ..core.updates import check_for_updates, _version_gt

from ..dialogs.welcome_dialog import WelcomeDialog
from ..dialogs.settings_dialog import SettingsDialog
from ..dialogs.advanced_settings_dialog import AdvancedSettingsDialog
from ..dialogs.overlay_manager import OverlayManagerWindow, OverlayPreviewDialog
from ..dialogs.overlays_dock_panel import OverlaysDockPanel
from ..dialogs.audio_panel import AudioPanel
from ..dialogs.audio_level_meter import AudioLevelMeter
from ..dialogs.custom_messagebox import CustomMessageBox

log = logging.getLogger("homrec")


class AudioMixin:

    def get_dshow_audio_devices(self) -> list[str]:
        if not self.ffmpeg_path: return []
        try:
            from homrec_native import tools_engine as _te, TOOLS_OK as _TOK
            if _TOK and _te: return _te.get_dshow_devices(self.ffmpeg_path)
        except Exception: pass
        return []

    def merge_audio_video(self, video_file: str, audio_file: str) -> bool:
        log.info(f"merge_audio_video: video={video_file!r} audio={audio_file!r}")
        if not audio_file or not os.path.exists(audio_file):
            log.warning(f"merge_audio_video: audio missing: {audio_file!r}"); return False
        if not os.path.exists(video_file):
            log.warning(f"merge_audio_video: video missing: {video_file!r}"); return False
        if not self.ffmpeg_path:
            log.warning("merge_audio_video: no ffmpeg path"); return False

        # BUG FIX: audio_aac_bitrate was a real, saved UI setting (Advanced
        # Settings → Audio) but was never actually passed to ffmpeg — the
        # fallback merge command always used the encoder's default bitrate.
        # Also route custom_ffmpeg_args here for full control over the mux step.
        audio_bitrate = getattr(self, 'audio_aac_bitrate', '192k') or '192k'
        custom_args_str = (getattr(self, 'custom_ffmpeg_args', '') or '').strip()

        # The native merge_av() has no way to accept a custom bitrate/args,
        # so only take that fast path when there's nothing for it to ignore.
        if not custom_args_str and audio_bitrate == '192k':
            try:
                from homrec_native import tools_engine as _te, TOOLS_OK as _TOK
                if _TOK and _te:
                    ok = _te.merge_av(self.ffmpeg_path, video_file, audio_file)
                    if ok:
                        log.info(f"merge_audio_video: C++ success → {video_file}"); return True
            except Exception as e:
                log.warning(f"merge_audio_video: C++ path error: {e}")
        ext = os.path.splitext(video_file)[1] or '.mp4'
        tmp = video_file.replace(ext, f'_merge_tmp{ext}')
        try:
            cmd = [self.ffmpeg_path, '-i', video_file, '-i', audio_file,
                   '-c:v', 'copy', '-c:a', 'aac', '-b:a', audio_bitrate,
                   '-af', 'aresample=async=1000', '-map', '0:v:0', '-map', '1:a:0',
                   '-shortest']
            if custom_args_str:
                try:
                    import shlex
                    cmd += shlex.split(custom_args_str)
                except Exception as e:
                    log.warning(f"Invalid custom ffmpeg args, ignoring: {e}")
            cmd += ['-y', tmp]
            result = subprocess.run(cmd, capture_output=True, timeout=120, creationflags=subprocess.CREATE_NO_WINDOW if platform.system()=='Windows' else 0)
            if result.returncode == 0 and os.path.exists(tmp):
                os.remove(video_file); os.remove(audio_file); os.rename(tmp, video_file)
                log.info(f"merge_audio_video: fallback success → {video_file}"); return True
        except Exception as e:
            log.warning(f"merge_audio_video: fallback exception: {e}")
        return False

    def get_audio_channels(self) -> int:
        try:
            from homrec_native import AUDIO_OK
            if AUDIO_OK: return 2
        except Exception: pass
        if not _PYAUDIO_AVAILABLE: return 2
        try:
            p = _pyaudio_mod.PyAudio()
            try:
                for ch in (2, 1):
                    try:
                        s = p.open(format=_pyaudio_mod.paInt16, channels=ch, rate=44100, input=True, frames_per_buffer=1024)
                        s.close(); return ch
                    except Exception: pass
                return 1
            finally:
                try: p.terminate()
                except: pass
        except Exception: return 2

    @staticmethod
    def _pyaudio_supports_loopback(p) -> bool:
        try:
            p.open(format=_pyaudio_mod.paInt16, channels=1, rate=44100, input=True, input_device_index=99999, frames_per_buffer=512, as_loopback=True)
        except TypeError: return False
        except Exception: return True
        return True

    def _find_wasapi_loopback(self, p, require_input: bool = False):
        if sys.platform != 'win32': return None
        try: wasapi_info = p.get_host_api_info_by_type(_pyaudio_mod.paWASAPI)
        except OSError: log.warning("WASAPI not available"); return None

        default_out_idx = first_wasapi_out_idx = None
        wasapi_default_dev = wasapi_info.get('defaultOutputDevice', -1)

        for i in range(p.get_device_count()):
            try: dev = p.get_device_info_by_index(i)
            except Exception: continue
            if dev.get('hostApi') != wasapi_info['index']: continue
            name = dev.get('name', '').lower()
            if dev.get('maxInputChannels', 0) >= 1:
                if any(k in name for k in ('loopback','stereo mix','what u hear','стерео микшер','что слышит')):
                    return i
            if not require_input and dev.get('maxOutputChannels', 0) >= 1:
                if default_out_idx is None and wasapi_default_dev >= 0 and i == wasapi_default_dev:
                    default_out_idx = i
                if first_wasapi_out_idx is None:
                    first_wasapi_out_idx = i

        chosen = default_out_idx if default_out_idx is not None else first_wasapi_out_idx
        if chosen is not None: return chosen
        log.warning("No WASAPI loopback device found"); return None

    def _notify_recording_start(self) -> None:
        if getattr(self, 'notify_flash', True):
            orig_bg = self.root.cget("bg")
            def _flash(n=0):
                if n >= 6: self.root.configure(bg=orig_bg); return
                self.root.configure(bg=self.colors.get("error","#f38ba8") if n % 2 == 0 else orig_bg)
                self.root.after(120, lambda: _flash(n + 1))
            _flash()
        if getattr(self, 'notify_sound', True):
            try: import winsound; winsound.MessageBeep(winsound.MB_OK)
            except Exception: pass

    def start_audio_recording(self) -> None:
        self.audio_thread = None; self.audio_frames = []; self.sys_audio_frames = []
        self.sys_audio_recording = False; self.sys_audio_filename = None; self.sys_ffmpeg_proc = None
        try:
            from homrec_native import audio_engine as _ae, AUDIO_OK as _AOK
        except Exception:
            _ae = None; _AOK = False
        if not (_AOK and _ae):
            log.warning("hr_audio.dll not available — audio recording disabled")
            self.audio_recording = False; self._using_cpp_audio = False; return

        mic_vol = self.audio_panel.mic_volume.get() / 100.0
        sys_vol = self.audio_panel.sys_volume.get() / 100.0
        flags = _ae.start(mic_vol, sys_vol, self.audio_panel.mic_mute.get(), self.audio_panel.sys_mute.get())
        mic_ok = bool(flags & 0x1); sys_ok = bool(flags & 0x2)
        log.info(f"C++ AudioEngine: mic={'OK' if mic_ok else 'FAIL'} sys={'OK' if sys_ok else 'FAIL'}")
        self._using_cpp_audio = mic_ok or sys_ok
        self.audio_recording = mic_ok; self.sys_audio_recording = sys_ok; self.audio_channels = 2

        if self._using_cpp_audio:
            def _cpp_vu_poll():
                while getattr(self, '_using_cpp_audio', False) and (self.audio_recording or self.sys_audio_recording):
                    m, s = _ae.get_levels()
                    self.audio_panel.update_mic_level(m); self.audio_panel.update_sys_level(s)
                    time.sleep(0.05)
            threading.Thread(target=_cpp_vu_poll, daemon=True).start()
        else:
            log.warning("C++ AudioEngine: no streams opened")
            self.audio_recording = False

    def stop_audio_recording(self) -> str | None:
        if not getattr(self, '_using_cpp_audio', False):
            self.audio_recording = False; self.sys_audio_recording = False; return None
        self._using_cpp_audio = False; self.audio_recording = False; self.sys_audio_recording = False
        try:
            from homrec_native import audio_engine as _ae, AUDIO_OK as _AOK
        except Exception:
            _ae = None; _AOK = False
        if not (_AOK and _ae): return None

        _base = os.path.splitext(self.filename)[0]
        audio_filename = _base + '_audio.wav'
        mic_wav = _base + '_mic_tmp.wav'
        sys_wav = _base + '_sys.wav'
        has_mic = not self.audio_panel.mic_mute.get()
        has_sys = not self.audio_panel.sys_mute.get()
        flags = _ae.stop(mic_wav if has_mic else None, sys_wav if has_sys else None)
        mic_written = bool(flags & 0x1); sys_written = bool(flags & 0x2)
        log.info(f"C++ AudioEngine stopped: mic={mic_written} sys={sys_written}")

        if mic_written and sys_written:
            if _ae.mix_wav(mic_wav, sys_wav, audio_filename):
                for f in (mic_wav, sys_wav):
                    try: os.remove(f)
                    except: pass
                return audio_filename
            try: os.rename(mic_wav, audio_filename)
            except: pass
            return audio_filename
        if mic_written:
            try: os.rename(mic_wav, audio_filename)
            except: pass
            return audio_filename
        if sys_written:
            try: os.rename(sys_wav, audio_filename)
            except: pass
            return audio_filename
        return None

