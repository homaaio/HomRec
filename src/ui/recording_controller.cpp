#include "recording_controller.h"
#include "window_picker_dialog.h"  // HR_ResolveCaptureWindow()
#include "../hr_log.h"
#include "../hr_overlay_render.h"
#include <windows.h>  // Sleep() - CaptureSnapshotFrame()'s short wait for the first frame; also
                      // QueryFullProcessImageNameA (kernel32) - ResolveCaptureAppName()'s {app} lookup
#include <vector>
#include <thread>
#include <exception>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <fstream>

extern "C" {
    // hr_tools.cpp (wide-string API)
    int hr_check_ffmpeg(const wchar_t *hint, wchar_t *out, int out_len);
    int hr_probe_gpu(const wchar_t *ffpath, wchar_t *out_enc, int out_len);
    int hr_build_codec_args(const wchar_t *codec, int quality, int fps, int cpu_count,
                             wchar_t *out_buf, int buf_chars, const wchar_t *preset_override);
    int hr_merge_av(const wchar_t *ffpath, const wchar_t *video_file, const wchar_t *audio_file,
                     double real_elapsed_sec);
    int hr_export_mp3(const wchar_t *ffpath, const wchar_t *wav_path, const wchar_t *mp3_path);
    int hr_concat_segments(const wchar_t *ffpath, const wchar_t *list_path, const wchar_t *out_path);

    // hr_ui_utils.cpp (narrow-string API - see README audit note: the core
    // is split between wide- and narrow-string exports depending on which
    // file it landed in; this class just calls each the way it expects).
    void hr_filename_from_template(const char *tmpl, const char *folder, const char *app_name, char *out, int out_len);
    int hr_make_output_dir(const char *path);
    int hr_path_exists(const char *path);
    float hr_file_size_mb(const char *path);
    int hr_get_free_disk_mb(const char *path, uint64_t *out_free_mb);

    // hr_display_info.cpp
    void *hr_di_create();
    void hr_di_destroy(void *handle);
    void hr_di_refresh(void *handle);
    int hr_di_count(void *handle);
    int hr_di_get(void *handle, int index, int *x, int *y, int *w, int *h, float *dpi);
    int hr_di_primary(void *handle, int *x, int *y, int *w, int *h, float *dpi);

    // hr_capture_ctl.cpp
    void *hr_ctl_create();
    void hr_ctl_destroy(void *handle);
    void hr_ctl_set_callbacks(void *handle, void (*state_cb)(int), void (*stats_cb)(double, double, int));
    void hr_ctl_set_output_path(void *handle, const char *path);
    int hr_ctl_start(void *handle);
    double hr_ctl_stop(void *handle);
    int hr_ctl_pause_toggle(void *handle);
    int hr_ctl_get_state(const void *handle);
    double hr_ctl_get_elapsed_sec(const void *handle);
    int hr_ctl_get_frame_count(const void *handle);
    void hr_ctl_update_stats(void *handle, long long file_bytes);
    int hr_ctl_format_elapsed(const void *handle, char *buf, int buf_len);

    // hr_pipeline.cpp
    void *hr_pl_create(int w, int h, int fps, intptr_t pipe_fd, int pv_w, int pv_h, int output_idx);
    void hr_pl_destroy(void *handle);
    int hr_pl_start(void *handle);
    void hr_pl_stop(void *handle);
    void hr_pl_pause(void *handle, int flag);
    int hr_pl_get_preview(void *handle, unsigned char *out_rgb, int *out_w, int *out_h);
    int hr_pl_get_native_size(void *handle, int *out_w, int *out_h);
    void hr_pl_set_recording(void *handle, int active, intptr_t pipe_fd);
    void hr_pl_set_priority_boost(void *handle, int flag);
    void hr_pl_stats(void *handle, long long *out_frames, long long *out_drops, double *out_fps);
    void hr_pl_set_overlays(void *handle, const HrOverlayDesc *items, int count);
    void hr_pl_set_include_cursor(void *handle, int flag);
    void hr_pl_set_capture_rect(void *handle, int x, int y, int w, int h);
    void hr_pl_set_output_size(void *handle, int w, int h);
    void hr_pl_set_preview_fps(void *handle, int fps);
    void hr_pl_set_preview_needed(void *handle, int enabled);
    int hr_pl_end_recording_segment(void *handle, int timeout_ms);

    // hr_ffmpeg_runner.cpp
    void *hr_ff_create();
    void hr_ff_destroy(void *handle);
    void hr_ff_set_ffmpeg_path(void *h, const char *path);
    void hr_ff_set_output_path(void *h, const char *path);
    void hr_ff_set_codec_args(void *h, const char *args);
    void hr_ff_set_extra_output_args(void *h, const char *args);
    void hr_ff_set_video_params(void *h, int w, int h2, int fps);
    void hr_ff_set_output_size(void *h, int out_w, int out_h);
    void hr_ff_set_pipe_input(void *h, int enable);
    int hr_ff_start(void *handle);
    intptr_t hr_ff_get_stdin_handle(void *handle);
    int hr_ff_stop_graceful(void *handle);
    int hr_ff_wait(void *handle, int timeout_ms);
    int hr_ff_is_running(const void *handle);
    double hr_ff_output_size_mb(const void *handle);
    void hr_ff_kill(void *handle);

    // hr_audio.cpp
    int hr_audio_init();
    int hr_audio_start(float mic_vol, float sys_vol, int mic_mute, int sys_mute, const wchar_t *mic_device_id);
    void hr_audio_set_max_buffer_sec(int seconds);
    int hr_audio_snapshot_to_wav(const char *mic_wav_path, const char *sys_wav_path);
    int hr_audio_stop(const char *mic_wav_path, const char *sys_wav_path);
    void hr_audio_set_volumes(float mic_vol, float sys_vol, int mic_mute, int sys_mute);
    void hr_audio_get_levels(int *out_mic, int *out_sys);
    void hr_audio_pause(int paused);
    void hr_audio_reset_buffers();
    int hr_audio_capture_to_wav(const char *mic_wav_path, const char *sys_wav_path);
    int hr_audio_stop(const char *mic_wav_path, const char *sys_wav_path);
    int hr_audio_mix_wav(const char *mic_path, const char *sys_path, const char *out_path);
}

#include <cstdio> // remove()/rename() for temp WAV cleanup in Stop()

