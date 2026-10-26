// recording_controller.h - Port of homrec_app/mixins/recording_mixin.py.
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>
#include "app_state.h"
#include "../core/overlay_render.h"

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
    void Stop();
    void TogglePause();

    // Same work as Stop(), minus the blocking: the slow tail (waiting for ffmpeg to
    // flush/finalize - up to ~30s, see the comment in the .cpp - plus
    // mixing/merging/mp3-exporting audio) runs on a background thread instead of the caller.
    void StopAsync(std::function<void()> on_done);

    // -- Quick codec test (Settings > Video & Codec > "Test Recording") --
    struct QuickTestResult {
        double avg_fps = 0.0;
        long long frames = 0;
        long long drops = 0;
        int width = 0, height = 0;
        std::string encoder_used; // UTF-8, e.g. "libx264" or "h264_nvenc"
        bool overloaded = false; // 3+ consecutive ticks with new drops
    };
    bool StartQuickTest(const std::string &codec, const std::string &preset,
                         int duration_sec, std::wstring &error_out);
    // Returns false once the test's duration has elapsed - the caller
    // should stop polling and call FinishQuickTest() the moment this
    // returns false. A no-op returning false if no test is running (e.g.
    bool PollQuickTest();
    QuickTestResult FinishQuickTest();

private:
    // The actual slow work described in StopAsync()'s comment above - waiting for ffmpeg to
    // finish, mixing/merging/exporting audio, switching pipeline_ out of recording mode (or
    // destroying it), and flipping state_.recording back to false.
    // `recorded_sec` = how long the recording really ran, sampled the moment Stop was requested
    // (BEFORE the slow finalize work below) - used for the A/V merge's real-time check and for the
    // summary duration.
    void StopFinalizeTail(bool keep_for_preview, double recorded_sec);

    // "Hook after recording" (todo2.3.md section 3): runs once StopFinalizeTail() above has a
    // finished output file, per AppState::post_record_hook_type.
    void RunPostRecordHook(const std::wstring &output_path);

    void FinishPipelineAfterStop(bool keep_for_preview);

