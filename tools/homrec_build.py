#!/usr/bin/env python3
# btw im from windows
import contextlib
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import threading
import time
import zipfile

# ---------------------------------------------------------------------------
# Pretty-printing helpers: colors, unicode, bouncing loading bar
# ---------------------------------------------------------------------------

def _enable_ansi():
    """Enable ANSI escape codes on Windows 10+ consoles."""
    if os.name != "nt":
        return True
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        mode = ctypes.c_uint32()
        if not kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            return False
        ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
        kernel32.SetConsoleMode(handle, mode.value | ENABLE_VIRTUAL_TERMINAL_PROCESSING)
        return True
    except Exception:
        return False

_ANSI_ENABLED = _enable_ansi()

def _stdout_is_tty():
    return hasattr(sys.stdout, "isatty") and sys.stdout.isatty()

def _color_enabled():
    if not _stdout_is_tty():
        return False
    if os.environ.get("NO_COLOR"):
        return False
    if os.name == "nt" and not _ANSI_ENABLED:
        return False
    return True

def _unicode_ok():
    enc = getattr(sys.stdout, "encoding", None) or "utf-8"
    try:
        for ch in "─│▸●✓✗█░":
            ch.encode(enc)
        return True
    except (UnicodeEncodeError, LookupError):
        return False

_COLOR = _color_enabled()
_UNI = _unicode_ok()
_ANIMATE = _COLOR  # animation needs ANSI (cursor hide + erase-line)

class C:
    RESET   = "\033[0m"  if _COLOR else ""
    BOLD    = "\033[1m"  if _COLOR else ""
    DIM     = "\033[2m"  if _COLOR else ""
    RED     = "\033[31m" if _COLOR else ""
    GREEN   = "\033[32m" if _COLOR else ""
    YELLOW  = "\033[33m" if _COLOR else ""
    BLUE    = "\033[34m" if _COLOR else ""
    MAGENTA = "\033[35m" if _COLOR else ""
    CYAN    = "\033[36m" if _COLOR else ""
    WHITE   = "\033[37m" if _COLOR else ""

H      = "─" if _UNI else "-"
ARROW  = "▸" if _UNI else ">"
OK     = "✓" if _UNI else "[OK]"
BAD    = "✗" if _UNI else "[X]"
DOT    = "●" if _UNI else "*"
BLOCK  = "█" if _UNI else "#"
SHADE  = "░" if _UNI else "."


def section(title):
    print()
    print(f"{C.BOLD}{C.CYAN}{ARROW} {title}{C.RESET}")
    print(f"{C.DIM}{H * 58}{C.RESET}")

def warn(msg):
    print(f"  {C.YELLOW}WARNING:{C.RESET} {msg}")

def error(msg):
    print(f"  {C.RED}ERROR:{C.RESET} {msg}")

def success(msg):
    print(f"  {C.GREEN}{OK}{C.RESET} {msg}")


class _NoopState:
    def __setitem__(self, k, v):
        pass


@contextlib.contextmanager
def bouncing_bar(initial_label, width=26, chunk=7):
    """Bouncing marquee shown while a blocking operation runs.

    The caller can update the label from the main thread via
    ``state['label'] = '...'``; the animation thread picks it up on the
    next frame. In non-interactive shells it degrades to a single printed line.
    """
    if not _ANIMATE:
        print(f"  {initial_label} ...", flush=True)
        yield _NoopState()
        return

    state = {"label": initial_label}
    stop = threading.Event()
    period = max(1, 2 * (width - chunk))

    def animate():
        sys.stdout.write("\033[?25l")  # hide cursor
        sys.stdout.flush()
        frame = 0
        try:
            while not stop.is_set():
                t = frame % period
                pos = t if t <= width - chunk else period - t
                cells = [SHADE] * width
                for i in range(chunk):
                    idx = pos + i
                    if 0 <= idx < width:
                        cells[idx] = BLOCK
                bar = "".join(cells)
                label = state["label"]
                sys.stdout.write(f"\r  {label} {C.CYAN}[{bar}]{C.RESET}\033[K")
                sys.stdout.flush()
                frame += 1
                time.sleep(0.045)
        finally:
            sys.stdout.write("\r\033[K")
            sys.stdout.write("\033[?25h")  # restore cursor
            sys.stdout.flush()

    t = threading.Thread(target=animate, daemon=True)
    t.start()
    try:
        yield state
    finally:
        stop.set()
        t.join(timeout=1.0)


