// recording_controller.h
//
// Port of homrec_app/mixins/recording_mixin.py. Talks directly to the
// existing native pipeline (hr_pl_*, hr_capture_ctl's hr_ctl_*), the ffmpeg
// process runner (hr_ff_*), and the ffmpeg discovery/codec-arg helpers in
// hr_tools.cpp - all already implemented, so this class is glue around the
// existing decision logic (codec fallback, GPU probe, filename templating),
// not new engine code.
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>
#include "app_state.h"
#include "../hr_overlay_render.h"

class RecordingController {
public:
    explicit RecordingController(AppState &state);
    ~RecordingController();

    // One-time setup at app startup: locates ffmpeg (hr_check_ffmpeg),
    // probes GPU encoder availability (hr_probe_gpu), and initializes audio
    // (hr_audio_init). Mirrors HomRecScreen.__init__'s startup sequence.
    void Initialize();

    bool ffmpeg_found() const { return ffmpeg_found_; }
    const std::wstring &resolved_ffmpeg_path() const { return ffmpeg_path_; }
    const std::wstring &resolved_hw_encoder() const { return hw_encoder_; }

    // Start/stop/pause - return false with `error_out` populated on failure
    // (folder missing, ffmpeg missing, pipeline create failed, etc.).
    bool Start(std::wstring &error_out);
    void Stop();          // matches "stop_recording()" - merges audio, updates AppState.recording
    void TogglePause();

    // Called on a timer (e.g. every 250-500ms) to refresh AppState.frame_count and pull
    // stats for the status bar / console.
    void PollStats();

    // Copies the latest preview frame (RGB24) into `out`, sized
    // `out_w`*`out_h`*3. Returns false if no frame is ready yet or capture
    // isn't running. Caller (main_window's WM_PAINT / preview timer) owns
    // the buffer.
    bool GetPreviewFrame(std::vector<uint8_t> &out, int &out_w, int &out_h);

    // Pushes the current AppState.overlays list into the running/preview
    // pipeline so it actually gets composited into captured frames (both
    // the live preview and, once recording, the encoded output). Cheap
    // enough to call on every UI tick (main_frame's preview timer does) --
    // just copies a handful of small structs unless overlays are empty.
    void SyncOverlays();

    // Runs a preview-only capture pipeline (frames captured + thumbnailed
    // for the UI, nothing written to disk) independent of whether an
    // actual recording is in progress - previously the pipeline only
    // existed at all between Start()/Stop(), so the preview stayed dark
    // until you were already recording. Safe to call repeatedly; no-ops
    // if already running or if the user has disabled preview in Settings.
    void EnsurePreview();
    // Tears down the preview-only pipeline (only if not currently
    // recording - recording owns the pipeline while it's active).
    void TeardownPreview();
    // Called after Settings is saved: re-syncs the preview-only pipeline
    // with whatever changed - on/off (disable_preview), but also monitor
    // or resolution, which an already-running preview pipeline wouldn't
    // otherwise pick up on its own. No-ops while actually recording
    // (that pipeline is owned by the recording until Stop()).
    void RefreshPreviewSettings();
    // "AFK/idle CPU" fix: the preview-only capture pipeline (a real DXGI
    // capture thread) used to run continuously from app launch onward
    // regardless of whether anything was actually visible to show it to
    // - main_frame.cpp now calls this on wxEVT_SHOW (covers minimize-to-
    // tray, tray double-click restore, and the tray menu's Restore item
    // alike, since all three ultimately go through Show()/Hide()) so the
    // pipeline pauses while the window is hidden and picks back up when
    // it's shown again. No-ops while actually recording - same "the
    // recording owns the pipeline until Stop()" rule as TeardownPreview().
    void SetPreviewVisible(bool visible);

