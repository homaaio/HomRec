from __future__ import annotations

import ctypes
import ctypes.util
import logging
import os
import sys
from pathlib import Path
from typing import Optional

log = logging.getLogger("homrec.native2")

# ---------------------------------------------------------------------------
# Library loader
# ---------------------------------------------------------------------------

def _lib_path(name: str) -> str:
    base = Path(sys.executable).parent if getattr(sys, "frozen", False) \
           else Path(__file__).parent
    for ext in (".dll", ".so", ".dylib"):
        p = base / (name + ext)
        if p.exists():
            return str(p)
    found = ctypes.util.find_library(name)
    if found:
        return found
    raise FileNotFoundError(f"Native library '{name}' not found near {base}")


def _load(name: str) -> Optional[ctypes.CDLL]:
    try:
        return ctypes.CDLL(_lib_path(name))
    except Exception as e:
        log.warning("%s not loaded: %s", name, e)
        return None


_app   = _load("hr_app_logic")
_prof  = _load("hr_profile_io")
_utils = _load("hr_ui_utils")

APP_OK   = _app   is not None
PROF_OK  = _prof  is not None
UTILS_OK = _utils is not None

# ---------------------------------------------------------------------------
# Compatibility flags expected by homrec.py
# ---------------------------------------------------------------------------

# True if at least one native library loaded successfully
NATIVE_OK   = APP_OK or PROF_OK or UTILS_OK

# GPU / encoder helpers live in hr_app_logic
ENCODER_OK  = APP_OK

# hr_audio.dll is a separate library not yet bound in this file
AUDIO_OK    = False

# hr_tools functionality is covered by hr_app_logic bindings above
TOOLS_OK    = APP_OK

# Ring-buffer DLL is not present in this build
RINGBUF_OK  = False


# ---------------------------------------------------------------------------
# tools_engine facade
# Wraps the GPU-probe, codec-args, merge and dshow functions from hr_app_logic.
# homrec.py uses:  tools_engine.probe_gpu(ffpath)
#                  tools_engine.build_codec_args(codec, quality, fps, cpu_count)
#                  tools_engine.merge_av(ffpath, video, audio)
#                  tools_engine.get_dshow_devices(ffpath)
# ---------------------------------------------------------------------------

class _ToolsEngine:
    """Python facade over hr_app_logic C++ functions used by homrec.py."""

    def probe_gpu(self, ffmpeg_path: str) -> str | None:
        return probe_gpu_encoder(ffmpeg_path)

    def build_codec_args(self, codec: str, quality: int,
                         fps: int, cpu_count: int) -> list[str]:
        return build_codec_args(codec, quality, fps, cpu_count)

    def merge_av(self, ffmpeg_path: str,
                 video_path: str, audio_path: str) -> bool:
        return merge_audio_video(ffmpeg_path, video_path, audio_path)

    def get_dshow_devices(self, ffmpeg_path: str) -> list[str]:
        """List DirectShow audio input devices via ffmpeg -list_devices."""
        import subprocess, platform as _plat, re
        if _plat.system() != "Windows":
            return []
        try:
            r = subprocess.run(
                [ffmpeg_path, "-list_devices", "true",
                 "-f", "dshow", "-i", "dummy"],
                capture_output=True, timeout=8,
                creationflags=subprocess.CREATE_NO_WINDOW,
            )
            text = (r.stdout + r.stderr).decode("utf-8", errors="replace")
            devices: list[str] = []
            in_audio = False
            for line in text.splitlines():
                if "audio" in line.lower():
                    in_audio = True
                if in_audio:
                    m = re.search(r'"([^"]+)"', line)
                    if m:
                        devices.append(m.group(1))
            return devices
        except Exception:
            return []


tools_engine = _ToolsEngine() if TOOLS_OK else None


# ---------------------------------------------------------------------------
# audio_engine facade
# hr_audio.dll is not yet loaded; methods are no-ops / return safe defaults.
# homrec.py guards every call with:  if _AOK and _ae is not None:
# ---------------------------------------------------------------------------

class _AudioEngine:
    """Stub audio engine returned when hr_audio.dll is absent."""

    def start(self, mic_vol: float = 1.0, sys_vol: float = 1.0,
              mic_mute: bool = False, sys_mute: bool = False) -> int:
        return 0  # 0 = no streams started

    def stop(self) -> None:
        pass

    def get_levels(self) -> tuple[int, int]:
        return 0, 0

    def set_volumes(self, mic_vol: float, sys_vol: float,
                    mic_mute: bool, sys_mute: bool) -> None:
        pass

    def read_mic(self) -> bytes:
        return b""

    def read_sys(self) -> bytes:
        return b""


