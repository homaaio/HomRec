from __future__ import annotations

import os
from .profile_io import _get_root_dir
from ._paths import SRC_DIR

CURRENT_VERSION = "1.7.1"
GITHUB_REPO = "homaaio/homrec"


LANG_SCHEMA_VERSION = 1
THEME_SCHEMA_VERSION = 1
LANG_REQUIRED_KEYS = [
    "app_title","live_preview","ready","recording","paused","fps","resolution",
    "start","pause","stop","resume","recording_btn","audio_mixer","microphone",
    "desktop_audio","mute","unmute","vol","level","enable_audio","ffmpeg_found",
    "ffmpeg_not_found","file_menu","open_recordings","exit","view_menu",
    "always_on_top","fullscreen","pc_analytics","cpu_info","ram_info","disk_info",
    "help_menu","check_updates","report_issue","capture_source","full_desktop",
    "select_window","minimize_tray","language","english","russian","theme","dark",
    "light","settings_menu","preferences","performance_menu","ultra","turbo",
    "balanced","eco","stats","time","status","warning","error","info",
    "folder_not_exist","recording_failed","settings_saved","recording_saved",
    "open_folder","ffmpeg_not_found_msg","saved","recording_status","file","size",
    "duration","audio","merged","separate","no_audio","save","cancel","browse",
    "output_folder","settings_title","video_settings","quality","mode","advanced",
    "monitor","output","countdown","timestamp","cursor","notification","made_by","audio_file",
]
THEME_REQUIRED_KEYS = ["bg","surface","accent","text","text_secondary","success","warning","error"]


_ROOT_DIR     = _get_root_dir()
ASSETS_DIR    = os.path.join(_ROOT_DIR, "Assets")
SETTINGS_PATH = os.path.join(_ROOT_DIR, "homrec_settings.json")
THEMES_DIR    = os.path.join(ASSETS_DIR, "Themes")
LANGS_DIR     = os.path.join(ASSETS_DIR, "L")


_HRC_MAGIC = b'HRC\x01'
_HRL_MAGIC = b'HRL\x01'