public:

    // Called on a timer (e.g. every 250-500ms) to refresh AppState.frame_count and pull
    // stats for the status bar / console.
    void PollStats();

    // Copies the latest preview frame (RGB24) into `out`, sized `out_w`*`out_h`*3.
    bool GetPreviewFrame(std::vector<uint8_t> &out, int &out_w, int &out_h);

    // Change-aware variant used by the paint path (PreviewPanel::OnPaint).
    // `seq` is the caller's last-seen sequence number (0 = "never seen
    // one"), updated whenever a new frame is returned. Returns:
    int GetPreviewFrameIfNew(std::vector<uint8_t> &out, int &out_w, int &out_h,
                             int &native_w, int &native_h, uint64_t &seq);

    // Lock-free: the sequence number of the newest preview thumbnail the
    // pipeline has produced (0 if none / no pipeline). The UI timer compares
    // this against what it last painted so it only repaints on a real change.
    uint64_t PreviewSeq() const;

    // The coordinate space OverlayDef::x/y/w/h are measured in for the frame GetPreviewFrame()
    // last returned (i.e.
    bool GetPreviewNativeSize(int &w, int &h);

    // Pushes the current AppState.overlays list into the running/preview pipeline so it
    // actually gets composited into captured frames (both the live preview and, once
    // recording, the encoded output).
    void SyncOverlays();

    // 7th pass: what the window source is doing right now (for the Layers panel's capture-source row):
    // 0 = not recording a window itself, 1 = native Windows.Graphics.Capture, 2 = exact window capture
    // (PrintWindow), 3 = smart mode, currently reading the cheap screen duplication.
    int WindowSourceMode() const;

    // -- Instant Replay -----------------------------------------------
    bool EnableInstantReplay(std::wstring &error_out);
    void DisableInstantReplay();
    bool instant_replay_active() const { return instant_replay_active_; }
    bool instant_replay_enabled() const { return instant_replay_enabled_; }
    bool SaveReplay(std::wstring &error_out, std::wstring *out_saved_path = nullptr);

    void EnsurePreview();
    // Tears down the preview-only pipeline (only if not currently
    // recording - recording owns the pipeline while it's active).
    void TeardownPreview();
    // Called after Settings is saved: re-syncs the preview-only pipeline with whatever changed
    // - on/off (disable_preview), but also monitor or resolution, which an already-running
    // preview pipeline wouldn't otherwise pick up on its own.
    void RefreshPreviewSettings();
    void SetPreviewVisible(bool visible);

    // Tells the pipeline how big the on-screen preview pane is, so it can produce the thumbnail at
    // that size (no UI-side rescale). Cheap; callers throttle it themselves.
    void SetPreviewViewSize(int w, int h);

    // "Apply with preview off" (overlays_dock_panel.cpp's row context menu): grabs one frame
    // for the overlay editor even when the user has Settings > Disable live preview on, by
    // starting the preview pipeline just long enough to capture it.
    bool CaptureSnapshotFrame(std::vector<uint8_t> &out, int &out_w, int &out_h,
                               int &native_w, int &native_h, bool first_call);
    void EndSnapshotEditing();

    bool recording() const { return state_.recording; }
    // Phase 1 (see commands.md): lets homrec.get_setting()/set_setting()
    AppState &state() { return state_; }
    bool paused() const { return state_.paused; }
    double elapsed_seconds() const;
    std::wstring elapsed_formatted() const;
    double output_size_mb() const;
    int frame_count() const;

    // True once PollStats() has seen several consecutive ticks (~1.5s) of real frame drops
    // while actively recording.
    bool overloaded() const { return overloaded_; }

    // True once PollStats() notices the ffmpeg process is gone while
    // state_.recording is still true - it crashed, was closed externally,
    // or hit a fatal encode error, none of which went through Stop().
    bool crashed() const { return crashed_; }
    void AcknowledgeCrash() { crashed_ = false; }
    // True when the crash reported by crashed() was the full-GPU (ddagrab) ffmpeg dying - the UI
    // then restarts the recording at once with the normal pipeline (ddagrab is blocked for the
    // rest of the session). Stays valid after AcknowledgeCrash(); reset by the next Start().
    bool crash_was_ddagrab() const { return crash_was_ddagrab_; }

    // True once the preview-only pipeline has failed to (re)start several times in a row (e.g.
    bool preview_capture_unavailable() const {
        return preview_unavailable_ && !state_.disable_preview && !state_.recording;
    }

    const std::wstring &last_output_path() const { return last_output_path_; }
    std::wstring last_duration_formatted() const;
    double last_output_size_mb() const { return last_output_size_mb_; }
    int capture_width() const { return capture_w_; }
    int capture_height() const { return capture_h_; }
    // The actual resolution the video ends up at (after Settings > Resolution scaling) - what
    // capture_width()/height() report is the raw capture size, which is always native and not
    // what most UI should be showing as "the recording's resolution".
    int output_width() const { return output_w_ > 0 ? output_w_ : capture_w_; }
    int output_height() const { return output_h_ > 0 ? output_h_ : capture_h_; }
    double current_fps() const { return current_fps_; }
    // Capture-loop rate of the (idle) live-preview pipeline; 0 when no pipeline is running.
    double preview_pipeline_fps() const;

    // Called by AudioPanel whenever a mic/system volume slider or mute
    // checkbox changes, so Start()/Stop() know what to actually record
    // instead of the previous hardcoded "mic+sys both on, full volume".
    void SetAudioLevels(float mic_vol, float sys_vol, bool mic_muted, bool sys_muted) {
        mic_vol_ = mic_vol; sys_vol_ = sys_vol;
        mic_muted_ = mic_muted; sys_muted_ = sys_muted;
    }