# AUDIO_OK is False, so homrec.py will not actually call these methods,
# but the name must be importable to avoid ImportError.
audio_engine = _AudioEngine()


# ---------------------------------------------------------------------------
# core facade
# Used by the capture thread:  from homrec_native import core as _native_core
# homrec.py checks _have_native before calling, so a stub is sufficient.
# ---------------------------------------------------------------------------

class _NativeCore:
    """
    Pure-Python / NumPy fallback for the C++ capture accelerator.

    homrec.py uses two methods from this object:
      bgrx_to_rgb_np(bgra_bytes, w, h)  → numpy uint8 array (h, w, 3) RGB
      resize_bilinear_np(rgb_np, sw, sh, dw, dh) → bytes  (dw*dh*3)

    When hr_app_logic.dll is present these are backed by C++; here we
    provide equivalent NumPy implementations so the capture loop works
    correctly without the DLL.
    """

    @staticmethod
    def bgrx_to_rgb_np(bgra_bytes: bytes, w: int, h: int):
        """Convert raw BGRX bytes → numpy RGB array (h, w, 3)."""
        import numpy as np
        arr = np.frombuffer(bgra_bytes, dtype=np.uint8).reshape(h, w, 4)
        # BGRX → RGB: swap channels and drop alpha/X
        return arr[:, :, ::-1][:, :, 1:]  # [..., B G R X] → [..., R G B]

    @staticmethod
    def resize_bilinear_np(rgb_np, sw: int, sh: int, dw: int, dh: int) -> bytes:
        """Resize an (h, w, 3) numpy array to (dh, dw) and return raw bytes."""
        import numpy as np
        # Use cv2 if available (fast), fall back to pure numpy via PIL
        try:
            import cv2
            resized = cv2.resize(rgb_np, (dw, dh), interpolation=cv2.INTER_LINEAR)
            return resized.tobytes()
        except Exception:
            pass
        try:
            from PIL import Image
            img = Image.fromarray(rgb_np, "RGB")
            img = img.resize((dw, dh), Image.Resampling.BILINEAR)
            return img.tobytes()
        except Exception:
            # Last resort: nearest-neighbour via numpy
            y_idx = (np.arange(dh) * sh // dh).astype(np.int32)
            x_idx = (np.arange(dw) * sw // dw).astype(np.int32)
            return rgb_np[np.ix_(y_idx, x_idx)].tobytes()


core = _NativeCore()

# ---------------------------------------------------------------------------
# hr_app_logic bindings
# ---------------------------------------------------------------------------

if APP_OK:
    # version
    _app.hr_version_string.argtypes  = [ctypes.c_char_p, ctypes.c_int]
    _app.hr_version_string.restype   = None
    _app.hr_version_gt.argtypes      = [ctypes.c_char_p, ctypes.c_char_p]
    _app.hr_version_gt.restype       = ctypes.c_int

    # find_ffmpeg
    _app.hr_find_ffmpeg.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    _app.hr_find_ffmpeg.restype  = ctypes.c_int

    # optimize
    _app.hr_optimize_process.argtypes = []
    _app.hr_optimize_process.restype  = ctypes.c_int

    # monitor
    class _MonitorInfo(ctypes.Structure):
        _fields_ = [
            ("left",   ctypes.c_int),
            ("top",    ctypes.c_int),
            ("width",  ctypes.c_int),
            ("height", ctypes.c_int),
            ("index",  ctypes.c_int),
            ("name",   ctypes.c_char * 32),
        ]
    _app.hr_get_monitor_info.argtypes = [ctypes.c_int, ctypes.POINTER(_MonitorInfo)]
    _app.hr_get_monitor_info.restype  = ctypes.c_int
    _app.hr_monitor_count.argtypes    = []
    _app.hr_monitor_count.restype     = ctypes.c_int

    # windows
    _app.hr_enum_windows.argtypes = [ctypes.c_char_p, ctypes.c_int]
    _app.hr_enum_windows.restype  = ctypes.c_int

    # GPU probe
    _app.hr_probe_gpu_encoder.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    _app.hr_probe_gpu_encoder.restype  = ctypes.c_int

    # codec args
    _app.hr_build_codec_args.argtypes = [
        ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.c_char_p, ctypes.c_int,
    ]
    _app.hr_build_codec_args.restype = None

    # ffmpeg process
    _app.hr_launch_ffmpeg.argtypes  = [ctypes.POINTER(ctypes.c_char_p)]
    _app.hr_launch_ffmpeg.restype   = ctypes.c_void_p
    _app.hr_stop_ffmpeg.argtypes    = [ctypes.c_void_p, ctypes.c_int]
    _app.hr_stop_ffmpeg.restype     = ctypes.c_int
    _app.hr_free_ffmpeg.argtypes    = [ctypes.c_void_p]
    _app.hr_free_ffmpeg.restype     = None
    _app.hr_ffmpeg_running.argtypes = [ctypes.c_void_p]
    _app.hr_ffmpeg_running.restype  = ctypes.c_int

    # merge
    _app.hr_merge_audio_video.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]
    _app.hr_merge_audio_video.restype  = ctypes.c_int

    # timing
    _app.hr_monotonic_ms.argtypes    = []
    _app.hr_monotonic_ms.restype     = ctypes.c_int64
    _app.hr_format_elapsed.argtypes  = [ctypes.c_double, ctypes.c_char_p, ctypes.c_int]
    _app.hr_format_elapsed.restype   = None

    # file types
    _app.hr_register_file_types.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    _app.hr_register_file_types.restype  = ctypes.c_int

    # single instance
    _app.hr_acquire_single_instance.argtypes = [ctypes.c_char_p]
    _app.hr_acquire_single_instance.restype  = ctypes.c_int

    # update
    _app.hr_fetch_latest_version.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    _app.hr_fetch_latest_version.restype  = ctypes.c_int


# ---------------------------------------------------------------------------
# hr_profile_io bindings
# ---------------------------------------------------------------------------

if PROF_OK:
    # raw binary I/O
    _prof.hr_hrc_write.argtypes  = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    _prof.hr_hrc_write.restype   = ctypes.c_int
    _prof.hr_hrc_read.argtypes   = [ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
    _prof.hr_hrc_read.restype    = ctypes.c_int
    _prof.hr_hrc_detect.argtypes = [ctypes.c_char_p]
    _prof.hr_hrc_detect.restype  = ctypes.c_int

    # profile object
    _prof.hr_profile_create.argtypes  = []
    _prof.hr_profile_create.restype   = ctypes.c_void_p
    _prof.hr_profile_destroy.argtypes = [ctypes.c_void_p]
    _prof.hr_profile_destroy.restype  = None

    _prof.hr_profile_load_json.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    _prof.hr_profile_load_json.restype  = ctypes.c_int
    _prof.hr_profile_save_json.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    _prof.hr_profile_save_json.restype  = ctypes.c_int
    _prof.hr_profile_load_hrc.argtypes  = [ctypes.c_void_p, ctypes.c_char_p]
    _prof.hr_profile_load_hrc.restype   = ctypes.c_int
    _prof.hr_profile_save_hrc.argtypes  = [ctypes.c_void_p, ctypes.c_char_p]
    _prof.hr_profile_save_hrc.restype   = ctypes.c_int

    _prof.hr_profile_get_str.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    _prof.hr_profile_get_str.restype  = ctypes.c_char_p
    _prof.hr_profile_set_str.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    _prof.hr_profile_set_str.restype  = None
    _prof.hr_profile_get_int.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    _prof.hr_profile_get_int.restype  = ctypes.c_int
    _prof.hr_profile_set_int.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    _prof.hr_profile_set_int.restype  = None
    _prof.hr_profile_get_double.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    _prof.hr_profile_get_double.restype  = ctypes.c_double
    _prof.hr_profile_set_double.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_double]
    _prof.hr_profile_set_double.restype  = None

    # directory scan
    _prof.hr_scan_dir_ext.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                       ctypes.c_char_p, ctypes.c_int]
    _prof.hr_scan_dir_ext.restype  = ctypes.c_int

    # theme / lang helpers
    _prof.hr_theme_get_color.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                          ctypes.c_char_p, ctypes.c_int]
    _prof.hr_theme_get_color.restype  = ctypes.c_int
    _prof.hr_lang_get_value.argtypes  = [ctypes.c_char_p, ctypes.c_char_p,
                                          ctypes.c_char_p, ctypes.c_int]
    _prof.hr_lang_get_value.restype   = ctypes.c_int
    _prof.hr_lang_schema_version.argtypes    = [ctypes.c_char_p]
    _prof.hr_lang_schema_version.restype     = ctypes.c_int
    _prof.hr_lang_count_missing_keys.argtypes= [ctypes.c_char_p, ctypes.c_char_p]
    _prof.hr_lang_count_missing_keys.restype = ctypes.c_int


