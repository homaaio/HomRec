"""core/constants.py — HomRec 2.0 constants"""
import os
import logging
import threading

log = logging.getLogger("homrec.constants")

CURRENT_VERSION = "2.0.0"
CORE_VERSION    = "1.4.3"
GITHUB_REPO = "homaaio/homrec"


def _version_gt(a: str, b: str) -> bool:
    """Return True if version string a is greater than b."""
    try:
        return tuple(int(x) for x in a.split(".")) > tuple(int(x) for x in b.split("."))
    except:
        return False

# ==================== LANGUAGE FILES ====================

def check_for_updates(callback) -> None:
    """Fetch latest release tag from GitHub in a background thread.
    Calls callback(latest_version: str) if a newer version is found.
    """
    def _fetch():
        try:
            import urllib.request
            import json as _json
            url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
            req = urllib.request.Request(url, headers={"User-Agent": "HomRec"})
            with urllib.request.urlopen(req, timeout=5) as resp:
                data = _json.loads(resp.read().decode())
            tag = data.get("tag_name", "").lstrip("v")
            if tag and _version_gt(tag, CURRENT_VERSION):
                log.info(f"Update available: v{tag}")
                callback(tag)
            else:
                log.info("No updates found")
        except Exception as e:
            log.warning(f"Update check failed: {e}")

    threading.Thread(target=_fetch, daemon=True).start()


LANG_SCHEMA_VERSION  = 1
THEME_SCHEMA_VERSION = 1

# Required keys for each schema
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
    "monitor","output","countdown","timestamp","cursor","notification","made_by",
    "audio_file",
]

THEME_REQUIRED_KEYS = ["bg","surface","accent","text","text_secondary",
                        "success","warning","error"]

ASSETS_DIR   = "Assets"
THEMES_DIR   = os.path.join(ASSETS_DIR, "Themes")
LANGS_DIR    = os.path.join(ASSETS_DIR, "L")

# ── HomRec binary file format helpers ────────────────────────────────────────
# Format: 4-byte magic header + gzip-compressed JSON body
# HRC = profile, HRL = language, HRT = theme