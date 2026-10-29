// homrec.cpp - "homrec", the HomRec command-line interface.
//
// Standalone tool like hom.exe: it does NOT link wxWidgets or Lua and is built separately
// (`make homrec`). Keep homrec.exe next to hr.exe and put that folder in PATH, then
// `homrec ...` works from any terminal location - everything it needs is looked up
// relative to its own exe, never the current directory (see homrec_binding.h).
//
// Commands:
//   homrec --version                 show the HomRec version
//   homrec config path               show the .hrc config homrec is bound to
//   homrec config change <path>      bind homrec to another .hrc config
//   homrec start [config-path]       record the screen (bound config, or the given .hrc); Ctrl+C stops + saves
//   homrec pause                     pause / resume the recording started by 'homrec start'
//
// 'start' uses the same RecordingController as hr.exe (no wxWidgets involved), so the engine objects
// are linked into homrec.exe - see the homrec target in the Makefile / CMakeLists.txt.
//
// To add a command: write a `int CmdXxx(const Args &)` below and add one Leaf()/Group() line
// to BuildCommands(). Help and error messages come from that table automatically.

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "homrec_binding.h"
#include "homrec_cli.h"
#include "../../src/ui/version.h"   // HR_APP_VERSION - single source of truth for the version

#ifdef _WIN32
  #include "../../src/ui/app_state.h"
  #include "../../src/ui/hrc_config.h"
  #include "../../src/ui/recording_controller.h"
  #include "../../src/utils/wgc_log.h"

  // pipeline.cpp - waits for capture pipelines that were handed off to a background thread; hr.exe
  // calls it from OnExit() (win_main.cpp) for the same reason.
  extern "C" int hr_pl_wait_all_detached(int timeout_ms);
#endif

using namespace hrcli;

