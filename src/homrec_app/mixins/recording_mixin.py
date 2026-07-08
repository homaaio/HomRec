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


class RecordingMixin:

    def _detect_gpu_encoder(self) -> str | None:
        return getattr(self, '_gpu_encoder_cache', None)

    def _warm_up_gpu_probe(self) -> None:
        if not self.ffmpeg_path or hasattr(self, '_gpu_encoder_cache'): return
        ffpath = self.ffmpeg_path

        def _probe():
            try:
                from homrec_native import tools_engine as _te, TOOLS_OK as _TOK
                if _TOK and _te:
                    enc = _te.probe_gpu(ffpath)
                    self._gpu_encoder_cache = enc
                    log.info(f"GPU encoder: {enc or 'none'}")
                    return
            except Exception as e: log.debug(f"C++ GPU probe error: {e}")
            _cf = subprocess.CREATE_NO_WINDOW if platform.system()=='Windows' else 0
            for name, args in [
                ('h264_nvenc',['-f','lavfi','-i','nullsrc=s=8x8:d=0.01','-c:v','h264_nvenc','-f','null','-']),
                ('h264_amf',  ['-f','lavfi','-i','nullsrc=s=8x8:d=0.01','-c:v','h264_amf',  '-f','null','-']),
                ('h264_qsv',  ['-f','lavfi','-i','nullsrc=s=8x8:d=0.01','-c:v','h264_qsv',  '-f','null','-']),
            ]:
                try:
                    r = subprocess.run([ffpath, '-y', *args],
                        capture_output=True, timeout=4, creationflags=_cf)
                    if r.returncode == 0:
                        log.info(f"GPU encoder detected: {name}")
                        self._gpu_encoder_cache = name; return
                except Exception: pass
            self._gpu_encoder_cache = None

        threading.Thread(target=_probe, daemon=True).start()

    def _safe_pix_fmt(self) -> str:
        return 'yuv420p'

    def _ddagrab_vf(self) -> str:
        """
        Конвертация GPU-текстуры (ddagrab) → CPU-фрейм для любого энкодера.
        hwdownload — копирует DXGI-текстуру из VRAM в RAM.
        Работает с QSV, NVENC, AMF, libx264. hwmap намеренно не используется
        (требует одного GPU-контекста у источника и энкодера — на практике даёт -40).
        """
        return 'hwdownload,format=bgra,format=yuv420p'

    def _build_codec_args(self) -> list:
        codec = getattr(self, 'video_codec', 'libx264')
        hw    = getattr(self, 'hw_accel', 'auto')
        if codec == 'libx264' and hw == 'auto':
            gpu = self._detect_gpu_encoder()
            if gpu:
                codec = gpu; log.info(f"Auto-upgraded codec → {codec}")
        quality = getattr(self, 'quality', 70)
        fps = getattr(self, 'target_fps', 30)
        cpu_count = os.cpu_count() or 4

        # BUG FIX: enc_preset / enc_crf already existed as real, saved UI
        # settings (Advanced Settings → Video), but were never actually read
        # here — the software-encoder branch below always hardcoded
        # 'superfast' / crf 28 regardless of what the user picked.
        preset_override = (getattr(self, 'enc_preset', '') or '').strip()
        crf_override = getattr(self, 'enc_crf', None)
        custom_args_str = (getattr(self, 'custom_ffmpeg_args', '') or '').strip()
        has_overrides = bool(preset_override or crf_override is not None or custom_args_str)

        args = None
        if not has_overrides:
            # Fast path: hand off to the native encoder-args builder, unchanged.
            try:
                from homrec_native import tools_engine as _te, TOOLS_OK as _TOK
                if _TOK and _te:
                    args = _te.build_codec_args(codec, quality, fps, cpu_count)
            except Exception as e: log.debug(f"C++ build_codec_args error: {e}")

        if args is None:
            # Python fallback — also used whenever overrides are active, since
            # the native builder has no way to accept enc_preset/enc_crf/custom args.
            qp = max(18, min(34, int(34 - (quality / 100) * 16)))
            gop = fps * 2
            is_nvenc = 'nvenc' in codec
            is_qsv   = 'qsv'   in codec
            is_amf   = 'amf'   in codec
            is_265   = codec == 'libx265' or 'hevc' in codec
            args = ['-c:v', codec]
            if is_nvenc:
                args += ['-preset','p1','-tune','ull','-rc','constqp',
                         '-qp',str(qp),'-g',str(gop),
                         '-spatial-aq','1','-aq-strength','8',
                         '-bf','0','-profile:v','high']
            elif is_qsv:
                # BUG FIX: -low_power 1 forces Intel QSV's VDENC hardware path,
                # which on many GPU generations requires width/height divisible
                # by 16 (sometimes 8) — stricter than the general even-dimension
                # rule enforced elsewhere. A resolution like 1200x674 (even, but
                # not a multiple of 16: 674/16 = 42.125) fails to encode with
                # low_power on affected hardware. The standard QSV path handles
                # arbitrary even dimensions fine via internal padding.
                args += ['-preset','veryfast','-look_ahead','0','-low_power','0',
                        '-global_quality',str(qp),'-g',str(gop),'-profile:v','high']
            elif is_amf:
                args += ['-quality','speed','-rc','cqp',
                         '-qp_i',str(qp),'-qp_p',str(qp),
                         '-g',str(gop),'-profile:v','high']
            else:
                thr = max(1, (cpu_count or 4) // 2)
                # Software encoder: honor enc_preset/enc_crf if the user set them
                preset = preset_override or 'superfast'
                crf = crf_override if crf_override is not None else 28
                args += ['-preset', preset, '-tune','zerolatency',
                         '-crf', str(crf), '-g',str(gop),'-threads','2']
                if not is_265: args += ['-profile:v','high','-level','4.2']
                if is_265: args += ['-x265-params','log-level=error:no-open-gop=1']

        if custom_args_str:
            # Raw passthrough — appended last, so it can add to or override
            # anything above (including -c:v itself) for full manual control.
            try:
                import shlex
                args = args + shlex.split(custom_args_str)
            except Exception as e:
                log.warning(f"Invalid custom ffmpeg args, ignoring: {e}")

        return args

    def start_with_countdown(self) -> None:
        if not self.recording:
            self.show_countdown() if self.countdown_var.get() else self.start_recording()
        else:
            self.stop_recording()

    def show_countdown(self) -> None:
        w = tk.Toplevel(self.root); self._set_icon(w)
        W, H = 300, 200
        w.geometry(f"{W}x{H}"); w.configure(bg="#0f0f17"); w.overrideredirect(True)
        try: w.attributes("-alpha", 0.92)
        except: pass
        w.update_idletasks()
        w.geometry(f"{W}x{H}+{(w.winfo_screenwidth()-W)//2}+{(w.winfo_screenheight()-H)//2}")
        w.lift(); w.attributes("-topmost", True)

        cv = tk.Canvas(w, width=W, height=H, bg="#0f0f17", highlightthickness=0)
        cv.pack(fill="both", expand=True)
        cx, cy, r = W//2, H//2 - 10, 60
        cv.create_oval(cx-r, cy-r, cx+r, cy+r, outline="#313244", width=6)
        arc_id = cv.create_arc(cx-r, cy-r, cx+r, cy+r, start=90, extent=0, outline=self.colors.get("success","#a6e3a1"), width=6, style="arc")
        num_id = cv.create_text(cx, cy, text="3", font=("Segoe UI", 42, "bold"), fill=self.colors.get("success","#a6e3a1"))
        hint_id = cv.create_text(cx, cy+r+22, text="Starting recording…", font=("Segoe UI", 10), fill="#6c7086")

        def tick(n: int) -> None:
            if n > 0:
                cv.itemconfig(arc_id, extent=-(n/3)*360, outline=self.colors.get("success","#a6e3a1"))
                cv.itemconfig(num_id, text=str(n), fill=self.colors.get("success","#a6e3a1"))
                w.after(1000, lambda: tick(n - 1))
            else:
                cv.itemconfig(arc_id, extent=-360, outline=self.colors.get("error","#f38ba8"))
                cv.itemconfig(num_id, text="●", fill=self.colors.get("error","#f38ba8"))
                cv.itemconfig(hint_id, text=self.lang["recording_btn"], fill=self.colors.get("error","#f38ba8"))
                w.after(400, w.destroy); self.start_recording()
        tick(3)

    def _make_rec_frames(self) -> list:
        from PIL import ImageFont
        frames = []
        for bright in (True, False):
            w, h = 72, 28
            img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
            d = ImageDraw.Draw(img)
            d.rounded_rectangle([0, 0, w-1, h-1], radius=10, fill=(20, 20, 30, 195))
            d.ellipse([8, 8, 20, 20], fill=(232, 66, 86, 255) if bright else (160, 40, 55, 200))
            try: font = ImageFont.truetype("segoeui.ttf", 13)
            except:
                try: font = ImageFont.truetype("arial.ttf", 13)
                except: font = ImageFont.load_default()
            d.text((26, 6), "REC", font=font, fill=(220, 220, 230, 255))
            frames.append(img)
        return frames

    def _composite_overlays_on_preview(self, img: "Image.Image", pw: int, ph: int) -> "Image.Image":
        overlays = getattr(self, 'overlays', [])
        if not overlays:
            self._sync_webcam_captures(set())
            return img
        try:
            img = img.convert("RGBA")
            active_cam_indices: set = set()
            for ov in overlays:
                if not ov.get('enabled', True):
                    continue
                kind = ov.get('kind', 'text')
                if kind == 'text':
                    text = ov.get('text', '')
                    if not text:
                        continue
                    x = int(ov.get('x', 0.05) * pw)
                    y = int(ov.get('y', 0.05) * ph)
                    rw = getattr(self, 'record_width', 0) or getattr(self, 'original_width', 0) or pw
                    fs_full = int(ov.get('font_size', 24))
                    fs = max(6, int(fs_full * (pw / max(1, rw))))
                    col = ov.get('color', '#ffffff')
                    opacity = ov.get('opacity', 1.0)
                    try:
                        alpha = max(0, min(255, int(opacity * 255)))
                        r = int(col[1:3], 16); g = int(col[3:5], 16); b = int(col[5:7], 16)
                    except Exception:
                        r, g, b, alpha = 255, 255, 255, 255
                    from PIL import ImageFont
                    font = getattr(self, '_preview_overlay_font_cache', {}).get(fs)
                    if font is None:
                        try: font = ImageFont.truetype("segoeui.ttf", fs)
                        except Exception:
                            try: font = ImageFont.truetype("arial.ttf", fs)
                            except Exception: font = ImageFont.load_default()
                        cache = getattr(self, '_preview_overlay_font_cache', {})
                        cache[fs] = font
                        self._preview_overlay_font_cache = cache
                    d = ImageDraw.Draw(img)
                    d.text((x + 1, y + 1), text, font=font, fill=(0, 0, 0, alpha))  # cheap drop shadow
                    d.text((x, y), text, font=font, fill=(r, g, b, alpha))
                elif kind == 'image':
                    path = ov.get('path', '')
                    if not path or not os.path.exists(path):
                        continue
                    ow = max(1, int(ov.get('w', 0.25) * pw))
                    oh = max(1, int(ov.get('h', 0.08) * ph))
                    ox = int(ov.get('x', 0.05) * pw)
                    oy = int(ov.get('y', 0.05) * ph)
                    opacity = ov.get('opacity', 1.0)
                    cache = getattr(self, '_preview_overlay_img_cache', {})
                    cache_key = (path, ow, oh)
                    thumb = cache.get(cache_key)
                    if thumb is None:
                        try:
                            src = Image.open(path).convert("RGBA")
                            src = src.resize((ow, oh), Image.Resampling.BILINEAR)
                            if len(cache) > 16:  # simple cap so this can't grow unbounded
                                cache.clear()
                            cache[cache_key] = src
                            self._preview_overlay_img_cache = cache
                            thumb = src
                        except Exception:
                            continue
                    if opacity < 1.0:
                        a = thumb.split()[-1].point(lambda p: int(p * opacity))
                        thumb = thumb.copy(); thumb.putalpha(a)
                    img.paste(thumb, (ox, oy), thumb)
                elif kind == 'webcam':
                    ow = max(1, int(ov.get('w', 0.25) * pw))
                    oh = max(1, int(ov.get('h', 0.25) * ph))
                    ox = int(ov.get('x', 0.05) * pw)
                    oy = int(ov.get('y', 0.05) * ph)
                    cam_index = ov.get('cam_index', 0)
                    active_cam_indices.add(cam_index)
                    cam_frame = self._get_webcam_preview_frame(cam_index, ow, oh)
                    if cam_frame is not None:
                        opacity = ov.get('opacity', 1.0)
                        if opacity < 1.0:
                            cam_frame = cam_frame.convert("RGBA")
                            a = cam_frame.split()[-1].point(lambda p: int(p * opacity))
                            cam_frame.putalpha(a)
                            img.paste(cam_frame, (ox, oy), cam_frame)
                        else:
                            img.paste(cam_frame, (ox, oy))
                    else:
                        # Camera unavailable (unplugged / in use / still opening) —
                        # fall back to a labeled placeholder instead of showing nothing.
                        d = ImageDraw.Draw(img)
                        d.rectangle([ox, oy, ox + ow, oy + oh], outline=(137, 180, 250, 255), width=2,
                                    fill=(30, 30, 46, 160))
                        d.text((ox + 4, oy + 4), f"📷 Cam {cam_index} (no signal)", fill=(205, 214, 244, 255))
            self._sync_webcam_captures(active_cam_indices)
            return img.convert("RGB")
        except Exception as e:
            log.debug(f"overlay preview composite error: {e}")
            return img.convert("RGB") if img.mode != "RGB" else img

    def _get_webcam_preview_frame(self, cam_index: int, ow: int, oh: int):
        caps = getattr(self, '_webcam_captures', None)
        if caps is None:
            caps = {}
            self._webcam_captures = caps
        now = time.time()
        entry = caps.get(cam_index)
        if entry is None or (entry.get('cap') is None and now >= entry.get('retry_at', 0)):
            cap = None
            try:
                backend = cv2.CAP_DSHOW if platform.system() == 'Windows' else cv2.CAP_ANY
                cap = cv2.VideoCapture(cam_index, backend)
                if not cap.isOpened():
                    cap.release()
                    cap = None
            except Exception as e:
                log.debug(f"webcam preview open failed (cam {cam_index}): {e}")
                cap = None
            entry = {'cap': cap, 'retry_at': now + 5.0}
            caps[cam_index] = entry
        cap = entry.get('cap')
        if cap is None:
            return None
        try:
            ok, frame = cap.read()
        except Exception as e:
            log.debug(f"webcam preview read failed (cam {cam_index}): {e}")
            ok, frame = False, None
        if not ok or frame is None:
            # Device likely unplugged/lost — release it and retry later.
            try: cap.release()
            except Exception: pass
            entry['cap'] = None
            entry['retry_at'] = now + 5.0
            return None
        try:
            frame = cv2.resize(frame, (ow, oh), interpolation=cv2.INTER_LINEAR)
            frame_rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            return Image.fromarray(frame_rgb, "RGB")
        except Exception as e:
            log.debug(f"webcam preview frame convert failed (cam {cam_index}): {e}")
            return None

    def _sync_webcam_captures(self, active_cam_indices: set) -> None:
        """Release any open webcam captures that are no longer used by an
        enabled overlay, so the camera isn't held open (and its indicator
        light left on) once it's no longer needed."""
        caps = getattr(self, '_webcam_captures', None)
        if not caps:
            return
        for idx in list(caps.keys()):
            if idx not in active_cam_indices:
                entry = caps.pop(idx)
                cap = entry.get('cap') if entry else None
                if cap is not None:
                    try: cap.release()
                    except Exception: pass

    def _capture_loop(self) -> None:
        # Python fallback (mss) -----------------------------------------
        import mss as _mss
        sct = _mss.mss()
        try:
            from homrec_native import core as _native_core; _have_native = True
        except Exception:
            _native_core = None; _have_native = False

        while self._preview_running:
            try:
                monitor = getattr(self, 'monitor', None)
                pw = getattr(self, 'preview_width', 640)
                ph = getattr(self, 'preview_height', 360)
                recording = getattr(self, 'recording', False)

                if monitor is None: time.sleep(0.1); continue

                if getattr(self, 'disable_preview', False):
                    try: self._preview_queue.get_nowait()
                    except queue.Empty: pass
                    self._preview_queue.put_nowait(None); time.sleep(0.5); continue

                if recording:
                    # BUG FIX: when paused, skip new screenshots entirely — ffmpeg will duplicate
                    # the last frame automatically.  This saves CPU and avoids stale frames.
                    if getattr(self, 'paused', False):
                        time.sleep(0.05)
                        continue
                    _now = time.monotonic()
                    if _now - getattr(self,'_rec_pv_last_t',0.0) >= 0.25:
                        self._rec_pv_last_t = _now
                        try:
                            screenshot = sct.grab(monitor)
                            sw2, sh2 = screenshot.size
                            if _have_native and _native_core:
                                rgb_np2 = _native_core.bgrx_to_rgb_np(screenshot.bgra, sw2, sh2)
                                small_np2 = _native_core.resize_nearest_np(rgb_np2, sw2, sh2, pw, ph)
                                rec_img = Image.frombuffer("RGB", (pw, ph), small_np2, "raw", "RGB", 0, 1)
                            else:
                                rec_img = Image.frombytes("RGB", screenshot.size, screenshot.bgra, "raw", "BGRX")
                                rec_img = rec_img.resize((pw, ph), Image.Resampling.NEAREST)
                            try: self._preview_queue.get_nowait()
                            except: pass
                            rec_img = self._composite_overlays_on_preview(rec_img, pw, ph)
                            self._preview_queue.put_nowait(rec_img)
                        except Exception: pass
                    time.sleep(0.05); continue

                screenshot = sct.grab(monitor); sw, sh = screenshot.size
                if _have_native and _native_core:
                    rgb_np = _native_core.bgrx_to_rgb_np(screenshot.bgra, sw, sh)
                    small_np = _native_core.resize_bilinear_np(rgb_np, sw, sh, pw, ph)
                    img = Image.frombuffer("RGB", (pw, ph), small_np, "raw", "RGB", 0, 1)
                else:
                    img = Image.frombytes("RGB", screenshot.size, screenshot.bgra, "raw", "BGRX")
                    img.thumbnail((pw, ph), Image.Resampling.BILINEAR)

                try: self._preview_queue.get_nowait()
                except queue.Empty: pass
                img = self._composite_overlays_on_preview(img, pw, ph)
                self._preview_queue.put_nowait(img)

                _now = time.time()
                if not hasattr(self, '_pv_last_t'): self._pv_last_t = _now; self._pv_frame_acc = 0
                self._pv_frame_acc += 1
                if _now - self._pv_last_t >= 2.0:
                    fps_val = self._pv_frame_acc / (_now - self._pv_last_t)
                    self._pv_last_t = _now; self._pv_frame_acc = 0
                    if hasattr(self, '_preview_fps_lbl'):
                        try: self._preview_fps_lbl.config(text=f"{fps_val:.0f} fps")
                        except: pass
            except Exception as e: log.debug(f"_capture_loop error: {e}")
            time.sleep(0.1)  # BUG FIX: was 0.083 (~12 fps) but preview only redraws at 100 ms; align to avoid wasted captures

    def update_preview(self) -> None:
        # Если виджеты были пересозданы (recreate_widgets сбросил флаг) — останавливаем
        # старый цикл; новый уже запущен recreate_widgets.
        if not getattr(self, '_preview_active', True):
            return
        try:
            img = self._preview_queue.get_nowait()
            if img is None:
                self._show_preview_placeholder()
            else:
                photo = ImageTk.PhotoImage(img)
                self.preview_label.config(image=photo, text="")
                self.preview_label.image = photo
        except queue.Empty: pass
        except Exception: pass
        self.root.after(150 if getattr(self, 'recording', False) else 100, self.update_preview)

    def _show_preview_placeholder(self) -> None:
        try:
            pw = getattr(self, 'preview_width', 640)
            ph = getattr(self, 'preview_height', 360)
            cache_key = (pw, ph)
            if getattr(self, '_placeholder_key', None) != cache_key:
                img = Image.new("RGB", (pw, ph), color="#181825")
                draw = ImageDraw.Draw(img)
                for x in range(0, pw, 20):
                    draw.rectangle([x, 0, x+10, 2], fill="#45475a")
                    draw.rectangle([x, ph-2, x+10, ph], fill="#45475a")
                for y in range(0, ph, 20):
                    draw.rectangle([0, y, 2, y+10], fill="#45475a")
                    draw.rectangle([pw-2, y, pw, y+10], fill="#45475a")
                cx, cy, r = pw//2, ph//2 - 20, 40
                draw.ellipse([cx-r, cy-r, cx+r, cy+r], outline="#45475a", width=3)
                draw.ellipse([cx-15, cy-15, cx+15, cy+15], outline="#45475a", width=2)
                try:
                    from PIL import ImageFont; font = ImageFont.truetype("segoeui.ttf", 16)
                except: font = None
                msg = "Preview disabled"
                try:
                    bbox = draw.textbbox((0,0), msg, font=font); tw = bbox[2]-bbox[0]
                except: tw = len(msg)*8
                draw.text((pw//2 - tw//2, cy+r+16), msg, fill="#6c7086", font=font)
                self._placeholder_photo = ImageTk.PhotoImage(img)
                self._placeholder_key = cache_key
            self.preview_label.config(image=self._placeholder_photo, text="")
            self.preview_label.image = self._placeholder_photo
        except Exception: pass

    def _probe_ddagrab_support(self) -> bool:
        if platform.system() != "Windows":
            return False
        if not getattr(self, 'ffmpeg_path', None) or not os.path.exists(self.ffmpeg_path):
            return False
        try:
            r = subprocess.run(
                [self.ffmpeg_path, '-filters'],
                capture_output=True, text=True, timeout=8,
                creationflags=subprocess.CREATE_NO_WINDOW
            )
            supported = 'ddagrab' in (r.stdout + r.stderr)
            log.info("ddagrab support: %s", supported)
            return supported
        except Exception as e:
            log.debug("ddagrab probe failed: %s", e)
            return False

    def _drawtext_fontfile(self) -> str:
        cached = getattr(self, '_drawtext_fontfile_cache', None)
        if cached is not None:
            return cached
        windir = os.environ.get('WINDIR', 'C:\\Windows')
        candidates = [
            os.path.join(windir, 'Fonts', 'segoeui.ttf'),
            os.path.join(windir, 'Fonts', 'arial.ttf'),
        ]
        result = ''
        for path in candidates:
            if os.path.exists(path):
                # ffmpeg filter arg syntax: forward slashes, escape the drive colon
                result = path.replace('\\', '/').replace(':', '\\:')
                break
        self._drawtext_fontfile_cache = result
        return result

    def _build_overlay_vf(self) -> str:
        filters = []
        w = self.record_width or self.original_width or 1920
        h = self.record_height or self.original_height or 1080
        fontfile = self._drawtext_fontfile()
        for ov in getattr(self, 'overlays', []):
            if not ov.get('enabled', True):
                continue
            if ov.get('kind') != 'text':
                continue
            text = ov.get('text', '').replace("'", "\\'").replace(':', '\\:').replace(',', '\\,')
            if not text:
                continue
            x_px = int(ov.get('x', 0.05) * w)
            y_px = int(ov.get('y', 0.05) * h)
            fs   = int(ov.get('font_size', 24))
            col  = ov.get('color', '#ffffff').lstrip('#')
            opacity = ov.get('opacity', 1.0)
            alpha_hex = f"{int(opacity * 255):02x}"
            color_ff = f"0x{col}@0x{alpha_hex}"
            fontfile_part = f"fontfile='{fontfile}':" if fontfile else ""
            filters.append(
                f"drawtext={fontfile_part}text='{text}':x={x_px}:y={y_px}:fontsize={fs}:"
                f"fontcolor={color_ff}:shadowcolor=0x00000080:shadowx=1:shadowy=1"
            )
        return ','.join(filters)

    def _build_filter_graph(self, base_label_in: str = "0:v", use_ddagrab: bool = False) -> tuple[list, str, str | None]:
        extra_inputs: list = []
        graph_parts: list = []
        next_input_idx = 1

        needs_scale = (self.record_width != self.original_width or
                       self.record_height != self.original_height)
        has_any_overlay = any(ov.get('enabled', True) for ov in getattr(self, 'overlays', []))
        needs_hwdownload = use_ddagrab and (needs_scale or has_any_overlay)
        cur_label = base_label_in
        if needs_hwdownload:
            if needs_scale:
                # When using ddagrab, convert D3D11 hardware frames to CPU frames first
                graph_parts.append(f"[{cur_label}]hwdownload,format=bgra,format=yuv420p,scale={self.record_width}:{self.record_height}:flags=fast_bilinear[scaled]")
            else:
                graph_parts.append(f"[{cur_label}]hwdownload,format=bgra,format=yuv420p[scaled]")
            cur_label = "scaled"
        elif needs_scale:
            # Non-ddagrab capture (gdigrab) already yields plain CPU frames —
            # just scale, no hwdownload needed.
            graph_parts.append(f"[{cur_label}]scale={self.record_width}:{self.record_height}:flags=fast_bilinear[scaled]")
            cur_label = "scaled"

        text_vf = self._build_overlay_vf()
        if text_vf:
            graph_parts.append(f"[{cur_label}]{text_vf}[txt]")
            cur_label = "txt"

        w = self.record_width or 1920
        h = self.record_height or 1080

        for ov in getattr(self, 'overlays', []):
            if not ov.get('enabled', True):
                continue
            kind = ov.get('kind')

            if kind == 'image':
                path = ov.get('path', '')
                if not path or not os.path.exists(path):
                    continue
                ow = max(2, int(ov.get('w', 0.25) * w))
                oh = max(2, int(ov.get('h', 0.08) * h))
                ox = int(ov.get('x', 0.05) * w)
                oy = int(ov.get('y', 0.05) * h)
                opacity = ov.get('opacity', 1.0)

                extra_inputs += ['-i', path]
                in_label = f"{next_input_idx}:v"
                scaled_label = f"img{next_input_idx}"
                if opacity < 1.0:
                    graph_parts.append(
                        f"[{in_label}]scale={ow}:{oh},format=rgba,"
                        f"colorchannelmixer=aa={opacity:.2f}[{scaled_label}]"
                    )
                else:
                    graph_parts.append(f"[{in_label}]scale={ow}:{oh}[{scaled_label}]")

                out_label = f"ov{next_input_idx}"
                graph_parts.append(
                    f"[{cur_label}][{scaled_label}]overlay={ox}:{oy}[{out_label}]"
                )
                cur_label = out_label
                next_input_idx += 1

            elif kind == 'webcam':
                cam_idx = ov.get('cam_index', 0)
                ow = max(2, int(ov.get('w', 0.25) * w))
                oh = max(2, int(ov.get('h', 0.25) * h))
                ox = int(ov.get('x', 0.05) * w)
                oy = int(ov.get('y', 0.05) * h)

                if platform.system() == 'Windows':
                    cam_args = ['-f', 'dshow', '-video_size', f'{ow}x{oh}',
                                '-i', f'video={self._dshow_cam_name(cam_idx)}']
                else:
                    cam_args = ['-f', 'v4l2', '-video_size', f'{ow}x{oh}',
                                '-i', f'/dev/video{cam_idx}']
                extra_inputs += cam_args
                in_label = f"{next_input_idx}:v"
                scaled_label = f"cam{next_input_idx}"
                graph_parts.append(f"[{in_label}]scale={ow}:{oh}[{scaled_label}]")

                out_label = f"ov{next_input_idx}"
                graph_parts.append(
                    f"[{cur_label}][{scaled_label}]overlay={ox}:{oy}[{out_label}]"
                )
                cur_label = out_label
                next_input_idx += 1

        if not graph_parts:
            return [], "", None

        filter_complex = ';'.join(graph_parts)
        return extra_inputs, filter_complex, cur_label

    def _probe_dshow_video_devices(self) -> list[str]:
        result = subprocess.run(
            [self.ffmpeg_path, '-list_devices', 'true', '-f', 'dshow', '-i', 'dummy'],
            capture_output=True, text=True, timeout=8,
            creationflags=subprocess.CREATE_NO_WINDOW)
        names = []
        in_video_section = False
        for line in result.stderr.splitlines():
            if 'DirectShow video devices' in line:
                in_video_section = True
                continue
            if 'DirectShow audio devices' in line:
                in_video_section = False
                continue
            if not in_video_section or 'Alternative name' in line:
                continue
            m = re.search(r'"([^"]+)"', line)
            if m:
                names.append(m.group(1))
        return names

    def list_webcams(self) -> list[str]:
        """Return a list of available webcam display names (Windows: DirectShow video devices)."""
        try:
            if platform.system() == 'Windows':
                cached = getattr(self, '_dshow_cam_names_cache', None)
                if cached is None:
                    cached = self._probe_dshow_video_devices()
                    self._dshow_cam_names_cache = cached
                return cached or ["Integrated Webcam"]
            else:
                # On Linux, enumerate /dev/video* nodes as a fallback.
                found = sorted(glob.glob('/dev/video*'))
                return found or ["/dev/video0"]
        except Exception as e:
            log.debug(f"webcam list probe failed: {e}")
            return ["Integrated Webcam"]

    def _dshow_cam_name(self, cam_index: int) -> str:
        try:
            cached = getattr(self, '_dshow_cam_names_cache', None)
            if cached is None:
                cached = self._probe_dshow_video_devices()
                self._dshow_cam_names_cache = cached
            if cached and 0 <= cam_index < len(cached):
                return cached[cam_index]
        except Exception as e:
            log.debug(f"dshow cam name probe failed: {e}")
        return "Integrated Webcam"

    def toggle_recording(self) -> None:
        if not self.recording: self.start_recording()
        else: self.stop_recording()

    def start_recording(self) -> None:
        try:
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            fmt = getattr(self, 'video_format', 'mp4')
            # BUG FIX: filename_template was stored/loaded but never actually used; apply it now.
            template = getattr(self, 'filename_template', 'HomRec_{date}_{time}')
            date_str = datetime.now().strftime("%Y%m%d")
            time_str = datetime.now().strftime("%H%M%S")
            base_name = template.replace('{date}', date_str).replace('{time}', time_str)
            # Strip characters that are invalid in filenames on Windows/Linux
            import re as _re
            base_name = _re.sub(r'[\\/:*?"<>|]', '_', base_name)
            self.filename = f"{self.output_folder}/{base_name}.{fmt}"
            log.info(f"Starting recording: {self.filename}")
            self._notify_recording_start()
            if not self.ffmpeg_path: raise Exception("FFmpeg not found!")

            self.stop_flag = False; self.paused = False; self.frame_count = 0
            if hasattr(self, 'ffmpeg_reader_thread') and self.ffmpeg_reader_thread and self.ffmpeg_reader_thread.is_alive():
                self.ffmpeg_reader_thread.join(timeout=2)

            fps = self.target_fps
            codec_args = self._build_codec_args()
            draw_mouse = '1' if getattr(self, 'cursor_var', None) and self.cursor_var.get() else '0'

            gdi_flags = ['-thread_queue_size','512','-probesize','32',
                         '-analyzeduration','0',
                         '-fflags','nobuffer+genpts','-rtbufsize','256M']
            out_flags = ['-vsync','0','-flush_packets','1','-max_muxing_queue_size','4096']
            if fmt == 'mp4':
                out_flags += ['-movflags', '+faststart']
            elif fmt == 'mkv':
                out_flags += ['-cluster_size_limit', '2M']
            safe_pix_fmt = self._safe_pix_fmt()

            use_ddagrab = getattr(self, 'use_ddagrab', None)
            if use_ddagrab is None:
                use_ddagrab = self._probe_ddagrab_support()
                self.use_ddagrab = use_ddagrab

            extra_inputs, filter_complex, out_pad = self._build_filter_graph(use_ddagrab=(use_ddagrab and self.capture_mode != "window"))
            if out_pad:
                graph_args = ['-filter_complex', filter_complex, '-map', f'[{out_pad}]']
            else:
                graph_args = []

            if use_ddagrab and self.capture_mode != "window":
                dda_flags = ['-thread_queue_size', '512', '-fflags', 'nobuffer']
                mon_idx = max(0, getattr(self, 'monitor_id', 1) - 1)
                dda_src = (
                    f'ddagrab=output_idx={mon_idx}'
                    f':framerate={fps}:draw_mouse={draw_mouse}:dup_frames=1'
                )
                dda_input = ['-f', 'lavfi', '-i', dda_src]
                
                # FIX: Don't use -vf when we have graph_args (scaling).
                # FFmpeg can't mix simple -vf filters with -filter_complex.
                if graph_args:
                    # With scaling: skip the simple -vf filter
                    cmd = [self.ffmpeg_path, '-y', *dda_flags, *dda_input,
                        *extra_inputs, *graph_args,
                        *codec_args, '-pix_fmt', 'yuv420p', *out_flags, '-an', self.filename]
                else:
                    # No scaling: use simple -vf filter
                    dda_vf = ['-vf', self._ddagrab_vf()]
                    cmd = [self.ffmpeg_path, '-y', *dda_flags, *dda_input,
                        *dda_vf, *codec_args,
                        '-pix_fmt', 'yuv420p', *out_flags, '-an', self.filename]
            elif self.capture_mode == "window" and self.capture_window_title:
                cmd = [self.ffmpeg_path, '-y', *gdi_flags,
                       '-f', 'gdigrab', '-framerate', str(fps),
                       '-draw_mouse', draw_mouse,
                       '-i', f'title={self.capture_window_title}',
                       *extra_inputs, *graph_args, *codec_args,
                       '-pix_fmt', safe_pix_fmt, *out_flags, '-an', self.filename]
            else:
                cmd = [self.ffmpeg_path, '-y', *gdi_flags,
                       '-f', 'gdigrab', '-framerate', str(fps),
                       '-draw_mouse', draw_mouse,
                       '-offset_x', str(self.monitor_left),
                       '-offset_y', str(self.monitor_top),
                       '-video_size', f'{self.original_width}x{self.original_height}',
                       '-i', 'desktop',
                       *extra_inputs, *graph_args, *codec_args,
                       '-pix_fmt', safe_pix_fmt, *out_flags, '-an', self.filename]

            log.debug(f"FFmpeg cmd: {' '.join(cmd)}")

            # Выбор пути запуска ffmpeg ---------------------------------
            _pl = getattr(self, 'cpp_pipeline', None)
            _use_cpp_pipe = (
                _pl is not None
                and use_ddagrab  # C++ pipeline использует DXGI, как и ddagrab
                and not graph_args  # с overlays пока только ffmpeg-путь
            )

            if _use_cpp_pipe:
                # C++ pipeline path -------------------------------------
                # Open pipe: C++ → YUV420p → ffmpeg stdin
                import os as _os
                r_fd, w_fd = _os.pipe()
                self._cpp_pipe_read_fd  = r_fd
                self._cpp_pipe_write_fd = w_fd

                # Переключаем pipeline на режим записи
                _pl.set_recording(True, w_fd)
                log.info("C++ pipeline: recording via pipe fd=%d", w_fd)

                # Строим команду ffmpeg для чтения из pipe (YUV420p)
                w_src = self.record_width  or self.original_width  or 1920
                h_src = self.record_height or self.original_height or 1080
                pipe_input = [
                    '-f',            'rawvideo',
                    '-pixel_format', 'yuv420p',
                    '-video_size',   f'{w_src}x{h_src}',
                    '-framerate',    str(fps),
                    '-i',            f'pipe:{r_fd}',
                ]
                pipe_cmd = [
                    self.ffmpeg_path, '-y',
                    *pipe_input,
                    *codec_args,
                    '-pix_fmt', 'yuv420p',
                    *out_flags, '-an', self.filename,
                ]
                log.debug(f"FFmpeg pipe cmd: {' '.join(pipe_cmd)}")

                try:
                    from homrec_native import FfmpegProcess as _FP
                    self._native_ffmpeg = _FP(pipe_cmd)
                    # subprocess.Popen-совместимый stub для stop_recording
                    self.ffmpeg_proc = None
                    self._using_native_ffmpeg = True
                    log.info("C++ FfmpegProcess started (pipe mode)")
                except Exception as _e:
                    log.warning(f"Native FfmpegProcess failed ({_e}), falling back")
                    _pl.set_recording(False, 0)
                    _os.close(r_fd); _os.close(w_fd)
                    self._cpp_pipe_read_fd = self._cpp_pipe_write_fd = -1
                    self._using_native_ffmpeg = False
                    # Fallback: обычный subprocess
                    self.ffmpeg_proc = subprocess.Popen(
                        cmd, stdout=subprocess.DEVNULL,
                        stderr=subprocess.PIPE, stdin=subprocess.PIPE,
                        creationflags=subprocess.CREATE_NO_WINDOW if platform.system()=="Windows" else 0
                    )
            else:
                # Python / ddagrab путь (без C++ pipeline) -------------
                self._using_native_ffmpeg = False
                self.ffmpeg_proc = subprocess.Popen(
                    cmd,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                    stdin=subprocess.PIPE,
                    creationflags=subprocess.CREATE_NO_WINDOW if platform.system()=="Windows" else 0
                )
                try:
                    import psutil as _ps; _fp = _ps.Process(self.ffmpeg_proc.pid)
                    _fp.nice(_ps.HIGH_PRIORITY_CLASS if platform.system()=="Windows" else -10)
                except Exception: pass

            # Stderr reader (только для subprocess-пути)
            self.stop_ffmpeg_reader = False
            if self.ffmpeg_proc is not None:
                self.ffmpeg_reader_thread = threading.Thread(
                    target=self._ffmpeg_reader, daemon=True, name='ffmpeg-stderr'
                )
                self.ffmpeg_reader_thread.start()
            else:
                self.ffmpeg_reader_thread = None

            if self.audio_panel.audio_enabled.get(): self.start_audio_recording()
            self.recording = True; self.start_time = time.time()
            self._set_taskbar_icon(recording=True)
            self.record_btn.config(text=self.lang["stop"], bg=self.colors["error"], command=self.stop_recording)
            self.pause_btn.config(state="normal"); self.stop_btn.config(state="normal")
            self.status_icon.config(fg=self.colors["success"])
            self.status_label.config(text=self.lang["recording"])
            self._update_stats()
        except Exception as e:
            messagebox.showerror(self.lang["error"], f"Failed to start recording:\n{str(e)}")
            log.exception("Failed to start recording")

    def _ffmpeg_reader(self) -> None:
        recent_lines = []
        while not self.stop_flag and not self.stop_ffmpeg_reader and self.ffmpeg_proc and self.ffmpeg_proc.poll() is None:
            try:
                line = self.ffmpeg_proc.stderr.readline()
                if not line: break
                line = line.decode('utf-8', errors='ignore').strip()
                if not line:
                    continue
                if 'frame=' in line:
                    import re as _re
                    m = _re.search(r'frame=\s*(\d+)', line)
                    if m: self.frame_count = int(m.group(1))
                else:
                    recent_lines.append(line)
                    if len(recent_lines) > 40:
                        recent_lines.pop(0)
                    low = line.lower()
                    if any(k in low for k in ('error', 'invalid', 'failed', 'unsupported', 'no such', 'cannot', 'unable')):
                        log.warning(f"ffmpeg: {line}")
                    else:
                        log.debug(f"ffmpeg: {line}")
            except Exception:
                break
        if self.recording and not self.stop_flag and recent_lines:
            log.warning("ffmpeg exited unexpectedly. Last output:\n" + "\n".join(recent_lines[-15:]))

    def _update_stats(self) -> None:
        if self.recording:
            try:
                elapsed = time.time() - self.start_time
                if elapsed > 0 and self.frame_count > 0:
                    self.fps_label.config(text=f"{self.lang['fps']} {self.frame_count/elapsed:.1f}")
                h = int(elapsed // 3600); m = int((elapsed % 3600) // 60); s = int(elapsed % 60)
                self.time_label.config(text=f"{h:02d}:{m:02d}:{s:02d}")
                # BUG FIX: auto_stop_min was stored/loaded but never checked — auto-stop never fired.
                auto_stop = getattr(self, 'auto_stop_min', 0)
                if auto_stop > 0 and elapsed >= auto_stop * 60:
                    log.info(f"Auto-stop triggered after {auto_stop} min")
                    self.stop_recording()
                    return
                proc_alive = (
                    (self.ffmpeg_proc and self.ffmpeg_proc.poll() is None)
                    or getattr(self, '_using_native_ffmpeg', False)
                )
                if proc_alive:
                    # BUG FIX: _last_ffmpeg_size_kb was read but never written, so the status
                    # bar always showed "0 MB".  Read the actual file size from disk instead.
                    try:
                        fn = getattr(self, 'filename', '')
                        if fn and os.path.exists(fn):
                            _kb = os.path.getsize(fn) // 1024
                            self._last_ffmpeg_size_kb = _kb
                        else:
                            _kb = getattr(self, '_last_ffmpeg_size_kb', 0)
                    except OSError:
                        _kb = getattr(self, '_last_ffmpeg_size_kb', 0)
                    _mb = _kb / 1024.0
                    _sz = f"{_mb:.1f} MB" if _mb >= 1.0 else f"{_kb} KB"
                    try:
                        self.file_label.config(
                            text=self.lang['recording_status'].format(
                                size=_mb, frames=self.frame_count))
                    except Exception:
                        self.file_label.config(text=f"{_sz}  {self.frame_count} кадров")
            except: pass
            self.root.after(1000, self._update_stats)

    def stop_recording(self) -> None:
        log.info("Stopping recording...")
        self.recording = False; self.stop_flag = True
        saved_filename = self.filename; saved_start_time = self.start_time
        saved_record_width = self.record_width; saved_record_height = self.record_height; saved_target_fps = self.target_fps

        self._set_taskbar_icon(recording=False)
        self.record_btn.config(text=self.lang["start"], bg=self.colors["success"], command=self.start_with_countdown)
        self.pause_btn.config(state="disabled", text=self.lang["pause"])
        self.stop_btn.config(state="disabled")
        self.status_icon.config(fg=self.colors["warning"])
        self.status_label.config(text="Saving…")
        self.time_label.config(text="00:00:00")
        self.file_label.config(text="Processing…")

        def _finalize():
            self.stop_ffmpeg_reader = True

            if getattr(self, '_using_native_ffmpeg', False):
                # C++ pipeline остановка --------------------------------
                _pl = getattr(self, 'cpp_pipeline', None)
                if _pl:
                    # Сначала останавливаем запись в pipe (C++ перестаёт писать YUV)
                    _pl.set_recording(False, 0)
                    log.info("C++ pipeline: recording stopped")

                # Закрываем write-конец pipe → ffmpeg получает EOF → финализирует файл
                import os as _os
                wfd = getattr(self, '_cpp_pipe_write_fd', -1)
                rfd = getattr(self, '_cpp_pipe_read_fd',  -1)
                if wfd >= 0:
                    try: _os.close(wfd)
                    except OSError: pass
                    self._cpp_pipe_write_fd = -1

                # Ждём завершения ffmpeg (30 с — время финализации moov-атома)
                nff = getattr(self, '_native_ffmpeg', None)
                if nff:
                    clean = nff.stop(timeout_ms=30000)
                    log.info(f"Native ffmpeg stopped cleanly: {clean}")
                    del nff; self._native_ffmpeg = None

                if rfd >= 0:
                    try: _os.close(rfd)
                    except OSError: pass
                    self._cpp_pipe_read_fd = -1
                self._using_native_ffmpeg = False

            elif self.ffmpeg_proc and self.ffmpeg_proc.poll() is None:
                # subprocess путь ---------------------------------------
                try: 
                    self.ffmpeg_proc.stdin.write(b'q')
                    self.ffmpeg_proc.stdin.flush()
                except: 
                    pass
                
                # Wait max 3 seconds for graceful exit (was 30 seconds)
                try:
                    self.ffmpeg_proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    # Force kill if stuck
                    try:
                        self.ffmpeg_proc.terminate()
                        self.ffmpeg_proc.wait(timeout=1)
                    except:
                        try:
                            self.ffmpeg_proc.kill()
                        except:
                            pass

            audio_file = None
            if self.audio_recording: audio_file = self.stop_audio_recording()
            time.sleep(0.25)

            has_ffmpeg = self.check_ffmpeg(); audio_merged = False
            mp3_file = None
            if audio_file and os.path.exists(audio_file) and getattr(self, 'separate_audio_mp3', False) and has_ffmpeg:
                mp3_path = os.path.splitext(saved_filename)[0] + '.mp3'
                try:
                    mp3_cmd = [self.ffmpeg_path, '-y', '-i', audio_file, '-codec:a', 'libmp3lame',
                               '-q:a', '2', '-ar', str(getattr(self, 'audio_sample_rate', 44100)), mp3_path]
                    subprocess.run(mp3_cmd, capture_output=True, timeout=60,
                                   creationflags=subprocess.CREATE_NO_WINDOW if platform.system()=='Windows' else 0)
                    if os.path.exists(mp3_path):
                        mp3_file = mp3_path
                        log.info(f"Separate MP3 saved: {mp3_path}")
                except Exception as e:
                    log.warning(f"MP3 export failed: {e}")

            if audio_file and os.path.exists(saved_filename) and self.audio_panel.audio_enabled.get():
                if has_ffmpeg:
                    self.root.after(0, lambda: self.file_label.config(text="Merging audio…"))
                    audio_merged = self.merge_audio_video(saved_filename, audio_file)

            self.root.after(0, lambda: self._finalize_ui(saved_filename, saved_start_time, saved_record_width, saved_record_height, saved_target_fps, audio_file, audio_merged, has_ffmpeg, mp3_file))

        threading.Thread(target=_finalize, daemon=True).start()

    def _finalize_ui(self, filename, start_time, rec_width, rec_height, target_fps, audio_file, audio_merged, has_ffmpeg, mp3_file=None) -> None:
        self.status_icon.config(fg=self.colors["error"])
        self.status_label.config(text=self.lang["ready"])

        if os.path.exists(filename):
            file_size = os.path.getsize(filename) / (1024 * 1024)
            duration = time.time() - start_time
            try:
                probe_cmd = [self.ffmpeg_path, '-i', filename, '-f', 'null', '-']
                probe_result = subprocess.run(probe_cmd, capture_output=True, text=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW if platform.system()=='Windows' else 0)
                import re
                for line in probe_result.stderr.split('\n'):
                    if 'Duration:' in line:
                        match = re.search(r'Duration: (\d+):(\d+):([\d.]+)', line)
                        if match:
                            h, m, s = match.groups()
                            duration = int(h)*3600 + int(m)*60 + float(s)
                        break
            except Exception: pass

            self.file_label.config(text=self.lang["saved"].format(size=file_size, duration=duration))
            audio_status = self.lang["merged"] if audio_merged else (self.lang["separate"] if audio_file else self.lang["no_audio"])
            info_lines = [
                f"{self.lang['file']} {os.path.basename(filename)}",
                f"{self.lang['size']} {file_size:.1f} MB",
                f"{self.lang['duration']} {duration:.1f} sec",
                f"{self.lang['resolution']} {rec_width}x{rec_height}",
                f"{self.lang['fps']} {target_fps}",
                f"{self.lang['audio']} {audio_status}",
            ]
            if audio_file and not audio_merged:
                info_lines.append(f"{self.lang['audio_file']} {os.path.basename(audio_file)}")
            if mp3_file and os.path.exists(mp3_file):
                info_lines.append(f"🎵 MP3: {os.path.basename(mp3_file)}")
            if not has_ffmpeg and audio_file:
                info_lines.extend(["", self.lang["ffmpeg_not_found_msg"]])

            if self.show_summary:
                dont_show_var = tk.BooleanVar(value=False)
                result = CustomMessageBox.show(self, "recording_saved", "recording_saved", "\n".join(info_lines), dont_show_var)
                if dont_show_var.get():
                    self.show_summary = False; self.save_settings(silent=True)
                if result: self.open_recordings()
        else:
            self.file_label.config(text=self.lang["recording_failed"])
            messagebox.showerror(self.lang["error"], self.lang["recording_failed"])

    def toggle_pause(self) -> None:
        if self.recording:
            self.paused = not self.paused
            if self.paused:
                # Pause audio too
                if self.audio_recording and hasattr(self, '_ae') and self._ae:
                    try:
                        self._ae.pause()
                    except Exception as e:
                        log.warning(f"Audio pause failed: {e}")
                
                self.pause_btn.config(text=self.lang["resume"], bg=self.colors["success"])
                self.status_icon.config(fg=self.colors["warning"])
                self.status_label.config(text=self.lang["paused"])
                self._pause_start = time.time()
            else:
                # Resume audio
                if self.audio_recording and hasattr(self, '_ae') and self._ae:
                    try:
                        self._ae.resume()
                    except Exception as e:
                        log.warning(f"Audio resume failed: {e}")
                
                self.pause_btn.config(text=self.lang["pause"], bg=self.colors["warning"])
                self.status_icon.config(fg=self.colors["success"])
                self.status_label.config(text=self.lang["recording"])
                if hasattr(self, '_pause_start'):
                    self.start_time += time.time() - self._pause_start
                    del self._pause_start

    def _show_welcome_and_save(self) -> None:
        self.save_settings(silent=True); WelcomeDialog.show(self)

