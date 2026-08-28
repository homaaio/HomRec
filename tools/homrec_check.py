#!/usr/bin/env python3
import ctypes
import json
import os
import platform
import shutil
import subprocess
import sys
import time

MIN_RAM_MB = 4096       # HomRec + Windows + wxWidgets + ffmpeg encoding
RECOMMENDED_RAM_MB = 8192
MIN_FREE_DISK_GB = 5

LOG_FAILURE_STRINGS = [
    "Failed to start",
    "dx_create() returned null",
    "did not stop in time",
]

results = []  # list of (level, message) - level is "OK", "WARN", or "FAIL"


def record(level, message):
    results.append((level, message))
    tag = {"OK": "  ok  ", "WARN": " warn ", "FAIL": " FAIL "}[level]
    print(f"[{tag}] {message}")


def section(title):
    print(f"\n=== {title} ===")


# ---------------------------------------------------------------------------
# Locating hr.exe and the files that live next to it
# ---------------------------------------------------------------------------

def find_hr_exe(explicit_path):
    if explicit_path:
        if os.path.isfile(explicit_path):
            return os.path.abspath(explicit_path)
        print(f"Given path does not exist: {explicit_path}")
        return None

    script_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(os.getcwd(), "hr.exe"),
        os.path.join(script_dir, "..", "hr.exe"),          # tools/ -> repo root (dev checkout)
        os.path.join(script_dir, "..", "dist", "hr.exe"),  # freshly built by homrec_build.py
    ]
    for c in candidates:
        if os.path.isfile(c):
            return os.path.abspath(c)

    which = shutil.which("hr.exe") or shutil.which("hr")
    if which:
        return os.path.abspath(which)
    return None


def run_quiet(args, timeout=10):
    """Best-effort subprocess run; returns stdout text or None."""
    try:
        out = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        return out.stdout
    except Exception:
        return None


# ---------------------------------------------------------------------------
# CPU / RAM
# ---------------------------------------------------------------------------

def check_cpu_ram():
    section("CPU / RAM")
    print(f"CPU: {platform.processor() or '(unknown)'}")
    print(f"Logical cores: {os.cpu_count()}")

    if platform.system() != "Windows":
        record("WARN", "Not running on Windows - skipping RAM check (HomRec itself is Windows-only).")
        return

    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [
            ("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
            ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
            ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
            ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
            ("sullAvailExtendedVirtual", ctypes.c_ulonglong),
        ]

    stat = MEMORYSTATUSEX()
    stat.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(stat))
    total_mb = stat.ullTotalPhys // (1024 * 1024)
    avail_mb = stat.ullAvailPhys // (1024 * 1024)
    print(f"Total RAM: {total_mb} MB, currently free: {avail_mb} MB")

    if total_mb < MIN_RAM_MB:
        record("FAIL", f"RAM below the ~{MIN_RAM_MB} MB minimum - expect paging/stutter, "
                        f"especially at higher preview FPS or resolution.")
    elif total_mb < RECOMMENDED_RAM_MB:
        record("WARN", f"RAM below the recommended ~{RECOMMENDED_RAM_MB} MB - should run, but keep "
                        f"preview FPS/quality low and close other apps while recording.")
    else:
        record("OK", "RAM looks fine.")


# ---------------------------------------------------------------------------
# Windows version + GPU (DXGI Desktop Duplication needs both)
# ---------------------------------------------------------------------------

def check_windows_version():
    section("Windows version")
    if platform.system() != "Windows":
        record("WARN", "Not on Windows - skipping (see note above).")
        return
    ver = sys.getwindowsversion()
    print(f"Windows version: {ver.major}.{ver.minor} build {ver.build}")
    # DXGI Desktop Duplication (IDXGIOutputDuplication), which
    # hr_dxgi_capture.cpp is built on, requires Windows 8 (NT 6.2) or newer.
    if ver.major < 6 or (ver.major == 6 and ver.minor < 2):
        record("FAIL", "Windows 7 or older detected - HomRec's capture backend (DXGI Desktop "
                        "Duplication) requires Windows 8 or newer and will not initialize here.")
    else:
        record("OK", "Windows version supports DXGI Desktop Duplication.")


