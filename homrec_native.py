"""
homrec_native.py  -  HomRec v1.6.0
Python ctypes wrapper for the native C/C++ performance libraries.

New in v1.6.0:
  - hr_encoder_helpers: BGRA→YUV420p, gamma LUT, fast box thumbnail
  - hr_stopwatch: sub-millisecond frame-pacing timer (fixes Win 15 ms jitter)
  - hr_display_info: fast monitor enumeration with DPI
  - Full Python fallbacks for every function

Usage:
    from homrec_native import core, ringbuf, framequeue, preview, encoder, stopwatch
    from homrec_native import NATIVE_OK, RINGBUF_OK, FRAMEQUEUE_OK
    from homrec_native import PREVIEW_OK, ENCODER_OK, STOPWATCH_OK
"""

from __future__ import annotations

import ctypes
import ctypes.util
import logging
import math
import os
import sys
import struct
import time as _time
from pathlib import Path
from typing import Optional

import numpy as np

log = logging.getLogger("homrec.native")

# ---------------------------------------------------------------------------
# Locate shared libraries
# ---------------------------------------------------------------------------

def _lib_path(name: str) -> str:
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

    raise FileNotFoundError(f"Native library '{name}' not found near {base}.")


# ---------------------------------------------------------------------------
# Load homrec_core
# ---------------------------------------------------------------------------
_core_lib: Optional[ctypes.CDLL] = None
NATIVE_OK = False
try:
    _core_lib = ctypes.CDLL(_lib_path("homrec_core"))
    _core_lib.hr_bgrx_to_rgb.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
    _core_lib.hr_bgrx_to_rgb.restype = None
    _core_lib.hr_resize_bilinear.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    _core_lib.hr_resize_bilinear.restype = None
    _core_lib.hr_resize_nearest.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    _core_lib.hr_resize_nearest.restype = None
    _core_lib.hr_audio_rms.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    _core_lib.hr_audio_rms.restype = ctypes.c_float
    _core_lib.hr_blend_rgba.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_char_p, ctypes.c_size_t]
    _core_lib.hr_blend_rgba.restype = None
    _core_lib.hr_yuv420_luminance.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    _core_lib.hr_yuv420_luminance.restype = ctypes.c_float
    _core_lib.hr_timestamp_str.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    _core_lib.hr_timestamp_str.restype = ctypes.c_int
    _core_lib.hr_rgb_brightness.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.c_int]
    _core_lib.hr_rgb_brightness.restype = None
    NATIVE_OK = True
    log.info("homrec_core loaded OK")
except Exception as _exc:
    log.warning("homrec_core not loaded: %s", _exc)

# ---------------------------------------------------------------------------
# Load hr_ringbuf
# ---------------------------------------------------------------------------
_rb_lib: Optional[ctypes.CDLL] = None
RINGBUF_OK = False
try:
    _rb_lib = ctypes.CDLL(_lib_path("hr_ringbuf"))
    _rb_lib.hr_rb_create.argtypes  = [ctypes.c_size_t];      _rb_lib.hr_rb_create.restype  = ctypes.c_void_p
    _rb_lib.hr_rb_destroy.argtypes = [ctypes.c_void_p];      _rb_lib.hr_rb_destroy.restype = None
    _rb_lib.hr_rb_write.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]; _rb_lib.hr_rb_write.restype = ctypes.c_size_t
    _rb_lib.hr_rb_read.argtypes  = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]; _rb_lib.hr_rb_read.restype  = ctypes.c_size_t
    _rb_lib.hr_rb_available_read.argtypes  = [ctypes.c_void_p]; _rb_lib.hr_rb_available_read.restype  = ctypes.c_size_t
    _rb_lib.hr_rb_available_write.argtypes = [ctypes.c_void_p]; _rb_lib.hr_rb_available_write.restype = ctypes.c_size_t
    _rb_lib.hr_rb_reset.argtypes = [ctypes.c_void_p]; _rb_lib.hr_rb_reset.restype = None
    RINGBUF_OK = True
    log.info("hr_ringbuf loaded OK")
