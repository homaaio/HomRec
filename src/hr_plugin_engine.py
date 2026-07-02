from __future__ import annotations
import os
import sys
import json
import zipfile
import importlib.util
import traceback
import logging
import threading
import tkinter as tk
from tkinter import messagebox, filedialog
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from homrec import HomRecScreen

log = logging.getLogger("homrec.plugins")

PLUGINS_DIR = "plugins"
PLUGIN_EXTENSIONS = {".hrp", ".jar", ".zip"}


#-----------------------------------------------------------
#  Base class — every plugin extends this
#-----------------------------------------------------------

class HomRecPluginBase:
    # Заполняется движком
    app: "HomRecScreen" = None
    meta: dict = {}
    engine: "PluginEngine" = None

    # lifecycle ------------------------------------------
    def on_load(self) -> None: pass
    def on_unload(self) -> None: pass

    # recording hooks -----------------------------------
    def on_recording_start(self) -> None: pass
    def on_recording_stop(self) -> None: pass

    # streaming hooks -----------------------------------
    def on_stream_start(self) -> None: pass
    def on_stream_stop(self) -> None: pass

    # ui hooks ------------------------------------------
    def on_theme_change(self, colors: dict) -> None: pass
    def on_frame(self, frame) -> None: pass
    def on_menu_build(self, menubar: tk.Menu) -> None: pass
    def on_settings_build(self, frame: tk.Frame) -> None: pass

    # helpers (используй freely в плагине) -------------
    def get_app(self) -> "HomRecScreen":
        """Прямой доступ к главному приложению."""
        return self.app

    def get_root(self) -> tk.Tk:
        """Tkinter корневое окно."""
        return self.app.root

    def get_colors(self) -> dict:
        """Текущие цвета темы."""
        return self.app.colors

    def get_ffmpeg(self) -> str | None:
        """Путь к ffmpeg."""
        return self.app.ffmpeg_path

    def emit(self, event: str, *args, **kwargs) -> None:
        """Вызвать событие для всех других плагинов."""
        self.engine.emit(event, *args, **kwargs)

    def on_custom_event(self, event: str, *args, **kwargs) -> None:
        """Получить кастомное событие от другого плагина."""
        pass

    def show_toast(self, message: str, color: str | None = None, duration: int = 3000) -> None:
        """Показать всплывающее уведомление в правом нижнем углу."""
        try:
            c = color or self.get_colors().get("accent", "#89b4fa")
            self.engine._show_toast(message, c, duration)
        except Exception:
            pass

    def store_set(self, key: str, value) -> None:
        """Сохранить значение в persistent storage плагина."""
        self.engine._plugin_store_set(self.meta.get("id", "?"), key, value)

    def store_get(self, key: str, default=None):
        """Получить значение из persistent storage плагина."""
        return self.engine._plugin_store_get(self.meta.get("id", "?"), key, default)


#-----------------------------------------------------------
#  Plugin Engine
#-----------------------------------------------------------