namespace {

// Paths and file names are printed as UTF-8. On Windows the console is switched to the UTF-8
// code page only while we run and put back afterwards, so cmd/PowerShell are left as they were.
struct ConsoleUtf8 {
#ifdef _WIN32
    UINT old_cp;
    ConsoleUtf8() : old_cp(GetConsoleOutputCP()) { if (old_cp) SetConsoleOutputCP(CP_UTF8); }
    ~ConsoleUtf8() { if (old_cp) SetConsoleOutputCP(old_cp); }
#endif
};

bool HasHrcExtension(const fs::path &p) {
    std::string ext = p.extension().u8string();
    for (char &c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".hrc";
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int CmdVersion(const Args &args) {
    if (!args.empty()) return Fail("--version takes no arguments");
    std::printf("homrec %s\n", HR_APP_VERSION);
    return 0;
}

// Prints only the path on stdout, so `$(homrec config path)` is usable in scripts.
int CmdConfigPath(const Args &args) {
    if (!args.empty()) return Fail("config path takes no arguments");
    const Binding b = ReadBinding();
    std::printf("%s\n", b.path.u8string().c_str());
    std::error_code ec;
    if (!fs::exists(b.path, ec))
        std::fprintf(stderr, "homrec: note: that file does not exist yet\n");
    return 0;
}

int CmdConfigChange(const Args &args) {
    if (args.empty())
        return Fail("config change needs the path of an .hrc file");
    if (args.size() > 1)
        return Fail("config change takes one path (use quotes if it contains spaces)");

    // Relative input is resolved against the directory homrec was started from - this is the
    // one place the cwd matters - and stored as an absolute path so the result is the same
    // from every other directory afterwards.
    std::error_code ec;
    fs::path target = fs::absolute(fs::u8path(args[0]), ec);
    if (ec || args[0].empty()) return Fail("'" + args[0] + "' is not a valid path");
    target = target.lexically_normal();
    const std::string shown = target.u8string();

    if (!fs::exists(target, ec))   return Fail("'" + shown + "' not found");
    if (fs::is_directory(target, ec))
        return Fail("'" + shown + "' is a folder - give the path of an .hrc file");
    if (!HasHrcExtension(target))  return Fail("'" + shown + "' is not an .hrc file");

    const Binding old = ReadBinding();
    if (!old.is_default && old.path == target) {
        std::printf("Already bound to %s\n", shown.c_str());
        return 0;
    }

    std::string err;
    if (!WriteBinding(target, &err)) return Fail(err);
    std::printf("Config changed to %s\n", shown.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// 'start' / 'pause' - text pieces (pure functions, no terminal I/O)
// ---------------------------------------------------------------------------

// Same look as the bouncing bar in tools/homrec_build.py: a 7-cell block bouncing over a dim track.
constexpr int kBarWidth  = 28;
constexpr int kWaveWidth = 7;

// Wraps `s` in an ANSI colour when `color` is on, leaves it alone otherwise.
[[maybe_unused]] std::string Paint(bool color, const char *sgr, const std::string &s) {
    return color ? std::string("\x1b[") + sgr + "m" + s + "\x1b[0m" : s;
}

[[maybe_unused]] std::string FormatClock(double sec) {
    const long long t = sec < 0 ? 0 : (long long)sec;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    return buf;
}

[[maybe_unused]] std::string FormatSize(double mb) {
    char buf[32];
    if (mb >= 1024.0) std::snprintf(buf, sizeof(buf), "%.2f GB", mb / 1024.0);
    else              std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
    return buf;
}

// "[   block   ]" bouncing left -> right -> left. While a recording is finalized the engine reports
// no progress, so this is a "working" indicator, not a percentage.
// `fancy` = ANSI colours + block characters; otherwise plain ASCII.
[[maybe_unused]] std::string SavingBar(int tick, bool fancy) {
    const int span   = kBarWidth - kWaveWidth;      // positions the block can take
    const int period = 2 * span;
    const int t      = tick % period;
    const int pos    = t <= span ? t : period - t;

    std::string out = "[";
    if (fancy) {
        out += "\x1b[90m";
        for (int i = 0; i < pos; ++i) out += "\u2591";
        out += "\x1b[36m";
        for (int i = 0; i < kWaveWidth; ++i) out += "\u2588";
        out += "\x1b[90m";
        for (int i = pos + kWaveWidth; i < kBarWidth; ++i) out += "\u2591";
        out += "\x1b[0m";
    } else {
        for (int i = 0; i < kBarWidth; ++i) out += (i >= pos && i < pos + kWaveWidth) ? '#' : '.';
    }
    out += ']';
    return out;
}

[[maybe_unused]] std::string DoneBar(bool fancy) {
    std::string out = "[";
    if (fancy) {
        out += "\x1b[92m";
        for (int i = 0; i < kBarWidth; ++i) out += "\u2588";
        out += "\x1b[0m";
    } else {
        out.append((size_t)kBarWidth, '#');
    }
    out += ']';
    return out;
}

// "* REC  00:01:23  42.5 MB  60 fps" - kept well under 80 columns.
[[maybe_unused]] std::string RecLine(bool fancy, bool paused, double sec, double mb, double fps, bool dropping) {
    std::string s = paused ? Paint(fancy, "93", "|| PAUSED")
                           : Paint(fancy, "91", fancy ? "\u25CF" : "*") + " REC";
    s += "  " + FormatClock(sec) + "  " + FormatSize(mb);
    if (!paused && fps >= 1.0) {
        char b[24];
        std::snprintf(b, sizeof(b), "  %.0f fps", fps);
        s += b;
    }
    if (dropping) s += "  " + Paint(fancy, "93", "! dropping frames");
    return s;
}

// "Saving  [bar] (3.2s)" - the timer turns yellow after 2 s and red after 20 s, like the release builder.
[[maybe_unused]] std::string SavingLine(bool fancy, int tick, double sec, bool ctrlc_pressed) {
    std::string s = "Saving  " + SavingBar(tick, fancy);
    if (sec >= 2.0) {
        char b[24];
        std::snprintf(b, sizeof(b), " (%.1fs)", sec);
        s += Paint(fancy, sec >= 20.0 ? "31" : "33", b);
    }
    if (ctrlc_pressed) s += "  " + Paint(fancy, "90", "(Ctrl+C: please wait)");
    return s;
}

#ifdef _WIN32

// ---------------------------------------------------------------------------
// 'start' / 'pause' - Windows side
// ---------------------------------------------------------------------------

// Named kernel objects through which 'homrec pause' finds and drives the running 'homrec start'.
constexpr wchar_t kLockName[]  = L"Local\\HomRecCli.Session";   // one recording at a time
constexpr wchar_t kPauseName[] = L"Local\\HomRecCli.Pause";     // signalled by 'homrec pause'
constexpr wchar_t kStateName[] = L"Local\\HomRecCli.State";     // one LONG: what the recorder is doing

enum : LONG { kStateStarting = 0, kStateRecording = 1, kStatePaused = 2, kStateSaving = 3 };

std::atomic<bool> g_stop{false};     // Ctrl+C (or console close) was pressed
std::atomic<int>  g_stop_hits{0};
HANDLE g_stop_ev = nullptr;          // manual-reset, signalled together with g_stop
HANDLE g_done_ev = nullptr;          // signalled once everything is saved (console close waits on it)

// Runs on a thread of its own. Ctrl+C only asks the main loop to stop (so the file is finalized
// properly); it never kills the process, not even a second time while saving.
BOOL WINAPI CtrlHandler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        ++g_stop_hits;
        g_stop = true;
        if (g_stop_ev) SetEvent(g_stop_ev);
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop = true;
        if (g_stop_ev) SetEvent(g_stop_ev);
        // Windows ends the process soon after this returns - give the save a few seconds.
        if (g_done_ev) WaitForSingleObject(g_done_ev, 4500);
        return TRUE;
    default:
        return FALSE;
    }
}

std::string WideToUtf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string s((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], len, nullptr, nullptr);
    return s;
}

// Same as win_main.cpp: capture sizes are wrong on scaled displays unless the process is DPI aware.
void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetCtxFn = BOOL(WINAPI *)(HANDLE);
        auto setCtx = reinterpret_cast<SetCtxFn>(reinterpret_cast<void *>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        if (setCtx && setCtx((HANDLE)-4)) return;   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }
    SetProcessDPIAware();
}

// The terminal. Interactive console: ANSI colours/blocks when the console accepts them (Windows 10+),
// one in-place status line. Redirected output: plain lines only, no animation.
class Term {
public:
    Term() : out_(GetStdHandle(STD_OUTPUT_HANDLE)) {
        DWORD mode = 0;
        if (out_ && out_ != INVALID_HANDLE_VALUE && GetConsoleMode(out_, &mode)) {
            interactive_ = true;
            old_mode_ = mode;
            fancy_ = SetConsoleMode(out_, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
        }
    }
    ~Term() {
        if (cursor_hidden_) Write("\x1b[?25h");
        if (interactive_ && fancy_) SetConsoleMode(out_, old_mode_);   // leave the console as we found it
    }
    Term(const Term &) = delete;
    Term &operator=(const Term &) = delete;

    bool interactive() const { return interactive_; }
    bool fancy() const { return fancy_; }

    // A permanent line (scrolls away like normal output). Replaces a status line that is showing.
    void Line(const std::string &s) { Clear(); Write(s + (interactive_ ? "\r\n" : "\n")); }

    // Rewrites the single status line in place. Console only.
    void Live(const std::string &s) {
        if (!interactive_) return;
        if (fancy_ && !cursor_hidden_) { Write("\x1b[?25l"); cursor_hidden_ = true; }
        std::string frame = "\r";
        if (fancy_) {
            frame += "\x1b[2K" + s;
        } else {
            frame += s;
            if (s.size() < live_len_) frame.append(live_len_ - s.size(), ' ');   // erase the longer old text
        }
        Write(frame);
        live_len_ = s.size();
        live_ = true;
    }

    // Keeps the status line on screen as it is and moves on to the next line.
    void EndLive() {
        if (!live_) return;
        Write("\r\n");
        live_ = false;
        live_len_ = 0;
    }

    // Removes the status line.
    void Clear() {
        if (!live_) return;
        if (fancy_) Write("\r\x1b[2K");
        else        Write("\r" + std::string(live_len_, ' ') + "\r");
        live_ = false;
        live_len_ = 0;
    }

private:
    // UTF-8 in. Console: WriteConsoleW, so the code page doesn't matter.
    void Write(const std::string &s) {
        if (s.empty()) return;
        if (interactive_) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
            if (n > 0) {
                std::wstring w((size_t)n, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
                DWORD written = 0;
                WriteConsoleW(out_, w.c_str(), (DWORD)w.size(), &written, nullptr);
                return;
            }
        }
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fflush(stdout);
    }

    HANDLE out_;
    DWORD  old_mode_ = 0;
    bool   interactive_ = false;
    bool   fancy_ = false;
    bool   live_ = false;
    bool   cursor_hidden_ = false;
    size_t live_len_ = 0;
};

// The recorder's end of the pause channel (see kLockName / kPauseName / kStateName above).
class CliSession {
public:
    CliSession() = default;
    CliSession(const CliSession &) = delete;
    CliSession &operator=(const CliSession &) = delete;
    ~CliSession() {
        if (view_) UnmapViewOfFile((LPCVOID)view_);
        if (map_)   CloseHandle(map_);
        if (pause_) CloseHandle(pause_);
        if (lock_)  CloseHandle(lock_);
    }