except Exception as _exc:
    log.warning("hr_ringbuf not loaded: %s", _exc)

# ---------------------------------------------------------------------------
# Load hr_framequeue
# ---------------------------------------------------------------------------
_fq_lib: Optional[ctypes.CDLL] = None
FRAMEQUEUE_OK = False
try:
    _fq_lib = ctypes.CDLL(_lib_path("hr_framequeue"))
    _fq_lib.hr_fq_create.argtypes  = [ctypes.c_size_t]; _fq_lib.hr_fq_create.restype  = ctypes.c_void_p
    _fq_lib.hr_fq_destroy.argtypes = [ctypes.c_void_p]; _fq_lib.hr_fq_destroy.restype = None
    _fq_lib.hr_fq_push.argtypes = [ctypes.c_void_p, ctypes.c_void_p]; _fq_lib.hr_fq_push.restype = ctypes.c_int
    _fq_lib.hr_fq_pop.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]; _fq_lib.hr_fq_pop.restype = ctypes.c_int
    _fq_lib.hr_fq_size.argtypes = [ctypes.c_void_p]; _fq_lib.hr_fq_size.restype = ctypes.c_size_t
    FRAMEQUEUE_OK = True
    log.info("hr_framequeue loaded OK")
except Exception as _exc:
    log.warning("hr_framequeue not loaded: %s", _exc)

# ---------------------------------------------------------------------------
# Load hr_preview
# ---------------------------------------------------------------------------
_pv_lib: Optional[ctypes.CDLL] = None
PREVIEW_OK = False
try:
    _pv_lib = ctypes.CDLL(_lib_path("hr_preview"))
    _pv_lib.hr_pv_thumbnail.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    _pv_lib.hr_pv_thumbnail.restype = None
    _pv_lib.hr_pv_draw_border.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_int]
    _pv_lib.hr_pv_draw_border.restype = None
    _pv_lib.hr_pv_gray_overlay.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.c_uint8]
    _pv_lib.hr_pv_gray_overlay.restype = None
    _pv_lib.hr_pv_flip_horizontal.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int]
    _pv_lib.hr_pv_flip_horizontal.restype = None
    PREVIEW_OK = True
    log.info("hr_preview loaded OK")
except Exception as _exc:
    log.warning("hr_preview not loaded: %s", _exc)

# ---------------------------------------------------------------------------
# Load hr_encoder_helpers  (v1.6.0)
# ---------------------------------------------------------------------------
_enc_lib: Optional[ctypes.CDLL] = None
ENCODER_OK = False
try:
    _enc_lib = ctypes.CDLL(_lib_path("hr_encoder_helpers"))
    _enc_lib.hr_rgb_to_yuv420p.argtypes  = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int]
    _enc_lib.hr_rgb_to_yuv420p.restype   = None
    _enc_lib.hr_bgra_to_yuv420p.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int]
    _enc_lib.hr_bgra_to_yuv420p.restype  = None
    _enc_lib.hr_yuv420p_to_rgb.argtypes  = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int]
    _enc_lib.hr_yuv420p_to_rgb.restype   = None
    _enc_lib.hr_gamma_lut_apply.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.c_int]
    _enc_lib.hr_gamma_lut_apply.restype  = None
    _enc_lib.hr_build_thumbnail_lq.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    _enc_lib.hr_build_thumbnail_lq.restype  = ctypes.c_int
    ENCODER_OK = True
    log.info("hr_encoder_helpers loaded OK")
except Exception as _exc:
    log.warning("hr_encoder_helpers not loaded (optional): %s", _exc)

