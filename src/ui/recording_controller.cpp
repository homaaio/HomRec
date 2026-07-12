#include "recording_controller.h"
#include <vector>
#include <thread>

extern "C" {
    // hr_tools.cpp (wide-string API)
    int hr_check_ffmpeg(const wchar_t *hint, wchar_t *out, int out_len);
    int hr_probe_gpu(const wchar_t *ffpath, wchar_t *out_enc, int out_len);
    int hr_build_codec_args(const wchar_t *codec, int quality, int fps, int cpu_count,
                             wchar_t *out_buf, int buf_chars);
    int hr_merge_av(const wchar_t *ffpath, const wchar_t *video_file, const wchar_t *audio_file);

    // hr_ui_utils.cpp (narrow-string API — see README audit note: the core
    // is split between wide- and narrow-string exports depending on which
    // file it landed in; this class just calls each the way it expects).
    void hr_filename_from_template(const char *tmpl, const char *folder, char *out, int out_len);
    int hr_make_output_dir(const char *path);
    int hr_path_exists(const char *path);
    float hr_file_size_mb(const char *path);

    // hr_display_info.cpp
    void *hr_di_create();
    void hr_di_destroy(void *handle);
    void hr_di_refresh(void *handle);
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
    void *hr_pl_create(int w, int h, int fps, int pipe_fd, int pv_w, int pv_h);
    void hr_pl_destroy(void *handle);
    int hr_pl_start(void *handle);
    void hr_pl_stop(void *handle);
    void hr_pl_pause(void *handle, int flag);
    int hr_pl_get_preview(void *handle, unsigned char *out_rgb, int *out_w, int *out_h);
    void hr_pl_stats(void *handle, long long *out_frames, long long *out_drops, double *out_fps);

    // hr_ffmpeg_runner.cpp
    void *hr_ff_create();
    void hr_ff_destroy(void *handle);
    void hr_ff_set_ffmpeg_path(void *h, const char *path);
    void hr_ff_set_output_path(void *h, const char *path);
    void hr_ff_set_codec_args(void *h, const char *args);
    void hr_ff_set_video_params(void *h, int w, int h2, int fps);
    void hr_ff_set_pipe_input(void *h, int enable);
    int hr_ff_start(void *handle);
    int hr_ff_stop_graceful(void *handle);
    int hr_ff_wait(void *handle, int timeout_ms);
    int hr_ff_is_running(const void *handle);
    double hr_ff_output_size_mb(const void *handle);
    void hr_ff_kill(void *handle);

    // hr_audio.cpp
    int hr_audio_init();
    int hr_audio_start(float mic_vol, float sys_vol, int mic_mute, int sys_mute);
    void hr_audio_set_volumes(float mic_vol, float sys_vol, int mic_mute, int sys_mute);
    void hr_audio_get_levels(int *out_mic, int *out_sys);
    void hr_audio_pause(int paused);
    int hr_audio_stop(const char *mic_wav_path, const char *sys_wav_path);
}

namespace {
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
    if (ctl_) hr_ctl_destroy(ctl_);
    if (ffproc_) hr_ff_destroy(ffproc_);
    if (pipeline_) hr_pl_destroy(pipeline_);
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
            }
        }
    }
    hr_audio_init();
}

std::wstring RecordingController::BuildCodecArgs(const std::wstring &codec) {
    wchar_t buf[512] = {};
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    hr_build_codec_args(codec.c_str(), state_.quality, state_.target_fps,
                         (int)si.dwNumberOfProcessors, buf, 512);
    return buf;
}

std::wstring RecordingController::BuildOutputPath() {
    char buf[256] = {};
    hr_filename_from_template(state_.filename_template.c_str(), state_.output_folder.c_str(), buf, 256);
    return WideFromNarrow(buf);
}

