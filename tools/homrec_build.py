#!/usr/bin/env python3
# btw im from windows
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import zipfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION_H = os.path.join(REPO_ROOT, "src", "ui", "version.h")
CHANGELOG = os.path.join(REPO_ROOT, "CHANGELOG.txt")
DIST_DIR = os.path.join(REPO_ROOT, "dist")

ALWAYS_SKIP_DIRS = {".git", "__pycache__", "dist", "build", "Build", ".github", "logs"}
ALWAYS_SKIP_SUFFIXES = (".o", ".obj", ".pdb", ".log")
ALWAYS_SKIP_FILES = {
    "homrec.hrc", "homrec_settings.json", "homrec_overlays.hrc",
}


def section(title):
    print(f"\n=== {title} ===")


def ask(prompt, default=None):
    suffix = f" [{default}]" if default not in (None, "") else ""
    while True:
        val = input(f"{prompt}{suffix}: ").strip()
        if val:
            return val
        if default is not None:
            return default
        print("  (a non-empty answer is required)")


def ask_yes_no(prompt, default_yes=True):
    hint = "Y/n" if default_yes else "y/N"
    val = input(f"{prompt} [{hint}]: ").strip().lower()
    if not val:
        return default_yes
    return val in ("y", "yes")


def ask_int(prompt, default, lo, hi):
    while True:
        raw = input(f"{prompt} [{default}]: ").strip()
        if not raw:
            raw = str(default)
        try:
            n = int(raw)
        except ValueError:
            print(f"  enter a number between {lo} and {hi}")
            continue
        if lo <= n <= hi:
            return n
        print(f"  enter a number between {lo} and {hi}")


def ask_choice(prompt, options):
    """options: list of (key, label). Returns the chosen key."""
    for i, (_key, label) in enumerate(options, 1):
        print(f"  {i}) {label}")
    while True:
        raw = input(f"{prompt} [1-{len(options)}]: ").strip()
        if not raw:
            raw = "1"
        if raw.isdigit() and 1 <= int(raw) <= len(options):
            return options[int(raw) - 1][0]
        print(f"  enter a number between 1 and {len(options)}")


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
        print("  WARNING: could not find HR_APP_VERSION/HR_APP_VERSION_W in "
              "version.h - file left untouched, update the version by hand.")
        return False
    with open(VERSION_H, "w", encoding="utf-8") as f:
        f.write(content)
    return True


def step_version():
    section("Version")
    current = read_current_version()
    if current:
        print(f"Current version in src/ui/version.h: {current}")
    else:
        print("Could not read the current version from src/ui/version.h.")

    version = ask("Release version (e.g. 2.1.0)", default=current)
    version = version.lstrip("vV")

    if current and version != current:
        if ask_yes_no(f"Update src/ui/version.h from {current} to {version}?"):
            if write_version(version):
                print(f"  version.h updated -> {version}")
    elif not current:
        if ask_yes_no(f"Write {version} to src/ui/version.h?"):
            write_version(version)
    return version