private:
    std::wstring BuildOutputPath();
    // Resolves the {app} filename-template placeholder: in window-capture
    // mode, the process name (no ".exe") of whatever state_.capture_window_title
    // currently points at, via the same HR_ResolveCaptureWindow() ResolveCaptureSize()
    std::string ResolveCaptureAppName() const;
    // 7th pass: a window target is "defined" when ANY of title / program / class is known (the title alone
    // used to be required, which made a window that was picked once and later renamed unrecoverable).
    bool WindowTargetDefined() const;
    // Short human text for log lines / messages: the title, or the program name when there is no title.
    std::string WindowTargetLabel() const;
    // Resolves the window target through the matching rules (program / class / title, see
    // window_picker_dialog.h); updates state_.capture_window_hwnd and writes what changed to wgc.log.
    bool ResolveWindowTarget(HWND &hwnd, RECT &rect) const;
    // Side-effect-free check that the person's chosen Window/Region capture target can
    // actually be recorded right now.
    bool CheckCaptureTarget(std::wstring &error_out) const;
    // Shared by EnableInstantReplay()/SaveReplay(): (re)starts the background segment-writer
    // ffmpeg process into a fresh run subfolder and points pipeline_'s recording pipe at it.
    bool StartInstantReplayEncoder(std::wstring &error_out);
    void StopInstantReplayEncoder();
    void StopInstantReplayEncoderAsync();
    std::wstring BuildCodecArgs(const std::wstring &codec);
    // In-process hardware encoder (Settings: mf_encoder, see mf_encoder.h). True when THIS recording should go
    // through it instead of an ffmpeg process.
    bool MfEncoderWanted(const std::wstring &codec, bool codec_is_hw) const;
    // Builds + starts the encoder for the current output file and attaches it to pipeline_ (not yet recording).
    bool StartMfEncoder();
    // Resolves capture_w_/capture_h_ from the selected monitor + scale
    // factor (was inline in Start() only; EnsurePreview() needs the same
    // logic to size its preview-only pipeline).
    void ResolveCaptureSize();
    void RetargetWindowCapture();
    // 8th pass (Desktop split): finds the window of a "window" layer again (program / class / title rules,
    // see window_picker_dialog.h) and fills the layer's descriptor for the compositor: where the window is
    // on the monitor now, and whether the window itself or a cut-out of the screen picture is shown.
    void ResolveWindowLayer(OverlayDef &ov, HrOverlayDesc &d);
public:
    // The layer's window rules were edited: search again on the very next tick.
    void ForgetLayerResolve(const std::string &layer_id) { layer_resolve_.erase(layer_id); }