# ---------------------------------------------------------------------------
# Project paths
# ---------------------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION_H = os.path.join(REPO_ROOT, "src", "ui", "version.h")
CHANGELOG = os.path.join(REPO_ROOT, "CHANGELOG.txt")
DIST_DIR = os.path.join(REPO_ROOT, "dist")

ALWAYS_SKIP_DIRS = {".git", "__pycache__", "dist", "build", "Build", ".github", "logs"}
ALWAYS_SKIP_SUFFIXES = (".o", ".obj", ".pdb", ".log")
ALWAYS_SKIP_FILES = {
    "homrec.hrc", "homrec_settings.json", "homrec_overlays.hrc",
}

# ---------------------------------------------------------------------------
# Prompts
# ---------------------------------------------------------------------------

def ask(prompt, default=None):
    suffix = f" [{default}]" if default not in (None, "") else ""
    prompt_str = f"{C.CYAN}{prompt}{C.RESET}{C.DIM}{suffix}{C.RESET}: "
    while True:
        val = input(prompt_str).strip()
        if val:
            return val
        if default is not None:
            return default
        print(f"  {C.YELLOW}(a non-empty answer is required){C.RESET}")

def ask_yes_no(prompt, default_yes=True):
    hint = "Y/n" if default_yes else "y/N"
    val = input(f"{C.CYAN}{prompt}{C.RESET} {C.DIM}[{hint}]{C.RESET}: ").strip().lower()
    if not val:
        return default_yes
    return val in ("y", "yes")

def ask_int(prompt, default, lo, hi):
    while True:
        raw = input(f"{C.CYAN}{prompt}{C.RESET} {C.DIM}[{default}]{C.RESET}: ").strip()
        if not raw:
            raw = str(default)
        try:
            n = int(raw)
        except ValueError:
            print(f"  {C.YELLOW}enter a number between {lo} and {hi}{C.RESET}")
            continue
        if lo <= n <= hi:
            return n
        print(f"  {C.YELLOW}enter a number between {lo} and {hi}{C.RESET}")

def ask_choice(prompt, options):
    """options: list of (key, label). Returns the chosen key."""
    for i, (_key, label) in enumerate(options, 1):
        print(f"  {C.BOLD}{i}){C.RESET} {label}")
    while True:
        raw = input(f"{C.CYAN}{prompt}{C.RESET} {C.DIM}[1-{len(options)}]{C.RESET}: ").strip()
        if not raw:
            raw = "1"
        if raw.isdigit() and 1 <= int(raw) <= len(options):
            return options[int(raw) - 1][0]
        print(f"  {C.YELLOW}enter a number between 1 and {len(options)}{C.RESET}")

def sanitize_label(label):
    label = re.sub(r"[^A-Za-z0-9._-]+", "-", label.strip())
    return label.strip("-") or "build"

# ---------------------------------------------------------------------------
# Step 2: version
# ---------------------------------------------------------------------------

def read_current_version():
    if not os.path.isfile(VERSION_H):
        return None
    with open(VERSION_H, "r", encoding="utf-8") as f:
        content = f.read()
    m = re.search(r'#define\s+HR_APP_VERSION\s+"([^"]+)"', content)
    return m.group(1) if m else None

def write_version(new_version):
    with open(VERSION_H, "r", encoding="utf-8") as f:
        content = f.read()
    content, n1 = re.subn(
        r'(#define\s+HR_APP_VERSION\s+)"[^"]*"',
        r'\1"' + new_version + '"',
        content,
    )
    content, n2 = re.subn(
        r'(#define\s+HR_APP_VERSION_W\s+)L"[^"]*"',
        r'\1L"' + new_version + '"',
        content,
    )
    if n1 == 0 or n2 == 0:
        warn("could not find HR_APP_VERSION/HR_APP_VERSION_W in version.h - "
             "file left untouched, update the version by hand.")
        return False
    with open(VERSION_H, "w", encoding="utf-8") as f:
        f.write(content)
    return True

