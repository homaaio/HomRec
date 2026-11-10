#!/usr/bin/env python3
"""Load test: records with homrec.exe for a few minutes and samples CPU / GPU / disk / memory meanwhile.

    python tools/perf_test.py                    # 5 minutes
    python tools/perf_test.py -m 10              # 10 minutes
    python tools/perf_test.py --label before
    python tools/perf_test.py --config other.hrc
    python tools/perf_test.py --interval 2 --warmup 20
    python tools/perf_test.py --no-build

Results go to logs/perf/<YYYYMMDD-HHMMSS>[-label]/ (samples.csv, homrec.log, run.json, summary.md) and are
compared with the previous run. Recordings are never deleted. Windows only.
"""
import argparse
import csv
import datetime
import glob
import hashlib
import json
import os
import platform
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from homrec_build import REPO_ROOT, C, OK, BAD, DOT, section, warn, error, success  # noqa: E402

PERF_DIR = os.path.join(REPO_ROOT, "logs", "perf")
SAMPLER = os.path.join(REPO_ROOT, "tools", "perf_sample.ps1")
FAIL_MARKERS = ("recording did not start", "start cancelled", "no output file")
VIDEO_EXT = (".mp4", ".mkv", ".avi", ".mov", ".webm", ".flv", ".ts")
# (key, label, unit); lower is better for every one of them.
METRICS = [("cpu_total", "CPU total", "%"), ("mem_used_mb", "Memory used", "MB"),
           ("gpu_total", "GPU (approx.)", "%"), ("disk_read_mb", "Disk read", "MB/s"),
           ("disk_write_mb", "Disk write", "MB/s"), ("homrec_cpu", "homrec CPU", "%"),
           ("ffmpeg_cpu", "ffmpeg CPU", "%"), ("proc_mem_mb", "Process memory", "MB")]


# --------------------------------------------------------------------------- helpers
def sha256(path):
    if not path or not os.path.isfile(path):
        return ""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_output_folder(config, exe_dir):
    """output_folder= from [capture] of the .hrc; relative paths are relative to the folder of homrec.exe."""
    folder = "recordings"
    if config and os.path.isfile(config):
        section_name = ""
        with open(config, encoding="utf-8-sig", errors="replace") as f:
            for line in f:
                line = line.strip()
                if line.startswith("[") and line.endswith("]"):
                    section_name = line[1:-1].strip().lower()
                elif section_name == "capture" and line.lower().startswith("output_folder="):
                    folder = line.split("=", 1)[1].strip() or folder
    return folder if os.path.isabs(folder) else os.path.join(exe_dir, folder)


def git_info():
    def g(*a):
        r = subprocess.run(["git", *a], cwd=REPO_ROOT, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        return r.stdout.strip() if r.returncode == 0 else ""
    return {"commit": g("rev-parse", "--short", "HEAD"), "dirty": bool(g("status", "--porcelain"))}


def machine_info():
    def ps(cmd):
        try:
            r = subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True,
                               text=True, encoding="utf-8", errors="replace", timeout=30)
            return [x.strip() for x in r.stdout.splitlines() if x.strip()]
        except Exception:
            return []
    cpu = ps("(Get-CimInstance Win32_Processor).Name")
    gpu = ps("(Get-CimInstance Win32_VideoController).Name")
    return {"cpu": cpu[0] if cpu else platform.processor(), "gpu": gpu, "os": platform.platform()}


def other_recording_running():
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True,
                             encoding="utf-8", errors="replace").stdout.lower()
    except Exception:
        return []
    return [n for n in ("homrec.exe", "ffmpeg.exe") if f'"{n}"' in out]


