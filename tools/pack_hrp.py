import os
import sys

# Ensure project root is importable when running from tools/
_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugin_engine.loader import write_hrp


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("Usage: python tools/pack_hrp.py <source_dir> <output_hrp_path>")
        return 2
    source_dir = argv[1]
    output_hrp = argv[2]
    write_hrp(output_hrp, source_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

