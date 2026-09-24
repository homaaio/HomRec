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
#include <atomic>
#include <functional>
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
    void Stop();          // matches "stop_recording()" - merges audio, updates AppState.recording; blocks until fully done, see StopAsync()
    void TogglePause();

    // Same work as Stop(), minus the blocking: the slow tail (waiting for
    // ffmpeg to flush/finalize - up to ~30s, see the comment in the .cpp -
    // plus mixing/merging/mp3-exporting audio) runs on a background thread
    // instead of the caller. AppState.recording only flips to false once
    // that thread finishes (so Start() called too soon still correctly
    // sees "already recording" instead of racing pipeline_/ffproc_ teardown
    // against a still-in-flight finalize); on_done is invoked from that
    // background thread once everything's done, so a caller that touches
    // UI from it must marshal back to the UI thread itself (e.g.
    // wxEvtHandler::CallAfter). A no-op (on_done not called) if a
    // finalize from a previous StopAsync()/Stop() is already in flight or
    // nothing is recording.
    void StopAsync(std::function<void()> on_done);

    // -- Quick codec test (Settings > Video & Codec > "Test Recording") --
    //
    // A short, disposable recording of the currently selected monitor,
    // using whatever codec/preset the Settings dialog currently has
    // picked (which may not be Saved yet) - exists purely to answer
    // "does this codec actually work on this machine, and what fps does
    // it get" without committing to a real recording or overwriting
    // anything. Uses its own local pipeline/ffmpeg process (never
    // pipeline_/ffproc_, and never touches state_.recording) so it can't
    // be confused with an actual recording by the rest of the app - it's
    // refused outright if one is already in progress.
    //
    // The live preview pipeline (if running) is torn down for the
    // duration, same as Start() would take it over for a real recording -
    // DXGI Desktop Duplication only allows one active duplication handle
    // per monitor, so the test and the preview can't run at once.
    // FinishQuickTest() brings preview back automatically.
    //
    // StartQuickTest() kicks it off; the caller (a wxTimer on whatever UI
    // owns this) should call PollQuickTest() every ~200-300ms. Once it
    // returns false (the duration elapsed), call FinishQuickTest() exactly
    // once to get the result, tear everything down, and restore preview.
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
    // StartQuickTest() failed) so a caller that always polls once after
    // Start regardless of its return value can't spin forever.
    bool PollQuickTest();
    QuickTestResult FinishQuickTest();

private:
    // The actual slow work described in StopAsync()'s comment above -
    // waiting for ffmpeg to finish, mixing/merging/exporting audio,
    // switching pipeline_ out of recording mode (or destroying it), and
    // flipping state_.recording back to false. Runs on finalize_thread_;
    // split out to its own method only so StopAsync() itself stays short
    // and the thread-lambda doesn't have to duplicate this. Not meant to
    // be called from anywhere but that lambda.
    void StopFinalizeTail(bool keep_for_preview);
    void RunPostRecordHook(const std::wstring &output_path);

    void FinishPipelineAfterStop(bool keep_for_preview);

