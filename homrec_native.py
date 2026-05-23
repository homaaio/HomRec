"""
homrec_native.py  -  HomRec v1.5.0
Python ctypes wrapper for the native C/C++ performance libraries.

Usage:
    from homrec_native import core, ringbuf, framequeue, preview
    from homrec_native import NATIVE_OK, RINGBUF_OK, FRAMEQUEUE_OK, PREVIEW_OK

    # Convert BGRX screenshot to RGB numpy array
    rgb = core.bgrx_to_rgb_np(bgrx_bytes, width, height)

    # Resize a frame
    small = core.resize_bilinear_np(rgb_np, src_w, src_h, dst_w, dst_h)

    # Compute audio RMS level (0-100)
    level = core.audio_rms_level(pcm_bytes)

    # Lock-free audio ring buffer
    rb = ringbuf.create(2 * 1024 * 1024)
    ringbuf.write(rb, pcm_bytes)
    data = ringbuf.read(rb, 4096)
    ringbuf.destroy(rb)

    # Preview helpers
    preview.draw_border(rgb_np, r=232, g=30, b=30, thickness=4)

All functions fall back gracefully to pure-Python / numpy equivalents when
the shared library cannot be loaded (e.g. first run before compilation).
"""

from __future__ import annotations

import ctypes
import ctypes.util
import logging
import math
import os
import sys
import struct
from pathlib import Path
from typing import Optional

import numpy as np

log = logging.getLogger("homrec.native")

# ---------------------------------------------------------------------------
# Locate shared libraries
# ---------------------------------------------------------------------------

def _lib_path(name: str) -> str:
    """Find <name>.so / <name>.dll next to this file or the .exe."""
    if getattr(sys, "frozen", False):
        base = Path(sys.executable).parent
    else:
        base = Path(__file__).parent

    for ext in (".dll", ".so", ".dylib"):
        p = base / (name + ext)
        if p.exists():
            return str(p)

    found = ctypes.util.find_library(name)
    if found:
        return found

    raise FileNotFoundError(
        f"Native library '{name}' not found near {base}. "
        f"Run `python build_native.py` to compile."
    )


# ---------------------------------------------------------------------------
# Load homrec_core
# ---------------------------------------------------------------------------

_core_lib: Optional[ctypes.CDLL] = None
NATIVE_OK = False

try:
    _core_lib = ctypes.CDLL(_lib_path("homrec_core"))

    # void hr_bgrx_to_rgb(const uint8_t*, uint8_t*, size_t)
    _core_lib.hr_bgrx_to_rgb.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
    ]
    _core_lib.hr_bgrx_to_rgb.restype = None

    # void hr_resize_bilinear(src, dst, sw, sh, dw, dh, ch)
    _core_lib.hr_resize_bilinear.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_int, ctypes.c_int,
        ctypes.c_int, ctypes.c_int,
        ctypes.c_int,
    ]
    _core_lib.hr_resize_bilinear.restype = None

    # void hr_resize_nearest(src, dst, sw, sh, dw, dh, ch)
    _core_lib.hr_resize_nearest.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_int, ctypes.c_int,
        ctypes.c_int, ctypes.c_int,
        ctypes.c_int,
    ]
    _core_lib.hr_resize_nearest.restype = None

    # float hr_audio_rms(const int16_t*, size_t)
    _core_lib.hr_audio_rms.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    _core_lib.hr_audio_rms.restype = ctypes.c_float

    # void hr_blend_rgba(uint8_t*, const uint8_t*, size_t)
    _core_lib.hr_blend_rgba.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_char_p,
        ctypes.c_size_t,
    ]
    _core_lib.hr_blend_rgba.restype = None

    # float hr_yuv420_luminance(const uint8_t*, size_t)
    _core_lib.hr_yuv420_luminance.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    _core_lib.hr_yuv420_luminance.restype = ctypes.c_float

    # int hr_timestamp_str(char*, size_t)
    _core_lib.hr_timestamp_str.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    _core_lib.hr_timestamp_str.restype = ctypes.c_int

    # void hr_rgb_brightness(uint8_t*, size_t, int)
    _core_lib.hr_rgb_brightness.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.c_int,
    ]
    _core_lib.hr_rgb_brightness.restype = None

    NATIVE_OK = True
    log.info("homrec_core native library loaded OK")

