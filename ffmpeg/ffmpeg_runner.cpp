// ffmpeg_runner.cpp  -  HomRec FFmpeg process manager  (v1.6.1)

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #define HR_EXPORT extern "C" __declspec(dllexport)
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
  #include <sys/stat.h>
  #include <signal.h>
  #include <unistd.h>
  #include <sys/wait.h>
  typedef int HANDLE;
  #define INVALID_HANDLE_VALUE (-1)
#endif

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <sstream>
#include <mutex>
#include <thread>
#include <chrono>
#ifdef _WIN32
  #include "../src/utils/log.h"
#endif

/* -- Return codes ---------------------------------------------------------- */
static constexpr int HR_FF_OK       =  0;
static constexpr int HR_FF_RUNNING  =  1;
static constexpr int HR_FF_STOPPED  =  2;
static constexpr int HR_FF_ERROR    = -1;

/* -- Process context ------------------------------------------------------- */
struct FfmpegCtx {
    std::string ffmpeg_path;
    std::string output_path;
    std::string audio_path;
    std::string input_path;  /* only used when pipe_input == false, see below */
    std::string codec_args;  /* space-separated ffmpeg codec flags */
    std::string extra_output_args;

    /* process */
#ifdef _WIN32
    HANDLE hProcess{nullptr};
    HANDLE hThread{nullptr};
    HANDLE hStdin{nullptr};   /* write end of stdin pipe for 'q\n' signal */
#else
    pid_t  pid{-1};
#endif

    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::mutex        mu;

    /* video params */
    int width{1920}, height{1080}, fps{30};
    // Output (post-scale) size, if different from width/height above - 0 means "same as input,
    // no scale filter needed".
    int output_width{0}, output_height{0};
    bool pipe_input{false};  /* true → read raw BGRA from stdin */

    bool gpu_capture{false};
    int  grab_output_idx{0};
    int  grab_off_x{0}, grab_off_y{0};
    int  grab_w{0}, grab_h{0};   /* 0x0 = the whole output */
    bool grab_cursor{true};
    bool normal_priority{false};
    /* Opt-in: put the ffmpeg child one GPU scheduling class below normal (see
     * hr_gpu_sched_set_process() in dxgi_capture.cpp) so a game on a shared iGPU
     * gets the GPU first. Applied right after CreateProcess. */
    bool gpu_low_priority{false};
    bool gpu_priority_applied{false};   /* result of the call above, for hr_ff_gpu_priority_applied() */

    // Diagnostics for the ddagrab path (stderr of a GUI-launched ffmpeg used to go
    // nowhere, so a mid-recording death left no trace of the reason):
    // log_path      - ffmpeg's stderr is redirected here (logs\ffmpeg_ddagrab.log)
    std::string log_path;
    std::string progress_path;
};

/* -- Helpers ---------------------------------------------------------------- */

