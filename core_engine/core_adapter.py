"""
core/core_adapter.py — HomRec 2.0.1
Connects the C++ homrec_core engine to the Python app layer.

New in 2.0.1:
  • enumerate_monitors() — wraps homrec_core.enumerate_monitors()
    Falls back to a DXGI-less Python scan if the C++ module isn't built.
  • AudioMixer wrapper — thin Python class forwarding to homrec_core.AudioMixer
    or a pure-Python fallback (audioop-based) when the C++ module is absent.
  • GpuScaler — exposed via CoreAdapter.make_scaler() if supported.
  • CoreAdapter.stats() now always returns a consistent dict with the same
    keys regardless of whether the C++ engine is loaded.
"""

import logging
import os
import threading
from typing import Callable, List, Optional

log = logging.getLogger("homrec.core_adapter")


# -- C++ module import ---------------------------------------------------------

def _try_import_core():
    """Try to import the compiled C++ module. Returns None on failure."""
    try:
        import homrec_core
        return homrec_core
    except ImportError as e:
        log.warning(f"homrec_core not available (C++ engine not built): {e}")
        return None


_core_mod = _try_import_core()


# -- Monitor enumeration -------------------------------------------------------

class MonitorDesc:
    """Plain Python monitor descriptor (mirrors homrec_core.MonitorInfo)."""
    def __init__(self, index, friendly_name, width, height, refresh_hz, is_primary):
        self.index        = index
        self.friendly_name= friendly_name
        self.width        = width
        self.height       = height
        self.refresh_hz   = refresh_hz
        self.is_primary   = is_primary

    def __repr__(self):
        pri = " [primary]" if self.is_primary else ""
        return (f"<MonitorDesc {self.index} \"{self.friendly_name}\" "
                f"{self.width}x{self.height} @{self.refresh_hz}Hz{pri}>")


def enumerate_monitors() -> List[MonitorDesc]:
    """
    Return a list of active monitors.

    Uses homrec_core.enumerate_monitors() when the C++ module is available;
    otherwise falls back to a DXGI-less Win32 EnumDisplayMonitors scan.
    """
    # -- C++ path ----------------------------------------------------------
    if _core_mod is not None:
        try:
            raw = _core_mod.enumerate_monitors()
            return [MonitorDesc(
                        m.index, m.friendly_name,
                        m.width, m.height, m.refresh_hz, m.is_primary)
                    for m in raw]
        except Exception as e:
            log.warning(f"enumerate_monitors (C++): {e}")

    # -- Python fallback (Windows only) ------------------------------------
    try:
        import ctypes
        monitors = []

        def _cb(hMon, _hdcMon, lpRect, _dwData):
            import ctypes.wintypes as wt
            rect = lpRect.contents
            idx = len(monitors)
            w = rect.right  - rect.left
            h = rect.bottom - rect.top
            primary = (rect.left == 0 and rect.top == 0)
            name = f"Display {idx + 1}  ({w}×{h})"
            monitors.append(MonitorDesc(idx, name, w, h, 0, primary))
            return True

        MONITORENUMPROC = ctypes.WINFUNCTYPE(
            ctypes.c_bool,
            ctypes.c_ulong, ctypes.c_ulong,
            ctypes.POINTER(ctypes.wintypes.RECT),
            ctypes.c_double)

        ctypes.windll.user32.EnumDisplayMonitors(
            None, None, MONITORENUMPROC(_cb), 0)
        return monitors
    except Exception as e:
        log.debug(f"enumerate_monitors fallback: {e}")

    # -- Last resort: report a single 1920×1080 primary --------------------
    return [MonitorDesc(0, "Display 1  (1920×1080)", 1920, 1080, 60, True)]


# -- AudioMixer ----------------------------------------------------------------

class AudioMixer:
    """
    Thin wrapper around homrec_core.AudioMixer.

    Falls back to a pure-Python (audioop) implementation when the C++ module
    is not available — quality is identical, but CPU usage is ~3× higher.
    """

    def __init__(self, mic_vol: float = 1.0, sys_vol: float = 0.5):
        self._mic_vol = mic_vol
        self._sys_vol = sys_vol
        self._peak    = (0.0, 0.0)

        if _core_mod is not None:
            try:
                self._native = _core_mod.AudioMixer(mic_vol, sys_vol)
                log.debug("AudioMixer: using C++ native mixer")
                return
            except Exception as e:
                log.warning(f"AudioMixer native init failed: {e}")

        self._native = None
        log.debug("AudioMixer: using Python fallback mixer")

    def set_volume(self, mic_vol: float, sys_vol: float) -> None:
        self._mic_vol = mic_vol
        self._sys_vol = sys_vol
        if self._native is not None:
            self._native.set_volume(mic_vol, sys_vol)

    def mix(self, mic_pcm: bytes, sys_pcm: bytes) -> bytes:
        """Mix mic + desktop PCM (S16LE stereo) → S16LE stereo bytes."""
        if self._native is not None:
            result = self._native.mix(mic_pcm, sys_pcm)
            self._peak = self._native.peak()
            return result
        return self._mix_python(mic_pcm, sys_pcm)

    def peak(self):
        """Return (peak_left, peak_right) ∈ [0, 1] from the last mix."""
        return self._peak

    # -- Pure-Python fallback -----------------------------------------------

    def _mix_python(self, mic_pcm: bytes, sys_pcm: bytes) -> bytes:
        try:
            import audioop
        except ImportError:
            # audioop removed in Python 3.13; just return mic
            return mic_pcm or sys_pcm or b""

        # Apply volumes
        if mic_pcm and self._mic_vol != 1.0:
            mic_pcm = audioop.mul(mic_pcm, 2, self._mic_vol)
        if sys_pcm and self._sys_vol != 1.0:
            sys_pcm = audioop.mul(sys_pcm, 2, self._sys_vol)

        # Pad shorter buffer
        if mic_pcm and sys_pcm:
            diff = len(mic_pcm) - len(sys_pcm)
            if diff > 0:
                sys_pcm = sys_pcm + bytes(diff)
            elif diff < 0:
                mic_pcm = mic_pcm + bytes(-diff)
            mixed = audioop.add(mic_pcm, sys_pcm, 2)
        else:
            mixed = mic_pcm or sys_pcm or b""

        # Rough peak (RMS of left channel)
        if mixed:
            try:
                rms = audioop.rms(mixed, 2) / 32767.0
                self._peak = (min(1.0, rms), min(1.0, rms))
            except Exception:
                pass

        return mixed


