from __future__ import annotations
import os, sys, json, shutil, zipfile, threading, platform
import tkinter as tk
from tkinter import ttk, messagebox
import logging

log = logging.getLogger("hr_core_manager")

_GITHUB_REPO  = "homaaaio/homrec"
_GITHUB_API   = f"https://api.github.com/repos/{_GITHUB_REPO}/releases"
_CORE_DIR_NAME = "cores"      # <app_root>/cores/
_ACTIVE_FILE   = "active_core.json"


def _app_root() -> str:
    return os.path.dirname(os.path.abspath(sys.argv[0]))


def _cores_dir() -> str:
    d = os.path.join(_app_root(), _CORE_DIR_NAME)
    os.makedirs(d, exist_ok=True)
    return d


def _active_core_path() -> str:
    return os.path.join(_cores_dir(), _ACTIVE_FILE)


def _read_active() -> dict:
    try:
        p = _active_core_path()
        if os.path.exists(p):
            return json.loads(open(p, encoding="utf-8").read())
    except Exception:
        pass
    return {"version": "default", "description": "Built-in (current release)"}


def _write_active(data: dict):
    open(_active_core_path(), "w", encoding="utf-8").write(
        json.dumps(data, ensure_ascii=False, indent=2)
    )


def _list_installed() -> list[dict]:
    """Return list of installed cores from <cores_dir>/*/core_manifest.json."""
    result = []
    cd = _cores_dir()
    for name in os.listdir(cd):
        mf = os.path.join(cd, name, "core_manifest.json")
        if os.path.isfile(mf):
            try:
                data = json.loads(open(mf, encoding="utf-8").read())
                data["_dir"] = os.path.join(cd, name)
                result.append(data)
            except Exception as e:
                log.warning("core manifest parse error %s: %s", mf, e)
    result.sort(key=lambda d: d.get("version", ""), reverse=True)
    return result


def _fetch_available_cores() -> list[dict]:
    """Fetch core release assets from GitHub API. Returns list of core info dicts."""
    import urllib.request
    try:
        req = urllib.request.Request(
            _GITHUB_API,
            headers={"Accept": "application/vnd.github+json",
                     "User-Agent": "HomRec-CoreManager/1.0"}
        )
        with urllib.request.urlopen(req, timeout=10) as r:
            releases = json.loads(r.read().decode("utf-8"))
        cores = []
        for rel in releases:
            for asset in rel.get("assets", []):
                name = asset.get("name", "")
                if name.startswith("core-") and name.endswith(".zip"):
                    ver = name[len("core-"):-len(".zip")]
                    cores.append({
                        "version":      ver,
                        "asset_name":   name,
                        "download_url": asset["browser_download_url"],
                        "size_bytes":   asset.get("size", 0),
                        "release_tag":  rel.get("tag_name", ""),
                        "release_body": rel.get("body", "")[:200],
                    })
        return cores
    except Exception as e:
        log.warning("Failed to fetch cores from GitHub: %s", e)
        return []


def _download_and_install(core_info: dict, progress_cb=None) -> tuple[bool, str]:
    """Download core zip, verify manifest, install to <cores_dir>/<version>/."""
    import urllib.request
    url  = core_info["download_url"]
    ver  = core_info["version"]
    dest_dir = os.path.join(_cores_dir(), ver)
    tmp_zip  = os.path.join(_cores_dir(), f"_tmp_core_{ver}.zip")
    try:
        os.makedirs(dest_dir, exist_ok=True)
        # Download
        def _reporthook(count, block, total):
            if progress_cb and total > 0:
                pct = min(100, int(count * block * 100 / total))
                progress_cb(pct, f"Downloading… {pct}%")
        urllib.request.urlretrieve(url, tmp_zip, _reporthook)
        if progress_cb: progress_cb(90, "Extracting…")
        # Extract
        with zipfile.ZipFile(tmp_zip, "r") as zf:
            zf.extractall(dest_dir)
        os.remove(tmp_zip)
        # Verify manifest
        mf = os.path.join(dest_dir, "core_manifest.json")
        if not os.path.isfile(mf):
            shutil.rmtree(dest_dir, ignore_errors=True)
            return False, "core_manifest.json not found in archive"
        manifest = json.loads(open(mf, encoding="utf-8").read())
        if manifest.get("version") != ver:
            return False, f"Manifest version mismatch: expected {ver}, got {manifest.get('version')}"
        if progress_cb: progress_cb(100, "Done")
        return True, ""
    except Exception as e:
        shutil.rmtree(dest_dir, ignore_errors=True)
        if os.path.exists(tmp_zip):
            try: os.remove(tmp_zip)
            except: pass
        return False, str(e)