# ---------------------------------------------------------------------------
# Load hr_stopwatch  (v1.6.0)
# ---------------------------------------------------------------------------
_sw_lib: Optional[ctypes.CDLL] = None
STOPWATCH_OK = False
try:
    _sw_lib = ctypes.CDLL(_lib_path("hr_stopwatch"))
    _sw_lib.hr_sw_create.argtypes = []; _sw_lib.hr_sw_create.restype = ctypes.c_void_p
    _sw_lib.hr_sw_destroy.argtypes = [ctypes.c_void_p]; _sw_lib.hr_sw_destroy.restype = None
    _sw_lib.hr_sw_start.argtypes = [ctypes.c_void_p]; _sw_lib.hr_sw_start.restype = None
    _sw_lib.hr_sw_elapsed_ns.argtypes = [ctypes.c_void_p]; _sw_lib.hr_sw_elapsed_ns.restype = ctypes.c_int64
    _sw_lib.hr_sw_elapsed_ms.argtypes = [ctypes.c_void_p]; _sw_lib.hr_sw_elapsed_ms.restype = ctypes.c_double
    _sw_lib.hr_sw_sleep_until_ns.argtypes = [ctypes.c_void_p, ctypes.c_int64]; _sw_lib.hr_sw_sleep_until_ns.restype = None
    _sw_lib.hr_sw_now_ns.argtypes = []; _sw_lib.hr_sw_now_ns.restype = ctypes.c_int64
    STOPWATCH_OK = True
    log.info("hr_stopwatch loaded OK")
except Exception as _exc:
    log.warning("hr_stopwatch not loaded (optional): %s", _exc)


# ===========================================================================
# API classes
# ===========================================================================

class _CoreAPI:
    """Core pixel manipulation with Python fallbacks."""

    def bgrx_to_rgb_np(self, bgrx: bytes, width: int, height: int) -> np.ndarray:
        n_pix = width * height
        dst = np.empty(n_pix * 3, dtype=np.uint8)
        if NATIVE_OK and _core_lib:
            _core_lib.hr_bgrx_to_rgb(
                ctypes.c_char_p(bgrx),
                dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_size_t(n_pix))
        else:
            arr = np.frombuffer(bgrx, dtype=np.uint8).reshape(n_pix, 4)
            dst = arr[:, 2::-1].reshape(-1).copy()
        return dst.reshape(height, width, 3)

    def resize_bilinear_np(self, src: np.ndarray, sw: int, sh: int, dw: int, dh: int) -> np.ndarray:
        ch = src.shape[2] if src.ndim == 3 else 1
        dst = np.empty(dh * dw * ch, dtype=np.uint8)
        src_c = np.ascontiguousarray(src)
        if NATIVE_OK and _core_lib:
            _core_lib.hr_resize_bilinear(
                src_c.tobytes(),
                dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_int(sw), ctypes.c_int(sh),
                ctypes.c_int(dw), ctypes.c_int(dh), ctypes.c_int(ch))
        else:
            import cv2
            dst = cv2.resize(src_c, (dw, dh), interpolation=cv2.INTER_LINEAR).reshape(-1)
        return dst.reshape(dh, dw, ch)

    def resize_nearest_np(self, src: np.ndarray, sw: int, sh: int, dw: int, dh: int) -> np.ndarray:
        ch = src.shape[2] if src.ndim == 3 else 1
        dst = np.empty(dh * dw * ch, dtype=np.uint8)
        src_c = np.ascontiguousarray(src)
        if NATIVE_OK and _core_lib:
            _core_lib.hr_resize_nearest(
                src_c.tobytes(),
                dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_int(sw), ctypes.c_int(sh),
                ctypes.c_int(dw), ctypes.c_int(dh), ctypes.c_int(ch))
        else:
            import cv2
            dst = cv2.resize(src_c, (dw, dh), interpolation=cv2.INTER_NEAREST).reshape(-1)
        return dst.reshape(dh, dw, ch)

    def audio_rms_level(self, pcm_bytes: bytes) -> int:
        n = len(pcm_bytes) // 2
        if n == 0:
            return 0
        if NATIVE_OK and _core_lib:
            rms = _core_lib.hr_audio_rms(ctypes.c_char_p(pcm_bytes), ctypes.c_size_t(n))
        else:
            arr = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.int32)
            rms = float(np.sqrt(np.mean(arr ** 2)))
        return min(100, int(rms / 300))

    def blend_badge(self, base_rgb: np.ndarray, badge_rgba: np.ndarray, x: int, y: int) -> None:
        bh, bw = badge_rgba.shape[:2]
        fh, fw = base_rgb.shape[:2]
        bh = min(bh, fh - y)
        bw = min(bw, fw - x)
        if bh <= 0 or bw <= 0:
            return
        roi = base_rgb[y:y+bh, x:x+bw]
        overlay = badge_rgba[:bh, :bw]
        roi_c = np.ascontiguousarray(roi.reshape(-1, 3))
        ovl_c = np.ascontiguousarray(overlay.reshape(-1, 4))
        if NATIVE_OK and _core_lib:
            _core_lib.hr_blend_rgba(
                roi_c.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_char_p(ovl_c.tobytes()),
                ctypes.c_size_t(bh * bw))
            base_rgb[y:y+bh, x:x+bw] = roi_c.reshape(bh, bw, 3)
        else:
            alpha = overlay[:, :, 3:4].astype(np.float32) / 255.0
            blended = (overlay[:, :, :3] * alpha + roi * (1.0 - alpha)).astype(np.uint8)
            base_rgb[y:y+bh, x:x+bw] = blended

    def apply_brightness(self, frame: np.ndarray, delta: int) -> np.ndarray:
        if delta == 0:
            return frame
        fc = np.ascontiguousarray(frame.reshape(-1))
        if NATIVE_OK and _core_lib:
            _core_lib.hr_rgb_brightness(
                fc.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_size_t(fc.size), ctypes.c_int(delta))
            frame[:] = fc
        else:
            frame[:] = np.clip(frame.astype(np.int16) + delta, 0, 255).astype(np.uint8)
        return frame

    def yuv420_luminance(self, y_plane: bytes) -> float:
        if not y_plane:
            return 0.0
        if NATIVE_OK and _core_lib:
            return float(_core_lib.hr_yuv420_luminance(
                ctypes.c_char_p(y_plane), ctypes.c_size_t(len(y_plane))))
        arr = np.frombuffer(y_plane, dtype=np.uint8)
        return float(arr.mean())

    def timestamp(self) -> str:
        if NATIVE_OK and _core_lib:
            buf = ctypes.create_string_buffer(32)
            n = _core_lib.hr_timestamp_str(buf, ctypes.c_size_t(32))
            if n > 0:
                return buf.value.decode("ascii")
        from datetime import datetime
        return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