namespace {
// Below this, Start() refuses to begin a new recording rather than let it
// run for a few seconds and then die mid-file when ffmpeg can't write any
// more - 200MB is comfortably more than a couple of encoded frames need
// but small enough not to nag on a nearly-full-but-still-usable drive.
constexpr uint64_t kMinFreeDiskMb = 200;

std::string NarrowFromWide(const std::wstring &w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
std::wstring WideFromNarrow(const std::string &s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}
}

RecordingController::RecordingController(AppState &state) : state_(state) {
    ctl_ = hr_ctl_create();
}

RecordingController::~RecordingController() {
    if (state_.recording) Stop();
    if (finalize_thread_.joinable()) finalize_thread_.join();
    if (instant_replay_active_) StopInstantReplayEncoder();
    JoinPendingInstantReplayStop();
    JoinPendingPreviewTeardown();
    if (ctl_) hr_ctl_destroy(ctl_);
    if (ffproc_) hr_ff_destroy(ffproc_);
    if (pipeline_) hr_pl_destroy(pipeline_);
    hr_audio_stop(nullptr, nullptr);
}

void RecordingController::Initialize() {
    wchar_t path_buf[MAX_PATH] = {};
    ffmpeg_found_ = hr_check_ffmpeg(state_.ffmpeg_path.empty() ? nullptr : WideFromNarrow(state_.ffmpeg_path).c_str(),
                                    path_buf, MAX_PATH) != 0;
    if (ffmpeg_found_) {
        ffmpeg_path_ = path_buf;
        state_.ffmpeg_path = NarrowFromWide(ffmpeg_path_);

        wchar_t enc_buf[64] = {};
        if (state_.hw_accel == "auto") {
            if (hr_probe_gpu(ffmpeg_path_.c_str(), enc_buf, 64)) {
                hw_encoder_ = enc_buf;
                HrLog::Info("Startup: GPU encoder available (" + NarrowFromWide(hw_encoder_) +
                            ") -- recordings will use it instead of software libx264.");
            } else {
                HrLog::Info("Startup: no working GPU encoder found (nvenc/qsv/amf all failed "
                            "to probe) -- recordings will fall back to software libx264, which "
                            "is noticeably heavier on CPU. If you have a GPU that should support "
                            "one of those, check its drivers and that this ffmpeg build was "
                            "compiled with that encoder.");
            }
        }
    }
    hr_audio_init();
    // Capture runs continuously from app startup so the mixer's level
    // meters actually move before you hit Start - previously this only
    // happened inside Start(), so the meters (and the sliders that look
    // like they "don't respond") sat dead until a recording was already
    // running. hr_audio_reset_buffers()/hr_audio_capture_to_wav() scope
    // what actually ends up in a given recording's WAV.
    hr_audio_start(1.0f, 1.0f, 0, 0, WideFromNarrow(state_.mic_device_id).c_str());
    applied_mic_device_id_ = state_.mic_device_id;
}

std::wstring RecordingController::BuildCodecArgs(const std::wstring &codec) {
    // state_.custom_ffmpeg_args (the "Custom FFmpeg args" box on
    // the Video/Codec settings tab) was saved/loaded like every other
    // setting but never actually consulted here - recording always used
    // the auto-generated args below regardless of what a power user typed
    // in that box. It's meant as a full escape hatch, so when it's set,
    // use it verbatim instead of the auto-built preset/crf/threads args
    // (still prefixed with the chosen codec, since that's a separate
    // dropdown from this one).
    if (!state_.custom_ffmpeg_args.empty()) {
        return L"-c:v " + codec + L" " + WideFromNarrow(state_.custom_ffmpeg_args);
    }

    wchar_t buf[512] = {};
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    // state_.enc_preset (the "Encoder preset" dropdown, same tab)
    // had the same problem - saved, reloaded, never read. Passed through
    // now; hr_build_codec_args() only actually uses it for the software
    // libx264 path (the hardware encoder branches use their own fixed
    // low-latency presets, which aren't meaningfully tunable via x264-style
    // preset names like "medium"/"veryslow").
    std::wstring preset = WideFromNarrow(state_.enc_preset);
    hr_build_codec_args(codec.c_str(), state_.quality, state_.target_fps,
                         (int)si.dwNumberOfProcessors, buf, 512, preset.c_str());
    return buf;
}

// Resolves the {app} filename-template placeholder - see the header
// comment on the declaration for what this falls back to and why.
std::string RecordingController::ResolveCaptureAppName() const {
    if (state_.capture_mode != CaptureMode::Window || state_.capture_window_title.empty()) {
        return "Desktop";
    }

    HWND hwnd = nullptr;
    RECT r{};
    if (!HR_ResolveCaptureWindow(state_.capture_window_title, hwnd, r) || !hwnd) {
        return "Desktop"; // window's gone (closed, title changed) - fall back rather than emit "{app}" literally
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return "Desktop";

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return "Desktop";

    char path[MAX_PATH] = {};
    DWORD path_len = MAX_PATH;
    bool ok = QueryFullProcessImageNameA(hProc, 0, path, &path_len) != 0;
    CloseHandle(hProc);
    if (!ok) return "Desktop";

    // path -> just the base name, no directory, no ".exe" - "notepad.exe"
    // at "C:\Windows\notepad.exe" becomes "notepad".
    std::string name = path;
    size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.size() > 4) {
        std::string ext = name.substr(name.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".exe") name = name.substr(0, name.size() - 4);
    }

    // Filenames can't contain \ / : * ? " < > | on Windows - a process
    // name shouldn't ever contain these, but a well-behaved fallback
    // beats a failed hr_ff_start() over one that theoretically could.
    for (char &c : name) {
        if (strchr("\\/:*?\"<>|", c)) c = '_';
    }
    return name.empty() ? "Desktop" : name;
}

std::wstring RecordingController::BuildOutputPath() {
    char buf[256] = {};
    std::string app_name = ResolveCaptureAppName();
    hr_filename_from_template(state_.filename_template.c_str(), state_.output_folder.c_str(),
                               app_name.c_str(), buf, 256);
    return WideFromNarrow(buf);
}

void RecordingController::ScaledPreviewSize(int &out_w, int &out_h) const {
    int pct = state_.preview_quality_pct;
    if (pct < 10) pct = 10;
    if (pct > 100) pct = 100;
    out_w = (state_.preview_width * pct) / 100;
    out_h = (state_.preview_height * pct) / 100;
    if (out_w < 2) out_w = 2;
    if (out_h < 2) out_h = 2;
}

// Settings > Resolution: turns a source size (the full monitor, or the
// cropped window rect in window-capture mode) into the desired *output*
// size, honoring ResolutionMode::Percent (scale_factor, e.g. 75% of
// src_w/src_h) or ResolutionMode::Absolute (an exact resolution_w x
// resolution_h target, e.g. "1280x720" for an old/low-power playback
// device). Absolute mode never upscales past src_w/src_h - there's no
// extra detail to gain from it, only extra bytes to capture/encode - and
// always leaves at least a 2x2 output so a bad/zero setting can't produce
// a degenerate 0x0 pipe.
void RecordingController::ComputeOutputDims(int src_w, int src_h, int &out_w, int &out_h) const {
    if (state_.resolution_mode == ResolutionMode::Absolute) {
        out_w = state_.resolution_w;
        out_h = state_.resolution_h;
        if (out_w > src_w) out_w = src_w;
        if (out_h > src_h) out_h = src_h;
        if (out_w < 2) out_w = 2;
        if (out_h < 2) out_h = 2;
    } else {
        out_w = (int)(src_w * state_.scale_factor);
        out_h = (int)(src_h * state_.scale_factor);
    }
    if (out_w % 2) out_w--;
    if (out_h % 2) out_h--;
}

void RecordingController::ResolveCaptureSize() {
    void *di = hr_di_create();
    hr_di_refresh(di);
    int mx = 0, my = 0, mw = 1920, mh = 1080;
    float dpi = 96.0f;
    int idx = state_.monitor_id > 0 ? state_.monitor_id - 1 : 0;

    HWND capture_hwnd = nullptr;
    RECT capture_win_rect{};
    bool have_window = false;
    if (state_.capture_mode == CaptureMode::Window && !state_.capture_window_title.empty()) {
        have_window = HR_ResolveCaptureWindow(state_.capture_window_title, capture_hwnd, capture_win_rect);
        if (have_window) {
            int wcx = (capture_win_rect.left + capture_win_rect.right) / 2;
            int wcy = (capture_win_rect.top + capture_win_rect.bottom) / 2;
            int count = hr_di_count(di);
            for (int i = 0; i < count; ++i) {
                int tx = 0, ty = 0, tw = 0, th = 0; float td = 96.0f;
                if (!hr_di_get(di, i, &tx, &ty, &tw, &th, &td)) continue;
                if (wcx >= tx && wcx < tx + tw && wcy >= ty && wcy < ty + th) {
                    idx = i;
                    break;
                }
            }
        }
    }

    // Resolve real capture resolution from the selected monitor
    // (hr_di_get is 0-based, hence the state_.monitor_id -1 above).
    if (!hr_di_get(di, idx, &mx, &my, &mw, &mh, &dpi)) {
        idx = 0; // fell back to primary just below - keep the DXGI output index in sync with it
        hr_di_primary(di, &mx, &my, &mw, &mh, &dpi); // fall back to primary if the index is out of range
    }
    hr_di_destroy(di);
    capture_output_idx_ = idx;
    state_.monitor_left = mx;
    state_.monitor_top = my;

    // capture_w_/capture_h_ MUST equal the monitor's actual native
    // resolution - this is the size DXGI Desktop Duplication actually
    // hands back (it has no "capture at a reduced size" mode), the size
    // the capture buffer is allocated at, and the size ffmpeg's rawvideo
    // demuxer is told to expect on the pipe. Any of those disagreeing
    // with what's *actually* captured is exactly what produced the
    // garbled/green/tiled recordings - this was previously scaled down
    // by scale_factor here, which the pipeline would then silently
    // "correct" back to native right before it started capturing (to
    // avoid overflowing the undersized buffer), but ffmpeg had already
    // been told the smaller, wrong size and had no way to know that changed.
    capture_w_ = mw;
    capture_h_ = mh;
    if (capture_w_ % 2) capture_w_--;
    if (capture_h_ % 2) capture_h_--;

    // Settings > Resolution (scale_factor / resolution_w+h, depending on
    // resolution_mode) instead becomes the desired *output* size - applied
    // as a capture-side downscale before encoding (see
    // hr_pl_set_output_size()/hr_pipeline.cpp), not by changing what DXGI
    // itself captures.
    ComputeOutputDims(mw, mh, output_w_, output_h_);

    // ====== WINDOW CAPTURE: crop the now-correctly-selected monitor's
    // frame down to the window's rect ======
    // Reuses the resolve done above (have_window/capture_win_rect)
    // instead of calling HR_ResolveCaptureWindow() a second time - besides
    // the redundant EnumWindows() pass, re-resolving here could in theory
    // hit a narrow window between the two calls where the window closed
    // and its title got reused by something else, cropping to the wrong
    // window's rect. One resolve, used consistently for both which
    // monitor to capture and where to crop it.
    crop_x_ = crop_y_ = crop_w_ = crop_h_ = 0;
    if (state_.capture_mode == CaptureMode::Window && !state_.capture_window_title.empty()) {
        if (have_window) {
            RECT r = capture_win_rect;
            // Window rect is in virtual-desktop coordinates; crop_x_/y_
            // need to be relative to the captured monitor's own frame
            // (monitor_left/top, set above), matching what bgra_buf
            // actually holds.
            int wx = r.left - mx, wy = r.top - my;
            int ww = r.right - r.left, wh = r.bottom - r.top;
            // Clamp to the monitor bounds - hr_pl_set_capture_rect() also
            // clamps defensively, but doing it here too means output_w_/
            // output_h_ (computed from ww/wh below) reflect the actual
            // clamped crop size, not the pre-clamp one.
            if (wx < 0) { ww += wx; wx = 0; }
            if (wy < 0) { wh += wy; wy = 0; }
            if (wx + ww > capture_w_) ww = capture_w_ - wx;
            if (wy + wh > capture_h_) wh = capture_h_ - wy;
            if (ww % 2) ww--;
            if (wh % 2) wh--;

            if (ww > 0 && wh > 0) {
                crop_x_ = wx; crop_y_ = wy; crop_w_ = ww; crop_h_ = wh;
                ComputeOutputDims(ww, wh, output_w_, output_h_);
            } else {
                HrLog::Warn("Window capture: '" + state_.capture_window_title +
                            "' is entirely off the selected monitor - falling back to full desktop.");
            }
        } else {
            // Window was closed/renamed since being picked, or isn't on
            // screen anymore. Fall back to full-desktop capture rather
            // than starting a recording of nothing/garbage - crop_*_ are
            // already 0 from the reset above, so capture_w_/h_/output_w_/
            // h_ (monitor-sized, set earlier in this function) stand as-is.
            HrLog::Warn("Window capture: couldn't find a window titled '" +
                        state_.capture_window_title + "' - falling back to full desktop.");
        }
    }
}

bool RecordingController::Start(std::wstring &error_out) {
    if (state_.recording) { error_out = L"Already recording."; return false; }

    // Instant Replay and a manual recording share this class's one
    // pipeline_ and hr_pl_set_recording()'s one active pipe_fd (see the
    // header comment on EnableInstantReplay()) - if the background
    // buffer is currently running, stop its segment writer so Start()
    // below is free to redirect pipeline_'s pipe to the manual recording
    // instead. instant_replay_enabled_ (the user's "I want this on"
    // choice) stays true; Stop() resumes it once this recording ends.
    if (instant_replay_active_) {
        StopInstantReplayEncoderAsync();
        instant_replay_active_ = false;
        HrLog::Info("Instant Replay: paused for a manual recording.");
    }

    if (!hr_path_exists(state_.output_folder.c_str())) {
        if (!hr_make_output_dir(state_.output_folder.c_str())) {
            error_out = L"Output folder doesn't exist and couldn't be created.";
            HrLog::Error("Start failed: output folder missing/uncreatable: " + state_.output_folder);
            return false;
        }
    }
    if (!ffmpeg_found_) {
        error_out = L"FFmpeg not found.";
        HrLog::Error("Start failed: ffmpeg not found");
        return false;
    }

    // hr_get_free_disk_mb() (hr_ui_utils.cpp) existed already but nothing
    // ever called it - a recording would happily start on an almost-full
    // drive and just die (ffmpeg write failure) partway through instead of
    // being refused up front. A 0 return (query failed, e.g. odd path) is
    // treated as "couldn't determine" and doesn't block Start(), same as
    // the helper's own doc comment says callers should treat it.
    uint64_t free_mb = 0;
    if (hr_get_free_disk_mb(state_.output_folder.c_str(), &free_mb) && free_mb < kMinFreeDiskMb) {
        error_out = L"Not enough free disk space on the output drive (" +
                    std::to_wstring(free_mb) + L" MB free, need at least " +
                    std::to_wstring(kMinFreeDiskMb) + L" MB).";
        HrLog::Error("Start failed: low disk space (" + std::to_string(free_mb) + " MB free)");
        return false;
    }

    current_output_path_ = BuildOutputPath();

    int prev_w = capture_w_, prev_h = capture_h_;
    ResolveCaptureSize();

    // Prefer the probed GPU encoder; fall back to libx264 if none/if the
    // caller already forced a specific codec in settings.
    std::wstring codec = state_.video_codec == "libx264" && !hw_encoder_.empty()
                              ? hw_encoder_
                              : WideFromNarrow(state_.video_codec);
    std::wstring codec_args = BuildCodecArgs(codec);
    HrLog::Info("Recording: encoding with " + NarrowFromWide(codec) +
                (codec == L"libx264" || codec == L"libx265" ? " (software)" : " (hardware)"));

    ffproc_ = hr_ff_create();
    hr_ff_set_ffmpeg_path(ffproc_, NarrowFromWide(ffmpeg_path_).c_str());
    hr_ff_set_output_path(ffproc_, NarrowFromWide(current_output_path_).c_str());
    hr_ff_set_codec_args(ffproc_, NarrowFromWide(codec_args).c_str());
    // Video params tell ffmpeg's rawvideo demuxer the size of the frames
    // that will actually arrive on the pipe - that's the CROPPED size in
    // window-capture mode (crop_w_/crop_h_ > 0), not the monitor's native
    // capture_w_/capture_h_, since hr_pl_set_capture_rect() below makes
    // the pipeline crop before writing to the pipe. Getting this wrong is
    // exactly the "garbled/green/tiled recording" failure mode the big
    // comment in ResolveCaptureSize() warns about, just triggered from
    // window-capture mode instead of a stale scale_factor.
    int pipe_w = output_w_;
    int pipe_h = output_h_;
    hr_ff_set_video_params(ffproc_, pipe_w, pipe_h, state_.target_fps);
    hr_ff_set_output_size(ffproc_, pipe_w, pipe_h);
    hr_ff_set_pipe_input(ffproc_, 1);

    if (hr_ff_start(ffproc_) != 0) {
        error_out = L"Failed to start the ffmpeg process.";
        HrLog::Error("Start failed: ffmpeg process didn't start");
        hr_ff_destroy(ffproc_);
        ffproc_ = nullptr;
        return false;
    }

    // The pipeline writes captured frames straight into ffmpeg's stdin
    // pipe, so it needs the real write-end HANDLE ffmpeg was launched
    // with - not a placeholder. hr_ff_start() above creates that pipe
    // internally; this is the only way to get it back out.
    intptr_t ff_stdin = hr_ff_get_stdin_handle(ffproc_);
    if (ff_stdin == 0) {
        error_out = L"Failed to start the ffmpeg process.";
        HrLog::Error("Start failed: ffmpeg stdin pipe handle unavailable");
        hr_ff_kill(ffproc_);
        hr_ff_destroy(ffproc_);
        ffproc_ = nullptr;
        return false;
    }

    if (state_.audio_out_channels > 0) {
        // A manual recording needs every sample from here to Stop() kept -
        // undo whatever rolling-window cap Instant Replay's background
        // buffering left in place (see hr_audio_set_max_buffer_sec()'s own
        // comment) before hr_audio_reset_buffers() starts this recording's
        // accumulation.
        hr_audio_set_max_buffer_sec(0);
        hr_audio_reset_buffers();
        hr_audio_set_volumes(mic_vol_, sys_vol_, mic_muted_ ? 1 : 0, sys_muted_ ? 1 : 0);
    }

    // Pipeline handles the actual DXGI capture + frame conversion + piping
    // frames into ffmpeg's stdin. pv_w/pv_h come from AppState's preview
    // panel size (set by main_window on layout).
    //
    // If EnsurePreview() already has a preview-only pipeline running at
    // the same capture size, just flip it into recording mode instead of
    // destroying and recreating it - keeps the live preview seamless
    // right through Start() instead of it blinking out and back.
    // ffmpeg expects.
    bool reused_preview_pipeline = false;
    if (pipeline_ && capture_w_ == prev_w && capture_h_ == prev_h &&
        pipeline_output_idx_ == capture_output_idx_) {
        hr_pl_set_capture_rect(pipeline_, crop_x_, crop_y_, crop_w_, crop_h_);
        hr_pl_set_output_size(pipeline_, output_w_, output_h_);
        hr_pl_set_recording(pipeline_, /*active=*/1, ff_stdin);
        reused_preview_pipeline = true;
    } else {
        JoinPendingInstantReplayStop();
        if (pipeline_) { hr_pl_destroy(pipeline_); pipeline_ = nullptr; }
        // See JoinPendingPreviewTeardown()'s comment
        // (recording_controller.h) - without this, starting a recording
        // shortly after preview was torn down (e.g. "Disable live preview"
        // is on, or the user just closed "Position Overlays...") could
        // race that async teardown's DXGI duplication release and fail
        // here with "Failed to start the capture pipeline", looking like
        // recording just can't start with preview off until the app is
        // restarted.
        JoinPendingPreviewTeardown();
        int pvw = 0, pvh = 0;
        ScaledPreviewSize(pvw, pvh);
        pipeline_ = hr_pl_create(capture_w_, capture_h_, state_.target_fps, ff_stdin,
                                 pvw, pvh, capture_output_idx_);
        pipeline_output_idx_ = capture_output_idx_;
        last_overlays_sent_valid_ = false;
    }
    bool pipeline_started = reused_preview_pipeline;
    if (pipeline_ && !reused_preview_pipeline) {
        hr_pl_set_capture_rect(pipeline_, crop_x_, crop_y_, crop_w_, crop_h_);
        hr_pl_set_output_size(pipeline_, output_w_, output_h_);
        pipeline_started = hr_pl_start(pipeline_) != 0;
    }
    if (pipeline_) {
        hr_pl_set_preview_fps(pipeline_, state_.preview_fps);
        // A manual recording always needs a real Pipeline (it's also the
        // encoder path) regardless of the "Disable live preview" setting,
        // but there's no reason for it to keep generating an unread
        // thumbnail every preview_fps tick when that setting is on - see
        // Pipeline::preview_needed's own comment (hr_pipeline.cpp).
        hr_pl_set_preview_needed(pipeline_, state_.disable_preview ? 0 : 1);
    }
    if (!pipeline_ || !pipeline_started) {
        error_out = L"Failed to start the capture pipeline.";
        HrLog::Error("Start failed: capture pipeline didn't start");
        hr_ff_kill(ffproc_);
        hr_ff_destroy(ffproc_);
        ffproc_ = nullptr;
        if (pipeline_) { hr_pl_destroy(pipeline_); pipeline_ = nullptr; }
        return false;
    }

    // Belt-and-suspenders: hr_pl_create() (fresh pipeline) already turns
    // recording on internally, and the reuse branch above already called
    // this too - but re-asserting here is a harmless no-op either way and
    // guards against either path's default ever silently changing.
    hr_pl_set_recording(pipeline_, /*active=*/1, ff_stdin);

    // A manual recording is genuinely happening now (as opposed to just
    // Instant Replay's background buffer, which also sets `recording` via
    // hr_pl_set_recording() above but should stay at ordinary priority -
    // see boost_priority's comment in hr_pipeline.cpp / bug #5) - this is
    // the one place that's actually true, so bump the capture thread.
    hr_pl_set_priority_boost(pipeline_, 1);

    // (Audio capture is already running continuously, started once in
    // Initialize() - hr_audio_reset_buffers()/hr_audio_set_volumes() for
    // this recording were already called above, right before the video
    // pipeline started, to keep the two as close in time as possible; see
    // that call site's comment for why it moved.)

    hr_ctl_set_output_path(ctl_, NarrowFromWide(current_output_path_).c_str());
    hr_ctl_start(ctl_);

    state_.recording = true;
    state_.paused = false;
    state_.frame_count = 0;
    last_drops_seen_ = 0;
    overload_streak_ = 0;
    overloaded_ = false;
    HrLog::Info("Recording started -> " + NarrowFromWide(current_output_path_) +
                " (" + std::to_string(output_w_) + "x" + std::to_string(output_h_) +
                " @ " + std::to_string(state_.target_fps) + "fps, captured at " +
                std::to_string(capture_w_) + "x" + std::to_string(capture_h_) + ")");
    return true;
}

void RecordingController::Stop() {
    // Old synchronous behavior, kept for callers that genuinely want to
    // block until everything's done (e.g. the app closing, where there's
    // nothing useful to do except wait anyway). See StopAsync() for what
    // actually does the work and why DoStop() (main_frame.cpp) uses that
    // instead - this used to be the whole implementation, freezing
    // whichever thread called it (in practice, the UI thread) for up to
    // ~30s.
    StopAsync(nullptr);
    if (finalize_thread_.joinable()) finalize_thread_.join();
}

void RecordingController::StopAsync(std::function<void()> on_done) {
    if (!state_.recording || finalizing_) return;
    finalizing_ = true;

    // If we're keeping the pipeline alive afterward for continued live
    // preview, don't call hr_pl_stop() here - that joins the capture
    // thread, which would freeze the preview the moment Stop() runs
    // instead of leaving it live. Just stop ffmpeg; the pipeline gets
    // switched out of recording mode (not stopped) further down.
    bool keep_for_preview = pipeline_ && !state_.disable_preview;

    // BUGFIX (Stop button freezes the whole app for a second or two):
    // hr_pl_stop() itself waits (with a timeout) for the capture thread
    // and then the writer thread to each signal that they've actually
    // stopped - up to ~1s per thread, by design (see its own comment in
    // hr_pipeline.cpp). This function is StopAsync() - callers (the Stop
    // button's DoStop()) call it directly on the UI thread specifically
    // so they *don't* have to block - but this call used to run right
    // here, before finalize_thread_ below even existed, so every one of
    // those waits still happened synchronously on whichever thread called
    // StopAsync(). Move it inside the background thread instead, right
    // alongside the rest of the (already-async) finalize work.
    hr_ff_stop_graceful(ffproc_);

    finalize_thread_ = std::thread([this, keep_for_preview, on_done]() {
    if (!keep_for_preview) {
        hr_pl_stop(pipeline_);
    } else {
        // BUGFIX (recordings always took ~30s to "finalize" and came out
        // corrupt whenever preview was left on - see
        // hr_pl_end_recording_segment()'s comment for the full story):
        // hr_pl_stop() above is what used to be the *only* thing that
        // ever actually closed ffmpeg's stdin pipe (as part of fully
        // tearing the pipeline's threads down) - skipped here specifically
        // so the pipeline survives for continued preview, which also
        // meant nothing closed the pipe, so StopFinalizeTail()'s
        // hr_ff_wait() below could never see the EOF it needs and always
        // ran out its full timeout before force-killing ffmpeg mid-write.
        // Ask the still-running writer thread to close just this
        // recording's pipe instead.
        hr_pl_end_recording_segment(pipeline_, 3000);
    }
    StopFinalizeTail(keep_for_preview);
    finalizing_ = false;
    if (on_done) on_done();
    });
}

void RecordingController::StopFinalizeTail(bool keep_for_preview) {
    // Belt-and-suspenders alongside the two call sites in Start()/
    // EnableInstantReplay() that actually race this: this function can
    // go on to destroy pipeline_ below (the !keep_for_preview / Instant-
    // Replay-resume-failed branches) - vanishingly unlikely but possible
    // for Start()'s own instant_replay_stop_thread_ (see
    // JoinPendingInstantReplayStop()'s comment) to still be running if
    // Stop() is pressed within a couple seconds of Start(). Cheap no-op
    // in the overwhelmingly common case where it already finished.
    JoinPendingInstantReplayStop();

    // This used to be hr_ff_wait(ffproc_, 3000) -- a flat 3 second
    // budget for ffmpeg to receive EOF on stdin, flush libx264's internal
    // frame buffer, and write the moov atom (mp4) / cues (mkv) that make the
    // file actually playable. 3 seconds is fine on an idle machine for a
    // short clip, but under real load (a long recording, a slower CPU, the
    // encoder having a backlog queued -- see the CPU-usage notes elsewhere)
    // it's easy to still be finishing that flush when the timeout expires.
    // hr_ff_wait() only clears ctx->running on a *successful* wait -- on a
    // timeout it just returns, leaving ffmpeg running -- and hr_ff_destroy()
    // a few lines down force-kills (TerminateProcess) anything still marked
    // running. So a slow-but-otherwise-fine finalize was getting yanked out
    // from under ffmpeg mid-write, which is exactly how you end up with a
    // ~1KB file Windows calls an unsupported codec: just the initial
    // ftyp/moov placeholder, no actual finalized index.
    //
    // Now: wait considerably longer (10s) before even checking in, and if
    // it's still not done, log that clearly and give it one more, longer
    // window (20s) rather than silently killing it. Only after ~30s total
    // -- long enough that something is genuinely stuck, not just finishing
    // up -- does hr_ff_destroy() below fall back to a force-kill, and by
    // then the log at least explains why the file might be incomplete
    // instead of leaving the user with an unexplained corrupt recording.
    if (hr_ff_wait(ffproc_, 10000) != 0 && hr_ff_is_running(ffproc_)) {
        HrLog::Warn("Recording: ffmpeg is still finalizing the output after 10s "
                    "(large/long recording, or the machine is under heavy load) -- "
                    "waiting longer before giving up.");
        if (hr_ff_wait(ffproc_, 20000) != 0 && hr_ff_is_running(ffproc_)) {
            HrLog::Error("Recording: ffmpeg didn't finish on its own after 30s total -- "
                        "force-stopping it now. The output file may be incomplete/corrupt.");
        }
    }

    // mic/system audio were captured into separate temp WAVs (unless
    // muted); mix them into one file, then remux that into the finished
    // video via hr_merge_av, the native fast path used when no custom
    // bitrate/extra ffmpeg args are set. See the notes below for
    // what happens to that intermediate WAV afterward.
    std::string base = NarrowFromWide(current_output_path_);
    size_t dot = base.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? base : base.substr(0, dot);
    std::string mic_wav = stem + "_mic_tmp.wav";
    std::string sys_wav = stem + "_sys.wav";
    std::string audio_wav = stem + "_audio.wav";

    bool want_mic = !mic_muted_;
    bool want_sys = !sys_muted_;
    int audio_result = hr_audio_capture_to_wav(want_mic ? mic_wav.c_str() : nullptr,
                                                want_sys ? sys_wav.c_str() : nullptr);
    bool mic_written = want_mic && (audio_result & 0x1) && hr_path_exists(mic_wav.c_str());
    bool sys_written = want_sys && (audio_result & 0x2) && hr_path_exists(sys_wav.c_str());

    bool have_audio_file = false;
    if (mic_written && sys_written) {
        if (hr_audio_mix_wav(mic_wav.c_str(), sys_wav.c_str(), audio_wav.c_str()) == 0) {
            std::remove(mic_wav.c_str());
            std::remove(sys_wav.c_str());
            have_audio_file = true;
        } else {
            // Mixing failed - fall back to whichever single track exists
            // (mic preferred).
            std::remove(sys_wav.c_str());
            std::rename(mic_wav.c_str(), audio_wav.c_str());
            have_audio_file = true;
        }
    } else if (mic_written) {
        std::rename(mic_wav.c_str(), audio_wav.c_str());
        have_audio_file = true;
    } else if (sys_written) {
        std::rename(sys_wav.c_str(), audio_wav.c_str());
        have_audio_file = true;
    }

    // This used to fire-and-forget hr_merge_av() and never look at
    // its result, and on a *successful* native merge deliberately left the
    // leftover audio_wav file sitting right next to the finished video
    // forever (per the old comment above, that was on purpose -- but it's
    // exactly what "the video and audio come out as separate files" looks
    // like from the outside: a working muxed .mp4 plus a same-named .wav
    // next to it that nothing ever cleaned up). Now: check the result, and
    // only keep a separate audio file around when there's a real reason to
    // (the merge failed/was skipped, so it's the only copy of the audio
    // that exists -- or the user asked for one, see below).
    bool merged = false;
    if (have_audio_file && state_.audio_out_channels > 0 && ffmpeg_found_ &&
        hr_path_exists(base.c_str())) {
        // real_elapsed_sec is the actual wall-clock recording time (still
        // live here - hr_ctl_stop() below is what zeroes it), used by
        // hr_merge_av() to detect and correct a video that came out much
        // shorter than the recording actually ran (dropped-frame
        // overload) instead of cutting the accurate audio down to match.
        merged = hr_merge_av(ffmpeg_path_.c_str(), current_output_path_.c_str(),
                              WideFromNarrow(audio_wav).c_str(),
                              elapsed_seconds()) != 0;
        if (!merged) {
            HrLog::Error("Recording: merging the captured audio into the video failed -- "
                        "keeping '" + audio_wav + "' next to the (silent) video instead "
                        "of losing the audio entirely.");
        }
    } else if (have_audio_file) {
        HrLog::Warn("Recording: audio was captured but not merged into the video "
                    "(ffmpeg wasn't found, or audio is disabled) -- keeping '" +
                    audio_wav + "' next to the video.");
    }

    // "Also save audio as a separate MP3" (Settings) was persisted
    // (see hrc_config.cpp) and shown in the dialog, but nothing anywhere
    // ever read state_.separate_audio_mp3 or produced an .mp3 -- the
    // checkbox did nothing at all. On a successful merge the WAV's contents
    // already live inside the finished video, so: encode a real .mp3 next
    // to it if the setting's on, otherwise just delete the now-redundant
    // WAV. On a failed/skipped merge the WAV is always kept regardless of
    // the setting, since in that case it's the only copy of the audio.
    if (merged) {
        if (state_.separate_audio_mp3 && ffmpeg_found_) {
            std::string mp3_path = stem + ".mp3";
            if (hr_export_mp3(ffmpeg_path_.c_str(), WideFromNarrow(audio_wav).c_str(),
                               WideFromNarrow(mp3_path).c_str())) {
                std::remove(audio_wav.c_str());
            } else {
                HrLog::Warn("Recording: couldn't export a separate MP3 -- leaving '" +
                            audio_wav + "' (WAV) instead.");
            }
        } else {
            std::remove(audio_wav.c_str());
        }
    }

    // hr_ctl_stop()'s return value is "elapsed seconds for final
    // summary" per its own doc comment, but this used to be called as a
    // bare statement that threw the result away. hr_ctl_stop() also flips
    // the session to HR_STATE_IDLE, at which point elapsed_seconds()/
    // elapsed_formatted() (both read the *live* session) start returning
    // 0.0 / "00:00:00" -- so anything built after this line that wanted to
    // show "you just recorded N seconds" got nothing. Snapshot it (plus the
    // output path/size, which have the same "still fine right now, gone a
    // few lines down" problem once ffproc_ is destroyed below) into
    // last_*_ members that stay valid until the next Start().
    last_duration_sec_ = hr_ctl_stop(ctl_);
    last_output_path_ = current_output_path_;
    last_output_size_mb_ = ffproc_ ? hr_ff_output_size_mb(ffproc_) : 0.0;

    // Previously this destroyed the pipeline outright, which is exactly
    // why preview only ever worked *during* a recording - once Stop() ran
    // there was nothing left to show a frame from. Now: if preview isn't
    // disabled, just switch the same (still-running) pipeline back to
    // preview-only mode so the live preview keeps working right after the
    // recording ends.

    // The manual recording that justified boosting the capture thread's
    // priority (see Start()'s hr_pl_set_priority_boost(pipeline_, 1) call
    // and boost_priority's comment in hr_pipeline.cpp / bug #5) is over -
    // every branch below either drops back to plain preview or resumes
    // Instant Replay's background buffer, neither of which should run
    // boosted. StartInstantReplayEncoder() below only ever calls
    // hr_pl_set_recording() (never touches priority), so this needs to
    // happen unconditionally here rather than in just one branch.
    if (pipeline_) hr_pl_set_priority_boost(pipeline_, 0);

    if (instant_replay_enabled_) {
        // Resume Instant Replay's background buffer (paused, if it was
        // running, at the top of Start() above) rather than falling back
        // to plain preview-only mode - a fresh buffer, same as any other
        // StartInstantReplayEncoder() call.
        std::wstring resume_err;
        if (StartInstantReplayEncoder(resume_err)) {
            instant_replay_active_ = true;
            HrLog::Info("Instant Replay: resumed buffering after the recording stopped.");
        } else {
            HrLog::Error(NarrowFromWide(L"Instant Replay: failed to resume after the recording stopped - " + resume_err));
            if (keep_for_preview) hr_pl_set_recording(pipeline_, /*active=*/0, /*pipe_fd=*/0);
            else if (pipeline_) { hr_pl_destroy(pipeline_); pipeline_ = nullptr; }
        }
    } else if (keep_for_preview) {
        hr_pl_set_recording(pipeline_, /*active=*/0, /*pipe_fd=*/0);
    } else if (pipeline_) {
        hr_pl_destroy(pipeline_);
        pipeline_ = nullptr;
    }
    hr_ff_destroy(ffproc_);
    ffproc_ = nullptr;

    state_.recording = false;
    state_.paused = false;
    overloaded_ = false;
    overload_streak_ = 0;
    HrLog::Info("Recording stopped -> " + base);
}

void RecordingController::TogglePause() {
    if (!state_.recording) return;
    int new_state = hr_ctl_pause_toggle(ctl_);
    state_.paused = (new_state == 2 /* HR_STATE_PAUSED */);
    if (pipeline_) hr_pl_pause(pipeline_, state_.paused ? 1 : 0);
    HrLog::Info(state_.paused ? "Recording paused" : "Recording resumed");
}

void RecordingController::PollStats() {
    if (!state_.recording) return;
    if (ffproc_ && !finalizing_ && !hr_ff_is_running(ffproc_)) {
        HrLog::Error("Recording: the ffmpeg process ended unexpectedly mid-recording "
                     "(crashed, was closed externally, or hit a fatal error) - flagging "
                     "this as a crash so the UI can stop and finalize what was captured.");
        crashed_ = true;
        return;
    }

    long long frames = 0, drops = 0;
    double fps = 0.0;
    if (pipeline_) hr_pl_stats(pipeline_, &frames, &drops, &fps);
    state_.frame_count = (long)frames;
    current_fps_ = fps;

    // Overload warning: a few consecutive ticks of new drops while
    // actually recording (not paused) means the capture/encode pipeline
    // can't keep up in real time, as opposed to one stray drop under a
    // momentary hiccup. Streak resets the instant a tick comes back clean.
    long long drops_delta = drops - last_drops_seen_;
    last_drops_seen_ = drops;
    if (!state_.paused && drops_delta > 0) {
        if (overload_streak_ < 3) ++overload_streak_;
    } else {
        overload_streak_ = 0;
    }
    bool now_overloaded = overload_streak_ >= 3;
    if (now_overloaded != overloaded_) {
        overloaded_ = now_overloaded;
        if (overloaded_) {
            HrLog::Warn("Recording overloaded: dropping frames -- system can't keep up "
                        "with capture/encode in real time.");
        }
    }

    double size_mb = ffproc_ ? hr_ff_output_size_mb(ffproc_) : 0.0;
    hr_ctl_update_stats(ctl_, (long long)(size_mb * 1024.0 * 1024.0));

    hr_audio_get_levels(&mic_level_, &sys_level_);
}

void RecordingController::SyncOverlays() {
    if (!pipeline_) {
        // pipeline - this retry is only for the idle/preview-only case.
        if (!state_.disable_preview && !state_.recording) {
            auto now = std::chrono::steady_clock::now();
            if (now >= next_preview_retry_) {
                int backoff_s = kPreviewRetryBaseSeconds;
                if (preview_retry_streak_ >= kPreviewRetryBackoffAfter) {
                    int shift = preview_retry_streak_ - kPreviewRetryBackoffAfter;
                    if (shift > 8) shift = 8; // avoid overflowing the left-shift below
                    int64_t scaled = (int64_t)kPreviewRetryBaseSeconds << shift;
                    backoff_s = (int)std::min<int64_t>(scaled, kPreviewRetryMaxSeconds);
                }
                next_preview_retry_ = now + std::chrono::seconds(backoff_s);
                EnsurePreview();
            }
        }
        return;
    }

    // "Cursor" setting - cheap atomic store, fine to re-apply every tick
    // rather than needing its own change-tracking like the overlay list
    // below (which is expensive enough per-tick to be worth skipping when
    // unchanged).
    hr_pl_set_include_cursor(pipeline_, state_.cursor_enabled ? 1 : 0);

    // "#RRGGBB" -> (r,g,b); falls back to white on anything malformed
    // (missing '#', wrong length, non-hex digits) so a bad/empty value never
    // renders invisible black-on-black text.
    auto parseHexColor = [](const std::string &hex, unsigned char &r, unsigned char &g, unsigned char &b) {
        r = g = b = 255;
        if (hex.size() != 7 || hex[0] != '#') return;
        auto nyb = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int digits[6];
        for (int i = 0; i < 6; ++i) {
            digits[i] = nyb(hex[1 + i]);
            if (digits[i] < 0) return; // non-hex character -> keep the white fallback
        }
        r = (unsigned char)((digits[0] << 4) | digits[1]);
        g = (unsigned char)((digits[2] << 4) | digits[3]);
        b = (unsigned char)((digits[4] << 4) | digits[5]);
    };

    std::vector<HrOverlayDesc> descs;
    descs.reserve(state_.overlays.size());
    for (const auto &ov : state_.overlays) {
        HrOverlayDesc d{};
        std::strncpy(d.type, ov.type.c_str(), sizeof(d.type) - 1);
        std::strncpy(d.text, ov.text.c_str(), sizeof(d.text) - 1);
        std::strncpy(d.image_path, ov.image_path.c_str(), sizeof(d.image_path) - 1);
        std::strncpy(d.input_json_path, ov.input_json_path.c_str(), sizeof(d.input_json_path) - 1);
        std::strncpy(d.input_png_path, ov.input_png_path.c_str(), sizeof(d.input_png_path) - 1);
        std::strncpy(d.webcam_name, ov.webcam_name.c_str(), sizeof(d.webcam_name) - 1);
        d.webcam_index = ov.webcam_index;
        d.x = ov.x; d.y = ov.y; d.w = ov.w; d.h = ov.h;
        d.visible = ov.visible ? 1 : 0;
        parseHexColor(ov.text_color, d.text_r, d.text_g, d.text_b);
        std::strncpy(d.font_name, ov.font_family.c_str(), sizeof(d.font_name) - 1);
        // ov.opacity is 0-100 (the UI slider's own units); HrOverlayDesc
        // wants 0-255 (what CompositeBgra actually multiplies alpha by).
        int pct = ov.opacity < 0 ? 0 : (ov.opacity > 100 ? 100 : ov.opacity);
        d.opacity = (unsigned char)((pct * 255 + 50) / 100);
        descs.push_back(d);
    }

    // SyncOverlays() runs on every preview timer tick (continuously, not
    // just while recording/dragging), but the overlay list itself only
    // actually changes on a real add/remove/drag/resize/edit. Skip the
    // pipeline call entirely when the list is identical to what was last
    // sent, instead of re-locking the pipeline's overlays_mtx and copying
    // the whole vector on every single tick for no reason.
    bool same = last_overlays_sent_valid_ &&
                last_overlays_sent_.size() == descs.size() &&
                (descs.empty() ||
                 std::memcmp(last_overlays_sent_.data(), descs.data(),
                             sizeof(HrOverlayDesc) * descs.size()) == 0);
    if (same) return;

    hr_pl_set_overlays(pipeline_, descs.empty() ? nullptr : descs.data(), (int)descs.size());
    last_overlays_sent_ = std::move(descs);
    last_overlays_sent_valid_ = true;
}

bool RecordingController::StartInstantReplayEncoder(std::wstring &error_out) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    // A fresh, uniquely-numbered subfolder per run rather than reusing/
    // clearing one folder - guarantees a stale segment from a previous
    // run (or a previous app session, if the temp folder was never
    // cleaned) can never end up in a concat list built from replay_dir_.
    std::wstring base_dir = std::wstring(tmp) + L"HomRec_replay\\";
    CreateDirectoryW(base_dir.c_str(), nullptr); // fine if it already exists
    replay_dir_ = base_dir + std::to_wstring(++replay_run_seq_) + L"\\";
    CreateDirectoryW(replay_dir_.c_str(), nullptr);