    // "Apply with preview off" (overlays_dock_panel.cpp's row context
    // menu): grabs one frame for the overlay editor even when the user
    // has Settings > Disable live preview on, by starting the preview
    // pipeline just long enough to capture it. If preview was already
    // running (enabled, or a recording in progress) this is just a
    // GetPreviewFrame() call. `first_call` should be true only for the
    // very first snapshot of an editing session (it may need to start the
    // pipeline and wait briefly for the first frame); pass false for
    // subsequent "Refresh screenshot" calls, which just re-read the
    // already-running pipeline's latest frame instantly.
    bool CaptureSnapshotFrame(std::vector<uint8_t> &out, int &out_w, int &out_h, bool first_call);
    // Ends an "Apply with preview off" editing session - tears the
    // preview pipeline back down if Settings > Disable live preview is
    // still on (CaptureSnapshotFrame() only started it for the snapshot,
    // it shouldn't keep running afterward), leaves it alone otherwise.
    void EndSnapshotEditing();

    bool recording() const { return state_.recording; }
    bool paused() const { return state_.paused; }
    double elapsed_seconds() const;
    std::wstring elapsed_formatted() const;
    double output_size_mb() const;
    int frame_count() const;

    // True once PollStats() has seen several consecutive ticks (~1.5s) of
    // real frame drops while actively recording. Meant for a UI warning
    // ("system can't keep up"), not a hard error - the recording keeps
    // going either way. Clears itself as soon as drops stop, no separate
    // reset call needed.
    bool overloaded() const { return overloaded_; }

    // True once the preview-only pipeline has failed to (re)start several
    // times in a row (e.g. DXGI dx_create() keeps returning null - RDP,
    // a virtual display, or a monitor that just changed mode) and
    // SyncOverlays() has backed off to its slow retry cadence instead of
    // hammering it every 2s. Purely informational - callers can use this
    // to show a clearer "capture unavailable" placeholder instead of a
    // preview that just looks frozen. Always false while disable_preview
    // is set (nothing is being attempted) or while a recording owns the
    // pipeline.
    bool preview_capture_unavailable() const {
        return preview_unavailable_ && !state_.disable_preview && !state_.recording;
    }

    // Snapshot of the recording that just finished, taken at the moment
    // Stop() runs (before ctl_/ffproc_ are torn down / reset to IDLE, at
    // which point elapsed_seconds()/output_size_mb() would report 0 -- see
    // Stop()'s use of hr_ctl_stop()'s return value). Valid until the next
    // Start(); this is what the post-recording summary popup should read
    // instead of the live accessors above.
    const std::wstring &last_output_path() const { return last_output_path_; }
    std::wstring last_duration_formatted() const;
    double last_output_size_mb() const { return last_output_size_mb_; }
    int capture_width() const { return capture_w_; }
    int capture_height() const { return capture_h_; }
    // The actual resolution the video ends up at (after Settings >
    // Resolution scaling) - what capture_width()/height() report is the
    // raw capture size, which is always native and not what most UI
    // should be showing as "the recording's resolution".
    int output_width() const { return output_w_ > 0 ? output_w_ : capture_w_; }
    int output_height() const { return output_h_ > 0 ? output_h_ : capture_h_; }
    double current_fps() const { return current_fps_; }

