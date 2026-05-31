"""
hr_console_bridge.py  —  thin Python shim for hr_console.dll
=====================================================================
This module replaces the old pure-Python HRConsole class.
The window, input handling, command parsing and dispatch all live
inside hr_console.dll (Win32 native), so the Tkinter event loop is
completely untouched while the console is open.

Usage (from homrec.py)
------
    from hr_console_bridge import NativeConsole

    # In HomRecScreen.__init__ (after self.root and methods are ready):
    self._console = NativeConsole(self)
    self.root.bind("<Control-Shift-T>", lambda e: self._console.toggle())
    self.root.bind("<Control-Shift-t>", lambda e: self._console.toggle())
"""

from __future__ import annotations

import ctypes
import ctypes.util
import logging
import os
import platform
import subprocess
import sys
import webbrowser
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from homrec import HomRecScreen  # noqa: F401

log = logging.getLogger("homrec.console")

# ──────────────────────────────────────────────
# DLL loading
# ──────────────────────────────────────────────

def _find_dll(name: str) -> str | None:
    base = Path(sys.executable).parent if getattr(sys, "frozen", False) \
           else Path(__file__).parent
    for ext in (".dll", ".so", ".dylib"):
        p = base / (name + ext)
        if p.exists():
            return str(p)
    found = ctypes.util.find_library(name)
    return found


def _load_console_lib() -> ctypes.CDLL | None:
    path = _find_dll("hr_console")
    if not path:
        log.warning("hr_console native library not found — console unavailable")
        return None
    try:
        lib = ctypes.CDLL(path)

        # void hr_console_init(cb_start, cb_stop, cb_quit, cb_open_log,
        #                      cb_open_url, log_path, github_url)
        _CB0 = ctypes.CFUNCTYPE(None)                          # no-arg void
        _CBW = ctypes.CFUNCTYPE(None, ctypes.c_wchar_p)        # wchar_t* arg
        lib.hr_console_init.argtypes = [
            _CB0, _CB0, _CB0, _CB0, _CBW,
            ctypes.c_wchar_p, ctypes.c_wchar_p,
        ]
        lib.hr_console_init.restype = None

        lib.hr_console_toggle.argtypes = []
        lib.hr_console_toggle.restype  = None

        lib.hr_console_print.argtypes = [ctypes.c_wchar_p, ctypes.c_int]
        lib.hr_console_print.restype  = None

        lib.hr_console_set_recording_state.argtypes = [ctypes.c_int]
        lib.hr_console_set_recording_state.restype  = None

        lib.hr_console_log_connected.argtypes = []
        lib.hr_console_log_connected.restype  = ctypes.c_int

        log.info("hr_console.dll loaded from %s", path)
        return lib
    except Exception as e:
        log.warning("hr_console.dll load failed: %s", e)
        return None


# ──────────────────────────────────────────────
# Log-disconnect / reconnect helpers
# (called by the DLL's !connect --log / !disconnect --log via Python thread)
# ──────────────────────────────────────────────

_saved_file_handler: logging.FileHandler | None = None


def _disconnect_log() -> None:
    global _saved_file_handler
    root_logger = logging.getLogger()
    for h in list(root_logger.handlers):
        if isinstance(h, logging.FileHandler):
            h.flush()
            h.close()
            root_logger.removeHandler(h)
            _saved_file_handler = h
            log.debug("Console: file log handler detached")
            return


def _reconnect_log() -> None:
    global _saved_file_handler
    root_logger = logging.getLogger()
    if any(isinstance(h, logging.FileHandler) for h in root_logger.handlers):
        return
    if _saved_file_handler:
        handler: logging.FileHandler = _saved_file_handler
        try:
            handler.stream = open(handler.baseFilename, "a", encoding="utf-8")
        except Exception:
            handler = None  # type: ignore[assignment]
    if not handler:
        log_dir = (os.path.dirname(sys.executable)
                   if getattr(sys, "frozen", False)
                   else os.path.dirname(os.path.abspath(__file__)))
        log_path_str = os.path.join(log_dir, "homrec.log")
        handler = logging.FileHandler(log_path_str, encoding="utf-8")
        handler.setFormatter(logging.Formatter(
            "%(asctime)s [%(levelname)s] %(message)s",
            datefmt="%Y-%m-%d %H:%M:%S",
        ))
    root_logger.addHandler(handler)
    _saved_file_handler = None
    log.info("Console: file log handler reconnected")


# ──────────────────────────────────────────────
# Log-filter that checks hr_console_log_connected
# ──────────────────────────────────────────────

class _NativeLogFilter(logging.Filter):
    """Suppress file-log writes when the native console disconnected the log."""
    def __init__(self, lib: ctypes.CDLL) -> None:
        super().__init__()
        self._lib = lib

    def filter(self, record: logging.LogRecord) -> bool:  # type: ignore[override]
        return bool(self._lib.hr_console_log_connected())


