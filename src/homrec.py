from __future__ import annotations

import ctypes
import os
import sys
import tkinter as tk

from homrec_app import HomRecScreen
from homrec_app.core.optional_deps import HAS_PSUTIL
from homrec_app.core.constants import _ROOT_DIR
import logging

log = logging.getLogger("homrec")

if HAS_PSUTIL:
    import psutil


def main() -> None:
    if sys.platform == "win32":
        _mutex = ctypes.windll.kernel32.CreateMutexW(None, False, "HomRec_SingleInstance_150")
        if ctypes.windll.kernel32.GetLastError() == 183:
            sys.exit(0)
        if not os.environ.get("HOMREC_SHOW_CONSOLE"):
            try:
                kernel32 = ctypes.windll.kernel32
                hwnd = kernel32.GetConsoleWindow()
                if hwnd:
                    hide = True
                    if HAS_PSUTIL:
                        try:
                            parent = psutil.Process(os.getpid()).parent()
                            parent_name = (parent.name() if parent else "").lower()
                            if parent_name in ("cmd.exe", "powershell.exe", "pwsh.exe", "windowsterminal.exe"):
                                hide = False  # inherited from a shell the user is actively using.
                        except Exception as _pe:
                            log.debug(f"console parent lookup failed, hiding anyway: {_pe}")
                    if hide:
                        ctypes.windll.user32.ShowWindow(hwnd, 0)  # SW_HIDE
            except Exception as _ce:
                log.debug(f"console auto-hide skipped: {_ce}")

    root = tk.Tk()
    app = HomRecScreen(root)

    try:
        from hr_plugin_engine import init_plugin_engine
        app._plugins_dir = os.path.join(_ROOT_DIR, 'plugins')
        app.plugin_engine = init_plugin_engine(app)
    except Exception as _pe:
        log.warning(f"Plugin engine failed to load: {_pe}")

    root.mainloop()


if __name__ == "__main__":
    main()