static int64_t _file_size(const char *path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) return 0;
    return ((int64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
#else
    struct stat st{};
    return (stat(path, &st) == 0) ? (int64_t)st.st_size : 0;
#endif
}

/* -- Process launch helpers ------------------------------------------------- */

static bool _wants_hw_pixfmt(const std::string &codec_args);
static std::wstring _utf8_to_wide(const std::string &s);
extern "C" int hr_gpu_sched_set_process(void *hProcess, int level);
extern "C" long hr_gpu_sched_last_status();

#ifdef _WIN32
static bool _create_overlapped_stdin_pipe(HANDLE *out_read, HANDLE *out_write) {
    static std::atomic<uint32_t> s_counter{0};
    wchar_t name[128];
    swprintf(name, 128, L"\\\\.\\pipe\\HomRecStdin_%lu_%u",
             GetCurrentProcessId(), s_counter.fetch_add(1, std::memory_order_relaxed));

    HANDLE hWrite = CreateNamedPipeW(
        name,
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1,             // one instance - this is a private pipe, not a server
        4 << 20,       // out buffer (4MB - holds a whole 1080p NV12 frame (3.1MB)
        0,             // in buffer - unused, outbound only
        0,             // default timeout
        nullptr);
    if (hWrite == INVALID_HANDLE_VALUE) return false;

    SECURITY_ATTRIBUTES sa_inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE hRead = CreateFileW(
        name, GENERIC_READ, 0, &sa_inherit, OPEN_EXISTING,
        0,             // ordinary synchronous handle - ffmpeg reads it like any pipe
        nullptr);
    if (hRead == INVALID_HANDLE_VALUE) { CloseHandle(hWrite); return false; }

    *out_read  = hRead;
    *out_write = hWrite;
    return true;
}

static bool _launch_win(FfmpegCtx *ctx, const std::wstring &cmdline) {
    HANDLE hReadStdin{}, hWriteStdin{};
    // stdin is redirected in pipe mode (raw frames) and in ddagrab mode (only
    // the "q\n" stop command). Only pipe mode needs the overlapped variant.
    const bool want_stdin = ctx->pipe_input || ctx->gpu_capture;

    if (ctx->pipe_input) {
        // hRead comes back already inheritable (sa_inherit above); hWrite
        // (CreateNamedPipeW's server handle) is not inheritable by default,
        // same as the SetHandleInformation(..., 0) call this replaces.
        if (!_create_overlapped_stdin_pipe(&hReadStdin, &hWriteStdin)) return false;
    } else if (ctx->gpu_capture) {
        // Plain synchronous anonymous pipe: hr_ff_stop_graceful() writes the
        // two bytes "q\n" with an ordinary WriteFile(), which is not allowed on
        // an overlapped handle.
        SECURITY_ATTRIBUTES sa_inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        if (!CreatePipe(&hReadStdin, &hWriteStdin, &sa_inherit, 0)) return false;
        SetHandleInformation(hWriteStdin, HANDLE_FLAG_INHERIT, 0);   // child must only inherit the read end
    }

    // ddagrab mode: send ffmpeg's stderr to a log file so that a failure mid-recording
    // (display mode change by a game, encoder error, ...) leaves its reason behind.
    HANDLE hErrLog = INVALID_HANDLE_VALUE;
    if (ctx->gpu_capture && !ctx->log_path.empty()) {
        SECURITY_ATTRIBUTES sa_log{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        hErrLog = CreateFileW(_utf8_to_wide(ctx->log_path).c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa_log, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    if (want_stdin) {
        si.hStdInput = hReadStdin;
        si.dwFlags  |= STARTF_USESTDHANDLES;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = (hErrLog != INVALID_HANDLE_VALUE) ? hErrLog : GetStdHandle(STD_ERROR_HANDLE);
    }
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::wstring mut_cmd = cmdline;
    DWORD priority_class = CREATE_NO_WINDOW |
        ((_wants_hw_pixfmt(ctx->codec_args) && !ctx->normal_priority) ? ABOVE_NORMAL_PRIORITY_CLASS
                                                                       : NORMAL_PRIORITY_CLASS);
    bool ok = (CreateProcessW(nullptr, mut_cmd.data(),
                               nullptr, nullptr, want_stdin ? TRUE : FALSE,
                               priority_class,
                               nullptr, nullptr, &si, &pi) != 0);

    if (want_stdin && hReadStdin) CloseHandle(hReadStdin);
    if (hErrLog != INVALID_HANDLE_VALUE) CloseHandle(hErrLog);   // the child keeps its own copy

    if (!ok) {
        if (want_stdin && hWriteStdin) CloseHandle(hWriteStdin);
        return false;
    }

    ctx->hProcess = pi.hProcess;
    ctx->hThread  = pi.hThread;
    if (want_stdin) ctx->hStdin = hWriteStdin;
    ctx->running = true;
    if (ctx->gpu_low_priority) {
        const int ok_prio = hr_gpu_sched_set_process(pi.hProcess, 1);
        OutputDebugStringA(ok_prio ? "HomRec: ffmpeg GPU scheduling class lowered (below normal)\n"
                                   : "HomRec: ffmpeg GPU scheduling class could not be lowered\n");
        ctx->gpu_priority_applied = ok_prio != 0;
        if (!ok_prio) {
            const long first_status = hr_gpu_sched_last_status();
            HANDLE hDup = nullptr;
            if (DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &hDup, 0, FALSE,
                                DUPLICATE_SAME_ACCESS) && hDup) {
                try {
                    std::thread([hDup, first_status]() {
                        const auto t0 = std::chrono::steady_clock::now();
                        long last_status = first_status;
                        bool applied = false, exited = false;
                        long long waited_ms = 0;
                        for (;;) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(150));
                            waited_ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - t0).count();
                            if (WaitForSingleObject(hDup, 0) == WAIT_OBJECT_0) { exited = true; break; }
                            if (hr_gpu_sched_set_process(static_cast<void *>(hDup), 1)) { applied = true; break; }
                            last_status = hr_gpu_sched_last_status();
                            if (waited_ms >= 8000) break;
                        }
                        CloseHandle(hDup);
                        char st_hex[16];
                        std::snprintf(st_hex, sizeof(st_hex), "0x%08lX", (unsigned long)last_status);
                        if (applied)
                            HrLog::Info("Recording: ffmpeg GPU priority lowered (below normal) " + std::to_string(waited_ms) +
                                        " ms after launch (the first try was refused with NTSTATUS " +
                                        std::string(st_hex) + ": ffmpeg had not opened the GPU yet) - the game gets the GPU first.");
                        else if (!exited)
                            HrLog::Warn("Recording: ffmpeg GPU priority could NOT be lowered even after " +
                                        std::to_string(waited_ms) + " ms of retries (NTSTATUS " + std::string(st_hex) +
                                        ") - recording as usual.");
                    }).detach();
                } catch (...) {
                    CloseHandle(hDup);   /* no thread available: keep the one-shot result, nothing else to undo */
                }
            }
        }
    }
    return true;
}
#endif