    int buf_sec = state_.replay_buffer_sec > 0 ? state_.replay_buffer_sec : 30;
    // +1 segment of slack so the oldest segment still needed for a
    // full-length Save Replay isn't wrapped-over right as it's about to
    // be read, and a sane hard ceiling (720 * 5s = 1hr) so a huge/typo'd
    // buffer-seconds setting can't make the writer wrap through an
    // unbounded number of segment files.
    int wrap = (buf_sec + kReplaySegmentSec - 1) / kReplaySegmentSec + 1;
    if (wrap > 720) wrap = 720;
    if (wrap < 2) wrap = 2;

    std::wstring codec = state_.video_codec == "libx264" && !hw_encoder_.empty()
                              ? hw_encoder_ : WideFromNarrow(state_.video_codec);
    std::wstring codec_args = BuildCodecArgs(codec);

    replay_ff_ = hr_ff_create();
    hr_ff_set_ffmpeg_path(replay_ff_, NarrowFromWide(ffmpeg_path_).c_str());
    hr_ff_set_output_path(replay_ff_, NarrowFromWide(replay_dir_ + L"seg_%03d.mp4").c_str());
    hr_ff_set_codec_args(replay_ff_, NarrowFromWide(codec_args).c_str());
    hr_ff_set_video_params(replay_ff_, output_w_, output_h_, state_.target_fps);
    hr_ff_set_output_size(replay_ff_, output_w_, output_h_);
    hr_ff_set_pipe_input(replay_ff_, 1);
    std::string seg_args = "-f segment -segment_time " + std::to_string(kReplaySegmentSec) +
                            " -segment_wrap " + std::to_string(wrap) + " -reset_timestamps 1";
    hr_ff_set_extra_output_args(replay_ff_, seg_args.c_str());