def step_version():
    section("Version")
    current = read_current_version()
    if current:
        print(f"  Current version: {C.BOLD}{current}{C.RESET}  {C.DIM}(src/ui/version.h){C.RESET}")
    else:
        warn("could not read the current version from src/ui/version.h")

    version = ask("Release version (e.g. 2.1.0)", default=current)
    version = version.lstrip("vV")

    if current and version != current:
        if ask_yes_no(f"Update src/ui/version.h from {current} to {version}?"):
            if write_version(version):
                success(f"version.h updated -> {version}")
    elif not current:
        if ask_yes_no(f"Write {version} to src/ui/version.h?"):
            write_version(version)
    return version

# ---------------------------------------------------------------------------
# Step 3: build + ffmpeg
# ---------------------------------------------------------------------------

# MSYS2 MINGW64 discovery
MSYS2_ROOT_CANDIDATES = (
    r"C:\msys64",
    r"C:\tools\msys64",
)

def find_msys2_bash():
    env_root = os.environ.get("MSYS2_ROOT")
    candidates = ([env_root] if env_root else []) + list(MSYS2_ROOT_CANDIDATES)
    for root in candidates:
        bash = os.path.join(root, "usr", "bin", "bash.exe")
        if os.path.isfile(bash):
            return bash
    return None

def to_msys_path(win_path):
    """C:\\foo\\bar -> /c/foo/bar, the way MSYS2's bash expects a path."""
    drive, rest = os.path.splitdrive(win_path)
    rest = rest.replace("\\", "/")
    if drive:
        return f"/{drive[0].lower()}{rest}"
    return rest

def run_in_mingw64(shell_cmd, cwd):
    bash = find_msys2_bash()
    if not bash:
        error("MSYS2 not found (looked in MSYS2_ROOT and "
              f"{', '.join(MSYS2_ROOT_CANDIDATES)}). Install MSYS2 "
              "(https://www.msys2.org/) and its mingw-w64-x86_64-toolchain "
              "package, or set MSYS2_ROOT to point at an existing install.")
        return False

    env = os.environ.copy()
    env["MSYSTEM"] = "MINGW64"
    # CHERE_INVOKING=1 is the documented MSYS2 flag that makes a login shell
    # keep the directory it was launched from instead of cd'ing to $HOME -
    # same mechanism the "MSYS2 MINGW64" shortcut's own -l flag relies on.
    env["CHERE_INVOKING"] = "1"

    msys_cwd = to_msys_path(cwd)
    full_cmd = f"cd '{msys_cwd}' && {shell_cmd}"
    print(f"  {C.DIM}$ [MSYS2 MINGW64]{C.RESET} {shell_cmd}")
    try:
        result = subprocess.run([bash, "-lc", full_cmd], cwd=cwd, env=env)
        return result.returncode == 0
    except FileNotFoundError:
        error(f"'{bash}' could not be run.")
        return False

def step_build():
    section("Build")
    hr_exe = os.path.join(REPO_ROOT, "hr.exe")
    hom_exe = os.path.join(REPO_ROOT, "hom.exe")

    if ask_yes_no("Rebuild hr.exe and hom.exe now "
                  "(make clean && make && make hom, in MSYS2 MINGW64)?"):
        # Same LUA_CFLAGS/LUA_LDFLAGS the Makefile itself defaults to
        # (see its `?=` lines) - overridable via env vars for anyone whose
        # lua54 isn't installed at C:\lua54.
        lua_cflags = os.environ.get("LUA_CFLAGS", "-IC:/lua54/include")
        lua_ldflags = os.environ.get("LUA_LDFLAGS", "-LC:/lua54/lib")
        make_cmd = (
            f"make clean && "
            f"make LUA_CFLAGS=\"{lua_cflags}\" LUA_LDFLAGS=\"{lua_ldflags}\" && "
            f"make hom"
        )
        ok = run_in_mingw64(make_cmd, REPO_ROOT)
        if not ok:
            error("build finished with an error")
            if not ask_yes_no("Continue with the old hr.exe/hom.exe (if any)?",
                              default_yes=False):
                sys.exit(1)

    if not os.path.isfile(hr_exe):
        warn("hr.exe not found in the repo root - the 'full' and "
             "'portable' presets will be built without it if you continue.")
    if not os.path.isfile(hom_exe):
        warn("hom.exe not found - it will be skipped in the archives if you continue.")

    return (hr_exe if os.path.isfile(hr_exe) else None,
            hom_exe if os.path.isfile(hom_exe) else None)

