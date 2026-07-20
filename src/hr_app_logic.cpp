/*
 * hr_app_logic.cpp  -  HomRec core application logic  (v1.6.2)
 *
 * Содержит всю бизнес-логику, вынесенную из homrec.py:
 *   - find_ffmpeg()           поиск ffmpeg в системе
 *   - optimize_for_performance()  настройка CPU/GC/GIL
 *   - GPU encoder probe       определение доступного GPU-кодека
 *   - build_codec_args()      построение аргументов FFmpeg для кодирования
 *   - launch_ffmpeg()         запуск процесса ffmpeg (gdigrab)
 *   - stop_ffmpeg()           корректная остановка ffmpeg
 *   - merge_audio_video()     склейка видео + WAV через ffmpeg
 *   - monitor_info()          получение геометрии монитора (через Win32)
 *   - enum_windows()          перечисление видимых окон (Win32)
 *   - version helpers         _version_gt(), CURRENT_VERSION
 *
 * Python-сторона (homrec.py) вызывает эти функции через ctypes.
 * Все строки - UTF-8, кроме явно помеченных как Wide.
 *
 * Build (MinGW-w64, Windows):
 *   g++ -O2 -std=c++17 -shared -static-libgcc -static-libstdc++ ^
 *       -o hr_app_logic.dll hr_app_logic.cpp ^
 *       -lkernel32 -luser32 -lpsapi -lshell32
 *
 * Build (GCC, Linux):
 *   g++ -O2 -std=c++17 -shared -fPIC -o hr_app_logic.so hr_app_logic.cpp
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#  include <shlobj.h>
#  include <wininet.h>   /* HINTERNET, INTERNET_PORT, etc. */
#  pragma comment(lib, "psapi.lib")
#  pragma comment(lib, "shell32.lib")
#  pragma comment(lib, "wininet.lib")
#  define HR_EXPORT extern "C" __declspec(dllexport)
   typedef HANDLE hr_proc_t;
#else
#  include <unistd.h>
#  include <sys/wait.h>
#  include <signal.h>
#  include <dirent.h>
#  define HR_EXPORT extern "C" __attribute__((visibility("default")))
   typedef pid_t hr_proc_t;
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <algorithm>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

/* --------------------------------------------------------------------------- */
/*  Internal helpers                                                            */
/* --------------------------------------------------------------------------- */

static std::string _str(const char *s) { return s ? std::string(s) : std::string(); }

/*
 * BUG FIX: run a shell command the same way system() does (blocking, returns
 * the process exit code), but without the visible cmd.exe flash that plain
 * system() causes on Windows. system() always spawns via cmd.exe, and it has
 * no way to pass CREATE_NO_WINDOW / SW_HIDE - the window is created and
 * shown by the OS before cmd.exe even gets a chance to run, so redirecting
 * the *command's* output (e.g. "... >nul 2>&1") does nothing to hide it.
 *
 * This was firing on every launch via hr_probe_gpu_encoder() (called ~3s
 * after startup to detect NVENC/AMF/QSV support), and again after any
 * recording that needed a separate audio/video merge - each one flashing a
 * console window that had nothing to do with any of the app's own windows.
 */
