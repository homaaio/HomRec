from __future__ import annotations

import io
import json
import gzip
import zipfile
import sys


HRP_MAGIC = b"HRP\x01"


def patch_hrp_author(hrp_path: str, new_author: str) -> None:
    with open(hrp_path, "rb") as f:
        magic = f.read(4)
        body = f.read()

    if magic != HRP_MAGIC:
        raise ValueError(f"Not a HomRec plugin (.hrp). magic={magic!r}")

    zip_data = gzip.decompress(body)
    zin = zipfile.ZipFile(io.BytesIO(zip_data), "r")
    names = zin.namelist()
    if "plugin.json" not in names:
        raise ValueError("plugin.json not found in .hrp")

    manifest = json.loads(zin.read("plugin.json").decode("utf-8"))
    manifest["author"] = new_author
    new_manifest_bytes = json.dumps(manifest, ensure_ascii=False, indent=2).encode("utf-8")

    out_buf = io.BytesIO()
    with zipfile.ZipFile(out_buf, "w", zipfile.ZIP_DEFLATED) as zout:
        for name in names:
            if name.endswith("/"):
                continue
            if name == "plugin.json":
                zout.writestr("plugin.json", new_manifest_bytes)
            else:
                zout.writestr(name, zin.read(name))
    zin.close()

    compressed = gzip.compress(out_buf.getvalue())
    with open(hrp_path, "wb") as f:
        f.write(HRP_MAGIC)
        f.write(compressed)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("Usage: python tools/patch_hrp_author.py <plugin.hrp> <new_author>")
        return 2
    patch_hrp_author(argv[1], argv[2])
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

