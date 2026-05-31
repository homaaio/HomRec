#!/usr/bin/env python3
"""
build_native.py  —  HomRec native library builder

Run from the HomRec folder:
    python build_native.py

Requirements (Windows):
    MinGW-w64 (g++ in PATH) — recommended
      Install via MSYS2: https://www.msys2.org/
      pacman -S mingw-w64-x86_64-gcc
      Add C:\\msys64\\mingw64\\bin to PATH
    OR MSVC (cl.exe) — not used by this script, do it manually.

Output DLLs (placed next to this script):
    homrec_core.dll         pixel ops, audio RMS, resize
    hr_ringbuf.dll          lock-free ring buffer
    hr_framequeue.dll       lock-free frame pointer queue
    hr_preview.dll          thumbnail / border / overlay
    hr_encoder_helpers.dll  BGRA→YUV420p, gamma, box filter
    hr_stopwatch.dll        sub-ms frame-pacing timer
    hr_display_info.dll     monitor enumeration
    hr_dxgi_capture.dll     DXGI Desktop Duplication (DirectX, Win8+)
    hr_audio.dll            WASAPI mic + loopback audio engine
    hr_pipeline.dll         central capture/encode pipeline
    hr_tools.dll            ffmpeg helpers: GPU probe, codec args,
                            dshow devices, audio/video merge      [NEW]
"""

import subprocess
import sys
import os
import platform

HERE = os.path.dirname(os.path.abspath(__file__))

# ── ANSI colours ─────────────────────────────────────────────────────────────
_USE_COLOR = sys.stdout.isatty() and (
    platform.system() != "Windows" or os.environ.get("WT_SESSION")
)
GREEN  = "\033[92m" if _USE_COLOR else ""
RED    = "\033[91m" if _USE_COLOR else ""
YELLOW = "\033[93m" if _USE_COLOR else ""
CYAN   = "\033[96m" if _USE_COLOR else ""
RESET  = "\033[0m"  if _USE_COLOR else ""
BOLD   = "\033[1m"  if _USE_COLOR else ""


def _check_compiler() -> tuple[bool, str]:
    try:
        r = subprocess.run(["g++", "--version"], capture_output=True, timeout=10)
        if r.returncode == 0:
            return True, r.stdout.decode(errors="replace").splitlines()[0]
    except FileNotFoundError:
        pass
    return False, ""


def run(cmd: list[str], label: str, src: str) -> bool:
    """Compile one target. Returns True on success / skip."""
    src_path = os.path.join(HERE, src)
    if not os.path.exists(src_path):
        print(f"  {YELLOW}SKIP{RESET}  {label}  (source not found: {src})")
        return True  # optional module — not a hard failure

    print(f"  {CYAN}Building{RESET} {label}...", end=" ", flush=True)
    try:
        r = subprocess.run(cmd, capture_output=True, cwd=HERE, timeout=120)
        if r.returncode == 0:
            print(f"{GREEN}OK{RESET}")
            return True
        print(f"{RED}FAILED{RESET}")
        for line in r.stderr.decode(errors="replace").splitlines()[-30:]:
            print(f"    {line}")
        return False
    except FileNotFoundError:
        print(f"{RED}FAILED{RESET} — g++ not found in PATH")
        return False
    except subprocess.TimeoutExpired:
        print(f"{RED}TIMEOUT{RESET} (>120 s)")
        return False
    except Exception as e:
        print(f"{RED}ERROR{RESET}: {e}")
        return False