def apply_core(core_dir: str | None) -> tuple[bool, str]:
    root = _app_root()
    backup_dir = os.path.join(_cores_dir(), "_backup_default")

    if core_dir is None:
        # Restore backup
        if not os.path.isdir(backup_dir):
            return False, "No default backup found — cannot revert."
        for fn in os.listdir(backup_dir):
            src = os.path.join(backup_dir, fn)
            dst = os.path.join(root, fn)
            try:
                shutil.copy2(src, dst)
            except Exception as e:
                return False, f"Failed to restore {fn}: {e}"
        return True, ""

    # Read manifest to know which files to copy
    mf_path = os.path.join(core_dir, "core_manifest.json")
    try:
        manifest = json.loads(open(mf_path, encoding="utf-8").read())
    except Exception as e:
        return False, f"Cannot read manifest: {e}"

    files = manifest.get("files", [])
    if not files:
        return False, "Manifest lists no files to install."

    # Back up originals before first swap
    os.makedirs(backup_dir, exist_ok=True)
    for fn in files:
        src = os.path.join(root, fn)
        bak = os.path.join(backup_dir, fn)
        if os.path.exists(src) and not os.path.exists(bak):
            try:
                shutil.copy2(src, bak)
            except Exception as e:
                log.warning("Could not back up %s: %s", fn, e)

    # Copy core files
    for fn in files:
        src = os.path.join(core_dir, fn)
        dst = os.path.join(root, fn)
        if not os.path.exists(src):
            return False, f"Core file missing: {fn}"
        try:
            shutil.copy2(src, dst)
        except Exception as e:
            return False, f"Failed to install {fn}: {e}"

    return True, ""


# ──────────────────────────────────────────────────────────────────────────────
# UI
# ──────────────────────────────────────────────────────────────────────────────