def find_ffmpeg():
    section("ffmpeg")
    # No "use this ffmpeg.exe for the full build?" prompt here: the
    # 'full' vs 'portable' choice in step_archives() *is* the
    # "add ffmpeg or not" decision.
    candidates = [
        os.path.join(REPO_ROOT, "ffmpeg", "ffmpeg.exe"),
        os.path.join(REPO_ROOT, "ffmpeg.exe"),  # legacy location
    ]
    found = next((c for c in candidates if os.path.isfile(c)), None)
    if not found:
        which = shutil.which("ffmpeg")
        if which:
            found = which

    if found:
        success(f"using {found} (bundled in 'full' archives)")
        return found

    manual = ask("Path to the ffmpeg.exe to bundle in 'full' archives "
                 "(Enter to skip)", default="")
    if manual and os.path.isfile(manual):
        return manual
    if manual:
        warn("file not found at that path - skipping ffmpeg")
    else:
        print(f"  {C.DIM}no ffmpeg - 'full' archives will be built without it "
              f"(like 'portable').{C.RESET}")
    return None

# ---------------------------------------------------------------------------
# Step 4: archives
# ---------------------------------------------------------------------------

BASE_DOCS = ["README.md", "LICENSE", "FREE.txt", "CHANGELOG.txt"]
BASE_DIRS = ["cfg"]

FORMAT_OPTIONS = [
    ("zip", "zip"),
    ("tar.gz", "tar.gz"),
    ("7z", "7z (needs 7-Zip's 7z/7za on PATH)"),
]

def discover_root_dlls():
    return sorted(
        f for f in os.listdir(REPO_ROOT)
        if f.lower().endswith(".dll") and os.path.isfile(os.path.join(REPO_ROOT, f))
    )

def iter_tree(root_dir, arc_prefix=""):
    """Yield (abs_path, arcname) for every file under root_dir, skipping
    build artifacts and VCS metadata."""
    for dirpath, dirnames, filenames in os.walk(root_dir):
        dirnames[:] = [d for d in dirnames if d not in ALWAYS_SKIP_DIRS]
        for fname in filenames:
            if fname.endswith(ALWAYS_SKIP_SUFFIXES) or fname in ALWAYS_SKIP_FILES:
                continue
            abs_path = os.path.join(dirpath, fname)
            rel = os.path.relpath(abs_path, root_dir)
            yield abs_path, os.path.join(arc_prefix, rel) if arc_prefix else rel

def collect_files(preset, hr_exe, hom_exe, ffmpeg_path):
    """Returns list of (abs_path, arcname)."""
    files = []

    if preset in ("full", "portable"):
        if hr_exe:
            files.append((hr_exe, "hr.exe"))
            # hr.exe needs its runtime DLLs next to it to launch at all -
            # see discover_root_dlls(). hom.exe doesn't need these.
            dlls = discover_root_dlls()
            if not dlls:
                warn("hr.exe is included, but no .dll was found next to it "
                     "in the repo root - if this is a dynamic build (the "
                     "default for this Makefile), the archive won't run on "
                     "another machine without them.")
            for dll in dlls:
                files.append((os.path.join(REPO_ROOT, dll), dll))
        if hom_exe:
            files.append((hom_exe, "hom.exe"))
        if preset == "full" and ffmpeg_path:
            files.append((ffmpeg_path, os.path.join("ffmpeg", "ffmpeg.exe")))
        for doc in BASE_DOCS:
            p = os.path.join(REPO_ROOT, doc)
            if os.path.isfile(p):
                files.append((p, doc))
        for d in BASE_DIRS:
            p = os.path.join(REPO_ROOT, d)
            if os.path.isdir(p):
                files.extend(iter_tree(p, arc_prefix=d))

    elif preset == "source":
        for entry in sorted(os.listdir(REPO_ROOT)):
            if entry in ALWAYS_SKIP_DIRS or entry == "dist" or entry in ALWAYS_SKIP_FILES:
                continue
            abs_path = os.path.join(REPO_ROOT, entry)
            if os.path.isdir(abs_path):
                files.extend(iter_tree(abs_path, arc_prefix=entry))
            elif not abs_path.endswith((".exe", ".dll") + ALWAYS_SKIP_SUFFIXES):
                files.append((abs_path, entry))

    return files