def check_gpu():
    section("GPU / display adapter")
    if platform.system() != "Windows":
        record("WARN", "Not on Windows - skipping GPU check.")
        return

    ps_cmd = [
        "powershell", "-NoProfile", "-Command",
        "Get-CimInstance Win32_VideoController | "
        "Select-Object -Property Name,DriverVersion,AdapterRAM,CurrentHorizontalResolution,"
        "CurrentVerticalResolution | ConvertTo-Json",
    ]
    out = run_quiet(ps_cmd)
    adapters = []
    if out:
        try:
            data = json.loads(out)
            adapters = data if isinstance(data, list) else [data]
        except json.JSONDecodeError:
            adapters = []

    if not adapters:
        wmic_out = run_quiet(["wmic", "path", "win32_VideoController", "get",
                               "name,driverversion", "/format:list"])
        if wmic_out:
            print(wmic_out.strip())
            record("WARN", "Could not parse GPU info as JSON (used wmic fallback above) - "
                            "check manually that this isn't a remote-desktop/basic display adapter.")
        else:
            record("WARN", "Could not query GPU info (powershell/wmic unavailable or blocked) - "
                            "skipping this check.")
        return

    found_real_gpu = False
    for a in adapters:
        name = a.get("Name") or "(unknown adapter)"
        driver = a.get("DriverVersion") or "(unknown driver)"
        res_w = a.get("CurrentHorizontalResolution")
        res_h = a.get("CurrentVerticalResolution")
        res = f"{res_w}x{res_h}" if res_w and res_h else "no active output"
        print(f"Adapter: {name} | driver {driver} | {res}")
        if "microsoft basic render" not in name.lower() and "remote display" not in name.lower():
            found_real_gpu = True

    if found_real_gpu:
        record("OK", "A real GPU/display adapter is present.")
    else:
        record("FAIL", "Only a basic/remote-display adapter was found - DXGI Desktop Duplication "
                        "capture will most likely fail to initialize (dx_create() returned null) "
                        "on this session (common over RDP or in a VM without GPU passthrough).")


# ---------------------------------------------------------------------------
# Disk
# ---------------------------------------------------------------------------

def check_disk(output_folder):
    section("Disk space")
    output_folder = os.path.expandvars(output_folder)
    check_dir = output_folder
    if not os.path.isdir(check_dir):
        print(f"Output folder does not exist yet: {check_dir} (will be created on first run)")
        check_dir = os.path.dirname(check_dir) or "."

    try:
        usage = shutil.disk_usage(check_dir)
        free_gb = usage.free / (1024 ** 3)
        print(f"Free space at {check_dir}: {free_gb:.1f} GB")
        if free_gb < MIN_FREE_DISK_GB:
            record("WARN", f"Less than {MIN_FREE_DISK_GB} GB free - long recordings may fail mid-way.")
        else:
            record("OK", "Enough free disk space.")
    except OSError as e:
        record("FAIL", f"Could not check disk space: {e}")
        return

    try:
        os.makedirs(check_dir, exist_ok=True)
        probe = os.path.join(check_dir, ".homrec_check_write_test.tmp")
        with open(probe, "wb") as f:
            f.write(b"ok")
        os.remove(probe)
        record("OK", "Output folder is writable.")
    except OSError as e:
        record("FAIL", f"Output folder is NOT writable ({e}) - recordings will fail to save here.")


# ---------------------------------------------------------------------------
# ffmpeg
# ---------------------------------------------------------------------------

def check_ffmpeg(hr_exe):
    section("ffmpeg")
    next_to_exe = os.path.join(os.path.dirname(hr_exe), "ffmpeg.exe") if hr_exe else None
    exe = (
        (next_to_exe if next_to_exe and os.path.isfile(next_to_exe) else None)
        or shutil.which("ffmpeg")
        or ("ffmpeg.exe" if os.path.isfile("ffmpeg.exe") else None)
    )
    if not exe:
        record("FAIL", "ffmpeg NOT found next to hr.exe or on PATH - encoding will fail without it.")
        return
    out = run_quiet([exe, "-version"])
    if out:
        first_line = out.splitlines()[0] if out else "(no output)"
        print(f"Found: {exe}")
        print(first_line)
        record("OK", "ffmpeg is reachable and runs.")
    else:
        record("FAIL", f"ffmpeg found at {exe} but failed to run.")


