from __future__ import annotations

# Import order matters: logging must be configured before any other module
# in this package calls logging.getLogger(...), matching the original
# homrec.py where setup_logging() ran as the very first executable statement.
from .core.logging_setup import setup_logging
setup_logging()

from .app import HomRecScreen  # noqa: E402  (must come after setup_logging())

__all__ = ["HomRecScreen"]
