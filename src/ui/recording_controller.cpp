#include "recording_controller.h"
#include "window_picker_dialog.h"  // HR_ResolveCaptureWindow()
#include "../core/wgc_capture.h"      // 2.4: HrWgcSupported()/HrWgcProbeWindow()
#include "../utils/log.h"
#include "../utils/wgc_log.h"      // 7th pass: logs\wgc.log
#include "../utils/log_paths.h"    // logs\ffmpeg_ddagrab.log (+ progress file)
#include "../core/overlay_render.h"
#include "../utils/str_convert.h"   // HrVideoFormatExt() (2.4 container choice)
#include "../core/mf_encoder.h"      // in-process Media Foundation encoder (AppState::mf_encoder)
#include <cstdlib>                    // atoi() - AAC bitrate setting
#include <windows.h>  // Sleep() - CaptureSnapshotFrame()'s short wait for the first frame; also
                      // QueryFullProcessImageNameA (kernel32) - ResolveCaptureAppName()'s {app} lookup
#include <shellapi.h> // ShellExecuteW - RunPostRecordHook()'s "open_folder" mode
#include <vector>
#include <thread>
#include <exception>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <cstdio>     // FILE / _wfopen - SaveReplay()'s concat list
#include <functional>  // Start()'s Instant Replay resume-on-failure guard

extern "C" {
    // tools.cpp (wide-string API)
    int hr_check_ffmpeg(const wchar_t *hint, wchar_t *out, int out_len);
    int hr_probe_gpu(const wchar_t *ffpath, wchar_t *out_enc, int out_len);
    int hr_probe_ddagrab(const wchar_t *ffpath);
    int hr_build_codec_args(const wchar_t *codec, int quality, int fps, int cpu_count,
                             wchar_t *out_buf, int buf_chars, const wchar_t *preset_override);
    int hr_build_codec_args_ex(const wchar_t *codec, int quality, int fps, int cpu_count,
                                int enc_w, int enc_h,
                                wchar_t *out_buf, int buf_chars, const wchar_t *preset_override);
    int hr_merge_av(const wchar_t *ffpath, const wchar_t *video_file, const wchar_t *audio_file,
                     double real_elapsed_sec, double av_start_skew_sec);
    // Same as hr_merge_av, plus (mp4_copy_path != null) writes the .mp4 copy in the same ffmpeg pass.
    int hr_merge_av_ex(const wchar_t *ffpath, const wchar_t *video_file, const wchar_t *audio_file,
                        double real_elapsed_sec, double av_start_skew_sec,
                        const wchar_t *mp4_copy_path, int *mp4_done);
    int hr_export_mp3(const wchar_t *ffpath, const wchar_t *wav_path, const wchar_t *mp3_path);
    int hr_concat_segments(const wchar_t *ffpath, const wchar_t *list_path, const wchar_t *out_path);
    // 2.4
    int hr_remux_copy(const wchar_t *ffpath, const wchar_t *in_path, const wchar_t *out_path);
    int hr_decode_audio_to_wav(const wchar_t *ffpath, const wchar_t *in_path, const wchar_t *wav_path);

    // ui_utils.cpp (narrow-string API - see README audit note: the core
    // is split between wide- and narrow-string exports depending on which
    // file it landed in; this class just calls each the way it expects).
    void hr_filename_from_template(const char *tmpl, const char *folder, const char *app_name, char *out, int out_len,
                                    const char *preset_name);
    void hr_filename_from_template_ex(const char *tmpl, const char *folder, const char *app_name, char *out, int out_len,
                                       const char *preset_name, const char *ext);
    int hr_make_output_dir(const char *path);
    int hr_path_exists(const char *path);
    float hr_file_size_mb(const char *path);
    int hr_get_free_disk_mb(const char *path, uint64_t *out_free_mb);

    // display_info.cpp
    void *hr_di_create();
    void hr_di_destroy(void *handle);
    void hr_di_refresh(void *handle);
    int hr_di_count(void *handle);
    int hr_di_get(void *handle, int index, int *x, int *y, int *w, int *h, float *dpi);
    int hr_di_primary(void *handle, int *x, int *y, int *w, int *h, float *dpi);

    // dxgi_capture.cpp - ResolveCaptureSize() maps the chosen monitor
    // (EnumDisplayMonitors order, see display_info.cpp) onto the DXGI
    // output index that actually duplicates that same monitor.
    int hr_dx_output_count(int adapter_idx);
    int hr_dx_output_desc(int adapter_idx, int output_idx, int *out_x, int *out_y,
                           int *out_w, int *out_h, char *name_buf, int name_buf_len);

    // capture_ctl.cpp
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

    // pipeline.cpp
    void *hr_pl_create(int w, int h, int fps, intptr_t pipe_fd, int pv_w, int pv_h, int output_idx);
    // 2.4: one window via Windows.Graphics.Capture (nullptr if it can't be captured that way)
    void *hr_pl_create_window(int w, int h, int fps, intptr_t pipe_fd, int pv_w, int pv_h,
                              void *hwnd, int client_only, int draw_cursor, int smart, int fps_cap);
    // 7th pass: follow another window handle without rebuilding the pipeline / what the source is doing now
    int hr_pl_window_retarget(void *handle, void *hwnd);
    int hr_pl_window_mode(void *handle);
    void hr_pl_destroy(void *handle);
    int hr_pl_start(void *handle);
    void hr_pl_stop(void *handle);
    void hr_pl_pause(void *handle, int flag);
    int hr_pl_get_preview(void *handle, unsigned char *out_rgb, int *out_w, int *out_h);
    int hr_pl_get_preview_ex(void *handle, unsigned char *out_rgb, size_t out_cap,
                              int *out_w, int *out_h, int *out_native_w, int *out_native_h,
                              uint64_t *seq_io);
    uint64_t hr_pl_get_preview_seq(void *handle);
    int hr_pl_get_native_size(void *handle, int *out_w, int *out_h);
    void hr_pl_set_recording(void *handle, int active, intptr_t pipe_fd);
    void hr_pl_set_timestamped_pipe(void *handle, int fps);
    void hr_pl_set_mf_sink(void *handle, void *mf, int use_textures);
    void *hr_pl_get_d3d_device(void *handle);
    void hr_dx_set_mt_protect(int enabled);   // dxgi_capture.cpp
    void hr_pl_set_priority_boost(void *handle, int flag);
    void hr_pl_stats(void *handle, long long *out_frames, long long *out_drops, double *out_fps);
    void hr_pl_set_overlays(void *handle, const HrOverlayDesc *items, int count);
    void hr_pl_set_include_cursor(void *handle, int flag);
    void hr_pl_set_capture_rect(void *handle, int x, int y, int w, int h);
    void hr_pl_set_output_size(void *handle, int w, int h);
    void hr_pl_set_output_pixfmt(void *handle, int nv12);
    void hr_pl_set_gpu_convert(void *handle, int enabled);
    void hr_pl_set_gpu_overlay(void *handle, int enabled);
    void hr_pl_set_preview_fps(void *handle, int fps);
    void hr_pl_set_preview_size(void *handle, int pw, int ph);
    void hr_pl_set_fps(void *handle, int fps);
    void hr_pl_set_preview_needed(void *handle, int enabled);
    int hr_pl_end_recording_segment(void *handle, int timeout_ms);

    // ffmpeg_runner.cpp
    void *hr_ff_create();
    // ddagrab + overlays (ffmpeg_runner.cpp / overlay_feed.cpp)
    void hr_ff_set_overlay_input(void *handle, int w, int h, int fps);
    int hr_ff_overlay_pipe_active(const void *handle);
    intptr_t hr_ff_take_overlay_pipe(void *handle);
    void *hr_ovf_create(intptr_t pipe_handle, int w, int h, int fps);
    void hr_ovf_set_overlays(void *handle, const HrOverlayDesc *items, int count);
    void hr_ovf_set_current_fps(void *handle, double fps);
    int hr_ovf_state(const void *handle);
    void hr_ovf_destroy(void *handle);
    void hr_ff_destroy(void *handle);
    void hr_ff_set_ffmpeg_path(void *h, const char *path);
    void hr_ff_set_output_path(void *h, const char *path);
    void hr_ff_set_codec_args(void *h, const char *args);
    void hr_ff_set_extra_output_args(void *h, const char *args);
    void hr_ff_set_video_params(void *h, int w, int h2, int fps);
    void hr_ff_set_output_size(void *h, int out_w, int out_h);
    void hr_ff_set_pipe_input(void *h, int enable);
    void hr_ff_set_normal_priority(void *h, int enable);
    // Opt-in (AppState::gpu_low_priority): ffmpeg child one GPU scheduling class below normal
    void hr_ff_set_gpu_low_priority(void *h, int enable);
    int  hr_ff_gpu_priority_applied(const void *h);
    // dxgi_capture.cpp: GPU scheduling class of a process (nullptr = HomRec itself), 0 idle / 1 below normal / 2 normal
    int  hr_gpu_sched_set_process(void *hProcess, int level);
    long hr_gpu_sched_last_status();   // NTSTATUS of the last hr_gpu_sched_set_process() call
    void hr_ff_set_gpu_capture(void *h, int enable, int output_idx, int off_x, int off_y,
                                int w, int h2, int draw_mouse);
    int hr_ff_start(void *handle);
    void hr_ff_set_capture_stdout(void *h, int enable);   // (hr_ff_read_stdout() is declared in replay_ram.h)
    intptr_t hr_ff_get_stdin_handle(void *handle);
    int hr_ff_stop_graceful(void *handle);
    int hr_ff_wait(void *handle, int timeout_ms);
    int hr_ff_is_running(const void *handle);
    double hr_ff_output_size_mb(const void *handle);
    // ddagrab diagnostics (ffmpeg_runner.cpp): stderr log, live frame count, exit code
    void hr_ff_set_diag_paths(void *h, const char *log_path, const char *progress_path);
    long long hr_ff_progress_frames(const void *handle);
    int hr_ff_read_log_tail(const void *handle, char *buf, int buf_len);
    int hr_ff_exit_code(const void *handle);
    void hr_ff_kill(void *handle);

    // audio.cpp
    int hr_audio_init();
    int hr_audio_start(float mic_vol, float sys_vol, int mic_mute, int sys_mute, const wchar_t *mic_device_id);
    void hr_audio_set_max_buffer_sec(int seconds);
    int hr_audio_snapshot_to_wav(const char *mic_wav_path, const char *sys_wav_path);
    // The newest window_ms only (0 = the whole rolling buffer)
    int hr_audio_snapshot_to_wav_ex(const char *mic_wav_path, const char *sys_wav_path, int window_ms);
    int hr_audio_stop(const char *mic_wav_path, const char *sys_wav_path);
    void hr_audio_set_volumes(float mic_vol, float sys_vol, int mic_mute, int sys_mute);
    void hr_audio_get_levels(int *out_mic, int *out_sys);
    void hr_audio_pause(int paused);
    void hr_audio_reset_buffers(const char *mic_wav_path, const char *sys_wav_path);
    void hr_audio_flush_buffered();
    int hr_audio_capture_to_wav(const char *mic_wav_path, const char *sys_wav_path);
    int hr_audio_mix_wav(const char *mic_path, const char *sys_path, const char *out_path);
    // 2.4 - extra Audio Mixer channels (another mic / browser / window / audio file)
    int  hr_audio_mix_wav_list(const char *inputs, const char *out_path);
    int  hr_audio_extra_add(int kind, const wchar_t *target, unsigned long pid, float vol, int mute);
    void hr_audio_extra_remove(int id);
    void hr_audio_extra_set_volume(int id, float vol, int mute);
    int  hr_audio_extra_level(int id);
    int  hr_audio_extra_status(int id);
    void hr_audio_extra_begin(const char *stem);
    int  hr_audio_extra_collect(char *out, int cap);
    void hr_audio_extra_shutdown();

    // In-process audio (audio.cpp / audio_pipe.cpp / ffmpeg_runner.cpp)
    int  hr_audio_live_prepare();
    int  hr_audio_live_start(void (*sink)(void *, const int16_t *, size_t), void *user, int out_rate, int head_ms);
    long long hr_audio_live_stop();
    int  hr_audio_live_active();
    void hr_audio_extra_begin_live();
    void hr_audio_extra_end_live();
    void hr_ff_set_audio_pipe(void *h, int rate_hz, int kbps);
    int  hr_ff_audio_pipe_active(const void *h);
    intptr_t hr_ff_take_audio_pipe(void *h);
    void *hr_apipe_open(intptr_t pipe_handle, int rate_hz);
    void hr_apipe_write(void *user, const int16_t *stereo, size_t frames);
    void hr_apipe_end_input(void *h);
    int  hr_apipe_finish(void *h, int timeout_ms);
    int  hr_apipe_failed(const void *h);
    int  hr_apipe_connected(const void *h);
    long long hr_apipe_frames_in(const void *h);
    void hr_apipe_destroy(void *h);
}

#include <cstdio> // remove()/rename() for temp WAV cleanup in Stop()

