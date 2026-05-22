#!/usr/bin/env python3
"""
build_native.py  —  HomRec v1.5.0
Compiles the C / C++ native performance extensions.

Usage:
    python build_native.py          # auto-detect compiler
    python build_native.py --check  # just check if already built
    python build_native.py --clean  # remove built files then rebuild
"""

from __future__ import annotations
import os, sys, subprocess, platform, shutil, argparse
from pathlib import Path

HERE    = Path(__file__).parent
IS_WIN  = platform.system() == "Windows"
EXT     = ".dll" if IS_WIN else ".so"
TARGETS = [
    # (source,           output,              compiler, extra_flags)
    ("homrec_core.c",    f"homrec_core{EXT}", "gcc",
     ["-O3", "-march=native", "-shared",
      *([] if IS_WIN else ["-fPIC"]),
      "-lm"]),
    ("hr_ringbuf.cpp",   f"hr_ringbuf{EXT}",  "g++",
     ["-O3", "-std=c++17", "-shared",
      *([] if IS_WIN else ["-fPIC"])]),
    ("hr_framequeue.cpp",f"hr_framequeue{EXT}","g++",
     ["-O3", "-std=c++17", "-shared",
      *([] if IS_WIN else ["-fPIC"])]),
]

ANSI_GREEN  = "\033[32m"
ANSI_RED    = "\033[31m"
ANSI_YELLOW = "\033[33m"
ANSI_RESET  = "\033[0m"

def ok(msg):   print(f"{ANSI_GREEN}  ✓ {msg}{ANSI_RESET}")
def err(msg):  print(f"{ANSI_RED}  ✗ {msg}{ANSI_RESET}")
def warn(msg): print(f"{ANSI_YELLOW}  ! {msg}{ANSI_RESET}")


def check_compiler(name: str) -> bool:
    return shutil.which(name) is not None


def build_target(src: str, out: str, compiler: str, flags: list[str]) -> bool:
    src_path = HERE / src
    out_path = HERE / out
    if not src_path.exists():
        err(f"Source not found: {src_path}")
        return False
    cmd = [compiler, *flags, "-o", str(out_path), str(src_path)]
    print(f"  Building {out} …  ", end="", flush=True)
    try:
        result = subprocess.run(
            cmd, capture_output=True, timeout=120, cwd=str(HERE)
        )
        if result.returncode == 0:
            size_kb = out_path.stat().st_size // 1024
            ok(f"{out}  ({size_kb} KB)")
            return True
        else:
            err(f"{out} FAILED")
            stderr = result.stderr.decode(errors="replace")
            print(f"      {stderr[:600]}")
            return False
    except FileNotFoundError:
        err(f"Compiler '{compiler}' not found in PATH")
        return False
    except subprocess.TimeoutExpired:
        err(f"Build timeout for {src}")
        return False


def all_built() -> bool:
    return all((HERE / out).exists() for _, out, *_ in TARGETS)


def clean():
    for _, out, *_ in TARGETS:
        p = HERE / out
        if p.exists():
            p.unlink()
            print(f"  Removed {out}")


def main():
    parser = argparse.ArgumentParser(description="Build HomRec native extensions")
    parser.add_argument("--check", action="store_true",
                        help="Report status without building")
    parser.add_argument("--clean", action="store_true",
                        help="Remove built files then rebuild")
    args = parser.parse_args()

    print(f"\nHomRec v1.5.0 — Native Extension Builder")
    print(f"Platform: {platform.system()} {platform.machine()}  Python {sys.version.split()[0]}\n")

    if args.check:
        for _, out, *_ in TARGETS:
            p = HERE / out
            if p.exists():
                ok(f"{out} exists ({p.stat().st_size // 1024} KB)")
            else:
                warn(f"{out} missing")
        return 0

    if args.clean:
        print("Cleaning previous build …")
        clean()
        print()

    # Check compilers
    missing = []
    for _, _, compiler, _ in TARGETS:
        if not check_compiler(compiler):
            missing.append(compiler)
    missing = list(dict.fromkeys(missing))  # deduplicate
    if missing:
        err(f"Required compiler(s) not found: {', '.join(missing)}")
        print("\n  Install GCC / G++:")
        if IS_WIN:
            print("    winget install MSYS2.MSYS2  →  pacman -S mingw-w64-x86_64-gcc")
        else:
            print("    Ubuntu/Debian:  sudo apt install build-essential")
            print("    Fedora:         sudo dnf install gcc gcc-c++")
            print("    macOS:          xcode-select --install")
        return 1

    # Build
    results = []
    for src, out, compiler, flags in TARGETS:
        ok_flag = build_target(src, out, compiler, flags)
        results.append(ok_flag)

    print()
    if all(results):
        ok("All extensions built successfully!")
        print("\n  Run HomRec normally — it will load them automatically.\n")
        return 0
    else:
        n_fail = results.count(False)
        warn(f"{n_fail} extension(s) failed — HomRec will use Python fallbacks.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
