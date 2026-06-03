"""
hr_console_bridge.py  —  Python shim для hr_console.dll
Версия 3.0: добавлены !start --rec, !rule, !edit --file/--window/--rule, !create --rule/--ae,
           !connect --window/--rule, !disconnect --window/--rule/--ae/--function,
           математика {int.random(a,b)} в именах
Исправлены баги:
  - LogFilter не защищал от crash при раннем вызове до инициализации DLL
  - _quit() мог зависнуть если root уже уничтожен
  - _load() не инициализировал _lib=None при раннем выходе
  - toggle() обращался к self._lib до проверки на None
  - GC мог удалить колбэки раньше времени (исправлено через аннотации типов)
"""
from __future__ import annotations

import ctypes
import json
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

# ────────────────────────────────────────────────────────────────────────────────
#  Вспомогательные утилиты разбора аргументов
# ────────────────────────────────────────────────────────────────────────────────


import random as _random


def _resolve_math(s: str) -> str:
    """Заменяет {int.random(a, b)} на случайное целое в строке s."""
    import re
    def _replace(m):
        try:
            a, b = int(m.group(1).strip()), int(m.group(2).strip())
            if a > b:
                a, b = b, a
            return str(_random.randint(a, b))
        except Exception:
            return m.group(0)
    return re.sub(r'\{int\.random\((\d+),\s*(\d+)\)\}', _replace, s)

def _parse_named(raw: str, key: str) -> str | None:
    """Извлечь значение #key="value" или #key=value из строки raw."""
    import re
    m = re.search(r'#' + re.escape(key) + r'=["\']?([^"\'#\s]+)["\']?', raw)
    if not m:
        # попробовать с кавычками
        m = re.search(r'#' + re.escape(key) + r'="([^"]*)"', raw)
    if not m:
        m = re.search(r'#' + re.escape(key) + r"='([^']*)'", raw)
    return _resolve_math(m.group(1)) if m else None


def _parse_flags(raw: str) -> set[str]:
    """Собрать все -флаги из строки (токены начинающиеся с -)."""
    import re
    return set(re.findall(r'(?<!\S)-[a-zA-Z]+', raw))


def _get_base_dir() -> str:
    return (os.path.dirname(sys.executable) if getattr(sys, "frozen", False)
            else os.path.dirname(os.path.abspath(__file__)))


# ────────────────────────────────────────────────────────────────────────────────
#  Менеджер созданных окон (хранилище)
# ────────────────────────────────────────────────────────────────────────────────

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




# ────────────────────────────────────────────────────────────────────────────────
#  Менеджер правил (хранилище)
# ────────────────────────────────────────────────────────────────────────────────

class RuleRegistry:
    """
    Хранит правила созданные через !create --rule.
    Данные в <base>/create/rules.json
    Каждое правило: {"body": str, "connected": bool}
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
            log.warning("RuleRegistry load error: %s", e)
            self._data = {}

    def _save(self):
        try:
            self._path.write_text(json.dumps(self._data, ensure_ascii=False, indent=2), "utf-8")
        except Exception as e:
            log.warning("RuleRegistry save error: %s", e)

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

    def set_connected(self, name: str, connected: bool) -> bool:
        if name in self._data:
            self._data[name]["connected"] = connected
            self._save()
            return True
        return False

    def all_names(self) -> list[str]:
        return list(self._data.keys())


# ────────────────────────────────────────────────────────────────────────────────
#  Менеджер AE-объектов (Anything Else)
# ────────────────────────────────────────────────────────────────────────────────

class AERegistry:
    """
    Хранит «anything else» объекты: цвета и другие.
    Данные в <base>/create/ae.json
    Формат: {"name": {"type": "color", "rgb": [r,g,b], "hex": "#..."}, ...}
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
            log.warning("AERegistry load error: %s", e)
            self._data = {}

    def _save(self):
        try:
            self._path.write_text(json.dumps(self._data, ensure_ascii=False, indent=2), "utf-8")
        except Exception as e:
            log.warning("AERegistry save error: %s", e)

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

