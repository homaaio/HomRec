"""
ui/library.py — Library window for HomRec 2.0

Changes in this version:
  - Console tab REMOVED from notebook — completely hidden from UI
  - Console accessible only via Ctrl+Shift+T  or  Ctrl+Shift+C
    (works anywhere in the app, not just in Library)
  - Console window opens as a standalone dark Toplevel
  - Improved card layout: gradient-accent left border, hover highlight
  - Verified-author badge now shows a checkmark tooltip
  - Plugin count badge in tab label
  - Smoother folder-poll (debounced, only redraws on actual changes)
  - Themes tab: live preview swatch on hover
  - Languages tab: flag emoji + native name
"""

import os
import io
import gzip
import json
import zipfile
import shutil
import logging
import threading
import tkinter as tk
import tkinter.ttk as ttk
import tkinter.messagebox as messagebox
import tkinter.filedialog as filedialog
import urllib.request

log = logging.getLogger("homrec.library")

_base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

VERIFIED_URL   = "https://raw.githubusercontent.com/homaaio/homrec/main/verified_authors.json"
VERIFIED_CACHE: dict = {}

# -- Design tokens --------------------------------------------------------------
C = {
    "bg":        "#090b18",
    "surface":   "#0f1220",
    "surface2":  "#151929",
    "surface3":  "#1a1f38",
    "surface4":  "#1f2540",
    "border":    "#252b48",
    "border2":   "#333b60",
    "accent":    "#5bc8f5",
    "accent2":   "#8b5cf6",
    "accent3":   "#06d6a0",
    "success":   "#00e676",
    "warning":   "#ffca28",
    "error":     "#ff5252",
    "text":      "#e2e8f8",
    "text2":     "#7986cb",
    "text3":     "#4a5470",
    "verified":  "#5bc8f5",
    "plugin_bg": "#0d1022",
    "hover":     "#1c2238",
}

PERM_COLORS = {
    "ui":         "#5bc8f5",
    "overlay":    "#ce93d8",
    "audio":      "#80cbc4",
    "keyboard":   "#ffb74d",
    "filesystem": "#ef9a9a",
    "network":    "#a5d6a7",
}

# -- Console hotkey hint (shown nowhere in main UI) -----------------------------
_CONSOLE_HOTKEY_TIP = "Ctrl+Shift+T  /  Ctrl+Shift+C"


def _fetch_verified() -> dict:
    global VERIFIED_CACHE
    if VERIFIED_CACHE:
        return VERIFIED_CACHE
    try:
        with urllib.request.urlopen(VERIFIED_URL, timeout=4) as r:
            VERIFIED_CACHE = json.loads(r.read().decode("utf-8"))
            return VERIFIED_CACHE
    except Exception as e:
        log.debug(f"Could not fetch verified authors: {e}")
        return {}


def _is_verified(author: str) -> bool:
    verified = _fetch_verified()
    return author.lower() in {k.lower() for k in verified.keys()}


def _load_plugin_icon(plugin_dir: str, size: int = 32):
    """Load icon_plugin.png from a plugin directory, return PhotoImage or None."""
    try:
        from PIL import Image, ImageTk
        icon_path = os.path.join(plugin_dir, "icon_plugin.png")
        if os.path.exists(icon_path):
            img = Image.open(icon_path).convert("RGBA").resize((size, size), Image.LANCZOS)
            return ImageTk.PhotoImage(img)
    except Exception:
        pass
    return None


# -- Tooltip helper ------------------------------------------------------------

class _Tooltip:
    """Simple hover tooltip."""
    def __init__(self, widget, text: str):
        self._widget = widget
        self._text   = text
        self._tip    = None
        widget.bind("<Enter>", self._show, add="+")
        widget.bind("<Leave>", self._hide, add="+")

    def _show(self, _event=None):
        x = self._widget.winfo_rootx() + 20
        y = self._widget.winfo_rooty() + self._widget.winfo_height() + 4
        self._tip = tw = tk.Toplevel(self._widget)
        tw.wm_overrideredirect(True)
        tw.wm_geometry(f"+{x}+{y}")
        tk.Label(tw, text=self._text,
                 bg="#1e2340", fg=C["text"],
                 font=("Segoe UI", 8), relief="flat",
                 padx=8, pady=4, bd=1).pack()

    def _hide(self, _event=None):
        if self._tip:
            try: self._tip.destroy()
            except Exception: pass
            self._tip = None


# ══════════════════════════════════════════════════════════════════════════════
#  ConsoleWindow — standalone Toplevel, opened only by hotkey
# ══════════════════════════════════════════════════════════════════════════════

