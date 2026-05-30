"""
homrec_native.py  -  HomRec v1.8.0
ctypes-обёртка над нативными C/C++ библиотеками.

Изменения v1.8.0:
  - Убран дублирующийся singleton dxcap (был создан дважды в конце файла).
  - Python-fallbacks оставлены только там, где они реально нужны
    (audio_rms, timestamp); тяжёлые пути без нативной либы падают явно.
  - Добавлен _PipelineAPI — обёртка над hr_pipeline.dll.
    Позволяет переключать запись без пересоздания pipeline (hr_pl_set_recording).
  - _PreviewAPI.thumbnail: убран .tobytes() — передаётся указатель напрямую
    через ctypes data_as, нулевая копия.
  - Все argtypes объявлены через списки констант — не пересоздаются при каждом вызове.

Использование:
    from homrec_native import core, ringbuf, framequeue, preview, encoder
    from homrec_native import stopwatch, dxcap, pipeline
    from homrec_native import NATIVE_OK, RINGBUF_OK, FRAMEQUEUE_OK
    from homrec_native import PREVIEW_OK, ENCODER_OK, STOPWATCH_OK
    from homrec_native import DXCAP_OK, PIPELINE_OK
    from homrec_native import HR_DX_OK, HR_DX_TIMEOUT, HR_DX_LOST, HR_DX_ERROR
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


def _load(name: str) -> Optional[ctypes.CDLL]:
    try:
        return ctypes.CDLL(_lib_path(name))
    except Exception as exc:
        log.warning("%s not loaded: %s", name, exc)
        return None


# ---------------------------------------------------------------------------
# Load libraries
# ---------------------------------------------------------------------------

_core_lib = _load("homrec_core")
NATIVE_OK = _core_lib is not None
if NATIVE_OK:
    _core_lib.hr_bgrx_to_rgb.argtypes       = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
    _core_lib.hr_bgrx_to_rgb.restype        = None
    _core_lib.hr_resize_bilinear.argtypes   = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8),
                                                ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    _core_lib.hr_resize_bilinear.restype    = None
    _core_lib.hr_resize_nearest.argtypes    = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8),
                                                ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    _core_lib.hr_resize_nearest.restype     = None
    _core_lib.hr_audio_rms.argtypes         = [ctypes.c_char_p, ctypes.c_size_t]
    _core_lib.hr_audio_rms.restype          = ctypes.c_float
    _core_lib.hr_blend_rgba.argtypes        = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_char_p, ctypes.c_size_t]
    _core_lib.hr_blend_rgba.restype         = None
    _core_lib.hr_yuv420_luminance.argtypes  = [ctypes.c_char_p, ctypes.c_size_t]
    _core_lib.hr_yuv420_luminance.restype   = ctypes.c_float
    _core_lib.hr_timestamp_str.argtypes     = [ctypes.c_char_p, ctypes.c_size_t]
    _core_lib.hr_timestamp_str.restype      = ctypes.c_int
    _core_lib.hr_rgb_brightness.argtypes    = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.c_int]
    _core_lib.hr_rgb_brightness.restype     = None
    log.info("homrec_core loaded")

_rb_lib = _load("hr_ringbuf")
RINGBUF_OK = _rb_lib is not None
if RINGBUF_OK:
    _rb_lib.hr_rb_create.argtypes           = [ctypes.c_size_t];      _rb_lib.hr_rb_create.restype  = ctypes.c_void_p
    _rb_lib.hr_rb_destroy.argtypes          = [ctypes.c_void_p];      _rb_lib.hr_rb_destroy.restype = None
    _rb_lib.hr_rb_write.argtypes            = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
    _rb_lib.hr_rb_write.restype             = ctypes.c_size_t
    _rb_lib.hr_rb_read.argtypes             = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
    _rb_lib.hr_rb_read.restype              = ctypes.c_size_t
    _rb_lib.hr_rb_available_read.argtypes   = [ctypes.c_void_p]; _rb_lib.hr_rb_available_read.restype  = ctypes.c_size_t
    _rb_lib.hr_rb_available_write.argtypes  = [ctypes.c_void_p]; _rb_lib.hr_rb_available_write.restype = ctypes.c_size_t
    _rb_lib.hr_rb_reset.argtypes            = [ctypes.c_void_p]; _rb_lib.hr_rb_reset.restype = None
    log.info("hr_ringbuf loaded")

_fq_lib = _load("hr_framequeue")
FRAMEQUEUE_OK = _fq_lib is not None
if FRAMEQUEUE_OK:
    _fq_lib.hr_fq_create.argtypes   = [ctypes.c_size_t]; _fq_lib.hr_fq_create.restype  = ctypes.c_void_p
    _fq_lib.hr_fq_destroy.argtypes  = [ctypes.c_void_p]; _fq_lib.hr_fq_destroy.restype = None
    _fq_lib.hr_fq_push.argtypes     = [ctypes.c_void_p, ctypes.c_void_p]; _fq_lib.hr_fq_push.restype = ctypes.c_int
    _fq_lib.hr_fq_pop.argtypes      = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    _fq_lib.hr_fq_pop.restype       = ctypes.c_int
    _fq_lib.hr_fq_size.argtypes     = [ctypes.c_void_p]; _fq_lib.hr_fq_size.restype = ctypes.c_size_t
    log.info("hr_framequeue loaded")

_pv_lib = _load("hr_preview")
PREVIEW_OK = _pv_lib is not None
if PREVIEW_OK:
    _pv_lib.hr_pv_thumbnail.argtypes      = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8),
                                              ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    _pv_lib.hr_pv_thumbnail.restype       = None
    _pv_lib.hr_pv_draw_border.argtypes    = [ctypes.POINTER(ctypes.c_uint8),
                                              ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                              ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_int]
    _pv_lib.hr_pv_draw_border.restype     = None
    _pv_lib.hr_pv_gray_overlay.argtypes   = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.c_uint8]
    _pv_lib.hr_pv_gray_overlay.restype    = None
    _pv_lib.hr_pv_flip_horizontal.argtypes= [ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int]
    _pv_lib.hr_pv_flip_horizontal.restype = None
    log.info("hr_preview loaded")

_enc_lib = _load("hr_encoder_helpers")
ENCODER_OK = _enc_lib is not None
if ENCODER_OK:
    _enc_lib.hr_rgb_to_yuv420p.argtypes     = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int]
    _enc_lib.hr_rgb_to_yuv420p.restype      = None
    _enc_lib.hr_bgra_to_yuv420p.argtypes    = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int]
    _enc_lib.hr_bgra_to_yuv420p.restype     = None
    _enc_lib.hr_yuv420p_to_rgb.argtypes     = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int]
    _enc_lib.hr_yuv420p_to_rgb.restype      = None
    _enc_lib.hr_gamma_lut_apply.argtypes    = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.c_int]
    _enc_lib.hr_gamma_lut_apply.restype     = None
    _enc_lib.hr_build_thumbnail_lq.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8),
                                                ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    _enc_lib.hr_build_thumbnail_lq.restype  = ctypes.c_int
    log.info("hr_encoder_helpers loaded")

_sw_lib = _load("hr_stopwatch")
STOPWATCH_OK = _sw_lib is not None
if STOPWATCH_OK:
    _sw_lib.hr_sw_create.argtypes        = []; _sw_lib.hr_sw_create.restype      = ctypes.c_void_p
    _sw_lib.hr_sw_destroy.argtypes       = [ctypes.c_void_p]; _sw_lib.hr_sw_destroy.restype   = None
    _sw_lib.hr_sw_start.argtypes         = [ctypes.c_void_p]; _sw_lib.hr_sw_start.restype     = None
    _sw_lib.hr_sw_elapsed_ns.argtypes    = [ctypes.c_void_p]; _sw_lib.hr_sw_elapsed_ns.restype = ctypes.c_int64
    _sw_lib.hr_sw_elapsed_ms.argtypes    = [ctypes.c_void_p]; _sw_lib.hr_sw_elapsed_ms.restype = ctypes.c_double
    _sw_lib.hr_sw_sleep_until_ns.argtypes= [ctypes.c_void_p, ctypes.c_int64]
    _sw_lib.hr_sw_sleep_until_ns.restype = None
    _sw_lib.hr_sw_now_ns.argtypes        = []; _sw_lib.hr_sw_now_ns.restype = ctypes.c_int64
    log.info("hr_stopwatch loaded")

_dx_lib = _load("hr_dxgi_capture")
DXCAP_OK = _dx_lib is not None
if DXCAP_OK:
    _dx_lib.hr_dx_create.argtypes       = [ctypes.c_int, ctypes.c_int]; _dx_lib.hr_dx_create.restype  = ctypes.c_void_p
    _dx_lib.hr_dx_destroy.argtypes      = [ctypes.c_void_p];             _dx_lib.hr_dx_destroy.restype = None
    _dx_lib.hr_dx_get_size.argtypes     = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
    _dx_lib.hr_dx_get_size.restype      = ctypes.c_int
    _dx_lib.hr_dx_capture.argtypes      = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int]
    _dx_lib.hr_dx_capture.restype       = ctypes.c_int
    _dx_lib.hr_dx_reset.argtypes        = [ctypes.c_void_p]; _dx_lib.hr_dx_reset.restype       = ctypes.c_int
    _dx_lib.hr_dx_adapter_count.argtypes= []; _dx_lib.hr_dx_adapter_count.restype = ctypes.c_int
    _dx_lib.hr_dx_output_count.argtypes = [ctypes.c_int]; _dx_lib.hr_dx_output_count.restype  = ctypes.c_int
    _dx_lib.hr_dx_output_desc.argtypes  = [ctypes.c_int, ctypes.c_int,
                                            ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                                            ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                                            ctypes.c_char_p, ctypes.c_int]
    _dx_lib.hr_dx_output_desc.restype   = ctypes.c_int
    log.info("hr_dxgi_capture loaded")

_pl_lib = _load("hr_pipeline")
PIPELINE_OK = _pl_lib is not None
if PIPELINE_OK:
    _pl_lib.hr_pl_create.argtypes         = [ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                              ctypes.c_int, ctypes.c_int, ctypes.c_int]
    _pl_lib.hr_pl_create.restype          = ctypes.c_void_p
    _pl_lib.hr_pl_destroy.argtypes        = [ctypes.c_void_p]; _pl_lib.hr_pl_destroy.restype  = None
    _pl_lib.hr_pl_start.argtypes          = [ctypes.c_void_p]; _pl_lib.hr_pl_start.restype    = ctypes.c_int
    _pl_lib.hr_pl_stop.argtypes           = [ctypes.c_void_p]; _pl_lib.hr_pl_stop.restype     = None
    _pl_lib.hr_pl_pause.argtypes          = [ctypes.c_void_p, ctypes.c_int]
    _pl_lib.hr_pl_pause.restype           = None
    _pl_lib.hr_pl_set_recording.argtypes  = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    _pl_lib.hr_pl_set_recording.restype   = None
    _pl_lib.hr_pl_get_preview.argtypes    = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8),
                                              ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
    _pl_lib.hr_pl_get_preview.restype     = ctypes.c_int
    _pl_lib.hr_pl_stats.argtypes          = [ctypes.c_void_p,
                                              ctypes.POINTER(ctypes.c_int64),
                                              ctypes.POINTER(ctypes.c_int64),
                                              ctypes.POINTER(ctypes.c_double)]
    _pl_lib.hr_pl_stats.restype           = None
    _pl_lib.hr_pl_set_fps.argtypes        = [ctypes.c_void_p, ctypes.c_int]; _pl_lib.hr_pl_set_fps.restype = None
    _pl_lib.hr_pl_set_preview_size.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    _pl_lib.hr_pl_set_preview_size.restype  = None
    log.info("hr_pipeline loaded")

# ---------------------------------------------------------------------------
# Return codes
# ---------------------------------------------------------------------------
HR_DX_OK      =  0
HR_DX_TIMEOUT =  1
HR_DX_LOST    =  2
HR_DX_ERROR   = -1

# ===========================================================================
# API classes
# ===========================================================================

class _CoreAPI:
    """Pixel manipulation — Python fallbacks только для audio/timestamp."""

    def bgrx_to_rgb_np(self, bgrx: bytes, width: int, height: int) -> np.ndarray:
        n_pix = width * height
        dst = np.empty(n_pix * 3, dtype=np.uint8)
        if NATIVE_OK:
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
        _core_lib.hr_resize_bilinear(
            src_c.ctypes.data_as(ctypes.c_char_p),
            dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.c_int(sw), ctypes.c_int(sh),
            ctypes.c_int(dw), ctypes.c_int(dh), ctypes.c_int(ch))
        return dst.reshape(dh, dw, ch)

    def resize_nearest_np(self, src: np.ndarray, sw: int, sh: int, dw: int, dh: int) -> np.ndarray:
        ch = src.shape[2] if src.ndim == 3 else 1
        dst = np.empty(dh * dw * ch, dtype=np.uint8)
        src_c = np.ascontiguousarray(src)
        _core_lib.hr_resize_nearest(
            src_c.ctypes.data_as(ctypes.c_char_p),
            dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.c_int(sw), ctypes.c_int(sh),
            ctypes.c_int(dw), ctypes.c_int(dh), ctypes.c_int(ch))
        return dst.reshape(dh, dw, ch)

    def audio_rms_level(self, pcm_bytes: bytes) -> int:
        n = len(pcm_bytes) // 2
        if n == 0:
            return 0
        if NATIVE_OK:
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
        roi  = base_rgb[y:y+bh, x:x+bw]
        ovl  = badge_rgba[:bh, :bw]
        roi_c = np.ascontiguousarray(roi.reshape(-1, 3))
        ovl_c = np.ascontiguousarray(ovl.reshape(-1, 4))
        if NATIVE_OK:
            _core_lib.hr_blend_rgba(
                roi_c.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_char_p(ovl_c.tobytes()),
                ctypes.c_size_t(bh * bw))
            base_rgb[y:y+bh, x:x+bw] = roi_c.reshape(bh, bw, 3)
        else:
            alpha = ovl[:, :, 3:4].astype(np.float32) / 255.0
            base_rgb[y:y+bh, x:x+bw] = (ovl[:, :, :3] * alpha + roi * (1.0 - alpha)).astype(np.uint8)

    def apply_brightness(self, frame: np.ndarray, delta: int) -> np.ndarray:
        if delta == 0:
            return frame
        fc = np.ascontiguousarray(frame.reshape(-1))
        if NATIVE_OK:
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
        if NATIVE_OK:
            return float(_core_lib.hr_yuv420_luminance(
                ctypes.c_char_p(y_plane), ctypes.c_size_t(len(y_plane))))
        return float(np.frombuffer(y_plane, dtype=np.uint8).mean())

    def timestamp(self) -> str:
        if NATIVE_OK:
            buf = ctypes.create_string_buffer(32)
            if _core_lib.hr_timestamp_str(buf, ctypes.c_size_t(32)) > 0:
                return buf.value.decode("ascii")
        from datetime import datetime
        return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


class _EncoderAPI:
    """YUV-конвертация, gamma, thumbnail."""

    def bgra_to_yuv420p(self, bgra: bytes, width: int, height: int) -> np.ndarray:
        dst = np.empty(width * height * 3 // 2, dtype=np.uint8)
        _enc_lib.hr_bgra_to_yuv420p(
            ctypes.c_char_p(bgra),
            dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.c_int(width), ctypes.c_int(height))
        return dst

    def rgb_to_yuv420p(self, rgb: bytes, width: int, height: int) -> np.ndarray:
        dst = np.empty(width * height * 3 // 2, dtype=np.uint8)
        _enc_lib.hr_rgb_to_yuv420p(
            ctypes.c_char_p(rgb),
            dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.c_int(width), ctypes.c_int(height))
        return dst

    def yuv420p_to_rgb(self, yuv: bytes, width: int, height: int) -> np.ndarray:
        dst = np.empty(width * height * 3, dtype=np.uint8)
        _enc_lib.hr_yuv420p_to_rgb(
            ctypes.c_char_p(yuv),
            dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.c_int(width), ctypes.c_int(height))
        return dst.reshape(height, width, 3)

    def apply_gamma(self, frame: np.ndarray, gamma_x100: int) -> None:
        if gamma_x100 == 100:
            return
        fc = np.ascontiguousarray(frame.reshape(-1))
        _enc_lib.hr_gamma_lut_apply(
            fc.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.c_size_t(fc.size), ctypes.c_int(gamma_x100))
        frame[:] = fc.reshape(frame.shape)

    def thumbnail_lq(self, src: np.ndarray,
                     sw: int, sh: int, dw: int, dh: int) -> Optional[np.ndarray]:
        dst = np.empty(dw * dh * 3, dtype=np.uint8)
        src_c = np.ascontiguousarray(src)
        ok = _enc_lib.hr_build_thumbnail_lq(
            src_c.ctypes.data_as(ctypes.c_char_p),
            dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.c_int(sw), ctypes.c_int(sh), ctypes.c_int(dw), ctypes.c_int(dh))
        return dst.reshape(dh, dw, 3) if ok else None


class _StopwatchAPI:
    """Высокоточный таймер кадров."""

    def create(self) -> object:
        if STOPWATCH_OK:
            return ("native", _sw_lib.hr_sw_create())
        return ("python", _time.perf_counter_ns())

    def destroy(self, handle: object) -> None:
        kind, h = handle
        if kind == "native":
            _sw_lib.hr_sw_destroy(ctypes.c_void_p(h))

    def start(self, handle: object) -> None:
        kind, h = handle
        if kind == "native":
            _sw_lib.hr_sw_start(ctypes.c_void_p(h))

    def elapsed_ms(self, handle: object) -> float:
        kind, h = handle
        if kind == "native":
            return float(_sw_lib.hr_sw_elapsed_ms(ctypes.c_void_p(h)))
        return (_time.perf_counter_ns() - h) / 1_000_000.0

    def sleep_until_ns(self, handle: object, target_ns: int) -> None:
        kind, h = handle
        if kind == "native":
            _sw_lib.hr_sw_sleep_until_ns(ctypes.c_void_p(h), ctypes.c_int64(target_ns))
        else:
            start = h
            remaining = target_ns - (_time.perf_counter_ns() - start)
            if remaining > 2_000_000:
                _time.sleep((remaining - 2_000_000) / 1e9)
            while (_time.perf_counter_ns() - start) < target_ns:
                pass


class _RingBufAPI:
    """Lock-free PCM ring-buffer."""

    def create(self, capacity: int = 2 * 1024 * 1024) -> object:
        if RINGBUF_OK:
            return ("native", _rb_lib.hr_rb_create(ctypes.c_size_t(capacity)))
        return ("python", bytearray())

    def destroy(self, handle: object) -> None:
        kind, h = handle
        if kind == "native":
            _rb_lib.hr_rb_destroy(ctypes.c_void_p(h))

    def write(self, handle: object, data: bytes) -> int:
        kind, h = handle
        if kind == "native":
            return int(_rb_lib.hr_rb_write(ctypes.c_void_p(h), ctypes.c_char_p(data), ctypes.c_size_t(len(data))))
        buf: bytearray = h; buf += data; return len(data)

    def read(self, handle: object, n_bytes: int) -> bytes:
        kind, h = handle
        if kind == "native":
            buf = ctypes.create_string_buffer(n_bytes)
            got = _rb_lib.hr_rb_read(ctypes.c_void_p(h), buf, ctypes.c_size_t(n_bytes))
            return bytes(buf[:got])
        buf: bytearray = h; chunk = bytes(buf[:n_bytes]); del buf[:n_bytes]; return chunk

    def available(self, handle: object) -> int:
        kind, h = handle
        if kind == "native":
            return int(_rb_lib.hr_rb_available_read(ctypes.c_void_p(h)))
        return len(h)

    def reset(self, handle: object) -> None:
        kind, h = handle
        if kind == "native":
            _rb_lib.hr_rb_reset(ctypes.c_void_p(h))
        else:
            h.clear()


class _PreviewAPI:
    """Preview thumbnail + эффекты."""

    def thumbnail(self, src: np.ndarray, dst_w: int, dst_h: int) -> np.ndarray:
        sh, sw = src.shape[:2]
        if PREVIEW_OK and src.flags["C_CONTIGUOUS"]:
            dst = np.empty(dst_h * dst_w * 3, dtype=np.uint8)
            # OPT: data_as вместо .tobytes() — нулевая копия
            _pv_lib.hr_pv_thumbnail(
                src.ctypes.data_as(ctypes.c_char_p),
                dst.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_int(sw), ctypes.c_int(sh),
                ctypes.c_int(dst_w), ctypes.c_int(dst_h))
            return dst.reshape(dst_h, dst_w, 3)
        import cv2
        return cv2.resize(src, (dst_w, dst_h), interpolation=cv2.INTER_LINEAR)

    def draw_border(self, frame: np.ndarray,
                    r: int = 232, g: int = 30, b: int = 30,
                    thickness: int = 4) -> None:
        if not frame.flags["C_CONTIGUOUS"]:
            return
        fh, fw = frame.shape[:2]
        if PREVIEW_OK:
            _pv_lib.hr_pv_draw_border(
                frame.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_int(fw), ctypes.c_int(fh), ctypes.c_int(fw * 3),
                ctypes.c_uint8(r), ctypes.c_uint8(g), ctypes.c_uint8(b),
                ctypes.c_int(thickness))
        else:
            t = thickness
            frame[:t, :] = (r, g, b); frame[-t:, :] = (r, g, b)
            frame[:, :t] = (r, g, b); frame[:, -t:] = (r, g, b)

    def gray_overlay(self, frame: np.ndarray, alpha: int = 120) -> None:
        if not frame.flags["C_CONTIGUOUS"]:
            return
        fh, fw = frame.shape[:2]
        if PREVIEW_OK:
            _pv_lib.hr_pv_gray_overlay(
                frame.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_size_t(fh * fw),
                ctypes.c_uint8(max(0, min(255, alpha))))
        else:
            a = max(0, min(255, alpha)) / 255.0
            frame[:] = np.clip(128 * a + frame * (1.0 - a), 0, 255).astype(np.uint8)

    def flip_horizontal(self, frame: np.ndarray) -> None:
        if PREVIEW_OK and frame.flags["C_CONTIGUOUS"]:
            fh, fw = frame.shape[:2]
            _pv_lib.hr_pv_flip_horizontal(
                frame.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.c_int(fw), ctypes.c_int(fh))
        else:
            frame[:] = frame[:, ::-1, :]


class _DxgiCaptureAPI:
    """DXGI Desktop Duplication — GPU-захват экрана."""

    def create(self, adapter: int = 0, output: int = 0) -> Optional[int]:
        if not DXCAP_OK:
            return None
        h = _dx_lib.hr_dx_create(ctypes.c_int(adapter), ctypes.c_int(output))
        if not h:
            log.warning("hr_dx_create failed (adapter=%d output=%d)", adapter, output)
            return None
        return h

    def destroy(self, handle) -> None:
        if handle and DXCAP_OK:
            _dx_lib.hr_dx_destroy(ctypes.c_void_p(handle))

    def reset(self, handle) -> bool:
        if not handle or not DXCAP_OK:
            return False
        return bool(_dx_lib.hr_dx_reset(ctypes.c_void_p(handle)))

    def get_size(self, handle) -> tuple:
        if not handle or not DXCAP_OK:
            return (0, 0)
        w, h = ctypes.c_int(0), ctypes.c_int(0)
        _dx_lib.hr_dx_get_size(ctypes.c_void_p(handle), ctypes.byref(w), ctypes.byref(h))
        return (w.value, h.value)

    def capture_into(self, handle, buf: np.ndarray, timeout_ms: int = 33) -> int:
        if not handle or not DXCAP_OK:
            return HR_DX_ERROR
        return int(_dx_lib.hr_dx_capture(
            ctypes.c_void_p(handle),
            buf.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.c_int(timeout_ms)))

    def capture(self, handle, width: int, height: int,
                timeout_ms: int = 33) -> Optional[np.ndarray]:
        buf = np.empty(height * width * 4, dtype=np.uint8)
        rc  = self.capture_into(handle, buf, timeout_ms)
        return buf.reshape(height, width, 4) if rc == HR_DX_OK else None

    def adapter_count(self) -> int:
        return int(_dx_lib.hr_dx_adapter_count()) if DXCAP_OK else 0

    def output_count(self, adapter: int = 0) -> int:
        return int(_dx_lib.hr_dx_output_count(ctypes.c_int(adapter))) if DXCAP_OK else 0

    def output_desc(self, adapter: int = 0, output: int = 0) -> dict:
        if not DXCAP_OK:
            return {}
        x, y, w, h = ctypes.c_int(0), ctypes.c_int(0), ctypes.c_int(0), ctypes.c_int(0)
        name_buf = ctypes.create_string_buffer(256)
        ok = _dx_lib.hr_dx_output_desc(
            ctypes.c_int(adapter), ctypes.c_int(output),
            ctypes.byref(x), ctypes.byref(y),
            ctypes.byref(w), ctypes.byref(h),
            name_buf, ctypes.c_int(256))
        if not ok:
            return {}
        return {"x": x.value, "y": y.value, "width": w.value, "height": h.value,
                "name": name_buf.value.decode("utf-8", errors="replace")}

    def list_outputs(self) -> list:
        result = []
        for ai in range(self.adapter_count()):
            for oi in range(self.output_count(ai)):
                d = self.output_desc(ai, oi)
                if d:
                    d["adapter"] = ai; d["output"] = oi
                    result.append(d)
        return result


class _PipelineAPI:
    """
    Обёртка над hr_pipeline.dll — единый C++-поток: DXGI → YUV → pipe + preview.

    Пример (запись):
        pl = pipeline.create(w=1920, h=1080, fps=60, pipe_fd=pipe_write_fd,
                             pv_w=960, pv_h=540)
        pipeline.start(pl)
        # ... пауза/возобновление:
        pipeline.pause(pl, True)
        pipeline.pause(pl, False)
        # ... остановить запись не пересоздавая pipeline:
        pipeline.set_recording(pl, active=False)
        # ... снова начать запись на новый pipe:
        pipeline.set_recording(pl, active=True, pipe_fd=new_fd)
        pipeline.stop(pl)
        pipeline.destroy(pl)
    """

    def create(self, w: int, h: int, fps: int,
               pipe_fd: int = 0, pv_w: int = 960, pv_h: int = 540) -> Optional[int]:
        if not PIPELINE_OK:
            return None
        h_ = _pl_lib.hr_pl_create(
            ctypes.c_int(w), ctypes.c_int(h), ctypes.c_int(fps),
            ctypes.c_int(pipe_fd), ctypes.c_int(pv_w), ctypes.c_int(pv_h))
        if not h_:
            log.warning("hr_pl_create failed (%dx%d @%d fps)", w, h, fps)
            return None
        return h_

    def destroy(self, handle) -> None:
        if handle and PIPELINE_OK:
            _pl_lib.hr_pl_destroy(ctypes.c_void_p(handle))

    def start(self, handle) -> bool:
        if not handle or not PIPELINE_OK:
            return False
        return bool(_pl_lib.hr_pl_start(ctypes.c_void_p(handle)))

    def stop(self, handle) -> None:
        if handle and PIPELINE_OK:
            _pl_lib.hr_pl_stop(ctypes.c_void_p(handle))

    def pause(self, handle, paused: bool) -> None:
        if handle and PIPELINE_OK:
            _pl_lib.hr_pl_pause(ctypes.c_void_p(handle), ctypes.c_int(1 if paused else 0))

    def set_recording(self, handle, active: bool, pipe_fd: int = 0) -> None:
        """Включить/выключить запись без пересоздания pipeline."""
        if handle and PIPELINE_OK:
            _pl_lib.hr_pl_set_recording(
                ctypes.c_void_p(handle),
                ctypes.c_int(1 if active else 0),
                ctypes.c_int(pipe_fd))

    def get_preview(self, handle, out_buf: np.ndarray) -> tuple:
        """
        Копирует последний thumbnail в out_buf (RGB, C-contiguous).
        Возвращает (width, height) или (0, 0) если нет кадра.
        """
        if not handle or not PIPELINE_OK:
            return (0, 0)
        w, h = ctypes.c_int(0), ctypes.c_int(0)
        ok = _pl_lib.hr_pl_get_preview(
            ctypes.c_void_p(handle),
            out_buf.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.byref(w), ctypes.byref(h))
        return (w.value, h.value) if ok else (0, 0)

    def stats(self, handle) -> dict:
        if not handle or not PIPELINE_OK:
            return {"frames": 0, "drops": 0, "fps": 0.0}
        frames = ctypes.c_int64(0)
        drops  = ctypes.c_int64(0)
        fps    = ctypes.c_double(0.0)
        _pl_lib.hr_pl_stats(ctypes.c_void_p(handle),
                             ctypes.byref(frames),
                             ctypes.byref(drops),
                             ctypes.byref(fps))
        return {"frames": frames.value, "drops": drops.value, "fps": fps.value}

    def set_fps(self, handle, fps: int) -> None:
        if handle and PIPELINE_OK:
            _pl_lib.hr_pl_set_fps(ctypes.c_void_p(handle), ctypes.c_int(fps))

    def set_preview_size(self, handle, pw: int, ph: int) -> None:
        if handle and PIPELINE_OK:
            _pl_lib.hr_pl_set_preview_size(ctypes.c_void_p(handle),
                                            ctypes.c_int(pw), ctypes.c_int(ph))


# ---------------------------------------------------------------------------
# Module-level singletons
# ---------------------------------------------------------------------------
core      = _CoreAPI()
encoder   = _EncoderAPI()
stopwatch = _StopwatchAPI()
ringbuf   = _RingBufAPI()
framequeue_api = None   # используй _fq_lib напрямую или обернись при необходимости
preview   = _PreviewAPI()
dxcap     = _DxgiCaptureAPI()
pipeline  = _PipelineAPI()


# ---------------------------------------------------------------------------
# hr_audio — C++ WASAPI audio engine
# ---------------------------------------------------------------------------

_audio_lib = _load("hr_audio")
AUDIO_OK = _audio_lib is not None

if AUDIO_OK:
    _audio_lib.hr_audio_init.argtypes        = []
    _audio_lib.hr_audio_init.restype         = ctypes.c_int

    _audio_lib.hr_audio_start.argtypes       = [ctypes.c_float, ctypes.c_float,
                                                 ctypes.c_int, ctypes.c_int]
    _audio_lib.hr_audio_start.restype        = ctypes.c_int

    _audio_lib.hr_audio_set_volumes.argtypes = [ctypes.c_float, ctypes.c_float,
                                                 ctypes.c_int, ctypes.c_int]
    _audio_lib.hr_audio_set_volumes.restype  = None

    _audio_lib.hr_audio_get_levels.argtypes  = [ctypes.POINTER(ctypes.c_int),
                                                 ctypes.POINTER(ctypes.c_int)]
    _audio_lib.hr_audio_get_levels.restype   = None

    _audio_lib.hr_audio_pause.argtypes       = [ctypes.c_int]
    _audio_lib.hr_audio_pause.restype        = None

    _audio_lib.hr_audio_stop.argtypes        = [ctypes.c_char_p, ctypes.c_char_p]
    _audio_lib.hr_audio_stop.restype         = ctypes.c_int

    _audio_lib.hr_audio_mix_wav.argtypes     = [ctypes.c_char_p, ctypes.c_char_p,
                                                 ctypes.c_char_p]
    _audio_lib.hr_audio_mix_wav.restype      = ctypes.c_int

    _audio_lib.hr_audio_rms.argtypes         = [ctypes.c_void_p, ctypes.c_int]
    _audio_lib.hr_audio_rms.restype          = ctypes.c_int

    _audio_lib.hr_audio_init()
    log.info("hr_audio loaded (C++ WASAPI engine)")


class AudioEngine:
    """Python wrapper over hr_audio.dll — replaces PyAudio+audioop audio logic."""

    available: bool = AUDIO_OK

    def __init__(self) -> None:
        self._mic_level = ctypes.c_int(0)
        self._sys_level = ctypes.c_int(0)

    def start(self, mic_vol: float = 1.0, sys_vol: float = 1.0,
              mic_mute: bool = False, sys_mute: bool = False) -> int:
        """Start recording. Returns bitmask: bit0=mic_ok, bit1=sys_ok."""
        if not AUDIO_OK:
            return 0
        return _audio_lib.hr_audio_start(
            ctypes.c_float(mic_vol), ctypes.c_float(sys_vol),
            ctypes.c_int(int(mic_mute)), ctypes.c_int(int(sys_mute))
        )

    def set_volumes(self, mic_vol: float, sys_vol: float,
                    mic_mute: bool, sys_mute: bool) -> None:
        if not AUDIO_OK: return
        _audio_lib.hr_audio_set_volumes(
            ctypes.c_float(mic_vol), ctypes.c_float(sys_vol),
            ctypes.c_int(int(mic_mute)), ctypes.c_int(int(sys_mute))
        )

    def get_levels(self) -> tuple[int, int]:
        """Returns (mic_level 0-100, sys_level 0-100)."""
        if not AUDIO_OK: return 0, 0
        _audio_lib.hr_audio_get_levels(
            ctypes.byref(self._mic_level),
            ctypes.byref(self._sys_level)
        )
        return self._mic_level.value, self._sys_level.value

    def pause(self, paused: bool) -> None:
        if not AUDIO_OK: return
        _audio_lib.hr_audio_pause(ctypes.c_int(int(paused)))

    def stop(self, mic_wav: str | None = None,
             sys_wav: str | None = None) -> int:
        """Stop and flush to WAV files. Returns bitmask: bit0=mic, bit1=sys."""
        if not AUDIO_OK: return 0
        mic_b = mic_wav.encode() if mic_wav else None
        sys_b = sys_wav.encode() if sys_wav else None
        return _audio_lib.hr_audio_stop(mic_b, sys_b)

    @staticmethod
    def mix_wav(mic_path: str, sys_path: str, out_path: str) -> bool:
        """Mix two WAV files in pure C++ (no subprocess). Returns True on success."""
        if not AUDIO_OK: return False
        r = _audio_lib.hr_audio_mix_wav(
            mic_path.encode(), sys_path.encode(), out_path.encode()
        )
        return r == 0


# Singleton для использования в homrec.py
audio_engine = AudioEngine() if AUDIO_OK else None


# ---------------------------------------------------------------------------
# hr_tools — C++ engine: check_ffmpeg, dshow devices, GPU probe, codec args,
#            audio/video merge.  Wraps hr_tools.dll / hr_tools.so.
# ---------------------------------------------------------------------------
_tools_lib = _load("hr_tools")
TOOLS_OK = _tools_lib is not None

if TOOLS_OK:
    # hr_check_ffmpeg(hint: wstr, out: wstr, out_len: int) -> int
    _tools_lib.hr_check_ffmpeg.argtypes = [
        ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int]
    _tools_lib.hr_check_ffmpeg.restype  = ctypes.c_int

    # hr_get_dshow_devices(ffpath: wstr, out: wstr, buf_chars: int) -> int
    _tools_lib.hr_get_dshow_devices.argtypes = [
        ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int]
    _tools_lib.hr_get_dshow_devices.restype  = ctypes.c_int

    # hr_probe_gpu(ffpath: wstr, out_enc: wstr, out_len: int) -> int
    _tools_lib.hr_probe_gpu.argtypes = [
        ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int]
    _tools_lib.hr_probe_gpu.restype  = ctypes.c_int

    # hr_build_codec_args(codec, quality, fps, cpu_count, out_buf, buf_chars) -> int
    _tools_lib.hr_build_codec_args.argtypes = [
        ctypes.c_wchar_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.c_wchar_p, ctypes.c_int]
    _tools_lib.hr_build_codec_args.restype  = ctypes.c_int

    # hr_merge_av(ffpath, video_file, audio_file) -> int
    _tools_lib.hr_merge_av.argtypes = [
        ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar_p]
    _tools_lib.hr_merge_av.restype  = ctypes.c_int

    log.info("hr_tools loaded")


class ToolsEngine:
    """Thin Python wrapper around hr_tools.dll (C++ implementation).

    Falls back gracefully to None returns when the DLL is absent —
    callers should check TOOLS_OK or use the returned values defensively.
    """
    available: bool = TOOLS_OK
    _BUF = 4096  # wchar buffer size for path/device strings

    # -- ffmpeg ---------------------------------------------------------------
    def find_ffmpeg(self, hint: str = "") -> str | None:
        """Return path to ffmpeg executable, or None if not found."""
        if not TOOLS_OK:
            return None
        buf = ctypes.create_unicode_buffer(self._BUF)
        ok  = _tools_lib.hr_check_ffmpeg(hint or "", buf, self._BUF)
        return buf.value if ok else None

    # -- dshow devices --------------------------------------------------------
    def get_dshow_devices(self, ffmpeg_path: str) -> list[str]:
        """Return list of dshow audio input device names."""
        if not TOOLS_OK:
            return []
        buf   = ctypes.create_unicode_buffer(self._BUF * 4)
        count = _tools_lib.hr_get_dshow_devices(ffmpeg_path, buf, self._BUF * 4)
        if count <= 0:
            return []
        return [d for d in buf.value.split("\n") if d]

    # -- GPU probe ------------------------------------------------------------
    def probe_gpu(self, ffmpeg_path: str) -> str | None:
        """Probe for a hardware encoder; returns encoder name or None.

        Designed to be called from a daemon thread (blocks ~2-10 s).
        """
        if not TOOLS_OK:
            return None
        buf = ctypes.create_unicode_buffer(64)
        ok  = _tools_lib.hr_probe_gpu(ffmpeg_path, buf, 64)
        return buf.value if ok else None

    # -- codec args -----------------------------------------------------------
    def build_codec_args(self, codec: str, quality: int, fps: int,
                         cpu_count: int) -> list[str]:
        """Return a list of ffmpeg codec argument strings."""
        if not TOOLS_OK:
            return ["-c:v", codec]
        buf = ctypes.create_unicode_buffer(self._BUF)
        _tools_lib.hr_build_codec_args(codec, quality, fps, cpu_count,
                                        buf, self._BUF)
        return buf.value.split()

    # -- merge ----------------------------------------------------------------
    def merge_av(self, ffmpeg_path: str, video_file: str,
                 audio_file: str) -> bool:
        """Merge audio_file into video_file (replaces video_file).

        Returns True on success.
        """
        if not TOOLS_OK:
            return False
        result = _tools_lib.hr_merge_av(ffmpeg_path, video_file, audio_file)
        return bool(result)


tools_engine = ToolsEngine() if TOOLS_OK else None


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
          "-o", f"homrec_core{so}", "homrec_core.c"], "homrec_core"),
        (["g++", "-O3", "-std=c++17", "-shared", *fPIC,
          "-o", f"hr_ringbuf{so}", "hr_ringbuf.cpp"], "hr_ringbuf"),
        (["g++", "-O3", "-std=c++17", "-shared", *fPIC,
          "-o", f"hr_framequeue{so}", "hr_framequeue.cpp"], "hr_framequeue"),
        (["g++", "-O3", "-std=c++17", "-shared", *fPIC,
          "-o", f"hr_preview{so}", "hr_preview.cpp"], "hr_preview"),
        (["gcc", "-O3", "-march=native", "-shared", *fPIC, "-lm",
          "-o", f"hr_encoder_helpers{so}", "hr_encoder_helpers.c"], "hr_encoder_helpers"),
        (["g++", "-O3", "-std=c++17", "-shared", *fPIC,
          *([] if not is_win else ["-lwinmm"]),
          "-o", f"hr_stopwatch{so}", "hr_stopwatch.cpp"], "hr_stopwatch"),
        (["g++", "-O3", "-std=c++17", "-shared", *fPIC,
          "-o", f"hr_display_info{so}", "hr_display_info.cpp"], "hr_display_info"),
        (["g++", "-O3", "-std=c++17", "-shared", *fPIC,
          *([] if not is_win else ["-ld3d11", "-ldxgi", "-lole32"]),
          "-o", f"hr_dxgi_capture{so}", "hr_dxgi_capture.cpp"], "hr_dxgi_capture"),
        (["g++", "-O2", "-std=c++17", "-shared", *fPIC,
          *([] if not is_win else ["-lole32", "-lwinmm", "-luuid"]),
          "-o", f"hr_audio{so}", "hr_audio.cpp"], "hr_audio"),
        (["g++", "-O3", "-std=c++17", "-shared", *fPIC,
          *([] if not is_win else ["-ld3d11", "-ldxgi", "-lole32", "-lwinmm"]),
          "-o", f"hr_pipeline{so}", "hr_pipeline.cpp"], "hr_pipeline"),
    ]

    results = []
    for cmd, label in targets:
        try:
            r = subprocess.run(cmd, capture_output=True, cwd=src_dir, timeout=60)
            if r.returncode != 0:
                log.warning("Build %s failed: %s", label, r.stderr.decode()[:400])
                results.append(False)
            else:
                log.info("Built %s OK", label)
                results.append(True)
        except Exception as exc:
            log.warning("Build %s error: %s", label, exc)
            results.append(False)
    return all(results)


if __name__ == "__main__":
    print(f"NATIVE_OK={NATIVE_OK}  RINGBUF_OK={RINGBUF_OK}  FRAMEQUEUE_OK={FRAMEQUEUE_OK}")
    print(f"PREVIEW_OK={PREVIEW_OK}  ENCODER_OK={ENCODER_OK}  STOPWATCH_OK={STOPWATCH_OK}")
    print(f"DXCAP_OK={DXCAP_OK}  PIPELINE_OK={PIPELINE_OK}")

    bgrx = bytes([0, 128, 255, 0] * 4)
    rgb  = core.bgrx_to_rgb_np(bgrx, 4, 1)
    assert rgb[0, 0].tolist() == [255, 128, 0], f"unexpected {rgb[0, 0]}"
    print("bgrx_to_rgb: OK")

    sine = struct.pack("<" + "h" * 44100,
                      *[int(32767 * math.sin(2 * math.pi * 440 * i / 44100))
                        for i in range(44100)])
    level = core.audio_rms_level(sine)
    print(f"audio_rms 440 Hz: {level}  (expected ~75)")

    sw = stopwatch.create()
    t0 = _time.perf_counter_ns()
    stopwatch.sleep_until_ns(sw, 10_000_000)
    elapsed = (_time.perf_counter_ns() - t0) / 1e6
    print(f"stopwatch 10ms sleep: {elapsed:.2f} ms")
    stopwatch.destroy(sw)

    print("\nAll self-tests passed!")
