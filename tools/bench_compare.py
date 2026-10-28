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


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
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