# ---------------------------------------------------------------------------
# hr_ui_utils bindings
# ---------------------------------------------------------------------------

if UTILS_OK:
    # stopwatch
    _utils.hr_stopwatch_create.argtypes  = []
    _utils.hr_stopwatch_create.restype   = ctypes.c_void_p
    _utils.hr_stopwatch_destroy.argtypes = [ctypes.c_void_p]
    _utils.hr_stopwatch_destroy.restype  = None
    _utils.hr_stopwatch_start.argtypes   = [ctypes.c_void_p]
    _utils.hr_stopwatch_start.restype    = None
    _utils.hr_stopwatch_pause.argtypes   = [ctypes.c_void_p]
    _utils.hr_stopwatch_pause.restype    = None
    _utils.hr_stopwatch_resume.argtypes  = [ctypes.c_void_p]
    _utils.hr_stopwatch_resume.restype   = None
    _utils.hr_stopwatch_elapsed_ms.argtypes = [ctypes.c_void_p]
    _utils.hr_stopwatch_elapsed_ms.restype  = ctypes.c_int64
    _utils.hr_stopwatch_format.argtypes  = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    _utils.hr_stopwatch_format.restype   = None

    # audio level
    _utils.hr_audio_rms_int16.argtypes = [ctypes.POINTER(ctypes.c_int16), ctypes.c_int]
    _utils.hr_audio_rms_int16.restype  = ctypes.c_int
    _utils.hr_lerp_color.argtypes      = [ctypes.c_float]
    _utils.hr_lerp_color.restype       = ctypes.c_uint32
    _utils.hr_peak_decay.argtypes      = [ctypes.c_int,
                                           ctypes.POINTER(ctypes.c_int),
                                           ctypes.POINTER(ctypes.c_int)]
    _utils.hr_peak_decay.restype       = None

    # rec badge
    _utils.hr_render_rec_badge.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                            ctypes.c_char_p]
    _utils.hr_render_rec_badge.restype  = None

    # sys stats
    class _SysStats(ctypes.Structure):
        _fields_ = [
            ("cpu_percent",   ctypes.c_float),
            ("ram_total_mb",  ctypes.c_uint64),
            ("ram_avail_mb",  ctypes.c_uint64),
            ("ram_percent",   ctypes.c_float),
            ("disk_total_gb", ctypes.c_uint64),
            ("disk_free_gb",  ctypes.c_uint64),
            ("disk_percent",  ctypes.c_float),
            ("cpu_count",     ctypes.c_int),
        ]
    _utils.hr_get_sys_stats.argtypes = [ctypes.c_char_p, ctypes.POINTER(_SysStats)]
    _utils.hr_get_sys_stats.restype  = ctypes.c_int

    # notify
    _utils.hr_notify_beep.argtypes   = []
    _utils.hr_notify_beep.restype    = None
    _utils.hr_flash_window.argtypes  = [ctypes.c_ssize_t, ctypes.c_int, ctypes.c_int]
    _utils.hr_flash_window.restype   = None

    # appid
    _utils.hr_set_app_user_model_id.argtypes = [ctypes.c_char_p]
    _utils.hr_set_app_user_model_id.restype  = None

    # countdown
    _utils.hr_countdown_async.argtypes = [
        ctypes.c_int,
        ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_void_p),
        ctypes.CFUNCTYPE(None, ctypes.c_void_p),
        ctypes.c_void_p,
    ]
    _utils.hr_countdown_async.restype = None

    # fps tracker
    _utils.hr_fps_tracker_create.argtypes  = []
    _utils.hr_fps_tracker_create.restype   = ctypes.c_void_p
    _utils.hr_fps_tracker_destroy.argtypes = [ctypes.c_void_p]
    _utils.hr_fps_tracker_destroy.restype  = None
    _utils.hr_fps_tracker_tick.argtypes    = [ctypes.c_void_p]
    _utils.hr_fps_tracker_tick.restype     = ctypes.c_float

    # paths / files
    _utils.hr_file_size_mb.argtypes     = [ctypes.c_char_p]
    _utils.hr_file_size_mb.restype      = ctypes.c_float
    _utils.hr_make_output_dir.argtypes  = [ctypes.c_char_p]
    _utils.hr_make_output_dir.restype   = ctypes.c_int
    _utils.hr_open_folder.argtypes      = [ctypes.c_char_p]
    _utils.hr_open_folder.restype       = None
    _utils.hr_path_exists.argtypes      = [ctypes.c_char_p]
    _utils.hr_path_exists.restype       = ctypes.c_int
    _utils.hr_filename_from_template.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                                  ctypes.c_char_p, ctypes.c_int]
    _utils.hr_filename_from_template.restype  = None


