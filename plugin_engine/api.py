"""
plugin_engine/api.py — HomRec Plugin API

Full permission surface:
  - ui           : menus, toolbars, notifications, dialogs, sidebar panels
  - overlay      : text/image/gradient/shape overlays drawn on top of recording
  - audio        : mic/sys volume + mute
  - keyboard     : hotkey capture
  - filesystem   : read/write inside plugin dir
  - network      : HTTP get/post
  - theme        : inject CSS-like color overrides into the app
  - inter_plugin : send messages to / call functions of other plugins
  - system       : read OS info, process list (read-only)

Conflicts between plugins are detected and logged as warnings in the message system.
"""

import logging
from typing import Callable, Any

log = logging.getLogger("homrec.api")

# Registry for inter-plugin communication
_PLUGIN_REGISTRY: dict[str, "LuaSandboxRef"] = {}


def build_api(manifest, app_ref) -> dict:
    """Build an API dict for one plugin."""
    perms = manifest.permissions
    api: dict[str, Any] = {}

    # ── Always available ───────────────────────────────────────────────────────
    api["log"]     = _LogAPI(manifest.name)
    api["storage"] = _StorageAPI(manifest.name)
    api["events"]  = _EventsAPI(app_ref)

    # ── By permission ─────────────────────────────────────────────────────────
    if "ui" in perms:
        api["ui"] = _UIAPI(app_ref, manifest.name, manifest)

    if "overlay" in perms:
        api["overlay"] = _OverlayAPI(app_ref, manifest.name)

    if "audio" in perms:
        api["audio"] = _AudioAPI(app_ref)

    if "keyboard" in perms:
        api["keyboard"] = _KeyboardAPI(app_ref)

    if "filesystem" in perms:
        api["filesystem"] = _FilesystemAPI(manifest)

    if "network" in perms:
        api["network"] = _NetworkAPI(manifest.name)

    if "theme" in perms:
        api["theme"] = _ThemeAPI(app_ref, manifest.name)

    if "inter_plugin" in perms:
        api["inter_plugin"] = _InterPluginAPI(manifest.name)

    if "system" in perms:
        api["system"] = _SystemAPI()

    return api


# ── Helpers ───────────────────────────────────────────────────────────────────

def _warn_conflict(source: str, msg: str) -> None:
    """Log a plugin conflict warning — also surfaces in UI if app_ref available."""
    full = f"[Plugin conflict] {source}: {msg}"
    log.warning(full)
    import tkinter.messagebox as _mb
    try:
        _mb.showwarning("Plugin Conflict", full)
    except Exception:
        pass


# ── Log API ───────────────────────────────────────────────────────────────────

class _LogAPI:
    def __init__(self, plugin_name: str):
        self._log = logging.getLogger(f"plugin.{plugin_name}")

    def info(self, msg: str)    -> None: self._log.info(str(msg))
    def warning(self, msg: str) -> None: self._log.warning(str(msg))
    def debug(self, msg: str)   -> None: self._log.debug(str(msg))
    def error(self, msg: str)   -> None: self._log.error(str(msg))


# ── Storage API ───────────────────────────────────────────────────────────────

class _StorageAPI:
    def __init__(self, plugin_name: str):
        import os, json
        self._file = os.path.join("plugins", plugin_name, "storage.json")
        self._data: dict = {}
        try:
            with open(self._file, "r", encoding="utf-8") as f:
                self._data = json.load(f)
        except Exception:
            pass

    def get(self, key: str, default=None):
        return self._data.get(key, default)

    def set(self, key: str, value) -> None:
        import json
        self._data[key] = value
        try:
            with open(self._file, "w", encoding="utf-8") as f:
                json.dump(self._data, f)
        except Exception as e:
            log.warning(f"storage.set failed: {e}")

    def delete(self, key: str) -> None:
        self._data.pop(key, None)

    def clear(self) -> None:
        self._data = {}

    def keys(self) -> list:
        return list(self._data.keys())


# ── Events API ────────────────────────────────────────────────────────────────

