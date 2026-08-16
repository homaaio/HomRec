#!/usr/bin/env python3
"""
homrec_check.py - Environment sanity check + smoke test for HomRec.

Run this on Windows with: python homrec_check.py
No third-party packages needed (stdlib only).

What it does:
  1. Checks CPU/RAM against what a DXGI screen recorder realistically needs.
  2. Checks disk free space in the configured output folder.
  3. Checks ffmpeg is reachable.
  4. Validates homrec_settings.json is present and parses as JSON.
  5. (Optional) Launches HomRec.exe, waits a few seconds, and greps
     homrec.log for known failure strings ("Failed to start",
     "dx_create() returned null", "did not stop in time").

This can't replace actually running the app - it just catches the
"obviously not going to work" cases before you waste time launching it.
"""

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


def section(title):
    print(f"\n=== {title} ===")


def check_cpu_ram():
    section("CPU / RAM")
    print(f"CPU: {platform.processor() or '(unknown)'}")
    print(f"Logical cores: {os.cpu_count()}")

    if platform.system() != "Windows":
        print("Not on Windows - skipping RAM check (script is meant to run on the target machine).")
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
        print(f"WARNING: below the ~{MIN_RAM_MB} MB minimum. Recording will likely "
              f"page/stutter, especially at higher preview FPS or resolution.")
    elif total_mb < RECOMMENDED_RAM_MB:
        print(f"Below the recommended ~{RECOMMENDED_RAM_MB} MB - should run, but keep "
              f"preview FPS/quality low and close other apps while recording.")
    else:
        print("RAM looks fine.")


def check_disk(output_folder):
    section("Disk space")
    output_folder = os.path.expandvars(output_folder)
    if not os.path.isdir(output_folder):
        print(f"Output folder does not exist yet: {output_folder} (will be created on first run)")
        output_folder = os.path.dirname(output_folder) or "."
    try:
        usage = shutil.disk_usage(output_folder)
        free_gb = usage.free / (1024 ** 3)
        print(f"Free space at {output_folder}: {free_gb:.1f} GB")
        if free_gb < MIN_FREE_DISK_GB:
            print(f"WARNING: less than {MIN_FREE_DISK_GB} GB free - long recordings may fail mid-way.")
    except OSError as e:
        print(f"Could not check disk space: {e}")


def check_ffmpeg():
    section("ffmpeg")
    exe = shutil.which("ffmpeg") or (
        "ffmpeg.exe" if os.path.isfile("ffmpeg.exe") else None
    )
    if not exe:
        print("ffmpeg NOT found on PATH or next to this script. Encoding will fail without it.")
        return
    try:
        out = subprocess.run([exe, "-version"], capture_output=True, text=True, timeout=10)
        first_line = out.stdout.splitlines()[0] if out.stdout else "(no output)"
        print(f"Found: {exe}")
        print(first_line)
    except Exception as e:
        print(f"ffmpeg found but failed to run: {e}")


def check_settings(settings_path):
    section("homrec_settings.json")
    if not os.path.isfile(settings_path):
        print(f"Not found at {settings_path} (fine on first run - defaults will be used).")
        return
    try:
        with open(settings_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        print(f"Parsed OK, {len(data)} top-level keys: {', '.join(sorted(data.keys()))}")
        fps = data.get("fps") or data.get("target_fps")
        preview_fps = data.get("preview_fps")
        if fps:
            print(f"Recording FPS: {fps}")
        if preview_fps:
            print(f"Preview FPS: {preview_fps}")
            if preview_fps > 15:
                print("Consider lowering Preview FPS on this machine - it directly "
                      "drives the idle-preview capture rate (see hr_pipeline.cpp).")
    except json.JSONDecodeError as e:
        print(f"INVALID JSON: {e}. The app may fall back to defaults, or fail to start.")


def smoke_test(exe_path, log_path, wait_seconds=6):
    section("Smoke test (launch + check log)")
    if not exe_path or not os.path.isfile(exe_path):
        print("No HomRec.exe path given/found - skipping. Pass it as the 3rd argument.")
        return
    print(f"Launching {exe_path} ...")
    try:
        proc = subprocess.Popen([exe_path])
    except Exception as e:
        print(f"Failed to launch: {e}")
        return

    time.sleep(wait_seconds)

    bad_strings = ["Failed to start", "dx_create() returned null", "did not stop in time"]
    found_issue = False
    if os.path.isfile(log_path):
        with open(log_path, "r", errors="ignore") as f:
            tail = f.readlines()[-200:]
        for line in tail:
            if any(b in line for b in bad_strings):
                print(f"LOG ISSUE: {line.strip()}")
                found_issue = True
        if not found_issue:
            print("No known failure strings found in the last 200 log lines.")
    else:
        print(f"Log file not found at {log_path} yet.")

    proc.terminate()
    print("Smoke test finished (app terminated).")


if __name__ == "__main__":
    output_folder = sys.argv[1] if len(sys.argv) > 1 else os.path.expandvars(r"%USERPROFILE%\Videos")
    settings_path = sys.argv[2] if len(sys.argv) > 2 else os.path.expandvars(r"%APPDATA%\HomRec\homrec_settings.json")
    exe_path = sys.argv[3] if len(sys.argv) > 3 else None
    log_path = sys.argv[4] if len(sys.argv) > 4 else "homrec.log"

    check_cpu_ram()
    check_disk(output_folder)
    check_ffmpeg()
    check_settings(settings_path)
    if exe_path:
        smoke_test(exe_path, log_path)

    print("\nDone.")