    // False (and *err filled) when another 'homrec start' is already recording.
    bool Open(std::string *err) {
        lock_ = CreateMutexW(nullptr, FALSE, kLockName);
        if (!lock_) { *err = "cannot create the session lock"; return false; }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            *err = "a recording is already running - stop it first (Ctrl+C in its window)";
            return false;
        }
        pause_ = CreateEventW(nullptr, FALSE, FALSE, kPauseName);   // auto-reset: one 'homrec pause' = one toggle
        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG), kStateName);
        if (map_) view_ = static_cast<volatile LONG *>(MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG)));
        if (!pause_ || !view_) { *err = "cannot create the pause channel"; return false; }
        return true;
    }

    HANDLE pause_event() const { return pause_; }
    void SetState(LONG s) { if (view_) InterlockedExchange(view_, s); }

private:
    HANDLE lock_  = nullptr;
    HANDLE pause_ = nullptr;
    HANDLE map_   = nullptr;
    volatile LONG *view_ = nullptr;
};

// The recording itself: countdown -> record (status line) -> save (bar) -> summary.
// `rec` lives in here, so it is destroyed before the caller waits for the pipelines to wind down.
int Record(AppState &state, Term &term, CliSession &session) {
    const bool fancy = term.fancy();

    term.Live("Preparing...");              // probing the GPU encoder can take a few seconds
    RecordingController rec(state);
    rec.Initialize();

    // Same 3-2-1 as the main window when 'countdown' is on in the config.
    if (state.countdown_enabled) {
        for (int n = 3; n > 0 && !g_stop.load(); --n) {
            term.Live("Starting in " + std::to_string(n) + "...");
            WaitForSingleObject(g_stop_ev, 1000);
        }
        if (g_stop.load()) { term.Line("Cancelled."); return 0; }
    }

    std::wstring err;
    if (!rec.Start(err)) {
        term.Clear();
        return Fail(WideToUtf8(err));
    }
    session.SetState(kStateRecording);
    term.Line(Paint(fancy, "90", "Ctrl+C = stop and save   |   homrec pause = pause / resume"));

    // -- recording: ~4 wake-ups a second (PollStats() wants 250-500 ms), or at once on pause / Ctrl+C --
    bool crashed = false;
    const HANDLE waits[2] = {session.pause_event(), g_stop_ev};
    while (!g_stop.load()) {
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 250);
        if (w == WAIT_OBJECT_0) {            // 'homrec pause'
            rec.TogglePause();
            session.SetState(rec.paused() ? kStatePaused : kStateRecording);
        } else if (w == WAIT_OBJECT_0 + 1) { // Ctrl+C
            break;
        }
        rec.PollStats();
        if (rec.crashed()) {                 // ffmpeg / the encoder died - save what there is
            rec.AcknowledgeCrash();
            crashed = true;
            break;
        }
        term.Live(RecLine(fancy, rec.paused(), rec.elapsed_seconds(), rec.output_size_mb(),
                          rec.current_fps(), rec.overloaded()));
    }
    term.EndLive();

    // -- saving: StopAsync() finalizes on a background thread; we only animate until it reports back --
    session.SetState(kStateSaving);
    if (!term.interactive()) term.Line("Saving...");
    const auto t0 = std::chrono::steady_clock::now();
    const int hits0 = g_stop_hits.load();
    auto done = std::make_shared<std::atomic<bool>>(false);
    rec.StopAsync([done]() { *done = true; });
    for (int tick = 0; !done->load(); ++tick) {
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        term.Live(SavingLine(fancy, tick, sec, g_stop_hits.load() > hits0));
        Sleep(50);
    }
    term.Live("Saved   " + DoneBar(fancy));
    term.EndLive();

    // -- summary --
    const std::wstring out = rec.last_output_path();
    const bool have_file = !out.empty() && GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (have_file) {
        char dims[32] = "";
        if (rec.output_width() > 0)
            std::snprintf(dims, sizeof(dims), "    %dx%d", rec.output_width(), rec.output_height());
        term.Line("  file    " + WideToUtf8(out));
        term.Line("  length  " + WideToUtf8(rec.last_duration_formatted()) + "    size " +
                  FormatSize(rec.last_output_size_mb()) + dims);
    } else {
        term.Line(Paint(fancy, "93", "No output file was found - see logs\\homrec.log"));
    }
    if (crashed) {
        term.Line(Paint(fancy, "93", "The encoder stopped unexpectedly; what was recorded up to then was saved."));
        return 1;
    }
    return have_file ? 0 : 1;
}