/* -- Build ffmpeg command line --------------------------------------------- */

// Builds the complete ffmpeg argument list for screen recording.
// When pipe_input is false, MSS captures to a file and ffmpeg re-encodes it.
#ifdef _WIN32
static std::wstring _utf8_to_wide(const std::string &s) {
    if (s.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring ws((size_t)needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), needed);
    return ws;
}
#else
static std::wstring _utf8_to_wide(const std::string &s) {
    return std::wstring(s.begin(), s.end());
}
#endif

static bool _wants_hw_pixfmt(const std::string &codec_args) {
    return codec_args.find("nvenc") != std::string::npos ||
           codec_args.find("qsv")   != std::string::npos ||
           codec_args.find("amf")   != std::string::npos;
}

// In that path the frames are D3D11/QSV BGRA surfaces and the HARDWARE ENCODER does the
// RGB -> YUV conversion itself (Intel MFX / NVENC / AMF).
static std::wstring _ddagrab_vui_bsf(const std::string &codec_args) {
    // 6 = smpte170m (BT.601), same tag the pipe path writes for its own data.
    if (codec_args.find("h264") != std::string::npos) {
        return L" -bsf:v h264_metadata=video_full_range_flag=0:colour_primaries=6"
               L":transfer_characteristics=6:matrix_coefficients=6";
    }
    if (codec_args.find("hevc") != std::string::npos || codec_args.find("h265") != std::string::npos) {
        return L" -bsf:v hevc_metadata=video_full_range_flag=0:colour_primaries=6"
               L":transfer_characteristics=6:matrix_coefficients=6";
    }
    if (codec_args.find("av1") != std::string::npos) {
        return L" -bsf:v av1_metadata=color_range=tv:color_primaries=6"
               L":transfer_characteristics=6:matrix_coefficients=6";
    }
    return L"";
}