class ConsoleWindow:
    """
    Hidden developer console.
    Opened by Ctrl+Shift+T or Ctrl+Shift+C.
    No reference to it exists anywhere in the main UI.
    """

    # Singleton per-app reference
    _instance = None

    @classmethod
    def toggle(cls, parent, plugin_loader, settings):
        """Open or close the console (hotkey callback)."""
        if cls._instance and cls._instance.alive:
            cls._instance._close()
        else:
            cls._instance = cls(parent, plugin_loader, settings)

    def __init__(self, parent, plugin_loader, settings):
        self.parent        = parent
        self.plugin_loader = plugin_loader
        self.settings      = settings
        self.alive         = True

        self.win = tk.Toplevel(parent)
        self.win.title("HomRec — Developer Console")
        self.win.configure(bg="#03040d")
        self.win.geometry("720x430")
        self.win.minsize(540, 300)
        self.win.attributes("-topmost", True)

        self._set_icon(self.win)
        self._build()
        self.win.protocol("WM_DELETE_WINDOW", self._close)
        self.win.bind("<Escape>", lambda _: self._close())

        # Focus the entry immediately
        self.win.after(50, self._entry.focus_set)

    @staticmethod
    def _set_icon(window) -> None:
        candidates = [
            os.path.join(_base_dir, "Assets", "ofc", "main.ico"),
            os.path.join(_base_dir, "icons", "main.ico"),
            os.path.join(_base_dir, "main.ico"),
        ]
        for ico in candidates:
            if os.path.exists(ico):
                try: window.iconbitmap(ico); return
                except Exception: pass

    def _close(self):
        self.alive = False
        try: self.win.destroy()
        except Exception: pass
        ConsoleWindow._instance = None

    # -- Build UI ----------------------------------------------------------

    def _build(self):
        w = self.win

        # -- Top title bar -------------------------------------------------
        bar = tk.Frame(w, bg="#07090f")
        bar.pack(fill="x")
        tk.Label(bar, text="⌨  HomRec Console",
                 font=("Consolas", 10, "bold"),
                 bg="#07090f", fg=C["accent"],
                 padx=12, pady=6).pack(side="left")
        tk.Label(bar,
                 text=f"Press Escape or {_CONSOLE_HOTKEY_TIP} to close",
                 font=("Segoe UI", 8),
                 bg="#07090f", fg=C["text3"]).pack(side="right", padx=10)

        # -- Prefix hint bar -----------------------------------------------
        hint = tk.Frame(w, bg="#07090f")
        hint.pack(fill="x")
        for prefix, color, tip in [
            ("/",  C["accent"],   "system"),
            ("!",  "#80cbc4",     "HomRec internal"),
            ("$",  C["success"],  "plugin manager"),
            ("%",  "#ce93d8",     "plugin actions"),
            ("--", C["text2"],    "flag  --key=val"),
        ]:
            tk.Label(hint, text=prefix, font=("Consolas", 9, "bold"),
                     bg="#07090f", fg=color, padx=6, pady=3).pack(side="left")
            tk.Label(hint, text=tip + "   ", font=("Segoe UI", 8),
                     bg="#07090f", fg=C["text3"]).pack(side="left")

        tk.Frame(w, bg=C["border"], height=1).pack(fill="x")

        # -- Output area ---------------------------------------------------
        out_frame = tk.Frame(w, bg="#03040d")
        out_frame.pack(fill="both", expand=True)

        self._out = tk.Text(
            out_frame,
            bg="#03040d", fg=C["text"],
            insertbackground=C["accent"],
            font=("Consolas", 10),
            relief="flat", bd=0,
            state="disabled",
            wrap="word",
            padx=10, pady=6,
        )
        sb = ttk.Scrollbar(out_frame, command=self._out.yview)
        self._out.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        self._out.pack(side="left", fill="both", expand=True)

        for tag, fg, font in [
            ("sys",    C["accent"],   ("Consolas", 10, "bold")),
            ("plugin", "#80cbc4",     ("Consolas", 10, "bold")),
            ("pmgr",   C["success"],  ("Consolas", 10, "bold")),
            ("pact",   "#ce93d8",     ("Consolas", 10, "bold")),
            ("ok",     C["success"],  None),
            ("err",    C["error"],    None),
            ("warn",   C["warning"],  None),
            ("dim",    C["text2"],    None),
            ("result", "#b0bec5",     None),
            ("sep",    C["border2"],  None),
        ]:
            kw = {"foreground": fg}
            if font: kw["font"] = font
            self._out.tag_configure(tag, **kw)

        self._cprint("━" * 60, "sep")
        self._cprint("  HomRec Developer Console", "sys")
        self._cprint("  /help  $help  !help   —  command lists", "dim")
        self._cprint("━" * 60, "sep")
        self._cprint("")

        # -- Status line ---------------------------------------------------
        self._status = tk.Label(w, text="",
                                font=("Consolas", 9),
                                bg="#07090f", fg=C["accent"],
                                anchor="w", padx=10, pady=3)
        self._status.pack(fill="x")

        # -- Input row -----------------------------------------------------
        row = tk.Frame(w, bg="#03040d")
        row.pack(fill="x")

        tk.Label(row, text=" › ", font=("Consolas", 13, "bold"),
                 bg="#03040d", fg=C["accent"]).pack(side="left")

        self._var = tk.StringVar()
        self._entry = tk.Entry(
            row, textvariable=self._var,
            font=("Consolas", 10),
            bg="#03040d", fg=C["text"],
            insertbackground=C["accent"],
            relief="flat", bd=6)
        self._entry.pack(side="left", fill="x", expand=True)
        self._entry.bind("<Return>", self._submit)
        self._entry.bind("<Up>",     self._hist_up)
        self._entry.bind("<Down>",   self._hist_down)
        self._entry.bind("<Tab>",    self._tab_complete)

        tk.Button(row, text="▶",
                  font=("Consolas", 10, "bold"),
                  bg=C["accent"], fg=C["bg"],
                  relief="flat", cursor="hand2", padx=10,
                  command=self._submit).pack(side="left", padx=(4, 4))

        self._history:  list[str] = []
        self._hist_idx: int       = -1

    # -- Output helpers ----------------------------------------------------

    def _cprint(self, text: str, tag: str = "") -> None:
        self._out.configure(state="normal")
        self._out.insert("end", text + "\n", tag if tag else ())
        self._out.see("end")
        self._out.configure(state="disabled")

    def _cstatus(self, text: str, color: str = None) -> None:
        self._status.config(text=text, fg=color or C["accent"])
        self._cprint(f"  → {text}", "result")

    # -- Input handling ----------------------------------------------------

    def _submit(self, _event=None):
        raw = self._var.get().strip()
        if not raw: return
        self._var.set("")
        self._history.append(raw)
        self._hist_idx = -1
        self._dispatch(raw)

    def _hist_up(self, _event=None):
        if not self._history: return
        self._hist_idx = min(self._hist_idx + 1, len(self._history) - 1)
        self._var.set(self._history[-(self._hist_idx + 1)])
        self._entry.icursor("end")
        return "break"

    def _hist_down(self, _event=None):
        if self._hist_idx <= 0:
            self._hist_idx = -1
            self._var.set("")
            return "break"
        self._hist_idx -= 1
        self._var.set(self._history[-(self._hist_idx + 1)])
        self._entry.icursor("end")
        return "break"

    def _tab_complete(self, _event=None):
        raw = self._var.get()
        cmds = ["/help", "/version", "/quit",
                "!reload", "!status", "!fps",
                "$list", "$install", "$uninstall", "$enable", "$disable",
                "%list"]
        matches = [c for c in cmds if c.startswith(raw)]
        if len(matches) == 1:
            self._var.set(matches[0])
            self._entry.icursor("end")
        elif matches:
            self._cprint("  " + "  ".join(matches), "dim")
        return "break"

    # -- Command dispatcher ------------------------------------------------

    def _dispatch(self, raw: str) -> None:
        def _echo(t, tag=""): self._cprint(t, tag)

        if raw.startswith("/"):
            _echo(f"/ {raw[1:]}", "sys")
            cmd, flags = self._parse(raw[1:])
            self._handle_system(cmd, flags, raw)
        elif raw.startswith("!"):
            _echo(f"! {raw[1:]}", "plugin")
            cmd, flags = self._parse(raw[1:])
            self._handle_internal(cmd, flags)
        elif raw.startswith("$"):
            _echo(f"$ {raw[1:]}", "pmgr")
            cmd, flags = self._parse(raw[1:])
            self._handle_plugin_mgr(cmd, flags)
        elif raw.startswith("%"):
            _echo(f"% {raw[1:]}", "pact")
            cmd, flags = self._parse(raw[1:])
            self._handle_plugin_action(cmd, flags)
        else:
            _echo(f"? {raw}", "dim")
            _echo("  /system   !internal   $plugin_mgr   %plugin_action", "warn")

    @staticmethod
    def _parse(text: str) -> tuple:
        parts = text.split()
        if not parts: return "", {}
        cmd = parts[0].lower()
        flags = {}
        pos = 1
        for p in parts[1:]:
            if p.startswith("--"):
                kv = p[2:].split("=", 1)
                flags[kv[0]] = kv[1] if len(kv) > 1 else True
            elif p.startswith("-"):
                flags[p[1:]] = True
            else:
                flags[f"_{pos}"] = p
                pos += 1
        return cmd, flags

    def _handle_system(self, cmd: str, flags: dict, _raw: str) -> None:
        if cmd in ("help", "h", "?"):
            self._cprint("  /version   /quit   /clear   /help", "dim")
        elif cmd == "version":
            try:
                from core.constants import CURRENT_VERSION
                self._cstatus(f"HomRec {CURRENT_VERSION}")
            except Exception:
                self._cstatus("version unknown")
        elif cmd == "clear":
            self._out.configure(state="normal")
            self._out.delete("1.0", "end")
            self._out.configure(state="disabled")
        elif cmd in ("quit", "exit"):
            self._close()
        else:
            self._cprint(f"  unknown system command: {cmd}", "warn")

    def _handle_internal(self, cmd: str, flags: dict) -> None:
        if cmd in ("help", "h", "?"):
            self._cprint("  !reload   !status   !fps   !help", "dim")
        elif cmd == "status":
            plugins = self.plugin_loader.plugins if self.plugin_loader else []
            self._cstatus(f"{len(plugins)} plugin(s) loaded")
        elif cmd == "reload":
            if self.plugin_loader:
                try:
                    self.plugin_loader.scan()
                    self._cstatus("plugins reloaded", C["success"])
                except Exception as e:
                    self._cstatus(f"reload error: {e}", C["error"])
            else:
                self._cstatus("no plugin loader", C["warning"])
        else:
            self._cprint(f"  unknown internal command: {cmd}", "warn")

    def _handle_plugin_mgr(self, cmd: str, flags: dict) -> None:
        plugins = self.plugin_loader.plugins if self.plugin_loader else []
        if cmd in ("help", "h", "?"):
            self._cprint("  $list   $enable <name>   $disable <name>   $help", "dim")
        elif cmd == "list":
            if not plugins:
                self._cprint("  (no plugins)", "dim")
            for p in plugins:
                state = "✓" if getattr(p, "active", True) else "✗"
                self._cprint(f"  {state}  {p.name}  v{getattr(p,'version','?')}  [{p.author}]", "result")
        else:
            name = flags.get("_1", "")
            self._cprint(f"  unknown plugin command: {cmd} {name}", "warn")

    def _handle_plugin_action(self, cmd: str, flags: dict) -> None:
        self._cprint(f"  %{cmd} — not implemented in console", "warn")