except Exception as _exc:
    log.warning("homrec_core not loaded, using Python fallbacks: %s", _exc)


# ---------------------------------------------------------------------------
# Load hr_ringbuf
# ---------------------------------------------------------------------------

_rb_lib: Optional[ctypes.CDLL] = None
RINGBUF_OK = False

try:
    _rb_lib = ctypes.CDLL(_lib_path("hr_ringbuf"))

    _rb_lib.hr_rb_create.argtypes  = [ctypes.c_size_t]
    _rb_lib.hr_rb_create.restype   = ctypes.c_void_p

    _rb_lib.hr_rb_destroy.argtypes = [ctypes.c_void_p]
    _rb_lib.hr_rb_destroy.restype  = None

    _rb_lib.hr_rb_write.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t
    ]
    _rb_lib.hr_rb_write.restype = ctypes.c_size_t

    _rb_lib.hr_rb_read.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t
    ]
    _rb_lib.hr_rb_read.restype = ctypes.c_size_t

    _rb_lib.hr_rb_available_read.argtypes  = [ctypes.c_void_p]
    _rb_lib.hr_rb_available_read.restype   = ctypes.c_size_t

    _rb_lib.hr_rb_available_write.argtypes = [ctypes.c_void_p]
    _rb_lib.hr_rb_available_write.restype  = ctypes.c_size_t

    _rb_lib.hr_rb_reset.argtypes = [ctypes.c_void_p]
    _rb_lib.hr_rb_reset.restype  = None

    RINGBUF_OK = True
    log.info("hr_ringbuf native library loaded OK")

except Exception as _exc:
    log.warning("hr_ringbuf not loaded: %s", _exc)


# ---------------------------------------------------------------------------
# Load hr_framequeue
# ---------------------------------------------------------------------------

_fq_lib: Optional[ctypes.CDLL] = None
FRAMEQUEUE_OK = False

try:
    _fq_lib = ctypes.CDLL(_lib_path("hr_framequeue"))

    _fq_lib.hr_fq_create.argtypes  = [ctypes.c_size_t]
    _fq_lib.hr_fq_create.restype   = ctypes.c_void_p

    _fq_lib.hr_fq_destroy.argtypes = [ctypes.c_void_p]
    _fq_lib.hr_fq_destroy.restype  = None

    _fq_lib.hr_fq_push.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _fq_lib.hr_fq_push.restype  = ctypes.c_int

    _fq_lib.hr_fq_pop.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)
    ]
    _fq_lib.hr_fq_pop.restype  = ctypes.c_int

    _fq_lib.hr_fq_size.argtypes = [ctypes.c_void_p]
    _fq_lib.hr_fq_size.restype  = ctypes.c_size_t

    FRAMEQUEUE_OK = True
    log.info("hr_framequeue native library loaded OK")

except Exception as _exc:
    log.warning("hr_framequeue not loaded: %s", _exc)


# ---------------------------------------------------------------------------
# Load hr_preview
# ---------------------------------------------------------------------------

_pv_lib: Optional[ctypes.CDLL] = None
PREVIEW_OK = False

try:
    _pv_lib = ctypes.CDLL(_lib_path("hr_preview"))

    _pv_lib.hr_pv_thumbnail.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_int, ctypes.c_int,
        ctypes.c_int, ctypes.c_int,
    ]
    _pv_lib.hr_pv_thumbnail.restype = None

    _pv_lib.hr_pv_draw_border.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8,
        ctypes.c_int,
    ]
    _pv_lib.hr_pv_draw_border.restype = None

    _pv_lib.hr_pv_gray_overlay.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_size_t,
        ctypes.c_uint8,
    ]
    _pv_lib.hr_pv_gray_overlay.restype = None

    _pv_lib.hr_pv_flip_horizontal.argtypes = [
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_int, ctypes.c_int,
    ]
    _pv_lib.hr_pv_flip_horizontal.restype = None

    PREVIEW_OK = True
    log.info("hr_preview native library loaded OK")