static std::wstring _build_cmdline(const FfmpegCtx *ctx) {
    std::wostringstream ss;

    /* quote helper */
    auto Q = [](const std::string &s) -> std::wstring {
        return L"\"" + _utf8_to_wide(s) + L"\"";
    };

    const wchar_t *raw_pixfmt = _wants_hw_pixfmt(ctx->codec_args) ? L"nv12" : L"yuv420p";

    ss << Q(ctx->ffmpeg_path);

    if (ctx->gpu_capture) {
        // -nostats: the \r-terminated stats line would flood the stderr log; the live frame
        // count comes from -progress instead (key=value blocks, independent of -loglevel).
        ss << L" -hide_banner -nostats -loglevel info";
        if (!ctx->progress_path.empty()) ss << L" -progress " << Q(ctx->progress_path);
    }

    if (ctx->gpu_capture) {
        // ddagrab fast path. Documented ffmpeg recipes (doc/filters.texi):
        // NVENC / AMF : -filter_complex ddagrab=0            -c:v h264_nvenc ...
        const bool is_qsv = ctx->codec_args.find("qsv") != std::string::npos;
        std::wostringstream grab;
        grab << L"ddagrab=output_idx=" << ctx->grab_output_idx
             << L":framerate=" << ctx->fps
             << L":draw_mouse=" << (ctx->grab_cursor ? 1 : 0);
        if (ctx->grab_w > 0 && ctx->grab_h > 0) {
            grab << L":offset_x=" << ctx->grab_off_x << L":offset_y=" << ctx->grab_off_y
                 << L":video_size=" << ctx->grab_w << L"x" << ctx->grab_h;
        }
        if (is_qsv) ss << L" -init_hw_device d3d11va";
        ss << L" -filter_complex \"" << grab.str();
        if (is_qsv) ss << L",hwmap=derive_device=qsv,format=qsv";
        ss << L"\"";
        if (!ctx->codec_args.empty()) ss << L" " << _utf8_to_wide(ctx->codec_args);
        // Fix the colour tags of the encoded stream (limited range, BT.601) - see _ddagrab_vui_bsf().
        ss << _ddagrab_vui_bsf(ctx->codec_args);
        ss << L" -y " << Q(ctx->output_path);
        return ss.str();
    }

    if (ctx->pipe_input) {
        /* Pipe mode: read raw BGRA from stdin */
        ss << L" -f rawvideo"
           << L" -pixel_format " << raw_pixfmt
           << L" -video_size " << ctx->width << L"x" << ctx->height
           << L" -framerate " << ctx->fps
           << L" -i pipe:0";
    } else {
        ss << L" -framerate " << ctx->fps
           << L" -i " << Q(ctx->input_path);
    }

    /* Codec args (already formatted, e.g. "-c:v libx264 -preset ultrafast …") */
    if (!ctx->codec_args.empty()) {
        ss << L" " << _utf8_to_wide(ctx->codec_args);
    }

    /* Pixel format for H.264 compatibility - matches raw_pixfmt above so
       hardware encoders receive NV12 directly instead of ffmpeg silently
       re-converting from yuv420p. */
    ss << L" -pix_fmt " << raw_pixfmt;
    // BUGFIX: this used to always say "-color_range pc" for both paths.
    const wchar_t *out_color_range = _wants_hw_pixfmt(ctx->codec_args) ? L"tv" : L"pc";
    ss << L" -color_range " << out_color_range << L" -colorspace smpte170m"
          L" -color_primaries smpte170m -color_trc smpte170m";

    // Encode-time downscale (AppState's Settings > Resolution, e.g.
    if (ctx->output_width > 0 && ctx->output_height > 0 &&
        (ctx->output_width != ctx->width || ctx->output_height != ctx->height)) {
        ss << L" -vf scale=" << ctx->output_width << L":" << ctx->output_height;
    }

    // Extra output-side flags (Instant Replay's segment muxer, etc.) - must land after
    // codec/scale args and before -y/output_path, same position ffmpeg itself expects "-f
    // segment ..." in on a real command line.
    if (!ctx->extra_output_args.empty()) {
        ss << L" " << _utf8_to_wide(ctx->extra_output_args);
    }

    /* Output */
    ss << L" -y " << Q(ctx->output_path);

    return ss.str();
}

/* -- Public API ------------------------------------------------------------- */

