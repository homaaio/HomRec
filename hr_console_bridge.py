"""
hr_console_bridge.py  —  Python shim для hr_console.dll
Версия 2.0: добавлены команды !edit, !create, !start --window, $rm, !connect --function
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

def _parse_named(raw: str, key: str) -> str | None:
    """Извлечь значение #key="value" или #key=value из строки raw."""
    import re
    m = re.search(r'#' + re.escape(key) + r'=["\']?([^"\'#\s]+)["\']?', raw)
    if not m:
        # попробовать с кавычками
        m = re.search(r'#' + re.escape(key) + r'="([^"]*)"', raw)
    if not m:
        m = re.search(r'#' + re.escape(key) + r"='([^']*)'", raw)
    return m.group(1) if m else None


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
        self._win_reg = WindowRegistry(base)
        self._hotkeys = HotkeyManager(base, self)

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
        Разбирает и выполняет расширенные команды:
          !edit --settings #name=shortcut  [1|0]
          !create --window #name="..."     [-o] [-s] [-n]
          !create --window --notepad #name="..."
          !start --window #name="..."
          $rm --window from homrec.create  [-q]
          !connect --function: <cmd> to|; <key>
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

        # ── !start ────────────────────────────────────────────────────────────────
        if raw.startswith("!start"):
            self._cmd_start_window(raw)
            return

        # ── !connect --function ───────────────────────────────────────────────────
        if raw.startswith("!connect"):
            self._cmd_connect_function(raw)
            return

    # ── Реализации команд ────────────────────────────────────────────────────────

    def _cmd_edit(self, raw: str):
        """
        !edit --settings #name=shortcut 1
          Включает/выключает ярлык приложения на рабочем столе.
        """
        flags = _parse_flags(raw)

        if "--settings" not in raw:
            log.warning("!edit: неизвестный модуль (поддерживается --settings)")
            return

        name = _parse_named(raw, "name")
        if not name:
            log.warning("!edit --settings: не указан #name")
            return

        # Извлечь значение (последний токен, не флаг и не #name=...)
        import re
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
        !create --window #name="example" [-o] [-s]
        !create --window --notepad #name="example"

        Флаги:
          -o   только создать, не открывать
          -s   тихий режим (не выводить сообщения)
          -n   не добавлять в реестр (временное окно)
        """
        flags = _parse_flags(raw)
        is_notepad = "--notepad" in raw
        only_create = "-o" in flags   # не открывать
        silent      = "-s" in flags
        no_register = "-n" in flags

        name = _parse_named(raw, "name")
        if not name:
            log.warning("!create --window: не указан #name")
            return

        base  = _get_base_dir()
        kind  = "notepad" if is_notepad else "window"
        extra: dict = {}

        if is_notepad:
            # Создать пустой файл блокнота в папке /create
            create_dir = Path(base) / "create"
            create_dir.mkdir(parents=True, exist_ok=True)
            file_path = create_dir / f"{name}.txt"
            if not file_path.exists():
                file_path.write_text("", encoding="utf-8")
            extra["file"] = str(file_path)

            if not no_register:
                self._win_reg.add(name, "notepad", extra)

            if not only_create:
                self._open_notepad_file(str(file_path))
            if not silent:
                log.info("!create --notepad: '%s' → %s", name, file_path)
        else:
            # Создать пустое Tk-окно
            if not no_register:
                self._win_reg.add(name, "window", extra)

            if not only_create:
                self._open_tk_window(name)
            if not silent:
                log.info("!create --window: '%s' создано%s", name,
                         ", не открыто (-o)" if only_create else "")

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

    def _cmd_connect_function(self, raw: str):
        """
        !connect --function: <команда> to <клавиша>
        !connect --function: <команда> ; <клавиша>

        Примеры:
          !connect --function: !create --window #name="test" to ctrl+shift+t
          !connect --function: !rec ; f9
        """
        import re

        # Ищем разделитель 'to' или ';'
        m = re.search(r'--function:\s*(.+?)\s+(?:to|;)\s+(\S+)\s*$', raw, re.IGNORECASE)
        if not m:
            log.warning("!connect --function: неверный синтаксис.\n"
                        "  Пример: !connect --function: !rec to f9")
            return

        cmd_part = m.group(1).strip()
        key_part = m.group(2).strip()

        self._hotkeys.bind(key_part, cmd_part)
        log.info("!connect: '%s' привязано к клавише '%s'", cmd_part, key_part)

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