    if (hr_ff_start(replay_ff_) != 0) {
        error_out = L"Failed to start the Instant Replay encoder.";
        HrLog::Error("Instant Replay: ffmpeg segment-writer process didn't start");
        hr_ff_destroy(replay_ff_);
        replay_ff_ = nullptr;
        return false;
    }
    intptr_t stdin_h = hr_ff_get_stdin_handle(replay_ff_);
    if (stdin_h == 0) {
        error_out = L"Failed to start the Instant Replay encoder.";
        HrLog::Error("Instant Replay: ffmpeg stdin pipe handle unavailable");
        hr_ff_kill(replay_ff_);
        hr_ff_destroy(replay_ff_);
        replay_ff_ = nullptr;
        return false;
    }
    hr_pl_set_recording(pipeline_, /*active=*/1, stdin_h);

    // Instant Replay's segment writer above only ever piped raw video -
    // a saved replay clip came out completely silent regardless of the
    // mic/system-audio settings, which from the outside looks the same
    // as "audio and video got saved separately" (here, audio just never
    // existed at all). Keep a rolling window of mic/system audio the same
    // length as the video buffer (+1 segment slack, matching `wrap`
    // above) so SaveReplay() has real audio to mux in instead of none.
    // Gated on the same "audio enabled" setting manual recording uses, and
    // skipped while a manual recording is running (it owns the buffer
    // uncapped from Start() to Stop() - see hr_audio_set_max_buffer_sec()).
    if (state_.audio_out_channels > 0 && !state_.recording) {
        hr_audio_set_max_buffer_sec(wrap * kReplaySegmentSec);
        hr_audio_reset_buffers();
        hr_audio_set_volumes(mic_vol_, sys_vol_, mic_muted_ ? 1 : 0, sys_muted_ ? 1 : 0);
    }
    return true;
}