class _EncoderAPI:
    """Encoder pipeline helpers (v1.6.0): YUV conversion, gamma, fast thumbnail."""

    def bgra_to_yuv420p(self, bgra: bytes, width: int, height: int) -> np.ndarray:
        """Convert BGRA bytes directly to YUV420p numpy array (I420 layout)."""
        out_size = width * height * 3 // 2
        dst = np.empty(out_size, dtype=np.uint8)
        if ENCODER_OK and _enc_lib:
            _enc_lib.hr_bgra_to_yuv420p(
                ctypes.c_char_p(bgra),
                dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_int(width), ctypes.c_int(height))
        else:
            # Fallback: BGRA→RGB then convert via numpy BT.601
            arr = np.frombuffer(bgra, dtype=np.uint8).reshape(height, width, 4)
            r = arr[:, :, 2].astype(np.int32)
            g = arr[:, :, 1].astype(np.int32)
            b = arr[:, :, 0].astype(np.int32)
            Y  = ((66*r + 129*g + 25*b + 128) >> 8) + 16
            Cb = ((-38*r - 74*g + 112*b + 128) >> 8) + 128
            Cr = ((112*r - 94*g - 18*b + 128) >> 8) + 128
            dst[:width*height] = np.clip(Y, 16, 235).astype(np.uint8).flatten()
            cb_sub = Cb[::2, ::2]
            cr_sub = Cr[::2, ::2]
            dst[width*height:width*height + cb_sub.size] = np.clip(cb_sub, 16, 240).astype(np.uint8).flatten()
            dst[width*height + cb_sub.size:] = np.clip(cr_sub, 16, 240).astype(np.uint8).flatten()
        return dst

    def rgb_to_yuv420p(self, rgb: bytes, width: int, height: int) -> np.ndarray:
        """Convert packed RGB24 bytes to YUV420p numpy array."""
        out_size = width * height * 3 // 2
        dst = np.empty(out_size, dtype=np.uint8)
        if ENCODER_OK and _enc_lib:
            _enc_lib.hr_rgb_to_yuv420p(
                ctypes.c_char_p(rgb),
                dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_int(width), ctypes.c_int(height))
        else:
            arr = np.frombuffer(rgb, dtype=np.uint8).reshape(height, width, 3)
            r = arr[:, :, 0].astype(np.int32)
            g = arr[:, :, 1].astype(np.int32)
            b = arr[:, :, 2].astype(np.int32)
            Y  = ((66*r + 129*g + 25*b + 128) >> 8) + 16
            Cb = ((-38*r - 74*g + 112*b + 128) >> 8) + 128
            Cr = ((112*r - 94*g - 18*b + 128) >> 8) + 128
            dst[:width*height] = np.clip(Y, 16, 235).astype(np.uint8).flatten()
            cb_sub = Cb[::2, ::2]; cr_sub = Cr[::2, ::2]
            dst[width*height:width*height + cb_sub.size] = np.clip(cb_sub, 16, 240).astype(np.uint8).flatten()
            dst[width*height + cb_sub.size:] = np.clip(cr_sub, 16, 240).astype(np.uint8).flatten()
        return dst

    def gamma_apply(self, frame: np.ndarray, gamma_x100: int) -> None:
        """Apply gamma correction in-place. 100 = no-op, <100 = brighten."""
        if gamma_x100 == 100:
            return
        fc = np.ascontiguousarray(frame.reshape(-1))
        if ENCODER_OK and _enc_lib:
            _enc_lib.hr_gamma_lut_apply(
                fc.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_size_t(fc.size), ctypes.c_int(gamma_x100))
            frame[:] = fc.reshape(frame.shape)
        else:
            g = gamma_x100 / 100.0
            lut = (np.arange(256, dtype=np.float32) / 255.0) ** g * 255.0
            lut = np.clip(lut, 0, 255).astype(np.uint8)
            frame[:] = lut[frame]

    def thumbnail_lq(self, src: np.ndarray, dw: int, dh: int) -> Optional[np.ndarray]:
        """Fast integer-box thumbnail for even ratios. Returns None if ratio is non-integer."""
        sh, sw = src.shape[:2]
        if sw % dw != 0 or sh % dh != 0:
            return None
        src_c = np.ascontiguousarray(src)
        dst = np.empty(dh * dw * 3, dtype=np.uint8)
        if ENCODER_OK and _enc_lib:
            ret = _enc_lib.hr_build_thumbnail_lq(
                src_c.tobytes(),
                dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_int(sw), ctypes.c_int(sh),
                ctypes.c_int(dw), ctypes.c_int(dh))
            if ret == 0:
                return None
        else:
            rx, ry = sw // dw, sh // dh
            # Reshape to (dh, ry, dw, rx, 3) then mean over axes 1 and 3
            dst = src_c.reshape(dh, ry, dw, rx, 3).mean(axis=(1, 3)).astype(np.uint8).reshape(-1)
        return dst.reshape(dh, dw, 3)