def main() -> None:
    is_win = platform.system() == "Windows"
    is_mac = platform.system() == "Darwin"
    so     = ".dll" if is_win else ".so"

    # Compiler flags shared across targets
    fPIC  = [] if is_win else ["-fPIC"]
    LNKST = ["-static-libgcc", "-static-libstdc++"] if is_win else []
    SSE2  = ["-msse2"] if is_win else []
    WINMM = ["-lwinmm"] if is_win else []
    LRT   = [] if (is_win or is_mac) else ["-lrt"]

    print(f"\n{BOLD}HomRec — Native Library Builder{RESET}")
    print(f"Platform : {platform.system()} {platform.machine()}")
    print(f"Directory: {HERE}\n")

    found, ver = _check_compiler()
    if not found:
        print(f"{RED}ERROR: g++ not found in PATH.{RESET}\n")
        print("Windows — install MinGW-w64 via MSYS2:")
        print("  1. Download https://www.msys2.org/")
        print("  2. Run:  pacman -S mingw-w64-x86_64-gcc")
        print("  3. Add   C:\\msys64\\mingw64\\bin   to PATH")
        print("  4. Re-run this script\n")
        sys.exit(1)

    print(f"Compiler : {ver}\n")

    # ── Build targets (order matters — pipeline last) ─────────────────────────
    targets = [
        # ── C modules ─────────────────────────────────────────────────────────
        (
            ["gcc", "-O3", "-march=native", *SSE2, "-shared", *fPIC, "-lm",
             "-o", f"homrec_core{so}", "homrec_core.c"],
            "homrec_core         (pixel ops, audio RMS, resize)",
            "homrec_core.c",
        ),
        (
            ["gcc", "-O3", "-march=native", *SSE2, "-shared", *fPIC, "-lm",
             "-o", f"hr_encoder_helpers{so}", "hr_encoder_helpers.c"],
            "hr_encoder_helpers  (BGRA→YUV420p, gamma, box filter)",
            "hr_encoder_helpers.c",
        ),

        # ── C++ utility modules ───────────────────────────────────────────────
        (
            ["g++", "-O3", "-std=c++17", "-shared", *fPIC, *LNKST,
             "-o", f"hr_ringbuf{so}", "hr_ringbuf.cpp"],
            "hr_ringbuf          (lock-free ring buffer)",
            "hr_ringbuf.cpp",
        ),
        (
            ["g++", "-O3", "-std=c++17", "-shared", *fPIC, *LNKST,
             "-o", f"hr_framequeue{so}", "hr_framequeue.cpp"],
            "hr_framequeue       (lock-free frame queue)",
            "hr_framequeue.cpp",
        ),
        (
            ["g++", "-O3", "-std=c++17", "-shared", *fPIC, *LNKST,
             "-o", f"hr_preview{so}", "hr_preview.cpp"],
            "hr_preview          (thumbnail / border / overlay)",
            "hr_preview.cpp",
        ),
        (
            ["g++", "-O3", "-std=c++17", "-shared", *fPIC, *LNKST,
             "-o", f"hr_stopwatch{so}", "hr_stopwatch.cpp", *WINMM, *LRT],
            "hr_stopwatch        (sub-ms frame-pacing timer)",
            "hr_stopwatch.cpp",
        ),

        # ── Display / capture ─────────────────────────────────────────────────
        (
            ["g++", "-O3", "-std=c++17", "-shared", *fPIC, *LNKST,
             "-o", f"hr_display_info{so}", "hr_display_info.cpp"],
            "hr_display_info     (monitor enumeration)",
            "hr_display_info.cpp",
        ),
        (
            ["g++", "-O3", "-std=c++17", "-shared", *fPIC, *LNKST,
             "-o", f"hr_dxgi_capture{so}", "hr_dxgi_capture.cpp",
             *([ "-ld3d11", "-ldxgi", "-lole32"] if is_win else [])],
            "hr_dxgi_capture     (DXGI Desktop Duplication, DirectX)",
            "hr_dxgi_capture.cpp",
        ),

        # ── Audio engine (WASAPI) ─────────────────────────────────────────────
        (
            ["g++", "-O2", "-std=c++17", "-shared", *fPIC, *LNKST,
             "-o", f"hr_audio{so}", "hr_audio.cpp",
             *([ "-lole32", "-lwinmm", "-luuid"] if is_win else [])],
            "hr_audio            (WASAPI mic + loopback engine)",
            "hr_audio.cpp",
        ),

        # ── Pipeline (central capture/encode — links DirectX + audio) ─────────
        (
            ["g++", "-O3", "-std=c++17", "-shared", *fPIC, *LNKST,
             "-o", f"hr_pipeline{so}", "hr_pipeline.cpp",
             *([ "-ld3d11", "-ldxgi", "-lole32", "-lwinmm"] if is_win else [])],
            "hr_pipeline         (central capture/encode pipeline)",
            "hr_pipeline.cpp",
        ),

        # ── Tools engine (NEW: GPU probe, codec args, dshow, merge) ───────────
        (
            ["g++", "-O2", "-std=c++17", "-shared", *fPIC, *LNKST,
             "-o", f"hr_tools{so}", "hr_tools.cpp"],
            "hr_tools            (GPU probe, codec args, dshow, av-merge) [NEW]",
            "hr_tools.cpp",
        ),
    ]

    ok = failed = skipped = 0
    for cmd, label, src in targets:
        result = run(cmd, label, src)
        if not os.path.exists(os.path.join(HERE, src)):
            skipped += 1
        elif result:
            ok += 1
        else:
            failed += 1

    # ── Summary ───────────────────────────────────────────────────────────────
    total = ok + failed
    print(f"\n{'═' * 60}")
    print(f"{BOLD}Result: {GREEN}{ok}{RESET}{BOLD}/{total}{RESET} libraries compiled successfully"
          + (f", {YELLOW}{skipped} skipped{RESET}." if skipped else "."))

    if failed == 0:
        print(f"{GREEN}All done!{RESET} Place the .dll files next to homrec.py and run it.")
        if is_win:
            print()
            print(f"  DLL              Purpose")
            print(f"  {'─'*55}")
            print(f"  hr_audio.dll     WASAPI audio — replaces PyAudio entirely")
            print(f"  hr_tools.dll     GPU probe, codec args, dshow, av-merge [NEW]")
            print(f"  hr_dxgi_capture  DirectX screen capture (enable in Settings)")
            print(f"  hr_pipeline.dll  Central encode/preview pipeline")
    else:
        print(f"{RED}{failed} library/libraries failed.{RESET}")
        print("\nCommon fixes:")
        print("  • Use MinGW-w64 64-bit (not 32-bit)")
        print("  • MSYS2: pacman -Syu  then  pacman -S mingw-w64-x86_64-gcc")
        print("  • Ensure all .cpp / .c source files are present in this folder")
        print("  • Python 3.10+ required")

    # ── List built files ──────────────────────────────────────────────────────
    dlls = [f for f in sorted(os.listdir(HERE)) if f.endswith(so)]
    if dlls:
        print(f"\nBuilt files in {HERE}:")
        for f in dlls:
            size = os.path.getsize(os.path.join(HERE, f))
            print(f"  {f:<40} {size // 1024:>5} KB")
    print()


if __name__ == "__main__":
    main()
