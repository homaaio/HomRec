"""
plugin_engine/loader.py — HomRec .hrp plugin loader

Changes vs previous:
  - _GLOBAL_SANDBOXES registry for inter-plugin API
  - uninstall() properly removes folder + loader state
  - conflict detection: duplicate plugin name raises warning dialog
  - icon_plugin.png named convention documented
"""

import os
import io
import gzip
import json
import zipfile
import logging
import shutil
from typing import Any

log = logging.getLogger("homrec.plugins")

HRP_MAGIC   = b"HRP\x01"
PLUGINS_DIR = "plugins"

REQUIRED_MANIFEST_KEYS = ["name", "version", "entry"]

# Global sandbox registry — used by inter_plugin API
_GLOBAL_SANDBOXES: dict[str, Any] = {}


class PluginManifest:
    def __init__(self, data: dict, plugin_dir: str):
        self.name        = data.get("name", "Unknown")
        self.version     = data.get("version", "0.0.0")
        self.author      = data.get("author", "")
        self.description = data.get("description", "")
        self.entry       = data.get("entry", "main.lua")
        self.min_version = data.get("homrec_min_version", "2.0.0")
        self.permissions = set(data.get("permissions", []))
        self.plugin_dir  = plugin_dir

    @property
    def entry_path(self) -> str:
        return os.path.join(self.plugin_dir, self.entry)

    @property
    def icon_path(self) -> str | None:
        """icon_plugin.png is the standard name for plugin icons."""
        for name in ("icon_plugin.png", "icon.png"):
            p = os.path.join(self.plugin_dir, name)
            if os.path.exists(p):
                return p
        return None

    def __repr__(self) -> str:
        return f"<Plugin {self.name} v{self.version}>"