class CoreManagerWindow:
    """
    HomRec Core Manager — manage which Core version is active.
    Accessible via Settings → Core Manager or Help → Core Manager.
    """
    def __init__(self, parent: tk.Tk | tk.Toplevel, app) -> None:
        self.app = app
        self.c   = app.colors

        win = tk.Toplevel(parent)
        win.title("HomRec — Core Manager")
        win.geometry("860x580")
        win.configure(bg=self.c["bg"])
        win.resizable(True, True)
        self.win = win

        self._available: list[dict] = []
        self._installed: list[dict] = []
        self._active = _read_active()
        self._build_ui()
        self._refresh_installed()
        # Fetch available in background
        threading.Thread(target=self._bg_fetch_available, daemon=True).start()

    # ── Layout ────────────────────────────────────────────────────────────────
    def _build_ui(self) -> None:
        c = self.c

        # Header
        hdr = tk.Frame(self.win, bg=c["surface"])
        hdr.pack(fill="x")
        tk.Label(hdr, text="⚙  Core Manager",
                 bg=c["surface"], fg=c["accent"],
                 font=("Segoe UI", 14, "bold")).pack(side="left", padx=16, pady=12)
        self._active_lbl = tk.Label(hdr,
            text=f"Active: {self._active.get('version','default')}",
            bg=c["surface"], fg=c["text_secondary"],
            font=("Segoe UI", 10))
        self._active_lbl.pack(side="left", padx=12)

        tk.Button(hdr, text="⟳ Refresh", command=self._manual_refresh,
                  bg=c.get("surface_light","#45475a"), fg=c["text"],
                  font=("Segoe UI", 9), relief="flat", padx=10, pady=5).pack(
                  side="right", padx=8, pady=8)
        tk.Button(hdr, text="📖 How Cores Work", command=self._show_help,
                  bg=c.get("surface_light","#45475a"), fg=c["accent"],
                  font=("Segoe UI", 9), relief="flat", padx=10, pady=5).pack(
                  side="right", padx=(0,4), pady=8)

        # Tabs
        nb = ttk.Notebook(self.win)
        nb.pack(fill="both", expand=True, padx=12, pady=(8,0))

        self._tab_installed = tk.Frame(nb, bg=c["bg"])
        self._tab_available = tk.Frame(nb, bg=c["bg"])
        nb.add(self._tab_installed, text="  Installed  ")
        nb.add(self._tab_available, text="  Available (GitHub)  ")

        self._build_installed_tab()
        self._build_available_tab()

        # Footer
        ftr = tk.Frame(self.win, bg=c["bg"])
        ftr.pack(fill="x", padx=12, pady=(4,8))
        self._status = tk.Label(ftr, text="",
                                bg=c["bg"], fg=c["text_secondary"],
                                font=("Segoe UI", 9))
        self._status.pack(side="left")
        tk.Button(ftr, text="Close", command=self.win.destroy,
                  bg=c["surface"], fg=c["text"],
                  font=("Segoe UI", 10), relief="flat",
                  padx=12, pady=5).pack(side="right")

    # ── Installed tab ─────────────────────────────────────────────────────────
    def _build_installed_tab(self) -> None:
        c = self.c
        tab = self._tab_installed

        tk.Label(tab,
                 text="These cores are already on disk and can be activated instantly.",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI", 9, "italic")).pack(anchor="w", padx=12, pady=(8,4))

        # Default / built-in row
        default_row = tk.Frame(tab, bg=c.get("surface_light","#45475a"), pady=0)
        default_row.pack(fill="x", padx=12, pady=2)
        is_default = (self._active.get("version") == "default")
        dot = "●" if is_default else "○"
        tk.Label(default_row,
                 text=f"  {dot}  default  —  Built-in (current release, always safe)",
                 bg=default_row["bg"], fg=c["accent"] if is_default else c["text"],
                 font=("Segoe UI", 10, "bold" if is_default else "normal")).pack(
                 side="left", padx=8, pady=8)
        if not is_default:
            tk.Button(default_row, text="↩ Revert to default",
                      command=self._revert_to_default,
                      bg=c["warning"], fg=c["bg"],
                      font=("Segoe UI", 9, "bold"), relief="flat",
                      padx=10, pady=4).pack(side="right", padx=8, pady=6)

        # Scrollable list
        canvas = tk.Canvas(tab, bg=c["bg"], highlightthickness=0)
        scroll = ttk.Scrollbar(tab, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True, padx=12, pady=4)
        self._inst_inner = tk.Frame(canvas, bg=c["bg"])
        canvas.create_window((0,0), window=self._inst_inner, anchor="nw")
        self._inst_inner.bind("<Configure>",
            lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        self._inst_canvas = canvas

    def _refresh_installed(self) -> None:
        self._installed = _list_installed()
        for w in self._inst_inner.winfo_children():
            w.destroy()
        c = self.c
        if not self._installed:
            tk.Label(self._inst_inner,
                     text="No additional cores installed.\nGo to the 'Available' tab to download one.",
                     bg=c["bg"], fg=c["text_secondary"],
                     font=("Segoe UI", 9, "italic"), justify="center").pack(pady=20)
            return
        for core in self._installed:
            ver = core.get("version","?")
            desc = core.get("description","")
            compat = core.get("compatible_with","")
            is_active = (self._active.get("version") == ver)
            dot = "●" if is_active else "○"
            bg = c["surface"] if is_active else c["bg"]
            row = tk.Frame(self._inst_inner, bg=bg, relief="flat")
            row.pack(fill="x", pady=2)

            info_col = tk.Frame(row, bg=bg)
            info_col.pack(side="left", fill="x", expand=True, padx=10, pady=6)
            tk.Label(info_col,
                     text=f"{dot}  Core {ver}",
                     bg=bg, fg=c["accent"] if is_active else c["text"],
                     font=("Segoe UI", 10, "bold")).pack(anchor="w")
            if desc:
                tk.Label(info_col, text=desc,
                         bg=bg, fg=c["text_secondary"],
                         font=("Segoe UI", 9)).pack(anchor="w")
            if compat:
                tk.Label(info_col, text=f"Compatible with HomRec {compat}",
                         bg=bg, fg=c["text_secondary"],
                         font=("Segoe UI", 8, "italic")).pack(anchor="w")

            btn_col = tk.Frame(row, bg=bg)
            btn_col.pack(side="right", padx=8, pady=6)
            if not is_active:
                tk.Button(btn_col, text="✔ Activate",
                          command=lambda d=core["_dir"], v=ver: self._activate_core(d, v),
                          bg=c["success"], fg=c["bg"],
                          font=("Segoe UI", 9, "bold"), relief="flat",
                          padx=10, pady=4).pack(side="left", padx=4)
            tk.Button(btn_col, text="🗑 Remove",
                      command=lambda d=core["_dir"], v=ver: self._remove_core(d, v),
                      bg=c["error"], fg=c["bg"],
                      font=("Segoe UI", 9), relief="flat",
                      padx=8, pady=4).pack(side="left")

    # ── Available tab ─────────────────────────────────────────────────────────
    def _build_available_tab(self) -> None:
        c = self.c
        tab = self._tab_available
        self._avail_inner = tk.Frame(tab, bg=c["bg"])
        self._avail_inner.pack(fill="both", expand=True, padx=12, pady=8)
        self._loading_lbl = tk.Label(self._avail_inner,
            text="⏳  Fetching available cores from GitHub…",
            bg=c["bg"], fg=c["text_secondary"],
            font=("Segoe UI", 10, "italic"))
        self._loading_lbl.pack(pady=30)

    def _refresh_available_ui(self) -> None:
        c = self.c
        for w in self._avail_inner.winfo_children():
            w.destroy()
        if not self._available:
            tk.Label(self._avail_inner,
                     text="No core releases found on GitHub.\n\n"
                          "Make sure you are connected to the internet and that\n"
                          "github.com/homaaaio/homrec has releases with  core-<ver>.zip  assets.",
                     bg=c["bg"], fg=c["text_secondary"],
                     font=("Segoe UI", 10, "italic"), justify="center").pack(pady=30)
            return
        tk.Label(self._avail_inner,
                 text="Available cores on GitHub — click Download to install:",
                 bg=c["bg"], fg=c["text_secondary"],
                 font=("Segoe UI", 9, "italic")).pack(anchor="w", pady=(0,6))
        installed_versions = {c.get("version") for c in self._installed}
        for core in self._available:
            ver  = core.get("version","?")
            size = core.get("size_bytes",0)
            size_str = f"{size/1024:.0f} KB" if size < 1_048_576 else f"{size/1_048_576:.1f} MB"
            already = ver in installed_versions
            row = tk.Frame(self._avail_inner, bg=c["surface"], relief="flat")
            row.pack(fill="x", pady=2)
            info = tk.Frame(row, bg=c["surface"])
            info.pack(side="left", fill="x", expand=True, padx=10, pady=6)
            tk.Label(info, text=f"Core {ver}  ({size_str})",
                     bg=c["surface"], fg=c["text"],
                     font=("Segoe UI", 10, "bold")).pack(anchor="w")
            note = core.get("release_body","")
            if note:
                tk.Label(info, text=note[:120],
                         bg=c["surface"], fg=c["text_secondary"],
                         font=("Segoe UI", 8)).pack(anchor="w")
            btn_frame = tk.Frame(row, bg=c["surface"])
            btn_frame.pack(side="right", padx=8, pady=6)
            if already:
                tk.Label(btn_frame, text="✔ Installed",
                         bg=c["surface"], fg=c["success"],
                         font=("Segoe UI", 9, "bold")).pack()
            else:
                dl_btn = tk.Button(btn_frame, text="⬇ Download",
                                   command=lambda ci=core: self._start_download(ci),
                                   bg=c["accent"], fg=c["bg"],
                                   font=("Segoe UI", 9, "bold"), relief="flat",
                                   padx=10, pady=4)
                dl_btn.pack()

    # ── Actions ───────────────────────────────────────────────────────────────
    def _activate_core(self, core_dir: str, version: str) -> None:
        if not messagebox.askyesno("Activate Core",
            f"Switch to Core {version}?\n\n"
            "HomRec will copy the core DLL files and restart.\n"
            "Your current DLL files will be backed up automatically.",
            parent=self.win):
            return
        ok, err = apply_core(core_dir)
        if not ok:
            messagebox.showerror("Core activation failed", err, parent=self.win)
            return
        mf = os.path.join(core_dir, "core_manifest.json")
        manifest = json.loads(open(mf, encoding="utf-8").read())
        _write_active({"version": version, "description": manifest.get("description",""),
                       "core_dir": core_dir})
        self._active = _read_active()
        self._active_lbl.config(text=f"Active: {version}")
        self._set_status(f"Core {version} activated. Restarting…")
        self.win.after(1200, self._restart_app)

    def _revert_to_default(self) -> None:
        if not messagebox.askyesno("Revert to default",
            "Restore the built-in default core?\n\n"
            "The original DLL files from the backup will be copied back.",
            parent=self.win):
            return
        ok, err = apply_core(None)
        if not ok:
            messagebox.showerror("Revert failed", err, parent=self.win)
            return
        _write_active({"version":"default","description":"Built-in (current release)"})
        self._active = _read_active()
        self._active_lbl.config(text="Active: default")
        self._set_status("Reverted to default. Restarting…")
        self.win.after(1200, self._restart_app)

    def _remove_core(self, core_dir: str, version: str) -> None:
        if self._active.get("version") == version:
            messagebox.showwarning("Cannot remove",
                "This core is currently active. Revert to default first.",
                parent=self.win)
            return
        if not messagebox.askyesno("Remove core",
            f"Delete Core {version} from disk?", parent=self.win):
            return
        shutil.rmtree(core_dir, ignore_errors=True)
        self._refresh_installed()
        self._set_status(f"Core {version} removed.")

    def _start_download(self, core_info: dict) -> None:
        ver = core_info["version"]
        self._set_status(f"Downloading Core {ver}…")
        def _progress(pct, msg):
            self.win.after(0, lambda: self._set_status(f"Core {ver}: {msg}"))
        def _bg():
            ok, err = _download_and_install(core_info, _progress)
            if ok:
                self.win.after(0, lambda: (
                    self._refresh_installed(),
                    self._refresh_available_ui(),
                    self._set_status(f"Core {ver} installed. Go to 'Installed' tab to activate.")
                ))
            else:
                self.win.after(0, lambda: (
                    messagebox.showerror("Download failed", err, parent=self.win),
                    self._set_status(f"Download failed: {err[:60]}")
                ))
        threading.Thread(target=_bg, daemon=True).start()

    def _bg_fetch_available(self) -> None:
        cores = _fetch_available_cores()
        self._available = cores
        self.win.after(0, self._refresh_available_ui)

    def _manual_refresh(self) -> None:
        self._set_status("Refreshing…")
        self._refresh_installed()
        for w in self._avail_inner.winfo_children():
            w.destroy()
        tk.Label(self._avail_inner,
                 text="⏳  Fetching…",
                 bg=self.c["bg"], fg=self.c["text_secondary"],
                 font=("Segoe UI", 10, "italic")).pack(pady=30)
        threading.Thread(target=self._bg_fetch_available, daemon=True).start()

    def _restart_app(self) -> None:
        """Restart the HomRec process with the same arguments."""
        import subprocess
        python = sys.executable
        args   = [python] + sys.argv
        try:
            if platform.system() == "Windows":
                subprocess.Popen(args, creationflags=subprocess.CREATE_NO_WINDOW)
            else:
                subprocess.Popen(args)
        except Exception as e:
            log.error("Restart failed: %s", e)
        finally:
            sys.exit(0)

    def _set_status(self, msg: str) -> None:
        self._status.config(text=msg)

    def _show_help(self) -> None:
        """Show the 'How Cores Work' explanation in a simple dialog."""
        HelpText = (
            "HomRec Core Manager\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n"
            "What is a Core?\n"
            "  A Core is a set of DLL files (hr_console.dll, homrec_core.dll, …)\n"
            "  that control HomRec's internal behaviour — capture logic, encoding\n"
            "  pipeline, console commands, etc.\n\n"
            "  Switching cores changes that behaviour while keeping all bug fixes\n"
            "  and new features of the current UI / Python layer.\n\n"
            "How to install a Core:\n"
            "  1. Open the 'Available (GitHub)' tab.\n"
            "  2. Click ⬇ Download next to a core version.\n"
            "  3. Once downloaded it appears in 'Installed'.\n"
            "  4. Click ✔ Activate — HomRec will copy the files and restart.\n\n"
            "How to add cores to GitHub (for developers / you):\n"
            "  • Build your DLL files.\n"
            "  • Create a  core_manifest.json  (see below).\n"
            "  • Zip them together as  core-<version>.zip\n"
            "  • Attach it to a GitHub Release on homaaaio/homrec.\n"
            "  • HomRec will find it automatically on the next refresh.\n\n"
            "core_manifest.json format:\n"
            "  {\n"
            "    \"version\":        \"1.4.4\",\n"
            "    \"description\":    \"Stable legacy core\",\n"
            "    \"compatible_with\": \">=1.6.0\",\n"
            "    \"files\": [\"hr_console.dll\", \"homrec_core.dll\"],\n"
            "    \"changelog\":      \"What changed in this core version…\"\n"
            "  }\n\n"
            "Reverting:\n"
            "  Click ↩ Revert to default to restore the original DLL files.\n"
            "  Your originals are backed up automatically before the first swap.\n"
        )
        dlg = tk.Toplevel(self.win)
        dlg.title("How Cores Work")
        dlg.geometry("620x560")
        dlg.configure(bg=self.c["bg"])
        dlg.resizable(True, True)
        text = tk.Text(dlg, bg=self.c["surface"], fg=self.c["text"],
                       font=("Consolas", 9), relief="flat",
                       wrap="word", padx=16, pady=12)
        text.pack(fill="both", expand=True, padx=12, pady=12)
        text.insert("1.0", HelpText)
        text.config(state="disabled")
        tk.Button(dlg, text="Close", command=dlg.destroy,
                  bg=self.c["surface"], fg=self.c["text"],
                  font=("Segoe UI", 10), relief="flat",
                  padx=14, pady=5).pack(pady=(0,10))