class _EventsAPI:
    def __init__(self, app_ref):
        self._app = app_ref

    def on(self, event: str, callback) -> None:
        """Subscribe to HomRec event.
        Events: recording_start, recording_stop, pause, resume, tick,
                theme_change, language_change, plugin_message
        """
        if hasattr(self._app, "_event_hooks"):
            self._app._event_hooks.setdefault(event, []).append(callback)

    def emit(self, event: str, *args) -> None:
        """Emit a custom event (visible to all plugins via events.on)."""
        if hasattr(self._app, "_event_hooks"):
            for cb in self._app._event_hooks.get(event, []):
                try:
                    cb(*args)
                except Exception as e:
                    log.debug(f"event {event} callback error: {e}")


# ── UI API ────────────────────────────────────────────────────────────────────

class _UIAPI:
    """
    homrec.ui — add/modify UI elements inside and outside the main window.
    Requires permission: ui
    """
    def __init__(self, app_ref, plugin_name: str, manifest=None):
        self._app      = app_ref
        self._name     = plugin_name
        self._manifest = manifest
        self._sidebar_widgets: list = []
        self._injected_frames: list = []

    def call_python(self, fn_name: str, *args):
        """
        Call a function from plugin_module.py in this plugin's folder.
        The function receives (app_ref, *args).

        Example (Lua):
            homrec.ui.call_python("open_my_window")
        """
        import importlib.util, sys as _sys, tkinter.messagebox as _mb
        plugin_dir = getattr(self._manifest, "plugin_dir", None)
        if plugin_dir is None:
            _mb.showerror("Plugin error",
                          f"call_python: no plugin_dir for '{self._name}'")
            return None
        mod_path = os.path.join(plugin_dir, "plugin_module.py")
        if not os.path.exists(mod_path):
            _mb.showerror("Plugin error",
                          f"plugin_module.py not found in:\n{plugin_dir}")
            return None
        cache_key = f"_hrp_mod_{self._name}"
        mod = _sys.modules.get(cache_key)
        if mod is None:
            try:
                spec = importlib.util.spec_from_file_location(cache_key, mod_path)
                mod  = importlib.util.module_from_spec(spec)
                _sys.modules[cache_key] = mod
                spec.loader.exec_module(mod)
            except Exception as e:
                import traceback
                _mb.showerror("Plugin error",
                              f"Failed to load plugin_module.py:\n\n{traceback.format_exc()}")
                _sys.modules.pop(cache_key, None)
                return None
        fn = getattr(mod, fn_name, None)
        if fn is None:
            _mb.showerror("Plugin error",
                          f"'{fn_name}' not found in plugin_module.py")
            return None
        try:
            return fn(self._app, *args)
        except Exception as e:
            import traceback
            msg = traceback.format_exc()
            log.warning(f"call_python '{fn_name}': {msg}")
            try:
                import tkinter.messagebox as _mb
                _mb.showerror("Plugin error",
                              f"call_python('{fn_name}') failed:\n\n{e}")
            except Exception:
                pass
            return None

    # Menus
    def add_menu_item(self, menu: str, label: str, callback) -> None:
        """Add item to File|View|Settings|Help menu."""
        if hasattr(self._app, "_plugin_menu_items"):
            self._app._plugin_menu_items.append((menu, label, callback))
        if hasattr(self._app, "_apply_plugin_menu_item"):
            self._app._apply_plugin_menu_item(menu, label, callback)

    def add_menu_separator(self, menu: str) -> None:
        """Add a separator to a menu."""
        if hasattr(self._app, "_menus"):
            m = self._app._menus.get(menu)
            if m:
                try:
                    m.add_separator()
                except Exception as e:
                    log.debug(f"add_menu_separator {menu}: {e}")

    # Toolbar
    def add_toolbar_button(self, label: str, callback, icon: str = None) -> None:
        """Add a button to the plugin toolbar area."""
        if hasattr(self._app, "_plugin_toolbar_items"):
            self._app._plugin_toolbar_items.append((label, callback, icon))

    # Notifications
    def show_notification(self, text: str, type: str = "info") -> None:
        if hasattr(self._app, "show_notification"):
            self._app.show_notification(text, type)

    def show_dialog(self, title: str, message: str) -> None:
        import tkinter.messagebox as _mb
        _mb.showinfo(title, message)

    def show_warning(self, title: str, message: str) -> None:
        import tkinter.messagebox as _mb
        _mb.showwarning(title, message)

    def ask_yes_no(self, title: str, message: str) -> bool:
        import tkinter.messagebox as _mb
        return bool(_mb.askyesno(title, message))

    def ask_string(self, title: str, prompt: str) -> str | None:
        import tkinter.simpledialog as _sd
        return _sd.askstring(title, prompt)

    # Sidebar panel injection
    def add_sidebar_panel(self, title: str, build_fn) -> None:
        """
        Inject a panel into the left sidebar of the main window.
        build_fn(parent_frame, colors) → None  (builds widgets inside parent_frame)
        Requires permission: ui
        """
        if not hasattr(self._app, "root"):
            return
        try:
            # Find the left_panel (first Frame child of main_container)
            left_panel = self._find_left_panel()
            if left_panel is None:
                log.warning(f"Plugin {self._name}: sidebar panel target not found")
                return
            import tkinter as tk
            c = getattr(self._app, "colors", {})
            sep = tk.Frame(left_panel, bg=c.get("surface_light", "#45475a"), height=1)
            sep.pack(fill="x", padx=8, pady=4)
            container = tk.Frame(left_panel, bg=c.get("surface", "#313244"))
            container.pack(fill="x", padx=8, pady=4)
            import tkinter as tk
            tk.Label(container, text=title,
                     bg=c.get("surface", "#313244"),
                     fg=c.get("accent", "#89b4fa"),
                     font=("Segoe UI", 9, "bold")).pack(anchor="w", padx=4, pady=(4,0))
            build_fn(container, c)
            self._sidebar_widgets.append(container)
            log.info(f"Plugin {self._name}: sidebar panel '{title}' injected")
        except Exception as e:
            log.warning(f"Plugin {self._name} add_sidebar_panel error: {e}")

    def _find_left_panel(self):
        """Heuristic: first Frame child of main_container that has pack_propagate=False."""
        try:
            root = self._app.root
            for child in root.winfo_children():
                # main_container is the outermost Frame
                ctype = child.winfo_class()
                if ctype == "Frame":
                    for sub in child.winfo_children():
                        if sub.winfo_class() == "Frame":
                            info = sub.pack_info() if hasattr(sub, "pack_info") else {}
                            if info.get("side") == "left":
                                return sub
        except Exception:
            pass
        return None

    # Inject arbitrary frame anywhere in main window
    def inject_frame(self, location: str, build_fn) -> None:
        """
        Inject a tk.Frame into the main window.
        location: 'left_panel' | 'right_panel' | 'bottom'
        build_fn(parent_frame, colors) → None
        """
        try:
            import tkinter as tk
            c  = getattr(self._app, "colors", {})
            bg = c.get("bg", "#1e1e2e")

            if location == "left_panel":
                parent = self._find_left_panel()
            elif location == "right_panel":
                parent = self._find_right_panel()
            elif location == "bottom":
                parent = self._find_bottom_bar()
            else:
                parent = None

            if parent is None:
                log.warning(f"Plugin {self._name}: inject_frame location '{location}' not found")
                return

            frame = tk.Frame(parent, bg=bg)
            frame.pack(fill="x", padx=4, pady=2)
            build_fn(frame, c)
            self._injected_frames.append(frame)
        except Exception as e:
            log.warning(f"Plugin {self._name} inject_frame error: {e}")

    def _find_right_panel(self):
        try:
            root = self._app.root
            for child in root.winfo_children():
                if child.winfo_class() == "Frame":
                    for sub in child.winfo_children():
                        info = sub.pack_info() if hasattr(sub, "pack_info") else {}
                        if info.get("side") == "right":
                            return sub
        except Exception:
            pass
        return None

    def _find_bottom_bar(self):
        """Return the last Frame in the main window (bottom bar area)."""
        try:
            root = self._app.root
            children = root.winfo_children()
            for child in reversed(children):
                if child.winfo_class() == "Frame":
                    return child
        except Exception:
            pass
        return None

    # Windows
    def open_advanced_settings(self) -> None:
        if hasattr(self._app, "_open_advanced_settings"):
            self._app._open_advanced_settings()

    def open_console(self) -> None:
        if hasattr(self._app, "_open_console"):
            self._app._open_console()

    def open_library(self) -> None:
        if hasattr(self._app, "_open_library"):
            self._app._open_library()

    def open_folders(self) -> None:
        if hasattr(self._app, "_open_folders_window"):
            self._app._open_folders_window()

    # Color / appearance modification (requires ui perm)
    def set_widget_color(self, widget_name: str, bg: str = None, fg: str = None) -> None:
        """
        Modify bg/fg of a named widget in the main window.
        widget_name: 'status_label' | 'time_label' | 'fps_label' | 'res_label'
        """
        try:
            w = getattr(self._app, widget_name, None)
            if w and hasattr(w, "config"):
                kw = {}
                if bg:
                    kw["bg"] = bg
                if fg:
                    kw["fg"] = fg
                if kw:
                    w.config(**kw)
        except Exception as e:
            log.debug(f"set_widget_color {widget_name}: {e}")

    def get_root(self):
        """Return the raw tk.Tk root window (advanced — use carefully)."""
        return getattr(self._app, "root", None)

    def register_command(self, cmd: str, callback) -> None:
        """
        Register a console command for this plugin.
        When the user types `cmd` in the status bar, callback(cmd) is called.

        Example (Lua):
            homrec.ui.register_command("!myplugin", my_handler)
        """
        if not hasattr(self._app, "_plugin_commands"):
            self._app._plugin_commands = {}
        self._app._plugin_commands[cmd.lower().strip()] = {
            "plugin": self._name,
            "cb":     callback,
        }
        log.debug(f"Plugin {self._name}: registered command '{cmd}'")


