"""Compare benchmark logs.

  python tools/bench_compare.py base.txt run1.txt [run2.txt ...]     run vs run
  python tools/bench_compare.py run.txt                              old vs new inside ONE bench run
  python tools/bench_compare.py --fail-above 10 base.txt run.txt     exit 1 if any timing got >10% slower

Two log kinds are understood (picked from the base file):
  * framerate logs (Average/Minimum/Maximum/1% low/0.1% low framerate) - higher is better
  * "[bench] name ms" logs from scripts/linux/bench.sh                  - lower is better
    (scripts/linux/bench.sh also prints "[pair] label old new" lines: an old and a new implementation measured
    in the same run - overlay scaling / blend, preview thumbnail, crop convert, picture copies)
"""
import math
import re
import sys

METRICS = [
    ("Average", r"Average framerate\s*:\s*([\d.]+)"),
    ("Minimum", r"Minimum framerate\s*:\s*([\d.]+)"),
    ("Maximum", r"Maximum framerate\s*:\s*([\d.]+)"),
    ("1% low", r"1% low framerate\s*:\s*([\d.]+)"),
    ("0.1% low", r"0\.1% low framerate\s*:\s*([\d.]+)"),
]


def parse(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    out = {}
    for name, rx in METRICS:
        m = re.search(rx, text)
        if not m:
            raise ValueError(f"{path}: '{name}' line not found")
        out[name] = float(m.group(1))
    m = re.search(r"(\d+) frames rendered in ([\d.]+) s", text)
    out["frames"], out["secs"] = (int(m.group(1)), float(m.group(2))) if m else (None, None)
    return out


def parse_ms(path):
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\[bench\]\s+(\S+)\s+([\d.]+)", line)
        if m:
            out[m.group(1)] = float(m.group(2))
    if not out:
        raise ValueError(f"{path}: no '[bench]' lines found")
    return out


def parse_pairs(path):
    out = []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\[pair\]\s+(\S+)\s+(\S+)\s+(\S+)", line)
        if m:
            out.append(m.groups())
    return out


def main_pairs(path):
    ms = parse_ms(path)
    pairs = [(l, a, b) for l, a, b in parse_pairs(path) if a in ms and b in ms]
    if not pairs:
        print(f"{path}: no [pair] lines (bench.sh prints them)")
        return 1
    w = max(len(l) for l, _, _ in pairs)
    print(f"{path}   (ms per call, old -> new)")
    for label, a, b in pairs:
        old, new = ms[a], ms[b]
        speed = f"{old / new:6.1f}x faster" if new > 0.0005 else "    ~free"
        print(f"  {label:<{w}}  {old:>8.3f}  {new:>8.3f}  {(new - old) / old * 100.0:+7.1f}%  {speed}")
    return 0


def main_ms(argv, fail_above=None):
    base = parse_ms(argv[1])
    runs = [(p, parse_ms(p)) for p in argv[2:]]
    w = max(len(n) for n in base)
    worst = 0.0
    for p, r in runs:
        print(f"{argv[1]} -> {p}   (ms per call, negative % = faster)")
        logs = []
        for n, b in base.items():
            if n not in r:
                continue
            d = (r[n] - b) / b * 100.0 if b > 0 else 0.0
            if b >= 0.01:               # sub-10-microsecond timings are noise
                worst = max(worst, d)
                logs.append(math.log(max(r[n], 1e-9) / b))
            print(f"  {n:<{w}}  {b:>8.3f}  {r[n]:>8.3f}  {d:+7.1f}%")
        if logs:
            print(f"  {'geometric mean':<{w}}  {'':>8}  {'':>8}  {(math.exp(sum(logs) / len(logs)) - 1) * 100.0:+7.1f}%")
    if fail_above is not None and worst > fail_above:
        print(f"FAIL: slowest change is {worst:+.1f}% (limit {fail_above:+.1f}%)")
        return 1
    return 0


def main(argv):
    argv = list(argv)
    fail_above = None
    if "--fail-above" in argv:
        i = argv.index("--fail-above")
        fail_above = float(argv[i + 1])
        del argv[i:i + 2]
    if len(argv) < 2:
        print(__doc__)
        return 2
    is_ms = "[bench]" in open(argv[1], encoding="utf-8", errors="replace").read()
    if len(argv) == 2 and is_ms:
        return main_pairs(argv[1])
    if len(argv) < 3:
        print(__doc__)
        return 2
    if is_ms:
        return main_ms(argv, fail_above)
    base = parse(argv[1])
    runs = [(p, parse(p)) for p in argv[2:]]
    names = [n for n, _ in METRICS]
    w = max(len(p) for p, _ in runs + [(argv[1], base)])
    print(f"{'file':<{w}}  " + "  ".join(f"{n:>16}" for n in names))
    print(f"{argv[1]:<{w}}  " + "  ".join(f"{base[n]:>10.1f} (base)" for n in names))
    for p, r in runs:
        cells = []
        for n in names:
            d = (r[n] - base[n]) / base[n] * 100.0
            cells.append(f"{r[n]:>7.1f} {d:+6.1f}%")
        print(f"{p:<{w}}  " + "  ".join(f"{c:>16}" for c in cells))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
