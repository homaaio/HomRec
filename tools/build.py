#!/usr/bin/env python3
"""HomRec build menu with a log that is easy to paste into a chat.

    python tools/build.py                 interactive menu (no TTY: mode "all")
    python tools/build.py all             hr.exe + homrec.exe
    python tools/build.py hr              hr.exe only
    python tools/build.py homrec          homrec.exe only
    python tools/build.py tests           build + ctest
    python tools/build.py bench           build bench.exe
    python tools/build.py clean           delete build/ and rebuild all
    python tools/build.py nolto           build-nolto/ with -DHR_LTO=OFF
    python tools/build.py package         run tools/homrec_build.py

Options: -j/--jobs N, --fresh (delete the build dir first), --no-lto.

Every command runs in the MSYS2 UCRT64 shell. The full output goes to logs/build/cmake.log;
on failure logs/build/last-error.txt holds the extracted errors plus the last 60 log lines,
on success logs/build/last-build.json records what was built.
"""
import argparse
import datetime
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from homrec_build import (REPO_ROOT, find_msys2_bash, to_msys_path, C, H, OK, BAD, DOT,  # noqa: E402
                          section, warn, error, success, ask_choice)

LOG_DIR = os.path.join(REPO_ROOT, "logs", "build")
LOG_FILE = os.path.join(LOG_DIR, "cmake.log")
ERR_FILE = os.path.join(LOG_DIR, "last-error.txt")
OK_FILE = os.path.join(LOG_DIR, "last-build.json")

MODES = [
    ("all", "all       - hr.exe + homrec.exe"),
    ("hr", "hr        - hr.exe only"),
    ("homrec", "homrec    - homrec.exe only"),
    ("tests", "tests     - build + ctest"),
    ("bench", "bench     - build bench.exe"),
    ("clean", "clean     - delete build/ and rebuild all"),
    ("nolto", "nolto     - build-nolto/ with -DHR_LTO=OFF"),
    ("package", "package   - run tools/homrec_build.py"),
]

# Lines worth showing from a failed build.
ERROR_RE = re.compile(
    r"fatal error:|\berror:|undefined reference to|multiple definition of|CMake Error"
    r"|mingw32-make.*Error|ld returned \d+ exit status")
PERCENT_RE = re.compile(r"^\[\s*(\d+)%\]")


def plan(mode, jobs, no_lto):
    """Returns (build_dir, [shell commands]) for a mode."""
    cfg = 'cmake -S . -B {d} -G "MinGW Makefiles"'
    build = "cmake --build {d} -j " + str(jobs)
    d = "build"
    extra = ""
    if mode == "nolto" or no_lto:
        d, extra = "build-nolto", " -DHR_LTO=OFF"
    if mode in ("tests", "bench"):
        extra += " -DHOMREC_BUILD_TESTS=ON"
    for var in ("LUA_INCLUDE_DIR", "LUA_LIB_DIR"):          # only when the user set them
        if os.environ.get(var):
            extra += f' -D{var}="{os.environ[var]}"'
    cmds = [cfg.format(d=d) + extra]
    if mode == "hr":
        cmds.append(build.format(d=d) + " --target hr")
    elif mode == "homrec":
        cmds.append(build.format(d=d) + " --target homrec")
    elif mode == "bench":
        cmds.append(build.format(d=d) + " --target bench")
    else:
        cmds.append(build.format(d=d))
    if mode == "tests":
        cmds.append(f"ctest --test-dir {d} --output-on-failure")
    return d, cmds


def _rm_readonly(func, path, _exc):
    os.chmod(path, stat.S_IWRITE)
    func(path)


def extract_errors(lines):
    seen, out = set(), []
    for ln in lines:
        if ERROR_RE.search(ln) and ln not in seen:
            seen.add(ln)
            out.append(ln)
    return out


def run_commands(cmds):
    """Runs the commands one after another in MSYS2 UCRT64. Returns (exit_code, all_lines)."""
    os.makedirs(LOG_DIR, exist_ok=True)
    bash = find_msys2_bash()
    if not bash:
        error("MSYS2 not found (set MSYS2_ROOT or install it from https://www.msys2.org/).")
        return 127, ["MSYS2 not found"]
    env = os.environ.copy()
    env["MSYSTEM"] = "UCRT64"
    env["CHERE_INVOKING"] = "1"
    lines, code = [], 0
    tty = sys.stdout.isatty()
    with open(LOG_FILE, "w", encoding="utf-8", newline="\n") as log:
        for cmd in cmds:
            print(f"  {C.DIM}$ [MSYS2 UCRT64]{C.RESET} {cmd}")
            log.write(f"$ {cmd}\n")
            proc = subprocess.Popen([bash, "-lc", f"cd '{to_msys_path(REPO_ROOT)}' && {cmd}"],
                                    cwd=REPO_ROOT, env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT)
            last_pct = -1
            for raw in proc.stdout:
                ln = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                lines.append(ln)
                log.write(ln + "\n")
                m = PERCENT_RE.match(ln)
                if m and tty and int(m.group(1)) != last_pct:
                    last_pct = int(m.group(1))
                    print(f"\r  {C.DIM}build {last_pct:3d}%{C.RESET}", end="", flush=True)
                elif ERROR_RE.search(ln):
                    if tty and last_pct >= 0:
                        print()
                        last_pct = -1
                    print(f"  {C.RED}{ln}{C.RESET}")
            code = proc.wait()
            if tty and last_pct >= 0:
                print()
            log.write(f"[exit code {code}]\n")
            if code != 0:
                break
    return code, lines


