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
CB_COMMAND = ctypes.CFUNCTYPE(None, ctypes.c_wchar_p)   # новый: произвольная команда из DLL

CONSOLE_VERSION = "1.2.3"
BRIDGE_VERSION  = "1.2.3"

# HomRec application version constants
HOMREC_VERSION = "1.6.4"
CORE_VERSION   = "1.4.3"

# --------------------------------------------------------------------------------
#  Вспомогательные утилиты разбора аргументов
# --------------------------------------------------------------------------------

def _parse_named(raw: str, key: str) -> str | None:
    """Извлечь значение #key="value" или #key=value из строки raw."""
    import re
    m = re.search(r'#' + re.escape(key) + r'=["\']?([^"\'#\s]+)["\']?', raw)
    if not m:
        # попробовать с кавычками
        m = re.search(r'#' + re.escape(key) + r'="([^"]*)"', raw)
    if not m:
        m = re.search(r'#' + re.escape(key) + r"='([^']*)'", raw)
    val = m.group(1) if m else None
    return _resolve_math(val) if val is not None else None


def _parse_flags(raw: str) -> set[str]:
    """Собрать все -флаги из строки (токены начинающиеся с -), кроме -return/-ret."""
    import re
    flags = set(re.findall(r'(?<!\S)-[a-zA-Z]+', raw))
    flags.discard('-return')
    flags.discard('-ret')
    return flags


def _get_base_dir() -> str:
    """Return the project root directory.

    When frozen (PyInstaller .exe): the folder containing the .exe.
    When running as .py from src/: the parent of src/ — i.e. the project root
    where create/, assets/, hr_terminal.exe, etc. all live.
    Falls back to the script's own directory if the src/ layout isn't detected.
    """
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    _src = os.path.dirname(os.path.abspath(__file__))
    _parent = os.path.dirname(_src)
    if os.path.isdir(os.path.join(_parent, "src")) or os.path.basename(_src).lower() == "src":
        return _parent
    return _src  # flat layout fallback


# --------------------------------------------------------------------------------
#  Менеджер созданных окон (хранилище)
# --------------------------------------------------------------------------------

class WindowRegistry:
    """
    Хранит список «виртуальных» окон созданных через !create --window.
    Данные сохраняются в <base>/create/windows.json
    """
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
    """Заменяет {int.random(a, b)} на случайное целое."""
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
    """
    Хранит правила из !create --rule.
    Файл: <base>/create/rules.json
    Формат записи: {"body": str, "connected": bool}
    Тело правила — строки, разделённые ; (каждая — команда консоли).
    """
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
    Хранит "Anything Else" объекты (цвета, и т.д.).
    Файл: <base>/create/ae.json
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


# --- AliasRegistry ------------------------------------------------------------