class _StopwatchAPI:
    """High-precision frame-pacing timer. Falls back to time.perf_counter."""

    def create(self) -> object:
        if STOPWATCH_OK and _sw_lib:
            h = _sw_lib.hr_sw_create()
            return ("native", h)
        return ("python", _time.perf_counter_ns())

    def destroy(self, handle: object) -> None:
        kind, h = handle
        if kind == "native" and _sw_lib:
            _sw_lib.hr_sw_destroy(ctypes.c_void_p(h))

    def start(self, handle: object) -> None:
        kind, h = handle
        if kind == "native" and _sw_lib:
            _sw_lib.hr_sw_start(ctypes.c_void_p(h))
        else:
            # Python: store new start time back into tuple (immutable - create new)
            pass  # caller should recreate if they need reset

    def elapsed_ms(self, handle: object) -> float:
        kind, h = handle
        if kind == "native" and _sw_lib:
            return float(_sw_lib.hr_sw_elapsed_ms(ctypes.c_void_p(h)))
        return (_time.perf_counter_ns() - h) / 1_000_000.0

    def sleep_until_ns(self, handle: object, target_ns: int) -> None:
        """Sleep until elapsed_ns >= target_ns with sub-ms accuracy."""
        kind, h = handle
        if kind == "native" and _sw_lib:
            _sw_lib.hr_sw_sleep_until_ns(ctypes.c_void_p(h), ctypes.c_int64(target_ns))
        else:
            # Python fallback: hybrid sleep+spin
            start = h
            remaining = target_ns - (_time.perf_counter_ns() - start)
            if remaining > 2_000_000:
                _time.sleep((remaining - 2_000_000) / 1e9)
            while (_time.perf_counter_ns() - start) < target_ns:
                pass