def make_archive(files, out_path, fmt):
    n = len(files)
    label = f"Packing {os.path.basename(out_path)}"

    if fmt == "zip":
        with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
            with bouncing_bar(label) as state:
                for i, (abs_path, arcname) in enumerate(files, 1):
                    zf.write(abs_path, arcname)
                    state["label"] = f"{label}  {i}/{n}"
    elif fmt == "tar.gz":
        with tarfile.open(out_path, "w:gz") as tf:
            with bouncing_bar(label) as state:
                for i, (abs_path, arcname) in enumerate(files, 1):
                    tf.add(abs_path, arcname)
                    state["label"] = f"{label}  {i}/{n}"
    elif fmt == "7z":
        sevenzip = shutil.which("7z") or shutil.which("7za")
        if not sevenzip:
            raise RuntimeError(
                "7z/7za not found in PATH - install 7-Zip (or the 7za "
                "command-line build) and make sure it's on PATH, then try again."
            )
        # 7z can't take arbitrary (src, arcname) pairs directly, so stage
        # into a temp folder mirroring the arcnames, then compress that.
        stage = out_path + ".stage"
        if os.path.isdir(stage):
            shutil.rmtree(stage)
        os.makedirs(stage)
        try:
            for abs_path, arcname in files:
                dest = os.path.join(stage, arcname)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                shutil.copy2(abs_path, dest)
            # Suppress 7z's own progress so it doesn't fight with our bar.
            with bouncing_bar(f"{label} (7z)"):
                result = subprocess.run(
                    [sevenzip, "a", "-mx=9", out_path, "."],
                    cwd=stage,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                    text=True,
                )
            if result.returncode != 0:
                raise RuntimeError(f"7z failed: {result.stderr.strip() or result.returncode}")
        finally:
            shutil.rmtree(stage, ignore_errors=True)
    else:
        raise ValueError(f"unknown format {fmt}")

def _size_str(num_bytes):
    mb = num_bytes / (1024 * 1024)
    if mb >= 1024:
        return f"{mb / 1024:.2f} GB"
    return f"{mb:.1f} MB"

def step_archives(version, hr_exe, hom_exe, ffmpeg_path):
    section("Release archives")
    os.makedirs(DIST_DIR, exist_ok=True)

    count = ask_int("How many archives to prepare", default=1, lo=1, hi=6)

    preset_options = [
        ("full", "Full (hr.exe + hom.exe + ffmpeg.exe + docs + cfg)"),
        ("portable", "Portable (same, but without ffmpeg.exe)"),
        ("source", "Source-only (source code, no binaries)"),
    ]

    produced = []
    for i in range(1, count + 1):
        print(f"\n{C.BOLD}--- Archive {i}/{count} ---{C.RESET}")
        label = sanitize_label(ask(f"Label for archive #{i} (e.g. win64-full)",
                                   default=f"build{i}"))
        preset = ask_choice("What should go in the archive?", preset_options)
        fmt = ask_choice("Archive format?", FORMAT_OPTIONS)

        files = collect_files(preset, hr_exe, hom_exe, ffmpeg_path)
        if not files:
            warn("nothing to archive (no files for this preset) - skipping")
            continue

        out_name = f"homrec-{version}-{label}.{fmt}"
        out_path = os.path.join(DIST_DIR, out_name)
        try:
            make_archive(files, out_path, fmt)
        except RuntimeError as e:
            error(str(e))
            print(f"  {C.DIM}skipping {out_name}{C.RESET}")
            continue
        success(f"{out_name}  {C.DIM}({len(files)} files, "
                f"{_size_str(os.path.getsize(out_path))}){C.RESET}")
        produced.append(out_path)

    return produced

# ---------------------------------------------------------------------------
# Step 5: other goodies
# ---------------------------------------------------------------------------