public:

    // Called on a timer (e.g. every 250-500ms) to refresh AppState.frame_count and pull
    // stats for the status bar / console.
    void PollStats();

    // Copies the latest preview frame (RGB24) into `out`, sized
    // `out_w`*`out_h`*3. Returns false if no frame is ready yet or capture
    // isn't running. Caller (main_window's WM_PAINT / preview timer) owns
    // the buffer.
    bool GetPreviewFrame(std::vector<uint8_t> &out, int &out_w, int &out_h);

    // Change-aware variant used by the paint path (PreviewPanel::OnPaint).
    // `seq` is the caller's last-seen sequence number (0 = "never seen
    // one"), updated whenever a new frame is returned. Returns:
    //   0 - no frame available (no pipeline / nothing captured yet)
    //   1 - a NEW frame was copied into `out` (out_w/out_h/native_* set;
    //       `out` holds at least out_w*out_h*3 valid bytes - it is only ever
    //       grown, never shrunk, so don't use out.size() as the frame size)
    //   2 - unchanged since `seq`: nothing was copied and `out` is untouched
    // Also bounds-checked against `out`'s real size (see hr_pl_get_preview_ex
    // in hr_pipeline.cpp for the heap-overflow this closes).
    int GetPreviewFrameIfNew(std::vector<uint8_t> &out, int &out_w, int &out_h,
                             int &native_w, int &native_h, uint64_t &seq);

    // Lock-free: the sequence number of the newest preview thumbnail the
    // pipeline has produced (0 if none / no pipeline). The UI timer compares
    // this against what it last painted so it only repaints on a real change.
    uint64_t PreviewSeq() const;

    // The coordinate space OverlayDef::x/y/w/h are measured in for the
    // frame GetPreviewFrame() last returned (i.e. the size overlays are
    // actually composited at - the cropped window rect in window-capture
    // mode, the full monitor otherwise). The on-screen preview MUST map
    // overlay rectangles through this and not capture_width()/height():
    // those are always the full monitor's size, which is wrong the moment
    // a window crop is active. Falls back to capture_width()/height() if
    // the pipeline hasn't produced a frame yet. False if neither is known.
    bool GetPreviewNativeSize(int &w, int &h);

    // Pushes the current AppState.overlays list into the running/preview
    // pipeline so it actually gets composited into captured frames (both
    // the live preview and, once recording, the encoded output). Cheap
    // enough to call on every UI tick (main_frame's preview timer does) --
    // just copies a handful of small structs unless overlays are empty.
    void SyncOverlays();

    // -- Instant Replay -----------------------------------------------
    //
    // A continuous background recording into a short rolling buffer,
    // independent of the manual Start()/Stop() above - "Save Replay"
    // writes out whatever's currently in the last AppState.replay_buffer_sec
    // seconds without you having had to be recording already.
    //
    // Shares this same class's single capture pipeline_ rather than
    // opening a second one (hr_pl_create() -> dx_create() only supports
    // one DXGI duplication handle per monitor at a time - a second
    // pipeline for the same monitor would just fail to create), and
    // hr_pl_set_recording() only ever has one active pipe_fd consumer -
    // so Instant Replay and a manual recording can't literally run at
    // the same time. Starting a manual recording while Instant Replay is
    // on pauses it for the duration (Start() takes the pipe over); Stop()
    // resumes it afterward with a fresh buffer.
    //
    // Enables the background buffer now (if not already recording
    // manually - if it's currently within Start()/Stop() it just
    // remembers to resume when Stop() runs).
    bool EnableInstantReplay(std::wstring &error_out);
    // Turns Instant Replay off entirely (not just pausing it) - the
    // background buffer stops and its temp segment files are abandoned
    // (cleaned up lazily: EnableInstantReplay() always starts into a
    // fresh subfolder, so nothing accumulates across sessions except
    // whatever's in the *current* run, which the OS temp folder isn't
    // expected to keep forever anyway).
    void DisableInstantReplay();
    bool instant_replay_active() const { return instant_replay_active_; }
    bool instant_replay_enabled() const { return instant_replay_enabled_; }
    // Saves the buffer's current contents to a real file (via the same
    // filename template - {date}/{time}/{app} - as a manual recording),
    // trimmed to approximately AppState.replay_buffer_sec (accurate to
    // within kReplaySegmentSec, the segment granularity). Restarts the
    // background segment writer immediately afterward so the next Save
    // Replay doesn't re-save clips that were already saved - that
    // restart briefly gaps the *background* buffer (not the clip just
    // saved), logged rather than surfaced as this call's own failure.
    bool SaveReplay(std::wstring &error_out, std::wstring *out_saved_path = nullptr);

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
    //
    // out_w/out_h are the returned thumbnail's own pixel size (what the
    // dialog should draw the screenshot at). native_w/native_h are the
    // *full-resolution* capture size that OverlayDef::x/y/w/h are actually
    // measured in (same space the real recording composites overlays
    // into) - out_w/out_h is almost always smaller than this, so any
    // on-screen drag math must convert through native_w/native_h, not
    // out_w/out_h, or every placement ends up scaled down to match the
    // thumbnail instead of the recording.
    bool CaptureSnapshotFrame(std::vector<uint8_t> &out, int &out_w, int &out_h,
                               int &native_w, int &native_h, bool first_call);
    // Ends an "Apply with preview off" editing session - tears the
    // preview pipeline back down if Settings > Disable live preview is
    // still on (CaptureSnapshotFrame() only started it for the snapshot,
    // it shouldn't keep running afterward), leaves it alone otherwise.
    void EndSnapshotEditing();

    bool recording() const { return state_.recording; }
    // Phase 1 (see commands.md): lets homrec.get_setting()/set_setting()
    // (src/plugins/lua_api.cpp) read/write real AppState fields directly
    // instead of the now-disconnected homrec_settings.json copy they used
    // to round-trip through - see that file's L_settings_get/set for why.
    AppState &state() { return state_; }
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

    // True once PollStats() notices the ffmpeg process is gone while
    // state_.recording is still true - it crashed, was closed externally,
    // or hit a fatal encode error, none of which went through Stop().
    // Stays true until the caller (main_frame's OnStatsTimer) reacts to
    // it and calls AcknowledgeCrash(), so it can't fire twice for the
    // same crash while the resulting StopAsync() finalize is in flight.
    bool crashed() const { return crashed_; }
    void AcknowledgeCrash() { crashed_ = false; }

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
    // Resolves the {app} filename-template placeholder: in window-capture
    // mode, the process name (no ".exe") of whatever state_.capture_window_title
    // currently points at, via the same HR_ResolveCaptureWindow() ResolveCaptureSize()
    // uses; "Desktop" in desktop-capture mode, or if the window can't be
    // resolved (closed since the profile was saved, etc.) - {app} should
    // always expand to *something* filename-safe rather than leaving a
    // literal "{app}" in the output path.
    std::string ResolveCaptureAppName() const;
    // Side-effect-free check that the person's chosen Window/Region capture
    // target can actually be recorded right now. Returns false (with a
    // user-facing reason in error_out) if a picked window is gone/minimized
    // or the picked region is entirely off every monitor. Start() and
    // EnableInstantReplay() use it to refuse to start rather than silently
    // recording the WHOLE desktop when the person asked for one window/region.
    bool CheckCaptureTarget(std::wstring &error_out) const;
    // Shared by EnableInstantReplay()/SaveReplay(): (re)starts the
    // background segment-writer ffmpeg process into a fresh run
    // subfolder and points pipeline_'s recording pipe at it. Assumes
    // pipeline_ already exists and is sized/cropped correctly.
    bool StartInstantReplayEncoder(std::wstring &error_out);
    // Stops+destroys replay_ff_ (blocking briefly so its last segment
    // finalizes) without touching pipeline_ - callers decide what the
    // pipeline should do next (redirect to a manual recording, go back
    // to preview-only, or immediately restart a fresh replay encoder).
    void StopInstantReplayEncoder();
    void StopInstantReplayEncoderAsync();
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
    // Clears the preview retry/backoff state (preview_retry_streak_,
    // preview_unavailable_, next_preview_retry_) so the very next
    // EnsurePreview() call gets a fresh attempt at the fast base cadence
    // instead of wherever an earlier failure streak had already escalated
    // backoff to. Shared by RefreshPreviewSettings() (a settings-driven
    // re-enable) and SetPreviewVisible(true) (a visibility-driven one,
    // e.g. restoring from the tray) - see SetPreviewVisible()'s comment
    // for why the latter needed this too.
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
    bool   instant_replay_active_  = false; // actually buffering right now (false while state_.recording is true)
    void  *replay_ff_ = nullptr;            // hr_ff_create() handle for the background segment-writer process
    std::wstring replay_dir_;               // current run's segment subfolder (see StartInstantReplayEncoder())
    int    replay_run_seq_ = 0;             // bumped per StartInstantReplayEncoder() call - keeps each run's folder unique
    static constexpr int kReplaySegmentSec = 5; // segment length - Save Replay's trim is accurate to within this many seconds

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
    std::thread instant_replay_stop_thread_;

    // DXGI Desktop Duplication only allows one active duplication
    // handle per output at a time - hr_pl_create() (via dx_create()) fails
    // and returns null if a previous pipeline's duplication handle hasn't
    // actually been released yet. TeardownPreview() destroys the old
    // pipeline_ on preview_teardown_thread_ in the background (see its
    // comment above) precisely so a stuck capture doesn't freeze the UI -
    // but that means a *new* pipeline created shortly after (EnsurePreview()
    // for "Position Overlays..." with preview off, or Start() reusing/
    // replacing the preview pipeline) could race the still-in-flight
    // teardown and hit exactly that "only one duplication handle" limit:
    // dx_create() returns null, CaptureSnapshotFrame() reports "no
    // screenshot", or Start() fails with "Failed to start the capture
    // pipeline" - both looking like they "only work after a restart"
    // because a fresh process obviously has no pending teardown to race.
    // Call this immediately before any hr_pl_create() so the previous
    // pipeline's DXGI handle is guaranteed to be gone first. A no-op
    // (joinable() is false) in the by-far-most-common case where there's
    // nothing pending.
    void JoinPendingPreviewTeardown() {
        if (preview_teardown_thread_.joinable()) preview_teardown_thread_.join();
    }

    void JoinPendingInstantReplayStop() {
        if (instant_replay_stop_thread_.joinable()) instant_replay_stop_thread_.join();
    }

    // StopAsync()'s background tail (see its .cpp comment) and its
    // re-entrancy guard - same tracked-thread-plus-join pattern as
    // preview_teardown_thread_ above, for the same reason: an untracked/
    // detached finalize thread still running when the app closes would be
    // touching the logger, ffmpeg process handle, etc. out from under the
    // CRT/DLL shutdown sequence. finalizing_ is checked (not just
    // state_.recording) so a second StopAsync()/Stop() call arriving while
    // one is already finishing (e.g. the user manages to click Stop twice,
    // or OnClose() runs while a Stop() from the UI is still finalizing)
    // doesn't run the whole tail a second time over the same ffproc_/
    // pipeline_.
    std::thread finalize_thread_;
    std::atomic<bool> finalizing_{false};

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
    // Measured gap between the video pipeline's and the audio buffer's
    // real-world "frame/sample 0" moments this Start() (positive: audio's
    // zero-point is later than video's; negative: earlier) - see the long
    // comment on its computation in Start() and its use in Stop()'s
    // hr_merge_av() call. 0.0 means "not measured this session" (e.g. no
    // audio channels enabled), which hr_merge_av() treats as "don't touch
    // AV alignment", matching its prior behavior.
    double av_start_skew_sec_ = 0.0;

    // Mic device id actually applied to the currently-running continuous
    // audio capture (see Init()'s hr_audio_start() and
    // RefreshPreviewSettings() below) -- compared against state_.mic_device_id
    // so a Settings change to the microphone picker only restarts capture
    // when it actually changed, not on every settings-dialog close.
    std::string applied_mic_device_id_;

    // Snapshot of capture-affecting settings last used to (re)build the
    // Preview pipeline - see RefreshPreviewSettings()'s comment.
    // Anything NOT in this list (theme, hotkeys, output folder, etc.)
    // closing Settings should never touch the pipeline for.
    struct PreviewCaptureSettings {
        bool disable_preview = false;
        int monitor_id = -1;
        CaptureMode capture_mode = CaptureMode::Desktop;
        std::string capture_window_title;
        // The picked window's HWND and the region rect drive what the
        // pipeline crops to, so changing either has to rebuild/re-crop the
        // preview pipeline too - they used to be missing from this
        // comparison, leaving the live preview on the full desktop after
        // File > Select Window/Region until something unrelated changed.
        HWND capture_window_hwnd = nullptr;
        int region_x = 0, region_y = 0, region_w = 0, region_h = 0;
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
                   capture_window_hwnd == o.capture_window_hwnd &&
                   region_x == o.region_x && region_y == o.region_y &&
                   region_w == o.region_w && region_h == o.region_h &&
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
    // Reused scratch for SyncOverlays()'s per-tick rebuild (see there) so the
    // steady state does no heap allocation.
    std::vector<HrOverlayDesc> overlays_scratch_;
    bool last_overlays_sent_valid_ = false;

    std::chrono::steady_clock::time_point next_preview_retry_{};
    // Consecutive EnsurePreview() failures since the last success (or
    // since the last time preview was actually needed) - drives the
    // backoff below. Reset to 0 the moment a preview pipeline actually
    // starts.
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

    // -- Auto-pause/resume on microphone silence (todo2.3.md section 3) --
    // last_loud_time_ is reset to "now" every PollStats() tick where the
    // mic is at/above silence_threshold_db; once it's been longer than
    // silence_duration_sec since that last happened, PollStats() pauses
    // the recording (auto_paused_by_silence_ = true, so the *next* loud
    // tick knows to resume it - a manual pause the user pressed
    // themselves is left alone, see PollStats()'s comment). Reset
    // (unset) on every fresh Start().
    std::chrono::steady_clock::time_point last_loud_time_ = std::chrono::steady_clock::now();
    bool auto_paused_by_silence_ = false;
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
    bool crashed_ = false; // see crashed()/AcknowledgeCrash() above

    // -- Quick codec test state (see StartQuickTest() above) ------------
    // Entirely separate from pipeline_/ffproc_ (the real recording/
    // preview pipeline) so a test can never be mistaken for, or collide
    // with, an actual recording.
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
    // Preview was actually torn down by StartQuickTest() and should be
    // restored by FinishQuickTest() - false if preview was already off
    // (state_.disable_preview) or wasn't running, so Finish doesn't
    // start it up when it wasn't the test's place to.
    bool qt_preview_was_torn_down_ = false;
};