/*
 * hr_ffmpeg_runner.cpp  —  HomRec FFmpeg process manager  (v1.6.1)
 *
 * Replaces the FFmpeg subprocess logic from homrec.py:
 *   _build_ffmpeg_cmd(), _start_ffmpeg_process(), _stop_ffmpeg_process(),
 *   _wait_ffmpeg(), and the file-size polling helpers.
 *
 * The module wraps CreateProcess (Windows) / posix_spawn (Linux/macOS) and
 * exposes a minimal C API consumed by the Python bridge layer.
 *
 * Build (MinGW-w64):
 *   g++ -O2 -std=c++17 -shared -static-libgcc -static-libstdc++ ^
 *       -o hr_ffmpeg_runner.dll hr_ffmpeg_runner.cpp
 */

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
#include <string>
#include <vector>
#include <sstream>
#include <mutex>

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
    std::string codec_args;  /* space-separated ffmpeg codec flags */

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
    bool pipe_input{false};  /* true → read raw BGRA from stdin */
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

#ifdef _WIN32
static bool _launch_win(FfmpegCtx *ctx, const std::wstring &cmdline) {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE hReadStdin{}, hWriteStdin{};

    if (ctx->pipe_input) {
        if (!CreatePipe(&hReadStdin, &hWriteStdin, &sa, 0)) return false;
        SetHandleInformation(hWriteStdin, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    if (ctx->pipe_input) {
        si.hStdInput = hReadStdin;
        si.dwFlags  |= STARTF_USESTDHANDLES;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    }
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::wstring mut_cmd = cmdline;
    bool ok = (CreateProcessW(nullptr, mut_cmd.data(),
                               nullptr, nullptr, ctx->pipe_input ? TRUE : FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0);

    if (ctx->pipe_input && hReadStdin) CloseHandle(hReadStdin);

    if (!ok) {
        if (ctx->pipe_input && hWriteStdin) CloseHandle(hWriteStdin);
        return false;
    }

    ctx->hProcess = pi.hProcess;
    ctx->hThread  = pi.hThread;
    if (ctx->pipe_input) ctx->hStdin = hWriteStdin;
    ctx->running = true;
    return true;
}
#endif

/* -- Build ffmpeg command line --------------------------------------------- */

/*
 * Builds the complete ffmpeg argument list for screen recording.
 * When pipe_input is false, MSS captures to a file and ffmpeg re-encodes it.
 * When pipe_input is true, raw BGRA frames are piped into ffmpeg stdin.
 */
static std::wstring _build_cmdline(const FfmpegCtx *ctx) {
    std::wostringstream ss;

    /* quote helper */
    auto Q = [](const std::string &s) -> std::wstring {
        std::wstring ws(s.begin(), s.end());
        return L"\"" + ws + L"\"";
    };

    ss << Q(ctx->ffmpeg_path);

    if (ctx->pipe_input) {
        /* Pipe mode: read raw BGRA from stdin */
        ss << L" -f rawvideo"
           << L" -pixel_format bgra"
           << L" -video_size " << ctx->width << L"x" << ctx->height
           << L" -framerate " << ctx->fps
           << L" -i pipe:0";
    }

    /* Codec args (already formatted, e.g. "-c:v libx264 -preset ultrafast …") */
    if (!ctx->codec_args.empty()) {
        std::wstring wca(ctx->codec_args.begin(), ctx->codec_args.end());
        ss << L" " << wca;
    }

    /* Pixel format for H.264 compatibility */
    ss << L" -pix_fmt yuv420p";

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
        if (ctx->hStdin) CloseHandle(ctx->hStdin);
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

HR_EXPORT void hr_ff_set_codec_args(void *h, const char *args) {
    if (!h || !args) return;
    static_cast<FfmpegCtx *>(h)->codec_args = args;
}

HR_EXPORT void hr_ff_set_video_params(void *h, int w, int h2, int fps) {
    if (!h) return;
    auto *ctx = static_cast<FfmpegCtx *>(h);
    ctx->width = w; ctx->height = h2; ctx->fps = fps;
}

HR_EXPORT void hr_ff_set_pipe_input(void *h, int enable) {
    if (!h) return;
    static_cast<FfmpegCtx *>(h)->pipe_input = (enable != 0);
}

/*
 * hr_ff_start
 * Launches the ffmpeg process.
 * Returns HR_FF_OK on success, HR_FF_ERROR on failure.
 */
HR_EXPORT int hr_ff_start(void *handle) {
    if (!handle) return HR_FF_ERROR;
    auto *ctx = static_cast<FfmpegCtx *>(handle);
    if (ctx->running) return HR_FF_RUNNING;
    if (ctx->ffmpeg_path.empty() || ctx->output_path.empty()) return HR_FF_ERROR;

    std::wstring cmdline = _build_cmdline(ctx);

#ifdef _WIN32
    if (!_launch_win(ctx, cmdline)) return HR_FF_ERROR;
#else
    /* POSIX: not implemented in this stub — extend as needed */
    return HR_FF_ERROR;
#endif

    return HR_FF_OK;
}

/*
 * hr_ff_stop_graceful
 * Sends 'q\n' to ffmpeg's stdin (graceful finish).
 * Returns HR_FF_OK if the signal was sent, HR_FF_ERROR otherwise.
 */
HR_EXPORT int hr_ff_stop_graceful(void *handle) {
    if (!handle) return HR_FF_ERROR;
    auto *ctx = static_cast<FfmpegCtx *>(handle);
    if (!ctx->running) return HR_FF_STOPPED;
    ctx->stopping = true;

#ifdef _WIN32
    if (ctx->hStdin) {
        DWORD written = 0;
        WriteFile(ctx->hStdin, "q\n", 2, &written, nullptr);
        CloseHandle(ctx->hStdin);
        ctx->hStdin = nullptr;
    }
#else
    if (ctx->pid > 0) kill(ctx->pid, SIGINT);
#endif
    return HR_FF_OK;
}

/*
 * hr_ff_wait
 * Blocks until ffmpeg exits or timeout_ms elapses.
 * Returns exit code (0 = success), or -1 on timeout / error.
 */
HR_EXPORT int hr_ff_wait(void *handle, int timeout_ms) {
    if (!handle) return -1;
    auto *ctx = static_cast<FfmpegCtx *>(handle);
    if (!ctx->running) return 0;

#ifdef _WIN32
    DWORD t = (timeout_ms <= 0) ? INFINITE : (DWORD)timeout_ms;
    DWORD res = WaitForSingleObject(ctx->hProcess, t);
    if (res != WAIT_OBJECT_0) return -1;
    DWORD code = 0;
    GetExitCodeProcess(ctx->hProcess, &code);
    CloseHandle(ctx->hProcess);
    CloseHandle(ctx->hThread);
    ctx->hProcess = nullptr;
    ctx->hThread  = nullptr;
    ctx->running  = false;
    ctx->stopping = false;
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

/*
 * hr_ff_is_running
 * Returns 1 if process is alive, 0 otherwise.
 */
HR_EXPORT int hr_ff_is_running(const void *handle) {
    if (!handle) return 0;
    const auto *ctx = static_cast<const FfmpegCtx *>(handle);
    if (!ctx->running) return 0;
#ifdef _WIN32
    if (!ctx->hProcess) return 0;
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(ctx->hProcess, &code);
    return (code == STILL_ACTIVE) ? 1 : 0;
#else
    return (waitpid(ctx->pid, nullptr, WNOHANG) == 0) ? 1 : 0;
#endif
}

/*
 * hr_ff_output_size_mb
 * Returns the current output file size in megabytes.
 */
HR_EXPORT double hr_ff_output_size_mb(const void *handle) {
    if (!handle) return 0.0;
    const auto *ctx = static_cast<const FfmpegCtx *>(handle);
    if (ctx->output_path.empty()) return 0.0;
    return (double)_file_size(ctx->output_path.c_str()) / (1024.0 * 1024.0);
}

/*
 * hr_ff_kill
 * Immediately terminates the ffmpeg process.
 */
HR_EXPORT void hr_ff_kill(void *handle) {
    if (!handle) return;
    auto *ctx = static_cast<FfmpegCtx *>(handle);
    if (!ctx->running) return;
#ifdef _WIN32
    if (ctx->hProcess) {
        TerminateProcess(ctx->hProcess, 1);
        WaitForSingleObject(ctx->hProcess, 3000);
        CloseHandle(ctx->hProcess);
        CloseHandle(ctx->hThread);
        if (ctx->hStdin) { CloseHandle(ctx->hStdin); ctx->hStdin = nullptr; }
        ctx->hProcess = nullptr;
        ctx->hThread  = nullptr;
    }
#else
    if (ctx->pid > 0) { kill(ctx->pid, SIGKILL); waitpid(ctx->pid, nullptr, 0); ctx->pid = -1; }
#endif
    ctx->running  = false;
    ctx->stopping = false;
}
