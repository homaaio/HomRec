from __future__ import annotations

import ctypes
import json
import re
import logging
import os
import platform
import shutil
import subprocess
import sys
import threading
import webbrowser
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from homrec import HomRecScreen

log = logging.getLogger("homrec.console")

CB_VOID    = ctypes.CFUNCTYPE(None)
CB_URL     = ctypes.CFUNCTYPE(None, ctypes.c_wchar_p)
CB_COMMAND = ctypes.CFUNCTYPE(None, ctypes.c_wchar_p)

CONSOLE_VERSION = "1.2.3"
BRIDGE_VERSION  = "1.2.3"

# HomRec application version constants
HOMREC_VERSION = "1.6.5"
CORE_VERSION   = "1.6.0"

# --------------------------------------------------------------------------------
#  Argument parsing utilities
# --------------------------------------------------------------------------------

def _parse_named(raw: str, key: str) -> str | None:
    """Extract the value of #key="value" or #key=value from raw string."""
    import re
    m = re.search(r'#' + re.escape(key) + r'=["\']?([^"\'#\s]+)["\']?', raw)
    if not m:
        # try with quotes
        m = re.search(r'#' + re.escape(key) + r'="([^"]*)"', raw)
    if not m:
        m = re.search(r'#' + re.escape(key) + r"='([^']*)'", raw)
    val = m.group(1) if m else None
    return _resolve_math(val) if val is not None else None


def _parse_flags(raw: str) -> set[str]:
    """Collect all -flags from string (tokens starting with -), except -return/-ret."""
    import re
    flags = set(re.findall(r'(?<!\S)-[a-zA-Z]+', raw))
    flags.discard('-return')
    flags.discard('-ret')
    return flags


def _get_base_dir() -> str:
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    _src = os.path.dirname(os.path.abspath(__file__))
    _parent = os.path.dirname(_src)
    if os.path.isdir(os.path.join(_parent, "src")) or os.path.basename(_src).lower() == "src":
        return _parent
    return _src  # flat layout fallback


# --------------------------------------------------------------------------------
#  Created window manager (registry)
# --------------------------------------------------------------------------------

class WindowRegistry:
    def __init__(self, base_dir: str):
        self._path = Path(base_dir) / "create" / "windows.json"
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._data: dict[str, dict] = {}
        self._load()

    def _load(self):
        try:
            if self._path.exists():
                self._data = json.loads(self._path.read_text("utf-8"))
        except Exception as e:
            log.warning("WindowRegistry load error: %s", e)
            self._data = {}

    def _save(self):
        try:
            self._path.write_text(json.dumps(self._data, ensure_ascii=False, indent=2), "utf-8")
        except Exception as e:
            log.warning("WindowRegistry save error: %s", e)

    def add(self, name: str, kind: str = "window", extra: dict | None = None):
        self._data[name] = {"kind": kind, **(extra or {})}
        self._save()

    def remove(self, name: str) -> bool:
        if name in self._data:
            del self._data[name]
            self._save()
            return True
        return False

    def exists(self, name: str) -> bool:
        return name in self._data

    def get(self, name: str) -> dict | None:
        return self._data.get(name)

    def all_names(self) -> list[str]:
        return list(self._data.keys())


import random as _random


def _resolve_math(s: str) -> str:
    """Replaces {int.random(a, b)} with a random integer."""
    import re
    def _rep(m):
        try:
            a, b = int(m.group(1).strip()), int(m.group(2).strip())
            if a > b: a, b = b, a
            return str(_random.randint(a, b))
        except Exception:
            return m.group(0)
    return re.sub(r'\{int\.random\((\d+),\s*(\d+)\)\}', _rep, s)



# --- RuleRegistry -------------------------------------------------------------

class RuleRegistry:
    def __init__(self, base_dir: str):
        self._path = Path(base_dir) / "create" / "rules.json"
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._data: dict[str, dict] = {}
        self._load()

    def _load(self):
        try:
            if self._path.exists():
                self._data = json.loads(self._path.read_text("utf-8"))
        except Exception as e:
            log.warning("RuleRegistry._load: %s", e)

    def _save(self):
        self._path.write_text(json.dumps(self._data, ensure_ascii=False, indent=2), "utf-8")

    def add(self, name: str, body: str, connected: bool = True):
        self._data[name] = {"body": body, "connected": connected}
        self._save()

    def remove(self, name: str) -> bool:
        if name in self._data:
            del self._data[name]
            self._save()
            return True
        return False

    def exists(self, name: str) -> bool:
        return name in self._data

    def get(self, name: str) -> dict | None:
        return self._data.get(name)

    def set_connected(self, name: str, val: bool) -> bool:
        if name in self._data:
            self._data[name]["connected"] = val
            self._save()
            return True
        return False

    def all_names(self) -> list[str]:
        return list(self._data.keys())


# --- AERegistry ---------------------------------------------------------------

class AERegistry:
    """
    Stores "Anything Else" objects (colors, etc.).
    File: <base>/create/ae.json
    """
    def __init__(self, base_dir: str):
        self._path = Path(base_dir) / "create" / "ae.json"
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._data: dict[str, dict] = {}
        self._load()

    def _load(self):
        try:
            if self._path.exists():
                self._data = json.loads(self._path.read_text("utf-8"))
        except Exception as e:
            log.warning("AERegistry._load: %s", e)

    def _save(self):
        self._path.write_text(json.dumps(self._data, ensure_ascii=False, indent=2), "utf-8")

    def add(self, name: str, ae_type: str, data: dict):
        self._data[name] = {"type": ae_type, **data}
        self._save()

    def remove(self, name: str) -> bool:
        if name in self._data:
            del self._data[name]
            self._save()
            return True
        return False

    def exists(self, name: str) -> bool:
        return name in self._data

    def get(self, name: str) -> dict | None:
        return self._data.get(name)

    def all_names(self) -> list[str]:
        return list(self._data.keys())


# --- TerminalFuncRegistry -----------------------------------------------------

class TerminalFuncRegistry:
    """
    Stores functions bound to @terminal via !connect --func @terminal <alias> <cmd>.
    These are short aliases that hr_terminal.exe resolves locally when typed.
    Persisted to <base>/create/terminal_funcs.json
    """
    def __init__(self, base_dir: str):
        self._path = Path(base_dir) / "create" / "terminal_funcs.json"
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._data: dict[str, str] = {}  # alias -> full command
        self._load()

    def _load(self):
        try:
            if self._path.exists():
                self._data = json.loads(self._path.read_text("utf-8"))
        except Exception as e:
            log.warning("TerminalFuncRegistry._load: %s", e)

    def _save(self):
        self._path.write_text(json.dumps(self._data, ensure_ascii=False, indent=2), "utf-8")

    def bind(self, alias: str, cmd: str):
        self._data[alias] = cmd
        self._save()

    def unbind(self, alias: str) -> bool:
        if alias in self._data:
            del self._data[alias]
            self._save()
            return True
        return False

    def resolve(self, alias: str) -> str | None:
        return self._data.get(alias)

    def all(self) -> dict[str, str]:
        return dict(self._data)


# --- AliasRegistry ------------------------------------------------------------

class AliasRegistry:
    """
    Stores command aliases created via !alias.
    File: <base>/create/aliases.json
    """
    def __init__(self, base_dir: str):
        self._path = Path(base_dir) / "create" / "aliases.json"
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._data: dict[str, str] = {}
        self._load()

    def _load(self):
        try:
            if self._path.exists():
                self._data = json.loads(self._path.read_text("utf-8"))
        except Exception as e:
            log.warning("AliasRegistry._load: %s", e)

    def _save(self):
        self._path.write_text(json.dumps(self._data, ensure_ascii=False, indent=2), "utf-8")

    def add(self, name: str, cmd: str):
        self._data[name] = cmd
        self._save()

    def remove(self, name: str) -> bool:
        if name in self._data:
            del self._data[name]
            self._save()
            return True
        return False

    def get(self, name: str) -> str | None:
        return self._data.get(name)

    def all(self) -> dict[str, str]:
        return dict(self._data)


# --- EnvStore -----------------------------------------------------------------

class EnvStore:
    """Stores internal console environment variables ($name)."""
    def __init__(self):
        self._vars: dict[str, str] = {}

    def set(self, name: str, val: str):
        self._vars[name] = val

    def get(self, name: str) -> str | None:
        return self._vars.get(name)

    def unset(self, name: str) -> bool:
        if name in self._vars:
            del self._vars[name]
            return True
        return False

    def all(self) -> dict[str, str]:
        return dict(self._vars)

    def resolve(self, s: str) -> str:
        """Replaces $name with the variable value in the string."""
        import re
        def _rep(m):
            return self._vars.get(m.group(1), m.group(0))
        return re.sub(r'\$([A-Za-z_]\w*)', _rep, s)


# --------------------------------------------------------------------------------

class HotkeyManager:
    """
    Registers global hotkeys via keyboard (if available)
    or via Tkinter bind as fallback.
    Stores bindings in <base>/create/hotkeys.json
    """
    def __init__(self, base_dir: str, console: "NativeConsole"):
        self._path = Path(base_dir) / "create" / "hotkeys.json"
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._console = console
        self._bindings: dict[str, str] = {}  # key → command
        self._load()

        # Try to use keyboard library
        try:
            import keyboard as _kb
            self._kb = _kb
        except ImportError:
            self._kb = None
            log.warning("'keyboard' package not found; hotkeys fallback to Tkinter bind")

        # Restore saved bindings
        for key, val in list(self._bindings.items()):
            cmd = val.get("cmd") if isinstance(val, dict) else val
            self._register_os(key, cmd)

    def _load(self):
        try:
            if self._path.exists():
                self._bindings = json.loads(self._path.read_text("utf-8"))
        except Exception as e:
            log.warning("HotkeyManager load error: %s", e)

    def _save(self):
        try:
            self._path.write_text(json.dumps(self._bindings, ensure_ascii=False, indent=2), "utf-8")
        except Exception as e:
            log.warning("HotkeyManager save error: %s", e)

    def _register_os(self, key: str, cmd: str):
        if self._kb:
            try:
                self._kb.add_hotkey(key, lambda c=cmd: self._console.run_command(c))
                log.info("Hotkey registered: %s → %s", key, cmd)
            except Exception as e:
                log.warning("Hotkey registration failed for '%s': %s", key, e)
        else:
            # Tkinter bind (only works when focus is on main window)
            app = self._console.app
            try:
                tk_key = "<" + key.replace("+", "-") + ">"
                app.root.bind(tk_key, lambda e, c=cmd: self._console.run_command(c))
                log.info("Tkinter hotkey bound: %s → %s", tk_key, cmd)
            except Exception as e:
                log.warning("Tkinter hotkey bind failed for '%s': %s", key, e)

    def bind(self, key: str, cmd: str, alias: str | None = None):
        self._bindings[key] = {"cmd": cmd, "alias": alias}
        self._save()
        self._register_os(key, cmd)
        log.info("HotkeyManager: bound '%s' → '%s'%s", key, cmd,
                 f"  alias={alias}" if alias else "")

    def unbind(self, key: str) -> bool:
        if key not in self._bindings:
            return False
        del self._bindings[key]
        self._save()
        if self._kb:
            try: self._kb.remove_hotkey(key)
            except Exception: pass
        log.info("HotkeyManager: unbound '%s'", key)
        return True

    def unbind_by_alias(self, alias: str) -> str | None:
        for key, val in list(self._bindings.items()):
            stored = val.get("alias") if isinstance(val, dict) else None
            if stored == alias:
                self.unbind(key)
                return key
        return None

    def all_bindings(self) -> dict:
        return dict(self._bindings)


# --------------------------------------------------------------------------------
#  NativeConsole
# --------------------------------------------------------------------------------