# ===========================================================================
# Pythonic wrappers  (высокоуровневый API, вызываемый из homrec.py)
# ===========================================================================

_ENC = "utf-8"


# --- Version ----------------------------------------------------------------

def version_string() -> str:
    if not APP_OK:
        return "1.6.2"
    buf = ctypes.create_string_buffer(32)
    _app.hr_version_string(buf, 32)
    return buf.value.decode(_ENC)


def version_gt(a: str, b: str) -> bool:
    if not APP_OK:
        try:
            return tuple(int(x) for x in a.split(".")) > tuple(int(x) for x in b.split("."))
        except Exception:
            return False
    return bool(_app.hr_version_gt(a.encode(_ENC), b.encode(_ENC)))


# --- find_ffmpeg ------------------------------------------------------------

def find_ffmpeg(exe_dir: str | None = None) -> str | None:
    if not APP_OK:
        return None
    buf = ctypes.create_string_buffer(1024)
    d = exe_dir.encode(_ENC) if exe_dir else None
    ok = _app.hr_find_ffmpeg(d, buf, 1024)
    return buf.value.decode(_ENC) if ok else None


# --- optimize_for_performance -----------------------------------------------

def optimize_for_performance() -> None:
    if APP_OK:
        _app.hr_optimize_process()


# --- monitor info -----------------------------------------------------------