def write_checksums(produced):
    if not produced:
        return None
    section("Checksums")
    sums_path = os.path.join(DIST_DIR, "SHA256SUMS.txt")
    lines = []
    total = len(produced)
    with bouncing_bar(f"Hashing {total} archive(s)") as state:
        for i, path in enumerate(produced, 1):
            name = os.path.basename(path)
            state["label"] = f"Hashing {name}  {i}/{total}"
            h = hashlib.sha256()
            with open(path, "rb") as f:
                for chunk in iter(lambda: f.read(1024 * 1024), b""):
                    h.update(chunk)
            lines.append(f"{h.hexdigest()}  {name}")
    with open(sums_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    for line in lines:
        print(f"  {C.DIM}{line}{C.RESET}")
    success(f"written to {sums_path}")
    return sums_path

def extract_release_notes(version):
    if not os.path.isfile(CHANGELOG):
        return None
    with open(CHANGELOG, "r", encoding="utf-8") as f:
        lines = f.readlines()

    start = None
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith(f"[Version {version}]") or s.startswith(f"[Version {version} "):
            start = i
            break
    if start is None:
        return None

    end = len(lines)
    for i in range(start + 1, len(lines)):
        if lines[i].strip().startswith("[Version") or lines[i].strip().startswith("[Unreleased"):
            end = i
            break

    section_text = "".join(lines[start:end]).rstrip() + "\n"
    notes_path = os.path.join(DIST_DIR, f"RELEASE_NOTES-{version}.txt")
    with open(notes_path, "w", encoding="utf-8") as f:
        f.write(section_text)
    return notes_path

def suggest_git_commands(version):
    section("Git (nothing is run automatically)")
    tag = f"v{version}"
    print(f"  Once everything checks out, tag and push - by hand:")
    print(f"    {C.BOLD}git tag -a {tag} -m \"homrec {version}\"{C.RESET}")
    print(f"    {C.BOLD}git push origin {tag}{C.RESET}")
    print()
    print("  Then create the GitHub Release for that tag and attach the archives")
    print(f"  from {C.BOLD}{DIST_DIR}{C.RESET} as release assets.")
    print()
    print("  The Windows installer (installer\\HomRec.iss) is no longer built by this")
    print("  script - build it yourself in a separate Inno Setup window (ISCC.exe or")
    print("  the Inno Setup Compiler GUI) and attach the resulting Setup .exe to the")
    print("  release by hand.")
    print()
    print(f"  {C.YELLOW}IMPORTANT for auto-update:{C.RESET} it has to be the installer .exe")
    print("  itself (not just the zip/tar.gz) - existing installs' Help > Check for")
    print("  Updates (src/hr_update.cpp) looks at the latest release for an asset")
    print("  whose name ends in .exe and offers to silently install it; skip this")
    print("  and auto-update simply won't find anything to offer.")

def banner():
    line = H * 58
    print(f"{C.BOLD}{C.MAGENTA}{line}{C.RESET}")
    print(f"{C.BOLD}{C.MAGENTA}  {DOT} HomRec release builder{C.RESET}")
    print(f"{C.BOLD}{C.MAGENTA}{line}{C.RESET}")

def main():
    banner()
    print("Welcome! Let's put together the release archives for GitHub/mirrors.")
    print(f"{C.DIM}Directory: {REPO_ROOT}{C.RESET}")

    start_time = time.time()

    version = step_version()
    hr_exe, hom_exe = step_build()
    ffmpeg_path = find_ffmpeg()
    produced = step_archives(version, hr_exe, hom_exe, ffmpeg_path)

    section("Other bits")
    write_checksums(produced)
    notes_path = extract_release_notes(version)
    if notes_path:
        success(f"release notes (from CHANGELOG.txt) saved to {notes_path}")
    else:
        warn("could not pull a section out of CHANGELOG.txt - no release notes created")
    suggest_git_commands(version)

    elapsed = time.time() - start_time
    section("Done")
    if produced:
        print(f"  {C.GREEN}{OK}{C.RESET} Built {C.BOLD}{len(produced)}{C.RESET} "
              f"archive(s) in {elapsed:.1f}s")
        for p in produced:
            print(f"    {C.DIM}{DOT}{C.RESET} {os.path.basename(p)}  "
                  f"{C.DIM}({_size_str(os.path.getsize(p))}){C.RESET}")
        print()
        print(f"  {C.DIM}Everything is in {DIST_DIR} - ready to upload to "
              f"GitHub Releases/mirrors.{C.RESET}")
    else:
        warn("no archive was built")

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print(f"\n{C.YELLOW}Interrupted by user.{C.RESET}")
        sys.exit(1)