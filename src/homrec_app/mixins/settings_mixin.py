from __future__ import annotations

import tkinter as tk
from tkinter import ttk, messagebox, filedialog
from tkinter import colorchooser as cc
import time
import os
import sys
import re
import glob
import json
import gzip
import shutil
import platform
import webbrowser
import subprocess
import threading
import queue
import ctypes
import logging
from datetime import datetime
import cv2
import numpy as np
from PIL import Image, ImageTk, ImageDraw
import mss

from ..core.optional_deps import (_DND_AVAILABLE, _PYAUDIO_AVAILABLE, _pyaudio_mod,
                                   _audioop_mod, wave, HAS_PSUTIL, HAS_TRAY, pystray, TrayItem)
from ..core.constants import (CURRENT_VERSION, GITHUB_REPO, ASSETS_DIR, THEMES_DIR,
                               LANGS_DIR, SETTINGS_PATH, THEME_REQUIRED_KEYS,
                               LANG_REQUIRED_KEYS, LANG_SCHEMA_VERSION,
                               THEME_SCHEMA_VERSION, _HRC_MAGIC, _HRL_MAGIC, _ROOT_DIR)
from ..core.languages import LANGUAGES
from ..core.profile_io import _hrc_write, _hrc_read, _hrc_detect
from ..core.system_utils import find_ffmpeg, optimize_for_performance, rms_to_level_percent
from ..core.updates import check_for_updates, _version_gt

from ..dialogs.welcome_dialog import WelcomeDialog
from ..dialogs.settings_dialog import SettingsDialog
from ..dialogs.advanced_settings_dialog import AdvancedSettingsDialog
from ..dialogs.overlay_manager import OverlayManagerWindow, OverlayPreviewDialog
from ..dialogs.overlays_dock_panel import OverlaysDockPanel
from ..dialogs.audio_panel import AudioPanel
from ..dialogs.audio_level_meter import AudioLevelMeter
from ..dialogs.custom_messagebox import CustomMessageBox

log = logging.getLogger("homrec")


class SettingsMixin:
    """Reserved extension point -- load_settings/save_settings currently
    live in UIMixin (see split.py) because they were originally interleaved
    with window/menu setup and share private helpers with it."""
    pass