def get_monitor_info(idx: int) -> dict | None:
    if not APP_OK:
        return None
    from ctypes import POINTER
    mi = _app.__class__.__mro__  # just to avoid re-defining _MonitorInfo
    # Re-use the already-defined struct via _app directly
    class _MI(ctypes.Structure):
        _fields_ = [("left",ctypes.c_int),("top",ctypes.c_int),
                    ("width",ctypes.c_int),("height",ctypes.c_int),
                    ("index",ctypes.c_int),("name",ctypes.c_char*32)]
    obj = _MI()
    ok = _app.hr_get_monitor_info(idx, ctypes.byref(obj))
    if not ok:
        return None
    return {"left": obj.left, "top": obj.top,
            "width": obj.width, "height": obj.height,
            "index": obj.index, "name": obj.name.decode(_ENC)}


def monitor_count() -> int:
    return _app.hr_monitor_count() if APP_OK else 1


# --- window enumeration -----------------------------------------------------

def enum_windows() -> list[str]:
    if not APP_OK:
        return []
    buf = ctypes.create_string_buffer(32768)
    count = _app.hr_enum_windows(buf, 32768)
    if count <= 0:
        return []
    titles = []
    raw = buf.raw
    pos = 0
    while pos < len(raw):
        end = raw.find(b'\x00', pos)
        if end == pos:
            break
        titles.append(raw[pos:end].decode(_ENC, errors="replace"))
        pos = end + 1
    return titles


# --- GPU probe --------------------------------------------------------------

def probe_gpu_encoder(ffmpeg_path: str) -> str | None:
    if not APP_OK:
        return None
    buf = ctypes.create_string_buffer(64)
    ok = _app.hr_probe_gpu_encoder(ffmpeg_path.encode(_ENC), buf, 64)
    return buf.value.decode(_ENC) if ok else None


# --- codec args -------------------------------------------------------------

def build_codec_args(codec: str, quality: int, fps: int, cpu_count: int) -> list[str]:
    if not APP_OK:
        return ["-c:v", codec]
    buf = ctypes.create_string_buffer(1024)
    _app.hr_build_codec_args(
        codec.encode(_ENC), quality, fps, cpu_count, buf, 1024)
    return buf.value.decode(_ENC).split()


# --- FFmpeg process ---------------------------------------------------------

class FfmpegProcess:
    """Wraps an hr_app_logic ffmpeg process handle."""

    def __init__(self, argv: list[str]) -> None:
        if not APP_OK:
            raise RuntimeError("hr_app_logic not loaded")
        enc = [a.encode(_ENC) for a in argv]
        enc.append(None)
        arr = (ctypes.c_char_p * len(enc))(*enc)
        self._handle = _app.hr_launch_ffmpeg(arr)
        if not self._handle:
            raise OSError("hr_launch_ffmpeg returned NULL")

    def running(self) -> bool:
        return bool(_app.hr_ffmpeg_running(self._handle))

    def stop(self, timeout_ms: int = 5000) -> bool:
        return bool(_app.hr_stop_ffmpeg(self._handle, timeout_ms))

    def __del__(self) -> None:
        if self._handle:
            _app.hr_free_ffmpeg(self._handle)
            self._handle = None


# --- merge audio/video ------------------------------------------------------

def merge_audio_video(ffmpeg_path: str, video_path: str, audio_path: str) -> bool:
    if not APP_OK:
        return False
    return bool(_app.hr_merge_audio_video(
        ffmpeg_path.encode(_ENC),
        video_path.encode(_ENC),
        audio_path.encode(_ENC),
    ))


