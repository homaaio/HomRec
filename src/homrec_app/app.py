from __future__ import annotations

import os
import queue
import threading
import logging
import tkinter as tk
import mss

from hr_console_bridge import NativeConsole

from .core.optional_deps import HAS_TRAY
from .core.constants import CURRENT_VERSION, _ROOT_DIR
from .core.system_utils import optimize_for_performance, find_ffmpeg
from .mixins.recording_mixin import RecordingMixin
from .mixins.audio_mixin import AudioMixin
from .mixins.settings_mixin import SettingsMixin
from .mixins.ui_mixin import UIMixin

log = logging.getLogger("homrec")


class HomRecScreen(RecordingMixin, AudioMixin, SettingsMixin, UIMixin):
    """Main HomRec window; method bodies live in the mixins (see homrec_app/README.md)."""

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.current_language = "en"
        self.lang = self._load_language(self.current_language)
        self.root.title(self.lang["app_title"])
        self.root.geometry("1300x750")
        self.root.minsize(1200, 650)
        optimize_for_performance()
        self.set_app_icon()
        self.current_theme = "dark"
        self.colors = self.get_theme_colors("dark")
        self.apply_theme()
        self.sct = mss.mss()
        # C++ pipeline — заменяет mss+_capture_loop при наличии hr_pipeline.dll
        self.cpp_pipeline = None
        self._cpp_pipe_read_fd  = -1   # читающий конец pipe → ffmpeg stdin
        self._cpp_pipe_write_fd = -1   # пишущий конец pipe → C++ pipeline
        self._preview_queue = queue.Queue(maxsize=1)
        self._preview_running = True
        self.audio_recording = False
        self.audio_thread = None
        self.audio_frames = []
        self.audio_stream = None
        self.audio_p = None
        self.audio_channels = 1
        self.sys_audio_recording = False
        self.sys_audio_thread = None
        self.sys_audio_frames = []
        self.sys_audio_stream = None
        self.sys_audio_p = None
        self.sys_audio_filename = None
        self.sys_ffmpeg_proc = None
        self.ffmpeg_proc = None
        self.ffmpeg_reader_thread = None
        self.stop_ffmpeg_reader = False
        self.scale_factor = 0.75
        self.output_folder = os.path.join(_ROOT_DIR, "recordings")
        self.quality = 70
        self.target_fps = 15
        self.recording_mode = "balanced"
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
        self.custom_ffmpeg_args = ""
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
        self.video_format = "mp4"       # "mp4" or "mkv"
        self.separate_audio_mp3 = False  # save audio as separate .mp3
        self.core_version = CURRENT_VERSION
        self.ui_registry: dict = {}      # logical name -> widget, built by _build_ui_registry()
        self._hide_geo_cache: dict = {}  # !hide bookkeeping (geometry-manager info per hidden widget)
        self.overlays: list[dict] = []   # overlay definitions
        self.overlays_panel = None       # set once the OverlaysDockPanel is built
        self.show_audio_panel = True     # Audio Mixer panel visible (View -> Show)
        self.show_overlays_panel = True  # Overlays dock panel visible (View -> Show)
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
        self.recording = False
        self.paused = False
        self.out = None
        self.frame_count = 0
        self.start_time = 0.0
        self.recording_thread = None
        self.stop_flag = False
        self.last_frame_time = 0.0
        self.monitor_id = 1
        self.monitor_left = 0
        self.monitor_top = 0
        self.update_monitor_info()
        self.capture_mode = "desktop"
        self.capture_window_title = ""
        self.tray_icon = None
        os.makedirs(self.output_folder, exist_ok=True)
        self.ffmpeg_path = find_ffmpeg()
        if self.ffmpeg_path:
            log.info(f"FFmpeg found: {self.ffmpeg_path}")
        else:
            log.warning("FFmpeg NOT found!")
        self.create_menu()
        self.create_widgets()
        self._capture_thread = threading.Thread(target=self._capture_loop, daemon=True)
        self._capture_thread.start()
        self.update_preview()
        self.root.after(3000, self._warm_up_gpu_probe)
        self._console = NativeConsole(self)
        self.root.bind("<Control-Shift-T>", lambda e: self._console.toggle())
        self.root.bind("<Control-Shift-t>", lambda e: self._console.toggle())
        self.root.bind('<Configure>', self.on_window_resize)
        self._apply_hotkeys()
        self._setup_drag_drop()
        self._register_file_types()
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)
        self.setup_tray()
        log.info("HomRec v1.7.1 started, language: %s", self.current_language)
        self.root.after(2000, self._start_update_check)
        if getattr(self, '_first_launch', False):
            self.root.after(400, self._show_welcome_and_save)