# ── Overlay API ───────────────────────────────────────────────────────────────

class _OverlayAPI:
    """
    homrec.overlay — draw text, images, gradients, shapes on the live preview
                     and encode them into the recording.
    Requires permission: overlay
    """
    def __init__(self, app_ref, plugin_name: str):
        self._app   = app_ref
        self._name  = plugin_name
        self._items: list[dict] = []

    def text(self, content: str, x: int = 10, y: int = 10,
             color: str = "#ffffff", size: int = 16, id: str = "default") -> None:
        """Draw a text overlay at (x,y)."""
        self._items = [i for i in self._items if i.get("id") != id]
        self._items.append(dict(type="text", id=id, content=content,
                                x=x, y=y, color=color, size=size))
        self._push()

    def image(self, path: str, x: int = 0, y: int = 0,
              w: int = 64, h: int = 64, id: str = "img") -> None:
        """Draw an image overlay from a file path."""
        self._items = [i for i in self._items if i.get("id") != id]
        self._items.append(dict(type="image", id=id,
                                path=path, x=x, y=y, w=w, h=h))
        self._push()

    def gradient(self, x: int = 0, y: int = 0, w: int = 200, h: int = 40,
                 color_start: str = "#000000", color_end: str = "#00000000",
                 direction: str = "horizontal", id: str = "gradient") -> None:
        """
        Draw a gradient rectangle overlay.
        direction: 'horizontal' | 'vertical'
        Colors support RGBA hex (#RRGGBBAA).
        """
        self._items = [i for i in self._items if i.get("id") != id]
        self._items.append(dict(type="gradient", id=id,
                                x=x, y=y, w=w, h=h,
                                color_start=color_start,
                                color_end=color_end,
                                direction=direction))
        self._push()

    def rect(self, x: int = 0, y: int = 0, w: int = 100, h: int = 50,
             color: str = "#ffffff", alpha: float = 1.0, id: str = "rect") -> None:
        """Draw a filled rectangle overlay."""
        self._items = [i for i in self._items if i.get("id") != id]
        self._items.append(dict(type="rect", id=id,
                                x=x, y=y, w=w, h=h, color=color, alpha=alpha))
        self._push()

    def remove(self, id: str) -> None:
        self._items = [i for i in self._items if i.get("id") != id]
        self._push()

    def hide(self) -> None:
        self._items.clear()
        self._push()

    def list_items(self) -> list:
        """Return current overlay item list (read-only copy)."""
        return list(self._items)

    def _push(self) -> None:
        if hasattr(self._app, "set_overlay_items"):
            self._app.set_overlay_items(self._items)