#endif // _WIN32

// Records until Ctrl+C. Config: the bound one (see 'config path'), or the .hrc given as the argument -
// the argument is used for this run only and does not change the binding.
int CmdStart(const Args &args) {
#ifndef _WIN32
    (void)args;
    return Fail("recording is only available on Windows");
#else
    if (args.size() > 1)
        return Fail("start takes at most one config path (use quotes if it contains spaces)");

    // Everything that depends on the directory homrec was started from is resolved first; further
    // down the working directory is switched to the exe folder.
    const bool explicit_cfg = !args.empty();
    const Binding bound = ReadBinding();
    fs::path cfg = bound.path;
    if (explicit_cfg) {
        std::error_code ec;
        cfg = fs::absolute(fs::u8path(args[0]), ec);
        if (ec || args[0].empty()) return Fail("'" + args[0] + "' is not a valid path");
        cfg = cfg.lexically_normal();
    }
    {
        std::error_code ec;
        const std::string shown = cfg.u8string();
        if (!fs::exists(cfg, ec)) {
            if (explicit_cfg) return Fail("'" + shown + "' not found");
            return Fail("config '" + shown + "' not found - start HomRec once to create it, "
                        "or run 'homrec start <config-path>'");
        }
        if (fs::is_directory(cfg, ec)) return Fail("'" + shown + "' is a folder - give the path of an .hrc file");
        if (!HasHrcExtension(cfg))     return Fail("'" + shown + "' is not an .hrc file");
    }

    CliSession session;
    {
        std::string err;
        if (!session.Open(&err)) return Fail(err);
    }

    g_stop_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_done_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_ev || !g_done_ev) return Fail("cannot create the stop event");
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    struct DoneOnExit { ~DoneOnExit() { if (g_done_ev) SetEvent(g_done_ev); } } done_on_exit;

    // hr.exe resolves ffmpeg\, recordings\, Assets\ ... relative to the current directory, i.e. it
    // only works when started from its own folder. homrec may be started from anywhere.
    SetCurrentDirectoryW(ExeDir().c_str());
    EnableDpiAwareness();

    // Same loading as hr.exe at startup: homrec.hrc, plus the custom settings file it may point to
    // (Settings > Advanced). Any other config is read as it is.
    AppState state;
    bool loaded = HrcConfig::Load(state, cfg.wstring());
    if (loaded && !explicit_cfg && bound.is_default) {
        const std::wstring custom = HrcConfig::ResolveSettingsPath(state);
        if (custom != HrcConfig::kDefaultSettingsPath) HrcConfig::Load(state, custom);
    }
    if (!loaded) return Fail("cannot read '" + cfg.u8string() + "'");

    HrWgcLog::SetEnabled(state.wgc_logging_enabled);
    state.disable_preview = true;   // there is no window to show the live preview in; never saved back

    Term term;
    std::error_code ec;
    const fs::path out_dir = fs::absolute(fs::u8path(state.output_folder.empty() ? "recordings" : state.output_folder), ec);
    term.Line("");
    term.Line(Paint(term.fancy(), "1", std::string("HomRec ") + HR_APP_VERSION));
    term.Line("  config  " + cfg.u8string());
    term.Line("  output  " + (ec ? state.output_folder : out_dir.lexically_normal().u8string()));
    term.Line("");

    const int rc = Record(state, term, session);
    hr_pl_wait_all_detached(5000);
    return rc;