# ---------------------------------------------------------------------------
# Settings: current .hrc, or legacy homrec_settings.json
# ---------------------------------------------------------------------------

def check_settings(install_dir):
    section("Settings (homrec.hrc / homrec_settings.json)")
    hrc_path = os.path.join(install_dir, "homrec.hrc")
    legacy_path = os.path.join(install_dir, "homrec_settings.json")

    if os.path.isfile(hrc_path):
        print(f"Found current-format config: {hrc_path}")
        try:
            with open(hrc_path, "r", encoding="utf-8", errors="ignore") as f:
                lines = [l for l in f if l.strip() and not l.strip().startswith("#")]
            print(f"  {len(lines)} setting(s) present.")
            record("OK", "homrec.hrc parses as a non-empty key=value file.")
        except OSError as e:
            record("WARN", f"Found homrec.hrc but couldn't read it: {e}")
        return

    if os.path.isfile(legacy_path):
        print(f"No homrec.hrc yet - found legacy {legacy_path} (pre-2.0 / not-yet-migrated "
              f"format). This still works but will be migrated automatically on next launch.")
        try:
            with open(legacy_path, "r", encoding="utf-8") as f:
                data = json.load(f)
            print(f"Parsed OK, {len(data)} top-level keys: {', '.join(sorted(data.keys()))}")
            record("OK", "Legacy homrec_settings.json parses fine.")
        except json.JSONDecodeError as e:
            record("FAIL", f"homrec_settings.json is INVALID JSON ({e}) - app may fall back to "
                            f"defaults or fail to start.")
        return

    record("WARN", f"No homrec.hrc or homrec_settings.json in {install_dir} - fine on first run, "
                    f"defaults will be used and homrec.hrc created on exit.")


# ---------------------------------------------------------------------------
# cfg/autoexec.cfg, cfg/startrec.cfg
# ---------------------------------------------------------------------------

def check_cfg_scripts(install_dir):
    section("cfg/ scripts")
    cfg_dir = os.path.join(install_dir, "cfg")
    if not os.path.isdir(cfg_dir):
        print("No cfg/ folder next to hr.exe yet - fine, it's auto-created on first run.")
        return

    for name in ("autoexec.cfg", "startrec.cfg"):
        path = os.path.join(cfg_dir, name)
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "r", encoding="utf-8", errors="ignore") as f:
                lines = f.readlines()
            commands = [l.strip() for l in lines
                        if l.strip() and not l.strip().startswith(("//", "#"))]
            print(f"{name}: {len(commands)} command line(s), {len(lines)} line(s) total.")
            record("OK", f"{name} is readable text with {len(commands)} command(s).")
        except OSError as e:
            record("WARN", f"Could not read {name}: {e}")


# ---------------------------------------------------------------------------
# plugins/*/plugin.json
# ---------------------------------------------------------------------------

def check_plugins(install_dir):
    section("plugins/")
    plugins_dir = os.path.join(install_dir, "plugins")
    if not os.path.isdir(plugins_dir):
        print("No plugins/ folder next to hr.exe - nothing to check.")
        return

    checked = 0
    for entry in sorted(os.listdir(plugins_dir)):
        plugin_json = os.path.join(plugins_dir, entry, "plugin.json")
        if not os.path.isfile(plugin_json):
            continue
        checked += 1
        try:
            with open(plugin_json, "r", encoding="utf-8") as f:
                data = json.load(f)
            name = data.get("name", entry)
            version = data.get("version", "?")
            author = data.get("author", "-")
            print(f"  {entry}: {name} v{version} (author: {author}) - OK")
        except json.JSONDecodeError as e:
            record("FAIL", f"plugins/{entry}/plugin.json is invalid JSON ({e}) - this plugin "
                            f"will fail to load.")
        except OSError as e:
            record("WARN", f"Could not read plugins/{entry}/plugin.json: {e}")

    if checked == 0:
        print("No plugin.json files found under plugins/.")
    else:
        record("OK", f"Checked {checked} plugin(s) under plugins/.")


