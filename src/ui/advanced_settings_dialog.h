// advanced_settings_dialog.h — Phase 5b
//
// Port of homrec_app/dialogs/advanced_settings_dialog.py's non-overlay
// tabs (codec/hardware, hotkeys, audio/misc — the overlays tab is
// overlay_manager.h/.cpp instead, since that's a substantial subsystem of
// its own).
//
// IMPORTANT GAP, flagged rather than silently worked around: hr_settings.cpp
// (the existing native settings store) only persists a fixed field set —
// output_folder/quality/fps/monitor/codec/audio/countdown/timestamp/cursor/
// notification/theme/language/minimize_tray/always_on_top/performance/dxgi.
// It has no fields for hw_accel, enc_preset, enc_crf, custom_ffmpeg_args,
// pix_fmt, audio_sample_rate/bitrate/channels, filename_template,
// auto_stop_min, replay_buffer_sec, video_format, separate_audio_mp3, or the
// three hotkeys — all of which this dialog edits. Those edits update
// AppState in memory (so they work for the current run), but won't survive
// an app restart until hr_settings.cpp's struct + JSON reader/writer are
// extended with the extra fields. That's a small, mechanical change to an
// existing core file, not something to do silently as a side effect of a UI
// dialog — flagging it here as a named follow-up instead.
#pragma once

#include <windows.h>
#include "app_state.h"

bool ShowAdvancedSettingsDialog(HWND parent, HINSTANCE hInst, AppState &state);