# --- timing -----------------------------------------------------------------

def monotonic_ms() -> int:
    return int(_app.hr_monotonic_ms()) if APP_OK else 0


def format_elapsed(elapsed_sec: float) -> str:
    if not APP_OK:
        t = int(elapsed_sec)
        return f"{t//3600:02d}:{(t%3600)//60:02d}:{t%60:02d}"
    buf = ctypes.create_string_buffer(16)
    _app.hr_format_elapsed(elapsed_sec, buf, 16)
    return buf.value.decode(_ENC)


# --- file types registration -------------------------------------------------

def register_file_types(exe_path: str, icons_dir: str) -> bool:
    if not APP_OK:
        return False
    return bool(_app.hr_register_file_types(
        exe_path.encode(_ENC), icons_dir.encode(_ENC)))


# --- single instance --------------------------------------------------------

def acquire_single_instance(mutex_name: str) -> bool:
    if not APP_OK:
        return True
    return bool(_app.hr_acquire_single_instance(mutex_name.encode(_ENC)))


# --- update check -----------------------------------------------------------

def fetch_latest_version(repo: str) -> str | None:
    if not APP_OK:
        return None
    buf = ctypes.create_string_buffer(32)
    ok = _app.hr_fetch_latest_version(repo.encode(_ENC), buf, 32)
    return buf.value.decode(_ENC) if ok else None


# ===========================================================================
# Profile I/O wrappers
# ===========================================================================

# --- raw binary I/O ---------------------------------------------------------

def hrc_write(path: str, json_body: str, file_type: int = 0) -> bool:
    """file_type: 0=hrc, 1=hrl, 2=hrt"""
    if not PROF_OK:
        return False
    return bool(_prof.hr_hrc_write(
        path.encode(_ENC), json_body.encode(_ENC), file_type))


def hrc_read(path: str, expected_type: int = 0) -> str | None:
    """Returns JSON body string, or None on error."""
    if not PROF_OK:
        return None
    # Find required size
    needed = _prof.hr_hrc_read(path.encode(_ENC), expected_type, None, 0)
    if needed >= 0:
        return None
    needed = -needed
    buf = ctypes.create_string_buffer(needed)
    r = _prof.hr_hrc_read(path.encode(_ENC), expected_type, buf, needed)
    return buf.value.decode(_ENC) if r > 0 else None


def hrc_detect(path: str) -> str | None:
    """Returns 'hrc', 'hrl', 'hrt', or None."""
    if not PROF_OK:
        return None
    r = _prof.hr_hrc_detect(path.encode(_ENC))
    return {0: "hrc", 1: "hrl", 2: "hrt"}.get(r)


# --- Profile object ---------------------------------------------------------

class NativeProfile:
    """
    Wraps the C++ HrProfileFull struct.
    Acts as a drop-in for Python dict-based settings.
    """

    def __init__(self) -> None:
        if not PROF_OK:
            raise RuntimeError("hr_profile_io not loaded")
        self._h = _prof.hr_profile_create()
        if not self._h:
            raise MemoryError("hr_profile_create returned NULL")

    def __del__(self) -> None:
        if self._h:
            _prof.hr_profile_destroy(self._h)
            self._h = None

    # Persistence
    def load_json(self, path: str) -> bool:
        return bool(_prof.hr_profile_load_json(self._h, path.encode(_ENC)))

    def save_json(self, path: str) -> bool:
        return bool(_prof.hr_profile_save_json(self._h, path.encode(_ENC)))

    def load_hrc(self, path: str) -> bool:
        return bool(_prof.hr_profile_load_hrc(self._h, path.encode(_ENC)))

    def save_hrc(self, path: str) -> bool:
        return bool(_prof.hr_profile_save_hrc(self._h, path.encode(_ENC)))

    # Generic accessors
    def get_str(self, field: str) -> str:
        r = _prof.hr_profile_get_str(self._h, field.encode(_ENC))
        return r.decode(_ENC) if r else ""

    def set_str(self, field: str, val: str) -> None:
        _prof.hr_profile_set_str(self._h, field.encode(_ENC), val.encode(_ENC))

    def get_int(self, field: str) -> int:
        return int(_prof.hr_profile_get_int(self._h, field.encode(_ENC)))

    def set_int(self, field: str, val: int) -> None:
        _prof.hr_profile_set_int(self._h, field.encode(_ENC), int(val))

    def get_double(self, field: str) -> float:
        return float(_prof.hr_profile_get_double(self._h, field.encode(_ENC)))

    def set_double(self, field: str, val: float) -> None:
        _prof.hr_profile_set_double(self._h, field.encode(_ENC), float(val))

    # Convenience: dump to Python dict (for passing to existing Python code)
    _STR_FIELDS = [
        "output_folder", "recording_mode", "video_codec", "hw_accel",
        "enc_preset", "pix_fmt", "audio_aac_bitrate", "theme", "language",
        "ui_font", "hotkey_start_stop", "hotkey_pause", "hotkey_fullscreen",
        "filename_template",
    ]
    _INT_FIELDS = [
        "target_fps", "quality", "enc_crf", "audio_sample_rate",
        "audio_out_channels", "mic_volume", "sys_volume",
        "mic_mute", "sys_mute", "audio_enabled",
        "always_on_top", "minimize_to_tray", "countdown", "timestamp",
        "cursor", "show_summary", "notify_sound", "notify_flash",
        "auto_stop_min", "replay_buffer_sec", "auto_save_profile",
        "disable_preview", "schema_version",
    ]
    _DBL_FIELDS = ["scale_factor", "ui_scale"]

    def to_dict(self) -> dict:
        d: dict = {}
        for f in self._STR_FIELDS:
            d[f] = self.get_str(f)
        for f in self._INT_FIELDS:
            d[f] = self.get_int(f)
        for f in self._DBL_FIELDS:
            d[f] = self.get_double(f)
        return d

    def from_dict(self, d: dict) -> None:
        for f in self._STR_FIELDS:
            if f in d:
                self.set_str(f, str(d[f]))
        for f in self._INT_FIELDS:
            if f in d:
                self.set_int(f, int(d[f]))
        for f in self._DBL_FIELDS:
            if f in d:
                self.set_double(f, float(d[f]))


