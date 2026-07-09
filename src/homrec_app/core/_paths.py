from __future__ import annotations

import os
import sys


def _compute_dirs() -> tuple[str, str]:
    """Returns (root_dir, src_dir), walking up from this file's own location
    (two levels deeper than the original src/homrec.py)."""
    if getattr(sys, "frozen", False):
        exe_dir = os.path.dirname(sys.executable)
        return exe_dir, exe_dir

    _core_dir = os.path.dirname(os.path.abspath(__file__))   # .../src/homrec_app/core
    _app_dir = os.path.dirname(_core_dir)                     # .../src/homrec_app
    _src_dir = os.path.dirname(_app_dir)                       # .../src  (== original __file__ dir)
    _parent = os.path.dirname(_src_dir)
    if os.path.isdir(os.path.join(_parent, "src")) or os.path.basename(_src_dir).lower() == "src":
        root_dir = _parent
    else:
        root_dir = _src_dir
    return root_dir, _src_dir


ROOT_DIR, SRC_DIR = _compute_dirs()