except Exception as _exc:
    log.warning("hr_preview not loaded (optional): %s", _exc)


# ===========================================================================
#  High-level Python API
# ===========================================================================

class _CoreAPI:
    """Pixel / audio helpers backed by C or pure-Python fallback."""

    # -----------------------------------------------------------------------
    # BGRX -> numpy RGB
    # -----------------------------------------------------------------------
    def bgrx_to_rgb_np(self, bgrx: bytes, width: int, height: int) -> np.ndarray:
        """Convert raw BGRX screenshot bytes to an (H, W, 3) uint8 numpy array."""
        npix = width * height
        if NATIVE_OK and _core_lib:
            dst = np.empty(npix * 3, dtype=np.uint8)
            _core_lib.hr_bgrx_to_rgb(
                ctypes.c_char_p(bgrx),
                dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_size_t(npix),
            )
            return dst.reshape(height, width, 3)
        # Pure-numpy fallback
        arr = np.frombuffer(bgrx, dtype=np.uint8).reshape(height, width, 4)
        return arr[:, :, [2, 1, 0]].copy()  # BGR -> RGB, drop X

    # -----------------------------------------------------------------------
    # Bilinear resize
    # -----------------------------------------------------------------------
    def resize_bilinear_np(
        self,
        src: np.ndarray,
        src_w: int,
        src_h: int,
        dst_w: int,
        dst_h: int,
    ) -> np.ndarray:
        """Resize RGB numpy array using native bilinear filter (fallback: cv2)."""
        if NATIVE_OK and _core_lib and src.flags["C_CONTIGUOUS"]:
            ch = src.shape[2] if src.ndim == 3 else 1
            dst = np.empty(dst_h * dst_w * ch, dtype=np.uint8)
            _core_lib.hr_resize_bilinear(
                src.tobytes(),
                dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_int(src_w), ctypes.c_int(src_h),
                ctypes.c_int(dst_w), ctypes.c_int(dst_h),
                ctypes.c_int(ch),
            )
            return dst.reshape(dst_h, dst_w, ch)
        import cv2
        return cv2.resize(src, (dst_w, dst_h), interpolation=cv2.INTER_LINEAR)

    # -----------------------------------------------------------------------
    # Nearest-neighbour resize (recording mode - minimal CPU)
    # -----------------------------------------------------------------------
    def resize_nearest_np(
        self,
        src: np.ndarray,
        src_w: int,
        src_h: int,
        dst_w: int,
        dst_h: int,
    ) -> np.ndarray:
        if NATIVE_OK and _core_lib and src.flags["C_CONTIGUOUS"]:
            ch = src.shape[2] if src.ndim == 3 else 1
            dst = np.empty(dst_h * dst_w * ch, dtype=np.uint8)
            _core_lib.hr_resize_nearest(
                src.tobytes(),
                dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_int(src_w), ctypes.c_int(src_h),
                ctypes.c_int(dst_w), ctypes.c_int(dst_h),
                ctypes.c_int(ch),
            )
            return dst.reshape(dst_h, dst_w, ch)
        import cv2
        return cv2.resize(src, (dst_w, dst_h), interpolation=cv2.INTER_NEAREST)

    # -----------------------------------------------------------------------
    # Audio RMS level
    # -----------------------------------------------------------------------
    def audio_rms_level(self, pcm_bytes: bytes) -> int:
        """Return audio level 0-100 from raw PCM-s16 bytes."""
        if not pcm_bytes:
            return 0
        if NATIVE_OK and _core_lib:
            n_samples = len(pcm_bytes) // 2
            rms = _core_lib.hr_audio_rms(
                ctypes.c_char_p(pcm_bytes),
                ctypes.c_size_t(n_samples),
            )
        else:
            try:
                import audioop
                rms = audioop.rms(pcm_bytes, 2)
            except Exception:
                arr = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.float32)
                rms = float(np.sqrt(np.mean(arr ** 2)))
        return min(100, int(rms / 300))

    # -----------------------------------------------------------------------
    # Alpha-blend RGBA badge onto RGB frame
    # -----------------------------------------------------------------------
    def blend_badge(
        self,
        base_rgb: np.ndarray,
        overlay_rgba: np.ndarray,
        x: int,
        y: int,
    ) -> None:
        """Paste overlay_rgba onto base_rgb at (x, y) in-place."""
        oh, ow = overlay_rgba.shape[:2]
        bh, bw = base_rgb.shape[:2]
        x2 = min(x + ow, bw)
        y2 = min(y + oh, bh)
        if x2 <= x or y2 <= y:
            return
        region = base_rgb[y:y2, x:x2]
        badge  = overlay_rgba[:y2 - y, :x2 - x]
        if NATIVE_OK and _core_lib:
            region_c = np.ascontiguousarray(region)
            badge_c  = np.ascontiguousarray(badge)
            npix = region_c.shape[0] * region_c.shape[1]
            _core_lib.hr_blend_rgba(
                region_c.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_char_p(badge_c.tobytes()),
                ctypes.c_size_t(npix),
            )
            base_rgb[y:y2, x:x2] = region_c
        else:
            # numpy src-over fallback
            alpha = badge[..., 3:4].astype(np.float32) / 255.0
            region[:] = np.clip(
                badge[..., :3] * alpha + region * (1.0 - alpha),
                0, 255
            ).astype(np.uint8)

    # -----------------------------------------------------------------------
    # Timestamp string
    # -----------------------------------------------------------------------
    def timestamp(self) -> str:
        """Return 'YYYY-MM-DD HH:MM:SS' via C or datetime fallback."""
        if NATIVE_OK and _core_lib:
            buf = ctypes.create_string_buffer(32)
            _core_lib.hr_timestamp_str(buf, ctypes.c_size_t(32))
            return buf.value.decode("ascii", errors="replace")
        from datetime import datetime
        return datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    # -----------------------------------------------------------------------
    # RGB brightness adjustment
    # -----------------------------------------------------------------------
    def rgb_brightness(self, frame: np.ndarray, delta: int) -> np.ndarray:
        """Apply brightness offset to RGB frame in-place. Returns frame."""
        if delta == 0:
            return frame
        fc = np.ascontiguousarray(frame)
        if NATIVE_OK and _core_lib:
            _core_lib.hr_rgb_brightness(
                fc.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_size_t(fc.size),
                ctypes.c_int(delta),
            )
            frame[:] = fc
        else:
            frame[:] = np.clip(frame.astype(np.int16) + delta, 0, 255).astype(np.uint8)
        return frame

    # -----------------------------------------------------------------------
    # YUV420 luminance
    # -----------------------------------------------------------------------
    def yuv420_luminance(self, y_plane: bytes) -> float:
        """Return average luma (0-255) of a YUV420 Y-plane."""
        if not y_plane:
            return 0.0
        if NATIVE_OK and _core_lib:
            return float(_core_lib.hr_yuv420_luminance(
                ctypes.c_char_p(y_plane),
                ctypes.c_size_t(len(y_plane)),
            ))
        arr = np.frombuffer(y_plane, dtype=np.uint8)
        return float(arr.mean())