bool RecordingController::Start(std::wstring &error_out) {
    if (state_.recording) { error_out = L"Already recording."; return false; }

    if (!hr_path_exists(state_.output_folder.c_str())) {
        if (!hr_make_output_dir(state_.output_folder.c_str())) {
            error_out = L"Output folder doesn't exist and couldn't be created.";
            return false;
        }
    }
    if (!ffmpeg_found_) {
        error_out = L"FFmpeg not found.";
        return false;
    }

    current_output_path_ = BuildOutputPath();

    // Resolve real capture resolution from the selected monitor — this was
    // the "w=0, h=0" placeholder flagged as a known gap when Phase 3 first
    // shipped. state_.monitor_id is 1-based (matches the Settings dialog's
    // "Monitor:" field); hr_di_get is 0-based, hence the -1.
    void *di = hr_di_create();
    hr_di_refresh(di);
    int mx = 0, my = 0, mw = 1920, mh = 1080;
    float dpi = 96.0f;
    int idx = state_.monitor_id > 0 ? state_.monitor_id - 1 : 0;
    if (!hr_di_get(di, idx, &mx, &my, &mw, &mh, &dpi)) {
        hr_di_primary(di, &mx, &my, &mw, &mh, &dpi); // fall back to primary if the index is out of range
    }
    hr_di_destroy(di);
    state_.monitor_left = mx;
    state_.monitor_top = my;
    capture_w_ = (int)(mw * state_.scale_factor);
    capture_h_ = (int)(mh * state_.scale_factor);
    // Most encoders choke on odd dimensions (yuv420p needs even w/h).
    if (capture_w_ % 2) capture_w_--;
    if (capture_h_ % 2) capture_h_--;

    // Prefer the probed GPU encoder; fall back to libx264 if none/if the
    // caller already forced a specific codec in settings.
    std::wstring codec = state_.video_codec == "libx264" && !hw_encoder_.empty()
                              ? hw_encoder_
                              : WideFromNarrow(state_.video_codec);
    std::wstring codec_args = BuildCodecArgs(codec);

    ffproc_ = hr_ff_create();
    hr_ff_set_ffmpeg_path(ffproc_, NarrowFromWide(ffmpeg_path_).c_str());
    hr_ff_set_output_path(ffproc_, NarrowFromWide(current_output_path_).c_str());
    hr_ff_set_codec_args(ffproc_, NarrowFromWide(codec_args).c_str());
    hr_ff_set_video_params(ffproc_, capture_w_, capture_h_, state_.target_fps);
    hr_ff_set_pipe_input(ffproc_, 1);

    if (!hr_ff_start(ffproc_)) {
        error_out = L"Failed to start the ffmpeg process.";
        hr_ff_destroy(ffproc_);
        ffproc_ = nullptr;
        return false;
    }

    // Pipeline handles the actual DXGI capture + frame conversion + piping
    // frames into ffmpeg's stdin. pv_w/pv_h come from AppState's preview
    // panel size (set by main_window on layout).
    pipeline_ = hr_pl_create(capture_w_, capture_h_, state_.target_fps, /*pipe_fd=*/1,
                             state_.preview_width, state_.preview_height);
    if (!pipeline_ || !hr_pl_start(pipeline_)) {
        error_out = L"Failed to start the capture pipeline.";
        hr_ff_kill(ffproc_);
        hr_ff_destroy(ffproc_);
        ffproc_ = nullptr;
        if (pipeline_) { hr_pl_destroy(pipeline_); pipeline_ = nullptr; }
        return false;
    }

    if (state_.audio_out_channels > 0) {
        hr_audio_start(1.0f, 1.0f, 0, 0);
    }

    hr_ctl_set_output_path(ctl_, NarrowFromWide(current_output_path_).c_str());
    hr_ctl_start(ctl_);

    state_.recording = true;
    state_.paused = false;
    state_.frame_count = 0;
    return true;
}

void RecordingController::Stop() {
    if (!state_.recording) return;

    hr_pl_stop(pipeline_);
    hr_ff_stop_graceful(ffproc_);
    hr_ff_wait(ffproc_, 3000);

    std::string mic_wav, sys_wav;
    int audio_result = hr_audio_stop(nullptr, nullptr); // paths TBD by settings (merged vs separate) — Phase 3.1
    (void)audio_result;

    hr_ctl_stop(ctl_);

    hr_pl_destroy(pipeline_);
    pipeline_ = nullptr;
    hr_ff_destroy(ffproc_);
    ffproc_ = nullptr;

    state_.recording = false;
    state_.paused = false;
}

void RecordingController::TogglePause() {
    if (!state_.recording) return;
    int new_state = hr_ctl_pause_toggle(ctl_);
    state_.paused = (new_state == 2 /* HR_STATE_PAUSED */);
    if (pipeline_) hr_pl_pause(pipeline_, state_.paused ? 1 : 0);
}

void RecordingController::PollStats() {
    if (!state_.recording) return;
    long long frames = 0, drops = 0;
    double fps = 0.0;
    if (pipeline_) hr_pl_stats(pipeline_, &frames, &drops, &fps);
    state_.frame_count = (long)frames;

    double size_mb = ffproc_ ? hr_ff_output_size_mb(ffproc_) : 0.0;
    hr_ctl_update_stats(ctl_, (long long)(size_mb * 1024.0 * 1024.0));

    hr_audio_get_levels(&mic_level_, &sys_level_);
}

bool RecordingController::GetPreviewFrame(std::vector<uint8_t> &out, int &out_w, int &out_h) {
    if (!pipeline_) return false;
    out.resize((size_t)state_.preview_width * state_.preview_height * 3);
    return hr_pl_get_preview(pipeline_, out.data(), &out_w, &out_h) != 0;
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

int RecordingController::frame_count() const {
    return hr_ctl_get_frame_count(ctl_);
}
