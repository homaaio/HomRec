from __future__ import annotations

# -- stdlib ------------------------------------------------------------------
import audioop
import ctypes
import gc
import gzip
import json
import logging
import os
import platform
import queue
import shutil
import subprocess
import sys
import threading
import time
import wave
from datetime import datetime

# -- third-party -------------------------------------------------------------
import tkinter as tk
from tkinter import ttk, messagebox, filedialog

import cv2
import numpy as np
import mss
import pyaudio
from PIL import Image, ImageTk, ImageDraw

try:
    from tkinterdnd2 import DND_FILES, TkinterDnD
    _DND_AVAILABLE = True
except ImportError:
    _DND_AVAILABLE = False

try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    HAS_PSUTIL = False

try:
    import pystray
    from pystray import MenuItem as TrayItem
    HAS_TRAY = True
except ImportError:
    HAS_TRAY = False

# -- native v2 ----------------------------------------------------------------
try:
    from homrec_native_v2 import (
        pipeline_ctl, PIPELINE_OK,
        core as _native_core, NATIVE_OK,
        RINGBUF_OK, ENCODER_OK, STOPWATCH_OK,
        create_yuv_pipe_windows, connect_yuv_pipe_windows,
    )
    _HAVE_NATIVE_V2 = True
except ImportError:
    _HAVE_NATIVE_V2 = False
    PIPELINE_OK = False
    NATIVE_OK = False

    class _FakePipelineCtl:
        available = False
        def configure(self, *a, **kw): return False
        def start(self): return False
        def stop(self): pass
        def destroy(self): pass
        def pause(self, p): pass
        def set_fps(self, f): pass
        def set_monitor(self, *a): pass
        def set_preview_size(self, *a): pass
        def get_preview_image(self): return None
        def stats(self): return (0, 0, 0.0)

    pipeline_ctl = _FakePipelineCtl()

    class _FakeCore:
        def audio_rms_level(self, data): return min(100, int(audioop.rms(data, 2) / 300))
        def bgrx_to_rgb_np(self, bgra, w, h):
            arr = np.frombuffer(bgra, dtype=np.uint8).reshape(h, w, 4)
            return arr[:, :, 2::-1]

    _native_core = _FakeCore()


try:
    from homrec import (
        LANGUAGES, LANG_SCHEMA_VERSION, THEME_SCHEMA_VERSION,
        LANG_REQUIRED_KEYS, THEME_REQUIRED_KEYS,
        ASSETS_DIR, SETTINGS_PATH, THEMES_DIR, LANGS_DIR,
        _HRC_MAGIC, _HRL_MAGIC, _HRT_MAGIC,
        _hrc_write, _hrc_read, _hrc_detect,
        CURRENT_VERSION, GITHUB_REPO,
        find_ffmpeg, check_for_updates, _version_gt,
        AudioLevelMeter, CustomMessageBox, WelcomeDialog,
        AudioPanel, LanguageEditorDialog, ThemeEditorDialog,
        AdvancedSettingsDialog, SettingsDialog,
    )
    _HOMREC_IMPORTED = True
except ImportError:
    _HOMREC_IMPORTED = False
    CURRENT_VERSION = "1.7.0"
    GITHUB_REPO = "homaaio/homrec"
    LANGUAGES = {"en": {"app_title": "HomRec v1.7.0", "ready": "Ready",
                        "recording": "Recording", "start": "▶ START",
                        "stop": "■ STOP", "pause": "⏸ PAUSE", "resume": "▶ RESUME"},
                 "ru": {"app_title": "HomRec v1.7.0", "ready": "Готов",
                        "recording": "Запись", "start": "▶ СТАРТ",
                        "stop": "■ СТОП", "pause": "⏸ ПАУЗА", "resume": "▶ ПРОДОЛЖИТЬ"}}
    SETTINGS_PATH = "homrec_settings.json"
    _HRC_MAGIC = b'HRC\x01'
    _HRL_MAGIC = b'HRL\x01'
    _HRT_MAGIC = b'HRT\x01'


# -- Logging ------------------------------------------------------------------
def _setup_logging():
    log_dir = os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) \
              else os.path.dirname(os.path.abspath(__file__))
    logging.basicConfig(
        level=logging.DEBUG,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
        handlers=[logging.FileHandler(os.path.join(log_dir, "homrec.log"), encoding="utf-8")],
    )

_setup_logging()
log = logging.getLogger("homrec")


