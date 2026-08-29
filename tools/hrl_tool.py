#!/usr/bin/env python3
"""
hrl_tool.py - pack/unpack HomRec .hrl language files.

Usage:
    python hrl_tool.py pack   <input.json> <output.hrl>
    python hrl_tool.py unpack <input.hrl>  <output.json>

Typical flow for adding a language:
    1. Copy lang_lg.json (or export one with `unpack` from an existing
       .hrl) and translate the values.
    2. python hrl_tool.py pack lang_ru.json ru.hrl
    3. Drop lg.hrl into  Assets\\L\\lg.hrl  next to hr.exe.
    4. Restart HomRec - it should now show up in Settings > Language.
"""
import gzip
import json
import sys

MAGIC_HRL = b"HRL\x01"


def pack(json_path: str, hrl_path: str) -> None:
    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)  # validates the JSON is well-formed first

    # ensure_ascii=False is what keeps Cyrillic (or any non-ASCII) text as
    # literal UTF-8 bytes instead of \uXXXX escapes the app can't decode.
    body = json.dumps(data, ensure_ascii=False, indent=2).encode("utf-8")
    compressed = gzip.compress(body, compresslevel=9)

    with open(hrl_path, "wb") as f:
        f.write(MAGIC_HRL)
        f.write(compressed)

    print(f"Wrote {hrl_path} ({len(compressed) + 4} bytes, {len(data)} keys)")


def unpack(hrl_path: str, json_path: str) -> None:
    with open(hrl_path, "rb") as f:
        raw = f.read()

    if raw[:4] != MAGIC_HRL:
        sys.exit(f"error: {hrl_path} does not start with the HRL magic bytes "
                  f"(got {raw[:4]!r}) - is this really a .hrl file?")

    body = gzip.decompress(raw[4:])
    data = json.loads(body.decode("utf-8"))

    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)

    print(f"Wrote {json_path} ({len(data)} keys)")


def main() -> None:
    if len(sys.argv) != 4 or sys.argv[1] not in ("pack", "unpack"):
        print(__doc__)
        sys.exit(1)

    cmd, src, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    if cmd == "pack":
        pack(src, dst)
    else:
        unpack(src, dst)


if __name__ == "__main__":
    main()
