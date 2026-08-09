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

    bool recording() const { return state_.recording; }
    bool paused() const { return state_.paused; }
    double elapsed_seconds() const;
    std::wstring elapsed_formatted() const;
    double output_size_mb() const;
    int frame_count() const;

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

    AppState &state_;

    void *pipeline_ = nullptr;   // hr_pl_create() handle
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

    // Last overlay list actually pushed to the pipeline via
    // hr_pl_set_overlays(), so SyncOverlays() (called every preview timer
    // tick, i.e. continuously while the app is open) can skip rebuilding
    // and re-sending an identical list instead of doing that work ~20-60
    // times a second regardless of whether anything changed.
    std::vector<HrOverlayDesc> last_overlays_sent_;
    bool last_overlays_sent_valid_ = false;

    int mic_level_ = 0, sys_level_ = 0;
    int capture_w_ = 0, capture_h_ = 0; // native monitor resolution - MUST match what DXGI actually captures
    int output_w_ = 0, output_h_ = 0;   // final encoded size after Settings > Resolution scaling (0 = same as capture)
    float mic_vol_ = 1.0f, sys_vol_ = 1.0f;
    bool mic_muted_ = false, sys_muted_ = false;
    double current_fps_ = 0.0;
};
