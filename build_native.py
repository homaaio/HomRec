#!/usr/bin/env python3
"""
build_native.py  -  HomRec v1.5.0  one-click native library builder

Run this script once from the HomRec folder to compile all C/C++ libraries:

    python build_native.py

Requirements:
  - Windows: MinGW-w64 (gcc/g++ in PATH)  or  MSVC (cl in PATH)
  - Linux/macOS: gcc and g++ installed

Output: homrec_core.dll/.so, hr_ringbuf.dll/.so, hr_framequeue.dll/.so,
        hr_preview.dll/.so, hr_encoder_helpers.dll/.so,
        hr_stopwatch.dll/.so, hr_display_info.dll/.so

FIXES vs v1.5.0:
  - hr_stopwatch: added -lwinmm to linker flags on Windows.
    Without it, timeGetDevCaps / timeBeginPeriod / timeEndPeriod are
    undefined references (they live in winmm.dll, not the default libs).
  - hr_encoder_helpers: added -msse2 flag so MinGW defines __SSE2__ and
    pulls in <emmintrin.h>, making _mm_sfence and _mm_storeu_si128 resolve.
  - hr_display_info: (source fix — see hr_display_info.cpp; no build change).
  - Linux rt library: hr_stopwatch links -lrt on Linux for clock_gettime.
"""

import subprocess
import sys
import os
import platform

HERE = os.path.dirname(os.path.abspath(__file__))

def run(cmd, label):
    print(f"  Building {label}...", end=" ", flush=True)
    try:
        r = subprocess.run(cmd, capture_output=True, cwd=HERE, timeout=120)
        if r.returncode == 0:
            print("OK")
            return True
        else:
            print("FAILED")
            print(f"    stderr: {r.stderr.decode(errors='replace')[-800:]}")
            return False
    except FileNotFoundError as e:
        print(f"FAILED (compiler not found: {e})")
        return False
    except Exception as e:
        print(f"ERROR: {e}")
        return False

def main():
    is_win  = platform.system() == "Windows"
    is_mac  = platform.system() == "Darwin"
    so      = ".dll" if is_win else ".so"
    fPIC    = [] if is_win else ["-fPIC"]
    # FIX: -lwinmm is required for timeGetDevCaps/timeBeginPeriod/timeEndPeriod
    WINMM   = ["-lwinmm"] if is_win else []
    # FIX: clock_gettime lives in -lrt on older Linux glibc (no-op on macOS)
    LRT     = [] if (is_win or is_mac) else ["-lrt"]
    LNKST   = ["-static-libgcc", "-static-libstdc++"] if is_win else []
    # FIX: explicit -msse2 ensures MinGW defines __SSE2__ and finds emmintrin.h
    SSE2    = ["-msse2"] if is_win else []

    print(f"\nHomRec v1.5.0 — Native Library Builder")
    print(f"Platform: {platform.system()} | Output dir: {HERE}\n")

    targets = [
        (
            ["gcc", "-O3", "-march=native", *SSE2, "-shared", *fPIC, "-lm",
             "-o", f"homrec_core{so}", "homrec_core.c"],
            "homrec_core  (pixel ops, audio RMS, resize)"
        ),
        (
            ["g++", "-O3", "-std=c++17", "-shared", *fPIC, *LNKST,
             "-o", f"hr_ringbuf{so}", "hr_ringbuf.cpp"],
            "hr_ringbuf   (lock-free audio ring buffer)"
        ),
        (
            ["g++", "-O3", "-std=c++17", "-shared", *fPIC, *LNKST,
             "-o", f"hr_framequeue{so}", "hr_framequeue.cpp"],
            "hr_framequeue (lock-free frame pointer queue)"
        ),
        (
            ["g++", "-O3", "-std=c++17", "-shared", *fPIC, *LNKST,
             "-o", f"hr_preview{so}", "hr_preview.cpp"],
            "hr_preview   (thumbnail, border, gray overlay)"
        ),
        (
            # FIX: added -msse2 so <emmintrin.h> / _mm_sfence resolve in MinGW
            ["gcc", "-O3", "-march=native", *SSE2, "-shared", *fPIC, "-lm",
             "-o", f"hr_encoder_helpers{so}", "hr_encoder_helpers.c"],
            "hr_encoder_helpers (BGRA->YUV420p, gamma, box thumbnail)"
        ),
        (
            # FIX: -lwinmm must come AFTER the source file on MinGW's linker.
            # GCC/ld resolves symbols left-to-right; a -l flag placed before
            # the .o has no unresolved symbols yet so winmm is silently dropped.
            ["g++", "-O3", "-std=c++17", "-shared", *fPIC, *LNKST,
             "-o", f"hr_stopwatch{so}", "hr_stopwatch.cpp", *WINMM],
            "hr_stopwatch (sub-ms frame pacing timer)"
        ),
        (
            ["g++", "-O3", "-std=c++17", "-shared", *fPIC, *LNKST,
             "-o", f"hr_display_info{so}", "hr_display_info.cpp"],
            "hr_display_info (monitor enumeration)"
        ),
    ]

    ok = 0
    for cmd, label in targets:
        if run(cmd, label):
            ok += 1

    print(f"\n{'='*52}")
    print(f"Built {ok}/{len(targets)} libraries successfully.")
    if ok == len(targets):
        print("All native libraries compiled! HomRec will use them automatically.")
    else:
        print("Some libraries failed. HomRec will use Python fallbacks for those.")
        print("\nTroubleshooting:")
        print("  Windows — install MinGW-w64 via MSYS2:")
        print("    https://www.msys2.org/")
        print("    pacman -S mingw-w64-x86_64-gcc")
        print("    Add C:\\msys64\\mingw64\\bin to PATH")
        print("  Linux   — sudo apt install gcc g++  (or equivalent)")
    print()

if __name__ == "__main__":
    main()