class _RingBufAPI:
    """Lock-free audio ring-buffer backed by C++ or Python bytearray fallback."""

    def create(self, capacity: int = 2 * 1024 * 1024) -> object:
        if RINGBUF_OK and _rb_lib:
            h = _rb_lib.hr_rb_create(ctypes.c_size_t(capacity))
            return ("native", h)
        return ("python", bytearray())

    def destroy(self, handle: object) -> None:
        kind, h = handle
        if kind == "native" and _rb_lib:
            _rb_lib.hr_rb_destroy(ctypes.c_void_p(h))

    def write(self, handle: object, data: bytes) -> int:
        kind, h = handle
        if kind == "native" and _rb_lib:
            return int(_rb_lib.hr_rb_write(
                ctypes.c_void_p(h),
                ctypes.c_char_p(data),
                ctypes.c_size_t(len(data)),
            ))
        buf: bytearray = h
        buf += data
        return len(data)

    def read(self, handle: object, n_bytes: int) -> bytes:
        kind, h = handle
        if kind == "native" and _rb_lib:
            buf = ctypes.create_string_buffer(n_bytes)
            got = _rb_lib.hr_rb_read(
                ctypes.c_void_p(h), buf, ctypes.c_size_t(n_bytes)
            )
            return bytes(buf[:got])
        buf: bytearray = h
        chunk = bytes(buf[:n_bytes])
        del buf[:n_bytes]
        return chunk

    def available(self, handle: object) -> int:
        kind, h = handle
        if kind == "native" and _rb_lib:
            return int(_rb_lib.hr_rb_available_read(ctypes.c_void_p(h)))
        return len(h)

    def reset(self, handle: object) -> None:
        kind, h = handle
        if kind == "native" and _rb_lib:
            _rb_lib.hr_rb_reset(ctypes.c_void_p(h))
        else:
            h.clear()