def percentile(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    k = (len(s) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def load_samples(path):
    rows = []
    with open(path, newline="", encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            try:
                rows.append({k: float(v) for k, v in r.items() if v not in (None, "")})
            except ValueError:
                continue
    for r in rows:   # approximate GPU total: the engine types added together, capped at 100
        r["gpu_total"] = min(100.0, r.get("gpu_3d", 0) + r.get("gpu_decode", 0)
                             + r.get("gpu_encode", 0) + r.get("gpu_copy", 0))
    return rows


def compute_stats(rows, rec_offset, warmup):
    """avg / p95 / max per metric over the samples taken after `warmup` s of recording."""
    used = [r for r in rows if r["elapsed_s"] - rec_offset >= warmup]
    stats = {}
    for key, _label, _unit in METRICS:
        vals = [r[key] for r in used if key in r]
        stats[key] = {"avg": round(sum(vals) / len(vals), 2) if vals else 0.0,
                      "p95": round(percentile(vals, 95), 2), "max": round(max(vals), 2) if vals else 0.0}
    return stats, len(used)


def verdict(before, after):
    if abs(after - before) < 0.5 or (before and abs(after - before) / abs(before) < 0.05):
        return "~"
    return "better" if after < before else "worse"


def compare(prev, cur):
    """Rows (label, before, after, delta %, verdict) for the averages, lower is better."""
    rows = []
    for key, label, unit in METRICS:
        b = prev["stats"].get(key, {}).get("avg")
        a = cur["stats"].get(key, {}).get("avg")
        if b is None or a is None:
            continue
        delta = (a - b) / b * 100.0 if b else (0.0 if a == 0 else float("inf"))
        rows.append((f"{label} ({unit})", b, a, delta, verdict(b, a)))
    return rows


def write_summary(path, run, cmp_rows, cmp_note):
    st = run["stats"]
    lines = [f"# Load test {run['name']}", "",
             f"- config: `{run['config'] or 'default'}`", f"- exe sha256: `{run['exe_sha256'][:16]}`",
             f"- git: {run['git']['commit']}{' (dirty)' if run['git']['dirty'] else ''}",
             f"- machine: {run['machine']['cpu']} / {', '.join(run['machine']['gpu']) or 'no GPU info'} / {run['machine']['os']}",
             f"- recorded: {run['recording']['path'] or 'file not found'} "
             f"({run['recording']['size_mb']} MB, {run['recorded_seconds']:.0f} s)",
             f"- samples used: {run['samples_used']} of {run['samples_total']} "
             f"(first {run['warmup_s']} s of the recording ignored)", "",
             "| metric | avg | p95 | max |", "|---|---:|---:|---:|"]
    for key, label, unit in METRICS:
        s = st[key]
        lines.append(f"| {label} ({unit}) | {s['avg']} | {s['p95']} | {s['max']} |")
    lines += ["", "GPU is approximate: the engine types (3D, decode, encode, copy) are added together and capped at 100.",
              "homrec / ffmpeg CPU is the share of the whole CPU, not of one core."]
    if cmp_rows:
        lines += ["", f"## Compared with {cmp_note}", "", "| metric (avg) | before | after | delta % | |", "|---|---:|---:|---:|---|"]
        for label, b, a, d, v in cmp_rows:
            lines.append(f"| {label} | {b} | {a} | {d:+.1f} | {v} |")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def previous_run(current_dir):
    found = [p for p in glob.glob(os.path.join(PERF_DIR, "*", "run.json"))
             if os.path.dirname(p) != current_dir]
    if not found:
        return None
    path = max(found, key=os.path.getmtime)
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f), path
    except (OSError, ValueError):
        return None


# --------------------------------------------------------------------------- recording
def tail_for_start(cli_log, offset, proc, timeout=120):
    """Waits for 'recording started' in logs/cli/cli.log (only what was written after `offset`)."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.isfile(cli_log):
            with open(cli_log, "rb") as f:
                f.seek(offset)
                text = f.read().decode("utf-8", errors="replace").lower()
            if "recording started" in text:
                return True, ""
            for m in FAIL_MARKERS:
                if m in text:
                    return False, m
        if proc.poll() is not None:
            return False, f"homrec exited with code {proc.returncode}"
        time.sleep(0.5)
    return False, "timeout waiting for 'recording started'"


def stop_recording(proc, stop_file):
    try:
        os.kill(proc.pid, signal.CTRL_BREAK_EVENT)      # homrec saves the recording on Ctrl+Break
    except (AttributeError, OSError) as e:
        warn(f"could not send Ctrl+Break: {e}")
    open(stop_file, "w").close()                         # stops the sampler
    try:
        proc.wait(timeout=240)
        return True
    except subprocess.TimeoutExpired:
        warn("homrec did not exit within 240 s - terminating it (the recording may be incomplete)")
        proc.terminate()
        return False


def main():
    ap = argparse.ArgumentParser(description="HomRec load test")
    ap.add_argument("-m", "--minutes", type=float, default=5.0)
    ap.add_argument("--label", default="")
    ap.add_argument("--config", default="")
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--warmup", type=float, default=20.0)
    ap.add_argument("--no-build", action="store_true")
    args = ap.parse_args()

    if os.name != "nt":
        error("perf_test.py needs Windows (PowerShell counters, Ctrl+Break).")
        return 1

    exe = os.path.join(REPO_ROOT, "homrec.exe")
    if not os.path.isfile(exe):
        if args.no_build:
            error("homrec.exe not found (and --no-build was given)")
            return 1
        if subprocess.call([sys.executable, os.path.join(REPO_ROOT, "tools", "build.py"), "homrec"]) != 0 \
                or not os.path.isfile(exe):
            error("could not build homrec.exe")
            return 1
    exe_dir = os.path.dirname(exe)

    running = other_recording_running()
    if running:
        error(f"{' / '.join(running)} is already running - stop the other recording first.")
        return 1

    config = args.config
    if not config and os.path.isfile(os.path.join(REPO_ROOT, "homrec.hrc")):
        config = os.path.join(REPO_ROOT, "homrec.hrc")
    out_dir = read_output_folder(config, exe_dir)
    os.makedirs(out_dir, exist_ok=True)
    before_files = set(os.listdir(out_dir))

    name = datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + (f"-{args.label}" if args.label else "")
    run_dir = os.path.join(PERF_DIR, name)
    os.makedirs(run_dir, exist_ok=True)
    samples_csv = os.path.join(run_dir, "samples.csv")
    stop_file = os.path.join(run_dir, "stop")
    homrec_log = os.path.join(run_dir, "homrec.log")
    cli_log = os.path.join(exe_dir, "logs", "cli", "cli.log")
    cli_offset = os.path.getsize(cli_log) if os.path.isfile(cli_log) else 0

    section(f"Load test {name}")
    print(f"  output folder: {out_dir}")
    sampler = subprocess.Popen(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", SAMPLER,
         "-IntervalSec", str(args.interval), "-OutFile", samples_csv, "-StopFile", stop_file,
         "-MaxSeconds", str(int(args.minutes * 60 + args.warmup + 600))])
    t_wait = time.time()
    while not os.path.isfile(samples_csv) and time.time() - t_wait < 30:
        time.sleep(0.2)
    t_sampler = time.time()

    cmd = [exe, "start"] + ([config] if config else [])
    log = open(homrec_log, "wb")
    proc = subprocess.Popen(cmd, cwd=exe_dir, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    ok, why = tail_for_start(cli_log, cli_offset, proc)
    if not ok:
        error(f"recording did not start: {why}")
        open(stop_file, "w").close()
        if proc.poll() is None:
            proc.terminate()
        log.close()
        try:
            print(open(homrec_log, encoding="utf-8", errors="replace").read())
        except OSError:
            pass
        return 1
    t_rec = time.time()
    rec_offset = t_rec - t_sampler            # sample time minus this = time since the recording began
    success("recording started")

    interrupted = False
    try:
        end = t_rec + args.minutes * 60
        while time.time() < end and proc.poll() is None:
            left = int(end - time.time())
            print(f"\r  recording... {left // 60:02d}:{left % 60:02d} left ", end="", flush=True)
            time.sleep(1)
        print()
    except KeyboardInterrupt:
        interrupted = True
        print(f"\n  {C.YELLOW}Ctrl+C - stopping and saving the recording{C.RESET}")
    recorded = time.time() - t_rec

    print("  stopping, homrec is saving the file...")
    stop_recording(proc, stop_file)
    log.close()
    try:
        sampler.wait(timeout=60)
    except subprocess.TimeoutExpired:
        sampler.terminate()

    new_files = [f for f in os.listdir(out_dir) if f not in before_files and f.lower().endswith(VIDEO_EXT)]
    rec_path = max((os.path.join(out_dir, f) for f in new_files), key=os.path.getmtime) if new_files else ""
    if not rec_path:
        warn("no new recording file was found in the output folder")

    rows = load_samples(samples_csv) if os.path.isfile(samples_csv) else []
    stats, used = compute_stats(rows, rec_offset, args.warmup)
    run = {"name": name, "time": datetime.datetime.now().isoformat(timespec="seconds"),
           "config": config, "config_sha256": sha256(config), "exe_sha256": sha256(exe),
           "git": git_info(), "machine": machine_info(), "warmup_s": args.warmup,
           "recorded_seconds": round(recorded, 1), "interrupted": interrupted,
           "recording": {"path": rec_path, "size_mb": round(os.path.getsize(rec_path) / 1048576, 1) if rec_path else 0},
           "samples_total": len(rows), "samples_used": used, "stats": stats}
    with open(os.path.join(run_dir, "run.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(run, f, indent=2, ensure_ascii=False)

    cmp_rows, note = [], ""
    prev = previous_run(run_dir)
    if prev:
        prun, ppath = prev
        note = os.path.basename(os.path.dirname(ppath))
        if prun.get("config_sha256") != run["config_sha256"]:
            warn("the config differs from the previous run - the comparison may not be fair")
        if prun.get("exe_sha256") != run["exe_sha256"]:
            warn("homrec.exe differs from the previous run (expected when you compare a change)")
        cmp_rows = compare(prun, run)
    write_summary(os.path.join(run_dir, "summary.md"), run, cmp_rows, note)

    section("Result")
    for key, label, unit in METRICS:
        s = stats[key]
        print(f"  {label:<18} avg {s['avg']:>8}  p95 {s['p95']:>8}  max {s['max']:>8} {unit}")
    for label, b, a, d, v in cmp_rows:
        colour = C.GREEN if v == "better" else C.RED if v == "worse" else C.DIM
        print(f"  {colour}{label:<24} {b} -> {a} ({d:+.1f}%) {v}{C.RESET}")
    print(f"  results: {os.path.relpath(run_dir, REPO_ROOT)}")
    if interrupted:
        return 130
    if not rows or not rec_path:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