class AliasRegistry:
    """
    Хранит псевдонимы команд, созданные через !alias.
    Файл: <base>/create/aliases.json
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
    """Хранит внутренние переменные окружения консоли ($name)."""
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
        """Заменяет $name на значение переменной в строке."""
        import re
        def _rep(m):
            return self._vars.get(m.group(1), m.group(0))
        return re.sub(r'\$([A-Za-z_]\w*)', _rep, s)


# --------------------------------------------------------------------------------

class HotkeyManager:
    """
    Регистрирует глобальные горячие клавиши через keyboard (если доступен)
    или через Tkinter bind как fallback.
    Хранит привязки в <base>/create/hotkeys.json
    """
    def __init__(self, base_dir: str, console: "NativeConsole"):
        self._path = Path(base_dir) / "create" / "hotkeys.json"
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._console = console
        self._bindings: dict[str, str] = {}  # key → command
        self._load()

        # Попытка использовать библиотеку keyboard
        try:
            import keyboard as _kb
            self._kb = _kb
        except ImportError:
            self._kb = None
            log.warning("'keyboard' package not found; hotkeys fallback to Tkinter bind")

        # Восстановить сохранённые привязки
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
            # Tkinter bind (работает только когда фокус на главном окне)
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
        self._lib = None  # BUG FIX: инициализировать до _load()

        base = _get_base_dir()
        self._win_reg  = WindowRegistry(base)
        self._rule_reg = RuleRegistry(base)
        self._ae_reg   = AERegistry(base)
        self._alias_reg = AliasRegistry(base)
        self._env      = EnvStore()
        self._hotkeys  = HotkeyManager(base, self)

        # История команд консоли (для !history)
        self._history: list[str] = []
        # Таймеры и watcher-ы
        self._timers: dict[str, threading.Timer] = {}
        self._watchers: dict[str, dict] = {}   # name → {thread, stop_event, ms, cmd, runs, max_runs}

        self._lib = self._load()
        if not self._lib:
            return

        log_path = os.path.join(base, "homrec.log")  # base is already root dir

        # Держим ссылки чтобы GC не удалил (BUG FIX: хранить как атрибуты экземпляра)
        self._cb_start    = CB_VOID(self._start)
        self._cb_stop     = CB_VOID(self._stop)
        self._cb_quit     = CB_VOID(self._quit)
        self._cb_open_log = CB_VOID(self._open_log)
        self._cb_open_url = CB_URL(self._open_url)
        self._cb_command  = CB_COMMAND(self._on_command)  # новый колбэк для расширенных команд

        self._lib.hr_con_init(
            self._cb_start, self._cb_stop, self._cb_quit,
            self._cb_open_log, self._cb_open_url,
            log_path, self.GITHUB,
        )

        # Регистрируем расширенный колбэк команд (если DLL поддерживает)
        if hasattr(self._lib, 'hr_con_set_command_cb'):
            self._lib.hr_con_set_command_cb.argtypes = [CB_COMMAND]
            self._lib.hr_con_set_command_cb.restype = None
            self._lib.hr_con_set_command_cb(self._cb_command)

        # Запустить pipe-сервер для hr_terminal.exe
        if hasattr(self._lib, 'hr_pipe_server_start'):
            self._lib.hr_pipe_server_start.argtypes = []
            self._lib.hr_pipe_server_start.restype  = None
            self._lib.hr_pipe_server_start()
            log.info("Pipe server started for external terminals")

        # Фильтр для !disconnect --log
        # BUG FIX: проверять self._lib != None перед вызовом
        class LogFilter(logging.Filter):
            def __init__(self, lib):
                super().__init__()
                self._lib = lib

            def filter(self, r):
                # BUG FIX: защита от случая когда _lib ещё None
                try:
                    return bool(self._lib and self._lib.hr_con_log_connected())
                except Exception:
                    return True  # при ошибке — не блокировать лог

        for h in logging.getLogger().handlers:
            if isinstance(h, logging.FileHandler):
                h.addFilter(LogFilter(self._lib))

        log.info("hr_console.dll OK")

    def _load(self):
        dll_dir  = os.path.dirname(os.path.abspath(__file__))
        dll_path = os.path.join(dll_dir, "hr_console.dll")

        if not os.path.exists(dll_path):
            log.warning("hr_console.dll not found at %s", dll_path)
            return None  # BUG FIX: явный None

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
            return None  # BUG FIX: явный None

    # -- Публичный API ------------------------------------------------------------

    def toggle(self):
        # BUG FIX: проверка self._lib перед любым обращением
        if not self._lib:
            return
        is_rec = 1 if getattr(self.app, "recording", False) else 0
        self._lib.hr_con_set_recording(is_rec)
        self._lib.hr_con_toggle()

    def run_command(self, cmd: str):
        """Выполнить расширенную команду (вызывается из горячих клавиш или DLL)."""
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
        # BUG FIX: проверять что root существует
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
        # BUG FIX: проверять что root жив до after()
        if self._root_alive():
            self.app.root.after(150, lambda: (self._safe_destroy(), sys.exit(0)))
        else:
            sys.exit(0)

    def _on_command(self, cmd: str):
        """Колбэк для расширенных команд от DLL (новый)."""
        log.info("Console command: %s", cmd)
        if self._root_alive():
            self.app.root.after(0, lambda: self.run_command(cmd))

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

    # -- Расширенный диспетчер команд ---------------------------------------------

    def _dispatch_extended(self, raw: str):
        """
        Роутер расширенных команд.
        Вызывается из NativeConsole.on_command() когда команда пришла из DLL.
        Флаги -return/-ret уже удалены из raw на стороне C++ DLL до пересылки.
        """
        import re
        # Дополнительная защита: стрипаем -return/-ret если они вдруг остались
        raw = re.sub(r'\s+-ret(?:urn)?\b', '', raw).strip()
        raw = raw.strip()

        # Нормализация: @all → --all
        raw = raw.replace("@all", "--all")

        # Подстановка переменных окружения ($name)
        raw = self._env.resolve(raw)

        # Подстановка математики
        raw = _resolve_math(raw)

        cmd = raw.split()[0] if raw.split() else ""

        # Проверить псевдоним
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

        # -- Новые команды ------------------------------------------------------
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
        if cmd == "!version":
            self._cmd_version(raw); return
        if cmd == "!homrec":
            self._cmd_homrec(raw); return
        if cmd == "!log":
            self._cmd_log(raw); return

        log.warning("_dispatch_extended: неизвестная команда '%s'", cmd)

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
            log.warning("!start --rec: укажи 1 (старт) или 0 (стоп)")
            return
        silent = "-s" in tokens or "--silent" in tokens
        if val == 1:
            if not getattr(self.app, "recording", False):
                if self._root_alive():
                    self.app.root.after(0, self.app.start_recording)
                if self._lib:
                    self._lib.hr_con_set_recording(1)
                if not silent:
                    log.info("!start --rec 1: запись начата")
            else:
                if not silent:
                    log.info("!start --rec 1: уже идёт запись")
        else:
            if getattr(self.app, "recording", False):
                if self._root_alive():
                    self.app.root.after(0, self.app.stop_recording)
                if self._lib:
                    self._lib.hr_con_set_recording(0)
                if not silent:
                    log.info("!start --rec 0: запись остановлена")
            else:
                if not silent:
                    log.info("!start --rec 0: запись не была активна")

    # --- !rule ----------------------------------------------------------------

    def _cmd_rule(self, raw: str):
        """
        !rule --check #name="example"
            → показывает: активно/не активно + тело
        !rule --get from connect #name="example"
            → читает правило из реестра (источник — !connect --rule)
        """
        tokens = raw.split()
        silent = "-s" in tokens
        name = _parse_named(raw, "name")
        if not name:
            log.warning("!rule: не указан #name")
            return

        if "--check" in raw:
            entry = self._rule_reg.get(name)
            if entry is None:
                log.warning("!rule --check '%s': не найдено", name)
                return
            status = "✔ активно" if entry.get("connected", True) else "✘ отключено"
            body   = entry.get("body", "(пусто)")
            log.info("!rule --check '%s':  %s", name, status)
            log.info("  тело: %s", body)
            return

        if "--get" in raw and "from" in raw and "connect" in raw:
            entry = self._rule_reg.get(name)
            if entry is None:
                log.warning("!rule --get '%s': не найдено в реестре", name)
                return
            log.info("!rule --get '%s': connected=%s  body=%s",
                     name, entry.get("connected"), entry.get("body", ""))
            return

        log.warning("!rule: используй --check или --get from connect")

    # --- !edit ----------------------------------------------------------------

    def _cmd_edit(self, raw: str):
        """
        !edit --file    #name="x"           → открыть файл notepad на редактирование
        !edit --window  #name="x"           → переоткрыть окно
        !edit --rule    #name="x"; <step>; <step>  → заменить тело правила
        !edit --settings #name=shortcut 1|0 → ярлык на рабочем столе
        """
        import re
        tokens = raw.split()
        silent = "-s" in tokens

        if "--file" in raw:
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!edit --file: не указан #name"); return
            entry = self._win_reg.get(name)
            if not entry:
                log.warning("!edit --file '%s': не найдено в реестре", name); return
            if entry.get("kind") != "notepad":
                log.warning("!edit --file '%s': это не notepad (kind=%s)", name, entry.get("kind")); return
            fp = entry.get("file", "")
            if not fp:
                log.warning("!edit --file '%s': путь к файлу не сохранён", name); return
            self._open_notepad_file(fp)
            if not silent:
                log.info("!edit --file '%s': открыт %s", name, fp)
            return

        if "--window" in raw:
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!edit --window: не указан #name"); return
            self._cmd_start_window(f'!start --window #name="{name}"')
            if not silent:
                log.info("!edit --window '%s': открыто", name)
            return

        if "--rule" in raw:
            # !edit --rule #name="x"; step1; step2; step3
            m = re.search(r'#name=["\']?([^"\';\s]+)["\']?\s*;\s*(.+)', raw, re.DOTALL)
            if not m:
                # Показать текущее тело, если нет новых шагов
                name = _parse_named(raw, "name")
                if name:
                    entry = self._rule_reg.get(name)
                    if entry:
                        log.info("!edit --rule '%s' (текущее тело):\n  %s",
                                 name, entry.get("body", "(пусто)"))
                    else:
                        log.warning("!edit --rule '%s': не найдено", name)
                else:
                    log.warning("!edit --rule: синтаксис: !edit --rule #name=\"x\"; шаг1; шаг2")
                return
            name     = _resolve_math(m.group(1).strip())
            new_body = m.group(2).strip()
            if not self._rule_reg.exists(name):
                log.warning("!edit --rule '%s': не найдено. Сначала !create --rule", name); return
            entry = self._rule_reg.get(name)
            self._rule_reg.add(name, new_body, entry.get("connected", True))
            if not silent:
                log.info("!edit --rule '%s': тело обновлено → %s", name, new_body)
            return

        if "--settings" in raw:
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!edit --settings: не указан #name"); return
            toks = raw.split()
            vals = [t for t in toks if t in ("0","1","true","false","on","off","yes","no")]
            enable = vals[-1] in ("1","true","on","yes") if vals else True
            if name == "shortcut":
                self._toggle_desktop_shortcut(enable)
                if not silent:
                    log.info("!edit --settings shortcut → %s", "вкл" if enable else "выкл")
            else:
                log.warning("!edit --settings: неизвестный параметр '%s'", name)
            return

        log.warning("!edit: используй --file / --window / --rule / --settings")

    # --- !create --------------------------------------------------------------

    def _cmd_create(self, raw: str):
        """
        !create --window #name="x" [#bg=COLOR] [#fg=COLOR] [#size=(WxH)]
                         [--notepad [as .EXT]]
                         [-o] [-s] [-n] [-c] [-d]

        !create --rule #name="x"; step1; step2; step3  [-c] [-d]
            Шаги — команды консоли через ';'.
            При -c: немедленно выполняется (connect).
            При -d: сохраняется как disconnected.
            Пример: !create --rule #name="auto"; !start --rec 1 then; $rm #name="notepad2"

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
            log.warning("!create: не указан #name"); return

        base = _get_base_dir()

        # -- --rule ------------------------------------------------------------
        if is_rule:
            # body = всё после первой ';' после #name=...
            m = re.search(r'#name=["\']?[^"\';\s]+["\']?\s*;\s*(.+)', raw, re.DOTALL)
            body = m.group(1).strip() if m else ""
            connected = not disconnected

            # Новые флаги для правил
            once    = "--once"  in raw
            delay_s = _parse_named(raw, "ms") if "--delay" in raw else None
            on_fail = _parse_named(raw, "cmd") if "--on-fail" in raw else None
            loop_s  = _parse_named(raw, "count") if "--loop" in raw else None
            extra_rule: dict = {"once": once}
            if delay_s: extra_rule["step_delay_ms"] = int(delay_s)
            if on_fail: extra_rule["on_fail"] = on_fail
            if loop_s:  extra_rule["loop"] = int(loop_s)

            self._rule_reg.add(name, body, connected)
            # Сохранить дополнительные мета-данные
            if extra_rule:
                entry = self._rule_reg.get(name) or {}
                entry.update(extra_rule)
                self._rule_reg.add(name, body, connected)

            if not silent:
                log.info("!create --rule '%s': сохранено  connected=%s  body: %s%s",
                         name, connected, body or "(пусто)",
                         f"  once={once}" if once else "")
            # -c: выполнить шаги прямо сейчас
            if auto_connect and body:
                loop_n = int(loop_s) if loop_s else 1
                for _ in range(loop_n if loop_n > 0 else 1):
                    self._run_rule_body(name, body, silent)
            return

        # -- --ae --------------------------------------------------------------
        if is_ae:
            type_raw = _parse_named(raw, "type")
            if not type_raw:
                log.warning("!create --ae: не указан #type"); return
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
                    log.warning("!create --ae color: нужен rgb=(...) или hex=(...)"); return
            else:
                log.warning("!create --ae: неизвестный #type='%s'", ae_type); return

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

        # Новые флаги для окон (v3.0)
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
        """Текущее состояние системы одним блоком."""
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
            log.info("запись  : %s", "вкл" if rec else "выкл")
            log.info("лог     : %s", "подключён" if log_conn else "нет")
            log.info("окна    : %d активных / %d всего", wins_on, len(self._win_reg.all_names()))
            log.info("правила : %d подключённых / %d всего", rules_on, len(self._rule_reg.all_names()))
            log.info("хоткеи  : %d привязок", hk_count)

    # --- !info ----------------------------------------------------------------

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
            if not name: log.warning("!info --window: не указан #name"); return
            e = self._win_reg.get(name)
            if e is None: log.warning("!info --window '%s': не найдено", name); return
            _show({"name": name, **e}); return

        if "--rule" in raw:
            name = _parse_named(raw, "name")
            if not name: log.warning("!info --rule: не указан #name"); return
            e = self._rule_reg.get(name)
            if e is None: log.warning("!info --rule '%s': не найдено", name); return
            steps = [s.strip() for s in e.get("body","").split(";") if s.strip()]
            _show({"name": name, "connected": e.get("connected", True),
                   "steps": len(steps), "body": e.get("body","")}); return

        if "--ae" in raw:
            name = _parse_named(raw, "name")
            if not name: log.warning("!info --ae: не указан #name"); return
            e = self._ae_reg.get(name)
            if e is None: log.warning("!info --ae '%s': не найдено", name); return
            _show({"name": name, **e}); return

        if "--hotkey" in raw:
            key = _parse_named(raw, "key")
            if not key: log.warning("!info --hotkey: не указан #key"); return
            b = self._hotkeys.all_bindings().get(key)
            if b is None: log.warning("!info --hotkey '%s': не найдено", key); return
            _show({"key": key, **(b if isinstance(b, dict) else {"cmd": b})}); return

        log.warning("!info: используй --window / --rule / --ae / --hotkey")

    # --- !history -------------------------------------------------------------

    def _cmd_history(self, raw: str):
        """!history [#count=N] [--clear] [--search "text"]"""
        tokens = raw.split()
        if "--clear" in tokens:
            self._history.clear()
            log.info("история очищена"); return

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
                log.info("псевдонимов нет"); return
            for n, c in aliases.items():
                log.info("  %-16s → %s", n, c)
            return
        if "--remove" in tokens:
            name = _parse_named(raw, "name")
            if not name: log.warning("!alias --remove: не указан #name"); return
            if self._alias_reg.remove(name):
                log.info("!alias: удалён '%s'", name)
            else:
                log.warning("!alias --remove: '%s' не найден", name)
            return
        name = _parse_named(raw, "name")
        cmd  = _parse_named(raw, "cmd")
        if not name or not cmd:
            log.warning("!alias: нужен #name=... и #cmd=..."); return
        self._alias_reg.add(name, cmd)
        log.info("!alias: '%s' → %s", name, cmd)

    # --- !repeat --------------------------------------------------------------

    def _cmd_repeat(self, raw: str):
        """!repeat #count=N <command>"""
        import re
        m = re.match(r'!repeat\s+#count=(\d+)\s+(.+)', raw, re.IGNORECASE)
        if not m:
            log.warning("!repeat: синтаксис: !repeat #count=N <команда>"); return
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
            log.warning("!delay: синтаксис: !delay #ms=N <команда>"); return
        ms  = int(m.group(1))
        cmd = m.group(2).strip()
        log.info("!delay: через %dms → %s", ms, cmd)
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
            log.warning("!batch: нет команд"); return
        for part in parts:
            log.info("!batch → %s", part)
            try:
                self._dispatch_extended(part)
            except Exception as e:
                log.error("!batch ошибка: %s", e)
                if stop_on_error:
                    log.warning("!batch: остановка при ошибке (-x)"); return

    # --- !run -----------------------------------------------------------------

    def _cmd_run(self, raw: str):
        """!run #file="script.hrc" [--encoding utf8|cp1251] [--ignore-errors] [--echo-each] [-x]"""
        file_path = _parse_named(raw, "file")
        if not file_path:
            log.warning("!run: не указан #file"); return
        encoding      = _parse_named(raw, "encoding") or "utf-8"
        ignore_errors = "--ignore-errors" in raw
        echo_each     = "--echo-each"     in raw
        stop_on_error = "-x" in raw.split() or "--stop-on-error" in raw

        p = Path(file_path)
        if not p.is_absolute():
            p = Path(_get_base_dir()) / p
        if not p.exists():
            log.warning("!run: файл не найден: %s", p); return

        try:
            text = p.read_text(encoding=encoding)
        except Exception as e:
            log.error("!run: ошибка чтения файла: %s", e); return

        lines = [l.strip() for l in text.splitlines()]
        for i, line in enumerate(lines, 1):
            if not line or line.startswith("#"):
                continue
            if echo_each:
                log.info("!run [%d]: %s", i, line)
            try:
                self._dispatch_extended(line)
            except Exception as e:
                log.error("!run [%d] ошибка '%s': %s", i, line, e)
                if stop_on_error and not ignore_errors:
                    log.warning("!run: остановка (-x)"); return

    # --- !clear ---------------------------------------------------------------

    def _cmd_clear(self, raw: str):
        """!clear — очистить вывод консоли."""
        # Специальный маркер, который можно перехватить на стороне UI
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
                log.info("!clip: буфер очищен")
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
                log.info("!clip: скопировано: %s", text)
            except Exception as e:
                log.warning("!clip --copy: %s", e)
            return
        log.warning("!clip: используй --copy \"текст\" / --paste / --clear")

    # --- !env -----------------------------------------------------------------

    def _cmd_env(self, raw: str):
        """!env --set #name="x" #val="y" | --get #name | --list | --unset #name"""
        tokens = raw.split()
        if "--list" in tokens:
            vs = self._env.all()
            if not vs: log.info("переменных нет"); return
            for k, v in vs.items():
                log.info("  $%-20s = %s", k, v)
            return
        if "--unset" in tokens:
            name = _parse_named(raw, "name")
            if not name: log.warning("!env --unset: не указан #name"); return
            if self._env.unset(name): log.info("!env: $%s удалена", name)
            else: log.warning("!env: $%s не найдена", name)
            return
        if "--get" in tokens:
            name = _parse_named(raw, "name")
            if not name: log.warning("!env --get: не указан #name"); return
            val = self._env.get(name)
            if val is None: log.warning("!env: $%s не установлена", name)
            else: log.info("$%s = %s", name, val)
            return
        if "--set" in tokens:
            name = _parse_named(raw, "name")
            val  = _parse_named(raw, "val")
            if not name: log.warning("!env --set: не указан #name"); return
            self._env.set(name, val or "")
            log.info("!env: $%s = %s", name, val)
            return
        log.warning("!env: используй --set / --get / --list / --unset")

    # --- !timer ---------------------------------------------------------------

    def _cmd_timer(self, raw: str):
        """!timer #name="x" #ms=N <cmd> | --cancel #name | --list"""
        tokens = raw.split()
        if "--list" in tokens:
            if not self._timers:
                log.info("активных таймеров нет"); return
            for n in list(self._timers.keys()):
                log.info("  %-20s (активен)", n)
            return
        if "--cancel" in tokens:
            name = _parse_named(raw, "name")
            if not name: log.warning("!timer --cancel: не указан #name"); return
            t = self._timers.pop(name, None)
            if t: t.cancel(); log.info("!timer '%s': отменён", name)
            else: log.warning("!timer '%s': не найден", name)
            return

        name  = _parse_named(raw, "name")
        ms_s  = _parse_named(raw, "ms")
        if not name or not ms_s:
            log.warning("!timer: нужен #name и #ms"); return
        import re
        # Команда — всё после последнего именованного параметра
        cmd_m = re.search(r'#ms=\d+\s+(.+)', raw)
        if not cmd_m:
            log.warning("!timer: не найдена команда после #ms=N"); return
        cmd = cmd_m.group(1).strip()
        ms  = int(ms_s)

        def _fire(n=name, c=cmd):
            self._timers.pop(n, None)
            log.info("!timer '%s' fired → %s", n, c)
            self._dispatch_extended(c)

        t = threading.Timer(ms / 1000.0, _fire)
        t.daemon = True; t.start()
        self._timers[name] = t
        log.info("!timer '%s': через %dms → %s", name, ms, cmd)

    # --- !watch ---------------------------------------------------------------

    def _cmd_watch(self, raw: str):
        """!watch #name="x" #ms=N <cmd> [--max-runs #count=N] | --stop #name | --list"""
        tokens = raw.split()
        if "--list" in tokens:
            if not self._watchers:
                log.info("активных watch нет"); return
            for n, w in self._watchers.items():
                log.info("  %-20s  ms=%d  cmd=%s  runs=%d", n, w["ms"], w["cmd"], w["runs"])
            return
        if "--stop" in tokens:
            name = _parse_named(raw, "name")
            if not name: log.warning("!watch --stop: не указан #name"); return
            w = self._watchers.pop(name, None)
            if w: w["stop_event"].set(); log.info("!watch '%s': остановлен", name)
            else: log.warning("!watch '%s': не найден", name)
            return

        name  = _parse_named(raw, "name")
        ms_s  = _parse_named(raw, "ms")
        if not name or not ms_s:
            log.warning("!watch: нужен #name и #ms"); return

        import re
        # Команда — всё между #ms=N и первым флагом --
        cmd_m = re.search(r'#ms=\d+\s+(.*?)(?:\s+--|$)', raw)
        if not cmd_m:
            cmd_m = re.search(r'#ms=\d+\s+(.+)', raw)
        if not cmd_m:
            log.warning("!watch: не найдена команда после #ms=N"); return
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
                    log.error("!watch '%s' ошибка: %s", n, e)
                if max_runs and i["runs"] >= max_runs:
                    log.info("!watch '%s': max-runs=%d достигнут, остановлен", n, max_runs)
                    self._watchers.pop(n, None)
                    break

        t = threading.Thread(target=_loop, daemon=True)
        t.start()
        log.info("!watch '%s': каждые %dms%s → %s%s",
                 name, ms, f"±{jitter_ms}ms" if jitter_ms else "",
                 cmd, f"  (max {max_runs})" if max_runs else "")

    # --- Console output helper ------------------------------------------------

    def _con_write(self, text: str, tag: int = 0):
        """Вывести строку напрямую в окно DLL-консоли (тег: 0=text 1=ok 2=warn 3=err 4=dim 5=accent)."""
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
                log.info("!log: homrec.log очищен")
            except Exception as e:
                log.warning("!log --clear: %s", e)
            return

        if not log_path.exists():
            log.warning("!log: homrec.log не найден"); return
        try:
            lines = log_path.read_text("utf-8", errors="replace").splitlines()
        except Exception as e:
            log.warning("!log: ошибка чтения: %s", e); return

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
        Выполняет шаги тела правила.
        Шаги разделены ';'.  Ключевые слова:
          then  — просто разделитель (игнорируется)
          !start --rec 1 then   → [!start --rec 1]
        """
        import re
        steps = [s.strip() for s in body.split(";") if s.strip()]
        for step in steps:
            # Убрать trailing 'then'
            step = re.sub(r'\s+then\s*$', '', step, flags=re.IGNORECASE).strip()
            if not step:
                continue
            if not silent:
                log.info("  rule '%s' → %s", rule_name, step)
            self._dispatch_extended(step)

    # --- !start --terminal as @terminal ------------------------------------

    def _cmd_start_terminal(self, raw: str):
        """!start --terminal as @terminal  — запустить hr_terminal.exe."""
        tokens = raw.split()
        silent = "-s" in tokens or "--silent" in tokens
        if "as" not in tokens or "@terminal" not in tokens:
            log.warning("!start --terminal: синтаксис: !start --terminal as @terminal")
            return
        base = _get_base_dir()
        exe  = os.path.join(base, "hr_terminal.exe")
        if not os.path.exists(exe):
            log.warning("!start --terminal: hr_terminal.exe не найден в %s", base)
            return
        try:
            subprocess.Popen([exe], cwd=base)
            if not silent:
                log.info("!start --terminal: hr_terminal.exe запущен")
        except Exception as e:
            log.error("!start --terminal: ошибка запуска: %s", e)

    # --- !start --window ------------------------------------------------------

    def _cmd_start_window(self, raw: str):
        """!start --window #name="x"  — переоткрыть созданное окно."""
        if "--window" not in raw:
            log.warning("!start: используй --window или --rec"); return
        name = _parse_named(raw, "name")
        if not name:
            log.warning("!start --window: не указан #name"); return
        entry = self._win_reg.get(name)
        if entry is None:
            log.warning("!start --window '%s': не найдено в реестре", name); return
        if entry.get("kind") == "notepad":
            fp = entry.get("file", "")
            if fp:
                self._open_notepad_file(fp)
            else:
                log.warning("!start: нет пути к файлу для '%s'", name)
        else:
            self._open_tk_window(name)

    # --- !rename --------------------------------------------------------------

    def _cmd_rename(self, raw: str):
        """
        !rename --window  #name="old_name" to #name="new_name"
        !rename --rule    #name="old_name" to #name="new_name"
        !rename --ae      #name="old_name" to #name="new_name"
        !rename --hotkey  #name="old_name" to #name="new_name"
        !rename --window  @all #prefix="pfx_"          (добавить префикс ко всем)
        !rename --window  @all #suffix="_v2"           (добавить суффикс ко всем)
        !rename --window  @all #replace="old" to="new" (замена подстроки во всех именах)
        """
        import re
        tokens = raw.split()
        silent = "-s" in tokens or "--silent" in tokens

        use_all = "--all" in raw  # @all уже нормализован в --all DLL-стороной

        def _do_rename(registry, reg_type: str, old: str, new_name: str) -> bool:
            entry = registry.get(old)
            if entry is None:
                self._con_warn(f"!rename {reg_type}: '{old}' не найдено")
                return False
            registry.add(new_name, **({} if reg_type == "ae" else {}))
            # Переносим данные: удаляем старое, создаём новое
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

        # Определить тип объекта
        reg = None
        reg_type = ""
        if "--window" in raw:
            reg, reg_type = self._win_reg, "window"
        elif "--rule" in raw:
            reg, reg_type = self._rule_reg, "rule"
        elif "--ae" in raw:
            reg, reg_type = self._ae_reg, "ae"
        elif "--hotkey" in raw:
            # Хоткеи переименовываются через alias
            if use_all:
                self._con_warn("!rename --hotkey @all: не поддерживается"); return
            old = _parse_named(raw, "name")
            m2  = re.search(r'\bto\b\s+#name=["\']?([^"\';\s]+)["\']?', raw)
            new_name = m2.group(1) if m2 else None
            if not old or not new_name:
                self._con_warn('!rename --hotkey: нужен #name="old" to #name="new"'); return
            # Переименовать alias hotkey
            unbound = self._hotkeys.unbind_by_alias(old)
            if unbound:
                # rebind с новым alias
                bindings = self._hotkeys.all_bindings()
                if unbound in bindings:
                    cmd_val = bindings[unbound]
                    cmd_str = cmd_val.get("cmd", "") if isinstance(cmd_val, dict) else str(cmd_val)
                    self._hotkeys.bind(unbound, cmd_str, alias=new_name)
                    if not silent:
                        self._con_ok(f"!rename --hotkey '{old}' → '{new_name}'")
            else:
                self._con_warn(f"!rename --hotkey '{old}': не найдено")
            return
        else:
            self._con_warn("!rename: используй --window / --rule / --ae / --hotkey"); return

        # @all — пакетное переименование
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
                self._con_ok(f"!rename @all ({reg_type}): переименовано {count} из {len(names)}")
            return

        # Одиночное переименование: #name="old" to #name="new"
        old = _parse_named(raw, "name")
        m2  = re.search(r'\bto\b\s+#name=["\']?([^"\';\s]+)["\']?', raw)
        if not m2:
            # попробовать формат: #name="old" to #name="new"
            m2 = re.search(r'#name=["\']?[^"\';\s]+["\']?\s+to\s+#name=["\']?([^"\';\s]+)["\']?', raw)
        new_name = m2.group(1) if m2 else None

        if not old or not new_name:
            self._con_warn('!rename: синтаксис: !rename --window #name="old" to #name="new"')
            return
        if old == new_name:
            self._con_warn(f"!rename: имена совпадают ('{old}')"); return
        if reg.exists(new_name):
            self._con_warn(f"!rename: '{new_name}' уже существует"); return

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
                return mb.askyesno("Удаление", msg)
            return True

        # --all: удалить всё из реестра
        if "--all" in raw:
            if "--window" in raw:
                names = self._win_reg.all_names()
                if not _confirm(f"Удалить все {len(names)} окон из реестра?"):
                    log.info("$rm --all --window: отменено"); return
                for n in names:
                    if if_dis:
                        e = self._win_reg.get(n) or {}
                        if e.get("enabled", True): continue
                    self._win_reg.remove(n)
                log.info("$rm --all --window: удалено %d записей", len(names))
            elif "--rule" in raw:
                names = self._rule_reg.all_names()
                if not _confirm(f"Удалить все {len(names)} правил?"):
                    log.info("$rm --all --rule: отменено"); return
                for n in names:
                    if if_dis:
                        e = self._rule_reg.get(n) or {}
                        if e.get("connected", True): continue
                    self._rule_reg.remove(n)
                log.info("$rm --all --rule: удалено %d записей", len(names))
            elif "--ae" in raw:
                names = self._ae_reg.all_names()
                if not _confirm(f"Удалить все {len(names)} ae-объектов?"):
                    log.info("$rm --all --ae: отменено"); return
                for n in names:
                    self._ae_reg.remove(n)
                log.info("$rm --all --ae: удалено %d записей", len(names))
            else:
                log.warning("$rm --all: укажи --window / --rule / --ae")
            return

        if not name:
            log.warning('$rm: не указан #name  (пример: $rm --window #name="x")'); return

        if "--window" in raw:
            if not self._win_reg.exists(name):
                log.warning("$rm: '%s' не найдено в реестре", name); return
            entry = self._win_reg.get(name)
            if if_dis and entry and entry.get("enabled", True):
                log.info("$rm: '%s' сейчас enabled — пропущено (--if-disconnected)", name); return
            if not _confirm(f"Удалить окно «{name}» из homrec.create?"):
                log.info("$rm: отменено"); return
            if purge and entry and entry.get("kind") == "notepad":
                fp = entry.get("file", "")
                if fp and Path(fp).exists():
                    try: Path(fp).unlink(); log.info("$rm --purge: файл удалён: %s", fp)
                    except Exception as e: log.warning("$rm --purge: %s", e)
            if purge:
                # удалить хоткеи, ссылающиеся на это окно
                for key, val in list(self._hotkeys.all_bindings().items()):
                    cmd_val = val.get("cmd","") if isinstance(val, dict) else str(val)
                    if name in cmd_val:
                        self._hotkeys.unbind(key)
                        log.info("$rm --purge: хоткей '%s' удалён", key)
            self._win_reg.remove(name)
            log.info("$rm --window: '%s' удалено", name)
            return

        if "--rule" in raw:
            if not self._rule_reg.exists(name):
                log.warning("$rm --rule: '%s' не найдено", name); return
            entry = self._rule_reg.get(name)
            if if_dis and entry and entry.get("connected", True):
                log.info("$rm --rule: '%s' сейчас connected — пропущено", name); return
            if not _confirm(f"Удалить правило «{name}»?"):
                log.info("$rm: отменено"); return
            if purge:
                for key, val in list(self._hotkeys.all_bindings().items()):
                    cmd_val = val.get("cmd","") if isinstance(val, dict) else str(val)
                    if name in cmd_val:
                        self._hotkeys.unbind(key)
            self._rule_reg.remove(name)
            log.info("$rm --rule: '%s' удалено", name)
            return

        if "--ae" in raw:
            if not self._ae_reg.exists(name):
                log.warning("$rm --ae: '%s' не найдено", name); return
            if not _confirm(f"Удалить ae-объект «{name}»?"):
                log.info("$rm: отменено"); return
            self._ae_reg.remove(name)
            log.info("$rm --ae: '%s' удалено", name)
            return

        log.warning("$rm: используй --window / --rule / --ae")

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
            # --all: connect все окна
            if all_obj:
                for name in self._win_reg.all_names():
                    self._cmd_connect(f'!connect --window #name="{name}" 1 -s')
                if not silent: log.info("!connect --window --all: все окна подключены")
                return
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!connect --window: не указан #name"); return
            val = None
            for t in tokens:
                if t in ("0", "1"): val = int(t)
            entry = self._win_reg.get(name)
            if entry is None:
                log.warning("!connect --window '%s': не найдено", name); return
            if toggle:
                val = 0 if entry.get("enabled", True) else 1
            enabled = (val != 0) if val is not None else True
            if not force and entry.get("enabled") == enabled:
                if not silent: log.info("!connect --window '%s': уже в состоянии %s", name, enabled)
                return
            entry["enabled"] = enabled
            self._win_reg.add(name, entry.get("kind", "window"), entry)
            if enabled:
                self._cmd_start_window(f'!start --window #name="{name}"')
            if not silent:
                log.info("!connect --window '%s' → %s", name, "включено" if enabled else "отключено")
            return

        if "--rule" in raw:
            if all_obj:
                for name in self._rule_reg.all_names():
                    self._cmd_connect(f'!connect --rule #name="{name}" -s')
                if not silent: log.info("!connect --rule --all: все правила подключены")
                return
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!connect --rule: не указан #name"); return
            if not self._rule_reg.exists(name):
                log.warning("!connect --rule '%s': не найдено", name); return
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
                if not silent: log.info("!connect --rule '%s': уже подключено", name)
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
                log.warning('!connect --function: синтаксис: !connect --function <cmd> to|; <key> [#name="func"]')
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

        log.warning("!connect: используй --window / --rule / --function")

    # --- !disconnect ----------------------------------------------------------

    def _cmd_disconnect(self, raw: str):
        """
        !disconnect --window  #name="x"      [-s][-q][--all][--toggle][-f]
        !disconnect --rule    #name="x"      [-s][-q][--all][--toggle]
        !disconnect --ae      #type=... #name="x"
        !disconnect --function <cmd> to|; <key>
        !disconnect           #name="func"   (по имени, заданному в !connect --function)
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
                if not silent: log.info("!disconnect --window --all: все окна отключены")
                return
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!disconnect --window: не указан #name"); return
            entry = self._win_reg.get(name)
            if entry is None:
                log.warning("!disconnect --window '%s': не найдено", name); return
            if toggle:
                new_state = not entry.get("enabled", True)
                entry["enabled"] = new_state
                self._win_reg.add(name, entry.get("kind", "window"), entry)
                if not silent: log.info("!disconnect --window '%s' toggle → %s", name, new_state)
                return
            if not force and not entry.get("enabled", True):
                if not silent: log.info("!disconnect --window '%s': уже disabled", name)
                return
            entry["enabled"] = False
            self._win_reg.add(name, entry.get("kind", "window"), entry)
            if not silent: log.info("!disconnect --window '%s' → disabled", name)
            return

        if "--rule" in raw:
            if all_obj:
                for name in self._rule_reg.all_names():
                    self._rule_reg.set_connected(name, False)
                if not silent: log.info("!disconnect --rule --all: все правила отключены")
                return
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!disconnect --rule: не указан #name"); return
            if toggle:
                entry = self._rule_reg.get(name)
                if entry is None:
                    log.warning("!disconnect --rule '%s': не найдено", name); return
                new_state = not entry.get("connected", True)
                self._rule_reg.set_connected(name, new_state)
                if not silent: log.info("!disconnect --rule '%s' toggle → %s", name, new_state)
                return
            if not self._rule_reg.set_connected(name, False):
                log.warning("!disconnect --rule '%s': не найдено", name)
            elif not silent:
                log.info("!disconnect --rule '%s' → disconnected", name)
            return

        if "--ae" in raw:
            ae_type = _parse_named(raw, "type")
            name    = _parse_named(raw, "name")
            if not name:
                log.warning("!disconnect --ae: не указан #name"); return
            if self._ae_reg.remove(name):
                if not silent:
                    log.info("!disconnect --ae [%s] '%s' → удалено", ae_type, name)
            else:
                log.warning("!disconnect --ae '%s': не найдено", name)
            return

        if "--function" in raw:
            m = re.search(r'--function\s+(.+?)\s+(?:to|;)\s+(\S+)', raw, re.IGNORECASE)
            if not m:
                log.warning("!disconnect --function: синтаксис: --function <cmd> to|; <key>")
                return
            key_part = m.group(2).strip()
            self._hotkeys.unbind(key_part)
            if not silent:
                log.info("!disconnect --function '%s' → отвязано", key_part)
            return

        # Отключить по имени (#name="func"), заданному при !connect --function
        name = _parse_named(raw, "name")
        if name:
            unbound = self._hotkeys.unbind_by_alias(name)
            if unbound:
                if not silent:
                    log.info("!disconnect #name='%s' → отвязано (%s)", name, unbound)
            else:
                log.warning("!disconnect #name='%s': не найдено", name)
            return

        log.warning("!disconnect: используй --window / --rule / --ae / --function  или #name=")


    # --- Вспомогательные методы -----------------------------------------------

    def _root_alive(self) -> bool:
        """Проверить, существует ли ещё root-окно Tkinter."""
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
        """Открыть Tkinter-окно с заданным именем и стилем из реестра."""
        if not self._root_alive():
            log.warning("_open_tk_window: root недоступен")
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
            log.info("Tkinter-окно открыто: %s", name)

        self.app.root.after(0, _create)

    def _open_notepad_file(self, path: str):
        """Открыть файл в системном редакторе."""
        try:
            if platform.system() == "Windows":
                os.startfile(path)
            elif platform.system() == "Darwin":
                subprocess.Popen(["open", "-a", "TextEdit", path])
            else:
                subprocess.Popen(["xdg-open", path])
            log.info("Открыт файл: %s", path)
        except Exception as e:
            log.warning("_open_notepad_file error: %s", e)

    def _toggle_desktop_shortcut(self, enable: bool):
        """Создать или удалить ярлык на рабочем столе (Windows)."""
        if platform.system() != "Windows":
            log.warning("Ярлык поддерживается только на Windows")
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
                subprocess.run(["powershell", "-Command", ps], capture_output=True)
                log.info("Ярлык создан: %s", lnk_path)
            except Exception as e:
                log.error("Ошибка создания ярлыка: %s", e)
        else:
            try:
                if lnk_path.exists():
                    lnk_path.unlink()
                    log.info("Ярлык удалён: %s", lnk_path)
            except Exception as e:
                log.error("Ошибка удаления ярлыка: %s", e)