# ── Audio API ─────────────────────────────────────────────────────────────────

class _AudioAPI:
    """homrec.audio — control mic/system audio. Requires permission: audio"""
    def __init__(self, app_ref):
        self._app = app_ref

    def set_mic_volume(self, volume: float) -> None:
        if hasattr(self._app, "settings"):
            self._app.settings.set("mic_volume", int(max(0.0, min(1.0, volume)) * 100))

    def set_sys_volume(self, volume: float) -> None:
        if hasattr(self._app, "settings"):
            self._app.settings.set("sys_volume", int(max(0.0, min(1.0, volume)) * 100))

    def get_mic_volume(self) -> float:
        if hasattr(self._app, "settings"):
            return self._app.settings.get("mic_volume", 80) / 100.0
        return 0.8

    def get_sys_volume(self) -> float:
        if hasattr(self._app, "settings"):
            return self._app.settings.get("sys_volume", 50) / 100.0
        return 0.5

    def mute_mic(self) -> None:
        if hasattr(self._app, "settings"):
            self._app.settings.set("mic_mute", True)

    def unmute_mic(self) -> None:
        if hasattr(self._app, "settings"):
            self._app.settings.set("mic_mute", False)

    def mute_sys(self) -> None:
        if hasattr(self._app, "settings"):
            self._app.settings.set("sys_mute", True)

    def unmute_sys(self) -> None:
        if hasattr(self._app, "settings"):
            self._app.settings.set("sys_mute", False)

    def is_mic_muted(self) -> bool:
        if hasattr(self._app, "settings"):
            return bool(self._app.settings.get("mic_mute", False))
        return False