HR_EXPORT void *hr_ff_create() {
    return new(std::nothrow) FfmpegCtx{};
}

HR_EXPORT void hr_ff_destroy(void *handle) {
    if (!handle) return;
    auto *ctx = static_cast<FfmpegCtx *>(handle);
    /* Force-kill if still running */
#ifdef _WIN32
    if (ctx->running && ctx->hProcess) {
        TerminateProcess(ctx->hProcess, 1);
        CloseHandle(ctx->hProcess);
        CloseHandle(ctx->hThread);
        // Same handle-ownership split as hr_ff_kill()/hr_ff_stop_graceful():
        if (ctx->hStdin) {
            if (!ctx->pipe_input) CloseHandle(ctx->hStdin);
            ctx->hStdin = nullptr;
        }
    }
#else
    if (ctx->running && ctx->pid > 0) kill(ctx->pid, SIGKILL);
#endif
    delete ctx;
}

HR_EXPORT void hr_ff_set_ffmpeg_path(void *h, const char *path) {
    if (!h || !path) return;
    static_cast<FfmpegCtx *>(h)->ffmpeg_path = path;
}

HR_EXPORT void hr_ff_set_output_path(void *h, const char *path) {
    if (!h || !path) return;
    static_cast<FfmpegCtx *>(h)->output_path = path;
}

// hr_ff_set_input_path
// Only meaningful when pipe_input is disabled (hr_ff_set_pipe_input(h, 0)):
HR_EXPORT void hr_ff_set_input_path(void *h, const char *path) {
    if (!h || !path) return;
    static_cast<FfmpegCtx *>(h)->input_path = path;
}

HR_EXPORT void hr_ff_set_codec_args(void *h, const char *args) {
    if (!h || !args) return;
    static_cast<FfmpegCtx *>(h)->codec_args = args;
}

HR_EXPORT void hr_ff_set_extra_output_args(void *h, const char *args) {
    if (!h) return;
    static_cast<FfmpegCtx *>(h)->extra_output_args = args ? args : "";
}

HR_EXPORT void hr_ff_set_video_params(void *h, int w, int h2, int fps) {
    if (!h) return;
    auto *ctx = static_cast<FfmpegCtx *>(h);
    ctx->width = w; ctx->height = h2; ctx->fps = fps;
}

// hr_ff_set_output_size Sets the final encoded resolution, if it should differ from the
// raw input size set via hr_ff_set_video_params() (i.e.
HR_EXPORT void hr_ff_set_output_size(void *h, int out_w, int out_h) {
    if (!h) return;
    auto *ctx = static_cast<FfmpegCtx *>(h);
    ctx->output_width = out_w; ctx->output_height = out_h;
}

// hr_ff_set_gpu_capture  (ffmpeg "ddagrab" fast path, ffmpeg 6.0+)
// enable   : 1 = ffmpeg captures + encodes on the GPU by itself (no pipe).
// output_idx : DXGI output index (same numbering as hr_dx_create()).
HR_EXPORT void hr_ff_set_normal_priority(void *h, int enable) {
    if (!h) return;
    static_cast<FfmpegCtx *>(h)->normal_priority = (enable != 0);
}

// hr_ff_set_gpu_low_priority:
HR_EXPORT void hr_ff_set_gpu_low_priority(void *h, int enable) {
    if (!h) return;
    static_cast<FfmpegCtx *>(h)->gpu_low_priority = (enable != 0);
}

HR_EXPORT int hr_ff_gpu_priority_applied(const void *h) {
    if (!h) return 0;
    return static_cast<const FfmpegCtx *>(h)->gpu_priority_applied ? 1 : 0;
}