class PluginEngine:
    def __init__(self, app: "HomRecScreen") -> None:
        self.app = app
        self.plugins: dict[str, HomRecPluginBase] = {}   # id → instance
        self._plugin_paths: dict[str, str] = {}           # id → source .hrp/.jar/.zip path
        self._store: dict[str, dict] = {}                # id → {key: val}
        self._plugins_dir = self._resolve_plugins_dir()
        os.makedirs(self._plugins_dir, exist_ok=True)
        self._plugins_menu_ref: tk.Menu | None = None
        self._plugins_menubar_ref: tk.Menu | None = None
        self._patch_app()
        log.info(f"PluginEngine init, plugins dir: {self._plugins_dir}")

    def _resolve_plugins_dir(self) -> str:
        """Resolve the plugins directory at the APP ROOT, not next to this
        module.

        BUG FIX: this used to be computed from __file__ (i.e. the folder
        hr_plugin_engine.py itself lives in — src/ during development),
        so the engine silently created and used src/plugins/ instead of
        the project's real plugins/ folder at the app root. That's why
        installed plugins ended up in src/plugins and why deleting them
        via the app appeared to do nothing — the app was reading/writing
        a different folder than the one being inspected.

        homrec.py already computes the correct root-relative path and
        assigns it to app._plugins_dir right before constructing this
        engine — use that when present. Otherwise fall back to the same
        frozen-aware root-detection homrec.py's _get_root_dir() uses.
        """
        configured = getattr(self.app, "_plugins_dir", None)
        if configured:
            return configured

        if getattr(sys, "frozen", False):
            root = os.path.dirname(os.path.abspath(sys.executable))
        else:
            _src = os.path.dirname(os.path.abspath(__file__))
            _parent = os.path.dirname(_src)
            if os.path.isdir(os.path.join(_parent, "src")) or os.path.basename(_src).lower() == "src":
                root = _parent
            else:
                root = _src
        return os.path.join(root, PLUGINS_DIR)

    # patch app with plugin hooks -----------------------

    def _patch_app(self) -> None:
        """Патчим методы app, чтобы вызывать хуки плагинов."""
        app = self.app
        engine = self

        # start_recording
        _orig_start = app.start_recording
        def _start_recording_patched():
            _orig_start()
            engine.emit_hook("on_recording_start")
        app.start_recording = _start_recording_patched

        # stop_recording
        _orig_stop = app.stop_recording
        def _stop_recording_patched():
            _orig_stop()
            engine.emit_hook("on_recording_stop")
        app.stop_recording = _stop_recording_patched

        # change_theme
        _orig_theme = app.change_theme
        def _change_theme_patched(theme: str):
            _orig_theme(theme)
            engine.emit_hook("on_theme_change", app.colors)
        app.change_theme = _change_theme_patched

        # recreate_widgets — rebuild plugin menu items
        _orig_recreate = app.recreate_widgets
        def _recreate_patched():
            # The menubar is destroyed and rebuilt from scratch inside
            # create_menu() during recreate_widgets(), so our previous
            # menu reference is now invalid — drop it before re-adding.
            engine._plugins_menu_ref = None
            engine._plugins_menubar_ref = None
            _orig_recreate()
            engine._rebuild_plugin_menu()
        app.recreate_widgets = _recreate_patched

        # Inject plugin menu after app fully built
        app.root.after(300, self._rebuild_plugin_menu)

    # loading ----------------------------------------------

    def load_all(self) -> None:
        """Загрузить все плагины из папки plugins/."""
        for fname in os.listdir(self._plugins_dir):
            ext = os.path.splitext(fname)[1].lower()
            if ext in PLUGIN_EXTENSIONS:
                path = os.path.join(self._plugins_dir, fname)
                self.load_plugin(path)

    def load_plugin(self, path: str) -> bool:
        """Загрузить один плагин из .hrp/.jar/.zip файла."""
        try:
            if not zipfile.is_zipfile(path):
                log.warning(f"Not a valid plugin archive: {path}")
                return False

            with zipfile.ZipFile(path, "r") as zf:
                # Read manifest
                if "plugin.json" not in zf.namelist():
                    log.warning(f"No plugin.json in {path}")
                    return False
                meta = json.loads(zf.read("plugin.json").decode("utf-8"))

                pid = meta.get("id", "")
                if not pid:
                    log.warning(f"plugin.json missing 'id' in {path}")
                    return False

                if pid in self.plugins:
                    log.info(f"Plugin {pid} already loaded, skipping")
                    return True

                # Extract to temp folder
                import tempfile
                tmp_dir = os.path.join(tempfile.gettempdir(), "homrec_plugins", pid)
                os.makedirs(tmp_dir, exist_ok=True)
                zf.extractall(tmp_dir)

            # Load entry point
            entry = meta.get("entry", "main.py")
            entry_path = os.path.join(tmp_dir, entry)
            if not os.path.exists(entry_path):
                log.warning(f"Entry point {entry} not found in {path}")
                return False

            spec = importlib.util.spec_from_file_location(f"hrplugin_{pid}", entry_path)
            mod = importlib.util.module_from_spec(spec)
            # Give the module full access
            mod.app = self.app
            mod.engine = self
            mod.tk = tk
            mod.log = logging.getLogger(f"homrec.plugin.{pid}")
            sys.modules[f"hrplugin_{pid}"] = mod
            spec.loader.exec_module(mod)

            if not hasattr(mod, "Plugin"):
                log.warning(f"No Plugin class in {entry_path}")
                return False

            # Instantiate
            instance: HomRecPluginBase = mod.Plugin()
            instance.app = self.app
            instance.meta = meta
            instance.engine = self

            self.plugins[pid] = instance
            self._plugin_paths[pid] = path
            self._store.setdefault(pid, {})

            instance.on_load()
            log.info(f"✅ Plugin loaded: {meta.get('name', pid)} v{meta.get('version','?')}")
            return True

        except Exception as e:
            log.error(f"Failed to load plugin {path}: {e}\n{traceback.format_exc()}")
            return False

    def unload_plugin(self, pid: str) -> None:
        if pid in self.plugins:
            try:
                self.plugins[pid].on_unload()
            except Exception as e:
                log.warning(f"on_unload error for {pid}: {e}")
            del self.plugins[pid]
            log.info(f"Plugin unloaded: {pid}")

    def uninstall_plugin(self, pid: str) -> bool:
        """Fully remove a plugin: unload it, delete its .hrp/.jar/.zip from
        plugins/, and clean up its extracted temp copy.

        BUG FIX: previously there was no way to actually remove an installed
        plugin from disk — unload_plugin() only dropped it from memory, so
        the file stayed in plugins/ and got reloaded again on next launch.
        This is the method that should back the "Delete"/"Uninstall" UI action.
        """
        self.unload_plugin(pid)

        ok = True
        src_path = self._plugin_paths.pop(pid, None)
        if src_path and os.path.exists(src_path):
            try:
                os.remove(src_path)
                log.info(f"Deleted plugin file: {src_path}")
            except Exception as e:
                log.warning(f"Failed to delete plugin file {src_path}: {e}")
                ok = False
        else:
            log.warning(f"uninstall_plugin: no known source path for {pid}")
            ok = False

        # Clean up the extracted copy in %TEMP%/homrec_plugins/<pid>/
        try:
            import tempfile, shutil
            tmp_dir = os.path.join(tempfile.gettempdir(), "homrec_plugins", pid)
            if os.path.isdir(tmp_dir):
                shutil.rmtree(tmp_dir, ignore_errors=True)
        except Exception as e:
            log.warning(f"Failed to clean temp dir for {pid}: {e}")

        return ok

    def install_plugin(self, path: str) -> bool:
        """Скопировать .hrp/.jar/.zip в plugins/ и загрузить."""
        import shutil
        fname = os.path.basename(path)
        dest = os.path.join(self._plugins_dir, fname)
        shutil.copy2(path, dest)
        return self.load_plugin(dest)

    # event bus -----------------------------------------

    def emit_hook(self, hook: str, *args, **kwargs) -> None:
        """Вызвать хук у всех загруженных плагинов."""
        for pid, plugin in list(self.plugins.items()):
            try:
                fn = getattr(plugin, hook, None)
                if callable(fn):
                    fn(*args, **kwargs)
            except Exception as e:
                log.warning(f"Plugin {pid} hook {hook} error: {e}")

    def emit(self, event: str, *args, **kwargs) -> None:
        for pid, plugin in list(self.plugins.items()):
            try:
                plugin.on_custom_event(event, *args, **kwargs)
            except Exception as e:
                log.warning(f"Plugin {pid} custom event {event} error: {e}")

    # menu ----------------------------------------------

    def _rebuild_plugin_menu(self) -> None:
        try:
            menubar = self.app.root.nametowidget(self.app.root["menu"])
            c = self.app.colors

            # If we previously added a Plugins cascade to THIS SAME menubar,
            # remove it directly via the tracked reference before re-adding.
            if self._plugins_menubar_ref is menubar and self._plugins_menu_ref is not None:
                try:
                    self._plugins_menu_ref.destroy()
                except Exception:
                    pass
                # Also try to drop the cascade entry itself by scanning
                # menu entries for one whose submenu is our tracked widget
                # (more reliable than label string matching).
                try:
                    last = menubar.index("end")
                    if last is not None:
                        for i in range(last, -1, -1):
                            try:
                                if menubar.type(i) == "cascade":
                                    sub_name = menubar.entrycget(i, "menu")
                                    if sub_name == str(self._plugins_menu_ref):
                                        menubar.delete(i)
                            except Exception:
                                continue
                except Exception:
                    pass

            plugin_menu = tk.Menu(menubar, tearoff=0, bg=c["surface"], fg=c["fg"])
            menubar.add_cascade(label="Plugins", menu=plugin_menu)
            self._plugins_menu_ref = plugin_menu
            self._plugins_menubar_ref = menubar

            plugin_menu.add_command(label="📂 Install Plugin…", command=self._open_install_dialog)
            plugin_menu.add_command(label="🗂 Manage Plugins…", command=self._open_manager)
            plugin_menu.add_separator()

            if self.plugins:
                for pid, p in self.plugins.items():
                    name = p.meta.get("name", pid)
                    plugin_menu.add_command(
                        label=f"⚙ {name}",
                        command=lambda pp=p: self._open_plugin_settings(pp)
                    )
            else:
                plugin_menu.add_command(label="(no plugins loaded)", state="disabled")

            # Let plugins add their own menu items
            self.emit_hook("on_menu_build", menubar)

        except Exception as e:
            log.warning(f"_rebuild_plugin_menu: {e}")

    # dialogs -----------------------------------------

    def _open_install_dialog(self) -> None:
        path = filedialog.askopenfilename(
            title="Install HomRec Plugin",
            filetypes=[
                ("HomRec Plugin", "*.hrp *.jar *.zip"),
                ("All files", "*.*"),
            ]
        )
        if path:
            ok = self.install_plugin(path)
            self._rebuild_plugin_menu()
            if ok:
                messagebox.showinfo("Plugin installed",
                    f"Plugin installed successfully!\nRestart may be needed for full effect.")
            else:
                messagebox.showerror("Plugin error",
                    "Failed to install plugin. Check homrec.log for details.")

    def _open_manager(self) -> None:
        PluginManagerWindow(self.app.root, self)

    def _open_plugin_settings(self, plugin: HomRecPluginBase) -> None:
        """Открыть окно настроек конкретного плагина."""
        c = self.app.colors
        dlg = tk.Toplevel(self.app.root)
        dlg.title(f"⚙ {plugin.meta.get('name','Plugin')} Settings")
        dlg.geometry("500x400")
        dlg.configure(bg=c["bg"])
        dlg.transient(self.app.root)

        tk.Label(dlg, text=plugin.meta.get("name","Plugin"),
                 bg=c["bg"], fg=c["accent"],
                 font=("Segoe UI", 14, "bold")).pack(pady=(16,2))
        tk.Label(dlg, text=f"v{plugin.meta.get('version','?')} by {plugin.meta.get('author','?')}",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI", 9)).pack()
        tk.Label(dlg, text=plugin.meta.get("description",""),
                 bg=c["bg"], fg=c["fg"],
                 font=("Segoe UI", 10), wraplength=440, justify="left").pack(pady=(8,4), padx=16)
        tk.Frame(dlg, bg=c["surface_light"], height=1).pack(fill="x", padx=16, pady=8)

        content = tk.Frame(dlg, bg=c["bg"])
        content.pack(fill="both", expand=True, padx=16)
        try:
            plugin.on_settings_build(content)
        except Exception as e:
            tk.Label(content, text=f"(no settings / error: {e})",
                     bg=c["bg"], fg=c["text_secondary"],
                     font=("Segoe UI", 9)).pack()

        btn_row = tk.Frame(dlg, bg=c["bg"])
        btn_row.pack(fill="x", padx=16, pady=12)
        tk.Button(btn_row, text="Uninstall Plugin",
                  command=lambda: self._uninstall_from_settings(plugin, dlg),
                  bg=c["error"], fg=c["bg"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=5,
                  cursor="hand2").pack(side="left")
        tk.Button(btn_row, text="Close", command=dlg.destroy,
                  bg=c["surface_light"], fg=c["fg"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=5,
                  cursor="hand2").pack(side="right")

    def _uninstall_from_settings(self, plugin: HomRecPluginBase, dlg: tk.Toplevel) -> None:
        name = plugin.meta.get("name", plugin.meta.get("id", "?"))
        if not messagebox.askyesno(
            "Uninstall Plugin",
            f"Uninstall '{name}'?\n\nThis removes it from memory AND deletes "
            "its plugin file from disk. This cannot be undone.",
            parent=dlg,
        ):
            return
        ok = self.uninstall_plugin(plugin.meta["id"])
        self._rebuild_plugin_menu()
        dlg.destroy()
        if not ok:
            messagebox.showwarning(
                "Uninstall",
                f"'{name}' was unloaded, but its file could not be found/deleted "
                "automatically. Check homrec.log for details.",
            )

    # toast notification --------------------------------

    def _show_toast(self, message: str, color: str = "#89b4fa", duration: int = 3000) -> None:
        try:
            root = self.app.root
            toast = tk.Toplevel(root)
            toast.overrideredirect(True)
            toast.attributes("-topmost", True)
            toast.configure(bg=color)
            tk.Label(toast, text=message, bg=color, fg="#1e1e2e",
                     font=("Segoe UI", 10, "bold"), padx=16, pady=10).pack()
            toast.update_idletasks()
            sw = root.winfo_screenwidth(); sh = root.winfo_screenheight()
            w = toast.winfo_width(); h = toast.winfo_height()
            toast.geometry(f"+{sw-w-20}+{sh-h-60}")
            root.after(duration, toast.destroy)
        except Exception:
            pass

    # persistent storage --------------------------------

    def _plugin_store_set(self, pid: str, key: str, value) -> None:
        self._store.setdefault(pid, {})[key] = value
        self._save_store()

    def _plugin_store_get(self, pid: str, key: str, default=None):
        return self._store.get(pid, {}).get(key, default)

    def _store_path(self) -> str:
        return os.path.join(self._plugins_dir, "plugin_store.json")

    def _save_store(self) -> None:
        try:
            with open(self._store_path(), "w", encoding="utf-8") as f:
                json.dump(self._store, f, indent=2, ensure_ascii=False)
        except Exception as e:
            log.warning(f"Plugin store save failed: {e}")

    def _load_store(self) -> None:
        try:
            if os.path.exists(self._store_path()):
                with open(self._store_path(), "r", encoding="utf-8") as f:
                    self._store = json.load(f)
        except Exception as e:
            log.warning(f"Plugin store load failed: {e}")


#-----------------------------------------------------------
#  Plugin Manager Window
#-----------------------------------------------------------

class PluginManagerWindow:
    def __init__(self, parent: tk.Tk, engine: PluginEngine) -> None:
        self.engine = engine
        self.app = engine.app
        c = self.app.colors

        self.win = tk.Toplevel(parent)
        self.win.title("🧩 Plugin Manager")
        self.win.geometry("640x480")
        self.win.configure(bg=c["bg"])
        self.win.transient(parent)
        self.win.resizable(True, True)
        self._build_ui()

    def _build_ui(self) -> None:
        c = self.app.colors

        hdr = tk.Frame(self.win, bg=c["surface"], pady=12)
        hdr.pack(fill="x")
        tk.Label(hdr, text="🧩  Plugin Manager",
                 bg=c["surface"], fg=c["accent"],
                 font=("Segoe UI", 13, "bold")).pack(side="left", padx=16)
        tk.Label(hdr, text=f"{len(self.engine.plugins)} loaded",
                 bg=c["surface"], fg=c["text_secondary"],
                 font=("Segoe UI", 9)).pack(side="left")
        tk.Frame(self.win, bg=c["accent"], height=2).pack(fill="x")

        # Plugin list
        list_frame = tk.Frame(self.win, bg=c["bg"])
        list_frame.pack(fill="both", expand=True, padx=12, pady=10)

        self.listbox = tk.Listbox(list_frame,
                                   bg=c["surface"], fg=c["text"],
                                   selectbackground=c["accent"],
                                   font=("Segoe UI", 10),
                                   relief="flat", borderwidth=0,
                                   activestyle="none")
        sb = tk.Scrollbar(list_frame)
        sb.pack(side="right", fill="y")
        self.listbox.pack(side="left", fill="both", expand=True)
        self.listbox.config(yscrollcommand=sb.set)
        sb.config(command=self.listbox.yview)
        self.listbox.bind("<<ListboxSelect>>", self._on_select)

        self._refresh_list()

        # Detail panel
        self.detail = tk.Frame(self.win, bg=c["surface"], height=100)
        self.detail.pack(fill="x", padx=12, pady=(0, 4))
        self.detail.pack_propagate(False)
        self._detail_text = tk.Label(self.detail, text="Select a plugin to see details",
                                      bg=c["surface"], fg=c["text_secondary"],
                                      font=("Segoe UI", 9), justify="left", anchor="w",
                                      wraplength=560)
        self._detail_text.pack(fill="both", expand=True, padx=10, pady=8)

        # Buttons
        btn_row = tk.Frame(self.win, bg=c["bg"])
        btn_row.pack(fill="x", padx=12, pady=8)

        def _btn(text, cmd, bg, fg="#1e1e2e"):
            tk.Button(btn_row, text=text, command=cmd,
                      bg=bg, fg=fg, font=("Segoe UI", 9, "bold"),
                      relief="flat", padx=12, pady=6, cursor="hand2").pack(side="left", padx=(0,6))

        _btn("📂 Install…", self.engine._open_install_dialog, c["accent"])
        _btn("⚙ Settings", self._open_selected_settings, c["surface_light"], c["fg"])
        _btn("🗑 Uninstall", self._uninstall_selected, c["error"])

        tk.Button(btn_row, text="Close", command=self.win.destroy,
                  bg=c["surface"], fg=c["fg"],
                  font=("Segoe UI", 9), relief="flat", padx=12, pady=6,
                  cursor="hand2").pack(side="right")

    def _refresh_list(self) -> None:
        self.listbox.delete(0, tk.END)
        for pid, p in self.engine.plugins.items():
            self.listbox.insert(tk.END,
                f"  {p.meta.get('name', pid)}  v{p.meta.get('version','?')}")

    def _on_select(self, event=None) -> None:
        sel = self.listbox.curselection()
        if not sel:
            return
        pid = list(self.engine.plugins.keys())[sel[0]]
        p = self.engine.plugins[pid]
        info = (
            f"ID: {pid}\n"
            f"Version: {p.meta.get('version','?')}  |  Author: {p.meta.get('author','?')}\n"
            f"{p.meta.get('description','')}"
        )
        self._detail_text.config(text=info)

    def _get_selected_plugin(self) -> HomRecPluginBase | None:
        sel = self.listbox.curselection()
        if not sel:
            return None
        pid = list(self.engine.plugins.keys())[sel[0]]
        return self.engine.plugins.get(pid)

    def _open_selected_settings(self) -> None:
        p = self._get_selected_plugin()
        if p:
            self.engine._open_plugin_settings(p)

    def _uninstall_selected(self) -> None:
        p = self._get_selected_plugin()
        if not p:
            return
        name = p.meta.get("name", p.meta.get("id", "?"))
        if not messagebox.askyesno(
            "Uninstall Plugin",
            f"Uninstall '{name}'?\n\nThis removes it from memory AND deletes "
            "its plugin file from disk. This cannot be undone.",
            parent=self.win,
        ):
            return
        ok = self.engine.uninstall_plugin(p.meta["id"])
        self._refresh_list()
        self.engine._rebuild_plugin_menu()
        if not ok:
            messagebox.showwarning(
                "Uninstall",
                f"'{name}' was unloaded, but its file could not be found/deleted "
                "automatically. Check homrec.log for details.",
                parent=self.win,
            )


def init_plugin_engine(app: "HomRecScreen") -> PluginEngine:
    engine = PluginEngine(app)
    engine._load_store()
    threading.Thread(target=engine.load_all, daemon=True).start()
    return engine