// Stops+destroys replay_ff_ (waiting briefly so its last, currently-open
// segment gets finalized rather than left mid-write) without touching
// pipeline_ itself - callers decide what the pipeline's recording pipe
// should point at next.
void RecordingController::StopInstantReplayEncoder() {
    if (!replay_ff_) return;
    hr_pl_end_recording_segment(pipeline_, 3000);
    hr_ff_stop_graceful(replay_ff_);
    if (hr_ff_wait(replay_ff_, 3000) != 0 && hr_ff_is_running(replay_ff_)) {
        HrLog::Warn("Instant Replay: segment writer didn't finish gracefully in time - killing it.");
        hr_ff_kill(replay_ff_);
    }
    hr_ff_destroy(replay_ff_);
    replay_ff_ = nullptr;
}

// BUGFIX (preview freezes for a second or two right after clicking
// Start/Record): Start() below pauses a running Instant Replay buffer by
// calling StopInstantReplayEncoder() - which used to be this same
// synchronous function, called directly on whichever thread called
// Start() (the UI thread, via RequestStart()). hr_ff_wait(replay_ff_,
// 3000) there blocks for up to a full 3 seconds waiting for the segment
// writer to finish gracefully, which is exactly the freeze users saw the
// instant they pressed the button. The replay buffer being paused for a
// manual recording doesn't need to be waited on synchronously - it's a
// background feature the user isn't actively looking at output from, and
// losing at most a fraction of a second off its last (in-progress)
// segment is a fine trade for not freezing the UI. Hand the same
// stop/wait/kill/destroy sequence off to a background thread instead
// (tracked via instant_replay_stop_thread_ rather than detached - see
// JoinPendingInstantReplayStop()'s comment), and clear replay_ff_
// immediately so nothing else touches it while that thread finishes up.
void RecordingController::StopInstantReplayEncoderAsync() {
    if (!replay_ff_) return;
    void *h = replay_ff_;
    replay_ff_ = nullptr;
    void *pl = pipeline_;
    // See JoinPendingInstantReplayStop()'s comment (recording_controller.h)
    // - joins whatever the *previous* call to this function started
    // (normally already finished; this is just the same defensive
    // pattern JoinPendingPreviewTeardown() uses) before overwriting the
    // member with this call's thread.
    JoinPendingInstantReplayStop();
    instant_replay_stop_thread_ = std::thread([h, pl]() {
        // BUGFIX (std::terminate() crash a moment after "Instant Replay:
        // segment writer didn't finish gracefully in time - killing it"):
        // this is a bare std::thread with no exception handler anywhere
        // above it on the call stack (tracked/joined now instead of
        // detached - see JoinPendingInstantReplayStop() - but that alone
        // doesn't catch anything thrown *inside* it) - every other bare
        // std::thread lambda in this codebase (capture_loop()/
        // writer_loop() in hr_pipeline.cpp) got wrapped in a try/catch
        // specifically because an uncaught throw in one of these calls
        // std::terminate() and takes the whole app down with it; this one
        // was missed. Whatever the actual trigger (hr_ff_wait()/
        // hr_ff_kill()/hr_ff_destroy() hitting an edge case under load),
        // catching it here turns a full crash into a logged, recoverable
        // failure - same tradeoff those other two threads already made.
        try {
            // See hr_pl_end_recording_segment()'s comment in
            // hr_pipeline.cpp - without this, hr_ff_wait() below can
            // never see the EOF it's waiting for (pipeline_'s pipe is
            // shared with everything else this class does with it), so
            // this branch was hitting the timeout/kill path on basically
            // every call instead of only under genuine load.
            hr_pl_end_recording_segment(pl, 3000);
            hr_ff_stop_graceful(h);
            if (hr_ff_wait(h, 3000) != 0 && hr_ff_is_running(h)) {
                HrLog::Warn("Instant Replay: segment writer didn't finish gracefully in time - killing it.");
                hr_ff_kill(h);
            }
            hr_ff_destroy(h);
        } catch (const std::exception &e) {
            HrLog::Error(std::string("Instant Replay: stopping the segment writer threw (") +
                         e.what() + ") -- this pipeline is stopping instead of crashing the app.");
        } catch (...) {
            HrLog::Error("Instant Replay: stopping the segment writer threw an unknown exception "
                         "-- this pipeline is stopping instead of crashing the app.");
        }
    });
}