static int _run_hidden(const std::string &cmd) {
#ifdef _WIN32
    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    std::string full = "cmd.exe /c \"" + cmd + "\"";
    std::vector<char> cl(full.begin(), full.end());
    cl.push_back('\0');

    if (!CreateProcessA(nullptr, cl.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
#else
    return system(cmd.c_str());
#endif
}

/* Split string by delimiter */
static std::vector<std::string> _split(const std::string &s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) { out.push_back(cur); cur.clear(); }
        else            { cur += c; }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/* Safe string copy into fixed-size buffer */
static void _scopy(char *dst, size_t dstlen, const char *src) {
    if (!dst || !src || dstlen == 0) return;
    strncpy(dst, src, dstlen - 1);
    dst[dstlen - 1] = '\0';
}

/* --------------------------------------------------------------------------- */
/*  Version                                                                    */
/* --------------------------------------------------------------------------- */

static const char CURRENT_VERSION[] = "1.6.2";

/*
 * hr_version_string
 * Returns the compiled-in version string (e.g. "1.6.2").
 * Buffer must be at least 16 bytes.
 */
HR_EXPORT void hr_version_string(char *out, int out_len) {
    if (!out || out_len < 1) return;
    _scopy(out, (size_t)out_len, CURRENT_VERSION);
}

/*
 * hr_version_gt
 * Returns 1 if version string a is strictly greater than b.
 * Strings must be in "major.minor.patch" format.
 */
HR_EXPORT int hr_version_gt(const char *a, const char *b) {
    if (!a || !b) return 0;
    auto va = _split(a, '.');
    auto vb = _split(b, '.');
    size_t n = std::max(va.size(), vb.size());
    va.resize(n, "0"); vb.resize(n, "0");
    for (size_t i = 0; i < n; ++i) {
        int ia = atoi(va[i].c_str()), ib = atoi(vb[i].c_str());
        if (ia > ib) return 1;
        if (ia < ib) return 0;
    }
    return 0;
}

/* --------------------------------------------------------------------------- */
/*  find_ffmpeg                                                                 */
/* --------------------------------------------------------------------------- */

/*
 * hr_find_ffmpeg
 *
 * Mirrors Python find_ffmpeg():
 *   1. Directory of the calling executable (exe_dir parameter).
 *   2. Current working directory.
 *   3. Directories in PATH.
 *
 * exe_dir : UTF-8 path to the directory containing homrec.exe / homrec.py.
 *           Pass nullptr to skip step 1.
 * out     : receives the found path (UTF-8).
 * out_len : size of out buffer.
 * Returns 1 if found, 0 otherwise.
 */
HR_EXPORT int hr_find_ffmpeg(const char *exe_dir, char *out, int out_len) {
    if (!out || out_len < 2) return 0;
    out[0] = '\0';

    auto _check = [&](const std::string &path) -> bool {
        FILE *f = fopen(path.c_str(), "rb");
        if (f) { fclose(f); _scopy(out, (size_t)out_len, path.c_str()); return true; }
        return false;
    };

#ifdef _WIN32
    const std::vector<std::string> names = {"ffmpeg.exe", "ffmpeg"};
#else
    const std::vector<std::string> names = {"ffmpeg"};
#endif

    /* Step 1: exe directory */
    if (exe_dir && exe_dir[0]) {
        std::string dir = _str(exe_dir);
        if (dir.back() != '/' && dir.back() != '\\') dir += '/';
        for (const auto &nm : names)
            if (_check(dir + nm)) return 1;
    }

    /* Step 2: current working directory */
    for (const auto &nm : names)
        if (_check(nm)) return 1;

    /* Step 3: PATH */
#ifdef _WIN32
    char path_env[32768] = {};
    GetEnvironmentVariableA("PATH", path_env, sizeof(path_env));
    auto dirs = _split(path_env, ';');
#else
    const char *path_env = getenv("PATH");
    auto dirs = _split(path_env ? path_env : "", ':');
#endif
    for (const auto &dir : dirs) {
        std::string d = dir;
        if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
        for (const auto &nm : names)
            if (_check(d + nm)) return 1;
    }
    return 0;
}

/* --------------------------------------------------------------------------- */
/*  optimize_for_performance                                                    */
/* --------------------------------------------------------------------------- */

/*
 * hr_optimize_process
 *
 * Equivalent of Python optimize_for_performance():
 *   - Raises the process priority to HIGH on Windows / nice -10 on Linux.
 *   - On Windows also raises the current thread's priority.
 *
 * Call once at startup.  Returns 1 on success, 0 if permission denied.
 */
HR_EXPORT int hr_optimize_process(void) {
#ifdef _WIN32
    BOOL ok = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    return ok ? 1 : 0;
#else
    int r = nice(-10);
    return (r != -1) ? 1 : 0;
#endif
}

/* --------------------------------------------------------------------------- */
/*  Monitor geometry  (Windows only; stubs on Linux)                           */
/* --------------------------------------------------------------------------- */

struct HrMonitorInfo {
    int left, top, width, height;
    int index;          /* 1-based */
    char name[32];
};

/*
 * hr_get_monitor_info
 *
 * Fills info for monitor at 1-based index `idx`.
 * Returns 1 on success, 0 if index out of range.
 */
HR_EXPORT int hr_get_monitor_info(int idx, HrMonitorInfo *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));

#ifdef _WIN32
    /* Enumerate monitors in order */
    struct EnumCtx {
        int target;
        int current;
        MONITORINFO mi;
        bool found;
    } ctx = {idx, 0, {}, false};
    ctx.mi.cbSize = sizeof(MONITORINFO);

    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto *c = reinterpret_cast<EnumCtx *>(lp);
            c->current++;
            if (c->current == c->target) {
                GetMonitorInfoA(hm, &c->mi);
                c->found = true;
                return FALSE; /* stop enumeration */
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

    if (!ctx.found) return 0;
    out->left   = ctx.mi.rcMonitor.left;
    out->top    = ctx.mi.rcMonitor.top;
    out->width  = ctx.mi.rcMonitor.right  - ctx.mi.rcMonitor.left;
    out->height = ctx.mi.rcMonitor.bottom - ctx.mi.rcMonitor.top;
    out->index  = idx;
    snprintf(out->name, sizeof(out->name), "Monitor %d", idx);
    return 1;
#else
    /* On Linux homrec uses mss from Python; this is a stub. */
    out->left = out->top = 0;
    out->width = 1920; out->height = 1080;
    out->index = idx;
    snprintf(out->name, sizeof(out->name), "Monitor %d", idx);
    return 1;
#endif
}

/*
 * hr_monitor_count
 * Returns the total number of connected monitors.
 */
HR_EXPORT int hr_monitor_count(void) {
#ifdef _WIN32
    return GetSystemMetrics(SM_CMONITORS);
#else
    return 1;
#endif
}

/* --------------------------------------------------------------------------- */
/*  Window enumeration (Windows only)                                          */
/* --------------------------------------------------------------------------- */

/*
 * hr_enum_windows
 *
 * Fills buf with null-separated window titles, double-null terminated.
 * Returns the number of titles written, or -1 on error.
 *
 * Python equivalent of HomRecScreen.get_open_windows().
 */
HR_EXPORT int hr_enum_windows(char *buf, int buf_len) {
    if (!buf || buf_len < 2) return -1;
    memset(buf, 0, (size_t)buf_len);

#ifdef _WIN32
    struct Ctx {
        char *buf;
        int   remaining;
        int   count;
    } ctx = {buf, buf_len - 2, 0};

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lp);
        if (!IsWindowVisible(hwnd)) return TRUE;
        int len = GetWindowTextLengthW(hwnd);
        if (len <= 0) return TRUE;
        std::wstring ws(len + 1, L'\0');
        GetWindowTextW(hwnd, ws.data(), len + 1);
        ws.resize(len);
        /* Convert to UTF-8 */
        int needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1,
                                         nullptr, 0, nullptr, nullptr);
        if (needed <= 1 || needed > c->remaining) return TRUE;
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1,
                            c->buf, needed, nullptr, nullptr);
        c->buf       += needed;      /* advance past null terminator */
        c->remaining -= needed;
        c->count++;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.count;
