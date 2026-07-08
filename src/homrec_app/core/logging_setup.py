from __future__ import annotations

import os
import sys
import logging


def setup_logging() -> None:
    if getattr(sys, 'frozen', False):
        log_dir = os.path.dirname(sys.executable)
    else:
        _src = os.path.dirname(os.path.abspath(__file__))
        _parent = os.path.dirname(_src)
        log_dir = _parent if (os.path.isdir(os.path.join(_parent, "src")) or os.path.basename(_src).lower() == "src") else _src
    logging.basicConfig(
        level=logging.DEBUG,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
        handlers=[logging.FileHandler(os.path.join(log_dir, "homrec.log"), encoding="utf-8")]
    )