bool RecordingController::EnableInstantReplay(std::wstring &error_out) {
    instant_replay_enabled_ = true;
    if (instant_replay_active_) return true; // already running
    if (state_.recording) return true;       // Stop() will start it once the manual recording ends

    if (!ffmpeg_found_) {
        error_out = L"FFmpeg not found.";
        return false;
    }

    int prev_w = capture_w_, prev_h = capture_h_;
    ResolveCaptureSize();

    if (!pipeline_ || capture_w_ != prev_w || capture_h_ != prev_h ||
        pipeline_output_idx_ != capture_output_idx_) {
        JoinPendingInstantReplayStop();
        if (pipeline_) { hr_pl_destroy(pipeline_); pipeline_ = nullptr; }
        JoinPendingPreviewTeardown();
        int pvw = 0, pvh = 0;
        ScaledPreviewSize(pvw, pvh);
        pipeline_ = hr_pl_create(capture_w_, capture_h_, state_.target_fps, /*pipe_fd=*/0,
                                 pvw, pvh, capture_output_idx_);
        pipeline_output_idx_ = capture_output_idx_;
        last_overlays_sent_valid_ = false;
        if (pipeline_) {
            hr_pl_set_capture_rect(pipeline_, crop_x_, crop_y_, crop_w_, crop_h_);
            hr_pl_set_output_size(pipeline_, output_w_, output_h_);
            if (!hr_pl_start(pipeline_)) { hr_pl_destroy(pipeline_); pipeline_ = nullptr; }
        }
    }
    if (!pipeline_) {
        error_out = L"Failed to start the capture pipeline.";
        return false;
    }
    hr_pl_set_preview_fps(pipeline_, state_.preview_fps);
    hr_pl_set_preview_needed(pipeline_, state_.disable_preview ? 0 : 1);

    if (!StartInstantReplayEncoder(error_out)) return false;
    instant_replay_active_ = true;
    HrLog::Info("Instant Replay: buffering started (" + std::to_string(state_.replay_buffer_sec) + "s buffer).");
    return true;
}

void RecordingController::DisableInstantReplay() {
    instant_replay_enabled_ = false;
    if (!instant_replay_active_) return;
    StopInstantReplayEncoder();
    instant_replay_active_ = false;
    if (pipeline_ && !state_.recording) {
        // See JoinPendingInstantReplayStop()'s comment - belt-and-suspenders
        // in case an earlier pause/resume cycle's async stop thread hasn't
        // actually finished (and been joined) yet.
        JoinPendingInstantReplayStop();
        if (state_.disable_preview) { hr_pl_destroy(pipeline_); pipeline_ = nullptr; }
        else hr_pl_set_recording(pipeline_, /*active=*/0, /*pipe_fd=*/0);
    }
    // Instant Replay's rolling audio window (see StartInstantReplayEncoder())
    // was only ever meant to run while the feature is active. If a manual
    // recording is what's actually running right now, leave it alone -
    // that recording owns audio buffering uncapped until its own Stop()
    // (Start() already reconfigured it via hr_audio_set_max_buffer_sec(0)).
    if (!state_.recording) {
        hr_audio_capture_to_wav(nullptr, nullptr); // stop buffering, discard the window
        hr_audio_set_max_buffer_sec(0);
    }
    HrLog::Info("Instant Replay: stopped.");
}