#else
    return 0;
#endif
}

/* --------------------------------------------------------------------------- */
/*  GPU encoder probe                                                           */
/* --------------------------------------------------------------------------- */

/*
 * hr_probe_gpu_encoder
 *
 * Probes for a hardware-accelerated H.264 encoder via ffmpeg.
 * Tries h264_nvenc, h264_amf, h264_qsv in order.
 *
 * ffmpeg_path : UTF-8 path to ffmpeg binary.
 * out         : receives the encoder name (e.g. "h264_nvenc"), or empty.
 * out_len     : buffer size.
 * Returns 1 if a GPU encoder is found, 0 otherwise.
 *
 * Mirrors Python HomRecScreen._warm_up_gpu_probe() fallback path.
 */
HR_EXPORT int hr_probe_gpu_encoder(const char *ffmpeg_path,
                                   char *out, int out_len) {
    if (!ffmpeg_path || !out || out_len < 2) return 0;
    out[0] = '\0';

    /* Each entry: encoder name + ffmpeg args to test */
    struct Probe {
        const char *name;
        const char *args;  /* space-separated ffmpeg args after the ffmpeg binary */
    };
    static const Probe probes[] = {
        {"h264_nvenc", "-y -f lavfi -i nullsrc=s=32x32:d=0.1 -c:v h264_nvenc -f null -"},
        {"h264_amf",   "-y -f lavfi -i nullsrc=s=32x32:d=0.1 -c:v h264_amf   -f null -"},
        {"h264_qsv",   "-y -f lavfi -i nullsrc=s=32x32:d=0.1 -c:v h264_qsv   -f null -"},
        {nullptr, nullptr}
    };

    for (int i = 0; probes[i].name; ++i) {
        /* Build command */
        std::string cmd = "\"" + _str(ffmpeg_path) + "\" " + probes[i].args;
#ifdef _WIN32
        cmd += " >nul 2>&1";
#else
        cmd += " >/dev/null 2>&1";
#endif
        int rc = _run_hidden(cmd);
        if (rc == 0) {
            _scopy(out, (size_t)out_len, probes[i].name);
            return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------- */
/*  build_codec_args                                                            */
/* --------------------------------------------------------------------------- */

/*
 * hr_build_codec_args used to be defined here too (narrow-char, void-
 * returning, mirroring Python's HomRecScreen._build_codec_args() directly).
 * It was DEAD CODE - the only real caller, recording_controller.cpp, has
 * always used the wide-char, int-returning version in hr_tools.cpp instead
 * - and being HR_EXPORT (extern "C") meant both shared one unmangled
 * linker symbol name, which finally surfaced as a "multiple definition of
 * `hr_build_codec_args`" link error once every source file started
 * actually compiling clean. Removed rather than renamed: this copy also
 * still had the QSV `-low_power 1` flag that hr_tools.cpp's version
 * documents fixing (see the BUG FIX comment there), so keeping it around
 * even under a new name would just reintroduce a second, worse
 * implementation of the same thing.
 */

/* --------------------------------------------------------------------------- */
/*  FFmpeg process management                                                   */
/* --------------------------------------------------------------------------- */

/* Opaque handle returned by hr_launch_ffmpeg */
struct HrFfmpegProc {
#ifdef _WIN32
    HANDLE hProcess;
    HANDLE hThread;
    HANDLE hStdin;
#else
    pid_t  pid;
    int    stdin_fd;
#endif
    std::atomic<bool> running;

    HrFfmpegProc()
#ifdef _WIN32
        : hProcess(INVALID_HANDLE_VALUE),
          hThread(INVALID_HANDLE_VALUE),
          hStdin(INVALID_HANDLE_VALUE),
#else
        : pid(-1), stdin_fd(-1),
#endif
          running(false) {}
};

/*
 * hr_launch_ffmpeg
 *
 * Launches ffmpeg with the provided argv array (null-terminated list of
 * UTF-8 strings).  Returns an opaque handle on success, nullptr on failure.
 *
 * The handle must be freed with hr_stop_ffmpeg() followed by hr_free_ffmpeg().
 *
 * This replaces the Python subprocess.Popen call in HomRecScreen.start_recording().
 */
HR_EXPORT void *hr_launch_ffmpeg(const char *const *argv) {
    if (!argv || !argv[0]) return nullptr;

    auto *proc = new(std::nothrow) HrFfmpegProc();
    if (!proc) return nullptr;

#ifdef _WIN32
    /* Build a single quoted command line from argv */
    std::string cmdline;
    for (int i = 0; argv[i]; ++i) {
        if (i) cmdline += ' ';
        bool needs_quote = (strchr(argv[i], ' ') || strchr(argv[i], '\t'));
        if (needs_quote) cmdline += '"';
        cmdline += argv[i];
        if (needs_quote) cmdline += '"';
    }

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE hStdinRd = INVALID_HANDLE_VALUE, hStdinWr = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&hStdinRd, &hStdinWr, &sa, 0)) {
        delete proc; return nullptr;
    }
    SetHandleInformation(hStdinWr, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput   = hStdinRd;
    si.hStdOutput  = INVALID_HANDLE_VALUE;
    si.hStdError   = INVALID_HANDLE_VALUE;

    PROCESS_INFORMATION pi = {};
    std::vector<char> cl(cmdline.begin(), cmdline.end());
    cl.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, cl.data(), nullptr, nullptr,
                              TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                              &si, &pi);
    CloseHandle(hStdinRd);

    if (!ok) { CloseHandle(hStdinWr); delete proc; return nullptr; }

    proc->hProcess = pi.hProcess;
    proc->hThread  = pi.hThread;
    proc->hStdin   = hStdinWr;

    /* Boost FFmpeg priority */
    SetPriorityClass(pi.hProcess, HIGH_PRIORITY_CLASS);

#else
    /* POSIX: use posix_spawn or fork/exec */
    std::vector<char*> args;
    for (int i = 0; argv[i]; ++i) args.push_back((char*)argv[i]);
    args.push_back(nullptr);

    int pfd[2];
    if (pipe(pfd) != 0) { delete proc; return nullptr; }

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); delete proc; return nullptr; }
    if (pid == 0) {
        dup2(pfd[0], STDIN_FILENO);
        close(pfd[0]); close(pfd[1]);
        /* redirect stdout/stderr to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
        execvp(args[0], args.data());
        _exit(127);
    }
    close(pfd[0]);
    proc->pid      = pid;
    proc->stdin_fd = pfd[1];
#endif

    proc->running = true;
    return proc;
}

/*
 * hr_stop_ffmpeg
 *
 * Sends 'q' to ffmpeg's stdin and waits up to `timeout_ms` milliseconds.
 * If ffmpeg doesn't exit, it is killed.
 * Returns 1 if it exited cleanly, 0 if killed.
 */
HR_EXPORT int hr_stop_ffmpeg(void *handle, int timeout_ms) {
    if (!handle) return 0;
    auto *proc = static_cast<HrFfmpegProc *>(handle);
    if (!proc->running.exchange(false)) return 1;

#ifdef _WIN32
    if (proc->hStdin != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(proc->hStdin, "q", 1, &written, nullptr);
        FlushFileBuffers(proc->hStdin);
    }
    if (proc->hProcess != INVALID_HANDLE_VALUE) {
        DWORD rc = WaitForSingleObject(proc->hProcess, (DWORD)timeout_ms);
        if (rc == WAIT_TIMEOUT) {
            TerminateProcess(proc->hProcess, 1);
            WaitForSingleObject(proc->hProcess, 2000);
            return 0;
        }
    }
    return 1;
#else
    if (proc->stdin_fd >= 0) {
        (void)write(proc->stdin_fd, "q", 1);
        close(proc->stdin_fd);
        proc->stdin_fd = -1;
    }
    if (proc->pid > 0) {
        /* Poll until timeout */
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (true) {
            int status;
            pid_t r = waitpid(proc->pid, &status, WNOHANG);
            if (r == proc->pid) return 1;
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        kill(proc->pid, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        kill(proc->pid, SIGKILL);
        waitpid(proc->pid, nullptr, 0);
        return 0;
    }
    return 1;
#endif
}

/*
 * hr_free_ffmpeg
 * Release the handle previously returned by hr_launch_ffmpeg.
 */
HR_EXPORT void hr_free_ffmpeg(void *handle) {
    if (!handle) return;
    auto *proc = static_cast<HrFfmpegProc *>(handle);
#ifdef _WIN32
    if (proc->hStdin   != INVALID_HANDLE_VALUE) CloseHandle(proc->hStdin);
    if (proc->hThread  != INVALID_HANDLE_VALUE) CloseHandle(proc->hThread);
    if (proc->hProcess != INVALID_HANDLE_VALUE) CloseHandle(proc->hProcess);
#else
    if (proc->stdin_fd >= 0) close(proc->stdin_fd);
#endif
    delete proc;
}

/*
 * hr_ffmpeg_running
 * Returns 1 if the process is still alive, 0 if it has exited.
 */
HR_EXPORT int hr_ffmpeg_running(void *handle) {
    if (!handle) return 0;
    auto *proc = static_cast<HrFfmpegProc *>(handle);
    if (!proc->running) return 0;
#ifdef _WIN32
    if (proc->hProcess == INVALID_HANDLE_VALUE) return 0;
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(proc->hProcess, &code);
    if (code != STILL_ACTIVE) { proc->running = false; return 0; }
    return 1;
#else
    int status;
    pid_t r = waitpid(proc->pid, &status, WNOHANG);
    if (r == proc->pid) { proc->running = false; return 0; }
    return 1;
#endif
}

/* --------------------------------------------------------------------------- */
/*  merge_audio_video                                                           */
/* --------------------------------------------------------------------------- */

/*
 * hr_merge_audio_video
 *
 * Merges a silent MP4 (video_path) with a WAV file (audio_path) using ffmpeg.
 * Produces an in-place replacement of video_path (atomic rename).
 *
 * Mirrors Python HomRecScreen.merge_audio_video().
 *
 * Returns 1 on success, 0 on failure.
 */
HR_EXPORT int hr_merge_audio_video(const char *ffmpeg_path,
                                   const char *video_path,
                                   const char *audio_path) {
    if (!ffmpeg_path || !video_path || !audio_path) return 0;

    /* Build temporary output path */
    std::string tmp = _str(video_path);
    {
        auto pos = tmp.rfind(".mp4");
        if (pos != std::string::npos) tmp.replace(pos, 4, "_merge_tmp.mp4");
        else tmp += "_merge_tmp.mp4";
    }

    std::string cmd =
        "\"" + _str(ffmpeg_path) + "\""
        " -i \"" + _str(video_path) + "\""
        " -i \"" + _str(audio_path) + "\""
        " -c:v copy -c:a aac -af aresample=async=1000"
        " -map 0:v:0 -map 1:a:0 -shortest -y"
        " \"" + tmp + "\"";
#ifdef _WIN32
    cmd += " >nul 2>&1";
#else
    cmd += " >/dev/null 2>&1";
#endif

    int rc = _run_hidden(cmd);
    if (rc != 0) return 0;

    /* Check tmp exists */
    {
        FILE *f = fopen(tmp.c_str(), "rb");
        if (!f) return 0;
        fclose(f);
    }

    /* Atomic replace: remove old video, audio; rename tmp → video */
    remove(video_path);
    remove(audio_path);
#ifdef _WIN32
    if (!MoveFileA(tmp.c_str(), video_path)) {
        CopyFileA(tmp.c_str(), video_path, FALSE);
        remove(tmp.c_str());
    }
#else
    rename(tmp.c_str(), video_path);
#endif
    return 1;
}

/* --------------------------------------------------------------------------- */
/*  Performance / timing                                                        */
/* --------------------------------------------------------------------------- */

/*
 * hr_monotonic_ms
 * Returns a monotonic timestamp in milliseconds (for elapsed-time tracking).
 */
HR_EXPORT int64_t hr_monotonic_ms(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
#endif
}

/*
 * hr_format_elapsed
 *
 * Given elapsed seconds, formats "HH:MM:SS" into out.
 * out must be at least 9 bytes.
 */
HR_EXPORT void hr_format_elapsed(double elapsed_sec, char *out, int out_len) {
    if (!out || out_len < 9) return;
    int total = (int)elapsed_sec;
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    snprintf(out, (size_t)out_len, "%02d:%02d:%02d", h, m, s);
}

/* --------------------------------------------------------------------------- */
/*  File type registration (Windows registry)                                  */
/* --------------------------------------------------------------------------- */

/*
 * hr_register_file_types
 *
 * Registers .hrc / .hrl / .hrt file associations in HKCU (no admin needed).
 * Mirrors Python HomRecScreen._register_file_types().
 *
 * exe_path : full path to the executable that opens these files (UTF-8).
 * icons_dir: directory containing hrc.ico / hrl.ico / hrt.ico (UTF-8).
 * Returns 1 on success, 0 on error.
 */
HR_EXPORT int hr_register_file_types(const char *exe_path,
                                     const char *icons_dir) {
#ifdef _WIN32
    if (!exe_path) return 0;

    struct TypeDef {
        const char *ext;
        const char *prog_id;
        const char *description;
        const char *ico_file;
    };
    static const TypeDef types[] = {
        {".hrc", "HomRec.Profile",  "HomRec Profile",  "hrc.ico"},
        {".hrl", "HomRec.Language", "HomRec Language", "hrl.ico"},
        {".hrt", "HomRec.Theme",    "HomRec Theme",    "hrt.ico"},
        {nullptr, nullptr, nullptr, nullptr}
    };

    for (int i = 0; types[i].ext; ++i) {
        const char *ext     = types[i].ext;
        const char *prog_id = types[i].prog_id;
        const char *desc    = types[i].description;

        std::string icon_path;
        if (icons_dir && icons_dir[0]) {
            icon_path = _str(icons_dir);
            if (icon_path.back() != '\\' && icon_path.back() != '/')
                icon_path += '\\';
            icon_path += types[i].ico_file;
        }

        /* Register extension → ProgID */
        std::string ext_key = std::string("Software\\Classes\\") + ext;
        HKEY hk;
        if (RegCreateKeyExA(HKEY_CURRENT_USER, ext_key.c_str(),
                            0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
            RegSetValueA(hk, "", REG_SZ, prog_id, (DWORD)strlen(prog_id));
            RegCloseKey(hk);
        }

        /* Register ProgID description */
        std::string pid_key = std::string("Software\\Classes\\") + prog_id;
        if (RegCreateKeyExA(HKEY_CURRENT_USER, pid_key.c_str(),
                            0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
            RegSetValueA(hk, "", REG_SZ, desc, (DWORD)strlen(desc));
            RegCloseKey(hk);
        }

        /* Set icon */
        if (!icon_path.empty()) {
            FILE *f = fopen(icon_path.c_str(), "rb");
            if (f) {
                fclose(f);
                std::string ico_key = pid_key + "\\DefaultIcon";
                if (RegCreateKeyExA(HKEY_CURRENT_USER, ico_key.c_str(),
                                    0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
                    RegSetValueA(hk, "", REG_SZ, icon_path.c_str(), (DWORD)icon_path.size());
                    RegCloseKey(hk);
                }
            }
        }

        /* Set open command */
        std::string open_cmd = "\"" + _str(exe_path) + "\" \"%1\"";
        std::string cmd_key = pid_key + "\\shell\\open\\command";
        if (RegCreateKeyExA(HKEY_CURRENT_USER, cmd_key.c_str(),
                            0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
            RegSetValueA(hk, "", REG_SZ, open_cmd.c_str(), (DWORD)open_cmd.size());
            RegCloseKey(hk);
        }
    }

    /* Notify Explorer */
    SHChangeNotify(0x08000000 /* SHCNE_ASSOCCHANGED */, 0, nullptr, nullptr);
    return 1;
#else
    (void)exe_path; (void)icons_dir;
    return 0; /* not applicable on Linux/macOS */
#endif
}

/* --------------------------------------------------------------------------- */
/*  Single-instance mutex (Windows)                                             */
/* --------------------------------------------------------------------------- */

/*
 * hr_acquire_single_instance
 *
 * Creates a named Win32 mutex.  Returns 1 if this is the first instance,
 * 0 if another instance is already running.
 * Call once at startup; the mutex is released automatically when the process
 * exits (handle is intentionally leaked).
 */
HR_EXPORT int hr_acquire_single_instance(const char *mutex_name) {
#ifdef _WIN32
    if (!mutex_name) return 1;
    HANDLE h = CreateMutexA(nullptr, FALSE, mutex_name);
    if (!h) return 1;                    /* CreateMutex failed - allow startup */
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        return 0;                        /* another instance running */
    }
    /* Intentionally leak h so the OS releases it on process exit */
    return 1;
#else
    (void)mutex_name;
    return 1;
#endif
}

/* --------------------------------------------------------------------------- */
/*  Update check (synchronous, call from a worker thread)                      */
/* --------------------------------------------------------------------------- */

/*
 * hr_fetch_latest_version
 *
 * Fetches the latest release tag from GitHub API (synchronous HTTP GET).
 * Uses WinINet on Windows, libcurl-less raw sockets elsewhere.
 *
 * repo      : "owner/repo" (e.g. "homaaio/homrec")
 * out       : receives version string like "1.6.3" (without 'v' prefix)
 * out_len   : buffer size
 * Returns 1 if a version was retrieved, 0 on any error.
 *
 * NOTE: This function blocks the calling thread.  Python wraps it in
 *       threading.Thread(target=..., daemon=True).
 */
HR_EXPORT int hr_fetch_latest_version(const char *repo,
                                      char *out, int out_len) {
    if (!repo || !out || out_len < 4) return 0;
    out[0] = '\0';

#ifdef _WIN32
    /* Use WinINet - available on all Windows versions, no extra DLL */
    HMODULE hWinInet = LoadLibraryA("wininet.dll");
    if (!hWinInet) return 0;

    typedef HINTERNET (WINAPI *pfnOpen)(LPCSTR,DWORD,LPCSTR,LPCSTR,DWORD);
    typedef HINTERNET (WINAPI *pfnConn)(HINTERNET,LPCSTR,INTERNET_PORT,LPCSTR,LPCSTR,DWORD,DWORD,DWORD_PTR);
    typedef HINTERNET (WINAPI *pfnReq) (HINTERNET,LPCSTR,LPCSTR,LPCSTR,LPCSTR,LPCSTR*,DWORD,DWORD_PTR);
    typedef BOOL      (WINAPI *pfnSend)(HINTERNET,LPCSTR,DWORD,LPVOID,DWORD,DWORD,DWORD_PTR);
    typedef BOOL      (WINAPI *pfnRead)(HINTERNET,LPVOID,DWORD,LPDWORD);
    typedef BOOL      (WINAPI *pfnClose)(HINTERNET);

    auto _Open  = (pfnOpen) GetProcAddress(hWinInet, "InternetOpenA");
    auto _Conn  = (pfnConn) GetProcAddress(hWinInet, "InternetConnectA");
    auto _Req   = (pfnReq)  GetProcAddress(hWinInet, "HttpOpenRequestA");
    auto _Send  = (pfnSend) GetProcAddress(hWinInet, "HttpSendRequestA");
    auto _Read  = (pfnRead) GetProcAddress(hWinInet, "InternetReadFile");
    auto _Close = (pfnClose)GetProcAddress(hWinInet, "InternetCloseHandle");

    if (!_Open || !_Conn || !_Req || !_Send || !_Read || !_Close) {
        FreeLibrary(hWinInet); return 0;
    }

    std::string path = std::string("/repos/") + repo + "/releases/latest";

    HINTERNET hSession = _Open("HomRec/1.6.2", INTERNET_OPEN_TYPE_PRECONFIG,
                                nullptr, nullptr, 0);
    if (!hSession) { FreeLibrary(hWinInet); return 0; }

    HINTERNET hConn = _Conn(hSession, "api.github.com", INTERNET_DEFAULT_HTTPS_PORT,
                            nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) { _Close(hSession); FreeLibrary(hWinInet); return 0; }

    /* INTERNET_FLAG_SECURE (0x00800000) | INTERNET_FLAG_NO_CACHE_WRITE */
    HINTERNET hReq = _Req(hConn, "GET", path.c_str(), nullptr, nullptr,
                          nullptr, 0x00800000u | 0x04000000u, 0);
    if (!hReq) { _Close(hConn); _Close(hSession); FreeLibrary(hWinInet); return 0; }

    BOOL ok = _Send(hReq, nullptr, 0, nullptr, 0, 0, 0);
    std::string body;
    if (ok) {
        char chunk[4096];
        DWORD read = 0;
        while (_Read(hReq, chunk, sizeof(chunk)-1, &read) && read > 0) {
            chunk[read] = '\0';
            body += chunk;
        }
    }
    _Close(hReq); _Close(hConn); _Close(hSession);
    FreeLibrary(hWinInet);

    /* Parse "tag_name":"v1.6.3" */
    const char *key = "\"tag_name\"";
    const char *p = strstr(body.c_str(), key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ' || *p == ':') ++p;
    if (*p != '"') return 0; ++p;
    if (*p == 'v' || *p == 'V') ++p;  /* strip leading 'v' */
    int i = 0;
    while (*p && *p != '"' && i < out_len - 1) out[i++] = *p++;
    out[i] = '\0';
    return i > 0 ? 1 : 0;
#else
    /* Linux/macOS: not implemented inline; Python uses urllib */
    (void)repo;
    return 0;
#endif
}
