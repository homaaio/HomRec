// recording_controller.h — Phase 3
//
// Port of homrec_app/mixins/recording_mixin.py. Talks directly to the
// existing native pipeline (hr_pl_*, hr_capture_ctl's hr_ctl_*), the ffmpeg
// process runner (hr_ff_*), and the ffmpeg discovery/codec-arg helpers in
// hr_tools.cpp — all already implemented, so this class is glue + the
// same decision logic the Python mixin had (codec fallback, GPU probe,
// filename templating), not new engine code.
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include "app_state.h"

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

    // Start/stop/pause — return false with `error_out` populated on failure
    // (folder missing, ffmpeg missing, pipeline create failed, etc.), same
    // failure surface as `_cmd_start_rec`/`start_recording()` in Python.
    bool Start(std::wstring &error_out);
    void Stop();          // matches "stop_recording()" — merges audio, updates AppState.recording
    void TogglePause();

    // Called on a timer (e.g. every 250-500ms, same cadence as the Python
    // `after(...)` polling loop) to refresh AppState.frame_count and pull
    // stats for the status bar / console.
    void PollStats();

    // Copies the latest preview frame (RGB24) into `out`, sized
    // `out_w`*`out_h`*3. Returns false if no frame is ready yet or capture
    // isn't running. Caller (main_window's WM_PAINT / preview timer) owns
    // the buffer.
    bool GetPreviewFrame(std::vector<uint8_t> &out, int &out_w, int &out_h);

    bool recording() const { return state_.recording; }
    bool paused() const { return state_.paused; }
    double elapsed_seconds() const;
    std::wstring elapsed_formatted() const;
    double output_size_mb() const;
    int frame_count() const;
    int capture_width() const { return capture_w_; }
    int capture_height() const { return capture_h_; }
    double current_fps() const { return current_fps_; }

    // Called by AudioPanel whenever a mic/system volume slider or mute
    // checkbox changes, so Start()/Stop() know what to actually record
    // instead of the previous hardcoded "mic+sys both on, full volume".
    // Mirrors reading self.audio_panel.mic_vol/sys_vol/*_mute in Python's
    // start_recording()/stop_audio_recording().
    void SetAudioLevels(float mic_vol, float sys_vol, bool mic_muted, bool sys_muted) {
        mic_vol_ = mic_vol; sys_vol_ = sys_vol;
        mic_muted_ = mic_muted; sys_muted_ = sys_muted;
    }

private:
    // Builds "HomRec_{date}_{time}"-style filename from
    // AppState.filename_template via hr_filename_from_template, and the
    // full codec argument string via hr_build_codec_args (falls back to a
    // software x264 path if the probed GPU encoder fails to actually start
    // — same fallback behavior recording_mixin.py has).
    std::wstring BuildOutputPath();
    std::wstring BuildCodecArgs(const std::wstring &codec);

    AppState &state_;

    void *pipeline_ = nullptr;   // hr_pl_create() handle
    void *ctl_ = nullptr;        // hr_ctl_create() handle
    void *ffproc_ = nullptr;     // hr_ff_create() handle

    bool ffmpeg_found_ = false;
    std::wstring ffmpeg_path_;
    std::wstring hw_encoder_;    // empty if no GPU encoder available -> software fallback
    std::wstring current_output_path_;

    int mic_level_ = 0, sys_level_ = 0;
    int capture_w_ = 0, capture_h_ = 0; // resolved from hr_di_* in Start(), was hardcoded 0,0 before
    float mic_vol_ = 1.0f, sys_vol_ = 1.0f;
    bool mic_muted_ = false, sys_muted_ = false;
    double current_fps_ = 0.0;
};