# -- Оптимизация процесса -----------------------------------------------------
def optimize_for_performance():
    try:
        import psutil, platform as _plt
        p = psutil.Process()
        if _plt.system() == "Windows":
            p.nice(psutil.HIGH_PRIORITY_CLASS)
        else:
            p.nice(-10)
    except Exception:
        pass
    try:
        cv2.setNumThreads(max(1, (os.cpu_count() or 4) // 4))
        cv2.setUseOptimized(True)
    except Exception:
        pass
    gc.set_threshold(10000, 50, 50)
    try:
        sys.setswitchinterval(0.020)
    except Exception:
        pass
    log.info(f"optimize_for_performance OK | pipeline={PIPELINE_OK} native={NATIVE_OK}")


# ═══════════════════════════════════════════════════════════════════════════
# Основной класс приложения (только ИЗМЕНЁННЫЕ методы)
# ═══════════════════════════════════════════════════════════════════════════

class HomRecApp:
    """
    HomRec v1.7.0 — минимальный «shell» вокруг UI из homrec.py.
    Переопределяет только методы, связанные с захватом и записью.
    UI (create_widgets, меню, диалоги) берётся из HomRecScreen если доступен.
    """

    # -- инициализация ----------------------------------------------------
    def __init__(self, root: tk.Tk):
        self.root = root
        self.current_language = "en"
        self.lang = dict(LANGUAGES.get("en", {}))

        self.root.title(self.lang.get("app_title", "HomRec v1.7.0"))
        self.root.geometry("1300x750")
        self.root.minsize(1200, 650)

        optimize_for_performance()
        self._set_app_icon()

        # -- Цвета / темы --
        self.current_theme = "dark"
        self.colors = self._get_default_colors()

        # -- mss для idle-preview (только когда не идёт запись) --
        self.sct = mss.mss()
        self._preview_queue: queue.Queue = queue.Queue(maxsize=1)
        self._preview_running = True

        # -- Аудио-состояние --
        self.audio_recording = False
        self.audio_thread = None
        self.audio_frames: list = []
        self.audio_stream = None
        self.audio_p = None
        self.audio_channels = 1
        self.sys_audio_recording = False
        self.sys_audio_thread = None
        self.sys_audio_frames: list = []
        self.sys_audio_stream = None
        self.sys_audio_p = None
        self.sys_audio_filename = None
        self.sys_ffmpeg_proc = None

        # -- FFmpeg --
        self.ffmpeg_proc = None
        self.ffmpeg_reader_thread = None
        self.stop_ffmpeg_reader = False

        # -- Настройки --
        self.scale_factor = 0.75
        self.output_folder = "recordings"
        self.quality = 70
        self.target_fps = 30
        self.recording_mode = "turbo"
        self.show_summary = True
        self.hotkey_start_stop = "F9"
        self.hotkey_pause = "F10"
        self.hotkey_fullscreen = "F11"
        self.notify_sound = True
        self.notify_flash = True
        self.auto_save_profile = False
        self.video_codec = "libx264"
        self.hw_accel = "auto"
        self.enc_preset = "ultrafast"
        self.enc_crf = 18
        self.pix_fmt = "yuv420p"
        self.audio_sample_rate = 44100
        self.audio_aac_bitrate = "192k"
        self.audio_out_channels = 2
        self.ui_theme = "dark"
        self.ui_scale = 1.0
        self.ui_font = "Segoe UI"
        self.filename_template = "HomRec_{date}_{time}"
        self.auto_stop_min = 0
        self.replay_buffer_sec = 0
        self.disable_preview = False

        self.always_on_top = tk.BooleanVar(value=False)
        self.minimize_to_tray = tk.BooleanVar(value=True)
        self.language_var = tk.StringVar(value="en")
        self.theme_var = tk.StringVar(value="dark")
        self.countdown_var = tk.BooleanVar(value=True)
        self.timestamp_var = tk.BooleanVar(value=False)
        self.cursor_var = tk.BooleanVar(value=False)

        self.preview_width = 900
        self.preview_height = 500

        self.load_settings()

        # -- Состояние записи --
        self.recording = False
        self.paused = False
        self.out = None
        self.frame_count = 0
        self.start_time = 0.0
        self.recording_thread = None
        self.stop_flag = False
        self.last_frame_time = 0.0
        self.filename = ""

        # -- Монитор --
        self.monitor_id = 1
        self.monitor_left = 0
        self.monitor_top = 0
        self.update_monitor_info()

        self.capture_mode = "desktop"
        self.capture_window_title = ""
        self.tray_icon = None

        os.makedirs(self.output_folder, exist_ok=True)

        self.ffmpeg_path = find_ffmpeg() if _HOMREC_IMPORTED else shutil.which("ffmpeg")
        log.info(f"FFmpeg: {self.ffmpeg_path}")
        log.info(f"Pipeline: {PIPELINE_OK} | Native: {NATIVE_OK}")

        # -- Строим UI --
        self._apply_theme()
        self.create_menu()
        self.create_widgets()

        # -- Запускаем idle-capture поток (превью когда не записываем) --
        self._capture_thread = threading.Thread(target=self._capture_loop, daemon=True)
        self._capture_thread.start()
        self.update_preview()

        self.root.after(500, self._warm_up_gpu_probe)
        self.root.bind('<Configure>', self._on_window_resize)
        self._apply_hotkeys()
        self._setup_drag_drop()

        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)
        self.setup_tray()
        log.info("HomRec v1.7.0 started")

        self.root.after(2000, self._start_update_check)
        if getattr(self, '_first_launch', False):
            self.root.after(400, self._show_welcome_and_save)

    # -- Цвета (упрощённые, если homrec.py не импортирован) --------------
    BUILTIN_THEMES = {
        "dark": {
            "bg": "#1e1e2e", "fg": "#cdd6f4", "accent": "#89b4fa",
            "success": "#a6e3a1", "warning": "#f9e2af", "error": "#f38ba8",
            "surface": "#313244", "surface_light": "#45475a",
            "preview_bg": "#11111b", "text": "#cdd6f4", "text_secondary": "#a6adc8",
        },
        "light": {
            "bg": "#f5f5f5", "fg": "#2c3e50", "accent": "#3498db",
            "success": "#27ae60", "warning": "#f39c12", "error": "#e74c3c",
            "surface": "#ecf0f1", "surface_light": "#bdc3c7",
            "preview_bg": "#ffffff", "text": "#2c3e50", "text_secondary": "#7f8c8d",
        },
    }

    def _get_default_colors(self):
        return dict(self.BUILTIN_THEMES["dark"])

    def get_theme_colors(self, theme: str) -> dict:
        return self.BUILTIN_THEMES.get(theme, self.BUILTIN_THEMES["dark"])

    def _apply_theme(self):
        self.root.configure(bg=self.colors["bg"])
        style = ttk.Style()
        style.theme_use('clam')
        style.configure("TFrame", background=self.colors["bg"])
        style.configure("TLabel", background=self.colors["bg"], foreground=self.colors["fg"])

    def apply_theme(self):
        self._apply_theme()

    # -- Монитор ----------------------------------------------------------
    def update_monitor_info(self):
        if self.monitor_id < len(self.sct.monitors):
            m = self.sct.monitors[self.monitor_id]
            self.monitor = m
            self.original_width  = m['width']
            self.original_height = m['height']
            self.monitor_left    = m['left']
            self.monitor_top     = m['top']
            self.record_width    = int(self.original_width  * self.scale_factor)
            self.record_height   = int(self.original_height * self.scale_factor)
            if self.record_width  % 2: self.record_width  -= 1
            if self.record_height % 2: self.record_height -= 1

    # -- Настройки --------------------------------------------------------
    def load_settings(self):
        try:
            if os.path.exists(SETTINGS_PATH):
                self._first_launch = False
                with open(SETTINGS_PATH) as f:
                    s = json.load(f)
                for k, v in s.items():
                    if hasattr(self, k) and not isinstance(getattr(self, k), tk.Variable):
                        setattr(self, k, v)
                self.always_on_top.set(s.get("always_on_top", False))
                self.countdown_var.set(s.get("countdown", True))
                self.timestamp_var.set(s.get("timestamp", False))
                self.cursor_var.set(s.get("cursor", False))
                self.minimize_to_tray.set(s.get("minimize_to_tray", True))
                if self.always_on_top.get():
                    self.root.attributes('-topmost', True)
        except Exception as e:
            log.warning(f"load_settings: {e}")
        if not hasattr(self, '_first_launch'):
            self._first_launch = True

    def save_settings(self, silent=False):
        s = {k: getattr(self, k) for k in (
            "output_folder","scale_factor","target_fps","quality","recording_mode",
            "current_theme","current_language","show_summary","video_codec","hw_accel",
            "enc_preset","enc_crf","pix_fmt","audio_sample_rate","audio_aac_bitrate",
            "audio_out_channels","ui_theme","ui_scale","ui_font","filename_template",
            "auto_stop_min","replay_buffer_sec","hotkey_start_stop","hotkey_pause",
            "hotkey_fullscreen","notify_sound","notify_flash","auto_save_profile",
            "disable_preview",
        )}
        s.update({
            "always_on_top":    self.always_on_top.get(),
            "countdown":        self.countdown_var.get(),
            "timestamp":        self.timestamp_var.get(),
            "cursor":           self.cursor_var.get(),
            "minimize_to_tray": self.minimize_to_tray.get(),
        })
        with open(SETTINGS_PATH, "w") as f:
            json.dump(s, f, indent=2)
        if not silent:
            messagebox.showinfo("Info", "Settings saved!")

    # ════════════════════════════════════════════════════════════════════
    # CAPTURE LOOP  (idle preview — только когда НЕ записываем)
    # ════════════════════════════════════════════════════════════════════
    def _capture_loop(self):
        """
        Idle-preview поток.
        Во время записи pipeline_ctl сам делает превью (C++),
        мы просто раз в 0.5 с запрашиваем его через get_preview_image().
        Когда запись не идёт — захватываем через mss (~12 fps).
        """
        import mss as _mss
        sct = _mss.mss()

        _pv_last = 0.0
        _rec_last = 0.0

        while self._preview_running:
            try:
                pw = getattr(self, 'preview_width', 640)
                ph = getattr(self, 'preview_height', 360)
                recording = getattr(self, 'recording', False)
                monitor = getattr(self, 'monitor', None)

                if monitor is None:
                    time.sleep(0.1)
                    continue

                if getattr(self, 'disable_preview', False):
                    try:
                        self._preview_queue.get_nowait()
                    except queue.Empty:
                        pass
                    self._preview_queue.put_nowait(None)
                    time.sleep(0.5)
                    continue

                # -- Во время записи: C++ pipeline уже делает превью --
                if recording:
                    now = time.time()
                    if now - _rec_last >= 0.08:   # 12 fps макс
                        _rec_last = now
                        if pipeline_ctl.available:
                            img = pipeline_ctl.get_preview_image()
                            if img is not None:
                                try:
                                    self._preview_queue.get_nowait()
                                except queue.Empty:
                                    pass
                                self._preview_queue.put_nowait(img)
                    time.sleep(0.04)
                    continue

                # -- Idle: mss захват --
                screenshot = sct.grab(monitor)
                sw, sh = screenshot.size

                # Используем нативную конвертацию если доступна
                if NATIVE_OK and _native_core:
                    rgb_np = _native_core.bgrx_to_rgb_np(screenshot.bgra, sw, sh)
                    img = Image.fromarray(rgb_np)
                    img.thumbnail((pw, ph), Image.Resampling.BILINEAR)
                else:
                    img = Image.frombytes("RGB", (sw, sh), screenshot.bgra, "raw", "BGRX")
                    img.thumbnail((pw, ph), Image.Resampling.BILINEAR)

                try:
                    self._preview_queue.get_nowait()
                except queue.Empty:
                    pass
                self._preview_queue.put_nowait(img)

                # Preview FPS tracking
                now = time.time()
                if not hasattr(self, '_pv_last_t'):
                    self._pv_last_t = now
                    self._pv_frame_acc = 0
                self._pv_frame_acc += 1
                if now - self._pv_last_t >= 2.0:
                    fps_val = self._pv_frame_acc / (now - self._pv_last_t)
                    self._pv_last_t = now
                    self._pv_frame_acc = 0
                    if hasattr(self, '_preview_fps_lbl'):
                        try:
                            self._preview_fps_lbl.config(text=f"{fps_val:.0f} fps")
                        except Exception:
                            pass

            except Exception as e:
                log.debug(f"_capture_loop: {e}")

            time.sleep(0.083)   # ~12 fps idle

    # ════════════════════════════════════════════════════════════════════
    # СТАРТ ЗАПИСИ  —  главный оптимизированный метод
    # ════════════════════════════════════════════════════════════════════
    def start_recording(self):
        try:
            ts = datetime.now().strftime("%Y%m%d_%H%M%S")
            self.filename = f"{self.output_folder}/HomRec_{ts}.mp4"
            log.info("=" * 50)
            log.info(f"START RECORDING: {self.filename}")
            log.info(f"Pipeline: {PIPELINE_OK} | FFmpeg: {self.ffmpeg_path}")

            if not self.ffmpeg_path:
                raise RuntimeError("FFmpeg not found!")

            self.stop_flag  = False
            self.paused     = False
            self.frame_count = 0

            w, h   = self.record_width, self.record_height
            fps    = self.target_fps
            ox, oy = self.monitor_left, self.monitor_top

            if PIPELINE_OK:
                self._start_recording_pipeline(w, h, fps, ox, oy)
            else:
                self._start_recording_gdigrab(w, h, fps, ox, oy)

            # Запускаем аудио
            if hasattr(self, 'audio_panel') and self.audio_panel.audio_enabled.get():
                self.start_audio_recording()

            self.recording  = True
            self.start_time = time.time()
            self._set_taskbar_icon(recording=True)

            self.record_btn.config(text=self.lang.get("stop", "■ STOP"),
                                   bg=self.colors["error"],
                                   command=self.stop_recording)
            self.pause_btn.config(state="normal")
            self.status_icon.config(fg=self.colors["success"])
            self.status_label.config(text=self.lang.get("recording", "Recording"))

            self._update_stats()
            self._notify_recording_start()

        except Exception as e:
            log.exception("start_recording failed")
            messagebox.showerror("Error", f"Failed to start recording:\n{e}")

    # -- Pipeline-режим (C++ DXGI + YUV pipe) --------------------------
    def _start_recording_pipeline(self, w, h, fps, ox, oy):
        """Запись через hr_pipeline → rawvideo pipe → ffmpeg."""

        # 1. Создаём именованный пайп (Windows) или os.pipe() (POSIX)
        if sys.platform == "win32":
            pipe_name = r"\\.\pipe\homrec_yuv_" + str(os.getpid())
            write_handle, self._pipe_name = create_yuv_pipe_windows(pipe_name)
            self._pipe_write_handle = write_handle
            self._pipe_read_arg = pipe_name
            pipe_fd_for_cpp = write_handle
        else:
            rd, wr = os.pipe()
            self._pipe_rd = rd
            self._pipe_wr = wr
            pipe_fd_for_cpp = wr
            self._pipe_read_arg = f"pipe:{rd}"

        # 2. Настраиваем pipeline (захват на мониторе с оригинальным разрешением)
        ow = self.original_width
        oh = self.original_height
        ok = pipeline_ctl.configure(
            w=ow, h=oh, fps=fps,
            pipe_fd=pipe_fd_for_cpp,
            pv_w=min(self.preview_width, 960),
            pv_h=min(self.preview_height, 540),
        )
        if not ok:
            raise RuntimeError("pipeline_ctl.configure() failed")

        # 3. Строим ffmpeg команду (-f rawvideo, читает YUV420p из пайпа)
        codec_args = self._build_codec_args()
        needs_scale = (w != ow or h != oh)
        vf_args = ['-vf', f'scale={w}:{h}'] if needs_scale else []

        if sys.platform == "win32":
            ff_input = [
                '-f', 'rawvideo',
                '-pix_fmt', 'yuv420p',
                '-s', f'{ow}x{oh}',
                '-r', str(fps),
                '-i', self._pipe_read_arg,
            ]
        else:
            ff_input = [
                '-f', 'rawvideo',
                '-pix_fmt', 'yuv420p',
                '-s', f'{ow}x{oh}',
                '-r', str(fps),
                '-i', self._pipe_read_arg,
            ]

        cmd = [
            self.ffmpeg_path, '-y',
            *ff_input,
            *vf_args,
            *codec_args,
            '-pix_fmt', getattr(self, 'pix_fmt', 'yuv420p'),
            '-an',
            self.filename,
        ]
        log.debug(f"FFmpeg cmd (pipeline): {' '.join(cmd)}")

        # 4. Запускаем ffmpeg
        self.ffmpeg_proc = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW if platform.system() == "Windows" else 0,
        )

        # 5. На Windows: ждём подключения ffmpeg к пайпу в фоновом потоке
        if sys.platform == "win32":
            def _connect():
                ok2 = connect_yuv_pipe_windows(self._pipe_write_handle)
                if ok2:
                    log.info("Named pipe connected to ffmpeg")
                    pipeline_ctl.start()
                else:
                    log.error("Named pipe connect failed")
            threading.Thread(target=_connect, daemon=True).start()
        else:
            # POSIX: ffmpeg открыл pipe, можно сразу стартовать
            pipeline_ctl.start()

        log.info("Recording started (pipeline mode)")

    def _start_recording_gdigrab(self, w, h, fps, ox, oy):
        """Fallback когда hr_pipeline.dll недоступна."""
        needs_scale = (w != self.original_width or h != self.original_height)
        vf_args = ['-vf', f'scale={w}:{h}'] if needs_scale else []
        codec_args = self._build_codec_args()

        draw_mouse = '1' if self.cursor_var.get() else '0'

        if self.capture_mode == "window" and self.capture_window_title:
            ff_input = ['-f', 'gdigrab', '-framerate', str(fps),
                        '-draw_mouse', draw_mouse,
                        '-i', f'title={self.capture_window_title}']
        else:
            ff_input = ['-f', 'gdigrab', '-framerate', str(fps),
                        '-probesize', '32', '-fflags', 'nobuffer',
                        '-draw_mouse', draw_mouse,
                        '-offset_x', str(ox), '-offset_y', str(oy),
                        '-video_size', f'{self.original_width}x{self.original_height}',
                        '-i', 'desktop']

        cmd = [self.ffmpeg_path, '-y',
               *ff_input,
               *vf_args,
               *codec_args,
               '-pix_fmt', getattr(self, 'pix_fmt', 'yuv420p'),
               '-an',
               self.filename]

        log.debug(f"FFmpeg cmd (gdigrab): {' '.join(cmd)}")
        self.ffmpeg_proc = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            stdin=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW if platform.system() == "Windows" else 0,
        )
        log.info("Recording started (gdigrab fallback)")

    # ════════════════════════════════════════════════════════════════════
    # СТОП ЗАПИСИ
    # ════════════════════════════════════════════════════════════════════
    def stop_recording(self):
        log.info("stop_recording called")
        self.recording = False
        self.stop_flag  = True

        saved_filename    = self.filename
        saved_start_time  = self.start_time
        saved_rec_w       = self.record_width
        saved_rec_h       = self.record_height
        saved_target_fps  = self.target_fps

        # UI немедленно
        self._set_taskbar_icon(recording=False)
        self.record_btn.config(text=self.lang.get("start", "▶ START"),
                               bg=self.colors["success"],
                               command=self.start_with_countdown)
        self.pause_btn.config(state="disabled", text=self.lang.get("pause", "⏸ PAUSE"))
        self.status_icon.config(fg=self.colors["warning"])
        self.status_label.config(text="Saving…")
        self.file_label.config(text="Processing…")

        def _finalize():
            # 1. Останавливаем pipeline (C++ поток)
            if PIPELINE_OK:
                pipeline_ctl.stop()
                pipeline_ctl.destroy()

            # 2. Закрываем пайп
            try:
                if sys.platform == "win32" and hasattr(self, '_pipe_write_handle'):
                    ctypes.windll.kernel32.CloseHandle(
                        ctypes.c_void_p(self._pipe_write_handle))
                elif hasattr(self, '_pipe_wr'):
                    os.close(self._pipe_wr)
            except Exception:
                pass

            # 3. Завершаем ffmpeg
            if self.ffmpeg_proc and self.ffmpeg_proc.poll() is None:
                try:
                    self.ffmpeg_proc.stdin and self.ffmpeg_proc.stdin.write(b'q')
                except Exception:
                    pass
                try:
                    self.ffmpeg_proc.wait(timeout=6)
                except Exception:
                    try:
                        self.ffmpeg_proc.kill()
                    except Exception:
                        pass

            # 4. Аудио
            audio_file = None
            if self.audio_recording:
                audio_file = self.stop_audio_recording()

            time.sleep(0.2)

            # 5. Merge audio
            has_ffmpeg = bool(self.ffmpeg_path)
            audio_merged = False
            if audio_file and os.path.exists(saved_filename) and \
               hasattr(self, 'audio_panel') and self.audio_panel.audio_enabled.get():
                if has_ffmpeg:
                    self.root.after(0, lambda: self.file_label.config(text="Merging audio…"))
                    audio_merged = self.merge_audio_video(saved_filename, audio_file)

            self.root.after(0, lambda: self._finalize_ui(
                saved_filename, saved_start_time,
                saved_rec_w, saved_rec_h, saved_target_fps,
                audio_file, audio_merged, has_ffmpeg,
            ))

        threading.Thread(target=_finalize, daemon=True).start()

    # -- Пауза ------------------------------------------------------------
    def toggle_pause(self):
        if self.recording:
            self.paused = not self.paused
            pipeline_ctl.pause(self.paused)
            if self.paused:
                self.pause_btn.config(text=self.lang.get("resume", "▶ RESUME"),
                                      bg=self.colors["success"])
                self.status_icon.config(fg=self.colors["warning"])
                self.status_label.config(text=self.lang.get("paused", "Paused"))
            else:
                self.pause_btn.config(text=self.lang.get("pause", "⏸ PAUSE"),
                                      bg=self.colors["warning"])
                self.status_icon.config(fg=self.colors["success"])
                self.status_label.config(text=self.lang.get("recording", "Recording"))

    # -- Статистика во время записи ---------------------------------------
    def _update_stats(self):
        if not self.recording:
            return
        try:
            elapsed = time.time() - self.start_time
            h = int(elapsed // 3600)
            m = int((elapsed % 3600) // 60)
            s = int(elapsed % 60)
            self.time_label.config(text=f"{h:02d}:{m:02d}:{s:02d}")

            if PIPELINE_OK and pipeline_ctl.available:
                frames, drops, fps = pipeline_ctl.stats()
                self.frame_count = frames
                self.fps_label.config(text=f"{self.lang.get('fps','FPS:')} {fps:.1f}")
            else:
                if elapsed > 0 and self.frame_count > 0:
                    self.fps_label.config(
                        text=f"{self.lang.get('fps','FPS:')} {self.frame_count/elapsed:.1f}")
        except Exception:
            pass
        self.root.after(1000, self._update_stats)

    # -- Preview update (UI-поток) -----------------------------------------
    def update_preview(self):
        try:
            img = self._preview_queue.get_nowait()
            if img is None:
                self._show_preview_placeholder()
            else:
                photo = ImageTk.PhotoImage(img)
                self.preview_label.config(image=photo, text="")
                self.preview_label.image = photo
        except queue.Empty:
            pass
        except Exception:
            pass

        # Во время записи обновляем реже (превью идёт из C++ с его ритмом)
        interval = 80 if not getattr(self, 'recording', False) else 80
        self.root.after(interval, self.update_preview)

    def _build_codec_args(self) -> list:
        codec   = getattr(self, 'video_codec', 'libx264')
        hw      = getattr(self, 'hw_accel',    'auto')
        if codec == 'libx264' and hw == 'auto':
            gpu = getattr(self, '_gpu_encoder_cache', None)
            if gpu:
                codec = gpu
        q  = getattr(self, 'quality', 70)
        qp = max(18, min(34, int(34 - (q / 100) * 16)))
        fps  = getattr(self, 'target_fps', 30)
        gop  = fps * 2
        is_hw  = codec in ('h264_nvenc','hevc_nvenc','h264_amf','hevc_amf','h264_qsv','hevc_qsv')
        is_265 = codec in ('libx265','hevc_nvenc','hevc_amf','hevc_qsv')
        preset = getattr(self, 'enc_preset', 'ultrafast')
        args = ['-c:v', codec]
        if is_hw:
            if 'nvenc' in codec:
                args += ['-preset','p1','-tune','ull','-rc','constqp','-qp',str(qp),'-g',str(gop)]
            elif 'qsv' in codec:
                args += ['-preset','veryfast','-async_depth','1','-qp',str(qp),'-g',str(gop)]
            elif 'amf' in codec:
                args += ['-quality','speed','-rc','cqp','-qp_i',str(qp),'-qp_p',str(qp),'-g',str(gop)]
        else:
            threads = max(1, (os.cpu_count() or 4) // 4)
            args += ['-preset',preset,'-tune','zerolatency',
                     '-crf',str(qp),'-g',str(gop),'-threads',str(threads)]
            if is_265:
                args += ['-x265-params','log-level=error']
        return args

    # -- GPU probe (без изменений) -------------------------------------
    def _warm_up_gpu_probe(self):
        if not self.ffmpeg_path or hasattr(self, '_gpu_encoder_cache'):
            return
        def _probe():
            for name, extra in [
                ('h264_nvenc', ['-c:v','h264_nvenc']),
                ('h264_amf',   ['-c:v','h264_amf']),
                ('h264_qsv',   ['-c:v','h264_qsv']),
            ]:
                try:
                    r = subprocess.run(
                        [self.ffmpeg_path,'-y','-f','lavfi','-i','nullsrc=s=32x32:d=0.1',
                         *extra,'-f','null','-'],
                        capture_output=True, timeout=6,
                        creationflags=subprocess.CREATE_NO_WINDOW if platform.system()=='Windows' else 0)
                    if r.returncode == 0:
                        self._gpu_encoder_cache = name
                        log.info(f"GPU encoder: {name}")
                        return
                except Exception:
                    pass
            self._gpu_encoder_cache = None
        threading.Thread(target=_probe, daemon=True).start()

    def _detect_gpu_encoder(self):
        return getattr(self, '_gpu_encoder_cache', None)

    # ════════════════════════════════════════════════════════════════════
    # UI — полностью из homrec.py (если импортирован), иначе минимальный
    # ════════════════════════════════════════════════════════════════════

    def _set_app_icon(self):
        if getattr(sys, 'frozen', False):
            base_dir = os.path.dirname(sys.executable)
        else:
            base_dir = os.path.dirname(os.path.abspath(__file__))
        self._icons_dir = os.path.join(base_dir, "icons")
        self._main_ico  = os.path.join(self._icons_dir, "main.ico")
        self._rec_ico   = os.path.join(self._icons_dir, "rec.ico")
        try:
            self.root.iconbitmap(self._main_ico)
        except Exception:
            pass
        if sys.platform == "win32":
            try:
                ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("homrec.1.7.0")
            except Exception:
                pass
        self._rec_frames = []

    def _set_taskbar_icon(self, recording: bool):
        try:
            ico = self._rec_ico if recording else self._main_ico
            if os.path.exists(ico):
                self.root.iconbitmap(ico)
        except Exception:
            pass

    def _set_icon(self, win):
        try:
            if os.path.exists(self._main_ico):
                win.iconbitmap(self._main_ico)
        except Exception:
            pass

    def _on_window_resize(self, event):
        if event.widget == self.root:
            self._update_preview_size()

    def _update_preview_size(self):
        try:
            self.preview_width  = max(600, min(self.root.winfo_width()  - 320, 1280))
            self.preview_height = max(350, min(self.root.winfo_height() - 240, 720))
            if PIPELINE_OK and pipeline_ctl.available:
                pipeline_ctl.set_preview_size(self.preview_width, self.preview_height)
        except Exception:
            pass

    def update_preview_size(self):
        self._update_preview_size()

    def _show_preview_placeholder(self):
        try:
            pw, ph = getattr(self,'preview_width',640), getattr(self,'preview_height',360)
            if getattr(self,'_placeholder_key',None) == (pw,ph):
                self.preview_label.config(image=self._placeholder_photo, text="")
                self.preview_label.image = self._placeholder_photo
                return
            img  = Image.new("RGB", (pw, ph), color="#181825")
            draw = ImageDraw.Draw(img)
            draw.text((pw//2 - 60, ph//2 - 10), "Preview disabled", fill="#6c7086")
            self._placeholder_photo = ImageTk.PhotoImage(img)
            self._placeholder_key   = (pw, ph)
            self.preview_label.config(image=self._placeholder_photo, text="")
            self.preview_label.image = self._placeholder_photo
        except Exception:
            pass

    def _notify_recording_start(self):
        if getattr(self, 'notify_flash', True):
            orig = self.root.cget("bg")
            def _flash(n=0):
                if n >= 6:
                    self.root.configure(bg=orig); return
                self.root.configure(bg=self.colors["error"] if n%2==0 else orig)
                self.root.after(120, lambda: _flash(n+1))
            _flash()
        if getattr(self, 'notify_sound', True):
            try:
                import winsound
                winsound.MessageBeep(winsound.MB_OK)
            except Exception:
                pass

    # -- Stub-методы (полные реализации в homrec.py) ------------------
    def check_ffmpeg(self): return bool(self.ffmpeg_path)

    def start_with_countdown(self):
        if not self.recording:
            if self.countdown_var.get():
                self._show_countdown()
            else:
                self.start_recording()
        else:
            self.stop_recording()

    def _show_countdown(self):
        # Простой вариант без анимации
        import time as _t
        w = tk.Toplevel(self.root)
        w.overrideredirect(True)
        w.geometry("200x120")
        w.configure(bg="#0f0f17")
        w.attributes("-topmost", True)
        sw, sh = w.winfo_screenwidth(), w.winfo_screenheight()
        w.geometry(f"+{(sw-200)//2}+{(sh-120)//2}")
        lbl = tk.Label(w, text="3", font=("Segoe UI", 48, "bold"),
                       bg="#0f0f17", fg="#a6e3a1")
        lbl.pack(expand=True)
        def _tick(n):
            if n > 0:
                lbl.config(text=str(n))
                w.after(1000, lambda: _tick(n-1))
            else:
                w.destroy()
                self.start_recording()
        _tick(3)

    def toggle_recording(self):
        if not self.recording: self.start_recording()
        else: self.stop_recording()

    def toggle_fullscreen(self):
        self.root.attributes('-fullscreen', not self.root.attributes('-fullscreen'))

    def toggle_always_on_top(self):
        self.root.attributes('-topmost', self.always_on_top.get())
        self.save_settings(silent=True)

    def open_recordings(self):
        if os.path.exists(self.output_folder):
            os.startfile(self.output_folder)
        else:
            messagebox.showwarning("Warning", "Folder doesn't exist!")

    def open_settings(self):
        if _HOMREC_IMPORTED:
            SettingsDialog(self.root, self)

    def change_language(self, lang):
        self.current_language = lang
        self.lang = dict(LANGUAGES.get(lang, LANGUAGES["en"]))
        self.language_var.set(lang)
        self.save_settings(silent=True)

    def change_theme(self, theme):
        self.current_theme = theme
        self.colors = self.BUILTIN_THEMES.get(theme, self.BUILTIN_THEMES["dark"])
        self.apply_theme()
        self.save_settings(silent=True)

    def update_ui_language(self):
        self.recreate_widgets()

    def recreate_widgets(self):
        for w in self.root.winfo_children():
            w.destroy()
        self.create_menu()
        self.create_widgets()

    def set_capture_desktop(self):
        self.capture_mode = "desktop"
        self.capture_window_title = ""

    def open_window_picker(self):
        messagebox.showinfo("Info", "Window picker: see full homrec.py for implementation.")

    def get_dshow_audio_devices(self):
        try:
            r = subprocess.run([self.ffmpeg_path,'-list_devices','true','-f','dshow','-i','dummy'],
                capture_output=True, text=True, timeout=5,
                creationflags=subprocess.CREATE_NO_WINDOW if platform.system()=='Windows' else 0)
            devs = []
            for line in r.stderr.split('\n'):
                if '"' in line and 'audio' in line.lower():
                    s = line.find('"'); e = line.find('"', s+1)
                    if e > s: devs.append(line[s+1:e])
            return devs
        except Exception:
            return []

    def merge_audio_video(self, video_file, audio_file):
        if not audio_file or not os.path.exists(audio_file): return False
        if not os.path.exists(video_file): return False
        if not self.ffmpeg_path: return False
        out = video_file.replace('.mp4', '_temp.mp4')
        try:
            cmd = [self.ffmpeg_path,'-i',video_file,'-i',audio_file,
                   '-c:v','copy','-c:a','aac','-af','aresample=async=1000',
                   '-map','0:v:0','-map','1:a:0','-shortest','-y',out]
            r = subprocess.run(cmd, capture_output=True, timeout=120,
                creationflags=subprocess.CREATE_NO_WINDOW if platform.system()=='Windows' else 0)
            if r.returncode == 0 and os.path.exists(out):
                os.remove(video_file); os.remove(audio_file)
                os.rename(out, video_file)
                return True
        except Exception as e:
            log.warning(f"merge_audio_video: {e}")
        return False

    # -- Audio recording (без изменений от v1.5.1) ---------------------
    def start_audio_recording(self):
        """Копия из homrec.py v1.5.1 с perf-фиксами (cached tk vars)."""
        try:
            self.audio_channels = self._get_audio_channels()
            silence = b'\x00' * 1024 * 2 * self.audio_channels
            self.audio_p = pyaudio.PyAudio()
            self.audio_stream = self.audio_p.open(
                format=pyaudio.paInt16, channels=self.audio_channels,
                rate=44100, input=True, frames_per_buffer=1024)
            self.audio_recording = True
            self.audio_frames    = []

            def _mic():
                while self.audio_recording and not self.stop_flag:
                    if not self.paused:
                        if not self.audio_panel._mic_mute_cached:
                            try:
                                data = self.audio_stream.read(1024, exception_on_overflow=False)
                                vol  = self.audio_panel._mic_vol_cached
                                if vol != 1.0: data = audioop.mul(data, 2, vol)
                                self.audio_frames.append(data)
                                level = _native_core.audio_rms_level(data)
                                self.audio_panel.update_mic_level(level)
                            except Exception: pass
                        else:
                            try: self.audio_stream.read(1024, exception_on_overflow=False)
                            except Exception: pass
                            self.audio_frames.append(silence)
                    else:
                        self.audio_frames.append(silence)
                        time.sleep(1024/44100)

            self.audio_thread = threading.Thread(target=_mic, daemon=True)
            self.audio_thread.start()
            log.info(f"Mic recording started ({self.audio_channels} ch)")
        except Exception as e:
            log.error(f"start_audio_recording: {e}")
            self.audio_recording = False

    def stop_audio_recording(self):
        self.audio_recording = False
        self.sys_audio_recording = False

        for stream in (self.audio_stream,
                       getattr(self, 'sys_audio_stream', None)):
            if stream:
                try: stream.stop_stream(); stream.close()
                except Exception: pass
        self.audio_stream = None

        for thr in (self.audio_thread,
                    getattr(self, 'sys_audio_thread', None)):
            if thr and thr.is_alive():
                thr.join(timeout=1)
        self.audio_thread = None

        for p in (self.audio_p,
                  getattr(self, 'sys_audio_p', None)):
            if p:
                try: p.terminate()
                except Exception: pass
        self.audio_p = None

        time.sleep(0.15)

        mic_frames = self.audio_frames[:]
        self.audio_frames = []

        if not mic_frames: return None

        audio_filename = self.filename.replace('.mp4', '_audio.wav')
        try:
            wf = wave.open(audio_filename, 'wb')
            wf.setnchannels(self.audio_channels)
            wf.setsampwidth(2)
            wf.setframerate(44100)
            CHUNK = 256
            for i in range(0, len(mic_frames), CHUNK):
                wf.writeframes(b''.join(mic_frames[i:i+CHUNK]))
            wf.close()
            return audio_filename
        except Exception as e:
            log.warning(f"WAV write: {e}")
            return None

    def _get_audio_channels(self):
        try:
            p = pyaudio.PyAudio()
            for ch in (2, 1):
                try:
                    s = p.open(format=pyaudio.paInt16, channels=ch,
                               rate=44100, input=True, frames_per_buffer=1024)
                    s.close(); p.terminate(); return ch
                except Exception: pass
            p.terminate()
        except Exception: pass
        return 1

    def _find_wasapi_loopback(self, p):
        """Stub — полная реализация в homrec.py."""
        return None

    # -- Finalize UI ----------------------------------------------------
    def _finalize_ui(self, filename, start_time, rw, rh, rfps,
                     audio_file, audio_merged, has_ffmpeg):
        self.status_icon.config(fg=self.colors["error"])
        self.status_label.config(text=self.lang.get("ready", "Ready"))

        if os.path.exists(filename):
            size = os.path.getsize(filename) / (1024*1024)
            dur  = time.time() - start_time
            self.file_label.config(text=f"✅ Saved: {size:.1f} MB | {dur:.1f}s")
        else:
            self.file_label.config(text="❌ Recording failed")

    # -- Tray, hotkeys, drag-drop (stubs — полные в homrec.py) ---------
    def setup_tray(self):
        if not HAS_TRAY: return
        try:
            img = Image.new("RGBA", (64,64), (0,0,0,0))
            d = ImageDraw.Draw(img)
            d.ellipse([4,4,60,60], fill="#89b4fa")
            menu = pystray.Menu(
                TrayItem("Show", lambda i,m: self.root.after(0, self.root.deiconify), default=True),
                TrayItem("Start/Stop", lambda i,m: self.root.after(0, self.toggle_recording)),
                pystray.Menu.SEPARATOR,
                TrayItem("Quit", lambda i,m: self.root.after(0, self.quit_app)),
            )
            self.tray_icon = pystray.Icon("HomRec", img, "HomRec", menu)
            threading.Thread(target=self.tray_icon.run, daemon=True).start()
        except Exception as e:
            log.warning(f"Tray: {e}")

    def _apply_hotkeys(self):
        for key in getattr(self, '_bound_hotkeys', []):
            try: self.root.unbind(key)
            except Exception: pass
        self._bound_hotkeys = []
        def _bind(key, cmd):
            if not key or " " in key: return
            k = f'<{key}>'
            try:
                self.root.bind(k, cmd)
                self._bound_hotkeys.append(k)
            except Exception as e:
                log.warning(f"hotkey bind {k}: {e}")
        _bind(self.hotkey_start_stop,  lambda e: self.toggle_recording())
        _bind(self.hotkey_pause,       lambda e: self.toggle_pause() if self.recording else None)
        _bind(self.hotkey_fullscreen,  lambda e: self.toggle_fullscreen())

    def _setup_drag_drop(self):
        if not _DND_AVAILABLE: return
        try:
            self.root.drop_target_register(DND_FILES)
            self.root.dnd_bind('<<Drop>>', lambda e: None)
        except Exception: pass

    def on_closing(self):
        if HAS_TRAY and self.tray_icon and self.minimize_to_tray.get():
            self.root.withdraw()
        else:
            self.quit_app()

    def quit_app(self):
        if self.recording:
            if not messagebox.askyesno("Warning", "Recording in progress! Stop and exit?"):
                return
            if self.ffmpeg_proc and self.ffmpeg_proc.poll() is None:
                try: self.ffmpeg_proc.kill()
                except Exception: pass
        if PIPELINE_OK:
            pipeline_ctl.stop()
            pipeline_ctl.destroy()
        self._preview_running = False
        if self.tray_icon:
            try: self.tray_icon.stop()
            except Exception: pass
        self.root.after(100, self.root.destroy)

    # -- Update check --------------------------------------------------
    def _start_update_check(self):
        if _HOMREC_IMPORTED:
            check_for_updates(self._on_update_found)

    def _on_update_found(self, ver):
        self.root.after(0, lambda: log.info(f"Update available: v{ver}"))

    def _show_welcome_and_save(self):
        self.save_settings(silent=True)
        if _HOMREC_IMPORTED:
            WelcomeDialog.show(self)

    def _manual_update_check(self): pass
    def _open_issues(self): pass
    def show_analytics(self): pass

    # ═══════════════════════════════════════════════════════════════════
    # CREATE_MENU / CREATE_WIDGETS — тонкие обёртки
    # Если homrec.py импортирован — вызываем его реализацию через mixin.
    # Иначе — минимальное UI достаточное для работы.
    # ═══════════════════════════════════════════════════════════════════

    def create_menu(self):
        c = self.colors
        mb = tk.Menu(self.root, bg=c["surface"], fg=c["fg"])
        self.root.config(menu=mb)

        fm = tk.Menu(mb, tearoff=0, bg=c["surface"], fg=c["fg"])
        mb.add_cascade(label=self.lang.get("file_menu","File"), menu=fm)
        fm.add_command(label=self.lang.get("open_recordings","Open Recordings"),
                       command=self.open_recordings)
        fm.add_separator()
        fm.add_command(label=self.lang.get("exit","Exit"), command=self.quit_app)

        sm = tk.Menu(mb, tearoff=0, bg=c["surface"], fg=c["fg"])
        mb.add_cascade(label=self.lang.get("settings_menu","Settings"), menu=sm)
        sm.add_command(label=self.lang.get("preferences","Preferences…"),
                       command=self.open_settings)

        hm = tk.Menu(mb, tearoff=0, bg=c["surface"], fg=c["fg"])
        mb.add_cascade(label=self.lang.get("help_menu","Help"), menu=hm)
        hm.add_command(label=self.lang.get("check_updates","Check for Updates"),
                       command=self._manual_update_check)

    def create_widgets(self):
        c = self.colors
        main = tk.Frame(self.root, bg=c["bg"])
        main.pack(fill="both", expand=True, padx=15, pady=15)

        # -- Левая панель --
        left = tk.Frame(main, bg=c["surface"], width=240)
        left.pack(side="left", fill="y", padx=(0,15))
        left.pack_propagate(False)

        # Заголовок
        tk.Label(left, text="HomRec", font=("Segoe UI",22,"bold"),
                 bg=c["surface"], fg=c["accent"]).pack(pady=(20,0))
        tk.Label(left, text="v1.7.0", font=("Segoe UI",10),
                 bg=c["surface"], fg=c["text_secondary"]).pack()

        # Кнопки записи
        bf = tk.Frame(left, bg=c["surface"])
        bf.pack(pady=20, padx=15, fill="x")

        self.record_btn = tk.Button(bf,
            text=self.lang.get("start","▶ START"),
            command=self.start_with_countdown,
            bg=c["success"], fg=c["bg"],
            font=("Segoe UI",11,"bold"), relief="flat", height=2, cursor="hand2")
        self.record_btn.pack(fill="x", pady=(0,4))

        self.pause_btn = tk.Button(bf,
            text=self.lang.get("pause","⏸ PAUSE"),
            command=self.toggle_pause,
            bg=c["warning"], fg=c["bg"],
            font=("Segoe UI",10,"bold"), state="disabled",
            relief="flat", height=1, cursor="hand2")
        self.pause_btn.pack(fill="x")
        self.stop_btn = tk.Button(bf)   # скрытая совместимость

        # Статус
        sf = tk.Frame(left, bg=c["surface"])
        sf.pack(pady=10, padx=15, fill="x")
        tk.Label(sf, text=self.lang.get("status","STATUS"),
                 font=("Segoe UI",10,"bold"),
                 bg=c["surface"], fg=c["accent"]).pack(anchor="w")
        sr = tk.Frame(sf, bg=c["surface"]); sr.pack(fill="x", pady=4)
        self.status_icon  = tk.Label(sr, text="⬤", fg=c["error"],
                                      bg=c["surface"], font=("Arial",16))
        self.status_icon.pack(side="left", padx=(0,6))
        self.status_label = tk.Label(sr, text=self.lang.get("ready","Ready"),
                                      bg=c["surface"], fg=c["text"],
                                      font=("Segoe UI",10))
        self.status_label.pack(side="left")

        # Таймер
        tf = tk.Frame(left, bg=c["surface"])
        tf.pack(pady=10, padx=15, fill="x")
        tk.Label(tf, text=self.lang.get("time","TIME"),
                 font=("Segoe UI",10,"bold"),
                 bg=c["surface"], fg=c["accent"]).pack(anchor="w")
        self.time_label = tk.Label(tf, text="00:00:00",
                                    font=("Consolas",22,"bold"),
                                    bg=c["surface"], fg=c["accent"])
        self.time_label.pack(pady=4)

        # Статистика
        stf = tk.Frame(left, bg=c["surface"])
        stf.pack(pady=10, padx=15, fill="x")
        tk.Label(stf, text=self.lang.get("stats","STATS"),
                 font=("Segoe UI",10,"bold"),
                 bg=c["surface"], fg=c["accent"]).pack(anchor="w")
        self.fps_label = tk.Label(stf,
            text=f"{self.lang.get('fps','FPS:')} 0",
            bg=c["surface"], fg=c["text"], font=("Consolas",10))
        self.fps_label.pack(anchor="w", pady=2)
        self.res_label = tk.Label(stf,
            text=f"{self.lang.get('resolution','Resolution:')} {self.record_width}x{self.record_height}",
            bg=c["surface"], fg=c["text"], font=("Consolas",10))
        self.res_label.pack(anchor="w", pady=2)

        # Pipeline-статус
        pl_txt = "⚡ C++ Pipeline" if PIPELINE_OK else "🐍 Python Fallback"
        pl_col = c.get("success","#a6e3a1") if PIPELINE_OK else c.get("warning","#f9e2af")
        tk.Label(left, text=pl_txt, bg=c["surface"], fg=pl_col,
                 font=("Segoe UI",8)).pack(pady=(0,4))

        # Inline FPS slider
        fps_f = tk.Frame(left, bg=c["surface"])
        fps_f.pack(pady=(0,8), padx=15, fill="x")
        tk.Frame(fps_f, bg=c.get("surface_light","#45475a"), height=1).pack(fill="x", pady=(0,6))
        fr = tk.Frame(fps_f, bg=c["surface"]); fr.pack(fill="x")
        tk.Label(fr, text="FPS limit:", bg=c["surface"], fg=c["text_secondary"],
                 font=("Segoe UI",8)).pack(side="left")
        self._inline_fps_val = tk.Label(fr, text=str(self.target_fps),
            bg=c["surface"], fg=c["accent"], font=("Segoe UI",8,"bold"), width=3)
        self._inline_fps_val.pack(side="right")

        def _fps_slide(v):
            val = int(float(v))
            self.target_fps = val
            self._inline_fps_val.config(text=str(val))
            if PIPELINE_OK and pipeline_ctl.available:
                pipeline_ctl.set_fps(val)
            self.save_settings(silent=True)

        self._inline_fps_slider = tk.Scale(fps_f, from_=1, to=60,
            orient="horizontal", length=160, showvalue=False,
            bg=c["surface"], fg=c["text"], highlightthickness=0,
            troughcolor=c.get("surface_light","#45475a"), command=_fps_slide)
        self._inline_fps_slider.set(self.target_fps)
        self._inline_fps_slider.pack(fill="x", pady=(2,0))

        # -- Правая панель (preview) --
        right = tk.Frame(main, bg=c["bg"])
        right.pack(side="right", fill="both", expand=True)

        pv_cont = tk.Frame(right, bg=c.get("surface_light","#45475a"),
                            relief="flat", bd=2)
        pv_cont.pack(fill="both", expand=True, pady=(0,10))

        pv_hdr = tk.Frame(pv_cont, bg=c.get("surface_light","#45475a"), height=28)
        pv_hdr.pack(fill="x"); pv_hdr.pack_propagate(False)
        tk.Label(pv_hdr, text="● PREVIEW",
                 bg=c.get("surface_light","#45475a"), fg=c["accent"],
                 font=("Segoe UI",9,"bold")).pack(side="left", padx=8, pady=4)
        self._preview_fps_lbl = tk.Label(pv_hdr, text="",
            bg=c.get("surface_light","#45475a"), fg=c.get("text_secondary","#a6adc8"),
            font=("Segoe UI",8))
        self._preview_fps_lbl.pack(side="right", padx=8)

        pv_frame = tk.Frame(pv_cont, bg=c["preview_bg"])
        pv_frame.pack(fill="both", expand=True, padx=6, pady=6)
        self.preview_label = tk.Label(pv_frame, bg=c["preview_bg"])
        self.preview_label.pack(fill="both", expand=True)

        # -- Аудио-панель --
        bot = tk.Frame(right, bg=c["bg"], height=280)
        bot.pack(fill="x"); bot.pack_propagate(False)

        if _HOMREC_IMPORTED:
            self.audio_panel = AudioPanel(bot, self)
        else:
            # Минимальная заглушка
            class _FakeAudio:
                audio_enabled    = tk.BooleanVar(value=True)
                mic_mute         = tk.BooleanVar(value=False)
                sys_mute         = tk.BooleanVar(value=False)
                _mic_vol_cached  = 0.8
                _sys_vol_cached  = 0.5
                _mic_mute_cached = False
                def update_mic_level(self, v): pass
                def update_sys_level(self, v): pass
            self.audio_panel = _FakeAudio()
            tk.Label(bot, text="Audio panel: install homrec.py for full mixer",
                     bg=c["bg"], fg=c.get("text_secondary","#a6adc8"),
                     font=("Segoe UI",9)).pack(pady=10)

        # -- Статусная строка --
        bbar = tk.Frame(self.root, bg=c["surface"], height=28)
        bbar.pack(side="bottom", fill="x"); bbar.pack_propagate(False)

        self._status_dot = tk.Label(bbar, text="●",
            bg=c["surface"], fg=c.get("success","#a6e3a1"), font=("Segoe UI",9))
        self._status_dot.pack(side="left", padx=(8,2), pady=4)
        self.file_label = tk.Label(bbar, text=self.lang.get("ready","Ready"),
            bg=c["surface"], fg=c["text_secondary"], font=("Segoe UI",9))
        self.file_label.pack(side="left", padx=(0,8))

        tk.Label(bbar, text=f"v{CURRENT_VERSION}",
                 bg=c["surface"], fg=c.get("text_secondary","#6c7086"),
                 font=("Segoe UI",8)).pack(side="right", padx=(0,4))
        tk.Label(bbar, text="Homa4ella",
                 bg=c["surface"], fg=c["accent"],
                 font=("Segoe UI",9,"bold")).pack(side="right", padx=10)

        self._update_preview_size()


# -- Если homrec.py доступен — используем его реализацию UI -----------------
if _HOMREC_IMPORTED:
    try:
        from homrec import HomRecScreen as _OriginalApp
        # Наследуемся от оригинала, переопределяем только методы записи
        class HomRecScreen(_OriginalApp):

            def start_recording(self):
                """Override: использует hr_pipeline если доступен."""
                # Переиспользуем логику из HomRecApp
                HomRecApp.start_recording(self)

            def stop_recording(self):
                HomRecApp.stop_recording(self)

            def toggle_pause(self):
                HomRecApp.toggle_pause(self)

            def _capture_loop(self):
                HomRecApp._capture_loop(self)

            def _start_recording_pipeline(self, w, h, fps, ox, oy):
                HomRecApp._start_recording_pipeline(self, w, h, fps, ox, oy)

            def _start_recording_gdigrab(self, w, h, fps, ox, oy):
                HomRecApp._start_recording_gdigrab(self, w, h, fps, ox, oy)

            def _update_stats(self):
                HomRecApp._update_stats(self)

            def update_preview(self):
                HomRecApp.update_preview(self)

            def _build_codec_args(self):
                return HomRecApp._build_codec_args(self)

    except Exception as _import_err:
        log.warning(f"Could not create HomRecScreen mixin: {_import_err}")
        HomRecScreen = HomRecApp
else:
    HomRecScreen = HomRecApp


# -- Точка входа -------------------------------------------------------------
if __name__ == "__main__":
    if platform.system() == "Windows":
        try:
            import ctypes as _ct
            _mutex = _ct.windll.kernel32.CreateMutexW(None, False, "HomRec_v170_SingleInstance")
            if _ct.windll.kernel32.GetLastError() == 183:
                sys.exit(0)
        except Exception:
            pass

    if _DND_AVAILABLE:
        root = TkinterDnD.Tk()
    else:
        root = tk.Tk()

    app = HomRecScreen(root)
    root.mainloop()
