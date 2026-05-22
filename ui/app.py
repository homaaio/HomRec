import logging
import os
import threading
from typing import Callable, Optional

log = logging.getLogger("homrec.core_adapter")


def _try_import_core():
    """Try to import the compiled C++ module.  Returns None on failure."""
    try:
        import homrec_core
        return homrec_core
    except ImportError as e:
        log.warning(f"homrec_core not available (C++ engine not built): {e}")
        return None


class CoreAdapter:
    """
    Thin Python wrapper around homrec_core.Engine.

    Falls back to None gracefully if the C++ module isn't compiled yet,
    so the rest of app.py can still run with the legacy gdigrab path.
    """

    def __init__(self, on_status: Optional[Callable[[str], None]] = None):
        self._mod    = _try_import_core()
        self._engine = None
        self._on_status = on_status
        self._lock   = threading.Lock()

        if self._mod is not None:
            self._engine = self._mod.Engine()
            self._engine.set_capture_callback(self._status_cb)
            self._engine.set_encoder_callback(self._status_cb)
            log.info("homrec_core C++ engine initialised")
        else:
            log.info("Running in legacy mode (gdigrab via subprocess)")

    # -- Public API ------------------------------------------------------------

    @property
    def available(self) -> bool:
        """True if the C++ engine is loaded and ready."""
        return self._engine is not None

    def init_capture(self, monitor_index: int = 0,
                     hwnd: Optional[int] = None) -> bool:
        """
        Start DXGI capture.
        hwnd: Win32 HWND as int (from win32gui.FindWindow etc.), or None for desktop.
        """
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
        """Begin encoding to file."""
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
        """Flush encoder and close file.  Blocking."""
        if not self.available:
            return
        try:
            self._engine.stop_recording()
            log.info("Recording stopped (C++ engine)")
        except Exception as e:
            log.error(f"stop_recording error: {e}")

    def stop_capture(self) -> None:
        """Full shutdown — stop capture and release DXGI."""
        if not self.available:
            return
        try:
            self._engine.stop_capture()
        except Exception as e:
            log.error(f"stop_capture error: {e}")

    def stats(self) -> dict:
        """Return a dict with current engine stats."""
        if not self.available:
            return {}
        try:
            return self._engine.stats().as_dict()
        except Exception as e:
            log.debug(f"stats error: {e}")
            return {}

    def is_recording(self) -> bool:
        if not self.available:
            return False
        try:
            return self._engine.is_recording()
        except Exception:
            return False

    # -- Internal --------------------------------------------------------------

    def _status_cb(self, msg: str) -> None:
        """Called from C++ threads (GIL already acquired by bindings.cpp)."""
        log.info(f"[core] {msg}")
        if self._on_status:
            try:
                self._on_status(msg)
            except Exception as e:
                log.debug(f"status callback error: {e}")