# --- directory scanning -----------------------------------------------------

def scan_dir_ext(dir_path: str, ext: str) -> list[str]:
    """Returns list of basenames (without ext) from dir_path matching ext."""
    if not PROF_OK:
        return []
    buf = ctypes.create_string_buffer(32768)
    count = _prof.hr_scan_dir_ext(
        dir_path.encode(_ENC), ext.encode(_ENC), buf, 32768)
    if count <= 0:
        return []
    result = []
    raw = buf.raw
    pos = 0
    while pos < len(raw):
        end = raw.find(b'\x00', pos)
        if end == pos:
            break
        result.append(raw[pos:end].decode(_ENC, errors="replace"))
        pos = end + 1
    return result


# ===========================================================================
# UI Utils wrappers
# ===========================================================================

# --- Stopwatch --------------------------------------------------------------

class Stopwatch:
    """C++-backed monotonic stopwatch with pause/resume support."""

    def __init__(self) -> None:
        if not UTILS_OK:
            raise RuntimeError("hr_ui_utils not loaded")
        self._h = _utils.hr_stopwatch_create()

    def __del__(self) -> None:
        if self._h:
            _utils.hr_stopwatch_destroy(self._h)
            self._h = None

    def start(self) -> None:
        _utils.hr_stopwatch_start(self._h)

    def pause(self) -> None:
        _utils.hr_stopwatch_pause(self._h)

    def resume(self) -> None:
        _utils.hr_stopwatch_resume(self._h)

    def elapsed_ms(self) -> int:
        return int(_utils.hr_stopwatch_elapsed_ms(self._h))

    def elapsed_sec(self) -> float:
        return self.elapsed_ms() / 1000.0

    def format(self) -> str:
        buf = ctypes.create_string_buffer(16)
        _utils.hr_stopwatch_format(self._h, buf, 16)
        return buf.value.decode(_ENC)


# --- Audio level meter helpers ----------------------------------------------

def audio_rms_int16(pcm_bytes: bytes) -> int:
    """Compute 0-100 level from raw PCM int16 bytes."""
    if not UTILS_OK:
        return 0
    n = len(pcm_bytes) // 2
    if n == 0:
        return 0
    arr = (ctypes.c_int16 * n).from_buffer_copy(pcm_bytes)
    return int(_utils.hr_audio_rms_int16(arr, n))


def lerp_color(t: float) -> str:
    """Returns '#rrggbb' string for VU meter fill colour."""
    if not UTILS_OK:
        return "#a6e3a1"
    packed = _utils.hr_lerp_color(ctypes.c_float(t))
    r = (packed >> 16) & 0xFF
    g = (packed >>  8) & 0xFF
    b =  packed        & 0xFF
    return f"#{r:02x}{g:02x}{b:02x}"


