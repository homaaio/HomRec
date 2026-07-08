from __future__ import annotations

import os
import sys
import json
import gzip


def _get_root_dir() -> str:
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    _src = os.path.dirname(os.path.abspath(__file__))
    _parent = os.path.dirname(_src)
    if os.path.isdir(os.path.join(_parent, "src")) or os.path.basename(_src).lower() == "src":
        return _parent
    return _src


_HRC_MAGIC = b'HRC\x01'
_HRL_MAGIC = b'HRL\x01'


def _hrc_write(path: str, data: dict, magic: bytes) -> None:
    body = gzip.compress(json.dumps(data, indent=2, ensure_ascii=False).encode('utf-8'))
    with open(path, 'wb') as f:
        f.write(magic); f.write(body)

def _hrc_read(path: str, expected_magic: bytes) -> dict:
    with open(path, 'rb') as f:
        magic = f.read(4); body = f.read()
    if magic != expected_magic:
        raise ValueError(f"Invalid file format. Expected {expected_magic!r}, got {magic!r}")
    return json.loads(gzip.decompress(body).decode('utf-8'))

def _hrc_detect(path: str) -> str:
    with open(path, 'rb') as f:
        magic = f.read(4)
    if magic == _HRC_MAGIC: return 'hrc'
    if magic == _HRL_MAGIC: return 'hrl'
    raise ValueError(f"Not a HomRec file (magic={magic!r})")
