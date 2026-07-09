from __future__ import annotations

import warnings
warnings.filterwarnings("ignore", category=DeprecationWarning)

try:
    from tkinterdnd2 import DND_FILES, TkinterDnD
    _DND_AVAILABLE = True
except ImportError:
    _DND_AVAILABLE = False

try:
    import pyaudio as _pyaudio_mod
    import audioop as _audioop_mod
    _PYAUDIO_AVAILABLE = True
except ImportError:
    _pyaudio_mod = None
    _audioop_mod = None
    _PYAUDIO_AVAILABLE = False

try:
    import wave
except ImportError:
    wave = None

try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    HAS_PSUTIL = False

try:
    import pystray
    from pystray import MenuItem as TrayItem
    HAS_TRAY = True
except ImportError:
    HAS_TRAY = False