# ────────────────────────────────────────────────────────────────────────────────
#  Хоткей-менеджер
# ────────────────────────────────────────────────────────────────────────────────

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
        for key, cmd in list(self._bindings.items()):
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

    def bind(self, key: str, cmd: str):
        self._bindings[key] = cmd
        self._save()
        self._register_os(key, cmd)

    def all_bindings(self) -> dict[str, str]:
        return dict(self._bindings)


# ────────────────────────────────────────────────────────────────────────────────
#  NativeConsole
# ────────────────────────────────────────────────────────────────────────────────

class NativeConsole:
    GITHUB = "https://github.com/homaaio/HomREC"

    def __init__(self, app: "HomRecScreen") -> None:
        self.app = app
        self._lib = None  # BUG FIX: инициализировать до _load()

        base = _get_base_dir()
        self._win_reg  = WindowRegistry(base)
        self._rule_reg = RuleRegistry(base)
        self._ae_reg   = AERegistry(base)
        self._hotkeys  = HotkeyManager(base, self)

        self._lib = self._load()
        if not self._lib:
            return

        log_path = os.path.join(base, "homrec.log")

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

            return lib
        except Exception as e:
            log.warning("hr_console.dll load failed: %s", e)
            return None  # BUG FIX: явный None

    # ── Публичный API ────────────────────────────────────────────────────────────

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

    # ── Callbacks DLL ────────────────────────────────────────────────────────────

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

    # ── Расширенный диспетчер команд ─────────────────────────────────────────────

    def _dispatch_extended(self, raw: str):
        """
        Разбирает и выполняет расширенные команды (v3.0):
          !start --rec 1|0
          !rule  --get from connect #name="..."
          !rule  --check #name="..."
          !edit  --file|--window|--rule|--settings #name="..."
          !create --window|--rule|--ae ...
          !start  --window #name="..."
          !connect --window|--rule|--function: ...
          !disconnect --window|--rule|--ae|--function: ...
          $rm --window #name="..." [-q]
        """
        raw = raw.strip()

        # ── $rm ──────────────────────────────────────────────────────────────────
        if raw.startswith("$rm"):
            self._cmd_rm(raw)
            return

        # ── !edit ─────────────────────────────────────────────────────────────────
        if raw.startswith("!edit"):
            self._cmd_edit(raw)
            return

        # ── !create ───────────────────────────────────────────────────────────────
        if raw.startswith("!create"):
            self._cmd_create(raw)
            return

        # ── !start --rec ──────────────────────────────────────────────────────────
        if raw.startswith("!start") and "--rec" in raw:
            self._cmd_start_rec(raw)
            return

        # ── !start --window ───────────────────────────────────────────────────────
        if raw.startswith("!start"):
            self._cmd_start_window(raw)
            return

        # ── !rule ─────────────────────────────────────────────────────────────────
        if raw.startswith("!rule"):
            self._cmd_rule(raw)
            return

        # ── !connect ──────────────────────────────────────────────────────────────
        if raw.startswith("!connect"):
            self._cmd_connect(raw)
            return

        # ── !disconnect ───────────────────────────────────────────────────────────
        if raw.startswith("!disconnect"):
            self._cmd_disconnect(raw)
            return

    # ── Реализации команд ────────────────────────────────────────────────────────

    def _cmd_edit(self, raw: str):
        """
        !edit --file    #name="example"  — открыть файл на редактирование
        !edit --window  #name="example"  — открыть окно
        !edit --rule    #name="example"; <new body>  — изменить тело правила
        !edit --settings #name=shortcut [1|0]  — ярлык на рабочем столе
        """
        import re

        if "--file" in raw:
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!edit --file: не указан #name")
                return
            entry = self._win_reg.get(name)
            if entry and entry.get("kind") == "notepad":
                fp = entry.get("file", "")
                if fp:
                    self._open_notepad_file(fp)
                    log.info("!edit --file '%s': открыт %s", name, fp)
                else:
                    log.warning("!edit --file '%s': нет пути к файлу", name)
            else:
                log.warning("!edit --file: '%s' не найдено или не является notepad", name)
            return

        if "--window" in raw:
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!edit --window: не указан #name")
                return
            self._cmd_start_window(f'!start --window #name="{name}"')
            log.info("!edit --window '%s': открыто", name)
            return

        if "--rule" in raw:
            # Синтаксис: !edit --rule #name="example"; <new body>
            m = re.search(r'#name=["\']?([^"\'\s;]+)["\']?\s*;\s*(.+)', raw)
            if not m:
                name = _parse_named(raw, "name")
                if name and self._rule_reg.exists(name):
                    entry = self._rule_reg.get(name)
                    log.info("!edit --rule '%s': текущее тело: %s", name, entry.get("body", ""))
                else:
                    log.warning("!edit --rule: синтаксис: !edit --rule #name=\"x\"; <new body>")
                return
            name     = _resolve_math(m.group(1).strip())
            new_body = m.group(2).strip()
            if self._rule_reg.exists(name):
                entry = self._rule_reg.get(name)
                entry["body"] = new_body
                self._rule_reg.add(name, new_body, entry.get("connected", True))
                log.info("!edit --rule '%s': тело обновлено", name)
            else:
                log.warning("!edit --rule: правило '%s' не найдено", name)
            return

        if "--settings" in raw:
            flags = _parse_flags(raw)
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!edit --settings: не указан #name")
                return
            tokens = raw.split()
            value_tokens = [t for t in tokens
                            if not t.startswith("!") and not t.startswith("--")
                            and not t.startswith("#") and not t.startswith("-")]
            value = value_tokens[-1] if value_tokens else "1"
            if name == "shortcut":
                enable = value.strip() in ("1", "true", "on", "yes")
                self._toggle_desktop_shortcut(enable)
            else:
                log.warning("!edit --settings: неизвестный параметр #name=%s", name)
            return

        log.warning("!edit: неизвестный режим. Используйте --file/--window/--rule/--settings")

    def _toggle_desktop_shortcut(self, enable: bool):
        """Создать или удалить ярлык на рабочем столе (Windows)."""
        if platform.system() != "Windows":
            log.warning("Ярлык на рабочий стол поддерживается только на Windows")
            return

        desktop = Path(os.path.join(os.path.expanduser("~"), "Desktop"))
        lnk_path = desktop / "HomRec.lnk"
        target = sys.executable if getattr(sys, "frozen", False) else os.path.abspath(__file__).replace("hr_console_bridge.py", "homrec.py")

        if enable:
            try:
                import winreg  # noqa — только для проверки Windows
                # Используем PowerShell для создания ярлыка (не требует pywin32)
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
                else:
                    log.info("Ярлык не найден, удалять нечего")
            except Exception as e:
                log.error("Ошибка удаления ярлыка: %s", e)

    def _cmd_create(self, raw: str):
        """
        !create --window #name="example" [-o] [-s] [-n] [-c] [-d]
        !create --window --notepad [as .ext] #name="example"
        !create --rule   #name="example"; <body>  [-c] [-d]
        !create --ae     #type=color{rgb=(r,g,b)} #name="example"
        !create --ae     #type=color{hex=(#RRGGBB)} #name="example"

        Флаги:
          -o   не открывать окно сразу
          -s   тихий режим
          -n   не добавлять в реестр
          -c   автоматически подключить (!connect) после создания
          -d   создать как disconnected
        """
        import re

        flags       = _parse_flags(raw)
        is_notepad  = "--notepad" in raw
        is_rule     = "--rule" in raw
        is_ae       = "--ae" in raw
        is_window   = "--window" in raw and not is_rule and not is_ae
        only_create = "-o" in flags
        silent      = "-s" in flags
        no_register = "-n" in flags
        auto_connect= "-c" in flags
        disconnected= "-d" in flags

        name = _parse_named(raw, "name")
        if not name:
            log.warning("!create: не указан #name")
            return

        base = _get_base_dir()

        # ── --rule ────────────────────────────────────────────────────────────────
        if is_rule:
            # Синтаксис: !create --rule #name="example"; <body>
            m = re.search(r'#name=["\']?([^"\'\s;]+)["\']?\s*;\s*(.+)', raw)
            body = m.group(2).strip() if m else ""
            connected = not disconnected
            self._rule_reg.add(name, body, connected)
            if auto_connect and not connected:
                self._rule_reg.set_connected(name, True)
            if not silent:
                log.info("!create --rule '%s': создано, body='%s', connected=%s",
                         name, body, self._rule_reg.get(name, {}).get("connected"))
            return

        # ── --ae ──────────────────────────────────────────────────────────────────
        if is_ae:
            ae_type_raw = _parse_named(raw, "type")
            if not ae_type_raw:
                log.warning("!create --ae: не указан #type")
                return
            # Parse color
            ae_type = ae_type_raw.split("{")[0].lower()  # e.g. "color"
            ae_data: dict = {"connected": not disconnected}

            if ae_type == "color":
                # rgb=(r,g,b) or hex=(#RRGGBB)
                rgb_m = re.search(r'rgb=\((\d+),\s*(\d+),\s*(\d+)\)', raw)
                hex_m = re.search(r'hex=\(#?([0-9A-Fa-f]{6})\)', raw)
                if rgb_m:
                    r2, g2, b2 = int(rgb_m.group(1)), int(rgb_m.group(2)), int(rgb_m.group(3))
                    ae_data["rgb"] = [r2, g2, b2]
                    ae_data["hex"] = "#{:02X}{:02X}{:02X}".format(r2, g2, b2)
                elif hex_m:
                    hx = hex_m.group(1).upper()
                    ae_data["hex"] = "#" + hx
                    ae_data["rgb"] = [int(hx[0:2], 16), int(hx[2:4], 16), int(hx[4:6], 16)]
                else:
                    log.warning("!create --ae color: не найдено rgb=(...) или hex=(...)")
                    return

            self._ae_reg.add(name, ae_type, ae_data)
            if not silent:
                log.info("!create --ae [%s] '%s': %s", ae_type, name, ae_data)
            return

        # ── --window ──────────────────────────────────────────────────────────────
        kind  = "notepad" if is_notepad else "window"
        extra: dict = {"enabled": not disconnected}

        if is_notepad:
            # Support: --notepad as .ext (custom extension)
            ext_m = re.search(r'as\s+\.([\w]+)', raw)
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
                log.info("!create --window '%s'%s%s",
                         name,
                         ", не открыто (-o)" if only_create else "",
                         ", disconnected (-d)" if disconnected else "")

        if auto_connect and not disconnected:
            self._cmd_connect(f'!connect --window #name="{name}" 1')


    def _cmd_start_window(self, raw: str):
        """
        !start --window #name="example"
        Открывает ранее созданное окно из реестра.
        """
        if "--window" not in raw:
            log.warning("!start: поддерживается только --window")
            return

        name = _parse_named(raw, "name")
        if not name:
            log.warning("!start --window: не указан #name")
            return

        entry = self._win_reg.get(name)
        if entry is None:
            log.warning("!start --window: окно '%s' не найдено в реестре", name)
            return

        if entry.get("kind") == "notepad":
            file_path = entry.get("file", "")
            if file_path:
                self._open_notepad_file(file_path)
            else:
                log.warning("!start: нет пути к файлу для '%s'", name)
        else:
            self._open_tk_window(name)

    def _cmd_rm(self, raw: str):
        """
        $rm --window from homrec.create [-q]
        [-q] — без подтверждения
        """
        if "--window" not in raw:
            log.warning("$rm: поддерживается только --window")
            return

        flags = _parse_flags(raw)
        quiet = "-q" in flags

        name = _parse_named(raw, "name")
        if not name:
            log.warning("$rm: не указан #name=... (пример: $rm --window #name=\"example\")")
            return

        if not self._win_reg.exists(name):
            log.warning("$rm: окно '%s' не найдено", name)
            return

        if not quiet:
            # Спросить у пользователя через Tkinter messagebox
            if self._root_alive():
                import tkinter.messagebox as mb
                answer = mb.askyesno(
                    "Удаление окна",
                    f"Вы уверены, что хотите удалить окно «{name}» из homrec.create?",
                )
                if not answer:
                    log.info("$rm: удаление отменено пользователем")
                    return
            else:
                # fallback: просто выполнить без GUI
                pass

        entry = self._win_reg.get(name)
        if entry and entry.get("kind") == "notepad":
            fp = entry.get("file", "")
            if fp and Path(fp).exists():
                try:
                    Path(fp).unlink()
                    log.info("$rm: файл удалён: %s", fp)
                except Exception as e:
                    log.warning("$rm: не удалось удалить файл: %s", e)

        self._win_reg.remove(name)
        log.info("$rm: окно '%s' удалено из реестра", name)

    def _cmd_connect(self, raw: str):
        """
        !connect --window  #name="..." 1|0
        !connect --rule    #name="..."  [-s/-q]
        !connect --function: <cmd> to|; <key> [#name="..."]
        """
        import re

        # ── --window ──────────────────────────────────────────────────────────────
        if "--window" in raw:
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!connect --window: не указан #name")
                return
            # Check value token after the command (1 = enable, 0 = disable)
            tokens = raw.split()
            val = None
            for t in tokens:
                if t in ("0", "1"):
                    val = int(t)
            entry = self._win_reg.get(name)
            if entry is None:
                log.warning("!connect --window: окно '%s' не найдено", name)
                return
            # Store enabled state
            entry["enabled"] = (val != 0) if val is not None else True
            self._win_reg.add(name, entry.get("kind", "window"), entry)
            if val != 0:
                self._cmd_start_window(f"!start --window #name=\"{name}\"")
            log.info("!connect --window '%s' → enabled=%s", name, entry["enabled"])
            return

        # ── --rule ────────────────────────────────────────────────────────────────
        if "--rule" in raw:
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!connect --rule: не указан #name")
                return
            if not self._rule_reg.exists(name):
                log.warning("!connect --rule: правило '%s' не найдено", name)
                return
            self._rule_reg.set_connected(name, True)
            log.info("!connect --rule '%s' → connected", name)
            return

        # ── --function: ───────────────────────────────────────────────────────────
        m = re.search(r'--function:\s*(.+?)\s+(?:to|;)\s+(\S+)\s*(?:#name=["\']?([^"\'\s]+)["\']?)?\s*$',
                      raw, re.IGNORECASE)
        if not m:
            log.warning("!connect --function: неверный синтаксис.\n"
                        "  Пример: !connect --function: !rec to f9 [#name=\"myfunc\"]")
            return

        cmd_part  = m.group(1).strip()
        key_part  = m.group(2).strip()
        func_name = m.group(3)

        self._hotkeys.bind(key_part, cmd_part)
        log.info("!connect: '%s' привязано к '%s'%s",
                 cmd_part, key_part, f" (name={func_name})" if func_name else "")

    # Keep old alias for backward compatibility
    def _cmd_connect_function(self, raw: str):
        self._cmd_connect(raw)


    def _cmd_start_rec(self, raw: str):
        """!start --rec 1|0  — запустить или остановить запись."""
        tokens = raw.split()
        val = None
        for i, t in enumerate(tokens):
            if t == "--rec" and i + 1 < len(tokens):
                try:
                    val = int(tokens[i + 1])
                except ValueError:
                    pass
        if val is None:
            log.warning("!start --rec: нужно указать 1 (старт) или 0 (стоп)")
            return
        if val == 1:
            if not getattr(self.app, "recording", False):
                if self._root_alive():
                    self.app.root.after(0, self.app.start_recording)
                if self._lib:
                    self._lib.hr_con_set_recording(1)
                log.info("!start --rec 1: запись начата")
            else:
                log.info("!start --rec 1: уже идёт запись")
        else:
            if getattr(self.app, "recording", False):
                if self._root_alive():
                    self.app.root.after(0, self.app.stop_recording)
                if self._lib:
                    self._lib.hr_con_set_recording(0)
                log.info("!start --rec 0: запись остановлена")
            else:
                log.info("!start --rec 0: запись не была активна")

    def _cmd_rule(self, raw: str):
        """
        !rule --get from connect #name="example"
        !rule --check #name="example"
        """
        name = _parse_named(raw, "name")
        if not name:
            log.warning("!rule: не указан #name")
            return

        if "--check" in raw:
            entry = self._rule_reg.get(name)
            if entry is None:
                log.warning("!rule --check: правило '%s' не найдено", name)
            else:
                status = "активно (connected)" if entry.get("connected", True) else "отключено (disconnected)"
                log.info("!rule --check '%s': %s | body: %s", name, status, entry.get("body", ""))
            return

        if "--get" in raw and "from connect" in raw:
            # Пример: получить правило из подключённого источника (заглушка — расширяется пользователем)
            entry = self._rule_reg.get(name)
            if entry:
                log.info("!rule --get '%s': %s", name, entry)
            else:
                log.warning("!rule --get: правило '%s' не найдено в реестре", name)
            return

        log.warning("!rule: неизвестный режим. Используйте --get from connect или --check")

    def _cmd_disconnect(self, raw: str):
        """
        !disconnect --window  #name="..."
        !disconnect --rule    #name="..."
        !disconnect --ae      #type=... #name="..."
        !disconnect --function: <cmd> to|; <key>
        """
        import re

        if "--window" in raw:
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!disconnect --window: не указан #name")
                return
            entry = self._win_reg.get(name)
            if entry is None:
                log.warning("!disconnect --window: окно '%s' не найдено", name)
                return
            entry["enabled"] = False
            self._win_reg.add(name, entry.get("kind", "window"), entry)
            log.info("!disconnect --window '%s' → disabled", name)
            return

        if "--rule" in raw:
            name = _parse_named(raw, "name")
            if not name:
                log.warning("!disconnect --rule: не указан #name")
                return
            if not self._rule_reg.set_connected(name, False):
                log.warning("!disconnect --rule: правило '%s' не найдено", name)
            else:
                log.info("!disconnect --rule '%s' → disconnected", name)
            return

        if "--ae" in raw:
            ae_type = _parse_named(raw, "type")
            name    = _parse_named(raw, "name")
            if not name:
                log.warning("!disconnect --ae: не указан #name")
                return
            if self._ae_reg.remove(name):
                log.info("!disconnect --ae [%s] '%s' → removed", ae_type, name)
            else:
                log.warning("!disconnect --ae: '%s' не найдено", name)
            return

        if "--function" in raw:
            m = re.search(r'--function:\s*(.+?)\s+(?:to|;)\s+(\S+)', raw, re.IGNORECASE)
            if not m:
                log.warning("!disconnect --function: неверный синтаксис")
                return
            key_part = m.group(2).strip()
            bindings = self._hotkeys.all_bindings()
            if key_part in bindings:
                del bindings[key_part]
                # Re-save
                self._hotkeys._bindings = bindings
                self._hotkeys._save()
                if self._hotkeys._kb:
                    try:
                        self._hotkeys._kb.remove_hotkey(key_part)
                    except Exception:
                        pass
                log.info("!disconnect --function: клавиша '%s' отвязана", key_part)
            else:
                log.warning("!disconnect --function: клавиша '%s' не найдена", key_part)
            return

        log.warning("!disconnect: неизвестный режим. Используйте --window/--rule/--ae/--function")

    # ── Вспомогательные методы ───────────────────────────────────────────────────

    def _open_tk_window(self, name: str):
        """Открыть пустое Tkinter-окно с заданным именем."""
        if not self._root_alive():
            log.warning("_open_tk_window: root недоступен")
            return

        def _create():
            import tkinter as tk
            top = tk.Toplevel(self.app.root)
            top.title(name)
            top.geometry("600x400")
            # Минимальный стиль чтобы не выглядело системным
            try:
                bg = self.app.colors.get("bg", "#1e1e2e")
                fg = self.app.colors.get("text", "#cdd6f4")
            except Exception:
                bg, fg = "#1e1e2e", "#cdd6f4"
            top.configure(bg=bg)
            import tkinter as tk2
            tk2.Label(top, text=name, bg=bg, fg=fg,
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

    def _safe_destroy(self):
        try:
            self.app.root.destroy()
        except Exception:
            pass

    def _root_alive(self) -> bool:
        """Проверить, существует ли ещё root-окно Tkinter."""
        try:
            return bool(self.app.root.winfo_exists())
        except Exception:
            return False