from __future__ import annotations

import os
import logging

from ._paths import ROOT_DIR


def setup_logging() -> None:
    log_dir = ROOT_DIR
    logging.basicConfig(
        level=logging.DEBUG,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
        handlers=[logging.FileHandler(os.path.join(log_dir, "homrec.log"), encoding="utf-8")]
    )