class NativeConsole:
    GITHUB = "https://github.com/homaaio/HomREC"

    def __init__(self, app: "HomRecScreen") -> None:
        self.app = app
        self._lib = None  # BUG FIX: initialize before _load()

        base = _get_base_dir()
        self._win_reg  = WindowRegistry(base)
        self._rule_reg = RuleRegistry(base)
        self._ae_reg   = AERegistry(base)
        self._alias_reg = AliasRegistry(base)
        self._env      = EnvStore()
        self._hotkeys  = HotkeyManager(base, self)

        # Console command history (for !history)
        self._history: list[str] = []
        # Timers and watchers
        self._timers: dict[str, threading.Timer] = {}
        self._watchers: dict[str, dict] = {}   # name → {thread, stop_event, ms, cmd, runs, max_runs}

        self._lib = self._load()
        if not self._lib:
            return

        log_path = os.path.join(base, "homrec.log")  # base is already root dir

        # Keep references so GC does not delete them (BUG FIX: store as instance attributes)
        self._cb_start    = CB_VOID(self._start)
        self._cb_stop     = CB_VOID(self._stop)
        self._cb_quit     = CB_VOID(self._quit)
        self._cb_open_log = CB_VOID(self._open_log)
        self._cb_open_url = CB_URL(self._open_url)
        self._cb_command  = CB_COMMAND(self._on_command)  # new callback for extended commands

        self._lib.hr_con_init(
            self._cb_start, self._cb_stop, self._cb_quit,
            self._cb_open_log, self._cb_open_url,
            log_path, self.GITHUB,
        )

        # Register extended command callback (if DLL supports it)
        if hasattr(self._lib, 'hr_con_set_command_cb'):
            self._lib.hr_con_set_command_cb.argtypes = [CB_COMMAND]
            self._lib.hr_con_set_command_cb.restype = None
            self._lib.hr_con_set_command_cb(self._cb_command)

        # Start pipe server for hr_terminal.exe
        # Bug fix: terminal.exe used to hang because the pipe had no keepalive.
        # We now start a daemon thread that sends a NUL heartbeat every 2 seconds
        # so terminal's ReadFile never blocks indefinitely.
        if hasattr(self._lib, 'hr_pipe_server_start'):
            self._lib.hr_pipe_server_start.argtypes = []
            self._lib.hr_pipe_server_start.restype  = None
            self._lib.hr_pipe_server_start()
            log.info("Pipe server started for external terminals")
            self._start_pipe_keepalive()

        # Filter for !disconnect --log
        # BUG FIX: check self._lib != None before calling
        class LogFilter(logging.Filter):
            def __init__(self, lib):
                super().__init__()
                self._lib = lib

            def filter(self, r):
                # BUG FIX: guard against case when _lib is still None
                try:
                    return bool(self._lib and self._lib.hr_con_log_connected())
                except Exception:
                    return True  # on error — do not block log

        for h in logging.getLogger().handlers:
            if isinstance(h, logging.FileHandler):
                h.addFilter(LogFilter(self._lib))

        log.info("hr_console.dll OK")

    def _load(self):
        # Search order:
        #   1. next to __file__  (dev: src/)
        #   2. next to sys.executable  (frozen: root)
        #   3. src/ subfolder next to sys.executable  (frozen + dlls in src/)
        def _candidates():
            if not getattr(sys, "frozen", False):
                yield os.path.dirname(os.path.abspath(__file__))
            exe_dir = os.path.dirname(sys.executable)
            yield exe_dir
            yield os.path.join(exe_dir, "src")

        dll_dir = dll_path = None
        for candidate in _candidates():
            p = os.path.join(candidate, "hr_console.dll")
            if os.path.exists(p):
                dll_dir  = candidate
                dll_path = p
                break

        if not dll_path:
            log.warning("hr_console.dll not found (searched: %s)",
                        ", ".join(_candidates()))
            return None

        try:
            if hasattr(os, "add_dll_directory"):
                os.add_dll_directory(dll_dir)

            lib = ctypes.CDLL(dll_path)

            lib.hr_con_init.argtypes = [CB_VOID, CB_VOID, CB_VOID, CB_VOID, CB_URL,
                                        ctypes.c_wchar_p, ctypes.c_wchar_p]
            lib.hr_con_init.restype  = None
            lib.hr_con_toggle.argtypes = []
            lib.hr_con_toggle.restype  = None
            lib.hr_con_set_recording.argtypes = [ctypes.c_int]
            lib.hr_con_set_recording.restype  = None
            lib.hr_con_log_connected.argtypes = []
            lib.hr_con_log_connected.restype  = ctypes.c_int

            lib.hr_con_write.argtypes = [ctypes.c_wchar_p, ctypes.c_int]
            lib.hr_con_write.restype  = None

            return lib
        except Exception as e:
            log.warning("hr_console.dll load failed: %s", e)
            return None  # BUG FIX: explicit None

    # -- Public API -------------------------------------------------------------------

    def toggle(self):
        # BUG FIX: check self._lib before any access
        if not self._lib:
            return
        is_rec = 1 if getattr(self.app, "recording", False) else 0
        self._lib.hr_con_set_recording(is_rec)
        self._lib.hr_con_toggle()

    def run_command(self, cmd: str):
        """Execute an extended command (called from hotkeys or DLL)."""
        cmd = cmd.strip()
        if not cmd:
            return
        try:
            self._dispatch_extended(cmd)
        except Exception as e:
            log.error("run_command error '%s': %s", cmd, e)

    # -- Callbacks DLL ------------------------------------------------------------

    def _start(self):
        log.info("Console: start_recording")
        # BUG FIX: check that root exists
        if self._root_alive():
            self.app.root.after(0, self.app.start_recording)
        if self._lib:
            self._lib.hr_con_set_recording(1)

    def _stop(self):
        log.info("Console: stop_recording")
        if self._root_alive():
            self.app.root.after(0, self.app.stop_recording)
        if self._lib:
            self._lib.hr_con_set_recording(0)

    def _quit(self):
        log.info("Console: force quit")
        try:
            if getattr(self.app, "ffmpeg_proc", None) and self.app.ffmpeg_proc.poll() is None:
                self.app.ffmpeg_proc.kill()
            self.app._preview_running = False
            self.app.stop_flag        = True
            self.app.recording        = False
            self.app.audio_recording  = False
            self.app.sys_audio_recording = False
            try:
                self.app.tray_icon.stop()
            except Exception:
                pass
        except Exception as e:
            log.warning("Console quit error: %s", e)
        # BUG FIX: check that root is alive before after()
        if self._root_alive():
            self.app.root.after(150, lambda: (self._safe_destroy(), sys.exit(0)))
        else:
            sys.exit(0)

    def _on_command(self, cmd: str):
        """Callback for extended commands from DLL (new)."""
        log.info("Console command: %s", cmd)
        if self._root_alive():
            self.app.root.after(0, lambda: self.run_command(cmd))

    def _push_terminal_funcs(self) -> None:
        """Send the full terminal func table to hr_terminal.exe via pipe as JSON packet."""
        try:
            if self._lib and hasattr(self._lib, 'hr_pipe_send'):
                import json as _json
                payload = _json.dumps({
                    "__type__": "terminal_funcs",
                    "funcs": self._term_funcs.all()
                })
                data = payload.encode("utf-8") + b"\n"
                self._lib.hr_pipe_send.argtypes = [ctypes.c_char_p]
                self._lib.hr_pipe_send.restype = None
                self._lib.hr_pipe_send(data)
        except Exception as e:
            log.debug("_push_terminal_funcs: %s", e)

    def _start_pipe_keepalive(self) -> None:
        """Start a daemon thread that sends periodic heartbeat pings through the DLL pipe.
        Prevents terminal.exe ReadFile from blocking forever (Bug fix v1.7.0).
        """
        import threading, time
        def _beat():
            while True:
                try:
                    if self._lib and hasattr(self._lib, 'hr_pipe_send'):
                        self._lib.hr_pipe_send.argtypes = [ctypes.c_char_p]
                        self._lib.hr_pipe_send.restype  = None
                        self._lib.hr_pipe_send(b"\x00")  # NUL keepalive byte
                except Exception:
                    pass
                time.sleep(2.0)
        t = threading.Thread(target=_beat, daemon=True, name="pipe-keepalive")
        t.start()

    def _open_log(self):
        base = _get_base_dir()
        path = os.path.join(base, "homrec.log")
        try:
            if platform.system() == "Windows":
                os.startfile(path)
            elif platform.system() == "Darwin":
                subprocess.Popen(["open", path])
            else:
                subprocess.Popen(["xdg-open", path])
        except Exception as e:
            log.warning("Console open_log error: %s", e)

    def _open_url(self, url: str):
        try:
            webbrowser.open(url)
        except Exception as e:
            log.warning("Console open_url error: %s", e)

    # -- Extended command dispatcher --------------------------------------------------

    def _dispatch_extended(self, raw: str, _ret_requested: bool = False):
        """
        Extended command router.
        Called from NativeConsole.on_command() when command arrives from DLL.
        -ret / -return flag: if present, logs (1) on success or (0) on failure after execution.
        """
        import re
        # Detect -ret flag before stripping
        ret_flag = bool(re.search(r'\s+-ret(?:urn)?\b', raw)) or _ret_requested
        raw = re.sub(r'\s+-ret(?:urn)?\b', '', raw).strip()
        raw = raw.strip()
        if ret_flag:
            # Wrap dispatch in try/except to determine success
            try:
                self._dispatch_extended_inner(raw)
                log.info("-ret: (1)  [command executed successfully]")
            except Exception as e:
                log.warning("-ret: (0)  [command failed: %s]", e)
            return
        self._dispatch_extended_inner(raw)

    def _dispatch_extended_inner(self, raw: str):
        """Inner dispatch — called by _dispatch_extended after -ret is handled."""

        # Normalize: @all → --all
        raw = raw.replace("@all", "--all")

        # Substitute environment variables ($name)
        raw = self._env.resolve(raw)

        # Substitute math expressions
        raw = _resolve_math(raw)

        cmd = raw.split()[0] if raw.split() else ""

        # Check alias
        alias_cmd = self._alias_reg.get(cmd)
        if alias_cmd:
            self._record_history(raw)
            self._dispatch_extended(alias_cmd)
            return

        self._record_history(raw)

        if cmd == "!rename":
            self._cmd_rename(raw); return
        if cmd == "$rm":
            self._cmd_rm(raw); return
        if cmd == "!edit":
            self._cmd_edit(raw); return
        if cmd == "!create":
            self._cmd_create(raw); return
        if cmd == "!start":
            if "--rec" in raw:
                self._cmd_start_rec(raw)
            elif "--terminal" in raw:
                self._cmd_start_terminal(raw)
            else:
                self._cmd_start_window(raw)
            return
        if cmd == "!rule":
            self._cmd_rule(raw); return
        if cmd == "!connect":
            self._cmd_connect(raw); return
        if cmd == "!disconnect":
            self._cmd_disconnect(raw); return

        # -- New commands -------------------------------------------------------
        if cmd == "!ls":
            self._cmd_ls(raw); return
        if cmd == "!status":
            self._cmd_status(raw); return
        if cmd == "!info":
            self._cmd_info(raw); return
        if cmd == "!history":
            self._cmd_history(raw); return
        if cmd == "!alias":
            self._cmd_alias(raw); return
        if cmd == "!repeat":
            self._cmd_repeat(raw); return
        if cmd == "!delay":
            self._cmd_delay(raw); return
        if cmd == "!batch":
            self._cmd_batch(raw); return
        if cmd == "!run":
            self._cmd_run(raw); return
        if cmd == "!clear":
            self._cmd_clear(raw); return
        if cmd == "$clear":
            self._cmd_dollar_clear(raw); return
        if cmd == "$clear":
            self._cmd_dollar_clear(raw); return
        if cmd == "!echo":
            self._cmd_echo(raw); return
        if cmd == "!clip":
            self._cmd_clip(raw); return
        if cmd == "!env":
            self._cmd_env(raw); return
        if cmd == "!timer":
            self._cmd_timer(raw); return
        if cmd == "!watch":
            self._cmd_watch(raw); return
        if cmd == "!ping":
            self._cmd_ping(raw); return
        if cmd == "!check.er":
            self._cmd_check_er(raw); return
        if cmd == "!func.list":
            self._cmd_func_list(raw); return
        if cmd == "!version":
            self._cmd_version(raw); return
        if cmd == "!homrec":
            self._cmd_homrec(raw); return
        if cmd == "!log":
            self._cmd_log(raw); return

        log.warning("_dispatch_extended: unknown command '%s'", cmd)

    def _record_history(self, raw: str):
        self._history.append(raw)
        if len(self._history) > 500:
            self._history = self._history[-500:]



    # --- !start --rec ---------------------------------------------------------

    def _cmd_start_rec(self, raw: str):
        """!start --rec 1|0"""
        tokens = raw.split()
        val = None
        for i, t in enumerate(tokens):
            if t == "--rec" and i + 1 < len(tokens):
                try:
                    val = int(tokens[i + 1])
                except ValueError:
                    pass
        if val is None:
            log.warning("!start --rec: specify 1 (start) or 0 (stop)")
            return
        silent = "-s" in tokens or "--silent" in tokens
        if val == 1:
            if not getattr(self.app, "recording", False):
                if self._root_alive():
                    self.app.root.after(0, self.app.start_recording)
                if self._lib:
                    self._lib.hr_con_set_recording(1)
                if not silent:
                    log.info("!start --rec 1: recording started")
            else:
                if not silent:
                    log.info("!start --rec 1: already recording")
        else:
            if getattr(self.app, "recording", False):
                if self._root_alive():
                    self.app.root.after(0, self.app.stop_recording)
                if self._lib:
                    self._lib.hr_con_set_recording(0)
                if not silent:
                    log.info("!start --rec 0: recording stopped")
            else:
                if not silent:
                    log.info("!start --rec 0: recording was not active")

    # --- !rule ----------------------------------------------------------------

    def _cmd_rule(self, raw: str):
        """
        !rule --check #name="example"
            → shows: active/inactive + body
        !rule --get from connect #name="example"
            → reads rule from registry (source — !connect --rule)
        """
        tokens = raw.split()
        silent = "-s" in tokens
        name = _parse_named(raw, "name")
        if not name:
            log.warning("!rule: #name not specified")
            return

        if "--check" in raw:
            entry = self._rule_reg.get(name)
            if entry is None:
                log.warning("!rule --check '%s': not found", name)
                return
            status = "✔ active" if entry.get("connected", True) else "✘ disabled"
            body   = entry.get("body", "(empty)")
            log.info("!rule --check '%s':  %s", name, status)
            log.info("  body: %s", body)
            return

        if "--get" in raw and "from" in raw and "connect" in raw:
            entry = self._rule_reg.get(name)
            if entry is None:
                log.warning("!rule --get '%s': not found in registry", name)
                return
            log.info("!rule --get '%s': connected=%s  body=%s",
                     name, entry.get("connected"), entry.get("body", ""))
            return

        log.warning("!rule: use --check or --get from connect")

    # --- !edit ----------------------------------------------------------------

    def _cmd_edit(self, raw: str):
        """
        !edit --file    #name="x"           → open file in notepad for editing
        !edit --window  #name="x"           → reopen window
        !edit --rule    #name="x"; <step>; <step>  → replace rule body
        !edit --settings #name=shortcut 1|0 → desktop shortcut
        """
        import re
        tokens = raw.split()
        silent = "-s" in tokens

        if "--file" in raw:
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!edit --file: #name not specified"); return
            entry = self._win_reg.get(name)
            if not entry:
                log.warning("!edit --file '%s': not found in registry", name); return
            if entry.get("kind") != "notepad":
                log.warning("!edit --file '%s': not a notepad entry (kind=%s)", name, entry.get("kind")); return
            fp = entry.get("file", "")
            if not fp:
                log.warning("!edit --file '%s': file path not saved", name); return
            self._open_notepad_file(fp)
            if not silent:
                log.info("!edit --file '%s': opened %s", name, fp)
            return

        if "--window" in raw:
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!edit --window: #name not specified"); return
            self._cmd_start_window(f'!start --window #name="{name}"')
            if not silent:
                log.info("!edit --window '%s': opened", name)
            return

        if "--rule" in raw:
            # !edit --rule #name="x"; step1; step2; step3
            m = re.search(r'#name=["\']?([^"\';\s]+)["\']?\s*;\s*(.+)', raw, re.DOTALL)
            if not m:
                # Show current body if no new steps provided
                name = _parse_named(raw, "name")
                if name:
                    entry = self._rule_reg.get(name)
                    if entry:
                        log.info("!edit --rule '%s' (current body):\n  %s",
                                 name, entry.get("body", "(empty)"))
                    else:
                        log.warning("!edit --rule '%s': not found", name)
                else:
                    log.warning("!edit --rule: syntax: !edit --rule #name=\"x\"; step1; step2")
                return
            name     = _resolve_math(m.group(1).strip())
            new_body = m.group(2).strip()
            if not self._rule_reg.exists(name):
                log.warning("!edit --rule '%s': not found. Use !create --rule first", name); return
            entry = self._rule_reg.get(name)
            self._rule_reg.add(name, new_body, entry.get("connected", True))
            if not silent:
                log.info("!edit --rule '%s': body updated → %s", name, new_body)
            return

        if "--settings" in raw:
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!edit --settings: #name not specified"); return
            toks = raw.split()
            vals = [t for t in toks if t in ("0","1","true","false","on","off","yes","no")]
            enable = vals[-1] in ("1","true","on","yes") if vals else True
            if name == "shortcut":
                self._toggle_desktop_shortcut(enable)
                if not silent:
                    log.info("!edit --settings shortcut → %s", "on" if enable else "off")
            else:
                log.warning("!edit --settings: unknown parameter '%s'", name)
            return

        if "--terminal" in raw:
            self._cmd_edit_terminal(raw)
            return

        log.warning("!edit: use --file / --window / --rule / --settings / --terminal")

    # --- !create --------------------------------------------------------------

    def _cmd_create(self, raw: str):
        """
        !create --window #name="x" [#bg=COLOR] [#fg=COLOR] [#size=(WxH)]
                         [--notepad [as .EXT]]
                         [-o] [-s] [-n] [-c] [-d]

        !create --rule #name="x"; step1; step2; step3  [-c] [-d]
            Steps — console commands separated by ';'.
            With -c: executes immediately (connect).
            With -d: saved as disconnected.
            Example: !create --rule #name="auto"; !start --rec 1 then; $rm #name="notepad2"

        !create --ae #type=color{rgb=(255,0,0)} #name="red"
        !create --ae #type=color{hex=(#FF0000)}  #name="red"
        """
        import re
        flags       = _parse_flags(raw)
        is_notepad  = "--notepad" in raw
        is_rule     = "--rule"    in raw
        is_ae       = "--ae"      in raw
        is_window   = "--window"  in raw and not is_rule and not is_ae
        only_create = "-o" in flags
        silent      = "-s" in flags
        no_register = "-n" in flags
        auto_connect= "-c" in flags
        disconnected= "-d" in flags

        name = _parse_named(raw, "name")
        if not name:
            log.warning("!create: #name not specified"); return

        base = _get_base_dir()

        # -- --rule ------------------------------------------------------------
        if is_rule:
            # body = everything after first ';' following #name=...
            m = re.search(r'#name=["\']?[^"\';\s]+["\']?\s*;\s*(.+)', raw, re.DOTALL)
            body = m.group(1).strip() if m else ""
            connected = not disconnected

            # New flags for rules
            once    = "--once"  in raw
            delay_s = _parse_named(raw, "ms") if "--delay" in raw else None
            on_fail = _parse_named(raw, "cmd") if "--on-fail" in raw else None
            loop_s  = _parse_named(raw, "count") if "--loop" in raw else None
            extra_rule: dict = {"once": once}
            if delay_s: extra_rule["step_delay_ms"] = int(delay_s)
            if on_fail: extra_rule["on_fail"] = on_fail
            if loop_s:  extra_rule["loop"] = int(loop_s)

            self._rule_reg.add(name, body, connected)
            # Save additional metadata
            if extra_rule:
                entry = self._rule_reg.get(name) or {}
                entry.update(extra_rule)
                self._rule_reg.add(name, body, connected)

            if not silent:
                log.info("!create --rule '%s': saved  connected=%s  body: %s%s",
                         name, connected, body or "(empty)",
                         f"  once={once}" if once else "")
            # -c: execute steps right now
            if auto_connect and body:
                loop_n = int(loop_s) if loop_s else 1
                for _ in range(loop_n if loop_n > 0 else 1):
                    self._run_rule_body(name, body, silent)
            return

        # -- --ae --------------------------------------------------------------
        if is_ae:
            type_raw = _parse_named(raw, "type")
            if not type_raw:
                log.warning("!create --ae: #type not specified"); return
            ae_type = type_raw.split("{")[0].lower()
            ae_data: dict = {"connected": not disconnected}

            if ae_type == "color":
                rgb_m = re.search(r'rgb=\((\d+),\s*(\d+),\s*(\d+)\)', raw)
                hex_m = re.search(r'hex=\(#?([0-9A-Fa-f]{6})\)',       raw)
                if rgb_m:
                    r2,g2,b2 = int(rgb_m.group(1)),int(rgb_m.group(2)),int(rgb_m.group(3))
                    ae_data["rgb"] = [r2, g2, b2]
                    ae_data["hex"] = "#{:02X}{:02X}{:02X}".format(r2, g2, b2)
                elif hex_m:
                    hx = hex_m.group(1).upper()
                    ae_data["hex"] = "#" + hx
                    ae_data["rgb"] = [int(hx[0:2],16), int(hx[2:4],16), int(hx[4:6],16)]
                else:
                    log.warning("!create --ae color: rgb=(...) or hex=(...) required"); return
            else:
                log.warning("!create --ae: unknown #type='%s'", ae_type); return

            self._ae_reg.add(name, ae_type, ae_data)
            if not silent:
                log.info("!create --ae [%s] '%s': hex=%s  rgb=%s",
                         ae_type, name, ae_data.get("hex"), ae_data.get("rgb"))
            return

        # -- --window ----------------------------------------------------------
        # Parse window style params
        bg   = _parse_named(raw, "bg")   or "white"
        fg   = _parse_named(raw, "fg")   or "black"
        size_m = re.search(r'#size=\((\d+)x(\d+)\)', raw)
        w, h = (int(size_m.group(1)), int(size_m.group(2))) if size_m else (500, 400)

        # New window flags (v3.0)
        topmost    = "-t" in flags or "--topmost"    in raw
        borderless = "-b" in flags or "--borderless" in raw
        resizable  = "-r" in flags or "--resizable"  in raw
        minimized  = "-m" in flags or "--minimized"  in raw
        center     = "--center"   in raw
        opacity_s  = _parse_named(raw, "val") if "--opacity" in raw else None
        icon_path  = _parse_named(raw, "path") if "--icon" in raw else None
        win_title  = _parse_named(raw, "val") if "--title" in raw else None

        kind  = "notepad" if is_notepad else "window"
        extra: dict = {
            "enabled":    not disconnected,
            "bg": bg, "fg": fg, "width": w, "height": h,
            "topmost":    topmost,
            "borderless": borderless,
            "resizable":  resizable,
            "minimized":  minimized,
            "center":     center,
        }
        if opacity_s:  extra["opacity"]   = int(opacity_s)
        if icon_path:  extra["icon"]       = icon_path
        if win_title:  extra["title"]      = win_title

        if is_notepad:
            ext_m = re.search(r'as\s+\.(\w+)', raw)
            ext   = "." + ext_m.group(1) if ext_m else ".txt"
            create_dir = Path(base) / "create"
            create_dir.mkdir(parents=True, exist_ok=True)
            file_path = create_dir / f"{name}{ext}"
            if not file_path.exists():
                file_path.write_text("", encoding="utf-8")
            extra["file"] = str(file_path)
            extra["ext"]  = ext
            if not no_register:
                self._win_reg.add(name, "notepad", extra)
            if not only_create and not disconnected:
                self._open_notepad_file(str(file_path))
            if not silent:
                log.info("!create --notepad [%s] '%s' → %s", ext, name, file_path)
        else:
            if not no_register:
                self._win_reg.add(name, "window", extra)
            if not only_create and not disconnected:
                self._open_tk_window(name)
            if not silent:
                log.info("!create --window '%s'  bg=%s fg=%s size=%dx%d%s%s",
                         name, bg, fg, w, h,
                         "  [-o]" if only_create else "",
                         "  [-d]" if disconnected else "")

        if auto_connect and not disconnected:
            self._cmd_connect(f'!connect --window #name="{name}" 1')

    # --- !ls ------------------------------------------------------------------

    def _cmd_ls(self, raw: str):
        """!ls [--windows][--rules][--ae][--hotkeys][--all][-v][--json][--connected][--disconnected][--count]"""
        import json as _json
        tokens = raw.split()
        verbose      = "-v" in tokens or "--verbose" in tokens or "--all" in tokens
        as_json      = "--json"         in tokens
        only_win     = "--windows"      in tokens
        only_rule    = "--rules"        in tokens
        only_ae      = "--ae"           in tokens
        only_hk      = "--hotkeys"      in tokens
        only_conn    = "--connected"    in tokens
        only_disconn = "--disconnected" in tokens
        count_only   = "--count"        in tokens
        sort_by      = _parse_named(raw, "val") if "--sort" in raw else None
        show_all     = not any([only_win, only_rule, only_ae, only_hk])

        result: dict = {}

        def _filter_entry(entry: dict) -> bool:
            if only_conn:
                return entry.get("enabled", entry.get("connected", True))
            if only_disconn:
                return not entry.get("enabled", entry.get("connected", True))
            return True

        if show_all or only_win:
            wins = {}
            for n in self._win_reg.all_names():
                e = self._win_reg.get(n) or {}
                if _filter_entry(e):
                    wins[n] = e if verbose else {"kind": e.get("kind","window"), "enabled": e.get("enabled", True)}
            result["windows"] = wins

        if show_all or only_rule:
            rules = {}
            for n in self._rule_reg.all_names():
                e = self._rule_reg.get(n) or {}
                if _filter_entry({"connected": e.get("connected", True)}):
                    rules[n] = e if verbose else {"connected": e.get("connected", True)}
            result["rules"] = rules

        if show_all or only_ae:
            ae = {}
            for n in self._ae_reg.all_names():
                e = self._ae_reg.get(n) or {}
                ae[n] = e if verbose else {"type": e.get("type","?"), "hex": e.get("hex","")}
            result["ae"] = ae

        if show_all or only_hk:
            result["hotkeys"] = self._hotkeys.all_bindings()

        if count_only:
            for section, items in result.items():
                log.info("%s: %d", section, len(items))
            return

        if as_json:
            log.info(_json.dumps(result, ensure_ascii=False, indent=2))
        else:
            for section, items in result.items():
                names = sorted(items.keys()) if sort_by == "name" else list(items.keys())
                log.info("-- %s (%d) --", section, len(names))
                for name in names:
                    data = items[name]
                    if verbose:
                        log.info("  %-24s  %s", name, data)
                    else:
                        brief = "  ".join(f"{k}={v}" for k, v in data.items())
                        log.info("  %-24s  %s", name, brief)

    # --- !status --------------------------------------------------------------

    def _cmd_status(self, raw: str):
        """Current system state in one block."""
        tokens = raw.split()
        as_json = "--json" in tokens
        rec      = getattr(self.app, "recording", False)
        log_conn = bool(self._lib and self._lib.hr_con_log_connected()) if self._lib else False
        wins_on  = sum(1 for n in self._win_reg.all_names()
                       if (self._win_reg.get(n) or {}).get("enabled", True))
        rules_on = sum(1 for n in self._rule_reg.all_names()
                       if (self._rule_reg.get(n) or {}).get("connected", True))
        hk_count = len(self._hotkeys.all_bindings())

        data = {
            "recording": rec,
            "log":       log_conn,
            "windows":   {"active": wins_on,  "total": len(self._win_reg.all_names())},
            "rules":     {"active": rules_on, "total": len(self._rule_reg.all_names())},
            "hotkeys":   hk_count,
        }
        import json as _json
        if as_json:
            log.info(_json.dumps(data, ensure_ascii=False))
        else:
            log.info("recording : %s", "on" if rec else "off")
            log.info("log      : %s", "connected" if log_conn else "none")
            log.info("windows  : %d active / %d total", wins_on, len(self._win_reg.all_names()))
            log.info("rules    : %d connected / %d total", rules_on, len(self._rule_reg.all_names()))
            log.info("hotkeys  : %d bindings", hk_count)

    # --- !info ----------------------------------------------------------------

    def _cmd_func_list(self, raw: str):
        """!func.list — list all functions bound to @terminal."""
        funcs = self._term_funcs.all()
        if not funcs:
            log.info("No functions bound to @terminal. Use: !connect --func @terminal <alias> <cmd>")
            return
        log.info("Terminal functions (%d):", len(funcs))
        for alias, cmd in funcs.items():
            log.info("  %-18s → %s", alias, cmd)

    # ------------------------------------------------------------------ !check.er
    # ------------------------------------------------------------------ !check.er

    def _cmd_check_er(self, raw: str):
        """!check.er — interactive text-mode integrity/diagnostic scanner.
        Opens a text console menu (no GUI) in a daemon thread so the main thread stays responsive.
        Menu options:
          1 - Scan a specific video file for corruption
          2 - Scan all video files in output folder
          3 - Run HomRec self-diagnostics (FFmpeg, DLL, audio, overlays)
          q - Quit
        """
        import threading
        def _run_checker():
            import sys, os, subprocess as sp, re
            ffmpeg = getattr(self.app, 'ffmpeg_path', 'ffmpeg') or 'ffmpeg'
            out_dir = getattr(self.app, 'output_folder', '') or os.path.expanduser('~')

            def _con(msg, tag="info"):
                """Emit a log line visible in the HomRec console."""
                getattr(log, tag if tag in ("info","warning","error") else "info")(msg)

            def _probe_video(path: str) -> list[str]:
                """Run ffprobe on a file and return list of issues found."""
                issues = []
                if not os.path.exists(path):
                    issues.append(f"File not found: {path}"); return issues
                size = os.path.getsize(path)
                if size == 0:
                    issues.append("File is empty (0 bytes)"); return issues
                if size < 8192:
                    issues.append(f"File suspiciously small: {size} bytes")
                try:
                    r = sp.run(
                        [ffmpeg, '-v','error','-i', path,
                         '-f','null','-'],
                        capture_output=True, text=True, timeout=60,
                        creationflags=0x08000000 if sys.platform=='win32' else 0
                    )
                    err = r.stderr or ""
                    # Parse FFmpeg error lines
                    for line in err.splitlines():
                        line = line.strip()
                        if not line: continue
                        low = line.lower()
                        if any(k in low for k in ('corrupt','invalid','error','missing','truncated','broken','bad')):
                            issues.append(line[:160])
                    if r.returncode not in (0, 1):
                        issues.append(f"FFmpeg exit code {r.returncode}")
                except sp.TimeoutExpired:
                    issues.append("Probe timed out (>60s) — file may be very large or corrupt")
                except FileNotFoundError:
                    issues.append(f"ffmpeg not found at: {ffmpeg}")
                except Exception as e:
                    issues.append(f"Probe error: {e}")
                return issues

            def _report(path, issues):
                name = os.path.basename(path)
                if issues:
                    _con(f"[ISSUES] {name}:", "warning")
                    for iss in issues[:20]:
                        _con(f"  ! {iss}", "warning")
                    if len(issues) > 20:
                        _con(f"  ... and {len(issues)-20} more issues", "warning")
                else:
                    _con(f"[OK] {name} — no errors detected")

            def _self_diag():
                _con("--- HomRec Self-Diagnostics ---")
                # 1. FFmpeg
                if os.path.exists(ffmpeg):
                    try:
                        r = sp.run([ffmpeg,'-version'], capture_output=True, text=True, timeout=5,
                                   creationflags=0x08000000 if sys.platform=='win32' else 0)
                        ver_line = r.stdout.splitlines()[0] if r.stdout else "?"
                        _con(f"  FFmpeg: {ver_line}")
                    except Exception as e:
                        _con(f"  FFmpeg: ERROR — {e}", "warning")
                else:
                    _con(f"  FFmpeg: NOT FOUND at '{ffmpeg}'", "error")
                # 2. ddagrab
                ddagrab_ok = getattr(self.app, 'use_ddagrab', None)
                if ddagrab_ok is None:
                    _con("  ddagrab (game capture): not probed yet — start a recording to check")
                else:
                    _con(f"  ddagrab (game capture): {'supported ✓' if ddagrab_ok else 'not available — using GDI fallback'}")
                # 3. DLL
                dll_ok = self._lib is not None
                _con(f"  hr_console.dll: {'loaded ✓' if dll_ok else 'NOT LOADED'}", "info" if dll_ok else "error")
                # 4. Audio
                try:
                    devices = self.app.get_dshow_audio_devices()
                    _con(f"  Audio devices: {len(devices)} found — {', '.join(devices[:3])}" + ("…" if len(devices)>3 else ""))
                except Exception as e:
                    _con(f"  Audio devices: error reading — {e}", "warning")
                # 5. Overlays
                ovs = getattr(self.app, 'overlays', [])
                enabled_ovs = [o for o in ovs if o.get('enabled', True)]
                _con(f"  Overlays: {len(ovs)} defined, {len(enabled_ovs)} enabled")
                # 6. Output folder
                if os.path.isdir(out_dir):
                    videos = [f for f in os.listdir(out_dir) if f.lower().endswith(('.mp4','.mkv'))]
                    _con(f"  Output folder: {out_dir}  ({len(videos)} video files)")
                else:
                    _con(f"  Output folder: NOT FOUND — {out_dir}", "warning")
                _con("--- Diagnostics complete ---")

            # ---- Interactive menu ----
            _con("╔══════════════════════════════════════╗")
            _con("║   !check.er — HomRec Integrity Tool  ║")
            _con("╠══════════════════════════════════════╣")
            _con("║  1 - Scan a specific video file       ║")
            _con("║  2 - Scan all videos in output folder ║")
            _con("║  3 - Run HomRec self-diagnostics      ║")
            _con("║  q - Quit                             ║")
            _con("╚══════════════════════════════════════╝")

            # Parse inline arg if provided: !check.er 1 or !check.er 3
            inline = raw.strip().split()[1] if len(raw.strip().split()) > 1 else None
            choice = inline or "menu"

            if choice == "1":
                # List recent videos and ask
                videos = sorted(
                    [os.path.join(out_dir,f) for f in (os.listdir(out_dir) if os.path.isdir(out_dir) else [])
                     if f.lower().endswith(('.mp4','.mkv'))],
                    key=os.path.getmtime, reverse=True
                )[:20]
                if not videos:
                    _con("No video files found in output folder.", "warning")
                    return
                _con("Recent videos:")
                for i, v in enumerate(videos, 1):
                    sz = os.path.getsize(v) / 1048576
                    _con(f"  {i:2}. {os.path.basename(v)}  ({sz:.1f} MB)")
                _con("To scan a specific file, type:  !check.er scan #file="<path>"")
                _con(f"Scanning the most recent file: {os.path.basename(videos[0])}")
                issues = _probe_video(videos[0])
                _report(videos[0], issues)
            elif choice == "2":
                if not os.path.isdir(out_dir):
                    _con(f"Output folder not found: {out_dir}", "error"); return
                videos = [os.path.join(out_dir,f) for f in os.listdir(out_dir) if f.lower().endswith(('.mp4','.mkv'))]
                if not videos:
                    _con("No video files found.", "warning"); return
                _con(f"Scanning {len(videos)} video files in: {out_dir}")
                bad = 0
                for v in videos:
                    issues = _probe_video(v)
                    _report(v, issues)
                    if issues: bad += 1
                _con(f"--- Scan complete: {len(videos)} files, {bad} with issues ---")
            elif choice == "3":
                _self_diag()
            elif inline and inline.startswith("#file="):
                path = re.sub(r'^#file=["\']?', '', inline).rstrip('"\'')
                _con(f"Scanning: {path}")
                _report(path, _probe_video(path))
            elif choice == "scan":
                # !check.er scan #file="..."
                m = re.search(r'#file=["\']?([^"\' ]+)["\']?', raw)
                if m:
                    path = m.group(1).strip()
                    _con(f"Scanning: {path}")
                    _report(path, _probe_video(path))
                else:
                    _con('Usage: !check.er scan #file="path/to/video.mp4"', "warning")
            else:
                _con("Type  !check.er 1  to scan a video,  !check.er 2  to scan all,  !check.er 3  for diagnostics.")

        threading.Thread(target=_run_checker, daemon=True, name="check.er").start()

    # ------------------------------------------------------------------ /!check.er

    def _cmd_info(self, raw: str):
        """!info --window|--rule|--ae|--hotkey #name="..." / #key="..."""
        tokens = raw.split()
        as_json = "--json" in tokens
        import json as _json

        def _show(d: dict):
            if as_json:
                log.info(_json.dumps(d, ensure_ascii=False, indent=2))
            else:
                for k, v in d.items():
                    log.info("  %-20s %s", k + ":", v)

        if "--window" in raw:
            name = _parse_named(raw, "name")
            if not name: log.warning("!info --window: #name not specified"); return
            e = self._win_reg.get(name)
            if e is None: log.warning("!info --window '%s': not found", name); return
            _show({"name": name, **e}); return

        if "--rule" in raw:
            name = _parse_named(raw, "name")
            if not name: log.warning("!info --rule: #name not specified"); return
            e = self._rule_reg.get(name)
            if e is None: log.warning("!info --rule '%s': not found", name); return
            steps = [s.strip() for s in e.get("body","").split(";") if s.strip()]
            _show({"name": name, "connected": e.get("connected", True),
                   "steps": len(steps), "body": e.get("body","")}); return

        if "--ae" in raw:
            name = _parse_named(raw, "name")
            if not name: log.warning("!info --ae: #name not specified"); return
            e = self._ae_reg.get(name)
            if e is None: log.warning("!info --ae '%s': not found", name); return
            _show({"name": name, **e}); return

        if "--hotkey" in raw:
            key = _parse_named(raw, "key")
            if not key: log.warning("!info --hotkey: #key not specified"); return
            b = self._hotkeys.all_bindings().get(key)
            if b is None: log.warning("!info --hotkey '%s': not found", key); return
            _show({"key": key, **(b if isinstance(b, dict) else {"cmd": b})}); return

        log.warning("!info: use --window / --rule / --ae / --hotkey")

    # --- !history -------------------------------------------------------------

    def _cmd_history(self, raw: str):
        """!history [#count=N] [--clear] [--search "text"]"""
        tokens = raw.split()
        if "--clear" in tokens:
            self._history.clear()
            log.info("history cleared"); return

        search  = _parse_named(raw, "search")
        count_s = _parse_named(raw, "count")
        count   = int(count_s) if count_s and count_s.isdigit() else 20

        hist = self._history[:]
        if search:
            hist = [h for h in hist if search.lower() in h.lower()]
        hist = hist[-count:]
        for i, line in enumerate(hist, 1):
            log.info("  %3d  %s", i, line)

    # --- !alias ---------------------------------------------------------------

    def _cmd_alias(self, raw: str):
        """!alias #name="sr" #cmd="!start --rec 1" | --list | --remove #name="sr" """
        tokens = raw.split()
        if "--list" in tokens:
            aliases = self._alias_reg.all()
            if not aliases:
                log.info("no aliases"); return
            for n, c in aliases.items():
                log.info("  %-16s → %s", n, c)
            return
        if "--remove" in tokens:
            name = _parse_named(raw, "name")
            if not name: log.warning("!alias --remove: #name not specified"); return
            if self._alias_reg.remove(name):
                log.info("!alias: removed '%s'", name)
            else:
                log.warning("!alias --remove: '%s' not found", name)
            return
        name = _parse_named(raw, "name")
        cmd  = _parse_named(raw, "cmd")
        if not name or not cmd:
            log.warning("!alias: #name=... and #cmd=... required"); return
        self._alias_reg.add(name, cmd)
        log.info("!alias: '%s' → %s", name, cmd)

    # --- !repeat --------------------------------------------------------------

    def _cmd_repeat(self, raw: str):
        """!repeat #count=N <command>"""
        import re
        m = re.match(r'!repeat\s+#count=(\d+)\s+(.+)', raw, re.IGNORECASE)
        if not m:
            log.warning("!repeat: syntax: !repeat #count=N <command>"); return
        count = int(m.group(1))
        cmd   = m.group(2).strip()
        for i in range(count):
            log.info("!repeat [%d/%d]: %s", i + 1, count, cmd)
            self._dispatch_extended(cmd)

    # --- !delay ---------------------------------------------------------------

    def _cmd_delay(self, raw: str):
        """!delay #ms=N <command>"""
        import re
        m = re.match(r'!delay\s+#ms=(\d+)\s+(.+)', raw, re.IGNORECASE)
        if not m:
            log.warning("!delay: syntax: !delay #ms=N <command>"); return
        ms  = int(m.group(1))
        cmd = m.group(2).strip()
        log.info("!delay: after %dms → %s", ms, cmd)
        t = threading.Timer(ms / 1000.0, lambda: self._dispatch_extended(cmd))
        t.daemon = True
        t.start()

    # --- !batch ---------------------------------------------------------------

    def _cmd_batch(self, raw: str):
        """!batch cmd1 && cmd2 && ...  [-x / --stop-on-error]"""
        import re
        stop_on_error = "-x" in raw.split() or "--stop-on-error" in raw
        body = re.sub(r'^!batch\s*', '', raw, flags=re.IGNORECASE)
        body = re.sub(r'\s+(-x|--stop-on-error)\b', '', body)
        parts = [p.strip() for p in body.split("&&") if p.strip()]
        if not parts:
            log.warning("!batch: no commands"); return
        for part in parts:
            log.info("!batch → %s", part)
            try:
                self._dispatch_extended(part)
            except Exception as e:
                log.error("!batch error: %s", e)
                if stop_on_error:
                    log.warning("!batch: stopped on error (-x)"); return

    # --- !run -----------------------------------------------------------------

    def _cmd_run(self, raw: str):
        """!run #file="script.hrc" [--encoding utf8|cp1251] [--ignore-errors] [--echo-each] [-x]"""
        file_path = _parse_named(raw, "file")
        if not file_path:
            log.warning("!run: #file not specified"); return
        encoding      = _parse_named(raw, "encoding") or "utf-8"
        ignore_errors = "--ignore-errors" in raw
        echo_each     = "--echo-each"     in raw
        stop_on_error = "-x" in raw.split() or "--stop-on-error" in raw

        p = Path(file_path)
        if not p.is_absolute():
            p = Path(_get_base_dir()) / p
        if not p.exists():
            log.warning("!run: file not found: %s", p); return

        try:
            text = p.read_text(encoding=encoding)
        except Exception as e:
            log.error("!run: file read error: %s", e); return

        lines = [l.strip() for l in text.splitlines()]
        for i, line in enumerate(lines, 1):
            if not line or line.startswith("#"):
                continue
            if echo_each:
                log.info("!run [%d]: %s", i, line)
            try:
                self._dispatch_extended(line)
            except Exception as e:
                log.error("!run [%d] error '%s': %s", i, line, e)
                if stop_on_error and not ignore_errors:
                    log.warning("!run: stopped (-x)"); return

    # --- !clear ---------------------------------------------------------------

    def _cmd_dollar_clear(self, raw: str):
        """
        $clear --app  — delete all application data (registries) and close main window.
        """
        tokens = raw.split()
        silent = "-s" in tokens or "--silent" in tokens
        if "--app" not in raw:
            # $clear without --app → clear console
            self._cmd_clear(raw)
            return

        if not silent:
            self._con_warn("Clearing ALL app data: windows, rules, ae, aliases, hotkeys...")

        # Clear all registries
        try:
            for name in list(self._win_reg.all_names()):
                entry = self._win_reg.get(name) or {}
                if entry.get("kind") == "notepad":
                    fp = entry.get("file", "")
                    if fp:
                        from pathlib import Path as _P
                        try: _P(fp).unlink(missing_ok=True)
                        except Exception: pass
                self._win_reg.remove(name)
        except Exception as e:
            log.warning("$clear --app: win_reg error: %s", e)

        try:
            for name in list(self._rule_reg.all_names()):
                self._rule_reg.remove(name)
        except Exception as e:
            log.warning("$clear --app: rule_reg error: %s", e)

        try:
            for name in list(self._ae_reg.all_names()):
                self._ae_reg.remove(name)
        except Exception as e:
            log.warning("$clear --app: ae_reg error: %s", e)

        try:
            for name in list(self._alias_reg.all()):
                self._alias_reg.remove(name)
        except Exception as e:
            log.warning("$clear --app: alias_reg error: %s", e)

        try:
            for key in list(self._hotkeys.all_bindings()):
                self._hotkeys.unbind(key)
        except Exception as e:
            log.warning("$clear --app: hotkeys error: %s", e)

        self._history.clear()
        self._env._vars.clear()

        if not silent:
            self._con_ok("All app data cleared.")

        log.info("$clear --app: all registries wiped")

        # Close main window
        if self._root_alive():
            self.app.root.after(300, self.app.quit_app)

    def _cmd_clear(self, raw: str):
        """!clear — clear console output."""
        # Special marker that can be intercepted on the UI side
        log.info("\x00CLEAR_CONSOLE\x00")

    # --- !echo ----------------------------------------------------------------

    def _cmd_echo(self, raw: str):
        """!echo [--ok|--warn|--err] <text>"""
        import re
        body = re.sub(r'^!echo\s*', '', raw, flags=re.IGNORECASE)
        level = "info"
        if body.startswith("--ok"):
            level = "ok";   body = body[4:].lstrip()
        elif body.startswith("--warn"):
            level = "warn"; body = body[6:].lstrip()
        elif body.startswith("--err"):
            level = "err";  body = body[5:].lstrip()
        body = self._env.resolve(_resolve_math(body))
        if level == "ok":
            log.info("✔  %s", body)
        elif level == "warn":
            log.warning("⚠  %s", body)
        elif level == "err":
            log.error("✖  %s", body)
        else:
            log.info(body)

    # --- !clip ----------------------------------------------------------------

    def _cmd_clip(self, raw: str):
        """!clip --copy "text" | --paste | --clear"""
        tokens = raw.split()
        if "--clear" in tokens:
            try:
                import tkinter as tk
                r = tk.Tk(); r.withdraw()
                r.clipboard_clear(); r.update(); r.destroy()
                log.info("!clip: clipboard cleared")
            except Exception as e:
                log.warning("!clip --clear: %s", e)
            return
        if "--paste" in tokens:
            try:
                import tkinter as tk
                r = tk.Tk(); r.withdraw()
                text = r.clipboard_get(); r.destroy()
                log.info("!clip --paste: %s", text)
            except Exception as e:
                log.warning("!clip --paste: %s", e)
            return
        if "--copy" in tokens:
            import re
            m = re.search(r'--copy\s+"([^"]*)"', raw)
            if not m:
                m = re.search(r"--copy\s+'([^']*)'", raw)
            text = m.group(1) if m else " ".join(
                tokens[tokens.index("--copy") + 1:] if "--copy" in tokens else [])
            try:
                import tkinter as tk
                r = tk.Tk(); r.withdraw()
                r.clipboard_clear(); r.clipboard_append(text); r.update(); r.destroy()
                log.info("!clip: copied: %s", text)
            except Exception as e:
                log.warning("!clip --copy: %s", e)
            return
        log.warning("!clip: use --copy \"text\" / --paste / --clear")

    # --- !env -----------------------------------------------------------------

    def _cmd_env(self, raw: str):
        """!env --set #name="x" #val="y" | --get #name | --list | --unset #name"""
        tokens = raw.split()
        if "--list" in tokens:
            vs = self._env.all()
            if not vs: log.info("no variables"); return
            for k, v in vs.items():
                log.info("  $%-20s = %s", k, v)
            return
        if "--unset" in tokens:
            name = _parse_named(raw, "name")
            if not name: log.warning("!env --unset: #name not specified"); return
            if self._env.unset(name): log.info("!env: $%s removed", name)
            else: log.warning("!env: $%s not found", name)
            return
        if "--get" in tokens:
            name = _parse_named(raw, "name")
            if not name: log.warning("!env --get: #name not specified"); return
            val = self._env.get(name)
            if val is None: log.warning("!env: $%s not set", name)
            else: log.info("$%s = %s", name, val)
            return
        if "--set" in tokens:
            name = _parse_named(raw, "name")
            val  = _parse_named(raw, "val")
            if not name: log.warning("!env --set: #name not specified"); return
            self._env.set(name, val or "")
            log.info("!env: $%s = %s", name, val)
            return
        log.warning("!env: use --set / --get / --list / --unset")

    # --- !timer ---------------------------------------------------------------

    def _cmd_timer(self, raw: str):
        """!timer #name="x" #ms=N <cmd> | --cancel #name | --list"""
        tokens = raw.split()
        if "--list" in tokens:
            if not self._timers:
                log.info("no active timers"); return
            for n in list(self._timers.keys()):
                log.info("  %-20s (active)", n)
            return
        if "--cancel" in tokens:
            name = _parse_named(raw, "name")
            if not name: log.warning("!timer --cancel: #name not specified"); return
            t = self._timers.pop(name, None)
            if t: t.cancel(); log.info("!timer '%s': cancelled", name)
            else: log.warning("!timer '%s': not found", name)
            return

        name  = _parse_named(raw, "name")
        ms_s  = _parse_named(raw, "ms")
        if not name or not ms_s:
            log.warning("!timer: #name and #ms required"); return
        import re
        # Command — everything after last named parameter
        cmd_m = re.search(r'#ms=\d+\s+(.+)', raw)
        if not cmd_m:
            log.warning("!timer: command not found after #ms=N"); return
        cmd = cmd_m.group(1).strip()
        ms  = int(ms_s)

        def _fire(n=name, c=cmd):
            self._timers.pop(n, None)
            log.info("!timer '%s' fired → %s", n, c)
            self._dispatch_extended(c)

        t = threading.Timer(ms / 1000.0, _fire)
        t.daemon = True; t.start()
        self._timers[name] = t
        log.info("!timer '%s': after %dms → %s", name, ms, cmd)

    # --- !watch ---------------------------------------------------------------

    def _cmd_watch(self, raw: str):
        """!watch #name="x" #ms=N <cmd> [--max-runs #count=N] | --stop #name | --list"""
        tokens = raw.split()
        if "--list" in tokens:
            if not self._watchers:
                log.info("no active watchers"); return
            for n, w in self._watchers.items():
                log.info("  %-20s  ms=%d  cmd=%s  runs=%d", n, w["ms"], w["cmd"], w["runs"])
            return
        if "--stop" in tokens:
            name = _parse_named(raw, "name")
            if not name: log.warning("!watch --stop: #name not specified"); return
            w = self._watchers.pop(name, None)
            if w: w["stop_event"].set(); log.info("!watch '%s': stopped", name)
            else: log.warning("!watch '%s': not found", name)
            return

        name  = _parse_named(raw, "name")
        ms_s  = _parse_named(raw, "ms")
        if not name or not ms_s:
            log.warning("!watch: #name and #ms required"); return

        import re
        # Command — everything between #ms=N and first -- flag
        cmd_m = re.search(r'#ms=\d+\s+(.*?)(?:\s+--|$)', raw)
        if not cmd_m:
            cmd_m = re.search(r'#ms=\d+\s+(.+)', raw)
        if not cmd_m:
            log.warning("!watch: command not found after #ms=N"); return
        cmd = cmd_m.group(1).strip()
        ms  = int(ms_s)

        max_runs_s = _parse_named(raw, "count") if "--max-runs" in raw else None
        max_runs   = int(max_runs_s) if max_runs_s and max_runs_s.isdigit() else 0

        jitter_ms_s = None
        if "--jitter" in raw:
            m2 = re.search(r'--jitter\s+#ms=(\d+)', raw)
            jitter_ms_s = m2.group(1) if m2 else None
        jitter_ms = int(jitter_ms_s) if jitter_ms_s else 0

        stop_event = threading.Event()
        info = {"ms": ms, "cmd": cmd, "stop_event": stop_event, "runs": 0, "max_runs": max_runs}
        self._watchers[name] = info

        def _loop(n=name, c=cmd, i=info):
            import random as _r
            while not i["stop_event"].is_set():
                interval = ms / 1000.0
                if jitter_ms:
                    interval += _r.uniform(-jitter_ms, jitter_ms) / 1000.0
                i["stop_event"].wait(max(0.001, interval))
                if i["stop_event"].is_set():
                    break
                i["runs"] += 1
                log.info("!watch '%s' [#%d] → %s", n, i["runs"], c)
                try:
                    self._dispatch_extended(c)
                except Exception as e:
                    log.error("!watch '%s' error: %s", n, e)
                if max_runs and i["runs"] >= max_runs:
                    log.info("!watch '%s': max-runs=%d reached, stopped", n, max_runs)
                    self._watchers.pop(n, None)
                    break

        t = threading.Thread(target=_loop, daemon=True)
        t.start()
        log.info("!watch '%s': every %dms%s → %s%s",
                 name, ms, f"±{jitter_ms}ms" if jitter_ms else "",
                 cmd, f"  (max {max_runs})" if max_runs else "")

    # --- Console output helper ------------------------------------------------

    def _con_write(self, text: str, tag: int = 0):
        """Print a line directly to the DLL console window (tag: 0=text 1=ok 2=warn 3=err 4=dim 5=accent)."""
        if self._lib:
            try:
                self._lib.hr_con_write(text, tag)
                return
            except Exception as e:
                log.warning("_con_write: %s", e)
        log.info(text)

    def _con_ok  (self, s: str): self._con_write("  \u2714  " + s, 1)
    def _con_info(self, s: str): self._con_write("  \u00b7  " + s, 4)
    def _con_warn(self, s: str): self._con_write("  \u26a0  " + s, 2)
    def _con_err (self, s: str): self._con_write("  \u2716  " + s, 3)

    # --- !ping ----------------------------------------------------------------

    def _cmd_ping(self, raw: str):
        import time
        t0 = time.perf_counter()
        elapsed = (time.perf_counter() - t0) * 1000
        self._con_ok(f"pong  ({elapsed:.3f} ms)")

    # --- !version -------------------------------------------------------------

    def _cmd_version(self, raw: str):
        self._con_write(f"  Console  {CONSOLE_VERSION}  |  Bridge  {BRIDGE_VERSION}  |  Python  {sys.version.split()[0]}", 5)

    # --- !homrec --------------------------------------------------------------

    def _cmd_homrec(self, raw: str):
        """
        !homrec --version   → prints version banner
        !homrec --help      → prints available !homrec sub-commands
        """
        tokens = raw.split()
        if "--version" in tokens or "-v" in tokens:
            self._con_write(
                f"Version HomRec - {HOMREC_VERSION}, "
                f"Core version - {CORE_VERSION}, "
                f"Console version {CONSOLE_VERSION}",
                5,
            )
            return
        if "--help" in tokens or "-h" in tokens:
            self._con_write("  !homrec sub-commands:", 5)
            self._con_info("  --version  / -v   Show version info")
            self._con_info("  --help     / -h   Show this help")
            return
        self._con_warn("!homrec: unknown option. Try  !homrec --version  or  !homrec --help")

    # --- !log -----------------------------------------------------------------

    def _cmd_log(self, raw: str):
        """!log --tail [#count=N] | --search "x" [--invert] | --level info|warn|err | --clear"""
        tokens   = raw.split()
        base     = _get_base_dir()
        log_path = Path(base) / "homrec.log"

        if "--clear" in tokens:
            try:
                log_path.write_text("", "utf-8")
                log.info("!log: homrec.log cleared")
            except Exception as e:
                log.warning("!log --clear: %s", e)
            return

        if not log_path.exists():
            log.warning("!log: homrec.log not found"); return
        try:
            lines = log_path.read_text("utf-8", errors="replace").splitlines()
        except Exception as e:
            log.warning("!log: read error: %s", e); return

        if "--search" in tokens:
            import re
            m = re.search(r'--search\s+"([^"]*)"', raw)
            if not m: m = re.search(r"--search\s+'([^']*)'", raw)
            if not m:
                idx = tokens.index("--search")
                term = tokens[idx + 1] if idx + 1 < len(tokens) else ""
            else:
                term = m.group(1)
            invert = "--invert" in tokens
            lines  = [l for l in lines if (term.lower() in l.lower()) != invert]

        level_val = _parse_named(raw, "level")
        if level_val and "--level" in raw:
            lmap = {"info": "INFO", "warn": "WARNING", "err": "ERROR"}
            target = lmap.get(level_val.lower(), level_val.upper())
            lines  = [l for l in lines if target in l]

        since = _parse_named(raw, "time")
        if since and "--since" in raw:
            lines = [l for l in lines if len(l) >= 5 and l[:5] >= since]

        count_s = _parse_named(raw, "count")
        count   = int(count_s) if count_s and count_s.isdigit() else 20
        lines   = lines[-count:]
        for line in lines:
            log.info(line)

    # --------------------------------------------------------------------------

    def _run_rule_body(self, rule_name: str, body: str, silent: bool = False):
        """
        Executes the rule body steps.
        Steps separated by ';'.  Keywords:
          then  — just a separator (ignored)
          !start --rec 1 then   → [!start --rec 1]
        """
        import re
        steps = [s.strip() for s in body.split(";") if s.strip()]
        for step in steps:
            # Remove trailing 'then'
            step = re.sub(r'\s+then\s*$', '', step, flags=re.IGNORECASE).strip()
            if not step:
                continue
            if not silent:
                log.info("  rule '%s' → %s", rule_name, step)
            self._dispatch_extended(step)

    # --- !start --terminal as @terminal ------------------------------------

    def _cmd_start_terminal(self, raw: str):
        """!start --terminal as @terminal  — launch hr_terminal.exe."""
        tokens = raw.split()
        silent = "-s" in tokens or "--silent" in tokens
        if "as" not in tokens or "@terminal" not in tokens:
            log.warning("!start --terminal: syntax: !start --terminal as @terminal")
            return
        base = _get_base_dir()
        exe  = os.path.join(base, "hr_terminal.exe")
        if not os.path.exists(exe):
            log.warning("!start --terminal: hr_terminal.exe not found in %s", base)
            return
        try:
            _popen_no_window([exe], cwd=base)
            if not silent:
                log.info("!start --terminal: hr_terminal.exe started")
        except Exception as e:
            log.error("!start --terminal: launch error: %s", e)

    # --- !start --window ------------------------------------------------------

    def _cmd_start_window(self, raw: str):
        """!start --window #name="x"  — reopen a created window."""
        if "--window" not in raw:
            log.warning("!start: use --window or --rec"); return
        name = _parse_named(raw, "name")
        if not name:
            log.warning("!start --window: #name not specified"); return
        entry = self._win_reg.get(name)
        if entry is None:
            log.warning("!start --window '%s': not found in registry", name); return
        if entry.get("kind") == "notepad":
            fp = entry.get("file", "")
            if fp:
                self._open_notepad_file(fp)
            else:
                log.warning("!start: no file path for '%s'", name)
        else:
            self._open_tk_window(name)

    # --- !rename --------------------------------------------------------------

    def _cmd_rename(self, raw: str):
        """
        !rename --window  #name="old_name" to #name="new_name"
        !rename --rule    #name="old_name" to #name="new_name"
        !rename --ae      #name="old_name" to #name="new_name"
        !rename --hotkey  #name="old_name" to #name="new_name"
        !rename --window  @all #prefix="pfx_"          (add prefix to all)
        !rename --window  @all #suffix="_v2"           (add suffix to all)
        !rename --window  @all #replace="old" to="new" (replace substring in all names)
        """
        import re
        tokens = raw.split()
        silent = "-s" in tokens or "--silent" in tokens

        use_all = "--all" in raw  # @all already normalized to --all on the DLL side

        def _do_rename(registry, reg_type: str, old: str, new_name: str) -> bool:
            entry = registry.get(old)
            if entry is None:
                self._con_warn(f"!rename {reg_type}: '{old}' not found")
                return False
            registry.add(new_name, **({} if reg_type == "ae" else {}))
            # Move data: delete old, create new
            if reg_type == "window":
                data = dict(entry)
                registry.remove(old)
                registry.add(new_name, data.get("kind", "window"), data)
            elif reg_type == "rule":
                body = entry.get("body", "")
                conn = entry.get("connected", True)
                registry.remove(old)
                registry.add(new_name, body, conn)
            elif reg_type == "ae":
                ae_type = entry.get("type", "color")
                data = {k: v for k, v in entry.items() if k != "type"}
                registry.remove(old)
                registry.add(new_name, ae_type, data)
            return True

        # Determine object type
        reg = None
        reg_type = ""
        if "--window" in raw:
            reg, reg_type = self._win_reg, "window"
        elif "--rule" in raw:
            reg, reg_type = self._rule_reg, "rule"
        elif "--ae" in raw:
            reg, reg_type = self._ae_reg, "ae"
        elif "--hotkey" in raw:
            # Hotkeys are renamed via alias
            if use_all:
                self._con_warn("!rename --hotkey @all: not supported"); return
            old = _parse_named(raw, "name")
            m2  = re.search(r'\bto\b\s+#name=["\']?([^"\';\s]+)["\']?', raw)
            new_name = m2.group(1) if m2 else None
            if not old or not new_name:
                self._con_warn('!rename --hotkey: #name="old" to #name="new" required'); return
            # Rename alias hotkey
            unbound = self._hotkeys.unbind_by_alias(old)
            if unbound:
                # rebind with new alias
                bindings = self._hotkeys.all_bindings()
                if unbound in bindings:
                    cmd_val = bindings[unbound]
                    cmd_str = cmd_val.get("cmd", "") if isinstance(cmd_val, dict) else str(cmd_val)
                    self._hotkeys.bind(unbound, cmd_str, alias=new_name)
                    if not silent:
                        self._con_ok(f"!rename --hotkey '{old}' → '{new_name}'")
            else:
                self._con_warn(f"!rename --hotkey '{old}': not found")
            return
        else:
            self._con_warn("!rename: use --window / --rule / --ae / --hotkey"); return

        # @all — batch rename
        if use_all:
            prefix  = _parse_named(raw, "prefix")  or ""
            suffix  = _parse_named(raw, "suffix")  or ""
            replace_from = _parse_named(raw, "replace") or None
            replace_to_m = re.search(r'\bto=["\']?([^"\';\s]+)["\']?', raw)
            replace_to   = replace_to_m.group(1) if replace_to_m else ""

            names = list(reg.all_names())
            count = 0
            for old in names:
                new_name = old
                if prefix:   new_name = prefix + new_name
                if suffix:   new_name = new_name + suffix
                if replace_from is not None:
                    new_name = new_name.replace(replace_from, replace_to)
                if new_name == old:
                    continue
                if _do_rename(reg, reg_type, old, new_name):
                    count += 1
                    if not silent:
                        self._con_info(f"  '{old}' → '{new_name}'")
            if not silent:
                self._con_ok(f"!rename @all ({reg_type}): renamed {count} of {len(names)}")
            return

        # Single rename: #name="old" to #name="new"
        old = _parse_named(raw, "name")
        m2  = re.search(r'\bto\b\s+#name=["\']?([^"\';\s]+)["\']?', raw)
        if not m2:
            # try format: #name="old" to #name="new"
            m2 = re.search(r'#name=["\']?[^"\';\s]+["\']?\s+to\s+#name=["\']?([^"\';\s]+)["\']?', raw)
        new_name = m2.group(1) if m2 else None

        if not old or not new_name:
            self._con_warn('!rename: syntax: !rename --window #name="old" to #name="new"')
            return
        if old == new_name:
            self._con_warn(f"!rename: names are identical ('{old}')"); return
        if reg.exists(new_name):
            self._con_warn(f"!rename: '{new_name}' already exists"); return

        if _do_rename(reg, reg_type, old, new_name):
            if not silent:
                self._con_ok(f"!rename --{reg_type} '{old}' → '{new_name}'")

    # --- $rm ------------------------------------------------------------------

    def _cmd_rm(self, raw: str):
        """
        $rm --window #name="x"     [-q][--purge][--if-disconnected]
        $rm --rule   #name="x"     [-q][--if-disconnected]
        $rm --ae     #name="x"     [-q]
        $rm --all --window|--rule|--ae  [-y]
        """
        flags  = _parse_flags(raw)
        quiet  = "-q" in flags or "-y" in flags
        purge  = "--purge" in raw
        if_dis = "--if-disconnected" in raw
        name   = _parse_named(raw, "name")

        def _confirm(msg: str) -> bool:
            if quiet: return True
            if self._root_alive():
                import tkinter.messagebox as mb
                return mb.askyesno("Delete", msg)
            return True

        # --all: remove everything from registry
        if "--all" in raw:
            if "--window" in raw:
                names = self._win_reg.all_names()
                if not _confirm(f"Delete all {len(names)} windows from registry?"):
                    log.info("$rm --all --window: cancelled"); return
                for n in names:
                    if if_dis:
                        e = self._win_reg.get(n) or {}
                        if e.get("enabled", True): continue
                    self._win_reg.remove(n)
                log.info("$rm --all --window: removed %d entries", len(names))
            elif "--rule" in raw:
                names = self._rule_reg.all_names()
                if not _confirm(f"Delete all {len(names)} rules?"):
                    log.info("$rm --all --rule: cancelled"); return
                for n in names:
                    if if_dis:
                        e = self._rule_reg.get(n) or {}
                        if e.get("connected", True): continue
                    self._rule_reg.remove(n)
                log.info("$rm --all --rule: removed %d entries", len(names))
            elif "--ae" in raw:
                names = self._ae_reg.all_names()
                if not _confirm(f"Delete all {len(names)} ae-objects?"):
                    log.info("$rm --all --ae: cancelled"); return
                for n in names:
                    self._ae_reg.remove(n)
                log.info("$rm --all --ae: removed %d entries", len(names))
            else:
                log.warning("$rm --all: specify --window / --rule / --ae")
            return

        if not name:
            log.warning('$rm: #name not specified  (example: $rm --window #name="x")'); return

        if "--window" in raw:
            if not self._win_reg.exists(name):
                log.warning("$rm: '%s' not found in registry", name); return
            entry = self._win_reg.get(name)
            if if_dis and entry and entry.get("enabled", True):
                log.info("$rm: '%s' is currently enabled — skipped (--if-disconnected)", name); return
            if not _confirm(f"Delete window '{name}' from homrec.create?"):
                log.info("$rm: cancelled"); return
            # BUG FIX: always delete notepad file (not only with --purge)
            if entry and entry.get("kind") == "notepad":
                fp = entry.get("file", "")
                if fp and Path(fp).exists():
                    try: Path(fp).unlink(); log.info("$rm: notepad file deleted: %s", fp)
                    except Exception as e: log.warning("$rm: failed to delete file: %s", e)
            if purge:
                # remove hotkeys referencing this window
                for key, val in list(self._hotkeys.all_bindings().items()):
                    cmd_val = val.get("cmd","") if isinstance(val, dict) else str(val)
                    if name in cmd_val:
                        self._hotkeys.unbind(key)
                        log.info("$rm --purge: hotkey '%s' removed", key)
            self._win_reg.remove(name)
            log.info("$rm --window: '%s' removed", name)
            return

        if "--rule" in raw:
            if not self._rule_reg.exists(name):
                log.warning("$rm --rule: '%s' not found", name); return
            entry = self._rule_reg.get(name)
            if if_dis and entry and entry.get("connected", True):
                log.info("$rm --rule: '%s' is currently connected — skipped", name); return
            if not _confirm(f"Delete rule '{name}'?"):
                log.info("$rm: cancelled"); return
            if purge:
                for key, val in list(self._hotkeys.all_bindings().items()):
                    cmd_val = val.get("cmd","") if isinstance(val, dict) else str(val)
                    if name in cmd_val:
                        self._hotkeys.unbind(key)
            self._rule_reg.remove(name)
            log.info("$rm --rule: '%s' removed", name)
            return

        if "--ae" in raw:
            if not self._ae_reg.exists(name):
                log.warning("$rm --ae: '%s' not found", name); return
            if not _confirm(f"Delete ae-object '{name}'?"):
                log.info("$rm: cancelled"); return
            self._ae_reg.remove(name)
            log.info("$rm --ae: '%s' removed", name)
            return

        log.warning("$rm: use --window / --rule / --ae")

    # --- !connect -------------------------------------------------------------

    def _cmd_connect(self, raw: str):
        """
        !connect --window  #name="x" 1   [-s][-q][--all][--toggle][-f]
        !connect --rule    #name="x"     [-s][-q][--all][--toggle]
        !connect --function <cmd> to|; <key> [#name="func"]  [-s][-q]
        """
        import re
        tokens = raw.split()
        silent  = "-s" in tokens or "--silent" in tokens
        toggle  = "--toggle"  in tokens
        all_obj = "--all"     in tokens
        force   = "-f" in tokens or "--force" in tokens

        if "--window" in raw:
            # --all: connect all windows
            if all_obj:
                for name in self._win_reg.all_names():
                    self._cmd_connect(f'!connect --window #name="{name}" 1 -s')
                if not silent: log.info("!connect --window --all: all windows connected")
                return
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!connect --window: #name not specified"); return
            val = None
            for t in tokens:
                if t in ("0", "1"): val = int(t)
            entry = self._win_reg.get(name)
            if entry is None:
                log.warning("!connect --window '%s': not found", name); return
            if toggle:
                val = 0 if entry.get("enabled", True) else 1
            enabled = (val != 0) if val is not None else True
            if not force and entry.get("enabled") == enabled:
                if not silent: log.info("!connect --window '%s': already in state %s", name, enabled)
                return
            entry["enabled"] = enabled
            self._win_reg.add(name, entry.get("kind", "window"), entry)
            if enabled:
                self._cmd_start_window(f'!start --window #name="{name}"')
            if not silent:
                log.info("!connect --window '%s' → %s", name, "enabled" if enabled else "disabled")
            return

        if "--rule" in raw:
            if all_obj:
                for name in self._rule_reg.all_names():
                    self._cmd_connect(f'!connect --rule #name="{name}" -s')
                if not silent: log.info("!connect --rule --all: all rules connected")
                return
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!connect --rule: #name not specified"); return
            if not self._rule_reg.exists(name):
                log.warning("!connect --rule '%s': not found", name); return
            entry = self._rule_reg.get(name)
            if toggle:
                new_state = not entry.get("connected", True)
                self._rule_reg.set_connected(name, new_state)
                if not silent:
                    log.info("!connect --rule '%s' toggle → %s", name,
                             "connected" if new_state else "disconnected")
                if new_state:
                    body = entry.get("body", "") if entry else ""
                    if body: self._run_rule_body(name, body, silent)
                return
            if not force and entry and entry.get("connected", False):
                if not silent: log.info("!connect --rule '%s': already connected", name)
                return
            self._rule_reg.set_connected(name, True)
            entry = self._rule_reg.get(name)
            if not silent: log.info("!connect --rule '%s' → connected", name)
            body = entry.get("body", "") if entry else ""
            if body: self._run_rule_body(name, body, silent)
            return

        if "--function" in raw:
            m = re.search(
                r'--function\s+(.+?)\s+(?:to|;)\s+(\S+)'
                r'(?:\s+#name=["\']?([^"\';\s]+)["\']?)?',
                raw, re.IGNORECASE
            )
            if not m:
                log.warning('!connect --function: syntax: !connect --function <cmd> to|; <key> [#name="func"]')
                return
            cmd_part  = m.group(1).strip()
            key_part  = m.group(2).strip()
            func_name = m.group(3)
            self._hotkeys.bind(key_part, cmd_part, alias=func_name)
            if not silent:
                log.info("!connect --function '%s' → %s%s",
                         key_part, cmd_part,
                         f"  (name={func_name})" if func_name else "")
            return

        if "--func" in raw and "@terminal" in raw:
            # Syntax: !connect --func @terminal <alias> <cmd...>
            # Example: !connect --func @terminal status !status --verbose
            import re
            m = re.search(r'--func\s+@terminal\s+(\S+)\s+(.+)', raw, re.IGNORECASE)
            if not m:
                log.warning('!connect --func @terminal: syntax: !connect --func @terminal <alias> <command>')
                return
            alias = m.group(1).strip()
            cmd_str = m.group(2).strip()
            self._term_funcs.bind(alias, cmd_str)
            # Push updated func table to terminal via pipe
            self._push_terminal_funcs()
            if not silent:
                log.info("!connect --func @terminal '%s' → %s", alias, cmd_str)
            return

        log.warning("!connect: use --window / --rule / --function / --func @terminal")

    # --- !disconnect ----------------------------------------------------------

    def _cmd_disconnect(self, raw: str):
        """
        !disconnect --window  #name="x"      [-s][-q][--all][--toggle][-f]
        !disconnect --rule    #name="x"      [-s][-q][--all][--toggle]
        !disconnect --ae      #type=... #name="x"
        !disconnect --function <cmd> to|; <key>
        !disconnect           #name="func"   (by name set in !connect --function)
        """
        import re
        tokens = raw.split()
        silent  = "-s" in tokens or "--silent" in tokens
        all_obj = "--all" in tokens
        toggle  = "--toggle" in tokens
        force   = "-f" in tokens or "--force" in tokens

        if "--window" in raw:
            if all_obj:
                for name in self._win_reg.all_names():
                    self._cmd_disconnect(f'!disconnect --window #name="{name}" -s')
                if not silent: log.info("!disconnect --window --all: all windows disconnected")
                return
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!disconnect --window: #name not specified"); return
            entry = self._win_reg.get(name)
            if entry is None:
                log.warning("!disconnect --window '%s': not found", name); return
            if toggle:
                new_state = not entry.get("enabled", True)
                entry["enabled"] = new_state
                self._win_reg.add(name, entry.get("kind", "window"), entry)
                if not silent: log.info("!disconnect --window '%s' toggle → %s", name, new_state)
                return
            if not force and not entry.get("enabled", True):
                if not silent: log.info("!disconnect --window '%s': already disabled", name)
                return
            entry["enabled"] = False
            self._win_reg.add(name, entry.get("kind", "window"), entry)
            if not silent: log.info("!disconnect --window '%s' → disabled", name)
            return

        if "--rule" in raw:
            if all_obj:
                for name in self._rule_reg.all_names():
                    self._rule_reg.set_connected(name, False)
                if not silent: log.info("!disconnect --rule --all: all rules disconnected")
                return
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!disconnect --rule: #name not specified"); return
            if toggle:
                entry = self._rule_reg.get(name)
                if entry is None:
                    log.warning("!disconnect --rule '%s': not found", name); return
                new_state = not entry.get("connected", True)
                self._rule_reg.set_connected(name, new_state)
                if not silent: log.info("!disconnect --rule '%s' toggle → %s", name, new_state)
                return
            if not self._rule_reg.set_connected(name, False):
                log.warning("!disconnect --rule '%s': not found", name)
            elif not silent:
                log.info("!disconnect --rule '%s' → disconnected", name)
            return

        if "--ae" in raw:
            ae_type = _parse_named(raw, "type")
            name    = _parse_named(raw, "name")
            if not name:
                log.warning("!disconnect --ae: #name not specified"); return
            if self._ae_reg.remove(name):
                if not silent:
                    log.info("!disconnect --ae [%s] '%s' → removed", ae_type, name)
            else:
                log.warning("!disconnect --ae '%s': not found", name)
            return

        if "--function" in raw:
            m = re.search(r'--function\s+(.+?)\s+(?:to|;)\s+(\S+)', raw, re.IGNORECASE)
            if not m:
                log.warning("!disconnect --function: syntax: --function <cmd> to|; <key>")
                return
            key_part = m.group(2).strip()
            self._hotkeys.unbind(key_part)
            if not silent:
                log.info("!disconnect --function '%s' → unbound", key_part)
            return

        # Disconnect by name (#name="func") set in !connect --function
        name = _parse_named(raw, "name")
        if name:
            unbound = self._hotkeys.unbind_by_alias(name)
            if unbound:
                if not silent:
                    log.info("!disconnect #name='%s' → unbound (%s)", name, unbound)
            else:
                log.warning("!disconnect #name='%s': not found", name)
            return

        if "--func" in raw and "@terminal" in raw:
            import re
            m = re.search(r'--func\s+@terminal\s+(\S+)', raw, re.IGNORECASE)
            if not m:
                log.warning('!disconnect --func @terminal: syntax: !disconnect --func @terminal <alias>')
                return
            alias = m.group(1).strip()
            if self._term_funcs.unbind(alias):
                self._push_terminal_funcs()
                if not silent: log.info("!disconnect --func @terminal '%s': unbound", alias)
            else:
                log.warning("!disconnect --func @terminal '%s': not found", alias)
            return

        log.warning("!disconnect: use --window / --rule / --ae / --function / --func @terminal  or #name=")


    # --- !edit --terminal ---------------------------------------------------------

    def _cmd_edit_terminal(self, raw: str):
        """
        !edit --terminal [#name="title"] [#bg=color] [#fg=color] [#size=(WxH)]
        Use # instead of a value to skip a parameter.

        Examples:
          !edit --terminal #name="MyConsole" #size=(1200x700)
          !edit --terminal ##bg=# #size=(800x600)   <- bg skipped
        """
        import re
        tokens = raw.split()
        silent = "-s" in tokens or "--silent" in tokens

        def _parg(key: str) -> str | None:
            m = re.search(r'#' + re.escape(key) + r'=(?:"([^"]*)"|(\S+))', raw)
            if not m: return None
            val = m.group(1) if m.group(1) is not None else m.group(2)
            return None if val == "#" else val  # '#' = skip

        name_val = _parg("name")
        size_val = _parg("size")
        # bg/fg are noted but not applied live (C++ handles them)
        bg_val   = _parg("bg")
        fg_val   = _parg("fg")

        if name_val is not None:
            if not silent:
                self._con_ok(f"Terminal title → {name_val}")
            # Title is actually changed by C++ side via SetWindowTextW
            # If console is unavailable — just log
            log.info("!edit --terminal #name=%s", name_val)

        if size_val is not None:
            # Parse WxH
            m = re.match(r'(\d+)[xX](\d+)', size_val.strip("()"))
            if m:
                groups = [g for g in m.groups() if g is not None]
                if len(groups) >= 2:
                    nw, nh = int(groups[0]), int(groups[1])
                    if not silent:
                        self._con_ok(f"Terminal size → {nw}x{nh}")
                    log.info("!edit --terminal #size=(%dx%d)", nw, nh)
                else:
                    self._con_warn("!edit --terminal: invalid #size format, expected WxH")
            else:
                self._con_warn("!edit --terminal: invalid #size format, expected WxH")

        if bg_val is not None and not silent:
            self._con_info(f"#bg={bg_val} — will apply on next console open")
        if fg_val is not None and not silent:
            self._con_info(f"#fg={fg_val} — will apply on next console open")

        if all(v is None for v in (name_val, size_val, bg_val, fg_val)):
            self._con_warn("!edit --terminal: no parameters given (use #name=, #size=, #bg=, #fg= or # to skip)")

    # --- Helper methods -------------------------------------------------------

    def _root_alive(self) -> bool:
        """Check whether the Tkinter root window still exists."""
        try:
            return bool(self.app.root.winfo_exists())
        except Exception:
            return False

    def _safe_destroy(self):
        try:
            self.app.root.destroy()
        except Exception:
            pass

    def _open_tk_window(self, name: str):
        """Open a Tkinter window with the given name and style from registry."""
        if not self._root_alive():
            log.warning("_open_tk_window: root unavailable")
            return

        entry = self._win_reg.get(name) or {}
        bg_color = entry.get("bg", "#1e1e2e")
        fg_color = entry.get("fg", "#cdd6f4")
        width    = entry.get("width", 600)
        height   = entry.get("height", 400)

        def _create():
            import tkinter as tk
            top = tk.Toplevel(self.app.root)
            top.title(name)
            top.geometry(f"{width}x{height}")
            top.configure(bg=bg_color)
            tk.Label(top, text=name, bg=bg_color, fg=fg_color,
                     font=("Segoe UI", 14)).pack(pady=20)
            log.info("Tkinter window opened: %s", name)

        self.app.root.after(0, _create)

    def _open_notepad_file(self, path: str):
        """Open a file in the system editor."""
        try:
            if platform.system() == "Windows":
                os.startfile(path)
            elif platform.system() == "Darwin":
                subprocess.Popen(["open", "-a", "TextEdit", path])
            else:
                subprocess.Popen(["xdg-open", path])
            log.info("File opened: %s", path)
        except Exception as e:
            log.warning("_open_notepad_file error: %s", e)

    def _toggle_desktop_shortcut(self, enable: bool):
        """Create or delete a desktop shortcut (Windows)."""
        if platform.system() != "Windows":
            log.warning("Shortcuts are only supported on Windows")
            return

        desktop  = Path(os.path.expanduser("~")) / "Desktop"
        lnk_path = desktop / "HomRec.lnk"
        target   = (sys.executable if getattr(sys, "frozen", False)
                    else os.path.abspath(__file__).replace("hr_console_bridge.py", "homrec.py"))

        if enable:
            try:
                ps = (
                    f"$s=(New-Object -COM WScript.Shell).CreateShortcut('{lnk_path}');"
                    f"$s.TargetPath='{target}';"
                    f"$s.WorkingDirectory='{os.path.dirname(target)}';"
                    f"$s.Save()"
                )
                _run_no_window(["powershell", "-Command", ps], capture_output=True)
                log.info("Shortcut created: %s", lnk_path)
            except Exception as e:
                log.error("Failed to create shortcut: %s", e)
        else:
            try:
                if lnk_path.exists():
                    lnk_path.unlink()
                    log.info("Shortcut deleted: %s", lnk_path)
            except Exception as e:
                log.error("Failed to delete shortcut: %s", e)