private:
    struct LayerResolveState {
        bool found = false;
        RECT rect{};
        std::chrono::steady_clock::time_point next{};
        std::string logged;     // last "found by" text written to wgc.log (avoid log spam)
    };
    std::unordered_map<std::string, LayerResolveState> layer_resolve_;
    // Applies Settings > General's "Preview quality" (preview_quality_pct)
    void ScaledPreviewSize(int &out_w, int &out_h) const;
    void ResetPreviewRetryState();
    // Settings > Resolution: src_w/src_h (native monitor or cropped window
    // rect) -> desired output size, honoring Percent vs Absolute mode. See
    // the .cpp for the no-upscale/even-dimensions rules.
    void ComputeOutputDims(int src_w, int src_h, int &out_w, int &out_h) const;

    AppState &state_;

    void *pipeline_ = nullptr;   // hr_pl_create() handle

    // -- Instant Replay state -------------------------------------------
    bool   instant_replay_enabled_ = false; // user turned it on - survives being paused for a manual recording
    // True between a CaptureSnapshotFrame() call that had to switch the
    // pipeline's thumbnail generator back on (Disable live preview is on)
    // and the matching EndSnapshotEditing().
    bool   snapshot_forced_preview_ = false;

    // Last visibility reported through SetPreviewVisible() (false = window minimized / hidden
    // in the tray).
    bool   preview_window_visible_ = true;
    // Value to pass to hr_pl_set_preview_needed(): 0 when the live preview is
    // disabled in Settings OR the window can't be seen, 1 otherwise.
    int    PreviewNeededFlag() const { return (state_.disable_preview || !preview_window_visible_) ? 0 : 1; }
    bool   instant_replay_active_  = false; // actually buffering right now (false while state_.recording is true)
    void  *replay_ff_ = nullptr;            // hr_ff_create() handle for the background segment-writer process
    std::wstring replay_dir_;               // current run's segment subfolder (see StartInstantReplayEncoder())
    int    replay_applied_buffer_sec_ = 0;   // buffer length the running Instant Replay encoder was started with
    int    replay_run_seq_ = 0;             // bumped per StartInstantReplayEncoder() call - keeps each run's folder unique
    static constexpr int kReplaySegmentSec = 5; // segment length - Save Replay's trim is accurate to within this many seconds

    // TeardownPreview() hands the actual hr_pl_destroy() off to a background thread (see its
    // own comment for why - avoids freezing the UI while a stuck DXGI capture times out).
    std::thread preview_teardown_thread_;
    std::thread instant_replay_stop_thread_;

    // DXGI Desktop Duplication only allows one active duplication handle per output at a time
    // - hr_pl_create() (via dx_create()) fails and returns null if a previous pipeline's
    // duplication handle hasn't actually been released yet.
    void JoinPendingPreviewTeardown() {
        if (preview_teardown_thread_.joinable()) preview_teardown_thread_.join();
    }

    void JoinPendingInstantReplayStop() {
        if (instant_replay_stop_thread_.joinable()) instant_replay_stop_thread_.join();
    }

    std::thread finalize_thread_;
    std::atomic<bool> finalizing_{false};

    void *ctl_ = nullptr;        // hr_ctl_create() handle
    void *ffproc_ = nullptr;     // hr_ff_create() handle
    // In-process Media Foundation encoder of the CURRENT recording (hr_mf_create() handle) - used INSTEAD of
    // ffproc_ when AppState::mf_encoder applies. ffmpeg is then only run afterwards, to merge the audio.
    void *mfenc_ = nullptr;
    bool mf_blocked_ = false;    // the encoder failed to start / died once: not tried again this session

    // Item 10 - in-process audio. live_audio_rate_ > 0 while THIS recording is set up for it (Hz of the live mix, the
    // encoder was built for that rate); live_audio_ = the mixer is really running and feeds the encoder (so there is no
    // WAV and no merge after Stop). apipe_ = the named-pipe sink when the encoder is ffmpeg (nullptr with the MF encoder).
    int   live_audio_rate_ = 0;
    bool  live_audio_ = false;
    void *apipe_ = nullptr;
    // Starts the live mixer + its sink for the recording that has just been set up; false = use the WAV + merge path.
    bool StartLiveAudio(int head_ms);
    // Stop, step 1 (BEFORE the video side is ended): the live mixer hands its last audio to the sink and the sink is told
    // there is no more. Does not block. No-op if this recording is not live.
    void StopLiveAudio();
    // Stop, step 2 (after the video segment has been closed): waits until the ffmpeg audio pipe has delivered everything
    // and closes it. No-op without a pipe sink.
    void FinishAudioSink();

    // ffmpeg "ddagrab" fast path (Settings > Video > GPU acceleration): true while
    // the CURRENT recording is captured + encoded entirely by ffmpeg on the GPU
    // (no HomRec pipeline attached, so no overlays/pause). See TryStartDdagrab().
    bool ddagrab_active_ = false;
    bool ddagrab_pause_warned_ = false;
    bool ddagrab_blocked_ = false;        // ddagrab died mid-recording once: not used again this session
    bool crash_was_ddagrab_ = false;      // see crash_was_ddagrab()
    bool gpu_self_lowered_ = false;       // HomRec's own GPU scheduling class is lowered (AppState::gpu_low_priority)
    double crash_video_elapsed_sec_ = 0.0; // recording time at the moment ffmpeg died (0 = no crash)
    long long ddagrab_last_frames_ = -1;   // -progress frame count at the last FPS sample
    std::chrono::steady_clock::time_point ddagrab_last_frames_t_{};
    int  ddagrab_probe_ = 0;     // 0 = not probed yet, 1 = ffmpeg has ddagrab, 2 = it doesn't
    bool TryStartDdagrab(const std::wstring &codec_args, bool codec_is_hw,
                         std::chrono::steady_clock::time_point &t_video_go);

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
    double av_start_skew_sec_ = 0.0;

    std::string applied_mic_device_id_;

    // Snapshot of capture-affecting settings last used to (re)build the
    // Preview pipeline - see RefreshPreviewSettings()'s comment.
    // Anything NOT in this list (theme, hotkeys, output folder, etc.)
    struct PreviewCaptureSettings {
        bool disable_preview = false;
        int monitor_id = -1;
        CaptureMode capture_mode = CaptureMode::Desktop;
        std::string capture_window_title;
        HWND capture_window_hwnd = nullptr;
        // 7th pass (capture-source dialog): how the window is recorded and found again. A change of any of
        // these rebuilds the preview pipeline so the new method / area / smart mode takes effect.
        std::string window_capture_method, window_capture_area, window_title_mode;
        bool window_match_exe = true, window_match_class = true, window_match_title = false;
        bool window_follow_hwnd = true, window_capture_smart = true;
        int window_capture_fps_cap = 0;
        int region_x = 0, region_y = 0, region_w = 0, region_h = 0;
        int target_fps = -1;
        int preview_width = -1, preview_height = -1;
        int preview_quality_pct = -1;
        bool operator==(const PreviewCaptureSettings &o) const {
            return disable_preview == o.disable_preview && monitor_id == o.monitor_id &&
                   capture_mode == o.capture_mode && capture_window_title == o.capture_window_title &&
                   capture_window_hwnd == o.capture_window_hwnd &&
                   window_capture_method == o.window_capture_method &&
                   window_capture_area == o.window_capture_area && window_title_mode == o.window_title_mode &&
                   window_match_exe == o.window_match_exe && window_match_class == o.window_match_class &&
                   window_match_title == o.window_match_title && window_follow_hwnd == o.window_follow_hwnd &&
                   window_capture_smart == o.window_capture_smart &&
                   window_capture_fps_cap == o.window_capture_fps_cap &&
                   region_x == o.region_x && region_y == o.region_y &&
                   region_w == o.region_w && region_h == o.region_h &&
                   target_fps == o.target_fps && preview_width == o.preview_width &&
                   preview_height == o.preview_height && preview_quality_pct == o.preview_quality_pct;
        }
    };
    PreviewCaptureSettings applied_preview_capture_settings_;
    bool applied_preview_capture_settings_valid_ = false;

    // Last overlay list actually pushed to the pipeline via hr_pl_set_overlays(), so
    // SyncOverlays() (called every preview timer tick, i.e.
    std::vector<HrOverlayDesc> last_overlays_sent_;
    // Reused scratch for SyncOverlays()'s per-tick rebuild (see there) so the
    // steady state does no heap allocation.
    std::vector<HrOverlayDesc> overlays_scratch_;
    bool last_overlays_sent_valid_ = false;

    std::chrono::steady_clock::time_point next_preview_retry_{};
    // Consecutive EnsurePreview() failures since the last success (or since the last time
    // preview was actually needed) - drives the backoff below.
    int preview_retry_streak_ = 0;
    // Set once preview_retry_streak_ crosses kPreviewRetryBackoffAfter,
    // cleared again on the next successful EnsurePreview(). See
    // preview_capture_unavailable() above.
    bool preview_unavailable_ = false;
    static constexpr int kPreviewRetryBaseSeconds = 2;
    static constexpr int kPreviewRetryMaxSeconds = 30;
    static constexpr int kPreviewRetryBackoffAfter = 5;
    int mic_level_ = 0, sys_level_ = 0;

    std::chrono::steady_clock::time_point last_loud_time_ = std::chrono::steady_clock::now();
    bool auto_paused_by_silence_ = false;
    int capture_w_ = 0, capture_h_ = 0; // native monitor resolution - MUST match what DXGI actually captures
    // DXGI output index (0-based) for the monitor ResolveCaptureSize() just
    // resolved state_.monitor_id to - passed into hr_pl_create() so the
    // pipeline actually captures that output instead of always output 0.
    int capture_output_idx_ = 0;
    int pipeline_output_idx_ = -1;
    int output_w_ = 0, output_h_ = 0;   // final encoded size after Settings > Resolution scaling (0 = same as capture)
    // Window-capture crop rect, monitor-relative pixels; crop_w_==0 means "no crop" (full
    // desktop).
    int crop_x_ = 0, crop_y_ = 0, crop_w_ = 0, crop_h_ = 0;
    // 2.4: Window mode records the window itself via Windows.Graphics.Capture instead of
    // cropping a monitor duplication. Set by ResolveCaptureSize(); crop_*_ stay 0 then.
    bool use_wgc_ = false;
    HWND wgc_hwnd_ = nullptr;
    // Set when Windows.Graphics.Capture failed to start for a window: stay on the screen-crop path
    // for the rest of this session (or until the user re-selects the method in Settings).
    bool wgc_disabled_ = false;
    std::string wgc_method_seen_;         // window_capture_method last seen - used to clear wgc_disabled_
    int pv_view_w_ = 0, pv_view_h_ = 0;   // preview pane size last reported by the UI (SetPreviewViewSize)
    bool pipeline_wgc_ = false;           // what the CURRENT pipeline_ was created as
    HWND pipeline_wgc_hwnd_ = nullptr;
    bool PipelineMatchesTarget() const { return pipeline_wgc_ == use_wgc_ && (!use_wgc_ || pipeline_wgc_hwnd_ == wgc_hwnd_); }
    void *CreatePipelineForTarget(intptr_t pipe_fd, int pvw, int pvh);
    bool window_track_lost_warned_ = false;
    std::chrono::steady_clock::time_point next_window_retarget_{}; // throttles re-resolving a lost window
    mutable std::string window_route_logged_;   // last "found by" text written to wgc.log (avoid log spam)
    bool window_follow_failed_warned_ = false;  // native WGC cannot follow a new window handle: say so once
    float mic_vol_ = 1.0f, sys_vol_ = 1.0f;
    bool mic_muted_ = false, sys_muted_ = false;
    double current_fps_ = 0.0;

    long long last_drops_seen_ = 0;
    int overload_streak_ = 0;
    bool overloaded_ = false;
    bool crashed_ = false; // see crashed()/AcknowledgeCrash() above

    // -- Quick codec test state (see StartQuickTest() above) ------------ Entirely separate
    // from pipeline_/ffproc_ (the real recording/ preview pipeline) so a test can never be
    // mistaken for, or collide with, an actual recording.
    void *qt_pipeline_ = nullptr;
    void *qt_ffproc_ = nullptr;
    std::wstring qt_output_path_;
    bool qt_running_ = false;
    std::chrono::steady_clock::time_point qt_end_time_{};
    double qt_fps_sum_ = 0.0;
    int qt_fps_samples_ = 0;
    long long qt_last_drops_ = 0;
    int qt_overload_streak_ = 0;
    bool qt_overloaded_ = false;
    long long qt_frames_ = 0, qt_drops_ = 0;
    int qt_w_ = 0, qt_h_ = 0;
    std::string qt_encoder_used_;
    // Preview was actually torn down by StartQuickTest() and should be restored by
    // FinishQuickTest() - false if preview was already off (state_.disable_preview) or wasn't
    // running, so Finish doesn't start it up when it wasn't the test's place to.
    bool qt_preview_was_torn_down_ = false;
};
