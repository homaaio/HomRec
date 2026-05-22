"""core/ffmpeg_utils.py — FFmpeg utilities"""
import os, sys, subprocess, platform, logging, shutil
log = logging.getLogger("homrec.ffmpeg")

try:
    import cv2  # optional: used only for small perf tuning
except Exception:
    cv2 = None

def find_ffmpeg() -> str | None:
    """Find FFmpeg in system or in program directory.

    When running as a PyInstaller .exe:
      - __file__ points to the temp _MEIXXXXXX unpack folder, NOT the .exe folder
      - os.getcwd() is wherever the user launched from, NOT the .exe folder
      - sys.executable is always the actual .exe path, so its directory IS the
        folder the user placed ffmpeg.exe next to the app.
    """
    # 1. Folder containing the running .exe (or .py script)
    if getattr(sys, 'frozen', False):
        # PyInstaller sets sys.frozen=True and sys.executable = path to .exe
        app_dir = os.path.dirname(sys.executable)
    else:
        app_dir = os.path.dirname(os.path.abspath(__file__))

    for name in ('ffmpeg.exe', 'ffmpeg'):
        candidate = os.path.join(app_dir, name)
        if os.path.exists(candidate):
            return candidate

    # 2. Same directory as cwd (fallback, works when running .py directly)
    for name in ('ffmpeg.exe', 'ffmpeg'):
        if os.path.exists(name):
            return os.path.abspath(name)

    # 3. System PATH
    ffmpeg_path = shutil.which("ffmpeg")
    if ffmpeg_path:
        return ffmpeg_path

    return None

def optimize_for_performance() -> None:
    """Apply optimizations for low-end PCs"""
    try:
        import psutil
        p = psutil.Process()
        p.nice(psutil.HIGH_PRIORITY_CLASS)
    except:
        pass
    
    # OpenCV is optional; if present, disable its internal thread pool.
    if cv2 is not None:
        try:
            cv2.setNumThreads(0)
        except Exception:
            pass