class PluginLoader:
    def __init__(self, settings):
        self.settings  = settings
        self.plugins:   list[PluginManifest] = []
        self._sandboxes: dict[str, Any] = {}

    def scan(self) -> None:
        """
        Scan plugins/ and load ONLY already-extracted plugin folders.

        Raw .hrp files sitting in the plugins/ folder are intentionally
        NOT auto-installed here — installation is always an explicit user
        action (Library UI or the runtime poller while the app is running).

        Consequence: if the user deletes a plugin folder, it stays deleted.
        A leftover .hrp file does NOT resurrect the plugin on next launch.
        """
        base        = os.path.dirname(os.path.abspath(__file__))
        plugins_dir = os.path.join(os.path.dirname(base), PLUGINS_DIR)
        os.makedirs(plugins_dir, exist_ok=True)

        self.plugins = []

        for entry in os.listdir(plugins_dir):
            entry_path    = os.path.join(plugins_dir, entry)
            manifest_path = os.path.join(entry_path, "plugin.json")

            # Only directories that contain plugin.json
            if not os.path.isdir(entry_path) or not os.path.exists(manifest_path):
                continue

            try:
                with open(manifest_path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                manifest = PluginManifest(data, entry_path)
                self._add_plugin(manifest)
                log.info(f"Plugin found: {manifest}")
            except Exception as e:
                log.warning(f"Failed to read manifest in '{entry}': {e}")

        log.info(f"Total plugins loaded: {len(self.plugins)}")

    def _add_plugin(self, manifest: PluginManifest) -> None:
        """Add plugin with conflict check."""
        existing = next((p for p in self.plugins if p.name == manifest.name), None)
        if existing:
            log.warning(
                f"Plugin conflict: '{manifest.name}' already loaded from "
                f"{existing.plugin_dir}. Ignoring duplicate from {manifest.plugin_dir}.")
            # Surface warning via messagebox if possible
            try:
                import tkinter.messagebox as _mb
                _mb.showwarning("Plugin Conflict",
                    f"Plugin '{manifest.name}' is installed twice.\n"
                    f"Using version {existing.version} from:\n  {existing.plugin_dir}\n\n"
                    f"Duplicate at:\n  {manifest.plugin_dir}\nwas ignored.")
            except Exception:
                pass
            return
        self.plugins.append(manifest)
        log.info(f"Plugin loaded: {manifest}")

    def activate(self, app_ref) -> None:
        """Create Lua sandbox for each plugin and call on_load()."""
        global _GLOBAL_SANDBOXES
        log.info(f"Activating plugins: {len(self.plugins)}")
        try:
            from plugin_engine.api import build_api
            from plugin_engine.sandbox import LuaSandbox
        except Exception as e:
            log.warning(f"Plugin activation unavailable: {e}")
            return

        self._sandboxes = {}
        _GLOBAL_SANDBOXES = {}

        for manifest in self.plugins:
            try:
                api = build_api(manifest, app_ref)
                sb  = LuaSandbox(manifest, api)
                if sb.ok:
                    self._sandboxes[manifest.name]   = sb
                    _GLOBAL_SANDBOXES[manifest.name] = sb
                    sb.call("on_load")
                    log.info(f"Plugin activated: {manifest.name}")
                else:
                    log.warning(f"Plugin sandbox not ok: {manifest.name}")
                    self._bundled_fallback(manifest, app_ref)
            except Exception as e:
                log.warning(f"Plugin activate failed: {manifest.name}: {e}")

    def _bundled_fallback(self, manifest: PluginManifest, app_ref) -> None:
        """
        Python fallback for plugins whose Lua sandbox could not start.

        Rules:
          - "folders"        → NO menu entry. Folders has a dedicated UI button.
          - "plugin_console" → NO menu entry. Console lives inside the Library.
          - "advanced_settings" → adds "Advanced Settings…" to Settings menu,
                                  tagged with plugin name for clean removal.
          - anything else    → logged, no UI added.
        """
        name_lower = manifest.name.lower().replace(" ", "_")
        try:
            if name_lower in ("folders", "plugin_console"):
                log.info(f"Bundled fallback: '{manifest.name}' skipped (dedicated UI exists)")
                return

            if name_lower in ("advanced_settings", "advancedsettings"):
                label = "Advanced Settings…"
                if hasattr(app_ref, "_apply_plugin_menu_item"):
                    app_ref._apply_plugin_menu_item(
                        "Settings", label,
                        lambda: app_ref._open_advanced_settings())
                # Tag the entry so _rebuild_plugin_menus can remove it
                if not hasattr(app_ref, "_plugin_menu_items"):
                    app_ref._plugin_menu_items = []
                app_ref._plugin_menu_items = [
                    i for i in app_ref._plugin_menu_items
                    if not (isinstance(i, dict) and i.get("label") == label)]
                app_ref._plugin_menu_items.append({
                    "plugin": manifest.name,
                    "menu":   "Settings",
                    "label":  label,
                    "cb":     lambda: app_ref._open_advanced_settings(),
                })
                log.info("Bundled fallback activated: Advanced Settings")
                return

            log.debug(f"Bundled fallback: no action for '{manifest.name}'")

        except Exception as e:
            log.warning(f"Bundled fallback failed for {manifest.name}: {e}")

    def install(self, hrp_path: str,
                progress_cb=None) -> PluginManifest | None:
        """
        Install a .hrp file, handling conflicts gracefully.
        progress_cb(pct: int, msg: str) — optional progress callback.
        """
        def _p(pct, msg):
            if progress_cb:
                try:
                    progress_cb(pct, msg)
                except Exception:
                    pass

        base        = os.path.dirname(os.path.abspath(__file__))
        plugins_dir = os.path.join(os.path.dirname(base), PLUGINS_DIR)
        os.makedirs(plugins_dir, exist_ok=True)

        _p(20, "Extracting archive…")
        manifest = self._install_hrp(hrp_path, plugins_dir, progress_cb=_p)
        _p(90, "Registering plugin…")
        if manifest:
            self.plugins = [p for p in self.plugins if p.name != manifest.name]
            self.plugins.append(manifest)
        _p(100, "Done!")
        return manifest

    def uninstall(self, name: str) -> bool:
        """
        Remove plugin by name.

        Deletes:
          1. The extracted plugin folder  (plugins/<name>/)
          2. Any matching .hrp file(s) in plugins/ — so the plugin
             cannot resurrect itself on the next scan/startup.
        Then deregisters the plugin from all in-memory structures.
        """
        global _GLOBAL_SANDBOXES
        plugin = next((p for p in self.plugins if p.name == name), None)
        if not plugin:
            log.warning(f"uninstall: '{name}' not found in plugin list")
            return False

        base        = os.path.dirname(os.path.abspath(__file__))
        plugins_dir = os.path.join(os.path.dirname(base), PLUGINS_DIR)

        try:
            # 1. Delete the extracted folder
            if os.path.isdir(plugin.plugin_dir):
                shutil.rmtree(plugin.plugin_dir, ignore_errors=False)
                log.info(f"Plugin folder removed: {plugin.plugin_dir}")

            # 2. Delete any .hrp files in plugins/ whose name matches
            #    (could be <name>.hrp or <sanitised_name>.hrp)
            safe_name = name.lower().replace(" ", "_")
            for fname in os.listdir(plugins_dir):
                if not fname.endswith(".hrp"):
                    continue
                stem = fname[:-4].lower().replace(" ", "_")
                if stem == safe_name:
                    try:
                        os.remove(os.path.join(plugins_dir, fname))
                        log.info(f"Deleted leftover .hrp: {fname}")
                    except OSError as e:
                        log.warning(f"Could not delete {fname}: {e}")

            # 3. Deregister from memory
            self.plugins = [p for p in self.plugins if p.name != name]
            self._sandboxes.pop(name, None)
            _GLOBAL_SANDBOXES.pop(name, None)
            log.info(f"Plugin uninstalled: {name}")
            return True

        except PermissionError as e:
            log.warning(f"uninstall {name}: permission denied — {e}")
            # Still remove from in-memory lists so the UI reflects reality
            self.plugins = [p for p in self.plugins if p.name != name]
            self._sandboxes.pop(name, None)
            _GLOBAL_SANDBOXES.pop(name, None)
            return False
        except Exception as e:
            log.warning(f"Failed to uninstall {name}: {e}")
            return False

    def call_hook(self, hook: str, *args) -> None:
        for name, sandbox in self._sandboxes.items():
            try:
                sandbox.call(hook, *args)
            except Exception as e:
                log.debug(f"Plugin {name} hook {hook} error: {e}")

    # -- Internal --------------------------------------------------------------

    def _install_hrp(self, hrp_path: str, plugins_dir: str, progress_cb=None) -> PluginManifest | None:
        """Extract .hrp into plugins/<name>/ and return manifest."""
        with open(hrp_path, "rb") as f:
            magic = f.read(4)
            body  = f.read()

        if magic != HRP_MAGIC:
            raise ValueError(f"Not a HomRec plugin (magic={magic!r})")

        zip_data = gzip.decompress(body)
        with zipfile.ZipFile(io.BytesIO(zip_data)) as zf:
            if "plugin.json" not in zf.namelist():
                raise ValueError("plugin.json not found in .hrp")
            data = json.loads(zf.read("plugin.json").decode("utf-8"))

            missing = [k for k in REQUIRED_MANIFEST_KEYS if k not in data]
            if missing:
                raise ValueError(f"plugin.json missing keys: {missing}")

            plugin_name = data["name"].lower().replace(" ", "_")
            plugin_dir  = os.path.join(plugins_dir, plugin_name)
            os.makedirs(plugin_dir, exist_ok=True)
            names = zf.namelist()
            total = len(names)
            for i, member in enumerate(names):
                zf.extract(member, plugin_dir)
                if progress_cb and total > 0:
                    pct = 20 + int((i + 1) / total * 65)
                    try:
                        progress_cb(pct, f"Extracting {member}…")
                    except Exception:
                        pass

        manifest = PluginManifest(data, plugin_dir)
        log.info(f"Plugin extracted: {manifest} → {plugin_dir}")
        return manifest


def write_hrp(output_path: str, source_dir: str) -> None:
    """Pack a plugin folder into a .hrp file (for developers)."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for root, _, files in os.walk(source_dir):
            for fname in files:
                fpath   = os.path.join(root, fname)
                arcname = os.path.relpath(fpath, source_dir)
                zf.write(fpath, arcname)
    compressed = gzip.compress(buf.getvalue())
    with open(output_path, "wb") as f:
        f.write(HRP_MAGIC)
        f.write(compressed)
    log.info(f"Plugin packed: {output_path} ({len(compressed):,} bytes)")