def built_files(build_dir, mode):
    names = {"all": ["hr.exe", "homrec.exe"], "hr": ["hr.exe"], "homrec": ["homrec.exe"],
             "clean": ["hr.exe", "homrec.exe"], "nolto": ["hr.exe", "homrec.exe"]}.get(mode, [])
    paths = [os.path.join(REPO_ROOT, n) for n in names]
    if mode == "bench":
        paths.append(os.path.join(REPO_ROOT, build_dir, "bench.exe"))
    return [p for p in paths if os.path.isfile(p)]


def write_error_file(mode, cmds, code, lines):
    errs = extract_errors(lines)
    with open(ERR_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"time: {datetime.datetime.now().isoformat(timespec='seconds')}\n")
        f.write(f"mode: {mode}\nexit code: {code}\ncommands:\n")
        for c in cmds:
            f.write(f"  {c}\n")
        f.write(f"\nerrors ({len(errs)}):\n")
        for e in errs:
            f.write(f"  {e}\n")
        f.write("\nlast 60 log lines:\n")
        f.write("\n".join(lines[-60:]) + "\n")
    return errs


def main():
    ap = argparse.ArgumentParser(description="HomRec build menu")
    ap.add_argument("mode", nargs="?", choices=[m for m, _ in MODES])
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--fresh", action="store_true", help="delete the build dir before building")
    ap.add_argument("--no-lto", action="store_true", help="same as building into build-nolto/ without LTO")
    args = ap.parse_args()

    mode = args.mode
    if mode is None:
        if sys.stdin.isatty():
            section("HomRec build")
            mode = ask_choice("Build", [(m, label) for m, label in MODES])
        else:
            mode = "all"

    if mode == "package":
        sys.exit(subprocess.call([sys.executable, os.path.join(REPO_ROOT, "tools", "homrec_build.py")]))

    build_dir, cmds = plan(mode, max(1, args.jobs), args.no_lto)
    section(f"Build: {mode}")
    if mode == "clean" or args.fresh:
        target = os.path.join(REPO_ROOT, build_dir)     # only the selected dir, never recordings/ or dist/
        if os.path.isdir(target):
            print(f"  {C.DIM}removing {build_dir}/{C.RESET}")
            shutil.rmtree(target, onerror=_rm_readonly)

    t0 = time.time()
    code, lines = run_commands(cmds)
    secs = time.time() - t0

    if code == 0:
        files = built_files(build_dir, mode)
        with open(OK_FILE, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"time": datetime.datetime.now().isoformat(timespec="seconds"), "mode": mode,
                       "build_dir": build_dir, "seconds": round(secs, 1), "commands": cmds,
                       "exit_code": 0,
                       "files": [os.path.relpath(p, REPO_ROOT) for p in files]}, f, indent=2)
        success(f"done in {secs:.0f} s")
        for p in files:
            st = os.stat(p)
            when = datetime.datetime.fromtimestamp(st.st_mtime).strftime("%Y-%m-%d %H:%M:%S")
            print(f"  {C.GREEN}{DOT}{C.RESET} {os.path.relpath(p, REPO_ROOT)}  {st.st_size // 1024} KB  {when}")
        return 0

    errs = write_error_file(mode, cmds, code, lines)
    error(f"build failed (exit code {code}) after {secs:.0f} s")
    for e in errs[:20]:
        print(f"  {C.RED}{BAD}{C.RESET} {e}")
    if len(errs) > 20:
        print(f"  ... and {len(errs) - 20} more in the error file")
    print(f"  full log:   {os.path.relpath(LOG_FILE, REPO_ROOT)}")
    print(f"  error file: {os.path.relpath(ERR_FILE, REPO_ROOT)}")
    return code if code else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print(f"\n{C.YELLOW}Interrupted by user.{C.RESET}")
        sys.exit(1)