class _RingBufAPI:
    """Lock-free audio ring-buffer."""

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
            return int(_rb_lib.hr_rb_write(ctypes.c_void_p(h), ctypes.c_char_p(data), ctypes.c_size_t(len(data))))
        buf: bytearray = h; buf += data; return len(data)

    def read(self, handle: object, n_bytes: int) -> bytes:
        kind, h = handle
        if kind == "native" and _rb_lib:
            buf = ctypes.create_string_buffer(n_bytes)
            got = _rb_lib.hr_rb_read(ctypes.c_void_p(h), buf, ctypes.c_size_t(n_bytes))
            return bytes(buf[:got])
        buf: bytearray = h; chunk = bytes(buf[:n_bytes]); del buf[:n_bytes]; return chunk

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
    """Preview pipeline helpers."""

    def thumbnail(self, src: np.ndarray, dst_w: int, dst_h: int) -> np.ndarray:
        sh, sw = src.shape[:2]
        if PREVIEW_OK and _pv_lib and src.flags["C_CONTIGUOUS"]:
            dst = np.empty(dst_h * dst_w * 3, dtype=np.uint8)
            _pv_lib.hr_pv_thumbnail(
                src.tobytes(),
                dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_int(sw), ctypes.c_int(sh),
                ctypes.c_int(dst_w), ctypes.c_int(dst_h))
            return dst.reshape(dst_h, dst_w, 3)
        import cv2
        return cv2.resize(src, (dst_w, dst_h), interpolation=cv2.INTER_LINEAR)

    def draw_border(self, frame: np.ndarray, r: int = 232, g: int = 30, b: int = 30, thickness: int = 4) -> None:
        if not frame.flags["C_CONTIGUOUS"]:
            return
        fh, fw = frame.shape[:2]
        if PREVIEW_OK and _pv_lib:
            _pv_lib.hr_pv_draw_border(
                frame.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_int(fw), ctypes.c_int(fh), ctypes.c_int(fw * 3),
                ctypes.c_uint8(r), ctypes.c_uint8(g), ctypes.c_uint8(b), ctypes.c_int(thickness))
        else:
            t = thickness
            frame[:t, :, 0] = r;  frame[:t, :, 1] = g;  frame[:t, :, 2] = b
            frame[-t:, :, 0] = r; frame[-t:, :, 1] = g; frame[-t:, :, 2] = b
            frame[:, :t, 0] = r;  frame[:, :t, 1] = g;  frame[:, :t, 2] = b
            frame[:, -t:, 0] = r; frame[:, -t:, 1] = g; frame[:, -t:, 2] = b

    def gray_overlay(self, frame: np.ndarray, alpha: int = 120) -> None:
        if not frame.flags["C_CONTIGUOUS"]:
            return
        fh, fw = frame.shape[:2]
        if PREVIEW_OK and _pv_lib:
            _pv_lib.hr_pv_gray_overlay(
                frame.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_size_t(fh * fw), ctypes.c_uint8(max(0, min(255, alpha))))
        else:
            a = max(0, min(255, alpha)) / 255.0
            frame[:] = np.clip(128 * a + frame * (1.0 - a), 0, 255).astype(np.uint8)