def peak_decay(level: int, peak: int, peak_decay_count: int) -> tuple[int, int]:
    """Returns (new_peak, new_peak_decay_count)."""
    if not UTILS_OK:
        return peak, peak_decay_count
    p  = ctypes.c_int(peak)
    pd = ctypes.c_int(peak_decay_count)
    _utils.hr_peak_decay(level, ctypes.byref(p), ctypes.byref(pd))
    return p.value, pd.value


# --- REC badge --------------------------------------------------------------

def render_rec_badge(bright: bool, w: int = 72, h: int = 28) -> bytes:
    """Returns raw RGBA bytes (w*h*4)."""
    if not UTILS_OK:
        return bytes(w * h * 4)
    buf = ctypes.create_string_buffer(w * h * 4)
    _utils.hr_render_rec_badge(1 if bright else 0, w, h, buf)
    return bytes(buf)


# --- System analytics -------------------------------------------------------

def get_sys_stats(disk_path: str) -> dict:
    """Returns dict with cpu_percent, ram_*, disk_* keys."""
    if not UTILS_OK:
        return {}

    class _SS(ctypes.Structure):
        _fields_ = [
            ("cpu_percent",   ctypes.c_float),
            ("ram_total_mb",  ctypes.c_uint64),
            ("ram_avail_mb",  ctypes.c_uint64),
            ("ram_percent",   ctypes.c_float),
            ("disk_total_gb", ctypes.c_uint64),
            ("disk_free_gb",  ctypes.c_uint64),
            ("disk_percent",  ctypes.c_float),
            ("cpu_count",     ctypes.c_int),
        ]

    ss = _SS()
    ok = _utils.hr_get_sys_stats(disk_path.encode(_ENC), ctypes.byref(ss))
    if not ok:
        return {}
    return {
        "cpu_percent":   round(float(ss.cpu_percent), 1),
        "cpu_count":     ss.cpu_count,
        "ram_total_mb":  ss.ram_total_mb,
        "ram_avail_mb":  ss.ram_avail_mb,
        "ram_percent":   round(float(ss.ram_percent), 1),
        "disk_total_gb": ss.disk_total_gb,
        "disk_free_gb":  ss.disk_free_gb,
        "disk_percent":  round(float(ss.disk_percent), 1),
    }


# --- Notifications ----------------------------------------------------------

def notify_beep() -> None:
    if UTILS_OK:
        _utils.hr_notify_beep()


def flash_window(hwnd: int, n_times: int = 3, interval_ms: int = 120) -> None:
    if UTILS_OK:
        _utils.hr_flash_window(hwnd, n_times, interval_ms)


def set_app_user_model_id(app_id: str) -> None:
    if UTILS_OK:
        _utils.hr_set_app_user_model_id(app_id.encode(_ENC))


# --- FPS tracker ------------------------------------------------------------

class FpsTracker:
    def __init__(self) -> None:
        if not UTILS_OK:
            raise RuntimeError("hr_ui_utils not loaded")
        self._h = _utils.hr_fps_tracker_create()

    def __del__(self) -> None:
        if self._h:
            _utils.hr_fps_tracker_destroy(self._h)
            self._h = None

    def tick(self) -> float:
        return float(_utils.hr_fps_tracker_tick(self._h))


# --- Path / file helpers ----------------------------------------------------

def file_size_mb(path: str) -> float:
    if not UTILS_OK:
        try:
            return os.path.getsize(path) / (1024 * 1024)
        except Exception:
            return -1.0
    return float(_utils.hr_file_size_mb(path.encode(_ENC)))


def make_output_dir(path: str) -> bool:
    if not UTILS_OK:
        os.makedirs(path, exist_ok=True)
        return True
    return bool(_utils.hr_make_output_dir(path.encode(_ENC)))


def open_folder(path: str) -> None:
    if UTILS_OK:
        _utils.hr_open_folder(path.encode(_ENC))
    else:
        import subprocess
        subprocess.Popen(["xdg-open", path])


def path_exists(path: str) -> bool:
    if not UTILS_OK:
        return os.path.exists(path)
    return bool(_utils.hr_path_exists(path.encode(_ENC)))


def filename_from_template(template: str, folder: str) -> str:
    if not UTILS_OK:
        from datetime import datetime
        now = datetime.now()
        name = template.replace("{date}", now.strftime("%Y%m%d")) \
                       .replace("{time}", now.strftime("%H%M%S"))
        return os.path.join(folder, name + ".mp4")
    buf = ctypes.create_string_buffer(512)
    _utils.hr_filename_from_template(
        template.encode(_ENC), folder.encode(_ENC), buf, 512)
    return buf.value.decode(_ENC)


log.info(
    "homrec_native2 loaded: app=%s prof=%s utils=%s",
    APP_OK, PROF_OK, UTILS_OK,
)