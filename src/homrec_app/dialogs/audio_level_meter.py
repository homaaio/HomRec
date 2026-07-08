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
import subprocess
import threading
from datetime import datetime
import cv2
import numpy as np
from PIL import Image, ImageTk, ImageDraw
import logging

from ..core.optional_deps import (_PYAUDIO_AVAILABLE, _pyaudio_mod, _audioop_mod,
                                   HAS_PSUTIL, HAS_TRAY)
from ..core.constants import (ASSETS_DIR, THEMES_DIR, LANGS_DIR, SETTINGS_PATH,
                               THEME_REQUIRED_KEYS, LANG_REQUIRED_KEYS,
                               LANG_SCHEMA_VERSION, THEME_SCHEMA_VERSION,
                               _HRC_MAGIC, _HRL_MAGIC)
from ..core.languages import LANGUAGES
from ..core.profile_io import _hrc_write, _hrc_read, _hrc_detect

log = logging.getLogger("homrec")


class AudioLevelMeter(tk.Canvas):
    def __init__(self, parent, width: int = 180, height: int = 20, dynamics: int = 5, **kwargs) -> None:
        super().__init__(parent, width=width, height=height, highlightthickness=0, **kwargs)
        self.meter_width = width
        self.meter_height = height
        self.level = 0.0          # smoothed display level
        self._raw_level = 0       # latest raw level from audio thread
        self._peak = 0.0
        self._peak_decay = 0
        self._bar_id = None
        self._peak_id = None
        self.dynamics = max(0, min(10, dynamics))  # 0=off, 1=slow…10=instant
        self.enabled = True
        self._init_canvas()

    def _lerp_color(self, t: float) -> str:
        if t < 0.7:
            s = t / 0.7
            r = int(166 + (249 - 166) * s); g = int(227 + (226 - 227) * s); b = int(161 + (175 - 161) * s)
        else:
            s = (t - 0.7) / 0.3
            r = int(249 + (243 - 249) * s); g = int(226 + (56 - 226) * s); b = int(175 + (168 - 175) * s)
        return f'#{r:02x}{g:02x}{b:02x}'

    def _init_canvas(self) -> None:
        self.delete("all")
        self.create_rectangle(0, 0, self.meter_width, self.meter_height, fill='#1e1e2e', outline='#45475a', width=1)
        self._bar_id = self.create_rectangle(2, 2, 2, self.meter_height - 2, fill='#a6e3a1', outline='')
        self._peak_id = self.create_line(2, 2, 2, self.meter_height - 2, fill='#a6e3a1', width=2, state='hidden')

    def draw_meter(self) -> None:
        if not self.enabled:
            self.coords(self._bar_id, 2, 2, 2, self.meter_height - 2)
            self.itemconfig(self._peak_id, state='hidden')
            return
        inner_w = self.meter_width - 4
        bar_x1 = 2 + max(0, int(self.level / 100 * inner_w))
        self.coords(self._bar_id, 2, 2, bar_x1, self.meter_height - 2)
        self.itemconfig(self._bar_id, fill=self._lerp_color(self.level / 100))
        if self._peak > 1:
            px = 2 + int(self._peak / 100 * inner_w)
            pcol = '#f38ba8' if self._peak > 90 else '#f9e2af' if self._peak > 70 else '#a6e3a1'
            self.coords(self._peak_id, px, 2, px, self.meter_height - 2)
            self.itemconfig(self._peak_id, fill=pcol, state='normal')
        else:
            self.itemconfig(self._peak_id, state='hidden')

    def set_level(self, level: int) -> None:
        if not self.enabled:
            return
        self._raw_level = max(0, min(100, level))

        if self.dynamics == 0:
            self.level = float(self._raw_level)
        else:
            alpha = self.dynamics / 10.0
            self.level = alpha * self._raw_level + (1.0 - alpha) * self.level

        decay_speed = max(1, 4 - self.dynamics // 3)  # 1..4 levels per tick
        if self.level > self._peak:
            self._peak = self.level
            self._peak_decay = max(5, 25 - self.dynamics * 2)  # hold frames
        else:
            if self._peak_decay > 0:
                self._peak_decay -= 1
            else:
                self._peak = max(0.0, self._peak - decay_speed)
        self.draw_meter()