    // Called by AudioPanel whenever a mic/system volume slider or mute
    // checkbox changes, so Start()/Stop() know what to actually record
    // instead of the previous hardcoded "mic+sys both on, full volume".
    // Reflects the AudioPanel's current mic_vol/sys_vol/*_mute state so
    // Start()/Stop() record with the levels actually shown in the UI.
    void SetAudioLevels(float mic_vol, float sys_vol, bool mic_muted, bool sys_muted) {
        mic_vol_ = mic_vol; sys_vol_ = sys_vol;
        mic_muted_ = mic_muted; sys_muted_ = sys_muted;
    }

private:
    // Builds "HomRec_{date}_{time}"-style filename from
    // AppState.filename_template via hr_filename_from_template, and the
    // full codec argument string via hr_build_codec_args (falls back to a
    // software x264 path if the probed GPU encoder fails to actually start
    // - same fallback behavior recording_mixin.py has).
    std::wstring BuildOutputPath();
    std::wstring BuildCodecArgs(const std::wstring &codec);
    // Resolves capture_w_/capture_h_ from the selected monitor + scale
    // factor (was inline in Start() only; EnsurePreview() needs the same
    // logic to size its preview-only pipeline).
    void ResolveCaptureSize();
    // Applies Settings > General's "Preview quality" (preview_quality_pct)
    // to the preview panel's pixel size before it's handed to the
    // pipeline as its thumbnail render target - the pipeline (and the GPU/
    // CPU time it spends compositing overlays/cursor into the thumbnail
    // and box-filtering it down) only ever sees the *scaled* size, so a
    // lower quality setting genuinely saves work, not just visual detail.
    void ScaledPreviewSize(int &out_w, int &out_h) const;
    // Settings > Resolution: src_w/src_h (native monitor or cropped window
    // rect) -> desired output size, honoring Percent vs Absolute mode. See
    // the .cpp for the no-upscale/even-dimensions rules.
    void ComputeOutputDims(int src_w, int src_h, int &out_w, int &out_h) const;

    AppState &state_;

    void *pipeline_ = nullptr;   // hr_pl_create() handle

    // TeardownPreview() hands the actual hr_pl_destroy() off to a background
    // thread (see its own comment for why - avoids freezing the UI while a
    // stuck DXGI capture times out). That thread used to be fully detached
    // and untracked: if it was still running (which a stuck capture can
    // stretch to several seconds) when the app closed, it kept touching
    // globals (the logger, the DXGI/D3D11 libs) that the CRT/DLL shutdown
    // sequence was concurrently tearing down out from under it - the
    // "unhandled C++ exception / std::terminate()" crash some users hit
    // right around closing the app after toggling preview off. Tracking it
    // here and joining it (in the destructor, and before starting a new
    // one) keeps every pipeline teardown finished before anything it
    // depends on goes away.
    std::thread preview_teardown_thread_;
    void *ctl_ = nullptr;        // hr_ctl_create() handle
    void *ffproc_ = nullptr;     // hr_ff_create() handle

    bool ffmpeg_found_ = false;
    std::wstring ffmpeg_path_;
    std::wstring hw_encoder_;    // empty if no GPU encoder available -> software fallback
    std::wstring current_output_path_;

    // See last_output_path()/last_duration_formatted()/last_output_size_mb()
    // above - populated by Stop() right before the values they snapshot
    // become unavailable/zeroed.
    std::wstring last_output_path_;
    double last_duration_sec_ = 0.0;
    double last_output_size_mb_ = 0.0;

    // Mic device id actually applied to the currently-running continuous
    // audio capture (see Init()'s hr_audio_start() and
    // RefreshPreviewSettings() below) -- compared against state_.mic_device_id
    // so a Settings change to the microphone picker only restarts capture
    // when it actually changed, not on every settings-dialog close.
    std::string applied_mic_device_id_;

    // Snapshot of capture-affecting settings last used to (re)build the
    // preview pipeline - see RefreshPreviewSettings()'s BUGFIX comment.
    // Anything NOT in this list (theme, hotkeys, output folder, etc.)
    // closing Settings should never touch the pipeline for.
    struct PreviewCaptureSettings {
        bool disable_preview = false;
        int monitor_id = -1;
        CaptureMode capture_mode = CaptureMode::Desktop;
        std::string capture_window_title;
        int target_fps = -1;
        int preview_width = -1, preview_height = -1;
        int preview_quality_pct = -1;
        // preview_fps deliberately excluded - hr_pl_set_preview_fps() applies
        // it cheaply to an already-running pipeline without touching the
        // DXGI duplication interface at all, so it's applied unconditionally
        // below instead of being a reason to rebuild.
        bool operator==(const PreviewCaptureSettings &o) const {
            return disable_preview == o.disable_preview && monitor_id == o.monitor_id &&
                   capture_mode == o.capture_mode && capture_window_title == o.capture_window_title &&
                   target_fps == o.target_fps && preview_width == o.preview_width &&
                   preview_height == o.preview_height && preview_quality_pct == o.preview_quality_pct;
        }
    };
    PreviewCaptureSettings applied_preview_capture_settings_;
    bool applied_preview_capture_settings_valid_ = false;

