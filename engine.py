from __future__ import annotations
import ctypes
import os
import sys
import logging

log = logging.getLogger("homrec.engine")

# -- Locate the DLL -----------------------------------------------------------
def _find_dll() -> str:
    if getattr(sys, "frozen", False):
        base = os.path.dirname(sys.executable)
    else:
        base = os.path.dirname(os.path.abspath(__file__))
    candidate = os.path.join(base, "homrec_engine.dll")
    if os.path.exists(candidate):
        return candidate
    raise FileNotFoundError(
        f"homrec_engine.dll not found next to the application.\n"
        f"Expected: {candidate}\n"
        f"Build it with MinGW-w64 (see BUILD.md)."
    )

# -- Load DLL -----------------------------------------------------------------
_dll: ctypes.CDLL | None = None

def load_engine() -> bool:
    """Load the C++ engine DLL. Returns True on success."""
    global _dll
    try:
        path = _find_dll()
        _dll = ctypes.CDLL(path)
        _setup_prototypes()
        log.info(f"Engine DLL loaded: {path}")
        # Set 1ms timer resolution for accurate frame timing
        _dll.HR_SetTimerResolution(1)
        return True
    except Exception as e:
        log.warning(f"Engine DLL not available: {e}")
        _dll = None
        return False

def _setup_prototypes() -> None:
    """Define argtypes/restype for every exported function."""
    d = _dll
    # HR_CaptureScreen
    d.HR_CaptureScreen.argtypes = [
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_int, ctypes.c_int
    ]
    d.HR_CaptureScreen.restype = ctypes.c_int
    # HR_PreviewStart / Stop / GetFrame
    d.HR_PreviewStart.argtypes  = [ctypes.c_int]*6
    d.HR_PreviewStart.restype   = ctypes.c_int
    d.HR_PreviewStop.argtypes   = []
    d.HR_PreviewStop.restype    = None
    d.HR_PreviewGetFrame.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
    ]
    d.HR_PreviewGetFrame.restype = ctypes.c_int
    # HR_RecordStart / Stop / IsRunning / GetFrameCount
    d.HR_RecordStart.argtypes = [
        ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p,
        ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p,
    ]
    d.HR_RecordStart.restype      = ctypes.c_int
    d.HR_RecordStop.argtypes      = []
    d.HR_RecordStop.restype       = None
    d.HR_RecordIsRunning.argtypes = []
    d.HR_RecordIsRunning.restype  = ctypes.c_int
    d.HR_RecordGetFrameCount.argtypes = []
    d.HR_RecordGetFrameCount.restype  = ctypes.c_int
    # HR_AudioStart / Stop / Levels
    d.HR_AudioStart.argtypes = [
        ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_int, ctypes.c_int,
        ctypes.c_float, ctypes.c_float,
    ]
    d.HR_AudioStart.restype      = ctypes.c_int
    d.HR_AudioStop.argtypes      = []
    d.HR_AudioStop.restype       = None
    d.HR_AudioGetMicLevel.argtypes = []
    d.HR_AudioGetMicLevel.restype  = ctypes.c_float
    d.HR_AudioGetSysLevel.argtypes = []
    d.HR_AudioGetSysLevel.restype  = ctypes.c_float
    # Monitors
    d.HR_GetMonitorCount.argtypes = []
    d.HR_GetMonitorCount.restype  = ctypes.c_int
    d.HR_GetMonitorRect.argtypes  = [
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
    ]
    d.HR_GetMonitorRect.restype = ctypes.c_int
    # Version
    d.HR_GetVersion.argtypes = [ctypes.c_char_p, ctypes.c_int]
    d.HR_GetVersion.restype  = ctypes.c_int

# -- Public Python API ---------------------------------------------------------
def is_available() -> bool:
    return _dll is not None

# -- Preview -------------------------------------------------------------------
_prev_buf: ctypes.Array | None = None
_prev_w = ctypes.c_int(0)
_prev_h = ctypes.c_int(0)

def preview_start(x: int, y: int, w: int, h: int,
                  pw: int, ph: int, fps: int = 10) -> bool:
    if not _dll: return False
    global _prev_buf
    _prev_buf = (ctypes.c_uint8 * (pw * ph * 3))()
    ret = _dll.HR_PreviewStart(x, y, w, h, pw, ph, fps)
    return ret == 0

def preview_stop() -> None:
    if _dll: _dll.HR_PreviewStop()

def preview_get_frame():
    """Returns (bytes_rgb24, width, height) or None if no new frame."""
    if not _dll or _prev_buf is None: return None
    ow, oh = ctypes.c_int(0), ctypes.c_int(0)
    got = _dll.HR_PreviewGetFrame(_prev_buf,
                                   ctypes.byref(ow), ctypes.byref(oh))
    if not got: return None
    return bytes(_prev_buf), ow.value, oh.value

# -- Recording -----------------------------------------------------------------
def record_start(
    ffmpeg_path: str, out_file: str,
    x: int, y: int, w: int, h: int, fps: int,
    codec: str = "libx264", preset: str = "ultrafast",
    crf: int = 18, pix_fmt: str = "yuv420p", hw_accel: str = "auto",
    capture_window: bool = False, window_title: str = ""
) -> bool:
    if not _dll: return False
    ret = _dll.HR_RecordStart(
        ffmpeg_path.encode(), out_file.encode(),
        x, y, w, h, fps,
        codec.encode(), preset.encode(), crf,
        pix_fmt.encode(), hw_accel.encode(),
        1 if capture_window else 0,
        window_title.encode()
    )
    return ret == 0

def record_stop() -> None:
    if _dll: _dll.HR_RecordStop()

def record_is_running() -> bool:
    if not _dll: return False
    return bool(_dll.HR_RecordIsRunning())

def record_frame_count() -> int:
    if not _dll: return 0
    return _dll.HR_RecordGetFrameCount()

# -- Audio ---------------------------------------------------------------------
def audio_start(mic_path: str, sys_path: str,
                sample_rate: int = 44100, channels: int = 2,
                mic_vol: float = 1.0, sys_vol: float = 1.0) -> bool:
    if not _dll: return False
    ret = _dll.HR_AudioStart(
        mic_path.encode(), sys_path.encode(),
        sample_rate, channels,
        ctypes.c_float(mic_vol), ctypes.c_float(sys_vol)
    )
    return ret == 0

def audio_stop() -> None:
    if _dll: _dll.HR_AudioStop()

def audio_mic_level() -> float:
    if not _dll: return 0.0
    return float(_dll.HR_AudioGetMicLevel())

def audio_sys_level() -> float:
    if not _dll: return 0.0
    return float(_dll.HR_AudioGetSysLevel())

# -- Monitors -----------------------------------------------------------------
def get_monitors() -> list[tuple[int,int,int,int]]:
    """Returns list of (x, y, w, h) for each monitor."""
    if not _dll: return [(0, 0, 1920, 1080)]
    n = _dll.HR_GetMonitorCount()
    result = []
    for i in range(n):
        x, y, w, h = ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
        _dll.HR_GetMonitorRect(i, ctypes.byref(x), ctypes.byref(y),
                                   ctypes.byref(w), ctypes.byref(h))
        result.append((x.value, y.value, w.value, h.value))
    return result