#endif
}

// Toggles pause in the recording started by 'homrec start' (run it from another terminal).
int CmdPause(const Args &args) {
#ifndef _WIN32
    (void)args;
    return Fail("recording is only available on Windows");
#else
    if (!args.empty()) return Fail("pause takes no arguments");

    HANDLE ev  = OpenEventW(EVENT_MODIFY_STATE, FALSE, kPauseName);
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, kStateName);
    const volatile LONG *state = map ? static_cast<const volatile LONG *>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(LONG)))
                                     : nullptr;
    int rc = 0;
    if (!ev || !state) {
        rc = Fail("no recording in progress (start one with 'homrec start')");
    } else {
        const LONG before = *state;
        if (before == kStateSaving) {
            rc = Fail("the recording is being saved");
        } else if (before != kStateRecording && before != kStatePaused) {
            rc = Fail("the recording has not started yet");
        } else {
            SetEvent(ev);
            LONG now = before;
            for (int i = 0; i < 250 && now == before; ++i) { Sleep(20); now = *state; }   // up to ~5 s
            if (now == kStatePaused)         std::printf("Recording paused (run 'homrec pause' again to resume)\n");
            else if (now == kStateRecording) std::printf("Recording resumed\n");
            else if (now == kStateSaving)    rc = Fail("the recording is being saved");
            else rc = Fail("the recording did not react (pause is switched off for full-GPU capture: ddagrab_pause)");
        }
    }
    if (state) UnmapViewOfFile((LPCVOID)state);
    if (map) CloseHandle(map);
    if (ev)  CloseHandle(ev);
    return rc;