# ---------------------------------------------------------------------------
# logs/
# ---------------------------------------------------------------------------

def grep_log_tail(path, label):
    if not os.path.isfile(path):
        print(f"{label}: not found yet at {path}")
        return
    with open(path, "r", errors="ignore") as f:
        tail = f.readlines()[-200:]
    hits = [l.strip() for l in tail if any(b in l for b in LOG_FAILURE_STRINGS)]
    if hits:
        for h in hits:
            record("FAIL", f"{label}: {h}")
    else:
        record("OK", f"{label}: no known failure strings in the last 200 lines.")


def check_logs(install_dir):
    section("logs/")
    logs_dir = os.path.join(install_dir, "logs")
    if not os.path.isdir(logs_dir):
        print(f"No logs/ folder yet at {logs_dir} - fine before the first launch.")
        return
    grep_log_tail(os.path.join(logs_dir, "homrec.log"), "logs/homrec.log")
    grep_log_tail(os.path.join(logs_dir, "plugins.log"), "logs/plugins.log")
    pc_log = os.path.join(logs_dir, "pc.log")
    if os.path.isfile(pc_log):
        print(f"logs/pc.log present ({os.path.getsize(pc_log)} bytes).")


# ---------------------------------------------------------------------------
# Optional smoke test
# ---------------------------------------------------------------------------

def smoke_test(hr_exe, install_dir, wait_seconds=6):
    section("Smoke test (launch hr.exe + check log)")
    print(f"Launching {hr_exe} ...")
    try:
        proc = subprocess.Popen([hr_exe])
    except Exception as e:
        record("FAIL", f"Failed to launch hr.exe: {e}")
        return

    time.sleep(wait_seconds)
    grep_log_tail(os.path.join(install_dir, "logs", "homrec.log"), "logs/homrec.log (post-launch)")

    proc.terminate()
    print("Smoke test finished (app terminated).")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    print("=" * 60)
    print("  HomRec environment check")
    print("=" * 60)

    explicit_exe = sys.argv[1] if len(sys.argv) > 1 else None
    run_smoke = "--launch" in sys.argv
    output_folder = os.path.expandvars(r"%USERPROFILE%\Videos")

    hr_exe = find_hr_exe(explicit_exe)
    if hr_exe:
        install_dir = os.path.dirname(hr_exe)
        print(f"Using hr.exe: {hr_exe}")
    else:
        install_dir = os.getcwd()
        print("Could not locate hr.exe automatically - checking config/logs/plugins relative "
              f"to the current folder ({install_dir}) instead. Pass its path as the first "
              "argument to check the real install, e.g.:\n"
              "    python homrec_check.py C:\\HomRec\\hr.exe")

    check_cpu_ram()
    check_windows_version()
    check_gpu()
    check_disk(output_folder)
    check_ffmpeg(hr_exe)
    check_settings(install_dir)
    check_cfg_scripts(install_dir)
    check_plugins(install_dir)
    check_logs(install_dir)

    if run_smoke:
        if hr_exe:
            smoke_test(hr_exe, install_dir)
        else:
            print("\n--launch given but hr.exe wasn't found - skipping smoke test.")

    section("Summary")
    n_ok = sum(1 for lvl, _ in results if lvl == "OK")
    n_warn = sum(1 for lvl, _ in results if lvl == "WARN")
    n_fail = sum(1 for lvl, _ in results if lvl == "FAIL")
    print(f"{n_ok} ok, {n_warn} warning(s), {n_fail} failure(s).")
    if n_fail:
        print("\nFailures found - fix these first, they're likely why recording/starting fails:")
        for lvl, msg in results:
            if lvl == "FAIL":
                print(f"  - {msg}")
    elif n_warn:
        print("No failures, but check the warnings above if something still isn't working.")
    else:
        print("Everything checked out.")

    return 1 if n_fail else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nПрервано пользователем.")
        sys.exit(1)