bool RecordingController::SaveReplay(std::wstring &error_out, std::wstring *out_saved_path) {
    if (!instant_replay_active_ || !replay_ff_) {
        error_out = L"Instant Replay isn't running.";
        return false;
    }
    std::wstring dir_to_read = replay_dir_;

    // Stop the current segment writer so its last (possibly still-open)
    // segment gets finalized on disk, then immediately start a fresh one
    // into a new folder - the gap this leaves is in the *background*
    // buffer, not in the clip being saved right now, and starting the
    // replacement first (before the slower concat below) means the
    // background buffer is covering new ground again as soon as possible.
    StopInstantReplayEncoder();
    std::wstring restart_err;
    bool restarted = StartInstantReplayEncoder(restart_err);

    std::vector<std::wstring> segs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir_to_read + L"seg_*.mp4").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { segs.push_back(dir_to_read + fd.cFileName); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (segs.empty()) {
        error_out = L"Instant Replay buffer is empty (nothing captured yet).";
        instant_replay_active_ = restarted;
        if (!restarted) HrLog::Error(NarrowFromWide(L"Instant Replay: failed to resume buffering - " + restart_err));
        return false;
    }

    // segment_wrap reuses filenames (seg_000 can be newer than seg_005
    // once the writer has wrapped around once), so chronological order
    // has to come from last-write-time, not the filename itself.
    std::sort(segs.begin(), segs.end(), [](const std::wstring &a, const std::wstring &b) {
        auto mtime = [](const std::wstring &p) -> uint64_t {
            WIN32_FILE_ATTRIBUTE_DATA d{};
            if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
            return (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
        };
        return mtime(a) < mtime(b);
    });

    int buf_sec = state_.replay_buffer_sec > 0 ? state_.replay_buffer_sec : 30;
    int want = (buf_sec + kReplaySegmentSec - 1) / kReplaySegmentSec;
    if (want < 1) want = 1;
    if ((int)segs.size() > want) segs.erase(segs.begin(), segs.end() - want);

    // Instant Replay's segment writer only ever piped raw video (see
    // StartInstantReplayEncoder()), so a saved clip used to come out
    // completely silent - snapshot the rolling mic/system-audio window
    // that same function now keeps (non-destructively: the buffer keeps
    // accumulating for the *next* Save Replay) and mux it into the clip
    // below, the same mix-then-merge path a manual recording's Stop()
    // uses.
    std::string mic_wav_a = NarrowFromWide(dir_to_read + L"replay_mic_tmp.wav");
    std::string sys_wav_a = NarrowFromWide(dir_to_read + L"replay_sys.wav");
    std::string audio_wav_a = NarrowFromWide(dir_to_read + L"replay_audio.wav");
    bool want_mic = !mic_muted_;
    bool want_sys = !sys_muted_;
    bool have_replay_audio = false;
    if (state_.audio_out_channels > 0 && (want_mic || want_sys)) {
        int ar = hr_audio_snapshot_to_wav(want_mic ? mic_wav_a.c_str() : nullptr,
                                           want_sys ? sys_wav_a.c_str() : nullptr);
        bool mic_written = want_mic && (ar & 0x1) && hr_path_exists(mic_wav_a.c_str());
        bool sys_written = want_sys && (ar & 0x2) && hr_path_exists(sys_wav_a.c_str());
        if (mic_written && sys_written) {
            if (hr_audio_mix_wav(mic_wav_a.c_str(), sys_wav_a.c_str(), audio_wav_a.c_str()) == 0) {
                std::remove(mic_wav_a.c_str());
                std::remove(sys_wav_a.c_str());
            } else {
                std::remove(sys_wav_a.c_str());
                std::rename(mic_wav_a.c_str(), audio_wav_a.c_str());
            }
            have_replay_audio = true;
        } else if (mic_written) {
            std::rename(mic_wav_a.c_str(), audio_wav_a.c_str());
            have_replay_audio = true;
        } else if (sys_written) {
            std::rename(sys_wav_a.c_str(), audio_wav_a.c_str());
            have_replay_audio = true;
        }
    }

    std::wstring list_path = dir_to_read + L"concat_list.txt";
    {
        std::wofstream out(list_path.c_str(), std::ios::binary);
        for (const auto &s : segs) {
            // ffmpeg concat-demuxer lines are single-quoted; escape any
            // literal single quote the format's own way ('\'') - not
            // expected in a temp-folder path, but cheap to handle.
            std::wstring esc = s;
            size_t pos = 0;
            while ((pos = esc.find(L'\'', pos)) != std::wstring::npos) {
                esc.replace(pos, 1, L"'\\''");
                pos += 4;
            }
            out << L"file '" << esc << L"'\n";
        }
    }

    current_output_path_ = BuildOutputPath();
    bool ok = hr_concat_segments(ffmpeg_path_.c_str(), list_path.c_str(),
                                  current_output_path_.c_str()) != 0;

    if (ok && have_replay_audio) {
        // real_elapsed_sec = 0.0 deliberately skips hr_merge_av()'s
        // dropped-frame "stretch" correction (see its own comment) - that
        // logic needs a genuine wall-clock recording duration to compare
        // against, which doesn't apply to an already-concatenated set of
        // fixed-length segments. Just mux the audio in at the fast
        // stream-copy path.
        if (hr_merge_av(ffmpeg_path_.c_str(), current_output_path_.c_str(),
                         WideFromNarrow(audio_wav_a).c_str(), 0.0) != 0) {
            std::remove(audio_wav_a.c_str());
        } else {
            HrLog::Error("Instant Replay: merging the captured audio into the saved replay "
                         "failed -- keeping '" + audio_wav_a + "' next to the (silent) clip "
                         "instead of losing the audio entirely.");
        }
    }

    instant_replay_active_ = restarted;
    if (!restarted) {
        HrLog::Error(NarrowFromWide(L"Instant Replay: failed to resume buffering after Save Replay - " + restart_err));
    }

    if (!ok) {
        error_out = L"Couldn't save the replay (ffmpeg concat failed).";
        HrLog::Error("Instant Replay: hr_concat_segments failed");
        return false;
    }
    if (out_saved_path) *out_saved_path = current_output_path_;
    HrLog::Info("Instant Replay: saved -> " + NarrowFromWide(current_output_path_));
    return true;
}

bool RecordingController::GetPreviewFrame(std::vector<uint8_t> &out, int &out_w, int &out_h) {
    if (!pipeline_) return false;
    out.resize((size_t)state_.preview_width * state_.preview_height * 3);
    return hr_pl_get_preview(pipeline_, out.data(), &out_w, &out_h) != 0;
}

void RecordingController::EnsurePreview() {
    if (pipeline_ || state_.disable_preview) return; // already running, or user turned it off
    // See JoinPendingPreviewTeardown()'s comment (recording_controller.h) -
    // without this, creating a new pipeline right after TeardownPreview()
    // kicked off an async destroy of the old one could race its DXGI
    // duplication handle and silently fail to start.
    JoinPendingPreviewTeardown();
    ResolveCaptureSize();
    // pipe_fd=0 -> hr_pl_create() leaves this in preview-only mode (frames
    // captured + thumbnailed for the UI, nothing written anywhere) - see
    // its "false -> preview only" comment in hr_pipeline.cpp. Start()
    // later flips this same pipeline into recording mode via
    // hr_pl_set_recording() instead of replacing it, when the size matches.
    int pvw = 0, pvh = 0;
    ScaledPreviewSize(pvw, pvh);
    pipeline_ = hr_pl_create(capture_w_, capture_h_, state_.target_fps, /*pipe_fd=*/0,
                             pvw, pvh, capture_output_idx_);
    pipeline_output_idx_ = capture_output_idx_;
    last_overlays_sent_valid_ = false;
    if (pipeline_ && !hr_pl_start(pipeline_)) {
        hr_pl_destroy(pipeline_);
        pipeline_ = nullptr;
    }
    if (!pipeline_) {
        // This used to fail completely silently - no error, no
        // status change, nothing - so a stuck DXGI capture (dx_create()
        // returning null: RDP, a virtual display, a display mode that
        // just changed) just looked like a frozen preview forever, with
        // SyncOverlays() hammering it again every 2s regardless. Track
        // the failure streak here so SyncOverlays() can back off and,
        // past a few failures, flag preview_capture_unavailable() so the
        // UI can show something more honest than "still loading".
        if (preview_retry_streak_ < 1'000'000) ++preview_retry_streak_; // don't overflow if this runs for days
        if (preview_retry_streak_ >= kPreviewRetryBackoffAfter && !preview_unavailable_) {
            preview_unavailable_ = true;
            HrLog::Warn("Preview: capture pipeline has failed to start " +
                        std::to_string(preview_retry_streak_) +
                        " times in a row - backing off retries. Common causes: "
                        "running over RDP/a virtual display, a just-changed display "
                        "mode, or insufficient permissions.");
        }
        return;
    }
    // Reached only on success - clear any backoff state from earlier
    // failures so the next time preview needs to (re)start (settings
    // change, monitor reconnected, etc.) it retries at the fast cadence
    // again instead of staying artificially slow.
    preview_retry_streak_ = 0;
    preview_unavailable_ = false;
    // Same crop rect ResolveCaptureSize() above just resolved for Start()
    // to reuse - without this, the live preview panel would keep showing
    // the full desktop even when a window is selected, only "correcting"
    // itself once Start() reuses this pipeline and re-applies the crop.
    hr_pl_set_capture_rect(pipeline_, crop_x_, crop_y_, crop_w_, crop_h_);
    hr_pl_set_preview_fps(pipeline_, state_.preview_fps);
    // EnsurePreview() already bailed out above when state_.disable_preview
    // is set, so reaching here means the preview genuinely is wanted.
    hr_pl_set_preview_needed(pipeline_, 1);
}

void RecordingController::TeardownPreview() {
    // Never tear down while an actual recording owns this pipeline -
    // Stop() is what decides whether to keep or destroy it for a running
    // recording, based on the same state_.disable_preview flag.
    if (!pipeline_ || state_.recording) return;

    void *doomed = pipeline_;
    pipeline_ = nullptr;

    // If an earlier teardown is still in flight (its own stuck-DXGI 1s x2
    // timeout hasn't elapsed yet), wait for it here rather than detaching a
    // second one - two of these racing on unrelated Pipeline objects is
    // harmless in itself, but it's needless overlap and makes it easy to
    // lose track of one at shutdown. This only blocks the caller for the
    // (rare) remainder of the previous teardown, not a fresh one.
    if (preview_teardown_thread_.joinable()) preview_teardown_thread_.join();
    // Wrapped in try/catch for the same reason as the thread
    // entry points in hr_pipeline.cpp/hr_audio.cpp (see their matching
    // comments) - this lambda is a bare std::thread with nothing above it
    // to catch an uncaught exception out of hr_pl_destroy(), which would
    // otherwise be another std::terminate() path.
    preview_teardown_thread_ = std::thread([doomed]() {
        try {
            hr_pl_destroy(doomed);
        } catch (const std::exception &e) {
            HrLog::Error(std::string("Preview teardown thread: uncaught exception (") + e.what() + ")");
        } catch (...) {
            HrLog::Error("Preview teardown thread: uncaught unknown exception");
        }
    });
}

bool RecordingController::StartQuickTest(const std::string &codec_in, const std::string &preset,
                                          int duration_sec, std::wstring &error_out) {
    if (state_.recording) {
        error_out = L"Can't run a test while a recording is already in progress.";
        return false;
    }
    if (qt_running_) {
        error_out = L"A test is already running.";
        return false;
    }
    if (!ffmpeg_found_) {
        error_out = L"FFmpeg not found.";
        return false;
    }

    // Resolve the selected monitor exactly like ResolveCaptureSize()'s
    // desktop-capture branch (same state_.monitor_id -1 convention), but
    // kept entirely local - this must NOT touch capture_w_/h_/output_w_/
    // h_/capture_output_idx_, since those belong to the real recording/
    // preview pipeline_ and a test can run without a real recording ever
    // starting.
    void *di = hr_di_create();
    hr_di_refresh(di);
    int mx = 0, my = 0, mw = 1920, mh = 1080;
    float dpi = 96.0f;
    int idx = state_.monitor_id > 0 ? state_.monitor_id - 1 : 0;
    if (!hr_di_get(di, idx, &mx, &my, &mw, &mh, &dpi)) {
        idx = 0;
        hr_di_primary(di, &mx, &my, &mw, &mh, &dpi);
    }
    hr_di_destroy(di);
    if (mw <= 0 || mh <= 0) { mw = 1920; mh = 1080; }
    if (mw % 2) mw--;
    if (mh % 2) mh--;
    qt_w_ = mw;
    qt_h_ = mh;

    // DXGI Desktop Duplication only allows one active duplication handle
    // per monitor at a time - if the live preview is currently capturing
    // this same monitor, creating a second pipeline for the test would
    // just fail. Tear it down for the test's duration (JoinPendingPreviewTeardown()
    // makes sure that's actually finished, not just kicked off, before the
    // create below runs); FinishQuickTest() brings it back.
    qt_preview_was_torn_down_ = (pipeline_ != nullptr);
    TeardownPreview();
    JoinPendingPreviewTeardown();

    wchar_t tmp_dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp_dir);
    qt_output_path_ = std::wstring(tmp_dir) + L"homrec_test_" +
                       std::to_wstring(GetTickCount()) + L".mp4";

    std::wstring codec = codec_in == "libx264" && !hw_encoder_.empty()
                              ? hw_encoder_
                              : WideFromNarrow(codec_in);
    qt_encoder_used_ = NarrowFromWide(codec);

    // Same args-building hr_build_codec_args() call BuildCodecArgs() makes,
    // just with the Settings dialog's live (possibly-unsaved) preset
    // instead of state_.enc_preset - BuildCodecArgs() itself isn't
    // parameterized on preset, so it's inlined here rather than mutating
    // state_ (which a background timer/preview tick could be reading
    // concurrently) to borrow it temporarily.
    std::wstring codec_args;
    if (!state_.custom_ffmpeg_args.empty()) {
        codec_args = L"-c:v " + codec + L" " + WideFromNarrow(state_.custom_ffmpeg_args);
    } else {
        wchar_t buf[512] = {};
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        std::wstring wpreset = WideFromNarrow(preset);
        hr_build_codec_args(codec.c_str(), state_.quality, state_.target_fps,
                             (int)si.dwNumberOfProcessors, buf, 512, wpreset.c_str());
        codec_args = buf;
    }

    qt_ffproc_ = hr_ff_create();
    hr_ff_set_ffmpeg_path(qt_ffproc_, NarrowFromWide(ffmpeg_path_).c_str());
    hr_ff_set_output_path(qt_ffproc_, NarrowFromWide(qt_output_path_).c_str());
    hr_ff_set_codec_args(qt_ffproc_, NarrowFromWide(codec_args).c_str());
    hr_ff_set_video_params(qt_ffproc_, mw, mh, state_.target_fps);
    hr_ff_set_output_size(qt_ffproc_, mw, mh);
    hr_ff_set_pipe_input(qt_ffproc_, 1);
    if (hr_ff_start(qt_ffproc_) != 0) {
        error_out = L"Failed to start the ffmpeg process for the test.";
        HrLog::Error("Quick test: ffmpeg process didn't start");
        hr_ff_destroy(qt_ffproc_);
        qt_ffproc_ = nullptr;
        if (qt_preview_was_torn_down_) EnsurePreview();
        return false;
    }

    intptr_t ff_stdin = hr_ff_get_stdin_handle(qt_ffproc_);
    if (ff_stdin == 0) {
        error_out = L"Failed to start the ffmpeg process for the test.";
        HrLog::Error("Quick test: ffmpeg stdin pipe handle unavailable");
        hr_ff_kill(qt_ffproc_);
        hr_ff_destroy(qt_ffproc_);
        qt_ffproc_ = nullptr;
        if (qt_preview_was_torn_down_) EnsurePreview();
        return false;
    }

    qt_pipeline_ = hr_pl_create(mw, mh, state_.target_fps, ff_stdin, 0, 0, idx);
    if (!qt_pipeline_ || !hr_pl_start(qt_pipeline_)) {
        error_out = L"Failed to start the capture pipeline for the test.";
        HrLog::Error("Quick test: capture pipeline didn't start");
        hr_ff_kill(qt_ffproc_);
        hr_ff_destroy(qt_ffproc_);
        qt_ffproc_ = nullptr;
        if (qt_pipeline_) { hr_pl_destroy(qt_pipeline_); qt_pipeline_ = nullptr; }
        if (qt_preview_was_torn_down_) EnsurePreview();
        return false;
    }
    hr_pl_set_recording(qt_pipeline_, /*active=*/1, ff_stdin);

    qt_fps_sum_ = 0.0;
    qt_fps_samples_ = 0;
    qt_last_drops_ = 0;
    qt_overload_streak_ = 0;
    qt_overloaded_ = false;
    qt_frames_ = 0;
    qt_drops_ = 0;
    if (duration_sec < 1) duration_sec = 1;
    qt_end_time_ = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
    qt_running_ = true;
    HrLog::Info("Quick test: recording " + std::to_string(duration_sec) + "s at " +
                std::to_string(mw) + "x" + std::to_string(mh) + " with " +
                NarrowFromWide(codec));
    return true;
}