class _PreviewAPI:
    """Preview pipeline helpers backed by hr_preview.so or numpy fallbacks."""

    def thumbnail(
        self,
        src: np.ndarray,
        dst_w: int,
        dst_h: int,
    ) -> np.ndarray:
        """Downscale RGB frame for preview using box/bilinear filter."""
        sh, sw = src.shape[:2]
        if PREVIEW_OK and _pv_lib and src.flags["C_CONTIGUOUS"]:
            dst = np.empty(dst_h * dst_w * 3, dtype=np.uint8)
            _pv_lib.hr_pv_thumbnail(
                src.tobytes(),
                dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_int(sw), ctypes.c_int(sh),
                ctypes.c_int(dst_w), ctypes.c_int(dst_h),
            )
            return dst.reshape(dst_h, dst_w, 3)
        import cv2
        return cv2.resize(src, (dst_w, dst_h), interpolation=cv2.INTER_LINEAR)

    def draw_border(
        self,
        frame: np.ndarray,
        r: int = 232,
        g: int = 30,
        b: int = 30,
        thickness: int = 4,
    ) -> None:
        """Draw coloured border on RGB frame in-place."""
        if not frame.flags["C_CONTIGUOUS"]:
            return
        h, w = frame.shape[:2]
        stride = w * 3
        if PREVIEW_OK and _pv_lib:
            _pv_lib.hr_pv_draw_border(
                frame.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_int(w), ctypes.c_int(h), ctypes.c_int(stride),
                ctypes.c_uint8(r), ctypes.c_uint8(g), ctypes.c_uint8(b),
                ctypes.c_int(thickness),
            )
        else:
            # numpy fallback
            t = thickness
            frame[:t, :, 0] = r; frame[:t, :, 1] = g; frame[:t, :, 2] = b
            frame[-t:, :, 0] = r; frame[-t:, :, 1] = g; frame[-t:, :, 2] = b
            frame[:, :t, 0] = r; frame[:, :t, 1] = g; frame[:, :t, 2] = b
            frame[:, -t:, 0] = r; frame[:, -t:, 1] = g; frame[:, -t:, 2] = b

    def gray_overlay(self, frame: np.ndarray, alpha: int = 120) -> None:
        """Blend a grey veil over RGB frame in-place (paused state)."""
        if not frame.flags["C_CONTIGUOUS"]:
            return
        h, w = frame.shape[:2]
        if PREVIEW_OK and _pv_lib:
            _pv_lib.hr_pv_gray_overlay(
                frame.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_size_t(h * w),
                ctypes.c_uint8(max(0, min(255, alpha))),
            )
        else:
            a = max(0, min(255, alpha)) / 255.0
            frame[:] = np.clip(128 * a + frame * (1.0 - a), 0, 255).astype(np.uint8)


