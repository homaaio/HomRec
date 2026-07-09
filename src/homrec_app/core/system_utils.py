from __future__ import annotations

import os
import sys
import ctypes
import shutil
import logging
import cv2

from ._paths import SRC_DIR

log = logging.getLogger("homrec")


def find_ffmpeg() -> str | None:
    app_dir = os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) else SRC_DIR
    for name in ('ffmpeg.exe', 'ffmpeg'):
        c = os.path.join(app_dir, name)
        if os.path.exists(c):
            return c
        if os.path.exists(name):
            return os.path.abspath(name)
    return shutil.which("ffmpeg")

def optimize_for_performance() -> None:
    try:
        import psutil, platform as _plat
        p = psutil.Process()
        p.nice(psutil.HIGH_PRIORITY_CLASS if _plat.system() == "Windows" else -10)
    except Exception:
        pass
    if sys.platform == 'win32':
        try: ctypes.windll.winmm.timeBeginPeriod(1)
        except Exception: pass
        try:
            _io = ctypes.c_ulong(3)
            ctypes.windll.ntdll.NtSetInformationProcess(
                ctypes.windll.kernel32.GetCurrentProcess(), 33,
                ctypes.byref(_io), ctypes.sizeof(_io))
        except Exception: pass
    try:
        cv2.setNumThreads(0)
        cv2.setUseOptimized(True)
    except Exception:
        pass
    import gc
    gc.set_threshold(50000, 200, 200)
    try:
        sys.setswitchinterval(0.005)
    except Exception:
        pass
    try:
        from homrec_native import NATIVE_OK, RINGBUF_OK
        log.info(f"Native extensions: core={NATIVE_OK} ringbuf={RINGBUF_OK}")
    except Exception as _e:
        log.warning(f"Native ext not loaded at startup: {_e}")


def rms_to_level_percent(raw_rms: float, floor_db: float = -55.0) -> int:
    if raw_rms <= 0:
        return 0
    import math as _math
    db = 20.0 * _math.log10(min(raw_rms, 32767) / 32767.0)
    db = max(floor_db, min(0.0, db))
    pct = (db - floor_db) / (0.0 - floor_db) * 100.0
    return max(0, min(100, int(round(pct))))