HR_EXPORT void hr_ff_set_gpu_capture(void *h, int enable, int output_idx,
                                      int off_x, int off_y, int w, int h2, int draw_mouse) {
    if (!h) return;
    auto *ctx = static_cast<FfmpegCtx *>(h);
    ctx->gpu_capture     = (enable != 0);
    ctx->grab_output_idx = output_idx < 0 ? 0 : output_idx;
    ctx->grab_off_x = off_x < 0 ? 0 : off_x;
    ctx->grab_off_y = off_y < 0 ? 0 : off_y;
    ctx->grab_w = w > 0 ? w : 0;
    ctx->grab_h = h2 > 0 ? h2 : 0;
    ctx->grab_cursor = (draw_mouse != 0);
    if (ctx->gpu_capture) ctx->pipe_input = false;
}

// hr_ff_set_diag_paths
// ddagrab mode only. log_path: ffmpeg's stderr is written there. progress_path: ffmpeg's
// "-progress" output (read back by hr_ff_progress_frames()). Either may be NULL/empty.
HR_EXPORT void hr_ff_set_diag_paths(void *h, const char *log_path, const char *progress_path) {
    if (!h) return;
    auto *ctx = static_cast<FfmpegCtx *>(h);
    ctx->log_path      = log_path ? log_path : "";
    ctx->progress_path = progress_path ? progress_path : "";
}