#endif
}

// ---------------------------------------------------------------------------
// Command table - the only place that lists commands
// ---------------------------------------------------------------------------

std::vector<Command> BuildCommands() {
    return {
        Leaf("--version", "", "Show the HomRec version", CmdVersion, {"-v", "-V"}),
        Leaf("start", "[config-path]", "Record the screen; Ctrl+C stops and saves", CmdStart),
        Leaf("pause", "", "Pause / resume the running recording", CmdPause),
        Group("config", {
            Leaf("path",   "",       "Show the .hrc config homrec is bound to", CmdConfigPath),
            Leaf("change", "<path>", "Bind homrec to another .hrc config",      CmdConfigChange),
        }),
    };
}

int Run(const Args &args) {
    const std::string banner = std::string("homrec ") + HR_APP_VERSION + " -- HomRec command-line interface";
    return Dispatch(banner, "homrec", BuildCommands(), args);
}

} // namespace

#ifdef _WIN32

// Wide entry point (build with -municode) so non-ASCII paths in arguments survive intact.
static std::string ToUtf8(const wchar_t *w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return std::string();
    std::string s((size_t)len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    return s;
}

int wmain(int argc, wchar_t **argv) {
    ConsoleUtf8 console;
    Args args;
    for (int i = 1; i < argc; ++i) args.push_back(ToUtf8(argv[i]));
    return Run(args);
}

#else

int main(int argc, char **argv) {   // non-Windows: lets the logic be built and tested anywhere
    Args args(argv + 1, argv + argc);
    return Run(args);
}

#endif