# ---------------------------------------------------------------------------
# Step 3: build + ffmpeg
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# MSYS2 MINGW64 discovery
#
# `make`/`mingw32-make` found on a plain Windows PATH is *not* the same
# thing as running inside an "MSYS2 MINGW64" shell: the whole point of that
# environment is the PATH/MSYSTEM setup its shell profile does, which is
# what puts the mingw-w64 g++/windres/pkg-config (and, via LUA_CFLAGS/
# LUA_LDFLAGS below, the matching lua54) in front of `make`. Running plain
# `make` from a normal `cmd.exe`/PowerShell prompt - which is what this
# script used to do - either fails outright (no make on PATH at all) or,
# worse, silently picks up some other make/gcc that isn't set up for this
# project's wx/Lua/ffmpeg headers and libs. So instead of invoking `make`
# directly, this shells out to MSYS2's own bash with MSYSTEM=MINGW64 set,
# which is exactly what launching "MSYS2 MINGW64" from the Start Menu does
# and is the one environment the Makefile (see its LUA_CFLAGS/LDFLAGS
# comments) is written against.
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
    """Runs `shell_cmd` (a single bash command line) inside an MSYS2 MINGW64
    login shell, cd'd into `cwd` - i.e. the same environment "MSYS2 MINGW64"
    from the Start Menu gives you, not whatever `make`/`gcc` a plain Windows
    PATH happens to resolve to."""
    bash = find_msys2_bash()
    if not bash:
        print("  MSYS2 not found (looked in MSYS2_ROOT and "
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
    print(f"  $ [MSYS2 MINGW64] {shell_cmd}")
    try:
        result = subprocess.run([bash, "-lc", full_cmd], cwd=cwd, env=env)
        return result.returncode == 0
    except FileNotFoundError:
        print(f"  '{bash}' could not be run.")
        return False


def step_build():
    section("Build")
    hr_exe = os.path.join(REPO_ROOT, "hr.exe")
    hom_exe = os.path.join(REPO_ROOT, "hom.exe")

    if ask_yes_no("Rebuild hr.exe and hom.exe now (make clean && make && make hom, "
                  "in MSYS2 MINGW64)?"):
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
            print("  Build finished with an error.")
            if not ask_yes_no("Continue with the old hr.exe/hom.exe (if any)?", default_yes=False):
                sys.exit(1)

    if not os.path.isfile(hr_exe):
        print("  hr.exe not found in the repo root - the 'full' and "
              "'portable' presets will be built without it if you continue.")
    if not os.path.isfile(hom_exe):
        print("  hom.exe not found - it will be skipped in the archives if you continue.")

    return hr_exe if os.path.isfile(hr_exe) else None, \
        hom_exe if os.path.isfile(hom_exe) else None


def find_ffmpeg():
    section("ffmpeg")
    # No more "use this ffmpeg.exe for the full build?" yes/no here - the
    # 'full' vs 'portable' choice made in step_archives() *is* the "add
    # ffmpeg or not" decision; asking again here was a second prompt for
    # the same choice. If an ffmpeg.exe is found (or given), 'full'
    # archives get it automatically; 'portable' archives never get it
    # regardless, by definition of the preset - see collect_files().
    candidates = [
        os.path.join(REPO_ROOT, "ffmpeg", "ffmpeg.exe"),
        os.path.join(REPO_ROOT, "ffmpeg.exe"),  # legacy location, still honored if present
    ]
    found = next((c for c in candidates if os.path.isfile(c)), None)
    if not found:
        which = shutil.which("ffmpeg")
        if which:
            found = which

    if found:
        print(f"Found: {found} (will be bundled in 'full' archives)")
        return found

    manual = ask("Path to the ffmpeg.exe to bundle in 'full' archives (Enter to skip)", default="")
    if manual and os.path.isfile(manual):
        return manual
    if manual:
        print("  File not found at that path - skipping ffmpeg.")
    else:
        print("  ffmpeg not found - 'full' archives will be built without it (like 'portable').")
    return None


# ---------------------------------------------------------------------------
# Step 4: archives
# ---------------------------------------------------------------------------

# README.md/LICENSE/CHANGELOG.txt are the only docs actual end users of a
# release archive need. commands.md (console command reference), SUPPORT.md
# and CONTRIBUTORS.md are contributor/dev-facing docs that don't belong in
# a binary release archive; they're still in the repo (and in the
# 'source' preset below, which packages the repo as-is) for anyone
# building from source, just not duplicated into every 'full'/'portable'
# zip. Likewise "plugins" here used to mean *this repo's own*
# src/plugins (bundled Lua examples, not a user-facing thing) getting
# copied into every release archive under a top-level plugins/ folder -
# removed for the same reason; a fresh install's own plugins/ directory
# (where .hrp packages get installed via File > Import Plugin) is created
# by the app itself, not shipped pre-populated.
BASE_DOCS = ["README.md", "LICENSE", "FREE.txt", "CHANGELOG.txt"]
BASE_DIRS = ["cfg"]

# Archive formats offered in the menu. 7z is always listed - not just when
# a 7z/7za binary happens to already be on PATH - since "add the .7z build
# variant" should mean an actual, discoverable menu entry, not one that
# silently vanishes on a fresh machine. make_archive()'s 7z branch still
# checks for the binary right before it's actually needed and fails with a
# clear, actionable message if it's missing, instead of the option just
# not being there in the first place.
FORMAT_OPTIONS = [
    ("zip", "zip"),
    ("tar.gz", "tar.gz"),
    ("7z", "7z (needs 7-Zip's 7z/7za on PATH)"),
]


def discover_root_dlls():
    """hr.exe is linked dynamically against wxWidgets + the image codec libs
    it pulls in (see LDFLAGS in the Makefile - only libgcc/libstdc++/pthread
    are -static, everything else, e.g. libpng/zlib/libwebp/wx*.dll, is a
    runtime DLL that MinGW drops next to the exe). hom.exe only links
    system DLLs (-lwinhttp -lshlwapi), so it needs none of these. Without
    bundling these, hr.exe simply won't launch on a machine that doesn't
    have the exact same MSYS2/MinGW install."""
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
                print("  WARNING: hr.exe is included, but no .dll was found next to it "
                      "in the repo root - if this is a dynamic build (the default for "
                      "this Makefile), the archive won't run on another machine without them.")
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
    if fmt == "zip":
        with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for abs_path, arcname in files:
                zf.write(abs_path, arcname)
    elif fmt == "tar.gz":
        with tarfile.open(out_path, "w:gz") as tf:
            for abs_path, arcname in files:
                tf.add(abs_path, arcname)
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
        for abs_path, arcname in files:
            dest = os.path.join(stage, arcname)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copy2(abs_path, dest)
        subprocess.run([sevenzip, "a", "-mx=9", out_path, "."], cwd=stage, check=True)
        shutil.rmtree(stage)
    else:
        raise ValueError(f"unknown format {fmt}")


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
        print(f"\n--- Archive {i}/{count} ---")
        label = sanitize_label(ask(f"Label for archive #{i} (e.g. win64-full)", default=f"build{i}"))
        preset = ask_choice("What should go in the archive?", preset_options)
        fmt = ask_choice("Archive format?", FORMAT_OPTIONS)

        files = collect_files(preset, hr_exe, hom_exe, ffmpeg_path)
        if not files:
            print("  Nothing to archive (no files for this preset) - skipping.")
            continue

        out_name = f"homrec-{version}-{label}.{fmt}"
        out_path = os.path.join(DIST_DIR, out_name)
        print(f"  Building {out_name} ({len(files)} files)...")
        try:
            make_archive(files, out_path, fmt)
        except RuntimeError as e:
            print(f"  {e}")
            print(f"  Skipping {out_name}.")
            continue
        size_mb = os.path.getsize(out_path) / (1024 * 1024)
        print(f"  Done: {out_path} ({size_mb:.1f} MB)")
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
    for path in produced:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                h.update(chunk)
        lines.append(f"{h.hexdigest()}  {os.path.basename(path)}")
        print(f"  {lines[-1]}")
    with open(sums_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Written to {sums_path}")
    return sums_path


def extract_release_notes(version):
    if not os.path.isfile(CHANGELOG):
        return None
    with open(CHANGELOG, "r", encoding="utf-8") as f:
        lines = f.readlines()

    start = None
    for i, line in enumerate(lines):
        if line.strip().startswith("[Version") or line.strip().startswith("[Unreleased"):
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
    print("Once everything checks out, tag and push - by hand:")
    print(f'  git tag -a {tag} -m "homrec {version}"')
    print(f"  git push origin {tag}")
    print("\nThen create the GitHub Release for that tag and attach the archives")
    print(f"from {DIST_DIR} as release assets.")
    print("\nThe Windows installer (installer\\HomRec.iss) is no longer built by this")
    print("script - build it yourself in a separate Inno Setup window (ISCC.exe or the")
    print("Inno Setup Compiler GUI) and attach the resulting Setup .exe to the release")
    print("by hand. IMPORTANT for auto-update: it has to be the installer .exe itself")
    print("(not just the zip/tar.gz) - existing installs' Help > Check for Updates")
    print("(src/hr_update.cpp) looks at the latest release for an asset whose name ends")
    print("in .exe and offers to silently install it; skip this and auto-update simply")
    print("won't find anything to offer.")


def main():
    print("=" * 60)
    print("  HomRec release builder")
    print("=" * 60)
    print("Welcome! Let's put together the release archives for GitHub/mirrors.")
    print(f"Repository: {REPO_ROOT}")

    start_time = time.time()

    version = step_version()
    hr_exe, hom_exe = step_build()
    ffmpeg_path = find_ffmpeg()
    produced = step_archives(version, hr_exe, hom_exe, ffmpeg_path)

    section("Other bits")
    write_checksums(produced)
    notes_path = extract_release_notes(version)
    if notes_path:
        print(f"Release notes (from CHANGELOG.txt) saved to {notes_path}")
    else:
        print("Could not pull a section out of CHANGELOG.txt - no release notes were created.")
    suggest_git_commands(version)

    elapsed = time.time() - start_time
    section("Done")
    if produced:
        print(f"Archives built: {len(produced)} in {elapsed:.1f}s.")
        for p in produced:
            print(f"  - {p}")
        print(f"\nEverything is in {DIST_DIR} - ready to upload to GitHub Releases/mirrors.")
    else:
        print("No archive was built.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nInterrupted by user.")
        sys.exit(1)