# ── Keyboard API ──────────────────────────────────────────────────────────────

class _KeyboardAPI:
    """homrec.keyboard — register hotkeys. Requires permission: keyboard"""
    def __init__(self, app_ref):
        self._app = app_ref
        self._callbacks: list = []

    def on_key(self, callback) -> None:
        """callback(key_name: str) called on every keypress."""
        self._callbacks.append(callback)
        if hasattr(self._app, "_keyboard_listeners"):
            self._app._keyboard_listeners.append(callback)

    def register_hotkey(self, key: str, callback) -> None:
        """
        Register a named hotkey (e.g. 'ctrl+shift+r').
        callback() called when the hotkey fires.
        """
        if hasattr(self._app, "root"):
            try:
                self._app.root.bind(f"<{key}>", lambda e: callback())
            except Exception as e:
                log.warning(f"register_hotkey {key}: {e}")

    def unregister_hotkey(self, key: str) -> None:
        if hasattr(self._app, "root"):
            try:
                self._app.root.unbind(f"<{key}>")
            except Exception:
                pass


# ── Filesystem API ────────────────────────────────────────────────────────────

class _FilesystemAPI:
    """
    homrec.filesystem — sandboxed file I/O inside plugin directory.
    Requires permission: filesystem
    Also exposes inter-plugin file access when both plugins agree.
    """
    def __init__(self, manifest):
        self._base = manifest.plugin_dir

    def _safe_path(self, filename: str) -> str:
        import os
        path = os.path.join(self._base, filename)
        if not os.path.abspath(path).startswith(os.path.abspath(self._base)):
            raise PermissionError("Access outside plugin directory denied")
        return path

    def read(self, filename: str) -> str | None:
        try:
            with open(self._safe_path(filename), "r", encoding="utf-8") as f:
                return f.read()
        except Exception:
            return None

    def read_bytes(self, filename: str) -> bytes | None:
        try:
            with open(self._safe_path(filename), "rb") as f:
                return f.read()
        except Exception:
            return None

    def write(self, filename: str, content: str) -> bool:
        import os
        try:
            path = self._safe_path(filename)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(content)
            return True
        except Exception as e:
            log.warning(f"filesystem.write {filename}: {e}")
            return False

    def write_bytes(self, filename: str, data: bytes) -> bool:
        import os
        try:
            path = self._safe_path(filename)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
            return True
        except Exception as e:
            log.warning(f"filesystem.write_bytes {filename}: {e}")
            return False

    def exists(self, filename: str) -> bool:
        import os
        try:
            return os.path.exists(self._safe_path(filename))
        except Exception:
            return False

    def delete(self, filename: str) -> bool:
        import os
        try:
            path = self._safe_path(filename)
            if os.path.isfile(path):
                os.remove(path)
                return True
            return False
        except Exception:
            return False

    def list(self, subdir: str = "") -> list[str]:
        import os
        try:
            d = self._safe_path(subdir) if subdir else self._base
            return os.listdir(d)
        except Exception:
            return []

    def mkdir(self, dirname: str) -> bool:
        import os
        try:
            os.makedirs(self._safe_path(dirname), exist_ok=True)
            return True
        except Exception:
            return False

    @property
    def plugin_dir(self) -> str:
        return self._base