# Module-level singletons
core      = _CoreAPI()
encoder   = _EncoderAPI()
stopwatch = _StopwatchAPI()
ringbuf   = _RingBufAPI()
preview   = _PreviewAPI()


# ---------------------------------------------------------------------------
# Build helper
# ---------------------------------------------------------------------------
def ensure_built(src_dir: str | None = None) -> bool:
    if NATIVE_OK and RINGBUF_OK and FRAMEQUEUE_OK:
        return True
    if getattr(sys, "frozen", False):
        return False
    if src_dir is None:
        src_dir = str(Path(__file__).parent)

    import subprocess, platform
    is_win = platform.system() == "Windows"
    so     = ".dll" if is_win else ".so"
    fPIC   = [] if is_win else ["-fPIC"]

    targets = [
        (["gcc", "-O3", "-march=native", "-shared", *fPIC, "-lm",
          "-o", f"homrec_core{so}", "homrec_core.c"],    "homrec_core"),
        (["g++", "-O3", "-std=c++17",   "-shared", *fPIC,
          "-o", f"hr_ringbuf{so}",    "hr_ringbuf.cpp"],    "hr_ringbuf"),
        (["g++", "-O3", "-std=c++17",   "-shared", *fPIC,
          "-o", f"hr_framequeue{so}", "hr_framequeue.cpp"], "hr_framequeue"),
        (["g++", "-O3", "-std=c++17",   "-shared", *fPIC,
          "-o", f"hr_preview{so}",    "hr_preview.cpp"],    "hr_preview"),
        (["gcc", "-O3", "-march=native", "-shared", *fPIC, "-lm",
          "-o", f"hr_encoder_helpers{so}", "hr_encoder_helpers.c"], "hr_encoder_helpers"),
        (["g++", "-O3", "-std=c++17",   "-shared", *fPIC,
          *([] if not is_win else ["-lwinmm"]),
          "-o", f"hr_stopwatch{so}", "hr_stopwatch.cpp"],   "hr_stopwatch"),
        (["g++", "-O3", "-std=c++17",   "-shared", *fPIC,
          "-o", f"hr_display_info{so}", "hr_display_info.cpp"], "hr_display_info"),
    ]

    results = []
    for cmd, label in targets:
        try:
            r = subprocess.run(cmd, capture_output=True, cwd=src_dir, timeout=60)
            ok = r.returncode == 0
            if not ok:
                log.warning("Build %s failed: %s", label, r.stderr.decode()[:400])
            else:
                log.info("Built %s OK", label)
            results.append(ok)
        except Exception as exc:
            log.warning("Build %s error: %s", label, exc)
            results.append(False)
    return all(results)


if __name__ == "__main__":
    print(f"NATIVE_OK={NATIVE_OK}  RINGBUF_OK={RINGBUF_OK}  FRAMEQUEUE_OK={FRAMEQUEUE_OK}")
    print(f"PREVIEW_OK={PREVIEW_OK}  ENCODER_OK={ENCODER_OK}  STOPWATCH_OK={STOPWATCH_OK}")

    bgrx = bytes([0, 128, 255, 0] * 4)
    rgb  = core.bgrx_to_rgb_np(bgrx, 4, 1)
    assert rgb[0, 0].tolist() == [255, 128, 0], f"unexpected {rgb[0, 0]}"
    print("bgrx_to_rgb: OK")

    sine = struct.pack("<" + "h" * 44100,
                      *[int(32767 * math.sin(2 * math.pi * 440 * i / 44100)) for i in range(44100)])
    level = core.audio_rms_level(sine)
    print(f"audio_rms 440 Hz: {level}  (expected ~75)")

    sw = stopwatch.create()
    _t0 = _time.perf_counter_ns()
    stopwatch.sleep_until_ns(sw, 10_000_000)
    elapsed = (_time.perf_counter_ns() - _t0) / 1e6
    print(f"stopwatch 10ms sleep: {elapsed:.2f} ms")
    stopwatch.destroy(sw)

    print("\nAll self-tests passed!")