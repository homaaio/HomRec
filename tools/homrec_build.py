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
        print("  (нужен непустой ответ)")


def ask_yes_no(prompt, default_yes=True):
    hint = "Y/n" if default_yes else "y/N"
    val = input(f"{prompt} [{hint}]: ").strip().lower()
    if not val:
        return default_yes
    return val in ("y", "yes", "д", "да")


def ask_int(prompt, default, lo, hi):
    while True:
        raw = input(f"{prompt} [{default}]: ").strip()
        if not raw:
            raw = str(default)
        try:
            n = int(raw)
        except ValueError:
            print(f"  введите число от {lo} до {hi}")
            continue
        if lo <= n <= hi:
            return n
        print(f"  введите число от {lo} до {hi}")


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
        print(f"  введите число от 1 до {len(options)}")


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
        print("  WARNING: не нашёл HR_APP_VERSION/HR_APP_VERSION_W в version.h - "
              "файл не тронут, поправьте версию вручную.")
        return False
    with open(VERSION_H, "w", encoding="utf-8") as f:
        f.write(content)
    return True


def step_version():
    section("Версия")
    current = read_current_version()
    if current:
        print(f"Текущая версия в src/ui/version.h: {current}")
    else:
        print("Не удалось прочитать текущую версию из src/ui/version.h.")

    version = ask("Версия релиза (например, 2.1.0)", default=current)
    version = version.lstrip("vV")

    if current and version != current:
        if ask_yes_no(f"Обновить src/ui/version.h с {current} на {version}?"):
            if write_version(version):
                print(f"  version.h обновлён -> {version}")
    elif not current:
        if ask_yes_no(f"Записать {version} в src/ui/version.h?"):
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
        print(f"  '{args[0]}' не найден в PATH.")
        return False


def find_make():
    for candidate in ("mingw32-make", "make"):
        if shutil.which(candidate):
            return candidate
    return None


def step_build():
    section("Сборка")
    hr_exe = os.path.join(REPO_ROOT, "hr.exe")
    hom_exe = os.path.join(REPO_ROOT, "hom.exe")

    if ask_yes_no("Пересобрать hr.exe и hom.exe сейчас (make clean && make && make hom)?"):
        make = find_make()
        if not make:
            print("  make/mingw32-make не найден в PATH - сборка пропущена, "
                  "буду использовать то, что уже лежит в корне репозитория.")
        else:
            ok = run_cmd([make, "clean"], REPO_ROOT)
            ok = run_cmd([make], REPO_ROOT) and ok
            ok = run_cmd([make, "hom"], REPO_ROOT) and ok
            if not ok:
                print("  Сборка завершилась с ошибкой.")
                if not ask_yes_no("Продолжить со старыми hr.exe/hom.exe (если они есть)?", default_yes=False):
                    sys.exit(1)

    if not os.path.isfile(hr_exe):
        print("  hr.exe не найден в корне репозитория - варианты 'full' и "
              "'portable' будут собраны без него, если продолжите.")
    if not os.path.isfile(hom_exe):
        print("  hom.exe не найден - будет пропущен в архивах, если продолжите.")

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
        print(f"Найден: {found}")
        if ask_yes_no("Использовать этот ffmpeg.exe для 'full' сборки?"):
            return found

    manual = ask("Путь к ffmpeg.exe для бандла (Enter, чтобы пропустить)", default="")
    if manual and os.path.isfile(manual):
        return manual
    if manual:
        print("  Файл не найден по указанному пути - пропускаю ffmpeg.")
    else:
        print("  ffmpeg пропущен - 'full' архив будет собран как 'portable'.")
    return None


# ---------------------------------------------------------------------------
# Step 4: archives
# ---------------------------------------------------------------------------

BASE_DOCS = [
    "README.md", "LICENSE", "FREE.txt", "SUPPORT.md",
    "commands.md", "CHANGELOG.txt",
]
BASE_DIRS = ["cfg", "plugins"]


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
                print("  WARNING: hr.exe включён, но рядом в корне репозитория не найдено ни "
                      "одной .dll - если сборка динамическая (по умолчанию для этого Makefile), "
                      "архив без них не запустится на чужой машине.")
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
            raise RuntimeError("7z.exe не найден в PATH")
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
    section("Архивы релиза")
    os.makedirs(DIST_DIR, exist_ok=True)

    count = ask_int("Сколько архивов подготовить", default=1, lo=1, hi=6)

    preset_options = [
        ("full", "Full (hr.exe + hom.exe + ffmpeg.exe + docs + cfg + plugins)"),
        ("portable", "Portable (то же самое, но без ffmpeg.exe)"),
        ("source", "Source-only (исходники, без бинарников)"),
    ]
    format_options = [
        ("zip", "zip"),
        ("tar.gz", "tar.gz"),
    ]
    if shutil.which("7z") or shutil.which("7za"):
        format_options.append(("7z", "7z"))

    produced = []
    for i in range(1, count + 1):
        print(f"\n--- Архив {i}/{count} ---")
        label = sanitize_label(ask(f"Метка для архива #{i} (например win64-full)", default=f"build{i}"))
        preset = ask_choice("Что положить в архив?", preset_options)
        fmt = ask_choice("Формат архива?", format_options)

        files = collect_files(preset, hr_exe, hom_exe, ffmpeg_path)
        if not files:
            print("  Нечего архивировать (нет файлов для этого пресета) - пропускаю.")
            continue

        out_name = f"HomRec-{version}-{label}.{fmt}"
        out_path = os.path.join(DIST_DIR, out_name)
        print(f"  Собираю {out_name} ({len(files)} файлов)...")
        make_archive(files, out_path, fmt)
        size_mb = os.path.getsize(out_path) / (1024 * 1024)
        print(f"  Готово: {out_path} ({size_mb:.1f} MB)")
        produced.append(out_path)

    return produced


# ---------------------------------------------------------------------------
# Step 5: other goodies
# ---------------------------------------------------------------------------

def write_checksums(produced):
    if not produced:
        return None
    section("Контрольные суммы")
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
    print(f"Записано в {sums_path}")
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
    section("Git (ничего не выполняется автоматически)")
    tag = f"v{version}"
    print("Когда всё проверено, тег и пуш - вручную:")
    print(f'  git tag -a {tag} -m "HomRec {version}"')
    print(f"  git push origin {tag}")


def main():
    print("=" * 60)
    print("  HomRec release builder")
    print("=" * 60)
    print("Приветствую! Соберём релизные архивы для GitHub/зеркал.")
    print(f"Репозиторий: {REPO_ROOT}")

    start_time = time.time()

    version = step_version()
    hr_exe, hom_exe = step_build()
    ffmpeg_path = find_ffmpeg()
    produced = step_archives(version, hr_exe, hom_exe, ffmpeg_path)

    section("Другие плюшки")
    write_checksums(produced)
    notes_path = extract_release_notes(version)
    if notes_path:
        print(f"Release notes (из CHANGELOG.txt) сохранены в {notes_path}")
    else:
        print("Не удалось вытащить секцию из CHANGELOG.txt - release notes не созданы.")
    suggest_git_commands(version)

    elapsed = time.time() - start_time
    section("Готово")
    if produced:
        print(f"Собрано архивов: {len(produced)} за {elapsed:.1f} сек.")
        for p in produced:
            print(f"  - {p}")
        print(f"\nВсё лежит в {DIST_DIR} - можно заливать на GitHub Releases/зеркала.")
    else:
        print("Ни один архив не был собран.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nПрервано пользователем.")
        sys.exit(1)