# ══════════════════════════════════════════════════════════════════════════════
#  LibraryWindow
# ══════════════════════════════════════════════════════════════════════════════

class LibraryWindow:
    """
    Standalone Library window — plugins, themes, languages.
    Console is NOT exposed here; open it with Ctrl+Shift+T or Ctrl+Shift+C.
    """

    def __init__(self, parent: tk.Tk, plugin_loader, settings, on_reload=None):
        self.parent        = parent
        self.plugin_loader = plugin_loader
        self.settings      = settings
        self.on_reload     = on_reload
        self._icon_refs    = []
        self._poll_id      = None
        self._last_plugin_set: set = set()

        # Single-instance guard
        for child in parent.winfo_children():
            if isinstance(child, tk.Toplevel) and getattr(child, "_is_library", False):
                child.focus()
                return

        self.win = tk.Toplevel(parent)
        self.win._is_library = True
        self.win.title("Library — HomRec")
        self.win.configure(bg=C["bg"])
        self.win.geometry("800x600")
        self.win.minsize(660, 480)

        self._set_icon(self.win)
        self._build_ui()

        # -- Register global hotkeys for console ---------------------------
        # Bind to both the library window and the parent root
        for widget in (self.win, parent):
            widget.bind("<Control-Shift-T>",
                        lambda _e: ConsoleWindow.toggle(parent, plugin_loader, settings),
                        add="+")
            widget.bind("<Control-Shift-t>",
                        lambda _e: ConsoleWindow.toggle(parent, plugin_loader, settings),
                        add="+")
            widget.bind("<Control-Shift-C>",
                        lambda _e: ConsoleWindow.toggle(parent, plugin_loader, settings),
                        add="+")
            widget.bind("<Control-Shift-c>",
                        lambda _e: ConsoleWindow.toggle(parent, plugin_loader, settings),
                        add="+")

        threading.Thread(target=_fetch_verified, daemon=True).start()
        self._poll_plugins_folder()
        self.win.protocol("WM_DELETE_WINDOW", self._on_close)

    @staticmethod
    def _set_icon(window) -> None:
        candidates = [
            os.path.join(_base_dir, "Assets", "ofc", "main.ico"),
            os.path.join(_base_dir, "icons", "main.ico"),
            os.path.join(_base_dir, "main.ico"),
        ]
        for ico in candidates:
            if os.path.exists(ico):
                try: window.iconbitmap(ico); return
                except Exception: pass

    def _on_close(self) -> None:
        if self._poll_id:
            try: self.win.after_cancel(self._poll_id)
            except Exception: pass
        self.win.destroy()

    # -- UI --------------------------------------------------------------------

    def _build_ui(self) -> None:
        # -- Header --------------------------------------------------------
        header = tk.Frame(self.win, bg=C["bg"])
        header.pack(fill="x", padx=22, pady=(20, 0))

        tk.Label(header, text="Library",
                 font=("Segoe UI", 20, "bold"),
                 bg=C["bg"], fg=C["accent"]).pack(side="left")

        # Install button
        inst_btn = tk.Button(header, text="＋  Install .hrp",
                             font=("Segoe UI", 9, "bold"),
                             bg=C["success"], fg=C["bg"],
                             activebackground="#00c853",
                             relief="flat", cursor="hand2",
                             padx=14, pady=5,
                             command=self._install_from_file)
        inst_btn.pack(side="right")
        _Tooltip(inst_btn, "Install a plugin from a local .hrp file")

        tk.Frame(self.win, bg=C["border2"], height=1).pack(fill="x", pady=(14, 0))

        # -- Notebook ------------------------------------------------------
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("Lib.TNotebook",
                        background=C["bg"], borderwidth=0,
                        tabmargins=[0, 0, 0, 0])
        style.configure("Lib.TNotebook.Tab",
                        background=C["surface2"], foreground=C["text2"],
                        padding=[18, 8], font=("Segoe UI", 9),
                        borderwidth=0)
        style.map("Lib.TNotebook.Tab",
                  background=[("selected", C["surface3"])],
                  foreground=[("selected", C["accent"])])

        nb = ttk.Notebook(self.win, style="Lib.TNotebook")
        nb.pack(fill="both", expand=True)

        self._tab_plugins = tk.Frame(nb, bg=C["surface"])
        self._tab_themes  = tk.Frame(nb, bg=C["surface"])
        self._tab_langs   = tk.Frame(nb, bg=C["surface"])

        nb.add(self._tab_plugins, text="🧩  Plugins")
        nb.add(self._tab_themes,  text="🎨  Themes")
        nb.add(self._tab_langs,   text="🌐  Languages")

        self._nb = nb
        self._build_plugins_tab()
        self._build_themes_tab()
        self._build_langs_tab()

    # -- Plugins tab -----------------------------------------------------------

    def _build_plugins_tab(self) -> None:
        t = self._tab_plugins

        # Toolbar
        toolbar = tk.Frame(t, bg=C["surface"], pady=8)
        toolbar.pack(fill="x", padx=16)

        tk.Label(toolbar, text="🔍", bg=C["surface"], fg=C["text2"],
                 font=("Segoe UI", 11)).pack(side="left", padx=(0, 4))

        self._search_var = tk.StringVar()
        self._search_var.trace_add("write", lambda *_: self._refresh_plugins())

        search_entry = tk.Entry(toolbar, textvariable=self._search_var,
                                bg=C["surface3"], fg=C["text"],
                                insertbackground=C["accent"],
                                font=("Segoe UI", 9), relief="flat",
                                width=26, bd=5)
        search_entry.pack(side="left")

        self._count_lbl = tk.Label(toolbar, text="",
                                   bg=C["surface"], fg=C["text3"],
                                   font=("Segoe UI", 8))
        self._count_lbl.pack(side="right", padx=(0, 4))

        tk.Frame(t, bg=C["border"], height=1).pack(fill="x")

        # Scrollable canvas
        canvas = tk.Canvas(t, bg=C["surface"], highlightthickness=0)
        sb = ttk.Scrollbar(t, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)

        self._plugins_frame  = tk.Frame(canvas, bg=C["surface"])
        self._plugins_window = canvas.create_window(
            (0, 0), window=self._plugins_frame, anchor="nw")

        self._plugins_frame.bind("<Configure>",
            lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>",
            lambda e: canvas.itemconfig(self._plugins_window, width=e.width))
        canvas.bind("<MouseWheel>",
            lambda e: canvas.yview_scroll(-1 * (e.delta // 120), "units"))

        self._refresh_plugins()

    def _refresh_plugins(self) -> None:
        for w in self._plugins_frame.winfo_children():
            w.destroy()
        self._icon_refs.clear()

        query   = self._search_var.get().lower().strip() if hasattr(self, "_search_var") else ""
        plugins = self.plugin_loader.plugins if self.plugin_loader else []
        matches = [p for p in plugins
                   if not query or query in p.name.lower()
                   or query in (p.author or "").lower()]

        if hasattr(self, "_count_lbl"):
            n = len(matches)
            self._count_lbl.config(text=f"{n} plugin{'s' if n != 1 else ''}")
            # Update tab label with count
            try:
                self._nb.tab(self._tab_plugins,
                             text=f"🧩  Plugins  ({n})" if n else "🧩  Plugins")
            except Exception:
                pass

        if not matches:
            msg = ("No plugins match your search." if query else
                   "No plugins installed.\nDrop a .hrp file here or click  ＋ Install .hrp")
            tk.Label(self._plugins_frame, text=msg,
                     font=("Segoe UI", 10), bg=C["surface"], fg=C["text2"],
                     justify="center").pack(pady=60)
            return

        for p in matches:
            self._plugin_card(self._plugins_frame, p)

    def _plugin_card(self, parent, p) -> None:
        verified = _is_verified(p.author)
        accent   = C["verified"] if verified else C["border2"]

        card = tk.Frame(parent, bg=C["plugin_bg"], bd=0)
        card.pack(fill="x", padx=14, pady=6)

        # Hover highlight
        def _on_enter(_): card.configure(bg=C["hover"])
        def _on_leave(_): card.configure(bg=C["plugin_bg"])
        card.bind("<Enter>", _on_enter)
        card.bind("<Leave>", _on_leave)

        # Left accent strip
        strip = tk.Frame(card, bg=accent, width=3)
        strip.pack(side="left", fill="y")

        inner = tk.Frame(card, bg=C["plugin_bg"])
        inner.pack(side="left", fill="both", expand=True, padx=14, pady=10)

        # Icon column
        icon_col = tk.Frame(inner, bg=C["plugin_bg"])
        icon_col.pack(side="left", padx=(0, 14))

        icon_img = _load_plugin_icon(getattr(p, "plugin_dir", ""))
        if icon_img:
            self._icon_refs.append(icon_img)
            tk.Label(icon_col, image=icon_img, bg=C["plugin_bg"]).pack()
        else:
            tk.Label(icon_col, text="🧩", font=("Segoe UI", 22),
                     bg=C["plugin_bg"]).pack()

        # Info column
        info = tk.Frame(inner, bg=C["plugin_bg"])
        info.pack(side="left", fill="both", expand=True)

        name_row = tk.Frame(info, bg=C["plugin_bg"])
        name_row.pack(fill="x")
        tk.Label(name_row, text=p.name,
                 font=("Segoe UI", 11, "bold"),
                 bg=C["plugin_bg"], fg=C["text"]).pack(side="left")
        if verified:
            badge = tk.Label(name_row, text=" ✓ verified",
                             font=("Segoe UI", 8),
                             bg=C["plugin_bg"], fg=C["verified"])
            badge.pack(side="left", padx=6)
            _Tooltip(badge, f"{p.author} is a verified HomRec author")

        meta_parts = []
        if hasattr(p, "version"): meta_parts.append(f"v{p.version}")
        if p.author:              meta_parts.append(f"by {p.author}")
        if meta_parts:
            tk.Label(info, text="  ".join(meta_parts),
                     font=("Segoe UI", 8), bg=C["plugin_bg"],
                     fg=C["text2"]).pack(anchor="w")

        desc = getattr(p, "description", "") or ""
        if desc:
            tk.Label(info, text=desc[:90] + ("…" if len(desc) > 90 else ""),
                     font=("Segoe UI", 8), bg=C["plugin_bg"],
                     fg=C["text3"], wraplength=340, justify="left").pack(anchor="w")

        # Permissions row
        perms = getattr(p, "permissions", []) or []
        if perms:
            prow = tk.Frame(info, bg=C["plugin_bg"])
            prow.pack(anchor="w", pady=(3, 0))
            for perm in perms[:6]:
                color = PERM_COLORS.get(perm, C["text3"])
                tk.Label(prow, text=perm,
                         font=("Segoe UI", 7), bg=C["surface3"],
                         fg=color, padx=5, pady=1,
                         relief="flat").pack(side="left", padx=2)

        # Action buttons
        btn_col = tk.Frame(inner, bg=C["plugin_bg"])
        btn_col.pack(side="right", padx=(0, 4))

        tk.Button(btn_col, text="Uninstall",
                  font=("Segoe UI", 8),
                  bg=C["surface3"], fg=C["error"],
                  activebackground=C["surface2"],
                  relief="flat", cursor="hand2",
                  padx=8, pady=3,
                  command=lambda _p=p: self._uninstall_plugin(_p)
                  ).pack(pady=2)

    def _uninstall_plugin(self, p) -> None:
        if not messagebox.askyesno(
                "Uninstall plugin",
                f"Remove '{p.name}' by {p.author}?\n\nThis will delete the plugin folder.",
                parent=self.win):
            return
        try:
            plugin_dir = getattr(p, "plugin_dir", None)
            if plugin_dir and os.path.isdir(plugin_dir):
                shutil.rmtree(plugin_dir)
            if self.plugin_loader and hasattr(self.plugin_loader, "remove_plugin"):
                self.plugin_loader.remove_plugin(p)
            if self.on_reload:
                self.on_reload()
            self._refresh_plugins()
        except Exception as e:
            messagebox.showerror("Error", str(e), parent=self.win)

    def _install_from_file(self) -> None:
        path = filedialog.askopenfilename(
            title="Install Plugin",
            filetypes=[("HomRec Plugin", "*.hrp"), ("All files", "*.*")],
            parent=self.win)
        if not path:
            return
        threading.Thread(target=self._do_install, args=(path,), daemon=True).start()

    def _do_install(self, path: str) -> None:
        try:
            plugins_dir = os.path.join(_base_dir, "plugins")
            os.makedirs(plugins_dir, exist_ok=True)

            with zipfile.ZipFile(path) as zf:
                meta_raw = next(
                    (zf.read(n) for n in zf.namelist()
                     if os.path.basename(n) == "plugin.json"), None)
                if not meta_raw:
                    raise ValueError("Not a valid .hrp: missing plugin.json")
                meta = json.loads(meta_raw)
                name = meta.get("name", os.path.splitext(os.path.basename(path))[0])
                dest = os.path.join(plugins_dir, name)
                if os.path.exists(dest):
                    self.win.after(0, lambda: messagebox.showerror(
                        "Already installed",
                        f"A plugin named '{name}' is already installed.",
                        parent=self.win))
                    return
                zf.extractall(dest)

            if self.plugin_loader:
                self.plugin_loader.scan()
            if self.on_reload:
                self.win.after(0, self.on_reload)
            self.win.after(0, self._refresh_plugins)
            log.info(f"Installed plugin: {name}")
        except Exception as e:
            self.win.after(0, lambda: messagebox.showerror(
                "Install Error", str(e), parent=self.win))

    # -- Themes tab ------------------------------------------------------------

    def _build_themes_tab(self) -> None:
        t = self._tab_themes

        tk.Label(t, text="Installed Themes",
                 font=("Segoe UI", 12, "bold"),
                 bg=C["surface"], fg=C["text"],
                 pady=14).pack(anchor="w", padx=16)

        tk.Frame(t, bg=C["border"], height=1).pack(fill="x")

        # Scrollable frame for theme cards
        canvas = tk.Canvas(t, bg=C["surface"], highlightthickness=0)
        sb = ttk.Scrollbar(t, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)

        frame = tk.Frame(canvas, bg=C["surface"])
        win_id = canvas.create_window((0, 0), window=frame, anchor="nw")
        frame.bind("<Configure>",
                   lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>",
                    lambda e: canvas.itemconfig(win_id, width=e.width))
        canvas.bind("<MouseWheel>",
                    lambda e: canvas.yview_scroll(-1 * (e.delta // 120), "units"))

        self._load_themes(frame)

    def _load_themes(self, parent) -> None:
        themes_dir = os.path.join(_base_dir, "themes")
        themes = []
        if os.path.isdir(themes_dir):
            for fn in os.listdir(themes_dir):
                fp = os.path.join(themes_dir, fn)
                if fn.endswith(".hrt") or fn.endswith(".json"):
                    try:
                        raw = open(fp, "rb").read()
                        if raw[:2] == b"\x1f\x8b":
                            raw = gzip.decompress(raw)
                        data = json.loads(raw)
                        themes.append((fn, data))
                    except Exception:
                        pass

        if not themes:
            tk.Label(parent,
                     text="No themes installed.\nThemes go in the  themes/  folder.",
                     font=("Segoe UI", 10), bg=C["surface"], fg=C["text2"],
                     justify="center").pack(pady=60)
            return

        for fn, data in themes:
            self._theme_card(parent, fn, data)

    def _theme_card(self, parent, filename: str, data: dict) -> None:
        name    = data.get("name", filename)
        author  = data.get("author", "")
        colors  = data.get("colors", {})

        card = tk.Frame(parent, bg=C["surface2"], bd=0)
        card.pack(fill="x", padx=14, pady=6)

        left = tk.Frame(card, bg=C["accent2"], width=3)
        left.pack(side="left", fill="y")

        inner = tk.Frame(card, bg=C["surface2"])
        inner.pack(side="left", fill="both", expand=True, padx=14, pady=10)

        tk.Label(inner, text=name,
                 font=("Segoe UI", 10, "bold"),
                 bg=C["surface2"], fg=C["text"]).pack(anchor="w")
        if author:
            tk.Label(inner, text=f"by {author}",
                     font=("Segoe UI", 8),
                     bg=C["surface2"], fg=C["text2"]).pack(anchor="w")

        # Color swatches
        if colors:
            swatch_row = tk.Frame(inner, bg=C["surface2"])
            swatch_row.pack(anchor="w", pady=(5, 0))
            for key in list(colors.keys())[:8]:
                col = colors[key]
                try:
                    sw = tk.Frame(swatch_row, bg=col, width=18, height=18,
                                  relief="flat", bd=0)
                    sw.pack(side="left", padx=2)
                    _Tooltip(sw, key)
                except Exception:
                    pass

        tk.Button(card, text="Apply",
                  font=("Segoe UI", 8, "bold"),
                  bg=C["accent2"], fg="white",
                  relief="flat", cursor="hand2",
                  padx=10, pady=3,
                  command=lambda d=data: self._apply_theme(d)
                  ).pack(side="right", padx=12, pady=8)

    def _apply_theme(self, data: dict) -> None:
        try:
            if self.settings and hasattr(self.settings, "apply_theme"):
                self.settings.apply_theme(data)
            elif self.settings:
                self.settings["colors"] = data.get("colors", {})
            messagebox.showinfo("Theme Applied",
                                f"Theme '{data.get('name', 'Unknown')}' applied.\n"
                                "Restart may be needed for full effect.",
                                parent=self.win)
        except Exception as e:
            messagebox.showerror("Error", str(e), parent=self.win)

    # -- Languages tab ---------------------------------------------------------

    _LANG_META = {
        "en": ("🇬🇧", "English"),
        "ru": ("🇷🇺", "Русский"),
        "de": ("🇩🇪", "Deutsch"),
        "fr": ("🇫🇷", "Français"),
        "zh": ("🇨🇳", "中文"),
        "ja": ("🇯🇵", "日本語"),
        "ko": ("🇰🇷", "한국어"),
        "es": ("🇪🇸", "Español"),
        "pt": ("🇧🇷", "Português"),
        "it": ("🇮🇹", "Italiano"),
    }

    def _build_langs_tab(self) -> None:
        t = self._tab_langs

        tk.Label(t, text="Interface Language",
                 font=("Segoe UI", 12, "bold"),
                 bg=C["surface"], fg=C["text"],
                 pady=14).pack(anchor="w", padx=16)

        tk.Frame(t, bg=C["border"], height=1).pack(fill="x")

        inner = tk.Frame(t, bg=C["surface"])
        inner.pack(fill="both", expand=True, padx=16, pady=12)

        try:
            from core.languages import LANGUAGES
            available = list(LANGUAGES.keys())
        except Exception:
            available = ["en", "ru"]

        current_lang = ""
        if self.settings:
            current_lang = (self.settings.get("language", "")
                            if isinstance(self.settings, dict)
                            else getattr(self.settings, "language", ""))

        for lang_code in available:
            flag, native = self._LANG_META.get(lang_code, ("🌐", lang_code.upper()))
            is_current   = (lang_code == current_lang)

            row = tk.Frame(inner, bg=C["surface2"] if is_current else C["surface"])
            row.pack(fill="x", pady=3)

            def _on_enter(e, r=row, cur=is_current):
                if not cur: r.configure(bg=C["hover"])
            def _on_leave(e, r=row, cur=is_current):
                r.configure(bg=C["surface2"] if cur else C["surface"])
            row.bind("<Enter>", _on_enter)
            row.bind("<Leave>", _on_leave)

            tk.Label(row, text=flag, font=("Segoe UI", 16),
                     bg=row["bg"]).pack(side="left", padx=(10, 6), pady=6)
            tk.Label(row, text=native, font=("Segoe UI", 10),
                     bg=row["bg"], fg=C["accent"] if is_current else C["text"]
                     ).pack(side="left")

            if is_current:
                tk.Label(row, text="✓  active",
                         font=("Segoe UI", 8),
                         bg=row["bg"], fg=C["success"]).pack(side="left", padx=8)
            else:
                tk.Button(row, text="Select",
                          font=("Segoe UI", 8),
                          bg=C["surface3"], fg=C["text2"],
                          relief="flat", cursor="hand2",
                          padx=8, pady=2,
                          command=lambda lc=lang_code: self._set_language(lc)
                          ).pack(side="right", padx=10)

    def _set_language(self, lang_code: str) -> None:
        try:
            if self.settings:
                if isinstance(self.settings, dict):
                    self.settings["language"] = lang_code
                else:
                    self.settings.language = lang_code
            if self.on_reload:
                self.on_reload()
            # Rebuild the tab to show new selection
            for w in self._tab_langs.winfo_children(): w.destroy()
            self._build_langs_tab()
        except Exception as e:
            messagebox.showerror("Error", str(e), parent=self.win)

    # -- Plugin folder polling (debounced) -------------------------------------

    def _poll_plugins_folder(self) -> None:
        plugins_dir = os.path.join(_base_dir, "plugins")
        try:
            if os.path.isdir(plugins_dir):
                current = {
                    f for f in os.listdir(plugins_dir)
                    if f.endswith(".hrp") or os.path.isdir(os.path.join(plugins_dir, f))
                }
                if current != self._last_plugin_set:
                    self._last_plugin_set = current
                    if self.plugin_loader:
                        self.plugin_loader.scan()
                    self._refresh_plugins()
        except Exception:
            pass

        try:
            self._poll_id = self.win.after(2500, self._poll_plugins_folder)
        except Exception:
            pass


# -- Module-level hotkey registration (called from app.py) --------------------

def register_console_hotkey(root: tk.Tk, plugin_loader, settings) -> None:
    """
    Register Ctrl+Shift+T and Ctrl+Shift+C globally on the root window.
    Call this from HomRecScreen.__init__ so the hotkey works even when
    the Library window is closed.
    """
    def _toggle(_event=None):
        ConsoleWindow.toggle(root, plugin_loader, settings)

    for seq in ("<Control-Shift-T>", "<Control-Shift-t>",
                "<Control-Shift-C>", "<Control-Shift-c>"):
        root.bind(seq, _toggle, add="+")