# ── Network API ───────────────────────────────────────────────────────────────

class _NetworkAPI:
    """homrec.network — HTTP requests. Requires permission: network"""

    def __init__(self, plugin_name: str):
        self._name = plugin_name

    def get(self, url: str, timeout: int = 10,
            headers: dict | None = None) -> str | None:
        try:
            import urllib.request
            req = urllib.request.Request(url, headers=headers or {})
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read().decode("utf-8", errors="replace")
        except Exception as e:
            log.warning(f"[{self._name}] network.get {url}: {e}")
            return None

    def post(self, url: str, data: str,
             content_type: str = "application/json",
             timeout: int = 10,
             headers: dict | None = None) -> str | None:
        try:
            import urllib.request
            h = {"Content-Type": content_type}
            if headers:
                h.update(headers)
            req = urllib.request.Request(url,
                data=data.encode("utf-8"), headers=h, method="POST")
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read().decode("utf-8", errors="replace")
        except Exception as e:
            log.warning(f"[{self._name}] network.post {url}: {e}")
            return None

    def download_file(self, url: str, dest_path: str, timeout: int = 30) -> bool:
        """Download a file from url to dest_path."""
        try:
            import urllib.request
            urllib.request.urlretrieve(url, dest_path)
            return True
        except Exception as e:
            log.warning(f"[{self._name}] network.download_file {url}: {e}")
            return False


# ── Theme API ─────────────────────────────────────────────────────────────────

class _ThemeAPI:
    """
    homrec.theme — override / read app colour tokens.
    Requires permission: theme
    Conflicts with other theme-modifying plugins are warned.
    """
    _OWNER: dict[str, str] = {}   # color_key → plugin_name

    def __init__(self, app_ref, plugin_name: str):
        self._app  = app_ref
        self._name = plugin_name

    def set_color(self, key: str, value: str) -> None:
        """Override a theme colour token (e.g. 'accent', 'bg', 'success')."""
        owner = _ThemeAPI._OWNER.get(key)
        if owner and owner != self._name:
            _warn_conflict(self._name,
                f"Plugin '{self._name}' is overriding theme key '{key}' "
                f"already owned by '{owner}'.")
        _ThemeAPI._OWNER[key] = self._name

        if hasattr(self._app, "colors"):
            self._app.colors[key] = value
            # Trigger lightweight theme re-apply if available
            if hasattr(self._app, "apply_theme"):
                try:
                    self._app.apply_theme()
                except Exception:
                    pass

    def get_color(self, key: str) -> str | None:
        if hasattr(self._app, "colors"):
            return self._app.colors.get(key)
        return None

    def get_all_colors(self) -> dict:
        return dict(getattr(self._app, "colors", {}))

    def reset_color(self, key: str) -> None:
        """Remove plugin's override of a colour key."""
        _ThemeAPI._OWNER.pop(key, None)
        # Restore from current built-in theme
        if hasattr(self._app, "current_theme") and hasattr(self._app, "BUILTIN_THEMES"):
            orig = self._app.BUILTIN_THEMES.get(
                self._app.current_theme, {}).get(key)
            if orig and hasattr(self._app, "colors"):
                self._app.colors[key] = orig


# ── Inter-Plugin API ──────────────────────────────────────────────────────────

