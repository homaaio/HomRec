from __future__ import annotations

from .core.logging_setup import setup_logging
setup_logging()

from .app import HomRecScreen  # noqa: E402  (must come after setup_logging())

__all__ = ["HomRecScreen"]