namespace {
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

// Std::remove()/std::rename() take an ANSI-codepage path on Windows, but every path in
// this file is UTF-8 - so a temp WAV inside an output folder with non-ASCII characters
// (e.g.
bool RemoveFileUtf8(const std::string &p) {
    return DeleteFileW(WideFromNarrow(p).c_str()) != 0;
}
bool RenameFileUtf8(const std::string &from, const std::string &to) {
    return MoveFileExW(WideFromNarrow(from).c_str(), WideFromNarrow(to).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
}

void DeleteReplayDir(const std::wstring &dir) {
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    std::wstring pattern = dir + L"*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name(fd.cFileName);
            if (name == L"." || name == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue; // not expected; skip defensively
            DeleteFileW((dir + name).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}
}

RecordingController::RecordingController(AppState &state) : state_(state) {
    ctl_ = hr_ctl_create();
}

RecordingController::~RecordingController() {
    if (state_.recording) Stop();
    if (finalize_thread_.joinable()) finalize_thread_.join();
    if (instant_replay_active_) {
        StopInstantReplayEncoder();
        DeleteReplayDir(replay_dir_); // see DeleteReplayDir()'s comment
    }
    JoinPendingInstantReplayStop();
    JoinPendingPreviewTeardown();
    if (ctl_) hr_ctl_destroy(ctl_);
    DestroyOverlayFeed();
    if (ffproc_) hr_ff_destroy(ffproc_);
    if (pipeline_) hr_pl_destroy(pipeline_);
    if (live_audio_) { hr_audio_live_stop(); live_audio_ = false; }
    if (apipe_) { hr_apipe_destroy(apipe_); apipe_ = nullptr; }
    if (mfenc_) { hr_mf_destroy(mfenc_); mfenc_ = nullptr; }
    hr_audio_extra_shutdown();   // 2.4: extra mixer channels (their threads must stop before the streams go)
    hr_audio_stop(nullptr, nullptr);
}

void RecordingController::Initialize() {
    // Capture devices created from now on (the live-preview pipeline included) can be shared with the in-process
    // encoder - see Start() / dxgi_capture.cpp.
    hr_dx_set_mt_protect((state_.mf_encoder && state_.mf_gpu_texture) ? 1 : 0);
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
    hr_audio_start(1.0f, 1.0f, 0, 0, WideFromNarrow(state_.mic_device_id).c_str());
    applied_mic_device_id_ = state_.mic_device_id;
}

// One line with the "weak PC" switches that are really in effect for THIS recording. The benchmark run "(c) with
// gpu_low_priority=0" still logged "GPU priority lowered", i.e. it ran with the setting ON and the A/B test compared
// two identical runs - this line makes such a mix-up visible in homrec.log at a glance.
// How the capture pipeline feeds ffmpeg's stdin: 1 = raw frames, every timeline slot is written in full;
// 2 = timestamped (NUT) frames - a frame is only sent when the picture changed and ffmpeg repeats the previous one
// (AppState::skip_static_frames). Custom ffmpeg args that touch the filter chain / frame rate keep the raw pipe,
// because the timestamped pipe adds its own `-vf fps=...`.
static int HrPipeInputMode(const AppState &st) {
    if (!st.skip_static_frames) return 1;
    const std::string c = " " + st.custom_ffmpeg_args + " ";
    static const char *const kConflicts[] = {" -vf ", " -vf=", "-filter", " -r ", "-fps_mode", "-vsync", " -f nut"};
    for (const char *k : kConflicts)
        if (c.find(k) != std::string::npos) return 1;
    return 2;
}

// Target bitrate (kbps) for the in-process encoder, from the same bits-per-pixel curve the QSV path already uses
// (the other hardware encoders get a constant QP from ffmpeg; Media Foundation takes a bitrate instead).
static int HrMfBitrateKbps(int w, int h, int fps, int quality) {
    const int q = quality < 0 ? 0 : (quality > 100 ? 100 : quality);
    const double bpp = 0.04 + 0.14 * (q / 100.0);
    long long kbps = (long long)((double)w * h * (fps > 0 ? fps : 30) * bpp / 1000.0);
    if (kbps < 1500)  kbps = 1500;
    if (kbps > 60000) kbps = 60000;
    return (int)kbps;
}

// AAC bitrate (kbps) from Settings > Audio ("192k", "192", ...); 160 when it is empty or nonsense.
static int HrAacKbps(const std::string &s) {
    const int v = std::atoi(s.c_str());
    return (v >= 32 && v <= 512) ? v : 160;
}

// Custom ffmpeg arguments that choose streams / audio codecs would fight with the audio input and the -map the
// in-process audio path adds: such a recording keeps the old WAV + merge way.
static bool HrCustomArgsTouchAudio(const AppState &st) {
    if (st.custom_ffmpeg_args.empty()) return false;
    const std::string c = " " + st.custom_ffmpeg_args + " ";
    static const char *const kConflicts[] = {" -map", " -c:a", " -acodec", " -an ", " -b:a", " -i ", "-filter_complex", "-lavfi"};
    for (const char *k : kConflicts)
        if (c.find(k) != std::string::npos) return true;
    return false;
}

static std::string HrLowImpactFlagsLine(const AppState &st) {
    auto onoff = [](bool v) { return v ? "ON" : "off"; };
    return std::string("Recording: weak-PC switches in effect: gpu_low_priority=") + onoff(st.gpu_low_priority) +
           ", game_friendly_priority=" + onoff(st.game_friendly_priority) +
           ", qsv_low_power=" + onoff(st.qsv_low_power) +
           ", gpu_convert=" + onoff(st.gpu_convert) +
           ", gpu_overlay=" + onoff(st.gpu_overlay) +
           ", inproc_audio=" + onoff(st.inproc_audio) +
           ", skip_static_frames=" + onoff(HrPipeInputMode(st) == 2) +
           ", ddagrab=" + onoff(st.gpu_capture_ddagrab) + ".";
}

// Result of the immediate GPU-priority call on the ffmpeg child. A refusal right after CreateProcess is normal (the
// child has not opened its GPU yet) - ffmpeg_runner.cpp keeps retrying in the background and logs the final outcome.
static void HrLogFfmpegGpuPriorityStart(bool applied_now) {
    if (applied_now) {
        HrLog::Info("Recording: ffmpeg GPU priority lowered (below normal) - the game gets the GPU first.");
        return;
    }
    char st_hex[16];
    std::snprintf(st_hex, sizeof(st_hex), "0x%08lX", (unsigned long)hr_gpu_sched_last_status());
    HrLog::Info(std::string("Recording: ffmpeg GPU priority not accepted right after launch (NTSTATUS ") + st_hex +
                ") - retrying in the background for up to 8 s; the result follows in this log.");
}

bool RecordingController::MfEncoderWanted(const std::wstring &codec, bool codec_is_hw) const {
    if (!state_.mf_encoder || mf_blocked_ || !codec_is_hw) return false;
    // Only the H.264 hardware encoders (h264_nvenc / h264_qsv / h264_amf): HEVC/AV1 in an MP4 written by
    // Media Foundation depends on codec extensions that are not always installed.
    if (codec.compare(0, 5, L"h264_") != 0) return false;
    // The Media Foundation file writer produces .mp4 only; custom ffmpeg args are meant for ffmpeg.
    if (state_.video_format != VideoFormat::Mp4) return false;
    if (!state_.custom_ffmpeg_args.empty()) return false;
    return true;
}

// Creates the Media Foundation encoder for current_output_path_ and attaches it to pipeline_ (the pipeline is
// not recording yet). On failure everything is released again and false is returned.
bool RecordingController::StartMfEncoder() {
    if (!pipeline_) return false;
    mfenc_ = hr_mf_create();
    if (!mfenc_) {
        HrLog::Warn("Recording: Media Foundation could not be initialised - using ffmpeg.");
        return false;
    }
    const int kbps = HrMfBitrateKbps(output_w_, output_h_, state_.target_fps, state_.quality);
    bool ok = hr_mf_configure(mfenc_, NarrowFromWide(current_output_path_).c_str(),
                              output_w_, output_h_, state_.target_fps, kbps) != 0;
    void *dev = nullptr;
    if (ok && state_.mf_gpu_texture) dev = hr_pl_get_d3d_device(pipeline_);
    // AAC stream in the same .mp4 writer (the audio mixer feeds it live).
    if (ok && live_audio_rate_ > 0) hr_mf_audio_configure(mfenc_, live_audio_rate_, HrAacKbps(state_.audio_aac_bitrate));
    if (ok) ok = hr_mf_start(mfenc_, dev, dev ? 1 : 0) != 0;
    if (!ok) {
        char err[256] = {};
        hr_mf_last_error(mfenc_, err, (int)sizeof(err));
        HrLog::Warn(std::string("Recording: the in-process (Media Foundation) encoder could not start: ") +
                    (err[0] ? err : "unknown error") + " - using the ffmpeg encoder instead.");
        hr_mf_destroy(mfenc_);
        mfenc_ = nullptr;
        RemoveFileUtf8(NarrowFromWide(current_output_path_));   // a half-created empty .mp4 must not stay behind
        return false;
    }
    if (live_audio_rate_ > 0 && !hr_mf_audio_enabled(mfenc_)) {
        live_audio_rate_ = 0;   // no AAC encoder here: the audio is merged after Stop (needs ffmpeg), the video is fine
        HrLog::Warn("Recording: the Media Foundation writer has no AAC audio stream - audio will be merged after Stop.");
    }
    hr_pl_set_mf_sink(pipeline_, mfenc_, hr_mf_textures_enabled(mfenc_));
    return true;
}

// Builds the sink for the encoder of THIS recording and starts the live mixer. Called right after the audio
// buffers were reset (the audio timeline starts there); head_ms = how much later than the video's start that was.
bool RecordingController::StartLiveAudio(int head_ms) {
    live_audio_ = false;
    void (*fn)(void *, const int16_t *, size_t) = nullptr;
    void *user = nullptr;
    if (mfenc_) {
        if (hr_mf_audio_enabled(mfenc_)) { fn = hr_mf_write_audio; user = mfenc_; }
    } else if (ffproc_ && hr_ff_audio_pipe_active(ffproc_)) {
        const intptr_t ph = hr_ff_take_audio_pipe(ffproc_);
        if (ph) {
            apipe_ = hr_apipe_open(ph, live_audio_rate_);
            if (!apipe_) CloseHandle(reinterpret_cast<HANDLE>(ph));
        }
        if (apipe_) { fn = hr_apipe_write; user = apipe_; }
    }
    if (fn && hr_audio_live_start(fn, user, live_audio_rate_, head_ms) != 0) {
        live_audio_ = true;
        HrLog::Info(std::string("Recording: audio is mixed live (") + std::to_string(live_audio_rate_) + " Hz) and " +
                    (mfenc_ ? "written as AAC by the in-process encoder" : "muxed by ffmpeg over a pipe") +
                    " - no temp WAV files, no merge after Stop.");
        return true;
    }
    // Nothing started. An ffmpeg that was launched with the audio input must still see its end (EOF), otherwise it
    // would wait for audio forever: the sink (if it exists) closes the pipe by itself once told there is no more.
    if (apipe_) hr_apipe_end_input(apipe_);
    hr_audio_extra_end_live();
    live_audio_rate_ = 0;
    HrLog::Warn("Recording: the live audio mixer could not start - audio will be merged after Stop instead.");
    return false;
}

void RecordingController::StopLiveAudio() {
    if (live_audio_) {
        const long long frames = hr_audio_live_stop();    // mixes the last buffered audio into the sink
        live_audio_ = false;
        HrLog::Info("Recording: live audio ended (" + std::to_string(frames) + " frames mixed" +
                    (live_audio_rate_ > 0 ? ", " + std::to_string((double)frames / live_audio_rate_) + " s" : std::string()) + ").");
    }
    if (apipe_) hr_apipe_end_input(apipe_);
}

void RecordingController::FinishAudioSink() {
    if (!apipe_) return;
    const bool ff_alive = ffproc_ && hr_ff_is_running(ffproc_);
    const bool ok = hr_apipe_finish(apipe_, ff_alive ? 10000 : 500) != 0;
    if (!ok)
        HrLog::Warn("Recording: the audio did not reach ffmpeg completely (ffmpeg stopped reading it) - the end of the "
                    "recording's sound may be missing.");
    hr_apipe_destroy(apipe_);
    apipe_ = nullptr;
}

std::wstring RecordingController::BuildCodecArgs(const std::wstring &codec) {
    if (!state_.custom_ffmpeg_args.empty()) {
        return L"-c:v " + codec + L" " + WideFromNarrow(state_.custom_ffmpeg_args);
    }

    wchar_t buf[512] = {};
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    // state_.enc_preset (the "Encoder preset" dropdown, same tab)
    std::wstring preset = WideFromNarrow(state_.enc_preset);
    hr_build_codec_args_ex(codec.c_str(), state_.quality, state_.target_fps,
                            (int)si.dwNumberOfProcessors, output_w_, output_h_,
                            buf, 512, preset.c_str());
    std::wstring args = buf;
    // Opt-in (AppState::qsv_low_power): fixed-function VDENC encoding for Intel QSV. Only touches
    // the auto-built args - custom_ffmpeg_args (returned above) is used verbatim.
    if (state_.qsv_low_power && codec.find(L"qsv") != std::wstring::npos &&
        args.find(L"-low_power") == std::wstring::npos) {
        args += L" -low_power 1";
        HrLog::Info("Recording: Intel QSV low-power (VDENC) encoding requested (-low_power 1).");
    }
    return args;
}

bool RecordingController::WindowTargetDefined() const {
    return state_.capture_mode == CaptureMode::Window &&
           (!state_.capture_window_title.empty() || !state_.capture_window_exe.empty() ||
            !state_.capture_window_class.empty());
}

std::string RecordingController::WindowTargetLabel() const {
    if (!state_.capture_window_title.empty()) return state_.capture_window_title;
    if (!state_.capture_window_exe.empty()) return state_.capture_window_exe;
    return state_.capture_window_class;
}

bool RecordingController::ResolveWindowTarget(HWND &hwnd, RECT &rect) const {
    std::string how;
    const bool ok = HR_ResolveCaptureWindowFromState(state_, hwnd, rect, &how);
    // One line per CHANGE of route (the resolver runs several times a second): "same window handle" ->
    // "matched exe class (search)" -> "not found" tells the whole story of a lost / re-found window.
    const std::string route = ok ? how : std::string("not found");
    if (route != window_route_logged_) {
        window_route_logged_ = route;
        HrWgcLog::Info("Window target '" + WindowTargetLabel() + "': " + route);
    }
    return ok;
}

int RecordingController::WindowSourceMode() const {
    return pipeline_ ? hr_pl_window_mode(pipeline_) : 0;
}

// Resolves the {app} filename-template placeholder - see the header
// comment on the declaration for what this falls back to and why.
std::string RecordingController::ResolveCaptureAppName() const {
    if (state_.capture_mode == CaptureMode::Region) return "Region";
    if (!WindowTargetDefined()) {
        return "Desktop";
    }

    HWND hwnd = nullptr;
    RECT r{};
    if (!ResolveWindowTarget(hwnd, r) || !hwnd) {
        return "Desktop"; // window's gone (closed) - fall back rather than emit "{app}" literally
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
    // Was a 256-byte buffer - a long output folder + template
    // got silently cut off, taking the ".mp4" extension with it.
    char buf[1024] = {};
    std::string app_name = ResolveCaptureAppName();
    // 2.4: the container comes from Settings > Video > Container (mp4 / mkv /
    // both). "both" records to .mkv and remuxes an .mp4 copy at the end.
    hr_filename_from_template_ex(state_.filename_template.c_str(), state_.output_folder.c_str(),
                                  app_name.c_str(), buf, (int)sizeof(buf), state_.active_preset_name.c_str(),
                                  HrVideoFormatExt(state_.video_format));
    std::wstring path = WideFromNarrow(buf);

    // Ffmpeg is always run with -y, so a template that isn't
    // unique per recording (e.g. "{app}_{date}", or two clips in the same
    // minute with "{hh}-{min}") silently overwrote the earlier file.
    const bool both_fmt = state_.video_format == VideoFormat::Both;
    auto taken = [both_fmt](const std::wstring &p) {
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
        if (!both_fmt) return false;
        const size_t d = p.find_last_of(L'.');
        if (d == std::wstring::npos) return false;
        return GetFileAttributesW((p.substr(0, d) + L".mp4").c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    if (taken(path)) {
        const size_t dot = path.find_last_of(L'.');
        const size_t sep = path.find_last_of(L"\\/");
        const bool has_ext = dot != std::wstring::npos && (sep == std::wstring::npos || dot > sep);
        const std::wstring stem = has_ext ? path.substr(0, dot) : path;
        const std::wstring ext = has_ext ? path.substr(dot) : std::wstring();
        for (int n = 2; n < 10000; ++n) {
            std::wstring cand = stem + L"_" + std::to_wstring(n) + ext;
            if (!taken(cand)) { path = cand; break; }
        }
    }
    return path;
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

// Settings > Resolution: turns a source size (the full monitor, or the cropped window rect
// in window-capture mode) into the desired *output* size, honoring ResolutionMode::Percent
// (scale_factor, e.g.
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
    // Percent mode had no floor (a tiny/zero scale_factor gave a
    // 0x0 output); Absolute mode already clamped to 2.
    if (out_w < 2) out_w = 2;
    if (out_h < 2) out_h = 2;
}

bool RecordingController::CheckCaptureTarget(std::wstring &error_out) const {
    if (WindowTargetDefined()) {
        HWND hwnd = nullptr;
        RECT r{};
        if (!ResolveWindowTarget(hwnd, r)) {
            error_out = L"The window you selected (\"" + WideFromNarrow(WindowTargetLabel()) +
                        L"\") is closed, hidden or minimized, so it can't be recorded.\n\n"
                        L"Restore it, or pick another one in the Layers panel (double-click the first row, "
                        L"\"Capture source...\") - or switch that layer to the full desktop - and start "
                        L"recording again.";
            HrLog::Error("Start refused: selected window '" + WindowTargetLabel() +
                         "' not found/minimized - not falling back to full-desktop capture.");
            return false;
        }
        if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            error_out = L"The window you selected is not on any screen right now, so it can't be recorded.\n\n"
                        L"Move it onto a screen, or pick another one in the Layers panel "
                        L"(\"Capture source...\").";
            HrLog::Error("Start refused: selected window '" + WindowTargetLabel() +
                         "' is entirely off-screen - not falling back to full-desktop capture.");
            return false;
        }
    } else if (state_.capture_mode == CaptureMode::Region && state_.region_w > 0 && state_.region_h > 0) {
        RECT r{state_.region_x, state_.region_y,
               state_.region_x + state_.region_w, state_.region_y + state_.region_h};
        if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            error_out = L"The region you selected is no longer on any screen (a monitor may have been "
                        L"disconnected), so it can't be recorded.\n\nChoose the region again in the Layers panel "
                        L"(\"Capture source...\").";
            HrLog::Error("Start refused: selected region is entirely off-screen - not falling back to "
                         "full-desktop capture.");
            return false;
        }
    }
    return true;
}

void RecordingController::ResolveCaptureSize() {
    // Fresh target (or a fresh Start()/EnsurePreview() on the same one) -
    // don't let a warning already shown for a *previous* recording's lost
    // window suppress the first warning a new one legitimately earns.
    window_track_lost_warned_ = false;

    void *di = hr_di_create();
    hr_di_refresh(di);
    int mx = 0, my = 0, mw = 1920, mh = 1080;
    float dpi = 96.0f;
    int idx = state_.monitor_id > 0 ? state_.monitor_id - 1 : 0;

    // Unified "crop target" rect in virtual-desktop coordinates, resolved
    // once, and used below for BOTH which monitor to capture and where to
    // crop it - for either Window capture (from the live window's RECT)
    RECT target_rect{};
    bool have_target = false;
    bool is_window = WindowTargetDefined();
    bool is_region = (state_.capture_mode == CaptureMode::Region && state_.region_w > 0 && state_.region_h > 0);
    HrLog::Info("Capture target resolve: mode=" + HrCaptureModeToStr(state_.capture_mode) + ", window='" +
                state_.capture_window_title + "' (" + state_.capture_window_exe + " / " + state_.capture_window_class +
                "), hwnd=" + std::to_string((unsigned long long)(uintptr_t)state_.capture_window_hwnd));

    HWND capture_hwnd = nullptr;
    if (is_window) {
        have_target = ResolveWindowTarget(capture_hwnd, target_rect);
    } else if (is_region) {
        target_rect.left = state_.region_x;
        target_rect.top = state_.region_y;
        target_rect.right = state_.region_x + state_.region_w;
        target_rect.bottom = state_.region_y + state_.region_h;
        have_target = true;
    }

    use_wgc_ = false;
    wgc_hwnd_ = nullptr;
    // Changing the method in Settings gives WGC a fresh chance after an earlier failure.
    if (wgc_method_seen_ != state_.window_capture_method) {
        wgc_method_seen_ = state_.window_capture_method;
        wgc_disabled_ = false;
    }
    // WGC is OPT-IN (window_capture_method = "wgc", Settings > General > Window capture).
    if (is_window && have_target && capture_hwnd && state_.WindowCaptureWantsWgc() && !wgc_disabled_) {
        int ww = 0, wh = 0;
        if (HrWgcSupported() && HrWgcProbeWindow(capture_hwnd) &&
            HrWgcQueryWindowSize(capture_hwnd, state_.window_capture_area == "client", &ww, &wh) &&
            ww >= 16 && wh >= 16) {
            hr_di_destroy(di);
            use_wgc_ = true;
            wgc_hwnd_ = capture_hwnd;
            capture_output_idx_ = 0;
            capture_w_ = std::min(ww, 16384) & ~1;
            capture_h_ = std::min(wh, 16384) & ~1;
            crop_x_ = crop_y_ = crop_w_ = crop_h_ = 0;
            ComputeOutputDims(capture_w_, capture_h_, output_w_, output_h_);
            HrLog::Info("Capture target (window): recording the window itself (Windows.Graphics.Capture) at " +
                        std::to_string(capture_w_) + "x" + std::to_string(capture_h_) + ", output " +
                        std::to_string(output_w_) + "x" + std::to_string(output_h_));
            HrWgcLog::Note("Window capture: recording the window itself, canvas " + std::to_string(capture_w_) + "x" +
                           std::to_string(capture_h_) + ", output " + std::to_string(output_w_) + "x" +
                           std::to_string(output_h_) + ", area=" + state_.window_capture_area +
                           ", smart=" + (state_.window_capture_smart ? "on" : "off") +
                           ", fps cap=" + std::to_string(state_.window_capture_fps_cap));
            return;
        }
        HrLog::Warn("Window capture: Windows.Graphics.Capture isn't usable for this window "
                    "(unsupported Windows version, crash guard tripped, or Windows refused this window) - "
                    "using the SCREEN CROP instead: anything on top of the window will be recorded too.");
        HrWgcLog::Warn("Window capture: the window source can't be used for this window (unsupported Windows, crash "
                       "guard, or the window was refused / is smaller than 16x16) - using the screen crop.");
    } else if (is_window && have_target && !state_.WindowCaptureWantsWgc()) {
        HrLog::Info("Window capture: using the screen-crop method (the default; Windows.Graphics.Capture is "
                    "opt-in in Settings) - anything on top of the window will be recorded too.");
    }

    if (have_target) {
        int tcx = (target_rect.left + target_rect.right) / 2;
        int tcy = (target_rect.top + target_rect.bottom) / 2;
        int count = hr_di_count(di);
        for (int i = 0; i < count; ++i) {
            int tx = 0, ty = 0, tw = 0, th = 0; float td = 96.0f;
            if (!hr_di_get(di, i, &tx, &ty, &tw, &th, &td)) continue;
            if (tcx >= tx && tcx < tx + tw && tcy >= ty && tcy < ty + th) {
                idx = i;
                break;
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

    {
        const int dx_count = hr_dx_output_count(0);
        for (int i = 0; i < dx_count; ++i) {
            int ox = 0, oy = 0, ow = 0, oh = 0;
            if (!hr_dx_output_desc(0, i, &ox, &oy, &ow, &oh, nullptr, 0)) continue;
            if (ox == mx && oy == my && ow == mw && oh == mh) {
                if (i != idx) {
                    HrLog::Info("Capture: monitor list index " + std::to_string(idx) +
                                " is DXGI output " + std::to_string(i) + " (orderings differ).");
                }
                capture_output_idx_ = i;
                break;
            }
        }
    }

    capture_w_ = mw;
    capture_h_ = mh;
    if (capture_w_ % 2) capture_w_--;
    if (capture_h_ % 2) capture_h_--;

    ComputeOutputDims(mw, mh, output_w_, output_h_);

    crop_x_ = crop_y_ = crop_w_ = crop_h_ = 0;
    if (is_window || is_region) {
        if (have_target) {
            RECT r = target_rect;
            // Target rect is in virtual-desktop coordinates; crop_x_/y_ need to be relative to the
            // captured monitor's own frame (monitor_left/top, set above), matching what bgra_buf
            // actually holds.
            int wx = r.left - mx, wy = r.top - my;
            int ww = r.right - r.left, wh = r.bottom - r.top;
            // Clamp to the monitor bounds - hr_pl_set_capture_rect() also clamps defensively, but
            // doing it here too means output_w_/ output_h_ (computed from ww/wh below) reflect the
            // actual clamped crop size, not the pre-clamp one.
            if (wx < 0) { ww += wx; wx = 0; }
            if (wy < 0) { wh += wy; wy = 0; }
            if (wx + ww > capture_w_) ww = capture_w_ - wx;
            if (wy + wh > capture_h_) wh = capture_h_ - wy;
            if (ww % 2) ww--;
            if (wh % 2) wh--;

            if (ww > 0 && wh > 0) {
                crop_x_ = wx; crop_y_ = wy; crop_w_ = ww; crop_h_ = wh;
                ComputeOutputDims(ww, wh, output_w_, output_h_);
                HrLog::Info(std::string("Capture target (") + (is_window ? "window" : "region") +
                            "): screen rect " + std::to_string(r.left) + "," + std::to_string(r.top) +
                            " -> " + std::to_string(r.right) + "," + std::to_string(r.bottom) +
                            " on monitor at " + std::to_string(mx) + "," + std::to_string(my) + " " +
                            std::to_string(mw) + "x" + std::to_string(mh) + " (DXGI output " +
                            std::to_string(capture_output_idx_) + ") -> crop " +
                            std::to_string(ww) + "x" + std::to_string(wh) + " at " +
                            std::to_string(wx) + "," + std::to_string(wy) + ", output " +
                            std::to_string(output_w_) + "x" + std::to_string(output_h_));
            } else if (is_window) {
                HrLog::Warn("Window capture: '" + WindowTargetLabel() +
                            "' is entirely off the selected monitor - falling back to full desktop.");
            } else {
                HrLog::Warn("Region capture: the selected region is entirely off the selected "
                            "monitor - falling back to full desktop.");
            }
        } else if (is_window) {
            // Window was closed/renamed since being picked, or isn't on screen anymore.
            HrLog::Warn("Window capture: couldn't find the window '" +
                        WindowTargetLabel() + "' - falling back to full desktop.");
        }
        // is_region with !have_target can't happen (is_region already
        // requires region_w/h > 0, and have_target is unconditionally set
        // true for it above).
    }
}

// Creates the capture pipeline for whatever ResolveCaptureSize() decided: the window source
// (Windows.Graphics.Capture) or the monitor duplication. Records which one it made so a later
// Start() only reuses a pipeline that matches the CURRENT target.
void *RecordingController::CreatePipelineForTarget(intptr_t pipe_fd, int pvw, int pvh) {
    void *pl = nullptr;
    if (use_wgc_ && wgc_hwnd_) {
        pl = hr_pl_create_window(capture_w_, capture_h_, state_.target_fps, pipe_fd, pvw, pvh,
                                 wgc_hwnd_, state_.window_capture_area == "client" ? 1 : 0,
                                 state_.cursor_enabled ? 1 : 0,
                                 state_.window_capture_smart ? 1 : 0, state_.window_capture_fps_cap);
        if (!pl) {
            // Don't keep retrying a source that just failed: from now on this session uses the crop.
            wgc_disabled_ = true;
            HrLog::Error("Window capture: couldn't start Windows.Graphics.Capture for this window - "
                         "falling back to the screen crop.");
            if (pipe_fd == 0) {
                // Preview-only pipeline: nothing downstream (ffmpeg) depends on the size yet, so
                // re-resolve the target as a screen crop and build that right away.
                ResolveCaptureSize();
                if (!use_wgc_)
                    pl = hr_pl_create(capture_w_, capture_h_, state_.target_fps, pipe_fd, pvw, pvh, capture_output_idx_);
            }
            // A recording (pipe_fd != 0) can't switch mid-way - ffmpeg was already told the output
            // size - so Start() reports it and the next press of Start uses the crop.
        }
    } else {
        pl = hr_pl_create(capture_w_, capture_h_, state_.target_fps, pipe_fd, pvw, pvh, capture_output_idx_);
    }
    pipeline_wgc_ = (pl != nullptr) && use_wgc_;
    pipeline_wgc_hwnd_ = pipeline_wgc_ ? wgc_hwnd_ : nullptr;
    // A brand-new pipeline starts with the generic thumbnail size; re-apply the pane size the
    // UI last reported so the preview doesn't need a CPU rescale after a pipeline rebuild.
    if (pl && pv_view_w_ >= 64 && pv_view_h_ >= 64) {
        int mw = 0, mh = 0;
        ScaledPreviewSize(mw, mh);
        hr_pl_set_preview_size(pl, std::min(pv_view_w_, mw) & ~1, std::min(pv_view_h_, mh) & ~1);
    }
    return pl;
}

void RecordingController::RetargetWindowCapture() {
    if (!pipeline_) return;
    // Region's whole point is a fixed area of the screen - it must NOT
    // track anything, so this only ever applies to Window mode.
    if (!WindowTargetDefined()) return;
    // OPTIMIZATION (2.3): while the tracked window is gone, every ~20 Hz overlay
    // tick used to run a full EnumWindows()+title scan. Retry twice a second.
    // 7th pass: a window source (which never needs a crop rect) only checks four times a second.
    const auto now_tp = std::chrono::steady_clock::now();
    if ((window_track_lost_warned_ || pipeline_wgc_) && now_tp < next_window_retarget_) return;

    HWND hwnd = nullptr;
    RECT r{};
    if (!ResolveWindowTarget(hwnd, r)) {
        if (!window_track_lost_warned_) {
            window_track_lost_warned_ = true;
            HrLog::Warn("Window capture: '" + WindowTargetLabel() +
                        "' is no longer available (closed or minimized) - keeping the last "
                        "captured picture until it's back.");
            HrWgcLog::Warn("Window target '" + WindowTargetLabel() + "' lost (closed / minimized / not matching any more) "
                           "- the last picture is kept, looking for it twice a second.");
        }
        next_window_retarget_ = now_tp + std::chrono::milliseconds(500);
        return;
    }
    // (ResolveWindowTarget already remembered the exact HWND in state_ - a retitled window (a browser
    // tab, an app's "*Unsaved" marker) keeps tracking the SAME window as long as "follow the window" is on.)

    // 7th pass: the source records a window by itself - nothing to crop, but when the program replaced its
    // window (new main window, the old handle is gone) the source has to be pointed at the new one.
    if (pipeline_wgc_) {
        next_window_retarget_ = now_tp + std::chrono::milliseconds(250);
        if (window_track_lost_warned_) {
            window_track_lost_warned_ = false;
            HrWgcLog::Note("Window target '" + WindowTargetLabel() + "' is back.");
        }
        if (hwnd != pipeline_wgc_hwnd_) {
            if (hr_pl_window_retarget(pipeline_, hwnd)) {
                HrWgcLog::Note("Window source follows the window to handle " +
                               std::to_string((unsigned long long)(uintptr_t)hwnd) + " ('" +
                               HR_WindowTitleUtf8(hwnd) + "').");
                pipeline_wgc_hwnd_ = hwnd;
                wgc_hwnd_ = hwnd;
                window_follow_failed_warned_ = false;
            } else if (!window_follow_failed_warned_) {
                window_follow_failed_warned_ = true;
                HrWgcLog::Warn("The window source cannot follow to another window handle (native "
                               "Windows.Graphics.Capture is bound to its window) - restart the recording to use the new one.");
            }
        }
        return;
    }

    int wx = r.left - state_.monitor_left, wy = r.top - state_.monitor_top;
    int ww = r.right - r.left, wh = r.bottom - r.top;
    if (wx < 0) { ww += wx; wx = 0; }
    if (wy < 0) { wh += wy; wy = 0; }
    if (wx + ww > capture_w_) ww = capture_w_ - wx;
    if (wy + wh > capture_h_) wh = capture_h_ - wy;
    if (ww % 2) ww--;
    if (wh % 2) wh--;

    if (ww <= 0 || wh <= 0) {
        if (!window_track_lost_warned_) {
            window_track_lost_warned_ = true;
            HrLog::Warn("Window capture: '" + WindowTargetLabel() +
                        "' moved off the monitor being recorded - keeping the last "
                        "captured position until it's back on it.");
        }
        return;
    }
    window_track_lost_warned_ = false;

    if (wx == crop_x_ && wy == crop_y_ && ww == crop_w_ && wh == crop_h_) return; // hasn't moved/resized

    crop_x_ = wx; crop_y_ = wy; crop_w_ = ww; crop_h_ = wh;
    hr_pl_set_capture_rect(pipeline_, crop_x_, crop_y_, crop_w_, crop_h_);
}

// Writes the reason an ffmpeg process died into homrec.log: its exit code plus the last lines of
// its stderr (only the full-GPU/ddagrab ffmpeg has a stderr log - see hr_ff_set_diag_paths()).
// A GUI app has no console, so before this the reason was simply lost.
static void LogFfmpegDeathDetails(void *ff, const std::string &what) {
    std::string msg = what + " - ffmpeg exit code " + std::to_string(hr_ff_exit_code(ff));
    char buf[4096] = {};
    const int n = hr_ff_read_log_tail(ff, buf, (int)sizeof(buf));
    if (n > 0) {
        std::string raw(buf, (size_t)n);
        for (char &c : raw) if (c == '\r') c = '\n';
        std::vector<std::string> lines;
        size_t from = 0;
        while (from < raw.size()) {
            size_t nl = raw.find('\n', from);
            std::string ln = raw.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
            if (!ln.empty()) lines.push_back(ln);
            if (nl == std::string::npos) break;
            from = nl + 1;
        }
        const size_t keep = 10;
        const size_t first = lines.size() > keep ? lines.size() - keep : 0;
        msg += "; last ffmpeg output (logs\\ffmpeg_ddagrab.log): ";
        for (size_t i = first; i < lines.size(); ++i) {
            if (i > first) msg += " | ";
            msg += lines[i];
        }
    }
    HrLog::Error(msg);
}

void RecordingController::DestroyOverlayFeed() {
    if (ovfeed_) { hr_ovf_destroy(ovfeed_); ovfeed_ = nullptr; }
}

// One ffmpeg ddagrab process writing `out_path` (the whole recording, or one piece after a Pause).
void *RecordingController::SpawnDdagrabFfmpeg(const std::wstring &out_path, bool overlay, bool &died, bool &capturing) {
    died = false;
    capturing = false;
    void *ff = hr_ff_create();
    if (!ff) return nullptr;
    const int ov_fps = state_.target_fps > 0 && state_.target_fps < 15 ? state_.target_fps : 15;
    hr_ff_set_ffmpeg_path(ff, NarrowFromWide(ffmpeg_path_).c_str());
    hr_ff_set_output_path(ff, NarrowFromWide(out_path).c_str());
    hr_ff_set_codec_args(ff, NarrowFromWide(ddagrab_codec_args_).c_str());
    hr_ff_set_normal_priority(ff, state_.game_friendly_priority ? 1 : 0);
    hr_ff_set_gpu_low_priority(ff, state_.gpu_low_priority ? 1 : 0);
    hr_ff_set_video_params(ff, ddagrab_src_w_, ddagrab_src_h_, state_.target_fps);
    hr_ff_set_gpu_capture(ff, /*enable=*/1, capture_output_idx_,
                          crop_x_, crop_y_, crop_w_, crop_h_,
                          state_.cursor_enabled ? 1 : 0);
    if (overlay) hr_ff_set_overlay_input(ff, ddagrab_src_w_, ddagrab_src_h_, ov_fps);
    // ffmpeg's stderr + a -progress file (live frame count for the FPS readout).
    {
        const std::string ff_log  = NarrowFromWide(HrLogPaths::LogFilePath(L"ffmpeg_ddagrab.log"));
        const std::string ff_prog = NarrowFromWide(HrLogPaths::LogFilePath(L"ffmpeg_ddagrab_progress.txt"));
        RemoveFileUtf8(ff_prog);   // a stale file must not report old frame counts
        hr_ff_set_diag_paths(ff, ff_log.c_str(), ff_prog.c_str());
    }
    if (hr_ff_start(ff) != 0) {
        HrLog::Warn("Recording: couldn't launch ffmpeg for ddagrab capture.");
        hr_ff_destroy(ff);
        return nullptr;
    }
    if (overlay) {
        // ffmpeg opens the overlay pipe while it initialises and waits for the first picture, so the feeder has to
        // be running right away (it connects, then sends two pictures at once).
        const intptr_t pipe = hr_ff_overlay_pipe_active(ff) ? hr_ff_take_overlay_pipe(ff) : 0;
        ovfeed_ = pipe ? hr_ovf_create(pipe, ddagrab_src_w_, ddagrab_src_h_, ov_fps) : nullptr;
        if (!ovfeed_) {
            if (pipe) CloseHandle(reinterpret_cast<HANDLE>(pipe));   // hr_ovf_create() did not take it
            HrLog::Warn("Recording: the ddagrab overlay feed could not be set up.");
            hr_ff_kill(ff);
            hr_ff_destroy(ff);
            RemoveFileUtf8(NarrowFromWide(out_path));
            return nullptr;
        }
        if (last_overlays_sent_valid_)
            hr_ovf_set_overlays(ovfeed_, last_overlays_sent_.empty() ? nullptr : last_overlays_sent_.data(),
                                (int)last_overlays_sent_.size());
    }
    HrLog::Info(HrLowImpactFlagsLine(state_));
    if (state_.gpu_low_priority)
        HrLogFfmpegGpuPriorityStart(hr_ff_gpu_priority_applied(ff) != 0);

    // ffmpeg writes the container header only once the first captured frame has gone through
    // the filter graph and the encoder is initialised, so "the output file has content" ==
    // "capture is really running".
    const std::string out_narrow = NarrowFromWide(out_path);
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        if (!hr_ff_is_running(ff)) { died = true; break; }
        if (hr_file_size_mb(out_narrow.c_str()) > 0.0f) { capturing = true; break; }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(3000)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    if (died) {
        LogFfmpegDeathDetails(ff, "Recording: ffmpeg exited while starting ddagrab capture");
        DestroyOverlayFeed();
        hr_ff_destroy(ff);
        RemoveFileUtf8(out_narrow);
        return nullptr;
    }
    return ff;
}

bool RecordingController::TryStartDdagrab(const std::wstring &codec_args, bool codec_is_hw,
                                          std::chrono::steady_clock::time_point &t_video_go) {
    if (!state_.gpu_capture_ddagrab) return false;

    const int src_w = (crop_w_ > 0) ? crop_w_ : capture_w_;
    const int src_h = (crop_h_ > 0) ? crop_h_ : capture_h_;
    bool any_overlay = false;
    for (const auto &o : state_.overlays)
        if (o.visible && o.w > 0 && o.h > 0) { any_overlay = true; break; }
    if (!state_.desktop_visible) any_overlay = true;   // a hidden Desktop layer needs the CPU pipeline (ddagrab can't blank it)

    // Overlays through ffmpeg are an opt-in and have their own preconditions; anything else keeps using the
    // normal pipeline.
    const char *overlay_why = nullptr;
    bool overlay_mode = false;
    if (any_overlay) {
        if (!state_.ddagrab_overlays)        overlay_why = "overlays/plugins are active and need the CPU pipeline (ddagrab_overlays=1 would draw them through ffmpeg)";
        else if (!state_.desktop_visible)    overlay_why = "a hidden Desktop layer needs the CPU pipeline";
        else if (!pipeline_)                 overlay_why = "the overlay feed needs the preview pipeline (the live preview is off)";
        else if (codec_args.find(L"qsv") != std::wstring::npos)
                                             overlay_why = "ddagrab overlays work with NVENC / AMF only";
        else {
            for (const auto &o : state_.overlays)
                if (o.visible && o.w > 0 && o.h > 0 && (o.type == "region" || o.type == "window")) {
                    overlay_why = "region / window layers need the CPU pipeline";
                    break;
                }
        }
        overlay_mode = (overlay_why == nullptr);
    }

    const char *why = nullptr;
    if (!ffmpeg_found_)                                   why = "ffmpeg not found";
    else if (ddagrab_blocked_)                            why = "it already failed mid-recording in this session (a game changing the display mode?)";
    else if (!codec_is_hw)                                why = "a software encoder is selected (ddagrab needs nvenc/qsv/amf)";
    else if (state_.capture_mode == CaptureMode::Window)  why = "window capture follows a moving window";
    else if (any_overlay && !overlay_mode)                why = overlay_why;
    else if (state_.auto_pause_on_silence)                why = "auto-pause on silence needs the pipeline's pause";
    else if (src_w <= 0 || src_h <= 0 ||
             output_w_ != src_w || output_h_ != src_h)    why = "the output resolution differs from the captured size";
    else if (state_.custom_ffmpeg_args.find("-vf") != std::string::npos ||
             state_.custom_ffmpeg_args.find("-filter") != std::string::npos)
                                                          why = "custom ffmpeg filters";
    if (!why) {
        if (ddagrab_probe_ == 0)
            ddagrab_probe_ = hr_probe_ddagrab(ffmpeg_path_.c_str()) ? 1 : 2;
        if (ddagrab_probe_ == 2) why = "this ffmpeg build has no ddagrab filter (needs ffmpeg 6.0+)";
    }
    if (why) {
        HrLog::Info(std::string("Recording: full-GPU (ddagrab) capture not used - ") + why +
                    " -> using the normal capture pipeline.");
        return false;
    }

    ddagrab_src_w_ = src_w;
    ddagrab_src_h_ = src_h;
    ddagrab_codec_args_ = codec_args;
    ddagrab_extra_segs_.clear();
    ddagrab_frame_base_ = 0;
    ddagrab_done_mb_ = 0.0;
    ddagrab_seg_seq_ = 0;
    ddagrab_gap_ = false;
    DestroyOverlayFeed();
    if (overlay_mode) SyncOverlays();   // make last_overlays_sent_ current before the feeder takes its first copy

    bool died = false, capturing = false;
    void *ff = SpawnDdagrabFfmpeg(current_output_path_, overlay_mode, died, capturing);
    if (!ff) {
        HrLog::Warn(died ? "Recording: ffmpeg exited while starting ddagrab capture (unsupported on this "
                           "GPU/driver/ffmpeg build?) -> falling back to the normal capture pipeline."
                         : "Recording: ddagrab capture could not start -> using the normal capture pipeline.");
        return false;
    }
    if (!capturing) {
        HrLog::Warn("Recording: ddagrab capture is still initialising after 3s - continuing; "
                    "a failure from here on is reported like any ffmpeg crash.");
    }

    ffproc_ = ff;
    ddagrab_overlay_ = overlay_mode;
    t_video_go = std::chrono::steady_clock::now();
    ddagrab_pause_warned_ = false;
    HrLog::Info(std::string("Recording: full-GPU capture + encode via ffmpeg ddagrab") +
                (overlay_mode ? " with HomRec overlays blended by ffmpeg (system-memory blend)" : " (HomRec overlays/plugins unavailable)") +
                (state_.ddagrab_pause ? ", Pause supported (piece-wise)." : ", Pause unavailable."));
    return true;
}

// ---- Pause / Resume of a ddagrab recording --------------------------------------------------------------
void RecordingController::DdagrabPause() {
    // The clocks stop at the click; ffmpeg needs a moment to close its piece.
    const int new_state = hr_ctl_pause_toggle(ctl_);
    state_.paused = (new_state == 2 /* HR_STATE_PAUSED */);
    hr_audio_pause(1);
    ddagrab_gap_ = true;          // BEFORE ffmpeg ends: PollStats must not call that a crash
    const long long fr = ffproc_ ? hr_ff_progress_frames(ffproc_) : -1;
    hr_ff_stop_graceful(ffproc_);   // 'q': the piece gets its index / trailer
    if (hr_ff_wait(ffproc_, 10000) != 0 && hr_ff_is_running(ffproc_)) {
        HrLog::Error("Recording: ffmpeg didn't close the piece within 10s of Pause - killing it (this piece may be damaged).");
        hr_ff_kill(ffproc_);
    }
    DestroyOverlayFeed();
    const long long fr_end = ffproc_ ? hr_ff_progress_frames(ffproc_) : -1;
    ddagrab_frame_base_ += (fr_end > fr ? fr_end : (fr > 0 ? fr : 0));
    ddagrab_done_mb_ += ffproc_ ? hr_ff_output_size_mb(ffproc_) : 0.0;
    ddagrab_last_frames_ = -1;
    HrLog::Info("Recording paused (ddagrab: piece " + std::to_string(ddagrab_extra_segs_.size() + 1) + " closed)");
}

void RecordingController::DdagrabResume() {
    // Every piece gets the SAME container as the first one: ffmpeg's concat demuxer mis-times a join of mp4 + mkv
    // pieces (checked with ffmpeg 6.1: 9 s of frames came out as 2.98 s), but joins same-container pieces exactly.
    std::wstring seg = current_output_path_;
    std::wstring ext = L".mkv";
    const size_t dot = seg.find_last_of(L'.');
    if (dot != std::wstring::npos && (seg.find_last_of(L"\\/") == std::wstring::npos || dot > seg.find_last_of(L"\\/"))) {
        ext = seg.substr(dot);
        seg.resize(dot);
    }
    seg += L".part" + std::to_wstring(++ddagrab_seg_seq_) + ext;

    if (ddagrab_overlay_ && pipeline_) SyncOverlays();
    bool died = false, capturing = false;
    void *ff = SpawnDdagrabFfmpeg(seg, ddagrab_overlay_, died, capturing);
    if (!ff) {
        HrLog::Error("Recording: ffmpeg couldn't be restarted after the Pause - stopping the recording "
                     "with what was captured so far.");
        crash_was_ddagrab_ = false;               // no automatic restart of a recording the user paused
        crash_video_elapsed_sec_ = elapsed_seconds();
        crashed_ = true;                          // the UI stops + finalizes (the closed pieces are joined)
        return;
    }
    ddagrab_extra_segs_.push_back(seg);
    if (ffproc_) hr_ff_destroy(ffproc_);          // the closed piece's handle
    ffproc_ = ff;
    ddagrab_gap_ = false;
    // Video is capturing again: now the clocks and the audio go on, so audio and picture stay together.
    const int new_state = hr_ctl_pause_toggle(ctl_);
    state_.paused = (new_state == 2);
    hr_audio_pause(0);
    last_loud_time_ = std::chrono::steady_clock::now();
    HrLog::Info("Recording resumed (ddagrab: piece " + std::to_string(ddagrab_extra_segs_.size() + 1) + " started)");
}

// Joins the first piece (current_output_path_) and the pieces made after Pauses into current_output_path_. Stream copy
// (no re-encode); on any failure everything stays where it is and false is returned.
bool RecordingController::JoinDdagrabSegments() {
    if (ddagrab_extra_segs_.empty()) return true;
    std::wstring stem = current_output_path_;
    std::wstring ext;
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > stem.find_last_of(L"\\/")) { ext = stem.substr(dot); stem.resize(dot); }
    const std::wstring list_path   = stem + L".parts.txt";
    const std::wstring joined_path = stem + L".joined" + ext;

    std::string list = "ffconcat version 1.0\n";
    auto add = [&](const std::wstring &p) {
        std::string n = NarrowFromWide(p);
        std::string esc;
        for (char c : n) { if (c == '\'') esc += "'\\''"; else esc += c; }
        list += "file '" + esc + "'\n";
    };
    add(current_output_path_);
    for (const auto &sp : ddagrab_extra_segs_) add(sp);
    bool wrote = false;
    if (FILE *f = _wfopen(list_path.c_str(), L"wb")) {
        wrote = std::fwrite(list.data(), 1, list.size(), f) == list.size();
        if (std::fclose(f) != 0) wrote = false;
    }
    if (!wrote) {
        HrLog::Error("Recording: couldn't write the list of recording pieces - they are left next to the output.");
        return false;
    }
    HrLog::Info("Recording: joining " + std::to_string(ddagrab_extra_segs_.size() + 1) + " ddagrab pieces (stream copy)...");
    const int rc = hr_concat_segments(ffmpeg_path_.c_str(), list_path.c_str(), joined_path.c_str());
    if (rc != 0 || !hr_path_exists(NarrowFromWide(joined_path).c_str()) ||
        hr_file_size_mb(NarrowFromWide(joined_path).c_str()) <= 0.0f) {
        HrLog::Error("Recording: joining the pieces failed - the first piece stays as the output and the other pieces "
                     "(<name>.partN.<ext>) are kept next to it.");
        RemoveFileUtf8(NarrowFromWide(joined_path));
        RemoveFileUtf8(NarrowFromWide(list_path));
        return false;
    }
    if (!MoveFileExW(joined_path.c_str(), current_output_path_.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        HrLog::Error("Recording: couldn't replace the output with the joined file - it is kept as " +
                     NarrowFromWide(joined_path));
        RemoveFileUtf8(NarrowFromWide(list_path));
        return false;
    }
    RemoveFileUtf8(NarrowFromWide(list_path));
    for (const auto &sp : ddagrab_extra_segs_) RemoveFileUtf8(NarrowFromWide(sp));
    ddagrab_extra_segs_.clear();
    return true;
}

bool RecordingController::Start(std::wstring &error_out) {
    if (state_.recording) { error_out = L"Already recording."; return false; }
    if (finalizing_) { error_out = L"The previous recording is still being saved - try again in a moment."; return false; }
    if (finalize_thread_.joinable()) finalize_thread_.join();

    // 2.4.2: Start() runs on the UI thread, so every millisecond spent in here is a frozen window.
    // Per-phase timing, written to homrec.log as ONE line when Start() returns (any way it returns),
    // so a "the UI freezes for 10-20 s when I press Record" report shows exactly which step it was.
    struct StartPhaseTimer {
        using Clock = std::chrono::steady_clock;
        Clock::time_point t0 = Clock::now(), last = t0;
        std::string line;
        bool preview_off = false;
        void mark(const char *name) {
            const auto now = Clock::now();
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
            last = now;
            if (ms >= 5) line += std::string(" ") + name + "=" + std::to_string(ms) + "ms";
        }
        ~StartPhaseTimer() {
            const long long total = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
            const std::string msg = std::string("Recording: Start() took ") + std::to_string(total) +
                                    " ms on the UI thread (preview " + (preview_off ? "off" : "on") + ")" +
                                    (line.empty() ? std::string() : std::string(" -") + line);
            if (total >= 1000) HrLog::Warn(msg); else HrLog::Info(msg);
        }
    } start_timer;
    start_timer.preview_off = state_.disable_preview;

    if (!CheckCaptureTarget(error_out)) return false;

    // Instant Replay gets paused just below, and only Stop()
    // ever resumed it - so ANY failed Start() (ffmpeg missing, low disk,
    // encoder/pipeline failed to start...) left Instant Replay silently dead.
    struct StartFailGuard {
        std::function<void()> resume;
        bool armed = true;
        ~StartFailGuard() { if (armed && resume) resume(); }
    } start_guard;

    if (instant_replay_active_) {
        StopInstantReplayEncoderAsync();
        instant_replay_active_ = false;
        HrLog::Info("Instant Replay: paused for a manual recording.");
        start_guard.resume = [this]() {
            if (!instant_replay_enabled_ || state_.recording) return;
            JoinPendingInstantReplayStop();
            std::wstring e;
            if (EnableInstantReplay(e)) {
                HrLog::Info("Instant Replay: resumed after the recording failed to start.");
            } else {
                HrLog::Error(NarrowFromWide(L"Instant Replay: failed to resume after a failed start - " + e));
            }
        };
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

    // hr_get_free_disk_mb() (ui_utils.cpp) existed already but nothing ever called it - a
    // recording would happily start on an almost-full drive and just die (ffmpeg write
    // failure) partway through instead of being refused up front.
    uint64_t free_mb = 0;
    if (hr_get_free_disk_mb(state_.output_folder.c_str(), &free_mb) && free_mb < kMinFreeDiskMb) {
        error_out = L"Not enough free disk space on the output drive (" +
                    std::to_wstring(free_mb) + L" MB free, need at least " +
                    std::to_wstring(kMinFreeDiskMb) + L" MB).";
        HrLog::Error("Start failed: low disk space (" + std::to_string(free_mb) + " MB free)");
        return false;
    }

    start_timer.mark("checks+disk");
    current_output_path_ = BuildOutputPath();

    int prev_w = capture_w_, prev_h = capture_h_;
    ResolveCaptureSize();
    start_timer.mark("resolve_size");

    // Prefer the probed GPU encoder; fall back to libx264 if none/if the
    // caller already forced a specific codec in settings.
    std::wstring codec = state_.video_codec == "libx264" && !hw_encoder_.empty()
                              ? hw_encoder_
                              : WideFromNarrow(state_.video_codec);
    std::wstring codec_args = BuildCodecArgs(codec);
    bool codec_is_hw = (codec != L"libx264" && codec != L"libx265");
    HrLog::Info("Recording: encoding with " + NarrowFromWide(codec) +
                (codec_is_hw ? " (hardware)" : " (software)"));
    // The in-process encoder shares the capture device with its own threads: every capture device created from
    // now on is made multithread-protected (a device already in use is never switched - see dxgi_capture.cpp).
    hr_dx_set_mt_protect((state_.mf_encoder && state_.mf_gpu_texture) ? 1 : 0);

    // ---- optional full-GPU path: ffmpeg's own "ddagrab" capture ------------- Settings >
    // Video > GPU acceleration.
    ddagrab_active_ = false;
    live_audio_ = false;          // decided below, per recording
    live_audio_rate_ = 0;
    crash_was_ddagrab_ = false;
    crash_video_elapsed_sec_ = 0.0;
    ddagrab_last_frames_ = -1;
    current_fps_ = 0.0;
    std::chrono::steady_clock::time_point t_video_go{};
    const bool ddagrab_started = TryStartDdagrab(codec_args, codec_is_hw, t_video_go);
    start_timer.mark("ddagrab_probe/start");
    if (ddagrab_started) {
        ddagrab_active_ = true;
        // A preview-only pipeline may keep running for the live preview, but it
        // must not be attached to a recording (nothing to pipe into).
        JoinPendingInstantReplayStop();
        if (pipeline_) {
            hr_pl_pause(pipeline_, 0);
            hr_pl_set_recording(pipeline_, /*active=*/0, /*pipe_fd=*/0);
            hr_pl_set_preview_fps(pipeline_, state_.preview_fps);
            hr_pl_set_preview_needed(pipeline_, PreviewNeededFlag());
        }
    } else {
        // In-process Media Foundation encoder instead of an ffmpeg process + pipe (see mf_encoder.h). ffmpeg is
        // then only needed afterwards, to merge the audio.
        const bool use_mf = MfEncoderWanted(codec, codec_is_hw);
        // Mix the audio while recording and give it to the encoder live (no WAV files, no merge after Stop).
        // Needs at least one capture stream; the Media Foundation path needs no ffmpeg at all, the ffmpeg path needs
        // an ffmpeg command line without custom stream / audio options.
        if (state_.inproc_audio && state_.audio_out_channels > 0 && (use_mf || !HrCustomArgsTouchAudio(state_)))
            live_audio_rate_ = hr_audio_live_prepare();
        // Non-zero "pipe" token for the pipeline in MF mode: it is never opened or closed, it only marks the
        // recording segment as open (see hr_pl_set_mf_sink()).
        constexpr intptr_t kMfPipeToken = 1;
        intptr_t ff_stdin = use_mf ? kMfPipeToken : 0;
        if (!use_mf) {
        ffproc_ = hr_ff_create();
        hr_ff_set_ffmpeg_path(ffproc_, NarrowFromWide(ffmpeg_path_).c_str());
        hr_ff_set_output_path(ffproc_, NarrowFromWide(current_output_path_).c_str());
        hr_ff_set_codec_args(ffproc_, NarrowFromWide(codec_args).c_str());
        hr_ff_set_normal_priority(ffproc_, state_.game_friendly_priority ? 1 : 0);
        hr_ff_set_gpu_low_priority(ffproc_, state_.gpu_low_priority ? 1 : 0);
        int pipe_w = output_w_;
        int pipe_h = output_h_;
        hr_ff_set_video_params(ffproc_, pipe_w, pipe_h, state_.target_fps);
        hr_ff_set_output_size(ffproc_, pipe_w, pipe_h);
        hr_ff_set_pipe_input(ffproc_, HrPipeInputMode(state_));
        if (live_audio_rate_ > 0)
            hr_ff_set_audio_pipe(ffproc_, live_audio_rate_, HrAacKbps(state_.audio_aac_bitrate));

        if (hr_ff_start(ffproc_) != 0) {
            error_out = L"Failed to start the ffmpeg process.";
            HrLog::Error("Start failed: ffmpeg process didn't start");
            hr_ff_destroy(ffproc_);
            ffproc_ = nullptr;
            return false;
        }
        start_timer.mark("ffmpeg_launch");
        // The audio pipe could not be created: this recording merges its audio after Stop, as before.
        if (live_audio_rate_ > 0 && !hr_ff_audio_pipe_active(ffproc_)) live_audio_rate_ = 0;
        HrLog::Info(HrLowImpactFlagsLine(state_));
        if (state_.gpu_low_priority)
            HrLogFfmpegGpuPriorityStart(hr_ff_gpu_priority_applied(ffproc_) != 0);

        // The pipeline writes captured frames straight into ffmpeg's stdin pipe, so it needs the
        // real write-end HANDLE ffmpeg was launched with - not a placeholder.
        ff_stdin = hr_ff_get_stdin_handle(ffproc_);
        if (ff_stdin == 0) {
            error_out = L"Failed to start the ffmpeg process.";
            HrLog::Error("Start failed: ffmpeg stdin pipe handle unavailable");
            hr_ff_kill(ffproc_);
            hr_ff_destroy(ffproc_);
            ffproc_ = nullptr;
            return false;
        }
        }   // !use_mf

        // Pipeline handles the actual DXGI capture + frame conversion + piping
        // frames into ffmpeg's stdin. pv_w/pv_h come from AppState's preview
        // panel size (set by main_window on layout).
        bool reused_preview_pipeline = false;
        // With the GPU-texture path the capture device must be one that was created multithread-protected; a
        // preview pipeline created before that was switched on is rebuilt once (window capture has no such device).
        const bool mf_needs_shared_device = use_mf && state_.mf_gpu_texture && !use_wgc_;
        if (pipeline_ && capture_w_ == prev_w && capture_h_ == prev_h &&
            pipeline_output_idx_ == capture_output_idx_ && PipelineMatchesTarget() &&
            (!mf_needs_shared_device || hr_pl_get_d3d_device(pipeline_) != nullptr)) {
            hr_pl_set_capture_rect(pipeline_, crop_x_, crop_y_, crop_w_, crop_h_);
            hr_pl_set_output_size(pipeline_, output_w_, output_h_);
            // The pixel format used to be set only AFTER recording had already been switched on, so
            // the very first frame(s) of a recording that reused the preview pipeline could go out in
            // the previous encoder's chroma layout (NV12 vs I420).
            hr_pl_pause(pipeline_, 0);
            hr_pl_set_output_pixfmt(pipeline_, codec_is_hw ? 1 : 0);
            // A reused preview pipeline keeps whatever fps it was created with, but ffmpeg above was
            // just told state_.target_fps - if they differ (e.g.
            hr_pl_set_fps(pipeline_, state_.target_fps);
            hr_pl_set_timestamped_pipe(pipeline_, (use_mf || HrPipeInputMode(state_) == 2) ? state_.target_fps : 0);
            if (use_mf && !StartMfEncoder()) {
                // Could not start: record this one with ffmpeg and do not try the in-process encoder again
                // this session. The pipeline is not recording yet, so there is nothing to undo.
                mf_blocked_ = true;
                return Start(error_out);
            }
            hr_pl_set_recording(pipeline_, /*active=*/1, ff_stdin);
            reused_preview_pipeline = true;
        } else {
            JoinPendingInstantReplayStop();
            start_timer.mark("join_replay_stop");
            if (pipeline_) { hr_pl_destroy(pipeline_); pipeline_ = nullptr; }
            start_timer.mark("destroy_old_pipeline");
            // See JoinPendingPreviewTeardown()'s comment (recording_controller.h) - without this,
            // starting a recording shortly after preview was torn down (e.g.
            JoinPendingPreviewTeardown();
            start_timer.mark("join_preview_teardown");
            int pvw = 0, pvh = 0;
            ScaledPreviewSize(pvw, pvh);
            pipeline_ = CreatePipelineForTarget(use_mf ? 0 : ff_stdin, pvw, pvh);   // MF: not recording until the encoder is attached
            start_timer.mark("create_pipeline");
            pipeline_output_idx_ = capture_output_idx_;
            last_overlays_sent_valid_ = false;
        }
        // Must be set every Start() (not only on fresh pipeline creation) -
        // a reused preview pipeline may have been left in the opposite format
        // by a previous recording that used a different encoder.
        if (pipeline_) hr_pl_set_output_pixfmt(pipeline_, codec_is_hw ? 1 : 0);
        bool pipeline_started = reused_preview_pipeline;
        if (pipeline_ && !reused_preview_pipeline) {
            hr_pl_set_capture_rect(pipeline_, crop_x_, crop_y_, crop_w_, crop_h_);
            hr_pl_set_output_size(pipeline_, output_w_, output_h_);
            pipeline_started = hr_pl_start(pipeline_) != 0;
            start_timer.mark("pipeline_start");
        }
        if (pipeline_) {
            hr_pl_set_preview_fps(pipeline_, state_.preview_fps);
            hr_pl_set_preview_needed(pipeline_, PreviewNeededFlag());
        }
        if (!pipeline_ || !pipeline_started) {
            // CreatePipelineForTarget() sets wgc_disabled_ when Windows.Graphics.Capture could not be
            // started for this window.
            const bool retry_with_crop = !pipeline_ && use_wgc_ && wgc_disabled_;
            HrLog::Error("Start failed: capture pipeline didn't start");
            hr_ff_kill(ffproc_);
            hr_ff_destroy(ffproc_);
            ffproc_ = nullptr;
            if (pipeline_) { hr_pl_destroy(pipeline_); pipeline_ = nullptr; }
            else if (!use_mf && ff_stdin != 0 && ff_stdin != -1) {
                // No pipeline was ever created, so nothing owns ffmpeg's stdin write end (the
                // ffmpeg runner deliberately leaves it to the pipeline) - close it here instead
                // of leaking one pipe handle per failed Start().
                CloseHandle(reinterpret_cast<HANDLE>(ff_stdin));
            }
            if (retry_with_crop) {
                HrLog::Warn("Window capture: Windows.Graphics.Capture couldn't start for this window - "
                            "retrying the recording with the screen-crop method (anything on top of the "
                            "window will be recorded too).");
                return Start(error_out);
            }
            error_out = L"Failed to start the capture pipeline.";
            return false;
        }

        hr_pl_set_timestamped_pipe(pipeline_, (use_mf || HrPipeInputMode(state_) == 2) ? state_.target_fps : 0);
        if (use_mf && !reused_preview_pipeline && !StartMfEncoder()) {
            // Could not start: drop this pipeline and record again through ffmpeg (this session only).
            mf_blocked_ = true;
            hr_pl_destroy(pipeline_);
            pipeline_ = nullptr;
            return Start(error_out);
        }
        hr_pl_set_recording(pipeline_, /*active=*/1, ff_stdin);
        t_video_go = std::chrono::steady_clock::now();
        if (use_mf) HrLog::Info("Recording: in-process Media Foundation encoder (no ffmpeg process for the video).");

        hr_pl_set_priority_boost(pipeline_, state_.game_friendly_priority ? 0 : 1);
        if (state_.game_friendly_priority)
            HrLog::Info("Recording: game-friendly priority is on (encoder at normal priority, "
                        "capture threads not boosted).");
        // Opt-in: the capture thread's D3D11 work (desktop copy, video-processor blit, NV12 readback copy)
        // runs in THIS process, so lower its GPU class too. Restored in FinishPipelineAfterStop().
        if (state_.gpu_low_priority && !gpu_self_lowered_) {
            gpu_self_lowered_ = hr_gpu_sched_set_process(nullptr, 1) != 0;
            if (gpu_self_lowered_) {
                HrLog::Info("Recording: HomRec capture GPU priority lowered (below normal).");
            } else {
                char st_hex[16];
                std::snprintf(st_hex, sizeof(st_hex), "0x%08lX", (unsigned long)hr_gpu_sched_last_status());
                HrLog::Info(std::string("Recording: HomRec capture GPU priority could NOT be lowered (NTSTATUS ") + st_hex +
                            ", driver/Windows refused).");
            }
        }
    }

    if (state_.audio_out_channels > 0) {
        hr_audio_set_max_buffer_sec(0);
        hr_audio_pause(0); // never start with audio left paused by a previous recording
        std::string audio_base = NarrowFromWide(current_output_path_);
        size_t audio_dot = audio_base.find_last_of('.');
        std::string audio_stem = (audio_dot == std::string::npos) ? audio_base : audio_base.substr(0, audio_dot);
        bool live_started = false;
        if (live_audio_rate_ > 0) {
            // No WAV files. The audio timeline starts at the buffer reset below; the video started
            // `skew` earlier, so that much silence goes in front (same measurement the merge step used).
            hr_audio_reset_buffers(nullptr, nullptr);
            hr_audio_set_volumes(mic_vol_, sys_vol_, mic_muted_ ? 1 : 0, sys_muted_ ? 1 : 0);
            hr_audio_extra_begin_live();
            const auto t_audio_go = std::chrono::steady_clock::now();
            av_start_skew_sec_ = std::chrono::duration<double>(t_audio_go - t_video_go).count();
            live_started = StartLiveAudio((int)std::lround(av_start_skew_sec_ * 1000.0));
            if (live_started)
                HrLog::Info("Recording: measured video/audio start skew = " + std::to_string(av_start_skew_sec_) +
                            "s (put in front of the audio as silence)");
        }
        if (!live_started) {
            std::string mic_wav_path = audio_stem + "_mic_tmp.wav";
            std::string sys_wav_path = audio_stem + "_sys.wav";
            hr_audio_reset_buffers(mic_wav_path.c_str(), sys_wav_path.c_str());
            hr_audio_set_volumes(mic_vol_, sys_vol_, mic_muted_ ? 1 : 0, sys_muted_ ? 1 : 0);
            hr_audio_extra_begin(audio_stem.c_str());   // 2.4: extra mixer channels start collecting too
            const auto t_audio_go = std::chrono::steady_clock::now();
            av_start_skew_sec_ = std::chrono::duration<double>(t_audio_go - t_video_go).count();
            HrLog::Info("Recording: measured video/audio start skew = " +
                        std::to_string(av_start_skew_sec_) + "s (applied at merge time)");
        }
    } else {
        av_start_skew_sec_ = 0.0;
    }

    start_timer.mark("audio_begin");
    hr_ctl_set_output_path(ctl_, NarrowFromWide(current_output_path_).c_str());
    hr_ctl_start(ctl_);

    start_guard.armed = false; // recording is really running now
    state_.recording = true;
    state_.paused = false;
    state_.frame_count = 0;
    last_drops_seen_ = 0;
    overload_streak_ = 0;
    overloaded_ = false;
    last_loud_time_ = std::chrono::steady_clock::now();
    auto_paused_by_silence_ = false;
    HrLog::Info("Recording started -> " + NarrowFromWide(current_output_path_) +
                " (" + std::to_string(output_w_) + "x" + std::to_string(output_h_) +
                " @ " + std::to_string(state_.target_fps) + "fps, captured at " +
                std::to_string(capture_w_) + "x" + std::to_string(capture_h_) + ")");
    return true;
}

void RecordingController::Stop() {
    // Old synchronous behavior, kept for callers that genuinely want to block until
    // everything's done (e.g.
    StopAsync(nullptr);
    if (finalize_thread_.joinable()) finalize_thread_.join();
}

void RecordingController::StopAsync(std::function<void()> on_done) {
    if (!state_.recording || finalizing_) return;
    finalizing_ = true;
    if (finalize_thread_.joinable()) finalize_thread_.join();

    // If we're keeping the pipeline alive afterward for continued live preview, don't call
    // hr_pl_stop() here - that joins the capture thread, which would freeze the preview the
    // moment Stop() runs instead of leaving it live.
    bool keep_for_preview = pipeline_ && !state_.disable_preview;

    // Sample the real recording time NOW. The finalize thread below spends seconds (up to ~30 s)
    // waiting for ffmpeg and mixing audio while hr_ctl is still in RECORDING state, so reading
    // elapsed_seconds() there returned "recording time + finalize time" - which made short
    // recordings look "too long" for their video and got them wrongly slow-motion-stretched
    // by hr_merge_av(), and inflated the duration shown in the summary.
    const double recorded_sec = elapsed_seconds();

    // hr_pl_stop() itself waits (with a timeout) for the capture thread and then the writer
    // thread to each signal that they've actually stopped - up to ~1s per thread, by design
    // (see its own comment in pipeline.cpp).
    hr_ff_stop_graceful(ffproc_);

    finalize_thread_ = std::thread([this, keep_for_preview, recorded_sec, on_done]() {
    // Everything below (stopping the pipeline, mixing/merging audio, the mkv->mp4 copy) is not time-critical, and the
    // user is usually back in the game by now: keep this thread out of the game's way.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    // The live mixer hands its last audio to the encoder BEFORE the video side is ended (the in-process
    // encoder is finalized by the pipeline's writer thread when the recording segment closes below, and ffmpeg only
    // opens its audio input after the video input is done).
    StopLiveAudio();
    if (ddagrab_active_) {
        // ffmpeg captured by itself (no pipe was ever attached to this recording), so there is no
        // recording segment to close; the graceful "q" sent above is all it needs.
        if (!keep_for_preview) hr_pl_stop(pipeline_);
    } else if (!keep_for_preview) {
        // hr_pl_stop() abandons whatever is still queued for ffmpeg (the writer thread's loop ends
        // as soon as writer_running goes false), so the last <=8 queued frames - and the timeline
        // slots they carry for a static screen - were lost from the end of every recording made
        // with the live preview disabled. Close the segment first (the writer drains the queue
        // and closes ffmpeg's stdin), exactly like the preview-on path does, then stop the threads.
        if (pipeline_) hr_pl_end_recording_segment(pipeline_, 3000);
        hr_pl_stop(pipeline_);
    } else {
        // (recordings always took ~30s to "finalize" and came out
        // corrupt whenever preview was left on - see
        // hr_pl_end_recording_segment()'s comment for the full story):
        hr_pl_end_recording_segment(pipeline_, 3000);
    }
    FinishPipelineAfterStop(keep_for_preview);
    StopFinalizeTail(keep_for_preview, recorded_sec);
    finalizing_ = false;
    if (on_done) on_done();
    });
}

void RecordingController::FinishPipelineAfterStop(bool keep_for_preview) {
    // Stop while paused left the pipeline (and audio) paused. With the
    // live preview on the pipeline is kept alive, so the preview froze and the NEXT
    // recording reused a paused pipeline and captured nothing.
    if (pipeline_) hr_pl_pause(pipeline_, 0);
    hr_audio_pause(0);
    // In-process encoder: the writer thread detaches it by itself when the segment ends; this only matters when
    // that timed out (the encoder object stays alive until StopFinalizeTail() ends and destroys it).
    if (pipeline_ && mfenc_) hr_pl_set_mf_sink(pipeline_, nullptr, 0);
    if (pipeline_) hr_pl_set_priority_boost(pipeline_, 0);
    // Back to the normal GPU class once the recording is over (live preview / idle capture are cheap).
    if (gpu_self_lowered_) {
        hr_gpu_sched_set_process(nullptr, 2);
        gpu_self_lowered_ = false;
    }

    if (instant_replay_enabled_) {
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
}

void RecordingController::StopFinalizeTail(bool /*keep_for_preview*/, double recorded_sec) {
    // Where does the wait after Stop go? One summary line at the end of this function (homrec.log).
    const auto fin_t0 = std::chrono::steady_clock::now();
    auto fin_lap = fin_t0;
    auto fin_ms = [&fin_lap]() {
        const auto now = std::chrono::steady_clock::now();
        const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - fin_lap).count();
        fin_lap = now;
        return ms;
    };
    long long ms_ffwait = 0, ms_audio = 0, ms_merge = 0, ms_mp3 = 0, ms_mp4 = 0;
    // This recording's audio went to the encoder live - there are no WAVs to mix and nothing to merge.
    const bool live_rec = live_audio_rate_ > 0;

    JoinPendingInstantReplayStop();

    if (mfenc_) {
        // In-process encoder: the pipeline's writer thread ended it when the recording segment closed (see
        // hr_pl_end_recording_segment() above); this call only matters if that never happened (segment-close
        // timeout) and is a no-op otherwise. Then wait for the file to be finalized (moov atom written).
        hr_mf_end(mfenc_, 0);
        int mw = hr_mf_wait(mfenc_, 10000);
        if (mw == -1) {
            HrLog::Warn("Recording: the Media Foundation encoder is still finalizing the output after 10s -- "
                        "waiting longer before giving up.");
            mw = hr_mf_wait(mfenc_, 20000);
        }
        if (mw == -1) {
            HrLog::Error("Recording: the Media Foundation encoder didn't finish after 30s total -- the output "
                         "file may be incomplete/corrupt.");
        } else if (mw == -2 || hr_mf_failed(mfenc_)) {
            char err[256] = {};
            hr_mf_last_error(mfenc_, err, (int)sizeof(err));
            HrLog::Error(std::string("Recording: the Media Foundation encoder reported an error: ") +
                         (err[0] ? err : "unknown") + " -- the output file may be incomplete/corrupt.");
        } else {
            HrLog::Info("Recording: Media Foundation encoder finished (" +
                        std::to_string((long long)hr_mf_frames_written(mfenc_)) + " frames written).");
        }
    }
    // ffmpeg reads the tail of the audio pipe once its video input has ended (the segment is closed by now).
    FinishAudioSink();
    // This used to be hr_ff_wait(ffproc_, 3000) -- a flat 3 second budget for ffmpeg to
    // receive EOF on stdin, flush libx264's internal frame buffer, and write the moov atom
    // (mp4) / cues (mkv) that make the file actually playable.
    if (hr_ff_wait(ffproc_, 10000) != 0 && hr_ff_is_running(ffproc_)) {
        HrLog::Warn("Recording: ffmpeg is still finalizing the output after 10s "
                    "(large/long recording, or the machine is under heavy load) -- "
                    "waiting longer before giving up.");
        if (hr_ff_wait(ffproc_, 20000) != 0 && hr_ff_is_running(ffproc_)) {
            HrLog::Error("Recording: ffmpeg didn't finish on its own after 30s total -- "
                        "force-stopping it now. The output file may be incomplete/corrupt.");
            // The message above used to be all that happened: ffmpeg kept running (and kept the
            // output file open) while the audio was merged into / remuxed from that very file.
            hr_ff_kill(ffproc_);
        }
    }
    ms_ffwait = fin_ms();

    // The overlay feeder is only a helper of the ffmpeg that just ended; and a recording that was paused
    // consists of several ffmpeg pieces, joined now (stream copy) so everything below sees ONE finished file.
    DestroyOverlayFeed();
    bool ddagrab_joined = false;
    if (ddagrab_active_ && !ddagrab_extra_segs_.empty()) ddagrab_joined = JoinDdagrabSegments();
    ddagrab_gap_ = false;

    // mic/system audio were captured into separate temp WAVs (unless muted); mix them into one
    // file, then remux that into the finished video via hr_merge_av, the native fast path used
    // when no custom bitrate/extra ffmpeg args are set.
    std::string base = NarrowFromWide(current_output_path_);
    size_t dot = base.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? base : base.substr(0, dot);
    std::string mic_wav = stem + "_mic_tmp.wav";
    std::string sys_wav = stem + "_sys.wav";
    std::string audio_wav = stem + "_audio.wav";

    bool want_mic = !mic_muted_;
    bool want_sys = !sys_muted_;
    int audio_result = live_rec ? 0
                                : hr_audio_capture_to_wav(want_mic ? mic_wav.c_str() : nullptr,
                                                          want_sys ? sys_wav.c_str() : nullptr);
    // Incremental streaming (see hr_audio_reset_buffers()) always opens both
    // WAVs at Start(), so a channel that's muted here still left a silent
    // file behind - remove it so no stray *_mic_tmp.wav/_sys.wav remains.
    if (!want_mic) RemoveFileUtf8(mic_wav);
    if (!want_sys) RemoveFileUtf8(sys_wav);
    bool mic_written = want_mic && (audio_result & 0x1) && hr_path_exists(mic_wav.c_str());
    bool sys_written = want_sys && (audio_result & 0x2) && hr_path_exists(sys_wav.c_str());

    bool have_audio_file = false;
    if (mic_written && sys_written) {
        // The two raw tracks are only deleted once the mixed file really exists and isn't a stub: a
        // "successful" mix of unreadable inputs used to leave an empty file and no source audio.
        if (hr_audio_mix_wav(mic_wav.c_str(), sys_wav.c_str(), audio_wav.c_str()) == 0 &&
            hr_file_size_mb(audio_wav.c_str()) > 0.01f) {
            RemoveFileUtf8(mic_wav);
            RemoveFileUtf8(sys_wav);
            have_audio_file = true;
        } else {
            // Mixing failed - fall back to whichever single track exists (mic preferred). The other
            // track is KEPT next to the recording, not deleted.
            RemoveFileUtf8(audio_wav);
            HrLog::Error("Recording: couldn't mix the microphone and system audio -- keeping '" + sys_wav +
                         "' (system audio) next to the recording.");
            RenameFileUtf8(mic_wav, audio_wav);
            have_audio_file = true;
        }
    } else if (mic_written) {
        RenameFileUtf8(mic_wav, audio_wav);
        have_audio_file = true;
    } else if (sys_written) {
        RenameFileUtf8(sys_wav, audio_wav);
        have_audio_file = true;
    }

    // 2.4 - extra Audio Mixer channels (another mic, a browser / window, audio files):
    // finish their WAVs and fold them into the audio track with the streaming mixer.
    if (!live_rec && state_.audio_out_channels > 0) {
        std::vector<char> xbuf(32768, '\0');
        const int xn = hr_audio_extra_collect(xbuf.data(), (int)xbuf.size());
        if (xn > 0) {
            const std::string extras(xbuf.data());
            std::string inputs = have_audio_file ? (audio_wav + "|" + extras) : extras;
            const std::string mixed = stem + "_audiomix.wav";
            if (hr_audio_mix_wav_list(inputs.c_str(), mixed.c_str()) == 0) {
                if (have_audio_file) RemoveFileUtf8(audio_wav);
                RenameFileUtf8(mixed, audio_wav);
                have_audio_file = true;
            } else {
                HrLog::Warn("Recording: couldn't mix the extra audio sources into the recording.");
                RemoveFileUtf8(mixed);
            }
            // The per-channel temp files are no longer needed either way.
            size_t from = 0;
            while (from <= extras.size()) {
                size_t bar = extras.find('|', from);
                std::string one = extras.substr(from, bar == std::string::npos ? std::string::npos : bar - from);
                if (!one.empty()) RemoveFileUtf8(one);
                if (bar == std::string::npos) break;
                from = bar + 1;
            }
        }
    }

    ms_audio = fin_ms();

    bool merged = false;
    bool mp4_done_in_merge = false;   // "Both": the .mp4 copy was written by the merge pass itself
    const std::string mp4_path_both = stem + ".mp4";
    if (have_audio_file && state_.audio_out_channels > 0 && ffmpeg_found_ &&
        hr_path_exists(base.c_str())) {
        const double merge_elapsed = (crash_video_elapsed_sec_ > 0.0) ? 0.0 : recorded_sec;
        const bool want_mp4_now = state_.video_format == VideoFormat::Both;
        const std::wstring mp4_w = WideFromNarrow(mp4_path_both);
        int mp4_flag = 0;
        merged = hr_merge_av_ex(ffmpeg_path_.c_str(), current_output_path_.c_str(),
                                 WideFromNarrow(audio_wav).c_str(),
                                 merge_elapsed,
                                 av_start_skew_sec_,
                                 want_mp4_now ? mp4_w.c_str() : nullptr,
                                 &mp4_flag) != 0;
        mp4_done_in_merge = merged && mp4_flag != 0;
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

    ms_merge = fin_ms();

    // "Also save audio as a separate MP3" (Settings) was persisted (see hrc_config.cpp) and
    // shown in the dialog, but nothing anywhere ever read state_.separate_audio_mp3 or
    // produced an .mp3 -- the checkbox did nothing at all.
    if (live_rec) {
        // The sound is already inside the video: the separate MP3 is cut out of the finished file (no WAV exists).
        if (state_.separate_audio_mp3 && ffmpeg_found_ && hr_path_exists(base.c_str())) {
            const std::string mp3_path = stem + ".mp3";
            if (hr_export_mp3(ffmpeg_path_.c_str(), WideFromNarrow(base).c_str(),
                              WideFromNarrow(mp3_path).c_str())) {
                HrLog::Info("Recording: separate audio saved -> " + mp3_path);
            } else {
                HrLog::Warn("Recording: couldn't export a separate MP3 from the recording.");
            }
        }
    } else if (merged) {
        if (state_.separate_audio_mp3 && ffmpeg_found_) {
            std::string mp3_path = stem + ".mp3";
            if (hr_export_mp3(ffmpeg_path_.c_str(), WideFromNarrow(audio_wav).c_str(),
                               WideFromNarrow(mp3_path).c_str())) {
                RemoveFileUtf8(audio_wav);
            } else {
                HrLog::Warn("Recording: couldn't export a separate MP3 -- leaving '" +
                            audio_wav + "' (WAV) instead.");
            }
        } else {
            RemoveFileUtf8(audio_wav);
        }
    }

    ms_mp3 = fin_ms();

    // 2.4 - Container "mp4 + mkv": the recording itself was written as .mkv;
    // stream-copy an .mp4 next to it now that audio is merged (no re-encode). When audio was merged
    // in the same pass as the .mp4 (see hr_merge_av_ex above) there is nothing left to copy.
    if (state_.video_format == VideoFormat::Both && mp4_done_in_merge) {
        HrLog::Info("Recording: also saved an .mp4 copy (same pass as the audio merge) -> " + mp4_path_both);
    } else if (state_.video_format == VideoFormat::Both && ffmpeg_found_ && hr_path_exists(base.c_str())) {
        const std::string mp4_path = stem + ".mp4";
        if (hr_remux_copy(ffmpeg_path_.c_str(), WideFromNarrow(base).c_str(),
                           WideFromNarrow(mp4_path).c_str())) {
            HrLog::Info("Recording: also saved an .mp4 copy -> " + mp4_path);
        } else {
            HrLog::Warn("Recording: couldn't create the .mp4 copy - the .mkv is intact.");
        }
    }
    ms_mp4 = fin_ms();
    HrLog::Info("Recording: finalize took " +
                std::to_string((long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - fin_t0).count()) +
                " ms (encoder flush " + std::to_string(ms_ffwait) + ", audio files/mix " + std::to_string(ms_audio) +
                ", A/V merge " + std::to_string(ms_merge) + ", mp3 " + std::to_string(ms_mp3) +
                ", mp4 copy " + std::to_string(ms_mp4) + ").");

    // hr_ctl_stop()'s return value is "elapsed seconds for final summary" per its own doc
    // comment, but this used to be called as a bare statement that threw the result away.
    // hr_ctl_stop() still has to run (it flips the session back to IDLE), but its return value now
    // includes the finalize time - the duration sampled at Stop is the real one.
    const double ctl_elapsed = hr_ctl_stop(ctl_);
    last_duration_sec_ = recorded_sec > 0.0 ? recorded_sec : ctl_elapsed;
    last_output_path_ = current_output_path_;
    last_output_size_mb_ = mfenc_ ? hr_mf_output_size_mb(mfenc_)
                                  : (ffproc_ ? hr_ff_output_size_mb(ffproc_) : 0.0);
    if (ddagrab_joined) last_output_size_mb_ = hr_file_size_mb(NarrowFromWide(current_output_path_).c_str());

    hr_ff_destroy(ffproc_);
    ffproc_ = nullptr;
    if (mfenc_) { hr_mf_destroy(mfenc_); mfenc_ = nullptr; }
    live_audio_rate_ = 0;

    state_.recording = false;
    state_.paused = false;
    overloaded_ = false;
    overload_streak_ = 0;
    HrLog::Info("Recording stopped -> " + base);

    RunPostRecordHook(last_output_path_);
}

void RecordingController::RunPostRecordHook(const std::wstring &output_path) {
    if (!state_.post_record_hook_enabled) return;
    if (state_.post_record_hook_type == AppState::PostRecordHookType::None) return;
    if (output_path.empty() || !hr_path_exists(NarrowFromWide(output_path).c_str())) {
        HrLog::Warn("Recording: post-record hook skipped - no finished output file to act on.");
        return;
    }

    switch (state_.post_record_hook_type) {
    case AppState::PostRecordHookType::OpenFolder: {
        std::wstring folder = WideFromNarrow(state_.output_folder);
        HINSTANCE r = ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) {
            HrLog::Error("Recording: post-record hook couldn't open the output folder.");
        }
        break;
    }
    case AppState::PostRecordHookType::Move: {
        std::wstring dest_dir = WideFromNarrow(state_.post_record_hook_path);
        if (dest_dir.empty()) {
            HrLog::Warn("Recording: post-record hook is set to \"move\" but no destination folder is configured.");
            break;
        }
        // Best-effort create - same tolerant approach hr_make_output_dir()
        // takes for output_folder itself (already-exists is not an error).
        CreateDirectoryW(dest_dir.c_str(), nullptr);
        size_t slash = output_path.find_last_of(L"\\/");
        std::wstring filename = (slash == std::wstring::npos) ? output_path : output_path.substr(slash + 1);
        if (!dest_dir.empty() && dest_dir.back() != L'\\' && dest_dir.back() != L'/') dest_dir += L'\\';
        std::wstring dest_path = dest_dir + filename;
        if (!MoveFileExW(output_path.c_str(), dest_path.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            HrLog::Error("Recording: post-record hook couldn't move the output file to " +
                         NarrowFromWide(dest_dir));
        } else {
            HrLog::Info("Recording: post-record hook moved the output file to " + NarrowFromWide(dest_dir));
        }
        break;
    }
    case AppState::PostRecordHookType::Script: {
        std::wstring script = WideFromNarrow(state_.post_record_hook_path);
        if (script.empty()) {
            HrLog::Warn("Recording: post-record hook is set to \"script\" but no script path is configured.");
            break;
        }
        // Fire-and-forget: quoted script path, then the finished file's full path as argv[1], same
        // convention as passing a file to handle on a command line generally.
        std::wstring cmdline = L"\"" + script + L"\" \"" + output_path + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
        buf.push_back(L'\0');
        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                            0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            HrLog::Info("Recording: post-record hook launched " + NarrowFromWide(script));
        } else {
            HrLog::Error("Recording: post-record hook couldn't launch " + NarrowFromWide(script) +
                         " (error " + std::to_string(GetLastError()) + ")");
        }
        break;
    }
    default:
        break;
    }
}

void RecordingController::TogglePause() {
    if (!state_.recording) return;
    if (ddagrab_active_) {
        // ffmpeg owns the capture in this mode and cannot be paused (pausing only the audio would desynchronize
        // the recording). Pause closes the current ffmpeg piece, Resume starts the next one, Stop joins them.
        if (!state_.ddagrab_pause) {
            if (!ddagrab_pause_warned_) {
                ddagrab_pause_warned_ = true;
                HrLog::Warn("Pause is switched off for full-GPU (ddagrab) recordings (ddagrab_pause=0) -- "
                            "turn off \"Full-GPU capture via ffmpeg ddagrab\" in Settings > Video "
                            "to be able to pause.");
            }
            return;
        }
        if (finalizing_ || !ffproc_) return;
        if (!state_.paused) DdagrabPause(); else DdagrabResume();
        return;
    }
    int new_state = hr_ctl_pause_toggle(ctl_);
    state_.paused = (new_state == 2 /* HR_STATE_PAUSED */);
    if (pipeline_) hr_pl_pause(pipeline_, state_.paused ? 1 : 0);
    // Audio was never paused (hr_audio_pause() was declared but not called anywhere), so it
    // kept recording while the video timeline stood still and everything after a pause was out
    // of sync by the pause length.
    hr_audio_pause(state_.paused ? 1 : 0);
    // Resuming counts as "just heard something" - otherwise the silence that
    // built up while paused made auto-pause fire again immediately.
    if (!state_.paused) last_loud_time_ = std::chrono::steady_clock::now();
    HrLog::Info(state_.paused ? "Recording paused" : "Recording resumed");
}

void RecordingController::PollStats() {
    if (!state_.recording) return;
    if (mfenc_ && !finalizing_ && hr_mf_failed(mfenc_)) {
        char err[256] = {};
        hr_mf_last_error(mfenc_, err, (int)sizeof(err));
        HrLog::Error(std::string("Recording: the in-process (Media Foundation) encoder stopped mid-recording: ") +
                     (err[0] ? err : "unknown error") + " - flagging this as a crash so the UI can stop and "
                     "finalize what was captured.");
        mf_blocked_ = true;   // the next recording uses ffmpeg
        crash_was_ddagrab_ = false;
        crash_video_elapsed_sec_ = elapsed_seconds();
        crashed_ = true;
        return;
    }
    if (ffproc_ && !finalizing_ && !ddagrab_gap_ && !hr_ff_is_running(ffproc_)) {
        HrLog::Error("Recording: the ffmpeg process ended unexpectedly mid-recording "
                     "(crashed, was closed externally, or hit a fatal error) - flagging "
                     "this as a crash so the UI can stop and finalize what was captured.");
        crash_was_ddagrab_ = ddagrab_active_;
        crash_video_elapsed_sec_ = elapsed_seconds();   // so the finalize step doesn't "stretch" a short video
        LogFfmpegDeathDetails(ffproc_, ddagrab_active_
            ? "Recording: the full-GPU (ddagrab) ffmpeg died mid-recording"
            : "Recording: the ffmpeg encoder died mid-recording");
        if (ddagrab_active_) {
            // ddagrab can't follow a display mode change (a game switching to its own fullscreen
            // resolution, alt-tab out of it, ...). Don't use it again this session: the UI restarts
            // the recording right away and the normal pipeline stretches frames to a fixed size.
            ddagrab_blocked_ = true;
            HrLog::Warn("Recording: full-GPU (ddagrab) capture is switched off for the rest of this "
                        "session - the next recording uses the normal capture pipeline.");
        }
        crashed_ = true;
        return;
    }

    long long frames = 0, drops = 0;
    double fps = 0.0;
    if (ddagrab_active_) {
        // ffmpeg captures by itself: HomRec's pipeline sees no frames, so the number comes from
        // ffmpeg's own -progress file (frames encoded per second, over >= 1 s windows).
        const long long fr = ffproc_ ? hr_ff_progress_frames(ffproc_) : -1;
        if (fr >= 0) {
            const auto now = std::chrono::steady_clock::now();
            if (ddagrab_last_frames_ < 0) {
                ddagrab_last_frames_ = fr;
                ddagrab_last_frames_t_ = now;
            } else {
                const double dt = std::chrono::duration<double>(now - ddagrab_last_frames_t_).count();
                if (dt >= 1.0) {
                    current_fps_ = (double)(fr - ddagrab_last_frames_) / dt;
                    ddagrab_last_frames_ = fr;
                    ddagrab_last_frames_t_ = now;
                }
            }
            state_.frame_count = (long)(ddagrab_frame_base_ + fr);
        }
        if (ovfeed_) hr_ovf_set_current_fps(ovfeed_, current_fps_);   // for text overlays that show the FPS
        // The overlay feeder ended while ffmpeg still runs (nothing breaks - ffmpeg holds the last overlay
        // picture - but the overlays stop updating). Say so once.
        if (ovfeed_ && !ddagrab_gap_ && hr_ovf_state(ovfeed_) < 0 && hr_ff_is_running(ffproc_)) {
            HrLog::Warn("Recording: the ddagrab overlay feed stopped - overlays keep showing their last picture.");
            DestroyOverlayFeed();
        }
    } else {
        if (pipeline_) hr_pl_stats(pipeline_, &frames, &drops, &fps);
        state_.frame_count = (long)frames;
        current_fps_ = fps;
    }

    // Overload warning: a few consecutive ticks of new drops while actually recording (not
    // paused) means the capture/encode pipeline can't keep up in real time, as opposed to one
    // stray drop under a momentary hiccup.
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

    double size_mb = mfenc_ ? hr_mf_output_size_mb(mfenc_) : (ffproc_ ? hr_ff_output_size_mb(ffproc_) : 0.0);
    if (ddagrab_active_) size_mb += ddagrab_done_mb_;      // + the pieces closed by Pause
    hr_ctl_update_stats(ctl_, (long long)(size_mb * 1024.0 * 1024.0));

    hr_audio_get_levels(&mic_level_, &sys_level_);

    // Drain buffered mic/system PCM to the incremental WAV streams so RAM
    // use stays flat for the whole recording (see audio.cpp
    // hr_audio_flush_buffered()). Cheap no-op when no stream is open.
    if (!finalizing_) hr_audio_flush_buffered();

    // Auto-pause/resume on mic silence (todo2.3.md section 3).
    if (state_.auto_pause_on_silence && !mic_muted_) {
        double threshold_rms = 32768.0 * std::pow(10.0, state_.silence_threshold_db / 20.0);
        int threshold_level = (int)(threshold_rms / 150.0);
        auto now = std::chrono::steady_clock::now();
        if (mic_level_ >= threshold_level) {
            last_loud_time_ = now;
            if (auto_paused_by_silence_ && state_.paused) {
                TogglePause();
                auto_paused_by_silence_ = false;
                HrLog::Info("Recording: mic is active again - auto-resumed.");
            }
        } else if (!state_.paused) {
            double quiet_for = std::chrono::duration<double>(now - last_loud_time_).count();
            if (quiet_for >= (double)state_.silence_duration_sec) {
                TogglePause();
                auto_paused_by_silence_ = true;
                HrLog::Info("Recording: mic has been silent for " +
                            std::to_string(state_.silence_duration_sec) + "s - auto-paused.");
            }
        }
    }
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
    RetargetWindowCapture();

    // "Cursor" setting - cheap atomic store, fine to re-apply every tick rather than needing
    // its own change-tracking like the overlay list below (which is expensive enough per-tick
    // to be worth skipping when unchanged).
    hr_pl_set_include_cursor(pipeline_, (state_.cursor_enabled && state_.desktop_visible) ? 1 : 0);
    // GPU colour conversion (Settings > Video): only ever takes effect for NV12
    // output, i.e. hardware encoders - the capture loop checks that itself.
    hr_pl_set_gpu_convert(pipeline_, state_.gpu_convert ? 1 : 0);
    // Overlays blended on the GPU when the GPU path is in use (falls back to the CPU by itself).
    hr_pl_set_gpu_overlay(pipeline_, state_.gpu_overlay ? 1 : 0);

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

    // Rebuilt into a member scratch vector (capacity is kept between ticks)
    std::vector<HrOverlayDesc> &descs = overlays_scratch_;
    descs.clear();
    descs.reserve(state_.overlays.size());
    for (auto &ov : state_.overlays) {
        HrOverlayDesc d{};
        std::strncpy(d.type, ov.type.c_str(), sizeof(d.type) - 1);
        if (ov.type == "region") {
            // monitor-relative pixels of the rectangle that is cut out of the captured monitor picture
            d.src_x = ov.rx - state_.monitor_left;
            d.src_y = ov.ry - state_.monitor_top;
            d.src_w = ov.rw;
            d.src_h = ov.rh;
        } else if (ov.type == "window") {
            ResolveWindowLayer(ov, d);
        }
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
    // The Desktop layer's closed eye. Appended at the END on purpose: the compositor keeps its per-layer
    // caches (webcams, GIFs...) by list position, and this entry must not shift everyone else's.
    if (!state_.desktop_visible) {
        HrOverlayDesc d{};
        std::strncpy(d.type, "desktop_hidden", sizeof(d.type) - 1);
        d.visible = 1;
        d.w = capture_w_ > 0 ? capture_w_ : 1;
        d.h = capture_h_ > 0 ? capture_h_ : 1;
        d.opacity = 255;
        descs.push_back(d);
    }

    // SyncOverlays() runs on every preview timer tick (continuously, not just while
    // recording/dragging), but the overlay list itself only actually changes on a real
    // add/remove/drag/resize/edit.
    bool same = last_overlays_sent_valid_ &&
                last_overlays_sent_.size() == descs.size() &&
                (descs.empty() ||
                 std::memcmp(last_overlays_sent_.data(), descs.data(),
                             sizeof(HrOverlayDesc) * descs.size()) == 0);
    if (same) return;

    hr_pl_set_overlays(pipeline_, descs.empty() ? nullptr : descs.data(), (int)descs.size());
    last_overlays_sent_.swap(descs); // descs (== overlays_scratch_) keeps the old buffer for reuse
    last_overlays_sent_valid_ = true;
    // a ddagrab recording blends the same overlays through ffmpeg; its feeder gets the same list.
    if (ovfeed_)
        hr_ovf_set_overlays(ovfeed_, last_overlays_sent_.empty() ? nullptr : last_overlays_sent_.data(),
                            (int)last_overlays_sent_.size());
}

void RecordingController::ResolveWindowLayer(OverlayDef &ov, HrOverlayDesc &d) {
    if (!ov.visible) return;                       // a hidden layer is not followed
    LayerResolveState &st = layer_resolve_[ov.id];
    const auto now = std::chrono::steady_clock::now();
    if (now >= st.next) {
        HWND hw = nullptr;
        RECT r{};
        std::string how;
        if (HR_ResolveWindowTarget(HrWindowTargetFromLayer(ov), hw, r, &how) && hw) {
            st.found = true;
            st.rect = r;
            if (hw != ov.win_hwnd) {
                ov.win_hwnd = hw;
                if (ov.win_exe.empty()) ov.win_exe = HR_WindowExeName(hw);
                if (ov.win_class.empty()) ov.win_class = HR_WindowClassName(hw);
                const std::string msg = "Window layer '" + ov.id + "': now showing '" + HR_WindowTitleUtf8(hw) +
                                        "' (" + HR_WindowExeName(hw) + " / " + HR_WindowClassName(hw) +
                                        "), found by: " + how;
                if (msg != st.logged) { st.logged = msg; HrWgcLog::Info(msg); }
            }
            st.next = now + std::chrono::milliseconds(33);
        } else {
            st.found = false;
            if (ov.win_hwnd && !IsWindow(ov.win_hwnd)) ov.win_hwnd = nullptr;
            st.next = now + std::chrono::milliseconds(1000);   // searching = enumerating every window
        }
    }
    if (!st.found) return;                         // d.src_w == 0 -> the compositor draws nothing
    d.src_x = (int)st.rect.left - state_.monitor_left;
    d.src_y = (int)st.rect.top - state_.monitor_top;
    d.src_w = (int)(st.rect.right - st.rect.left);
    d.src_h = (int)(st.rect.bottom - st.rect.top);
    d.win_client_only = ov.win_area == "client" ? 1 : 0;
    d.win_fps_cap = ov.win_fps_cap;
    if (ov.win_method == "wgc" && ov.win_hwnd && HrWgcSupported()) {
        d.win_method = 1;
        d.hwnd = (long long)(intptr_t)ov.win_hwnd;
    }
}

// The RAM buffer is MPEG-TS, which only carries H.264 / HEVC (and MPEG-2 / 4) here.
static bool ReplayRamCodecOk(const std::wstring &codec) {
    std::wstring lc = codec;
    std::transform(lc.begin(), lc.end(), lc.begin(), ::towlower);
    return lc.find(L"264") != std::wstring::npos || lc.find(L"hevc") != std::wstring::npos ||
           lc.find(L"265") != std::wstring::npos;
}

bool RecordingController::StartInstantReplayEncoder(std::wstring &error_out) {
    int buf_sec = state_.replay_buffer_sec > 0 ? state_.replay_buffer_sec : 30;
    replay_applied_buffer_sec_ = buf_sec;
    int wrap = (buf_sec + kReplaySegmentSec - 1) / kReplaySegmentSec + 1;
    if (wrap > 720) wrap = 720;
    if (wrap < 2) wrap = 2;

    std::wstring codec = state_.video_codec == "libx264" && !hw_encoder_.empty()
                              ? hw_encoder_ : WideFromNarrow(state_.video_codec);
    std::wstring codec_args = BuildCodecArgs(codec);
    bool codec_is_hw = (codec != L"libx264" && codec != L"libx265");
    const int fps = state_.target_fps > 0 ? state_.target_fps : 30;

    // RAM buffer when it is on, short enough and the codec fits into MPEG-TS; any failure to start it
    // falls through to the segment files below (the old behaviour).
    const bool ram_wanted = state_.replay_in_ram && buf_sec <= kReplayRamMaxSec && ReplayRamCodecOk(codec);

    // Starts the replay ffmpeg (RAM variant or segment-file variant) and checks its stdin pipe. On failure
    // nothing is left behind and `why` says what happened.
    auto launch = [&](bool ram, std::wstring &why) -> bool {
        replay_ff_ = hr_ff_create();
        hr_ff_set_ffmpeg_path(replay_ff_, NarrowFromWide(ffmpeg_path_).c_str());
        hr_ff_set_codec_args(replay_ff_, NarrowFromWide(codec_args).c_str());
        hr_ff_set_video_params(replay_ff_, output_w_, output_h_, state_.target_fps);
        hr_ff_set_output_size(replay_ff_, output_w_, output_h_);
        hr_ff_set_pipe_input(replay_ff_, HrPipeInputMode(state_));
        if (ram) {
            // MPEG-TS on stdout. -g = one GOP per second, so the window can be trimmed in 1 s steps (this overrides a
            // longer -g from the codec args for the replay encoder only); PAT/PMT at every key picture keeps every cut
            // chunk self-contained.
            hr_ff_set_output_path(replay_ff_, "pipe:1");
            hr_ff_set_capture_stdout(replay_ff_, 1);
            hr_ff_set_extra_output_args(replay_ff_, ("-g " + std::to_string(fps) +
                " -f mpegts -muxdelay 0 -muxpreload 0 -flush_packets 1 -mpegts_flags +pat_pmt_at_frames").c_str());
        } else {
            wchar_t tmp[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            std::wstring base_dir = std::wstring(tmp) + L"HomRec_replay\\";
            CreateDirectoryW(base_dir.c_str(), nullptr); // fine if it already exists
            replay_dir_ = base_dir + std::to_wstring(++replay_run_seq_) + L"\\";
            CreateDirectoryW(replay_dir_.c_str(), nullptr);
            hr_ff_set_output_path(replay_ff_, NarrowFromWide(replay_dir_ + L"seg_%03d.mp4").c_str());
            std::string seg_args = "-f segment -segment_time " + std::to_string(kReplaySegmentSec) +
                                    " -segment_wrap " + std::to_string(wrap) + " -reset_timestamps 1";
            hr_ff_set_extra_output_args(replay_ff_, seg_args.c_str());
        }

        if (hr_ff_start(replay_ff_) != 0) {
            why = L"Failed to start the Instant Replay encoder.";
            HrLog::Error(ram ? "Instant Replay: ffmpeg (RAM buffer) process didn't start"
                             : "Instant Replay: ffmpeg segment-writer process didn't start");
            hr_ff_destroy(replay_ff_);
            replay_ff_ = nullptr;
            return false;
        }
        intptr_t stdin_h = hr_ff_get_stdin_handle(replay_ff_);
        if (stdin_h == 0) {
            why = L"Failed to start the Instant Replay encoder.";
            HrLog::Error("Instant Replay: ffmpeg stdin pipe handle unavailable");
            hr_ff_kill(replay_ff_);
            hr_ff_destroy(replay_ff_);
            replay_ff_ = nullptr;
            return false;
        }
        if (ram) {
            size_t cap = (size_t)(state_.replay_ram_mb < 64 ? 64 : (state_.replay_ram_mb > 16384 ? 16384 : state_.replay_ram_mb)) << 20;
            replay_ram_ = std::make_shared<ReplayRam>(replay_ff_, (double)buf_sec, cap);
            replay_ram_->start();
        }
        hr_pl_set_output_pixfmt(pipeline_, codec_is_hw ? 1 : 0);
        hr_pl_set_timestamped_pipe(pipeline_, HrPipeInputMode(state_) == 2 ? state_.target_fps : 0);
        hr_pl_set_recording(pipeline_, /*active=*/1, stdin_h);
        return true;
    };

    bool started = false;
    replay_ram_.reset();
    if (ram_wanted) {
        std::wstring why;
        started = launch(true, why);
        if (started)
            HrLog::Info("Instant Replay: buffer is kept in RAM (MPEG-TS, up to " + std::to_string(state_.replay_ram_mb) + " MB) - no temp files.");
        else
            HrLog::Warn("Instant Replay: the RAM buffer could not start - using segment files in %TEMP% instead.");
    } else if (state_.replay_in_ram) {
        HrLog::Info(std::string("Instant Replay: RAM buffer not used (") +
                    (buf_sec > kReplayRamMaxSec ? "buffer longer than 300 s" : "codec is not H.264 / HEVC") +
                    ") - segment files in %TEMP%.");
    }
    if (!started && !launch(false, error_out)) return false;

    if (state_.audio_out_channels > 0 && !state_.recording) {
        hr_audio_set_max_buffer_sec(wrap * kReplaySegmentSec);
        hr_audio_reset_buffers(nullptr, nullptr);
        hr_audio_set_volumes(mic_vol_, sys_vol_, mic_muted_ ? 1 : 0, sys_muted_ ? 1 : 0);
    }
    return true;
}

// Stops+destroys replay_ff_ (waiting briefly so its last, currently-open segment gets
// finalized rather than left mid-write) without touching pipeline_ itself - callers decide
// what the pipeline's recording pipe should point at next.
void RecordingController::StopInstantReplayEncoder() {
    if (!replay_ff_) return;
    hr_pl_end_recording_segment(pipeline_, 3000);
    hr_ff_stop_graceful(replay_ff_);
    if (hr_ff_wait(replay_ff_, 3000) != 0 && hr_ff_is_running(replay_ff_)) {
        HrLog::Warn("Instant Replay: segment writer didn't finish gracefully in time - killing it.");
        hr_ff_kill(replay_ff_);
    }
    // The process is gone, so its stdout is at end-of-file and the reader ends; it must be joined before the
    // ffmpeg object (which owns the pipe handle) is destroyed. The buffered replay is discarded with it.
    if (replay_ram_) { replay_ram_->join(); replay_ram_.reset(); }
    hr_ff_destroy(replay_ff_);
    replay_ff_ = nullptr;
}

void RecordingController::StopInstantReplayEncoderAsync() {
    if (!replay_ff_) return;
    void *h = replay_ff_;
    replay_ff_ = nullptr;
    std::shared_ptr<ReplayRam> ram = std::move(replay_ram_);   // the stop thread joins its reader
    replay_ram_.reset();
    void *pl = pipeline_;
    // See JoinPendingInstantReplayStop()'s comment (recording_controller.h)
    JoinPendingInstantReplayStop();
    hr_pl_end_recording_segment(pl, 1500);
    instant_replay_stop_thread_ = std::thread([h, ram]() {
        // (std::terminate() crash a moment after "Instant Replay:
        // segment writer didn't finish gracefully in time - killing it"):
        try {
            hr_ff_stop_graceful(h);
            if (hr_ff_wait(h, 3000) != 0 && hr_ff_is_running(h)) {
                HrLog::Warn("Instant Replay: segment writer didn't finish gracefully in time - killing it.");
                hr_ff_kill(h);
            }
            if (ram) ram->join();      // before the ffmpeg object (= the pipe handle) goes away
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
    if (instant_replay_active_) {
        // Changing the buffer length while Instant Replay was already
        // running was ignored (the ring size is fixed when the encoder starts).
        const int want_sec = state_.replay_buffer_sec > 0 ? state_.replay_buffer_sec : 30;
        if (want_sec == replay_applied_buffer_sec_ || state_.recording) return true; // already running
        HrLog::Info("Instant Replay: buffer length changed - restarting the buffer.");
        DisableInstantReplay();
        instant_replay_enabled_ = true;
    }
    if (state_.recording) return true;       // Stop() will start it once the manual recording ends

    if (!ffmpeg_found_) {
        error_out = L"FFmpeg not found.";
        return false;
    }
    if (!CheckCaptureTarget(error_out)) return false;

    int prev_w = capture_w_, prev_h = capture_h_;
    ResolveCaptureSize();

    if (!pipeline_ || capture_w_ != prev_w || capture_h_ != prev_h ||
        pipeline_output_idx_ != capture_output_idx_ || !PipelineMatchesTarget()) {
        JoinPendingInstantReplayStop();
        if (pipeline_) { hr_pl_destroy(pipeline_); pipeline_ = nullptr; }
        JoinPendingPreviewTeardown();
        int pvw = 0, pvh = 0;
        ScaledPreviewSize(pvw, pvh);
        pipeline_ = CreatePipelineForTarget(/*pipe_fd=*/0, pvw, pvh);
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
    // The branch above only configures a pipeline it (re)creates.
    hr_pl_set_capture_rect(pipeline_, crop_x_, crop_y_, crop_w_, crop_h_);
    hr_pl_set_output_size(pipeline_, output_w_, output_h_);
    hr_pl_set_preview_fps(pipeline_, state_.preview_fps);
    hr_pl_set_preview_needed(pipeline_, PreviewNeededFlag());

    if (!StartInstantReplayEncoder(error_out)) return false;
    instant_replay_active_ = true;
    HrLog::Info("Instant Replay: buffering started (" + std::to_string(state_.replay_buffer_sec) + "s buffer).");
    return true;
}

void RecordingController::DisableInstantReplay() {
    instant_replay_enabled_ = false;
    if (!instant_replay_active_) return;
    StopInstantReplayEncoder();
    // See DeleteReplayDir()'s comment - the feature is fully off now, so
    // this run's segment folder is dead weight, not a buffer anyone will
    // read from again.
    DeleteReplayDir(replay_dir_);
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
    if (!state_.recording) {
        hr_audio_capture_to_wav(nullptr, nullptr); // stop buffering, discard the window
        hr_audio_set_max_buffer_sec(0);
    }
    HrLog::Info("Instant Replay: stopped.");
}

// Cuts the replay audio (mic + system, mixed to one WAV) into `dir`. Shared by both Save Replay modes; the body is the
// code SaveReplay() always had, only the window is new (see hr_audio_snapshot_to_wav_ex()).
std::string RecordingController::BuildReplayAudioWav(const std::wstring &dir, int window_ms) {
    std::string mic_wav_a = NarrowFromWide(dir + L"replay_mic_tmp.wav");
    std::string sys_wav_a = NarrowFromWide(dir + L"replay_sys.wav");
    std::string audio_wav_a = NarrowFromWide(dir + L"replay_audio.wav");
    bool want_mic = !mic_muted_;
    bool want_sys = !sys_muted_;
    bool have_replay_audio = false;
    if (state_.audio_out_channels > 0 && (want_mic || want_sys)) {
        int ar = hr_audio_snapshot_to_wav_ex(want_mic ? mic_wav_a.c_str() : nullptr,
                                              want_sys ? sys_wav_a.c_str() : nullptr, window_ms);
        bool mic_written = want_mic && (ar & 0x1) && hr_path_exists(mic_wav_a.c_str());
        bool sys_written = want_sys && (ar & 0x2) && hr_path_exists(sys_wav_a.c_str());
        if (mic_written && sys_written) {
            if (hr_audio_mix_wav(mic_wav_a.c_str(), sys_wav_a.c_str(), audio_wav_a.c_str()) == 0) {
                RemoveFileUtf8(mic_wav_a);
                RemoveFileUtf8(sys_wav_a);
            } else {
                RemoveFileUtf8(sys_wav_a);
                RenameFileUtf8(mic_wav_a, audio_wav_a);
            }
            have_replay_audio = true;
        } else if (mic_written) {
            RenameFileUtf8(mic_wav_a, audio_wav_a);
            have_replay_audio = true;
        } else if (sys_written) {
            RenameFileUtf8(sys_wav_a, audio_wav_a);
            have_replay_audio = true;
        }
    }
    return have_replay_audio ? audio_wav_a : std::string();
}

bool RecordingController::SaveReplay(std::wstring &error_out, std::wstring *out_saved_path) {
    if (!instant_replay_active_ || !replay_ff_) {
        error_out = L"Instant Replay isn't running.";
        return false;
    }
    std::wstring dir_to_read;
    std::wstring restart_err;
    bool restarted = true;
    std::vector<std::wstring> segs;
    int audio_window_ms = 0;

    if (replay_ram_) {
        // ---- RAM buffer. The encoder keeps running (no stop / restart, so the buffer is not emptied and the
        // next Save works at once); what is held in memory is written ONCE, as one MPEG-TS file, and ffmpeg
        // stream-copies it into the output container below.
        double sec = 0.0;
        const std::vector<ReplayRing::Chunk> parts = replay_ram_->ring().snapshot(&sec, /*final_flush=*/false);
        const ReplayRing::Stats st = replay_ram_->ring().stats();
        size_t total = 0;
        for (const auto &c : parts) total += c->size();
        if (parts.empty() || sec < 0.5 || total <= 2 * ReplayRing::kPkt) {
            error_out = L"Instant Replay buffer is empty (nothing captured yet).";
            return false;
        }
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring base_dir = std::wstring(tmp) + L"HomRec_replay\\";
        CreateDirectoryW(base_dir.c_str(), nullptr);
        dir_to_read = base_dir + L"save" + std::to_wstring(++replay_save_seq_) + L"\\";
        CreateDirectoryW(dir_to_read.c_str(), nullptr);
        const std::wstring ts_path = dir_to_read + L"replay_ram.ts";
        bool wrote = false;
        if (FILE *f = _wfopen(ts_path.c_str(), L"wb")) {
            wrote = true;
            for (const auto &c : parts)
                if (std::fwrite(c->data(), 1, c->size(), f) != c->size()) { wrote = false; break; }
            if (std::fclose(f) != 0) wrote = false;
        }
        if (!wrote) {
            error_out = L"Couldn't write the replay buffer to the temp folder (disk full?).";
            HrLog::Error("Instant Replay: writing the RAM buffer to " + NarrowFromWide(ts_path) + " failed");
            DeleteReplayDir(dir_to_read);
            return false;
        }
        segs.push_back(ts_path);
        audio_window_ms = (int)(sec * 1000.0);
        HrLog::Info("Instant Replay: saving " + std::to_string((int)(total >> 20)) + " MB / " +
                    std::to_string((int)(sec + 0.5)) + " s from the RAM buffer" +
                    (st.capped ? " (the memory cap shortened it below the requested length - raise replay_ram_mb)" : "") + ".");
    } else {
        // ---- Segment files in %TEMP% (the original behaviour).
        dir_to_read = replay_dir_;

        // Stop the current segment writer so its last (possibly still-open)
        StopInstantReplayEncoder();
        restarted = StartInstantReplayEncoder(restart_err);

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
            // See DeleteReplayDir()'s comment - dir_to_read's run is over
            // either way (StartInstantReplayEncoder() above already moved
            // replay_dir_ on to a new folder), empty or not.
            DeleteReplayDir(dir_to_read);
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
    }

    const std::string audio_wav_a = BuildReplayAudioWav(dir_to_read, audio_window_ms);
    const bool have_replay_audio = !audio_wav_a.empty();

    std::wstring list_path = dir_to_read + L"concat_list.txt";
    {
        // This was a std::wofstream in the "C" locale, which can't encode non-ASCII characters - a
        // temp path under a non-ASCII user name cut the list off and Save Replay failed.
        FILE *out = _wfopen(list_path.c_str(), L"wb");
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
            if (out) {
                const std::string line = "file '" + NarrowFromWide(esc) + "'\n";
                fwrite(line.data(), 1, line.size(), out);
            }
        }
        if (out) fclose(out);
    }

    current_output_path_ = BuildOutputPath();
    bool ok = hr_concat_segments(ffmpeg_path_.c_str(), list_path.c_str(),
                                  current_output_path_.c_str()) != 0;

    bool keep_dir_for_audio_fallback = false;
    if (ok && have_replay_audio) {
        if (hr_merge_av(ffmpeg_path_.c_str(), current_output_path_.c_str(),
                         WideFromNarrow(audio_wav_a).c_str(), 0.0, 0.0) != 0) {
            RemoveFileUtf8(audio_wav_a);
        } else {
            HrLog::Error("Instant Replay: merging the captured audio into the saved replay "
                         "failed -- keeping '" + audio_wav_a + "' next to the (silent) clip "
                         "instead of losing the audio entirely.");
            keep_dir_for_audio_fallback = true;
        }
    }

    if (ok && state_.video_format == VideoFormat::Both) {
        const size_t rd = current_output_path_.find_last_of(L'.');
        if (rd != std::wstring::npos) {
            const std::wstring mp4_copy = current_output_path_.substr(0, rd) + L".mp4";
            if (!hr_remux_copy(ffmpeg_path_.c_str(), current_output_path_.c_str(), mp4_copy.c_str()))
                HrLog::Warn("Instant Replay: couldn't create the .mp4 copy - the .mkv is intact.");
        }
    }

    instant_replay_active_ = restarted;
    if (!restarted) {
        HrLog::Error(NarrowFromWide(L"Instant Replay: failed to resume buffering after Save Replay - " + restart_err));
    }

    // See DeleteReplayDir()'s comment - dir_to_read's run is over either way
    // (StartInstantReplayEncoder() above already moved replay_dir_ on to a new folder).
    if (!keep_dir_for_audio_fallback) DeleteReplayDir(dir_to_read);

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
    int nw = 0, nh = 0;
    uint64_t seq = 0; // 0 = never seen one -> always returns the current frame
    if (GetPreviewFrameIfNew(out, out_w, out_h, nw, nh, seq) != 1) return false;
    // Legacy contract: `out` is exactly out_w*out_h*3 bytes on success.
    out.resize((size_t)out_w * (size_t)out_h * 3);
    return true;
}

int RecordingController::GetPreviewFrameIfNew(std::vector<uint8_t> &out, int &out_w, int &out_h,
                                              int &native_w, int &native_h, uint64_t &seq) {
    if (!pipeline_) return 0;
    // Room for the largest thumbnail this pipeline can produce.
    const size_t cap = (size_t)std::max(state_.preview_width, 2) *
                       (size_t)std::max(state_.preview_height, 2) * 3;
    if (out.size() < cap) out.resize(cap);
    return hr_pl_get_preview_ex(pipeline_, out.data(), out.size(), &out_w, &out_h,
                                &native_w, &native_h, &seq);
}

void RecordingController::SetPreviewViewSize(int w, int h) {
    if (w < 64 || h < 64) return;
    pv_view_w_ = w;
    pv_view_h_ = h;
    if (!pipeline_) return;
    int mw = 0, mh = 0;
    ScaledPreviewSize(mw, mh);
    w = std::min(w, mw);
    h = std::min(h, mh);
    hr_pl_set_preview_size(pipeline_, w & ~1, h & ~1);
}

uint64_t RecordingController::PreviewSeq() const {
    return pipeline_ ? hr_pl_get_preview_seq(pipeline_) : 0;
}

bool RecordingController::GetPreviewNativeSize(int &w, int &h) {
    w = h = 0;
    if (pipeline_ && hr_pl_get_native_size(pipeline_, &w, &h) && w > 0 && h > 0) return true;
    w = capture_w_;
    h = capture_h_;
    return w > 0 && h > 0;
}

void RecordingController::EnsurePreview() {
    if (pipeline_ || state_.disable_preview) return; // already running, or user turned it off
    // See JoinPendingPreviewTeardown()'s comment (recording_controller.h) -
    // without this, creating a new pipeline right after TeardownPreview()
    JoinPendingPreviewTeardown();
    ResolveCaptureSize();
    // pipe_fd=0 -> hr_pl_create() leaves this in preview-only mode (frames
    // captured + thumbnailed for the UI, nothing written anywhere) - see
    // its "false -> preview only" comment in pipeline.cpp. Start()
    int pvw = 0, pvh = 0;
    ScaledPreviewSize(pvw, pvh);
    pipeline_ = CreatePipelineForTarget(/*pipe_fd=*/0, pvw, pvh);
    pipeline_output_idx_ = capture_output_idx_;
    last_overlays_sent_valid_ = false;
    if (pipeline_ && !hr_pl_start(pipeline_)) {
        hr_pl_destroy(pipeline_);
        pipeline_ = nullptr;
    }
    if (!pipeline_) {
        // This used to fail completely silently - no error, no
        // status change, nothing - so a stuck DXGI capture (dx_create()
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
    // Reached only on success - clear any backoff state from earlier failures so the next time
    // preview needs to (re)start (settings change, monitor reconnected, etc.) it retries at
    // the fast cadence again instead of staying artificially slow.
    preview_retry_streak_ = 0;
    preview_unavailable_ = false;
    // Same crop rect ResolveCaptureSize() above just resolved for Start()
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

    if (preview_teardown_thread_.joinable()) preview_teardown_thread_.join();
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
    // `idx` indexes the monitor LIST, but hr_pl_create() wants the
    // DXGI output index; the two only agree by luck. Match by desktop coordinates
    // like ResolveCaptureSize() does, otherwise the test records another monitor.
    {
        const int dx_count = hr_dx_output_count(0);
        for (int i = 0; i < dx_count; ++i) {
            int ox = 0, oy = 0, ow = 0, oh = 0;
            if (!hr_dx_output_desc(0, i, &ox, &oy, &ow, &oh, nullptr, 0)) continue;
            if (ox == mx && oy == my && ow == mw && oh == mh) { idx = i; break; }
        }
    }
    if (mw <= 0 || mh <= 0) { mw = 1920; mh = 1080; }
    if (mw % 2) mw--;
    if (mh % 2) mh--;
    qt_w_ = mw;
    qt_h_ = mh;

    // DXGI Desktop Duplication only allows one active duplication handle per monitor at a time
    // - if the live preview is currently capturing this same monitor, creating a second
    // pipeline for the test would just fail.
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
    bool codec_is_hw = (codec != L"libx264" && codec != L"libx265");

    std::wstring codec_args;
    if (!state_.custom_ffmpeg_args.empty()) {
        codec_args = L"-c:v " + codec + L" " + WideFromNarrow(state_.custom_ffmpeg_args);
    } else {
        wchar_t buf[512] = {};
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        std::wstring wpreset = WideFromNarrow(preset);
        hr_build_codec_args_ex(codec.c_str(), state_.quality, state_.target_fps,
                                (int)si.dwNumberOfProcessors, mw, mh,
                                buf, 512, wpreset.c_str());
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
    hr_pl_set_output_pixfmt(qt_pipeline_, codec_is_hw ? 1 : 0);
    hr_pl_set_timestamped_pipe(qt_pipeline_, 0);   // benchmark: must measure real, unskipped frames
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

    // qt_overloaded_ (PollQuickTest) only fires on a *sustained* rise -- three consecutive
    // 200ms polls where the drop count went up.
    const bool ratio_bad = result.frames > 0 &&
        (double)result.drops / (double)result.frames > 0.15;
    result.overloaded = qt_overloaded_ || ratio_bad;

    if (qt_pipeline_) {
        hr_pl_stop(qt_pipeline_);
        hr_pl_destroy(qt_pipeline_);
        qt_pipeline_ = nullptr;
    }
    if (qt_ffproc_) {
        // Same generous wait Stop()'s finalize tail gives a real recording (see
        // StopFinalizeTail()'s comment) would be overkill for a few seconds of test footage that's
        // about to be deleted anyway - a shorter budget before force-stopping is fine here.
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
    now.capture_window_hwnd = state_.capture_window_hwnd;
    now.window_capture_method = state_.window_capture_method;
    now.window_capture_area = state_.window_capture_area;
    now.window_title_mode = state_.window_title_mode;
    now.window_match_exe = state_.window_match_exe;
    now.window_match_class = state_.window_match_class;
    now.window_match_title = state_.window_match_title;
    now.window_follow_hwnd = state_.window_follow_hwnd;
    now.window_capture_smart = state_.window_capture_smart;
    now.window_capture_fps_cap = state_.window_capture_fps_cap;
    now.region_x = state_.region_x; now.region_y = state_.region_y;
    now.region_w = state_.region_w; now.region_h = state_.region_h;
    now.target_fps = state_.target_fps;
    now.preview_width = state_.preview_width;
    now.preview_height = state_.preview_height;
    now.preview_quality_pct = state_.preview_quality_pct;

    if (!applied_preview_capture_settings_valid_ || !(now == applied_preview_capture_settings_)) {
        const bool replay_was_on = instant_replay_active_ && !state_.recording;
        if (replay_was_on) DisableInstantReplay();
        TeardownPreview();
        ResetPreviewRetryState();
        bool replay_back = false;
        if (replay_was_on) {
            std::wstring replay_err;
            replay_back = EnableInstantReplay(replay_err);
            if (!replay_back) {
                HrLog::Error(NarrowFromWide(L"Instant Replay: couldn't restart after the capture settings changed - " + replay_err));
            }
        }
        if (!replay_back) EnsurePreview();
        // TeardownPreview()/EnsurePreview() are both no-ops while a recording owns pipeline_, so
        // nothing was actually applied in that case.
        if (!state_.recording) {
            applied_preview_capture_settings_ = now;
            applied_preview_capture_settings_valid_ = true;
        }
    }
    if (pipeline_) {
        // Live-apply Target FPS to an idle pipeline (never mid-recording:
        // ffmpeg was already started with the old rate).
        if (!state_.recording) hr_pl_set_fps(pipeline_, state_.target_fps);
        hr_pl_set_preview_fps(pipeline_, state_.preview_fps);
        // Covers the case TeardownPreview()/EnsurePreview() above don't:
        hr_pl_set_preview_needed(pipeline_, PreviewNeededFlag());
    }

    // Picking a different microphone in Settings had no effect until the app was restarted --
    // hr_audio_start() only ever ran once, at Init(), with whatever mic_device_id was set at
    // the time.
    if (!state_.recording && state_.mic_device_id != applied_mic_device_id_) {
        hr_audio_stop(nullptr, nullptr);
        hr_audio_start(1.0f, 1.0f, mic_muted_ ? 1 : 0, sys_muted_ ? 1 : 0,
                        WideFromNarrow(state_.mic_device_id).c_str());
        applied_mic_device_id_ = state_.mic_device_id;
    }
}

void RecordingController::SetPreviewVisible(bool visible) {
    preview_window_visible_ = visible;
    if (state_.recording) {
        // The recording owns the pipeline until Stop(), so it can't be torn
        // down here - but its live-preview thumbnails (a downscale + copy of
        // every Nth frame, plus a repaint request to a window nobody can see)
        if (pipeline_ && !snapshot_forced_preview_)
            hr_pl_set_preview_needed(pipeline_, PreviewNeededFlag());
        return;
    }
    if (visible) {
        // Used to call EnsurePreview() directly here and nothing else.
        ResetPreviewRetryState();
        EnsurePreview();
        // EnsurePreview() returns early when a pipeline is already running
        // (kept alive by Instant Replay, or left over from a recording that
        // ended while the window was hidden) - re-enable its thumbnails.
        if (pipeline_ && !state_.disable_preview) hr_pl_set_preview_needed(pipeline_, 1);
    } else {
        TeardownPreview();
    }
}

bool RecordingController::CaptureSnapshotFrame(std::vector<uint8_t> &out, int &out_w, int &out_h,
                                                int &native_w, int &native_h, bool first_call) {
    // This used to be gated on `first_call` too, so if the very first attempt of an editing
    // session failed to get a pipeline going (EnsurePreview() briefly stuck right after e.g.
    bool just_started = false;
    if (!pipeline_) {
        // EnsurePreview() no-ops when state_.disable_preview is set - this is an explicit "show me
        // a screenshot anyway" request, so start it regardless, same as SetPreviewVisible() would
        // if the setting were off.
        bool was_disabled = state_.disable_preview;
        state_.disable_preview = false;
        EnsurePreview();
        state_.disable_preview = was_disabled;
        just_started = true;
    }
    if (!pipeline_) return false;

    if (state_.disable_preview) {
        hr_pl_set_preview_needed(pipeline_, 1);
        snapshot_forced_preview_ = true;
    }

    const int max_wait_ms = (first_call || just_started || snapshot_forced_preview_) ? 1000 : 0;
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
    if (!state_.disable_preview) { snapshot_forced_preview_ = false; return; }
    // A recording or Instant Replay still owns the pipeline (TeardownPreview()
    if (pipeline_ && (state_.recording || instant_replay_active_)) {
        hr_pl_set_preview_needed(pipeline_, 0);
    } else {
        TeardownPreview();
    }
    snapshot_forced_preview_ = false;
}

double RecordingController::preview_pipeline_fps() const {
    if (!pipeline_) return 0.0;
    long long frames = 0, drops = 0;
    double fps = 0.0;
    hr_pl_stats(pipeline_, &frames, &drops, &fps);
    return fps;
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
    if (mfenc_) return hr_mf_output_size_mb(mfenc_);
    return (ffproc_ ? hr_ff_output_size_mb(ffproc_) : 0.0) + (ddagrab_active_ ? ddagrab_done_mb_ : 0.0);
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