# ──────────────────────────────────────────────
# Public wrapper class
# ──────────────────────────────────────────────

class NativeConsole:
    """
    Drop-in replacement for the old HRConsole tkinter class.

    If hr_console.dll is missing, falls back to a no-op so the
    application still starts cleanly.
    """

    GITHUB_URL = "https://github.com/homaaio/HomREC"

    def __init__(self, app: "HomRecScreen") -> None:
        self.app = app
        self._lib = _load_console_lib()
        if not self._lib:
            log.warning("Native console not available — toggle() will be a no-op")
            return

        # ── log path ──────────────────────────────────────────────
        log_dir = (os.path.dirname(sys.executable)
                   if getattr(sys, "frozen", False)
                   else os.path.dirname(os.path.abspath(__file__)))
        log_path = os.path.join(log_dir, "homrec.log")

        # ── ctypes callback types ─────────────────────────────────
        _CB0 = ctypes.CFUNCTYPE(None)
        _CBW = ctypes.CFUNCTYPE(None, ctypes.c_wchar_p)

        # Keep references so GC doesn't collect them
        self._cb_start    = _CB0(self._on_start)
        self._cb_stop     = _CB0(self._on_stop)
        self._cb_quit     = _CB0(self._on_quit)
        self._cb_open_log = _CB0(self._on_open_log)
        self._cb_open_url = _CBW(self._on_open_url)

        self._lib.hr_console_init(
            self._cb_start,
            self._cb_stop,
            self._cb_quit,
            self._cb_open_log,
            self._cb_open_url,
            log_path,
            self.GITHUB_URL,
        )

        # ── attach log filter so !disconnect --log works ──────────
        root_logger = logging.getLogger()
        for h in root_logger.handlers:
            if isinstance(h, logging.FileHandler):
                h.addFilter(_NativeLogFilter(self._lib))

        log.info("NativeConsole initialised (hr_console.dll)")

    # ── public ────────────────────────────────────────────────────

    def toggle(self) -> None:
        if not self._lib:
            return
        # Keep recording state in sync
        self._lib.hr_console_set_recording_state(
            1 if getattr(self.app, "recording", False) else 0
        )
        self._lib.hr_console_toggle()

    def print(self, text: str, tag: int = 0) -> None:
        """Write a line to the console from Python. Tags: 0=normal 1=ok 2=warn 3=err 4=dim 5=accent"""
        if self._lib:
            self._lib.hr_console_print(text, tag)

    # ── callbacks (run on a background thread spawned by the DLL) ─

    def _on_start(self) -> None:
        log.info("Console: start_recording requested")
        if hasattr(self.app, "start_recording"):
            try:
                self.app.root.after(0, self.app.start_recording)
                self._lib.hr_console_set_recording_state(1)  # type: ignore[union-attr]
            except Exception as e:
                log.warning("Console cb_start error: %s", e)

    def _on_stop(self) -> None:
        log.info("Console: stop_recording requested")
        if hasattr(self.app, "stop_recording"):
            try:
                self.app.root.after(0, self.app.stop_recording)
                self._lib.hr_console_set_recording_state(0)  # type: ignore[union-attr]
            except Exception as e:
                log.warning("Console cb_stop error: %s", e)

    def _on_quit(self) -> None:
        log.info("Console: force-quit requested")
        try:
            # Kill ffmpeg immediately
            if getattr(self.app, "ffmpeg_proc", None) and \
               self.app.ffmpeg_proc.poll() is None:  # type: ignore[union-attr]
                self.app.ffmpeg_proc.kill()  # type: ignore[union-attr]
            self.app._preview_running = False
            self.app.stop_flag        = True
            self.app.recording        = False
            self.app.audio_recording  = False
            self.app.sys_audio_recording = False
            if getattr(self.app, "tray_icon", None):
                try:
                    self.app.tray_icon.stop()
                except Exception:
                    pass
            self.app.root.after(150, lambda: (self.app.root.destroy(), sys.exit(0)))
        except Exception as e:
            log.warning("Console cb_quit error: %s", e)

    def _on_open_log(self) -> None:
        log_dir = (os.path.dirname(sys.executable)
                   if getattr(sys, "frozen", False)
                   else os.path.dirname(os.path.abspath(__file__)))
        log_path = os.path.join(log_dir, "homrec.log")
        if not os.path.exists(log_path):
            log.warning("Console: log file not found: %s", log_path)
            return
        try:
            if platform.system() == "Windows":
                os.startfile(log_path)
            elif platform.system() == "Darwin":
                subprocess.Popen(["open", log_path])
            else:
                subprocess.Popen(["xdg-open", log_path])
        except Exception as e:
            log.warning("Console cb_open_log error: %s", e)

    def _on_open_url(self, url: str) -> None:
        try:
            webbrowser.open(url)
        except Exception as e:
            log.warning("Console cb_open_url error: %s", e)
