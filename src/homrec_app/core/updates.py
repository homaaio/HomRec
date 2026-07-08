from __future__ import annotations

import threading
import logging
from .constants import CURRENT_VERSION, GITHUB_REPO

log = logging.getLogger("homrec")


def check_for_updates(callback) -> None:
    def _fetch():
        try:
            import urllib.request, json as _json
            url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
            req = urllib.request.Request(url, headers={"User-Agent": "HomRec"})
            with urllib.request.urlopen(req, timeout=5) as resp:
                data = _json.loads(resp.read().decode())
            tag = data.get("tag_name", "").lstrip("v")
            if tag and _version_gt(tag, CURRENT_VERSION):
                callback(tag)
        except Exception as e:
            log.warning(f"Update check failed: {e}")
    threading.Thread(target=_fetch, daemon=True).start()

def _version_gt(a: str, b: str) -> bool:
    try:
        return tuple(int(x) for x in a.split(".")) > tuple(int(x) for x in b.split("."))
    except:
        return False