# ---------------------------------------------------------------------------
# Module-level singleton instances
# ---------------------------------------------------------------------------
core       = _CoreAPI()
ringbuf    = _RingBufAPI()
preview    = _PreviewAPI()


# ---------------------------------------------------------------------------
# Build helper (called once at startup if libraries are missing)
# ---------------------------------------------------------------------------
def ensure_built(src_dir: str | None = None) -> bool:
    """
    Try to compile the C/C++ sources if the .so/.dll files are missing.
    Returns True if everything is available after the call.
    """
    if NATIVE_OK and RINGBUF_OK and FRAMEQUEUE_OK:
        return True

    if getattr(sys, "frozen", False):
        return False   # cannot compile inside a frozen .exe

    if src_dir is None:
        src_dir = str(Path(__file__).parent)

    import subprocess
    import platform

    is_win = platform.system() == "Windows"
    so     = ".dll" if is_win else ".so"
    fPIC   = [] if is_win else ["-fPIC"]

    targets = [
        (["gcc",  "-O3", "-march=native", "-shared", *fPIC, "-lm",
           "-o", f"homrec_core{so}",    "homrec_core.c"],    "homrec_core"),
        (["g++",  "-O3", "-std=c++17",   "-shared", *fPIC,
           "-o", f"hr_ringbuf{so}",     "hr_ringbuf.cpp"],   "hr_ringbuf"),
        (["g++",  "-O3", "-std=c++17",   "-shared", *fPIC,
           "-o", f"hr_framequeue{so}",  "hr_framequeue.cpp"],"hr_framequeue"),
        (["g++",  "-O3", "-std=c++17",   "-shared", *fPIC,
           "-o", f"hr_preview{so}",     "hr_preview.cpp"],   "hr_preview"),
    ]

    results = []
    for cmd, label in targets:
        try:
            r = subprocess.run(cmd, capture_output=True, cwd=src_dir, timeout=60)
            ok = r.returncode == 0
            if ok:
                log.info("Built %s OK", label)
            else:
                log.warning("Build %s failed: %s", label, r.stderr.decode()[:400])
            results.append(ok)
        except Exception as exc:
            log.warning("Build %s error: %s", label, exc)
            results.append(False)

    return all(results)


# ---------------------------------------------------------------------------
# Quick self-test
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    print(f"NATIVE_OK={NATIVE_OK}  RINGBUF_OK={RINGBUF_OK}  "
          f"FRAMEQUEUE_OK={FRAMEQUEUE_OK}  PREVIEW_OK={PREVIEW_OK}")

    # BGRX -> RGB
    bgrx = bytes([0, 128, 255, 0] * 4)   # 4 pixels: BGRX -> RGB should be [255, 128, 0]
    rgb  = core.bgrx_to_rgb_np(bgrx, 4, 1)
    assert rgb[0, 0].tolist() == [255, 128, 0], f"unexpected {rgb[0, 0]}"
    print("bgrx_to_rgb: OK")

    # Audio RMS
    sine = struct.pack(
        "<" + "h" * 44100,
        *[int(32767 * math.sin(2 * math.pi * 440 * i / 44100)) for i in range(44100)],
    )
    level = core.audio_rms_level(sine)
    print(f"audio_rms for 440 Hz sine: {level}  (expected ~75)")
    assert 50 < level < 95

    # Ring buffer
    rb   = ringbuf.create(4096)
    n    = ringbuf.write(rb, b"hello world")
    assert n == 11
    data = ringbuf.read(rb, 11)
    assert data == b"hello world", data
    ringbuf.destroy(rb)
    print("ringbuf: OK")

    # Timestamp
    ts = core.timestamp()
    assert len(ts) == 19 and ts[4] == "-", f"bad timestamp: {ts!r}"
    print(f"timestamp: {ts!r}  OK")

    print("\nAll self-tests passed!")
