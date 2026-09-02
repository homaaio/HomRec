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

def run_cmd(args, cwd):
    print(f"  $ {' '.join(args)}")
    try:
        result = subprocess.run(args, cwd=cwd)
        return result.returncode == 0
    except FileNotFoundError:
        print(f"  '{args[0]}' not found in PATH.")
        return False


def find_make():
    for candidate in ("mingw32-make", "make"):
        if shutil.which(candidate):
            return candidate
    return None


def step_build():
    section("Build")
    hr_exe = os.path.join(REPO_ROOT, "hr.exe")
    hom_exe = os.path.join(REPO_ROOT, "hom.exe")

    if ask_yes_no("Rebuild hr.exe and hom.exe now (make clean && make && make hom)?"):
        make = find_make()
        if not make:
            print("  make/mingw32-make not found in PATH - skipping the build, "
                  "will use whatever is already sitting in the repo root.")
        else:
            ok = run_cmd([make, "clean"], REPO_ROOT)
            ok = run_cmd([make], REPO_ROOT) and ok
            ok = run_cmd([make, "hom"], REPO_ROOT) and ok
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
    candidates = [
        os.path.join(REPO_ROOT, "ffmpeg.exe"),
    ]
    found = next((c for c in candidates if os.path.isfile(c)), None)
    if not found:
        which = shutil.which("ffmpeg")
        if which:
            found = which

    if found:
        print(f"Found: {found}")
        if ask_yes_no("Use this ffmpeg.exe for the 'full' build?"):
            return found

    manual = ask("Path to the ffmpeg.exe to bundle (Enter to skip)", default="")
    if manual and os.path.isfile(manual):
        return manual
    if manual:
        print("  File not found at that path - skipping ffmpeg.")
    else:
        print("  ffmpeg skipped - the 'full' archive will be built like 'portable'.")
    return None


# ---------------------------------------------------------------------------
# Step 3b: Windows installer (Inno Setup)
# ---------------------------------------------------------------------------

def find_iscc():
    candidates = [
        r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        r"C:\Program Files\Inno Setup 6\ISCC.exe",
    ]
    found = next((c for c in candidates if os.path.isfile(c)), None)
    if found:
        return found
    return shutil.which("iscc") or shutil.which("ISCC")


def step_installer(version, hr_exe):
    section("Windows installer (Inno Setup)")
    iss_path = os.path.join(REPO_ROOT, "installer", "HomRec.iss")
    if not os.path.isfile(iss_path):
        print("  installer\\HomRec.iss not found - skipping.")
        return None
    if not hr_exe:
        print("  hr.exe wasn't built - skipping the installer (it packages "
              "whatever's already sitting in the repo root).")
        return None
    if not ask_yes_no("Build the Windows installer too (installer\\HomRec.iss)?"):
        return None

    iscc = find_iscc()
    if not iscc:
        print("  ISCC.exe (Inno Setup Compiler) not found. Install Inno Setup "
              "(https://jrsoftware.org/isinfo.php) or put iscc.exe on PATH, then try again.")
        return None

    os.makedirs(DIST_DIR, exist_ok=True)
    ok = run_cmd([iscc, f"/DMyAppVersion={version}", iss_path], REPO_ROOT)
    if not ok:
        print("  Inno Setup compile failed - see its output above.")
        return None

    out_path = os.path.join(DIST_DIR, f"HomRec-Setup-{version}.exe")
    if not os.path.isfile(out_path):
        print("  ISCC reported success but " + out_path + " wasn't found - "
              "check OutputDir/OutputBaseFilename in installer/HomRec.iss.")
        return None
    size_mb = os.path.getsize(out_path) / (1024 * 1024)
    print(f"  Done: {out_path} ({size_mb:.1f} MB)")
    return out_path


# ---------------------------------------------------------------------------
# Step 4: archives
# ---------------------------------------------------------------------------

BASE_DOCS = [
    "README.md", "LICENSE", "FREE.txt", "SUPPORT.md",
    "commands.md", "CHANGELOG.txt", "CONTRIBUTORS.md",
]
BASE_DIRS = ["cfg", "plugins"]

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
            files.append((ffmpeg_path, "ffmpeg.exe"))
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
        ("full", "Full (hr.exe + hom.exe + ffmpeg.exe + docs + cfg + plugins)"),
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

        out_name = f"HomRec-{version}-{label}.{fmt}"
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


def suggest_git_commands(version, installer_path):
    section("Git (nothing is run automatically)")
    tag = f"v{version}"
    print("Once everything checks out, tag and push - by hand:")
    print(f'  git tag -a {tag} -m "HomRec {version}"')
    print(f"  git push origin {tag}")
    print("\nThen create the GitHub Release for that tag and attach the archives")
    print(f"from {DIST_DIR} as release assets.")
    if installer_path:
        print(f"\nIMPORTANT for auto-update: attach {os.path.basename(installer_path)} itself")
        print("(not just the zip/tar.gz) to the release. Existing installs' Help > Check")
        print("for Updates (src/hr_update.cpp) looks at the latest release for an asset")
        print("whose name ends in .exe and offers to silently install it - skip this and")
        print("auto-update simply won't find anything to offer.")


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
    installer_path = step_installer(version, hr_exe)
    produced = step_archives(version, hr_exe, hom_exe, ffmpeg_path)

    section("Other bits")
    write_checksums(produced + ([installer_path] if installer_path else []))
    notes_path = extract_release_notes(version)
    if notes_path:
        print(f"Release notes (from CHANGELOG.txt) saved to {notes_path}")
    else:
        print("Could not pull a section out of CHANGELOG.txt - no release notes were created.")
    suggest_git_commands(version, installer_path)

    elapsed = time.time() - start_time
    section("Done")
    all_produced = produced + ([installer_path] if installer_path else [])
    if all_produced:
        print(f"Archives built: {len(all_produced)} in {elapsed:.1f}s.")
        for p in all_produced:
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