#ifdef _WIN32
// Reads up to `cap` bytes from the END of a file another process may still be writing.
static std::string _read_file_tail(const std::string &utf8_path, size_t cap) {
    std::string out;
    if (utf8_path.empty() || cap == 0) return out;
    HANDLE f = CreateFileW(_utf8_to_wide(utf8_path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0) {
        long long want = sz.QuadPart < (long long)cap ? sz.QuadPart : (long long)cap;
        LARGE_INTEGER pos{};
        pos.QuadPart = sz.QuadPart - want;
        if (SetFilePointerEx(f, pos, nullptr, FILE_BEGIN)) {
            out.resize((size_t)want);
            DWORD got = 0;
            if (ReadFile(f, &out[0], (DWORD)want, &got, nullptr)) out.resize(got);
            else out.clear();
        }
    }
    CloseHandle(f);
    return out;
}
#endif

// hr_ff_progress_frames
// Number of frames ffmpeg has encoded so far, from the last "frame=N" line of its -progress
// file; -1 if unknown (no progress file yet / not ddagrab mode).
HR_EXPORT long long hr_ff_progress_frames(const void *handle) {
    if (!handle) return -1;
#ifdef _WIN32
    const auto *ctx = static_cast<const FfmpegCtx *>(handle);
    if (ctx->progress_path.empty()) return -1;
    const std::string tail = _read_file_tail(ctx->progress_path, 2048);
    size_t at = std::string::npos;
    for (size_t i = tail.rfind("frame="); i != std::string::npos;
         i = (i == 0) ? std::string::npos : tail.rfind("frame=", i - 1)) {
        if (i == 0 || tail[i - 1] == '\n') { at = i; break; }   // a line that STARTS with frame=
    }
    if (at == std::string::npos) return -1;
    char *end = nullptr;
    long long v = std::strtoll(tail.c_str() + at + 6, &end, 10);
    return (end && end != tail.c_str() + at + 6) ? v : -1;
#else
    return -1;
#endif
}

// hr_ff_read_log_tail
// Copies the last bytes of ffmpeg's stderr log (see hr_ff_set_diag_paths) into buf
// (NUL-terminated). Returns the number of bytes written (0 if there is no log).
HR_EXPORT int hr_ff_read_log_tail(const void *handle, char *buf, int buf_len) {
    if (!buf || buf_len <= 0) return 0;
    buf[0] = '\0';
    if (!handle) return 0;
#ifdef _WIN32
    const auto *ctx = static_cast<const FfmpegCtx *>(handle);
    const std::string tail = _read_file_tail(ctx->log_path, (size_t)buf_len - 1);
    std::memcpy(buf, tail.data(), tail.size());
    buf[tail.size()] = '\0';
    return (int)tail.size();
#else
    return 0;
#endif
}

// hr_ff_exit_code
// Exit code of the ffmpeg process once it has ended; -1 while it is still running / unknown.
HR_EXPORT int hr_ff_exit_code(const void *handle) {
    if (!handle) return -1;
#ifdef _WIN32
    const auto *ctx = static_cast<const FfmpegCtx *>(handle);
    // 'mu' guards the process handle's lifetime (see hr_ff_wait()/hr_ff_kill()).
    std::lock_guard<std::mutex> lk(const_cast<FfmpegCtx *>(ctx)->mu);
    if (!ctx->hProcess) return -1;
    DWORD code = STILL_ACTIVE;
    if (!GetExitCodeProcess(ctx->hProcess, &code) || code == STILL_ACTIVE) return -1;
    return (int)code;
#else
    return -1;
#endif
}

HR_EXPORT void hr_ff_set_pipe_input(void *h, int enable) {
    if (!h) return;
    static_cast<FfmpegCtx *>(h)->pipe_input = (enable != 0);
}

// hr_ff_get_stdin_handle Returns the write end of ffmpeg's stdin pipe (valid only once
// hr_ff_start() has succeeded and pipe_input was enabled), as an intptr_t-sized value so
// it round-trips through a 64-bit HANDLE on Win64 without truncation.
HR_EXPORT intptr_t hr_ff_get_stdin_handle(void *h) {
    if (!h) return 0;
#ifdef _WIN32
    return reinterpret_cast<intptr_t>(static_cast<FfmpegCtx *>(h)->hStdin);
#else
    return 0;
#endif
}

// hr_ff_start
// Launches the ffmpeg process.
// Returns HR_FF_OK on success, HR_FF_ERROR on failure.
HR_EXPORT int hr_ff_start(void *handle) {
    if (!handle) return HR_FF_ERROR;
    auto *ctx = static_cast<FfmpegCtx *>(handle);
    if (ctx->running) return HR_FF_RUNNING;
    if (ctx->ffmpeg_path.empty() || ctx->output_path.empty()) return HR_FF_ERROR;
    if (!ctx->pipe_input && !ctx->gpu_capture && ctx->input_path.empty()) return HR_FF_ERROR;

    std::wstring cmdline = _build_cmdline(ctx);

#ifdef _WIN32
    if (!_launch_win(ctx, cmdline)) return HR_FF_ERROR;
#else
    /* POSIX: not implemented in this stub - extend as needed */
    return HR_FF_ERROR;
#endif

    return HR_FF_OK;
}

// hr_ff_stop_graceful
// Sends 'q\n' to ffmpeg's stdin (graceful finish).
// Returns HR_FF_OK if the signal was sent, HR_FF_ERROR otherwise.
HR_EXPORT int hr_ff_stop_graceful(void *handle) {
    if (!handle) return HR_FF_ERROR;
    auto *ctx = static_cast<FfmpegCtx *>(handle);
    if (!ctx->running) return HR_FF_STOPPED;
    ctx->stopping = true;

#ifdef _WIN32
    if (ctx->hStdin) {
        if (ctx->pipe_input) {
            ctx->hStdin = nullptr;
        } else {
            // Non-pipe-input mode: stdin genuinely is ffmpeg's interactive control channel here (no
            // raw video sharing the same fd), so the "q\n" graceful-stop keystroke is legitimate and
            // this is the only owner of the handle.
            DWORD written = 0;
            WriteFile(ctx->hStdin, "q\n", 2, &written, nullptr);
            CloseHandle(ctx->hStdin);
            ctx->hStdin = nullptr;
        }
    }
#else
    if (ctx->pid > 0) kill(ctx->pid, SIGINT);
#endif
    return HR_FF_OK;
}

// hr_ff_wait
// Blocks until ffmpeg exits or timeout_ms elapses.
// Returns exit code (0 = success), or -1 on timeout / error.
HR_EXPORT int hr_ff_wait(void *handle, int timeout_ms) {
    if (!handle) return -1;
    auto *ctx = static_cast<FfmpegCtx *>(handle);
    if (!ctx->running) return 0;

#ifdef _WIN32
    HANDLE hp = nullptr;
    { std::lock_guard<std::mutex> lk(ctx->mu); hp = ctx->hProcess; }
    if (!hp) return 0;
    DWORD t = (timeout_ms <= 0) ? INFINITE : (DWORD)timeout_ms;
    DWORD res = WaitForSingleObject(hp, t);     // not under the lock: hr_ff_is_running() is polled from the UI
    if (res != WAIT_OBJECT_0) return -1;
    DWORD code = 0;
    GetExitCodeProcess(hp, &code);
    {
        // hr_ff_is_running()/hr_ff_exit_code() run on the UI thread while this runs on the finalize
        // thread: the handles used to be closed first and nulled afterwards, so a poll in between
        // called GetExitCodeProcess() on a closed (possibly already re-used) handle value.
        std::lock_guard<std::mutex> lk(ctx->mu);
        HANDLE ht = ctx->hThread;
        ctx->hProcess = nullptr;
        ctx->hThread  = nullptr;
        ctx->running  = false;
        ctx->stopping = false;
        CloseHandle(hp);
        if (ht) CloseHandle(ht);
    }
    return (int)code;
#else
    /* Simple busy-wait stub for non-Windows */
    int waited = 0;
    while (waited < timeout_ms || timeout_ms <= 0) {
        int status = 0;
        pid_t r = waitpid(ctx->pid, &status, WNOHANG);
        if (r == ctx->pid) {
            ctx->running  = false;
            ctx->stopping = false;
            ctx->pid = -1;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        usleep(50000); /* 50 ms */
        waited += 50;
    }
    return -1;
#endif
}

// hr_ff_is_running
// Returns 1 if process is alive, 0 otherwise.
HR_EXPORT int hr_ff_is_running(const void *handle) {
    if (!handle) return 0;
    const auto *ctx = static_cast<const FfmpegCtx *>(handle);
    if (!ctx->running) return 0;
#ifdef _WIN32
    std::lock_guard<std::mutex> lk(const_cast<FfmpegCtx *>(ctx)->mu);
    if (!ctx->running || !ctx->hProcess) return 0;
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(ctx->hProcess, &code);
    return (code == STILL_ACTIVE) ? 1 : 0;
#else
    return (waitpid(ctx->pid, nullptr, WNOHANG) == 0) ? 1 : 0;
#endif
}

// hr_ff_output_size_mb
// Returns the current output file size in megabytes.
HR_EXPORT double hr_ff_output_size_mb(const void *handle) {
    if (!handle) return 0.0;
    const auto *ctx = static_cast<const FfmpegCtx *>(handle);
    if (ctx->output_path.empty()) return 0.0;
    return (double)_file_size(ctx->output_path.c_str()) / (1024.0 * 1024.0);
}

// hr_ff_kill
// Immediately terminates the ffmpeg process.
HR_EXPORT void hr_ff_kill(void *handle) {
    if (!handle) return;
    auto *ctx = static_cast<FfmpegCtx *>(handle);
    if (!ctx->running) return;
#ifdef _WIN32
    HANDLE hp = nullptr;
    { std::lock_guard<std::mutex> lk(ctx->mu); hp = ctx->hProcess; }
    if (hp) {
        TerminateProcess(hp, 1);
        WaitForSingleObject(hp, 3000);          // not under the lock (see hr_ff_wait())
        std::lock_guard<std::mutex> lk(ctx->mu);
        HANDLE ht = ctx->hThread;
        ctx->hProcess = nullptr;
        ctx->hThread  = nullptr;
        CloseHandle(hp);
        if (ht) CloseHandle(ht);
        // BUGFIX: this used to CloseHandle(ctx->hStdin) unconditionally, same as
        // hr_ff_stop_graceful() did before *its* pipe_input fix (see the big comment over there).
        if (ctx->hStdin) {
            if (!ctx->pipe_input) CloseHandle(ctx->hStdin);
            ctx->hStdin = nullptr;
        }
    }
#else
    if (ctx->pid > 0) { kill(ctx->pid, SIGKILL); waitpid(ctx->pid, nullptr, 0); ctx->pid = -1; }
#endif
    ctx->running  = false;
    ctx->stopping = false;
}