bool RecordingController::PollQuickTest() {
    if (!qt_running_) return false;

    long long frames = 0, drops = 0;
    double fps = 0.0;
    if (qt_pipeline_) hr_pl_stats(qt_pipeline_, &frames, &drops, &fps);
    qt_frames_ = frames;
    qt_drops_ = drops;
    if (fps > 0.0) { qt_fps_sum_ += fps; ++qt_fps_samples_; }

    long long delta = drops - qt_last_drops_;
    qt_last_drops_ = drops;
    if (delta > 0) {
        if (qt_overload_streak_ < 3) ++qt_overload_streak_;
    } else {
        qt_overload_streak_ = 0;
    }
    if (qt_overload_streak_ >= 3) qt_overloaded_ = true;

    if (std::chrono::steady_clock::now() >= qt_end_time_) {
        qt_running_ = false;
        return false;
    }
    return true;
}

RecordingController::QuickTestResult RecordingController::FinishQuickTest() {
    QuickTestResult result;
    result.width = qt_w_;
    result.height = qt_h_;
    result.frames = qt_frames_;
    result.drops = qt_drops_;
    result.avg_fps = qt_fps_samples_ > 0 ? qt_fps_sum_ / qt_fps_samples_ : 0.0;
    result.encoder_used = qt_encoder_used_;

    // qt_overloaded_ (PollQuickTest) only fires on a *sustained* rise --
    // three consecutive 200ms polls where the drop count went up. A codec
    // that's hopelessly behind for this machine usually falls behind hard
    // once (a single big catch-up burst that adds dozens of dropped/
    // duplicated frames between two polls) and then goes quiet because
    // there's nothing left to catch up on, or the pipeline simply can't
    // produce more - that shows up as one poll with a huge delta
    // surrounded by flat ones, which never reaches a streak of 3 and so
    // never sets qt_overloaded_, even though the codec is clearly not
    // fine (see: a test reporting 242 of 317 frames dropped and still
    // claiming "no sustained frame drops"). Back the verdict with the
    // actual drop ratio too, so a single catastrophic burst can't hide
    // behind the streak check.
    const bool ratio_bad = result.frames > 0 &&
        (double)result.drops / (double)result.frames > 0.15;
    result.overloaded = qt_overloaded_ || ratio_bad;

    if (qt_pipeline_) {
        hr_pl_stop(qt_pipeline_);
        hr_pl_destroy(qt_pipeline_);
        qt_pipeline_ = nullptr;
    }
    if (qt_ffproc_) {
        // Same generous wait Stop()'s finalize tail gives a real
        // recording (see StopFinalizeTail()'s comment) would be overkill
        // for a few seconds of test footage that's about to be deleted
        // anyway - a shorter budget before force-stopping is fine here.
        hr_ff_stop_graceful(qt_ffproc_);
        if (hr_ff_wait(qt_ffproc_, 5000) != 0) {
            HrLog::Warn("Quick test: ffmpeg didn't finish on its own after 5s - stopping it.");
        }
        hr_ff_destroy(qt_ffproc_);
        qt_ffproc_ = nullptr;
    }
    // Disposable by design - it was never meant to be kept, regardless
    // of whether it finalized cleanly.
    if (!qt_output_path_.empty()) {
        DeleteFileW(qt_output_path_.c_str());
        qt_output_path_.clear();
    }
    qt_running_ = false;

    if (qt_preview_was_torn_down_) {
        qt_preview_was_torn_down_ = false;
        EnsurePreview();
    }
    return result;
}

void RecordingController::ResetPreviewRetryState() {
    preview_retry_streak_ = 0;
    preview_unavailable_ = false;
    next_preview_retry_ = std::chrono::steady_clock::now();
}

void RecordingController::RefreshPreviewSettings() {
    PreviewCaptureSettings now;
    now.disable_preview = state_.disable_preview;
    now.monitor_id = state_.monitor_id;
    now.capture_mode = state_.capture_mode;
    now.capture_window_title = state_.capture_window_title;
    now.target_fps = state_.target_fps;
    now.preview_width = state_.preview_width;
    now.preview_height = state_.preview_height;
    now.preview_quality_pct = state_.preview_quality_pct;

    if (!applied_preview_capture_settings_valid_ || !(now == applied_preview_capture_settings_)) {
        TeardownPreview();
        // Re-enabling preview (Settings > "Disable live preview"
        // unchecked) used to just call EnsurePreview() and leave
        // preview_retry_streak_/next_preview_retry_ exactly as they were.
        // Those only ever get cleared on a *successful* EnsurePreview(), so
        // if preview had already backed off before being disabled (DXGI
        // hiccup, RDP, etc. - see EnsurePreview()'s comment), turning it
        // back on inherited that same escalated backoff. If this explicit,
        // user-initiated attempt then also failed just once (e.g. the
        // display hadn't quite settled yet), SyncOverlays()'s automatic
        // retry wouldn't fire again for up to kPreviewRetryMaxSeconds - the
        // preview panel just sits on "Preview loading..." for a long
        // stretch, looking stuck, until something unrelated (minimizing/
        // restoring the window) calls EnsurePreview() directly again and
        // happens to catch it after the real issue cleared. Always start
        // an explicit re-enable with a clean slate instead, so a failure
        // here retries at the fast base cadence, not wherever the old
        // backoff had escalated to.
        ResetPreviewRetryState();
        EnsurePreview();
        applied_preview_capture_settings_ = now;
        applied_preview_capture_settings_valid_ = true;
    }
    if (pipeline_) {
        hr_pl_set_preview_fps(pipeline_, state_.preview_fps);
        // Covers the case TeardownPreview()/EnsurePreview() above don't:
        // a recording already in progress owns pipeline_ and keeps it
        // regardless of this setting (see TeardownPreview()'s own "never
        // tear down while state_.recording" guard), so toggling "Disable
        // live preview" mid-recording needs to reach the existing
        // pipeline directly rather than only affecting the next preview-
        // only pipeline EnsurePreview() would create.
        hr_pl_set_preview_needed(pipeline_, state_.disable_preview ? 0 : 1);
    }

    // Picking a different microphone in Settings had no effect
    // until the app was restarted -- hr_audio_start() only ever ran once,
    // at Init(), with whatever mic_device_id was set at the time. Restart
    // the continuous capture stream (level meters, not an actual recording)
    // with the newly-chosen device instead. Skipped while a recording is
    // in progress -- tearing down/reopening the WASAPI stream mid-recording
    // would risk a gap or a hang, and there's no urgency to apply it before
    // the current recording finishes anyway.
    if (!state_.recording && state_.mic_device_id != applied_mic_device_id_) {
        hr_audio_stop(nullptr, nullptr);
        hr_audio_start(1.0f, 1.0f, mic_muted_ ? 1 : 0, sys_muted_ ? 1 : 0,
                        WideFromNarrow(state_.mic_device_id).c_str());
        applied_mic_device_id_ = state_.mic_device_id;
    }
}

void RecordingController::SetPreviewVisible(bool visible) {
    if (state_.recording) return; // recording owns the pipeline until Stop()
    if (visible) {
        // Used to call EnsurePreview() directly here and nothing else.
        // If the preview had already backed off before being hidden (DXGI
        // hiccup, RDP session, no monitor - see EnsurePreview()'s
        // comment), TeardownPreview() below never touches
        // preview_retry_streak_/next_preview_retry_, so this call
        // inherited whatever escalated backoff was already in effect. If
        // THIS attempt (e.g. right as the window un-hides, before the
        // display's had a moment to settle) also failed, the panel could
        // sit missing/on "Preview loading..." for up to
        // kPreviewRetryMaxSeconds after a tray restore, since
        // SyncOverlays()'s automatic retry wouldn't fire again until
        // next_preview_retry_ - a stale schedule from before the restore,
        // unrelated to when the window actually came back. Restoring
        // visibility is exactly as deliberate/user-initiated as toggling
        // the Settings checkbox RefreshPreviewSettings() already resets
        // this for, so it gets the same clean slate.
        ResetPreviewRetryState();
        EnsurePreview();
    } else {
        TeardownPreview();
    }
}

bool RecordingController::CaptureSnapshotFrame(std::vector<uint8_t> &out, int &out_w, int &out_h,
                                                int &native_w, int &native_h, bool first_call) {
    // This used to be gated on `first_call` too, so if the very
    // first attempt of an editing session failed to get a pipeline going
    // (EnsurePreview() briefly stuck right after e.g. a tray restore),
    // pipeline_ stayed null forever and every later Refresh - always
    // called with first_call=false - hit the `if (!pipeline_) return
    // false;` below without ever trying EnsurePreview() again. Refresh
    // now retries the start on every call, not just the first.
    bool just_started = false;
    if (!pipeline_) {
        // EnsurePreview() no-ops when state_.disable_preview is set - this
        // is an explicit "show me a screenshot anyway" request, so start
        // it regardless, same as SetPreviewVisible() would if the setting
        // were off. Left running afterward; EndSnapshotEditing() is what
        // decides whether to tear it back down once the user's done here.
        bool was_disabled = state_.disable_preview;
        state_.disable_preview = false;
        EnsurePreview();
        state_.disable_preview = was_disabled;
        just_started = true;
    }
    if (!pipeline_) return false;

    // A freshly-started pipeline's capture thread needs a moment to
    // produce its first thumbnail (hr_pl_get_preview() returns false
    // until then) - worth waiting out whenever EnsurePreview() just (re)ran
    // above, not only on the session's first call; an already-running
    // pipeline from an earlier call should already have one available
    // immediately.
    const int max_wait_ms = (first_call || just_started) ? 1000 : 0;
    const int step_ms = 25;
    for (int waited = 0; ; waited += step_ms) {
        if (GetPreviewFrame(out, out_w, out_h)) {
            if (!hr_pl_get_native_size(pipeline_, &native_w, &native_h)) {
                native_w = out_w;
                native_h = out_h;
            }
            return true;
        }
        if (waited >= max_wait_ms) return false;
        Sleep(step_ms);
    }
}

void RecordingController::EndSnapshotEditing() {
    if (state_.disable_preview) TeardownPreview();
}

double RecordingController::elapsed_seconds() const {
    return hr_ctl_get_elapsed_sec(ctl_);
}

std::wstring RecordingController::elapsed_formatted() const {
    char buf[16] = {};
    hr_ctl_format_elapsed(ctl_, buf, 16);
    return WideFromNarrow(buf);
}

double RecordingController::output_size_mb() const {
    return ffproc_ ? hr_ff_output_size_mb(ffproc_) : 0.0;
}

std::wstring RecordingController::last_duration_formatted() const {
    int total = (int)last_duration_sec_;
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    wchar_t buf[16];
    swprintf(buf, 16, L"%02d:%02d:%02d", h, m, s);
    return std::wstring(buf);
}

int RecordingController::frame_count() const {
    return hr_ctl_get_frame_count(ctl_);
}