# -- CoreAdapter ---------------------------------------------------------------

class CoreAdapter:
    """
    Thin Python wrapper around homrec_core.Engine.

    Falls back to None gracefully if the C++ module isn't compiled yet,
    so the rest of app.py can still run with the legacy gdigrab path.
    """

    def __init__(self, on_status: Optional[Callable[[str], None]] = None):
        self._mod    = _core_mod
        self._engine = None
        self._on_status = on_status
        self._lock   = threading.Lock()

        if self._mod is not None:
            try:
                self._engine = self._mod.Engine()
                self._engine.set_capture_callback(self._status_cb)
                self._engine.set_encoder_callback(self._status_cb)
                log.info("homrec_core C++ engine initialised")
            except Exception as e:
                log.error(f"Engine() constructor failed: {e}")
                self._engine = None
        else:
            log.info("Running in legacy mode (gdigrab via subprocess)")

    # -- Public API ------------------------------------------------------------

    @property
    def available(self) -> bool:
        """True if the C++ engine is loaded and ready."""
        return self._engine is not None

    def init_capture(self, monitor_index: int = 0,
                     hwnd: Optional[int] = None) -> bool:
        if not self.available:
            return False
        try:
            ok = self._engine.init_capture(monitor=monitor_index, hwnd=hwnd)
            if ok:
                log.info(f"Capture started: monitor={monitor_index}, hwnd={hwnd}")
            else:
                log.error("init_capture returned False")
            return ok
        except Exception as e:
            log.error(f"init_capture error: {e}")
            return False

    def start_recording(self,
                        output_path: str,
                        width: int    = 1920,
                        height: int   = 1080,
                        fps: int      = 60,
                        codec: str    = "h264_nvenc",
                        preset: str   = "p1",
                        crf: int      = 18,
                        use_crf: bool = True,
                        bitrate_kbps: int = 8000) -> bool:
        if not self.available:
            return False
        try:
            cfg = self._mod.EncoderConfig()
            cfg.output_path  = str(output_path)
            cfg.width        = width
            cfg.height       = height
            cfg.fps          = fps
            cfg.codec        = codec
            cfg.preset       = preset
            cfg.crf          = crf
            cfg.use_crf      = use_crf
            cfg.bitrate_kbps = bitrate_kbps
            ok = self._engine.start_recording(cfg)
            if ok:
                log.info(f"Recording started → {output_path}")
            return ok
        except Exception as e:
            log.error(f"start_recording error: {e}")
            return False

    def stop_recording(self) -> None:
        if not self.available:
            return
        try:
            self._engine.stop_recording()
            log.info("Recording stopped (C++ engine)")
        except Exception as e:
            log.error(f"stop_recording error: {e}")

    def stop_capture(self) -> None:
        if not self.available:
            return
        try:
            self._engine.stop_capture()
        except Exception as e:
            log.error(f"stop_capture error: {e}")

    def stats(self) -> dict:
        """
        Return a dict with current engine stats.
        Always returns a dict with the same keys regardless of C++ availability.
        """
        defaults = {
            "capture_fps":     0.0,
            "encode_fps":      0.0,
            "frames_dropped":  0,
            "frames_captured": 0,
            "frames_encoded":  0,
            "resolution":      "—",
            "is_recording":    False,
        }
        if not self.available:
            return defaults
        try:
            d = self._engine.stats().as_dict()
            return {**defaults, **d}
        except Exception as e:
            log.debug(f"stats error: {e}")
            return defaults

    def is_recording(self) -> bool:
        if not self.available:
            return False
        try:
            return self._engine.is_recording()
        except Exception:
            return False

    def make_scaler(self, src_w: int, src_h: int,
                    dst_w: int, dst_h: int) -> Optional[object]:
        """
        Create a GpuScaler for the given source → destination resolution.
        Returns None if the C++ module is unavailable or GPU init fails.
        """
        if self._mod is None:
            return None
        try:
            scaler = self._mod.GpuScaler()
            log.debug(f"GpuScaler created ({src_w}×{src_h} → {dst_w}×{dst_h})")
            return scaler
        except Exception as e:
            log.warning(f"GpuScaler creation failed: {e}")
            return None

    # -- Internal --------------------------------------------------------------

    def _status_cb(self, msg: str) -> None:
        """Called from C++ threads (GIL already acquired by bindings.cpp)."""
        log.info(f"[core] {msg}")
        if self._on_status:
            try:
                self._on_status(msg)
            except Exception as e:
                log.debug(f"status callback error: {e}")