class _InterPluginAPI:
    """
    homrec.inter_plugin — send messages to other plugins, call their exports.
    Requires permission: inter_plugin

    How it works:
      Plugin A registers an export:  inter_plugin.register("do_thing", fn)
      Plugin B calls it:             inter_plugin.call("PluginA", "do_thing", args...)
    """
    _EXPORTS: dict[str, dict[str, Callable]] = {}   # plugin_name → {fn_name → fn}

    def __init__(self, plugin_name: str):
        self._name = plugin_name

    def register(self, fn_name: str, fn: Callable) -> None:
        """Expose a callable for other plugins to call."""
        _InterPluginAPI._EXPORTS.setdefault(self._name, {})[fn_name] = fn
        log.debug(f"inter_plugin.register: {self._name}.{fn_name}")

    def call(self, plugin_name: str, fn_name: str, *args) -> Any:
        """Call a registered function from another plugin."""
        exports = _InterPluginAPI._EXPORTS.get(plugin_name, {})
        fn = exports.get(fn_name)
        if fn is None:
            log.warning(
                f"inter_plugin: {self._name} tried to call "
                f"{plugin_name}.{fn_name} — not found")
            return None
        try:
            return fn(*args)
        except Exception as e:
            log.warning(
                f"inter_plugin: call {plugin_name}.{fn_name} raised: {e}")
            _warn_conflict(self._name,
                f"Error calling {plugin_name}.{fn_name}: {e}")
            return None

    def list_plugins(self) -> list[str]:
        """Return list of plugin names that have registered exports."""
        return list(_InterPluginAPI._EXPORTS.keys())

    def list_exports(self, plugin_name: str) -> list[str]:
        """Return list of exported function names for a plugin."""
        return list(_InterPluginAPI._EXPORTS.get(plugin_name, {}).keys())

    def send_message(self, plugin_name: str, message: str, data: Any = None) -> None:
        """
        Fire the on_message(sender, message, data) hook on a target plugin's sandbox.
        Handled via the events system.
        """
        from plugin_engine import loader as _loader
        # Walk sandboxes looking for target
        for _name, sandbox in _loader._GLOBAL_SANDBOXES.items():
            if _name.lower() == plugin_name.lower():
                try:
                    sandbox.call("on_message", self._name, message, data)
                except Exception as e:
                    log.debug(f"inter_plugin.send_message {plugin_name}: {e}")
                return
        log.warning(
            f"inter_plugin.send_message: target '{plugin_name}' not found")

    def broadcast(self, message: str, data: Any = None) -> None:
        """Broadcast a message to all plugins."""
        from plugin_engine import loader as _loader
        for _name, sandbox in _loader._GLOBAL_SANDBOXES.items():
            if _name != self._name:
                try:
                    sandbox.call("on_message", self._name, message, data)
                except Exception as e:
                    log.debug(f"inter_plugin.broadcast to {_name}: {e}")


# ── System API ────────────────────────────────────────────────────────────────

class _SystemAPI:
    """
    homrec.system — read-only OS information.
    Requires permission: system
    """
    def platform(self) -> str:
        import platform
        return platform.system()

    def cpu_count(self) -> int:
        try:
            import psutil
            return psutil.cpu_count()
        except ImportError:
            import os
            return os.cpu_count() or 1

    def cpu_percent(self) -> float:
        try:
            import psutil
            return psutil.cpu_percent(interval=0.1)
        except ImportError:
            return 0.0

    def ram_total_gb(self) -> float:
        try:
            import psutil
            return psutil.virtual_memory().total / 1024**3
        except ImportError:
            return 0.0

    def ram_used_percent(self) -> float:
        try:
            import psutil
            return psutil.virtual_memory().percent
        except ImportError:
            return 0.0

    def hostname(self) -> str:
        import platform
        return platform.node()

    def python_version(self) -> str:
        import sys
        return sys.version


# ── Permission map ────────────────────────────────────────────────────────────
_PERM_MAP = {
    "ui":           "ui",
    "overlay":      "overlay",
    "audio":        "audio",
    "keyboard":     "keyboard",
    "filesystem":   "filesystem",
    "network":      "network",
    "theme":        "theme",
    "inter_plugin": "inter_plugin",
    "system":       "system",
    "log":          None,
    "storage":      None,
    "events":       None,
}