    // Last overlay list actually pushed to the pipeline via
    // hr_pl_set_overlays(), so SyncOverlays() (called every preview timer
    // tick, i.e. continuously while the app is open) can skip rebuilding
    // and re-sending an identical list instead of doing that work ~20-60
    // times a second regardless of whether anything changed.
    std::vector<HrOverlayDesc> last_overlays_sent_;
    bool last_overlays_sent_valid_ = false;

    std::chrono::steady_clock::time_point next_preview_retry_{};
    int preview_retry_streak_ = 0;
    // Set once preview_retry_streak_ crosses kPreviewRetryBackoffAfter,
    // cleared again on the next successful EnsurePreview(). See
    // preview_capture_unavailable() above.
    bool preview_unavailable_ = false;
    // First few failures retry quickly (kPreviewRetryBaseSeconds) in case
    // it's a one-off (display mode still settling, a game briefly holding
    // exclusive fullscreen); past that we're almost certainly looking at
    // something that won't resolve itself second-to-second (RDP session,
    // no monitor at all), so back off to kPreviewRetryMaxSeconds instead
    // of spinning dx_create() forever every 2s (see the "Pipeline create
    // failed: dx_create() returned null" flood this used to produce).
    static constexpr int kPreviewRetryBaseSeconds = 2;
    static constexpr int kPreviewRetryMaxSeconds = 30;
    static constexpr int kPreviewRetryBackoffAfter = 5;
    int mic_level_ = 0, sys_level_ = 0;
    int capture_w_ = 0, capture_h_ = 0; // native monitor resolution - MUST match what DXGI actually captures
    // DXGI output index (0-based) for the monitor ResolveCaptureSize() just
    // resolved state_.monitor_id to - passed into hr_pl_create() so the
    // pipeline actually captures that output instead of always output 0.
    int capture_output_idx_ = 0;
    // Output index the *currently-alive* pipeline_ was actually created
    // with, so Start()'s preview-pipeline-reuse check can tell "same size,
    // same monitor" (safe to reuse) apart from "same size, different
    // monitor" (two displays that happen to share a resolution - must
    // recreate, or it'd keep recording the old one).
    int pipeline_output_idx_ = -1;
    int output_w_ = 0, output_h_ = 0;   // final encoded size after Settings > Resolution scaling (0 = same as capture)
    // Window-capture crop rect, monitor-relative pixels; crop_w_==0 means
    // "no crop" (full desktop). Resolved once per ResolveCaptureSize()
    // call from state_.capture_window_title when capture_mode is Window -
    // see the .cpp for the full explanation and hr_pl_set_capture_rect()
    // in hr_pipeline.cpp for how it's actually applied to captured frames.
    int crop_x_ = 0, crop_y_ = 0, crop_w_ = 0, crop_h_ = 0;
    float mic_vol_ = 1.0f, sys_vol_ = 1.0f;
    bool mic_muted_ = false, sys_muted_ = false;
    double current_fps_ = 0.0;

    // Overload detection (see overloaded() above) - drops_delta is checked
    // every PollStats() tick (main_frame's stats_timer_, ~500ms) rather
    // than compared against a wall-clock rate, so the "3 ticks" streak
    // below is roughly 1.5s of sustained drops regardless of exact timer
    // interval.
    long long last_drops_seen_ = 0;
    int overload_streak_ = 0;
    bool overloaded_ = false;
};