"""core/hrc.py — HomRec binary file formats"""
import os, gzip, json, logging
log = logging.getLogger("homrec.hrc")

_HRC_MAGIC = b'HRC\x01'
_HRL_MAGIC = b'HRL\x01'
_HRT_MAGIC = b'HRT\x01'

def _hrc_write(path: str, data: dict, magic: bytes) -> None:
    """Write a HomRec binary file (magic header + gzip JSON)."""
    body = gzip.compress(json.dumps(data, indent=2, ensure_ascii=False).encode('utf-8'))
    with open(path, 'wb') as f:
        f.write(magic)
        f.write(body)

def _hrc_read(path: str, expected_magic: bytes) -> dict:
    """Read a HomRec binary file. Raises ValueError if magic doesn't match."""
    with open(path, 'rb') as f:
        magic = f.read(4)
        body = f.read()
    if magic != expected_magic:
        raise ValueError(f"Invalid file format. Expected {expected_magic!r}, got {magic!r}")
    return json.loads(gzip.decompress(body).decode('utf-8'))

def _hrc_detect(path: str) -> str:
    """Return 'hrc', 'hrl', 'hrt' or raise ValueError."""
    with open(path, 'rb') as f:
        magic = f.read(4)
    if magic == _HRC_MAGIC: return 'hrc'
    if magic == _HRL_MAGIC: return 'hrl'
    if magic == _HRT_MAGIC: return 'hrt'
    raise ValueError(f"Not a HomRec file (magic={magic!r})")



