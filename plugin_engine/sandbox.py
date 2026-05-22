"""
plugin_engine/sandbox.py — Lua sandbox for HomRec plugins

Uses lupa (pip install lupa).
Blocks dangerous Lua builtins, exposes only the HomRec API table.
Supports expanded permissions: ui, overlay, audio, keyboard,
filesystem, network, theme, inter_plugin, system.
"""

import logging
from typing import Any

log = logging.getLogger("homrec.sandbox")

try:
    import lupa
    _LUPA_OK = True
except ImportError:
    _LUPA_OK = False
    log.warning("lupa not installed — Lua plugins disabled.  pip install lupa")


class LuaSandbox:
    """Isolated execution environment for a single Lua plugin."""

    def __init__(self, manifest, api: dict):
        self.manifest = manifest
        self._lua     = None
        self._ok      = False

        if not _LUPA_OK:
            log.warning(f"Plugin {manifest.name}: lupa unavailable")
            return

        try:
            self._lua = lupa.LuaRuntime(unpack_returned_tuples=True)
            self._lockdown()
            self._inject_api(api)
            self._load_entry()
            self._ok = True
            log.info(f"Sandbox ready: {manifest.name}")
        except Exception as e:
            log.warning(f"Sandbox init failed for {manifest.name}: {e}")

    @property
    def ok(self) -> bool:
        return self._ok

    def call(self, func_name: str, *args) -> Any:
        if not self._ok or self._lua is None:
            return None
        try:
            fn = self._lua.globals()[func_name]
            if fn is not None:
                return fn(*args)
        except Exception as e:
            log.debug(f"Plugin {self.manifest.name} call {func_name}: {e}")
        return None

    def eval(self, code: str) -> Any:
        if not self._ok:
            return None
        return self._lua.execute(code)

    # -- Internal --------------------------------------------------------------

    def _lockdown(self) -> None:
        """Remove dangerous Lua globals."""
        dangerous = [
            "os", "io", "require", "dofile", "loadfile",
            "load", "collectgarbage", "debug",
        ]
        for name in dangerous:
            try:
                self._lua.execute(f"{name} = nil")
            except Exception:
                pass

        self._lua.execute("""
            print   = function(...) end   -- use homrec.log instead
            math    = math
            string  = string
            table   = table
            type    = type
            pairs   = pairs
            ipairs  = ipairs
            tostring = tostring
            tonumber = tonumber
            select  = select
            unpack  = table.unpack
            pcall   = pcall
            xpcall  = xpcall
            error   = error
        """)

    def _inject_api(self, api: dict) -> None:
        """Inject the homrec.* table into the Lua runtime."""
        g = self._lua.globals()
        self._lua.execute("homrec = {}")
        homrec = g.homrec
        permissions = self.manifest.permissions

        for key, obj in api.items():
            required_perm = _PERM_MAP.get(key)
            if required_perm and required_perm not in permissions:
                # Inject an inert stub so Lua code won't error on access
                self._lua.execute(f"homrec.{key} = {{}}")
                log.debug(f"Plugin {self.manifest.name}: {key} stubbed "
                          f"(needs permission: {required_perm})")
                continue
            try:
                setattr(homrec, key, obj)
            except Exception as e:
                log.debug(f"inject {key}: {e}")

    def _load_entry(self) -> None:
        path = self.manifest.entry_path
        try:
            with open(path, "r", encoding="utf-8") as f:
                code = f.read()
            self._lua.execute(code)
            log.debug(f"Plugin {self.manifest.name}: entry loaded from {path}")
        except FileNotFoundError:
            raise FileNotFoundError(f"Plugin entry not found: {path}")


# -- Permission → API namespace map --------------------------------------------
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
    # Always allowed — no permission required
    "log":          None,
    "storage":      None,
    "events":       None,
}
