// homrec.cpp - "homrec", the HomRec command-line interface.
//
// Standalone tool like hom.exe: it does NOT link wxWidgets or Lua and is built separately
// (`cmake --build build --target homrec`). Keep homrec.exe next to hr.exe and put that folder in PATH, then
// `homrec ...` works from any terminal location - everything it needs is looked up
// relative to its own exe, never the current directory (see homrec_binding.h).
//
// Commands:
//   homrec                           interactive menu (start / overlays / setup) in a console
//   homrec --version                 show the HomRec version
//   homrec setup                     first-run setup again: config, recordings folder, format, fps
//   homrec config path               show the .hrc config homrec is bound to
//   homrec config change <path>      bind homrec to another .hrc config
//   homrec start [config-path]       record the screen (bound config, or the given .hrc); Ctrl+C stops + saves
//   homrec pause                     pause / resume the recording started by 'homrec start'
//   homrec overlay list|add|set|show|hide|remove|edit   overlays of the active scene
//
// 'start' uses the same RecordingController as hr.exe (no wxWidgets involved), so the engine objects
// are linked into homrec.exe - see the homrec target in CMakeLists.txt.
//
// The text pieces (colours, status line, menus, ru/en strings) are in homrec_tui.h and the overlay
// editing rules in homrec_overlay.h; both are plain C++ and covered by scripts/tests/test_homrec_cli.cpp.
// All logs of this program go to logs\cli\ (see HrLogPaths::SetLogsSubdir).
//
// To add a command: write a `int CmdXxx(const Args &)` below and add one Leaf()/Group() line
// to BuildCommands(). Help and error messages come from that table automatically.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "homrec_binding.h"
#include "homrec_cli.h"
#include "homrec_overlay.h"
#include "homrec_tui.h"
#include "../../src/ui/version.h"   // HR_APP_VERSION - single source of truth for the version

#ifdef _WIN32
  #include "../../src/core/input_overlay.h"
  #include "../../src/core/webcam_enum.h"
  #include "../../src/ui/app_state.h"
  #include "../../src/ui/hrc_config.h"
  #include "../../src/ui/recording_controller.h"
  #include "../../src/utils/log_paths.h"
  #include "../../src/utils/wgc_log.h"

  // pipeline.cpp - waits for capture pipelines that were handed off to a background thread; hr.exe
  // calls it from OnExit() (win_main.cpp) for the same reason.
  extern "C" int hr_pl_wait_all_detached(int timeout_ms);
#endif

using namespace hrcli;

namespace {

namespace ovl = hrcli::overlay;
using tui::Lang;
using tui::Msg;
using tui::Paint;

// Language of the interactive texts (wizard, menus, status line). Set once in Run().
Lang g_lang = Lang::En;
[[maybe_unused]] const char *T(Msg m) { return tui::Tr(g_lang, m); }

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

// A path pasted into the console often comes wrapped in quotes.
std::string StripQuotes(std::string s) {
    s = TrimStr(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = TrimStr(s.substr(1, s.size() - 2));
    return s;
}

// The folder homrec was started from. Commands that load the scenes switch the working directory to the
// exe folder, so a relative path typed later (in the editors) is still taken relative to this one.
fs::path g_start_cwd;

[[maybe_unused]] std::string AbsPath(const std::string &p) {
    fs::path path = fs::u8path(StripQuotes(p));
    std::error_code ec;
    if (path.is_relative() && !g_start_cwd.empty()) path = g_start_cwd / path;
    else path = fs::absolute(path, ec);
    return ec ? StripQuotes(p) : path.lexically_normal().u8string();
}

// ---------------------------------------------------------------------------
// CLI log: logs\cli\cli.log (the engine's own logs - homrec.log, ffmpeg_*.log, ... - land in the
// same folder because Run() switches HrLogPaths to the "cli" subfolder).
// ---------------------------------------------------------------------------

void CliLog(const char *level, const std::string &msg) {
#ifdef _WIN32
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    const std::wstring path = HrLogPaths::LogFilePath(L"cli.log");
    HrLogPaths::CapFileSize(path, 2 * 1024 * 1024);
    std::ofstream f(path.c_str(), std::ios::app | std::ios::binary);
    if (!f) return;
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &lt);
    f << "[" << ts << "] [" << level << "] " << msg << "\n";
#else
    (void)level; (void)msg;
#endif
}

// ---------------------------------------------------------------------------
// Commands that need no engine
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
    CliLog("INFO", "config bound: " + shown);
    std::printf("Config changed to %s\n", shown.c_str());
    return 0;
}

#ifdef _WIN32

// ---------------------------------------------------------------------------
// Windows: the console
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

// Language of the interactive texts: 'lang=' in cfg\homrec_cli.cfg, else the language of the bound
// config, else the Windows UI language.
Lang DetectLang() {
    const std::string forced = ReadSettingIn(ExeDir(), kLangKey);
    if (!forced.empty()) return tui::LangFromCode(forced);

    std::ifstream f(ReadBinding().path);
    std::string line;
    for (int n = 0; n < 400 && std::getline(f, line); ++n) {
        line = TrimStr(line);
        if (line.compare(0, 9, "language=") == 0) return tui::LangFromCode(TrimStr(line.substr(9)));
    }
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN ? Lang::Ru : Lang::En;
}

// Keys for the menus.
enum class Key { Esc, Up, Down, Enter, Char };
struct KeyEvent { Key key = Key::Esc; wchar_t ch = 0; };

// The console input without line editing and echo, for as long as the object lives. Waiting for a key
// blocks in ReadConsoleInputW, so a menu costs no CPU while it sits there.
class RawInput {
public:
    explicit RawInput(HANDLE in) : in_(in) {
        if (GetConsoleMode(in_, &old_)) {
            ok_ = true;
            SetConsoleMode(in_, old_ & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT));
            FlushConsoleInputBuffer(in_);   // a key pressed before the menu appeared must not answer it
        }
    }
    ~RawInput() { if (ok_) SetConsoleMode(in_, old_); }
    RawInput(const RawInput &) = delete;
    RawInput &operator=(const RawInput &) = delete;

    KeyEvent Read() {
        for (;;) {
            INPUT_RECORD rec;
            DWORD n = 0;
            if (!ReadConsoleInputW(in_, &rec, 1, &n) || n == 0) return KeyEvent{Key::Esc, 0};   // input closed
            if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
            const KEY_EVENT_RECORD &k = rec.Event.KeyEvent;
            switch (k.wVirtualKeyCode) {
            case VK_UP:     return KeyEvent{Key::Up, 0};
            case VK_DOWN:   return KeyEvent{Key::Down, 0};
            case VK_RETURN: return KeyEvent{Key::Enter, 0};
            case VK_ESCAPE: return KeyEvent{Key::Esc, 0};
            default: break;
            }
            const wchar_t ch = k.uChar.UnicodeChar;
            if (ch == 3) return KeyEvent{Key::Esc, 0};   // Ctrl+C
            if (ch >= 32) return KeyEvent{Key::Char, ch};
        }
    }

private:
    HANDLE in_;
    DWORD  old_ = 0;
    bool   ok_ = false;
};

struct MenuResult {
    int     index = -1;   // the row Enter (or a hot key) was pressed on; -1 = Esc
    wchar_t key = 0;      // the upper-cased hot key, 0 for Enter
};

// The terminal. Interactive console: ANSI colours/blocks when the console accepts them (Windows 10+),
// one in-place status line, menus. Redirected output: plain lines only, no animation, no questions.
class Term {
public:
    Term() : out_(GetStdHandle(STD_OUTPUT_HANDLE)), in_(GetStdHandle(STD_INPUT_HANDLE)) {
        DWORD mode = 0;
        if (out_ && out_ != INVALID_HANDLE_VALUE && GetConsoleMode(out_, &mode)) {
            interactive_ = true;
            old_mode_ = mode;
            fancy_ = SetConsoleMode(out_, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
        }
        DWORD in_mode = 0;
        in_console_ = in_ && in_ != INVALID_HANDLE_VALUE && GetConsoleMode(in_, &in_mode);
    }
    ~Term() {
        if (cursor_hidden_) Write("\x1b[?25h");
        if (interactive_ && fancy_) SetConsoleMode(out_, old_mode_);   // leave the console as we found it
    }
    Term(const Term &) = delete;
    Term &operator=(const Term &) = delete;

    bool interactive() const { return interactive_; }
    bool fancy() const { return fancy_; }
    // Questions need a console on both sides: nobody can answer when stdin or stdout is redirected.
    bool can_ask() const { return interactive_ && in_console_; }

    // Usable line width: the window width, kept between 40 and 110 so lines never wrap and the
    // menus that move the cursor up stay in step.
    size_t Cols() const {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (interactive_ && GetConsoleScreenBufferInfo(out_, &csbi)) {
            const int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            if (w > 0) return (size_t)std::min(std::max(w - 1, 40), 110);
        }
        return 79;
    }

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

    // Wipes the screen (the full-screen editors redraw from the top).
    void ClearScreen() { if (fancy_) Write("\x1b[2J\x1b[H"); else Write("\r\n"); }

    // Asks for one line of text (the console edits it: arrows, paste and Cyrillic all work). False when
    // there is nobody to ask. The answer is trimmed and may be empty.
    bool ReadLine(const std::string &prompt, std::string *out) {
        out->clear();
        if (!can_ask()) return false;
        Clear();
        Write(prompt);
        DWORD mode = 0;
        GetConsoleMode(in_, &mode);
        SetConsoleMode(in_, mode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
        std::wstring buf(1024, L'\0');
        DWORD got = 0;
        const BOOL ok = ReadConsoleW(in_, &buf[0], (DWORD)buf.size(), &got, nullptr);
        SetConsoleMode(in_, mode);
        if (!ok || got == 0) return false;
        buf.resize(got);
        while (!buf.empty() && (buf.back() == L'\r' || buf.back() == L'\n')) buf.pop_back();
        *out = StripQuotes(WideToUtf8(buf));
        return true;
    }

    // A list the user picks from with Up/Down + Enter (digits 1-9 pick directly, Esc cancels).
    // `row(i, selected)` returns the text of row i - it is called again for the old and the new row
    // whenever the selection moves. `hot` lists extra letters that also end the menu (the editors use
    // A/D/V...); they are returned with the row that was highlighted. The menu removes itself from the
    // screen when it ends. Cost: nothing while waiting - it blocks on the console input.
    MenuResult Menu(size_t count, const std::function<std::string(size_t, bool)> &row, size_t sel,
                    const std::wstring &hot = std::wstring()) {
        MenuResult res;
        if (!can_ask() || count == 0) return res;
        if (sel >= count) sel = 0;
        if (!fancy_) return MenuPlain(count, row, sel);

        RawInput raw(in_);
        Clear();
        Write("\x1b[?25l");
        cursor_hidden_ = true;

        auto draw = [&](bool first) {
            std::string f;
            if (!first) f += "\x1b[" + std::to_string(count) + "A";
            for (size_t i = 0; i < count; ++i) f += "\r\x1b[2K" + row(i, i == sel) + "\r\n";
            Write(f);
        };
        draw(true);

        for (;;) {
            const KeyEvent k = raw.Read();
            if (k.key == Key::Up)        { sel = sel == 0 ? count - 1 : sel - 1; draw(false); }
            else if (k.key == Key::Down) { sel = (sel + 1) % count; draw(false); }
            else if (k.key == Key::Enter) { res.index = (int)sel; break; }
            else if (k.key == Key::Esc)   { break; }
            else {
                const wchar_t up = (wchar_t)std::towupper(k.ch);
                if (hot.find(up) != std::wstring::npos) { res.index = (int)sel; res.key = up; break; }
                if (k.ch >= L'1' && k.ch <= L'9' && (size_t)(k.ch - L'1') < count) { res.index = (int)(k.ch - L'1'); break; }
            }
        }

        Write("\x1b[" + std::to_string(count) + "A\x1b[0J\x1b[?25h");   // take the menu off the screen, cursor back
        cursor_hidden_ = false;
        return res;
    }

    // Plain console without ANSI: a numbered list and a typed number.
    MenuResult MenuPlain(size_t count, const std::function<std::string(size_t, bool)> &row, size_t sel) {
        MenuResult res;
        for (size_t i = 0; i < count; ++i) Line("  " + std::to_string(i + 1) + ") " + row(i, false));
        for (;;) {
            std::string typed;
            if (!ReadLine("> [" + std::to_string(sel + 1) + "] ", &typed)) return res;
            if (typed.empty()) { res.index = (int)sel; return res; }
            int n = 0;
            if (ovl::ParseInt(typed, 1, (int)count, &n)) { res.index = n - 1; return res; }
            if (typed == "q" || typed == "Q") return res;
        }
    }

    // A yes/no question; `yes_first` puts Yes under the cursor. Esc counts as No.
    bool Confirm(const std::string &question, bool yes_first) {
        Line(Paint(fancy_, tui::kBold, question));
        const MenuResult r = Menu(2, [&](size_t i, bool s) {
            return tui::MenuItemLine(i == 0 ? T(Msg::Yes) : T(Msg::No), s, fancy_);
        }, yes_first ? 0 : 1);
        return r.index == 0;
    }

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

private:
    HANDLE out_;
    HANDLE in_;
    DWORD  old_mode_ = 0;
    bool   interactive_ = false;
    bool   in_console_ = false;
    bool   fancy_ = false;
    bool   live_ = false;
    bool   cursor_hidden_ = false;
    size_t live_len_ = 0;
};

// "HomRec 3.1" + a rule: the top of every screen.
void Header(Term &term, const std::string &title) {
    const bool fancy = term.fancy();
    term.Line("");
    term.Line("  " + Paint(fancy, tui::kBold, title));
    term.Line("  " + tui::Rule(std::min<size_t>(term.Cols() - 2, 56), fancy));
}

void Ok(Term &term, const std::string &s)   { term.Line("  " + Paint(term.fancy(), tui::kGreen, term.fancy() ? "\xE2\x9C\x94 " : "+ ") + s); }
void Bad(Term &term, const std::string &s)  { term.Line("  " + Paint(term.fancy(), tui::kRed, "! ") + s); }
void Note(Term &term, const std::string &s) { term.Line("  " + Paint(term.fancy(), tui::kDim, s)); }

// ---------------------------------------------------------------------------
// Windows: config and overlay state
// ---------------------------------------------------------------------------

// Loads what the HomRec window loads at startup: the bound .hrc, then the overlays of the active scene.
// Scenes and the old homrec_overlays.hrc are looked up relative to the folder of homrec.exe, so the
// working directory is switched there - resolve every path the user typed BEFORE calling this.
bool LoadOverlayState(AppState &state, std::string *err) {
    SetCurrentDirectoryW(ExeDir().c_str());
    const Binding b = ReadBinding();
    std::error_code ec;
    if (fs::exists(b.path, ec) && !HrcConfig::Load(state, b.path.wstring())) {
        *err = "cannot read '" + b.path.u8string() + "'";
        return false;
    }
    HrcConfig::LoadScenes(state);
    return true;
}

bool SaveOverlayState(const AppState &state, std::string *err) {
    if (HrcConfig::SaveActiveOverlays(state)) return true;
    *err = T(Msg::OverlaySaveFailed);
    return false;
}

std::string SceneName(const AppState &state) {
    for (const SceneInfo &s : state.scenes) if (s.id == state.active_scene_id) return s.name;
    return state.active_scene_id;
}

// ---------------------------------------------------------------------------
// Windows: first-run setup
// ---------------------------------------------------------------------------

const char *FormatLabel(VideoFormat f) {
    switch (f) {
    case VideoFormat::Mp4:  return "MP4";
    case VideoFormat::Mkv:  return "MKV";
    case VideoFormat::Both: return "MKV + MP4";
    }
    return "MP4";
}

// The folder as the engine will use it: a relative one is relative to the folder of homrec.exe.
fs::path ResolveOutputFolder(const std::string &configured) {
    fs::path p = fs::u8path(configured.empty() ? "recordings" : configured);
    if (p.is_relative()) p = ExeDir() / p;
    return p.lexically_normal();
}

// Step 1: "Use this config?". False = the person pressed Esc (skip the setup).
bool AskConfig(Term &term, fs::path *cfg) {
    const bool fancy = term.fancy();
    for (;;) {
        term.Line("  " + Paint(fancy, tui::kBold, T(Msg::UseThisConfig)));
        term.Line("  " + Paint(fancy, tui::kDim, cfg->u8string()));
        const MenuResult r = term.Menu(2, [&](size_t i, bool s) {
            return tui::MenuItemLine(i == 0 ? T(Msg::Yes) : T(Msg::EnterPath), s, fancy);
        }, 0);
        if (r.index < 0) return false;
        if (r.index == 0) { Ok(term, cfg->u8string()); return true; }

        std::string typed;
        if (!term.ReadLine("  " + std::string(T(Msg::AskConfigPath)), &typed) || typed.empty()) continue;
        std::error_code ec;
        const fs::path p = fs::u8path(AbsPath(typed));
        if (typed.empty() || p.empty()) { Bad(term, "'" + typed + "' is not a valid path"); continue; }
        if (fs::is_directory(p, ec)) { Bad(term, "'" + p.u8string() + "' is a folder - give the path of an .hrc file"); continue; }
        if (!HasHrcExtension(p))     { Bad(term, "'" + p.u8string() + "' is not an .hrc file"); continue; }
        if (!fs::exists(p, ec)) {
            if (!term.Confirm(std::string("  ") + T(Msg::CreateConfigQ), true)) continue;
            fs::create_directories(p.parent_path(), ec);
            if (ec) { Bad(term, "cannot create '" + p.parent_path().u8string() + "': " + ec.message()); continue; }
        }
        *cfg = p;
        Ok(term, p.u8string());
        return true;
    }
}

// Step 2: where the recordings go. False = Esc.
bool AskFolder(Term &term, AppState &state) {
    const bool fancy = term.fancy();
    for (;;) {
        const std::string current = ResolveOutputFolder(state.output_folder).u8string();
        term.Line("  " + Paint(fancy, tui::kBold, T(Msg::OutputFolder)));
        const MenuResult r = term.Menu(2, [&](size_t i, bool s) {
            const std::string text = i == 0 ? std::string(T(Msg::KeepCurrent)) + ": " + tui::FitTail(current, term.Cols() - 20, fancy)
                                            : std::string(T(Msg::ChooseOther));
            return tui::MenuItemLine(text, s, fancy);
        }, 0);
        if (r.index < 0) return false;
        if (r.index == 0) { Ok(term, current); return true; }

        std::string typed;
        if (!term.ReadLine("  " + std::string(T(Msg::AskFolder)), &typed) || typed.empty()) continue;
        std::error_code ec;
        const fs::path p = fs::u8path(AbsPath(typed));
        fs::create_directories(p, ec);
        if (ec || !fs::is_directory(p, ec)) { Bad(term, std::string(T(Msg::FolderCreateFailed)) + ": " + p.u8string()); continue; }
        state.output_folder = p.u8string();
        Ok(term, p.u8string());
        return true;
    }
}

// Steps 3 and 4: a short list of values. False = Esc.
bool AskChoice(Term &term, const std::string &title, const std::vector<std::string> &labels, size_t initial, size_t *chosen) {
    const bool fancy = term.fancy();
    term.Line("  " + Paint(fancy, tui::kBold, title));
    const MenuResult r = term.Menu(labels.size(), [&](size_t i, bool s) { return tui::MenuItemLine(labels[i], s, fancy); }, initial);
    if (r.index < 0) return false;
    *chosen = (size_t)r.index;
    Ok(term, labels[*chosen]);
    return true;
}

// The whole first-run setup. Returns true when it ran to the end (settings saved).
bool RunSetup(Term &term) {
    const bool fancy = term.fancy();
    Header(term, T(Msg::SetupTitle));

    const Binding bound = ReadBinding();
    fs::path cfg = bound.path;
    if (!AskConfig(term, &cfg)) return false;

    AppState state;
    std::error_code ec;
    if (fs::exists(cfg, ec) && !HrcConfig::Load(state, cfg.wstring())) {
        Bad(term, "cannot read '" + cfg.u8string() + "'");
        return false;
    }

    term.Line("");
    if (!AskFolder(term, state)) return false;

    term.Line("");
    {
        const std::vector<std::string> labels = {FormatLabel(VideoFormat::Mp4), FormatLabel(VideoFormat::Mkv), FormatLabel(VideoFormat::Both)};
        size_t pick = 0;
        const size_t initial = state.video_format == VideoFormat::Mkv ? 1 : state.video_format == VideoFormat::Both ? 2 : 0;
        if (!AskChoice(term, T(Msg::VideoFormat), labels, initial, &pick)) return false;
        state.video_format = pick == 1 ? VideoFormat::Mkv : pick == 2 ? VideoFormat::Both : VideoFormat::Mp4;
    }

    term.Line("");
    {
        static const int kFps[] = {15, 30, 60};
        std::vector<std::string> labels;
        std::vector<int> values;
        for (int f : kFps) { labels.push_back(std::to_string(f)); values.push_back(f); }
        size_t initial = 0;
        bool known = false;
        for (size_t i = 0; i < values.size(); ++i) if (values[i] == state.target_fps) { initial = i; known = true; }
        if (!known) {   // a value set by hand in the config stays one of the choices
            labels.push_back(std::string(T(Msg::KeepCurrent)) + ": " + std::to_string(state.target_fps));
            values.push_back(state.target_fps);
            initial = values.size() - 1;
        }
        size_t pick = 0;
        if (!AskChoice(term, T(Msg::Fps), labels, initial, &pick)) return false;
        state.target_fps = values[pick];
    }

    // Save: the config itself, then which config homrec is bound to.
    std::string err;
    if (!HrcConfig::Save(state, cfg.wstring())) {
        Bad(term, std::string(T(Msg::SetupSaveFailed)) + ": " + cfg.u8string());
        CliLog("ERROR", "setup: cannot write " + cfg.u8string());
        return false;
    }
    if (cfg != bound.path) {
        if (!WriteBinding(cfg, &err)) { Bad(term, err); CliLog("ERROR", "setup: " + err); return false; }
    }
    term.Line("");
    Ok(term, Paint(fancy, tui::kBold, T(Msg::SetupSaved)));
    CliLog("INFO", "setup: config=" + cfg.u8string() + " output=" + ResolveOutputFolder(state.output_folder).u8string() +
                   " format=" + FormatLabel(state.video_format) + " fps=" + std::to_string(state.target_fps));
    return true;
}

// Runs the setup and remembers that it was offered, so it does not come back every time.
void SetupAndRemember(Term &term) {
    const bool done = RunSetup(term);
    if (!done) {
        term.Line("");
        Note(term, T(Msg::SetupSkipped));
        CliLog("INFO", "setup: skipped");
    }
    std::string err;
    if (!MarkSetupDone(&err)) CliLog("WARN", "setup: " + err);
    term.Line("");
}

int CmdSetup(const Args &args) {
    if (!args.empty()) return Fail("setup takes no arguments");
    Term term;
    if (!term.can_ask()) return Fail("setup asks questions - run it in a console window");
    SetupAndRemember(term);
    return 0;
}

// ---------------------------------------------------------------------------
// Windows: overlays
// ---------------------------------------------------------------------------

// "key=value" -> key, value. False when there is no '='.
bool SplitKeyValue(const std::string &arg, std::string *key, std::string *value) {
    const size_t eq = arg.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    *key = TrimStr(arg.substr(0, eq));
    *value = arg.substr(eq + 1);
    return true;
}

bool IsPathKey(const std::string &key) {
    const std::string k = ovl::Lower(key);
    return k == "path" || k == "json" || k == "png";
}

// Applies key=value arguments args[from..] to `ov`. The overlay changes only when every pair is valid.
bool ApplyPairs(OverlayDef &ov, const Args &args, size_t from, std::string *err) {
    OverlayDef work = ov;
    for (size_t i = from; i < args.size(); ++i) {
        std::string key, value;
        if (!SplitKeyValue(args[i], &key, &value)) {
            *err = "expected key=value, got '" + args[i] + "' (use quotes around values with spaces)";
            return false;
        }
        if (IsPathKey(key)) value = AbsPath(value);
        if (!ovl::SetProp(work, key, value, err)) return false;
    }
    ov = std::move(work);
    return true;
}

// The row number / id / name of the command line -> index into the list, or an error message.
bool ResolveTarget(const AppState &state, const std::string &token, size_t *index, std::string *err) {
    const int i = ovl::Find(state.overlays, token);
    if (i == ovl::kAmbiguous) { *err = "several overlays are named '" + token + "' - use the row number or the id"; return false; }
    if (i < 0) { *err = "no overlay '" + token + "' (see 'homrec overlay list')"; return false; }
    *index = (size_t)i;
    return true;
}

std::string Describe(const OverlayDef &ov) {
    return ov.id + " (" + ov.type + (ovl::Summary(ov).empty() ? "" : ", " + ovl::Summary(ov)) + ")";
}

int CmdOverlayList(const Args &args) {
    if (!args.empty()) return Fail("overlay list takes no arguments");
    AppState state;
    std::string err;
    if (!LoadOverlayState(state, &err)) return Fail(err);

    Term term;
    const bool fancy = term.fancy();
    term.Line("  " + Paint(fancy, tui::kBold, "Overlays") + Paint(fancy, tui::kDim, "  scene: " + SceneName(state)));
    if (state.overlays.empty()) {
        term.Line("  " + Paint(fancy, tui::kDim, "none - add one with: homrec overlay add text \"Hello\""));
        return 0;
    }
    const size_t cols = term.Cols();
    for (size_t i = 0; i < state.overlays.size(); ++i)
        term.Line(ovl::Row(i + 1, state.overlays[i], false, cols, fancy));
    term.Line("  " + Paint(fancy, tui::kDim, "row 1 is drawn in front"));
    return 0;
}

int CmdOverlayAdd(const Args &args) {
    if (args.empty()) return Fail("overlay add needs a type: text | image | gif | webcam | input");

    // Everything the user typed is turned into absolute paths first (LoadOverlayState changes the cwd).
    const std::string kind = ovl::Lower(args[0]);
    OverlayDef ov;
    size_t pairs_from = 1;
    std::string err;

    if (kind == "text") {
        if (args.size() < 2) return Fail("overlay add text needs the text (use quotes)");
        ov = ovl::MakeDefault("text");
        if (!ovl::SetProp(ov, "text", args[1], &err)) return Fail(err);
        pairs_from = 2;
    } else if (kind == "image" || kind == "gif") {
        if (args.size() < 2) return Fail("overlay add " + kind + " needs the path of the file");
        ov = ovl::MakeDefault(kind);
        if (!ovl::SetProp(ov, "path", AbsPath(args[1]), &err)) return Fail(err);
        pairs_from = 2;
    } else if (kind == "webcam") {
        const std::vector<HrWebcamDevice> cams = HrEnumerateWebcams();
        if (cams.empty()) return Fail(T(Msg::NoWebcam));
        size_t pick = 0;
        if (args.size() >= 2 && args[1].find('=') == std::string::npos) {
            int n = 0;
            if (!ovl::ParseInt(args[1], 0, 63, &n)) return Fail("the camera must be an index (0 = the first camera)");
            bool found = false;
            for (size_t i = 0; i < cams.size(); ++i) if (cams[i].index == n) { pick = i; found = true; }
            if (!found) return Fail("no camera with index " + args[1]);
            pairs_from = 2;
        }
        ov = ovl::MakeDefault("webcam");
        ov.webcam_index = cams[pick].index;
        ov.webcam_name = cams[pick].name;
    } else if (kind == "input" || kind == "input_overlay") {
        if (args.size() < 3) return Fail("overlay add input needs the layout .json and the spritesheet .png");
        ov = ovl::MakeDefault("input_overlay");
        if (!ovl::SetProp(ov, "json", AbsPath(args[1]), &err)) return Fail(err);
        if (!ovl::SetProp(ov, "png", AbsPath(args[2]), &err)) return Fail(err);
        HrInputOverlayLayout layout;
        if (!layout.Load(ov.input_json_path)) return Fail("that .json file could not be read as an input-overlay layout");
        if (layout.width > 0)  ov.w = layout.width;
        if (layout.height > 0) ov.h = layout.height;
        pairs_from = 3;
    } else {
        return Fail("unknown overlay type '" + args[0] + "' - use text, image, gif, webcam or input");
    }
    if (!ApplyPairs(ov, args, pairs_from, &err)) return Fail(err);

    AppState state;
    if (!LoadOverlayState(state, &err)) return Fail(err);
    ovl::AddOverlay(state.overlays, std::move(ov));
    if (!SaveOverlayState(state, &err)) return Fail(err);

    CliLog("INFO", "overlay added: " + Describe(state.overlays.front()));
    std::printf("Added %s (row 1, in front). Overlays are saved in scene '%s'.\n",
                Describe(state.overlays.front()).c_str(), SceneName(state).c_str());
    return 0;
}

int CmdOverlaySet(const Args &args) {
    if (args.size() < 2) return Fail("overlay set <row|id|name> key=value [key=value ...]");

    // Paths are made absolute first: loading the scenes moves the working directory.
    Args pairs;
    for (size_t i = 1; i < args.size(); ++i) {
        std::string key, value;
        if (!SplitKeyValue(args[i], &key, &value))
            return Fail("expected key=value, got '" + args[i] + "' (use quotes around values with spaces)");
        pairs.push_back(IsPathKey(key) ? key + "=" + AbsPath(value) : args[i]);
    }

    AppState state;
    std::string err;
    if (!LoadOverlayState(state, &err)) return Fail(err);

    size_t idx = 0;
    if (!ResolveTarget(state, args[0], &idx, &err)) return Fail(err);
    if (!ApplyPairs(state.overlays[idx], pairs, 0, &err)) return Fail(err);
    if (!SaveOverlayState(state, &err)) return Fail(err);

    CliLog("INFO", "overlay changed: " + Describe(state.overlays[idx]));
    std::printf("Updated %s\n", Describe(state.overlays[idx]).c_str());
    return 0;
}

int SetVisible(const Args &args, bool visible) {
    const char *verb = visible ? "show" : "hide";
    if (args.size() != 1) return Fail(std::string("overlay ") + verb + " <row|id|name>");
    AppState state;
    std::string err;
    if (!LoadOverlayState(state, &err)) return Fail(err);
    size_t idx = 0;
    if (!ResolveTarget(state, args[0], &idx, &err)) return Fail(err);
    state.overlays[idx].visible = visible;
    if (!SaveOverlayState(state, &err)) return Fail(err);
    CliLog("INFO", std::string("overlay ") + verb + ": " + Describe(state.overlays[idx]));
    std::printf("%s %s\n", visible ? "Showing" : "Hidden:", Describe(state.overlays[idx]).c_str());
    return 0;
}
int CmdOverlayShow(const Args &args) { return SetVisible(args, true); }
int CmdOverlayHide(const Args &args) { return SetVisible(args, false); }

int CmdOverlayRemove(const Args &args) {
    if (args.size() != 1) return Fail("overlay remove <row|id|name>");
    AppState state;
    std::string err;
    if (!LoadOverlayState(state, &err)) return Fail(err);
    size_t idx = 0;
    if (!ResolveTarget(state, args[0], &idx, &err)) return Fail(err);
    const std::string what = Describe(state.overlays[idx]);
    state.overlays.erase(state.overlays.begin() + (ptrdiff_t)idx);
    if (!SaveOverlayState(state, &err)) return Fail(err);
    CliLog("INFO", "overlay removed: " + what);
    std::printf("Removed %s\n", what.c_str());
    return 0;
}

// -- the interactive editor ----------------------------------------------------------------

// Ask for a value and apply it. `status` receives the result line (green) or the error (red).
bool EditProperty(Term &term, OverlayDef &ov, const std::string &key, std::string *status, bool *failed) {
    *failed = false;
    std::string err;
    OverlayDef work = ov;

    if (key == "visible" || key == "locked") {   // yes/no: flip it, no question needed
        const bool now = key == "visible" ? ov.visible : ov.locked;
        ovl::SetProp(work, key, now ? "no" : "yes", &err);
    } else if (key == "webcam") {
        const std::vector<HrWebcamDevice> cams = HrEnumerateWebcams();
        if (cams.empty()) { *status = T(Msg::NoWebcam); *failed = true; return false; }
        const MenuResult r = term.Menu(cams.size(), [&](size_t i, bool s) {
            return tui::MenuItemLine(std::to_string(cams[i].index) + "  " + cams[i].name, s, term.fancy());
        }, 0);
        if (r.index < 0) return false;
        work.webcam_index = cams[(size_t)r.index].index;
        work.webcam_name = cams[(size_t)r.index].name;
    } else {
        std::string typed;
        const std::string prompt = "  " + key + " [" + tui::Fit(ovl::GetProp(ov, key), 40, term.fancy()) + "] > ";
        if (!term.ReadLine(prompt, &typed) || typed.empty()) return false;
        if (IsPathKey(key)) typed = AbsPath(typed);
        if (!ovl::SetProp(work, key, typed, &err)) { *status = err; *failed = true; return false; }
    }
    if (!err.empty()) { *status = err; *failed = true; return false; }
    ov = std::move(work);
    *status = T(Msg::OverlayChanged);
    return true;
}

// The property list of one overlay. Returns true when something was changed.
bool EditOverlay(Term &term, AppState &state, size_t index) {
    bool changed = false;
    std::string status;
    bool failed = false;
    size_t sel = 0;
    for (;;) {
        OverlayDef &ov = state.overlays[index];
        const auto props = ovl::PropsFor(ov.type);
        term.ClearScreen();
        Header(term, T(Msg::OverlayEdit) + std::string(": ") + Describe(ov));
        if (!status.empty()) { if (failed) Bad(term, status); else Ok(term, status); }
        else term.Line("");

        const size_t cols = term.Cols();
        const MenuResult r = term.Menu(props.size() + 1, [&](size_t i, bool s) {
            if (i == props.size()) return tui::MenuItemLine(T(Msg::OverlayBack), s, term.fancy());
            const std::string key = props[i]->key;
            const std::string value = tui::Fit(ovl::GetProp(ov, key), cols > 30 ? cols - 22 : 8, term.fancy());
            return tui::MenuItemLine(tui::PadRight(key, 9) + " " + value, s, term.fancy());
        }, sel);
        if (r.index < 0 || (size_t)r.index == props.size()) return changed;
        sel = (size_t)r.index;
        status.clear();
        if (EditProperty(term, ov, props[sel]->key, &status, &failed) && !failed) {
            std::string err;
            if (!SaveOverlayState(state, &err)) { status = err; failed = true; }
            else changed = true;
        }
    }
}

// Picks a type, asks for what it needs and adds the overlay. Returns the status line to show.
bool AddOverlayInteractive(Term &term, AppState &state, std::string *status) {
    const bool fancy = term.fancy();
    term.ClearScreen();
    Header(term, std::string(T(Msg::OverlayAdd)) + ": " + T(Msg::OverlayType));
    const std::vector<std::string> labels = {T(Msg::OverlayText), T(Msg::OverlayImage), T(Msg::OverlayGif), T(Msg::OverlayWebcam), T(Msg::OverlayInput)};
    const MenuResult r = term.Menu(labels.size(), [&](size_t i, bool s) { return tui::MenuItemLine(labels[i], s, fancy); }, 0);
    if (r.index < 0) return false;

    OverlayDef ov;
    std::string typed, err;
    switch (r.index) {
    case 0:
        if (!term.ReadLine("  " + std::string(T(Msg::AskText)), &typed) || typed.empty()) return false;
        ov = ovl::MakeDefault("text");
        if (!ovl::SetProp(ov, "text", typed, &err)) { *status = err; return false; }
        break;
    case 1:
    case 2:
        if (!term.ReadLine("  " + std::string(r.index == 1 ? T(Msg::AskImage) : T(Msg::AskGif)), &typed) || typed.empty()) return false;
        ov = ovl::MakeDefault(r.index == 1 ? "image" : "gif");
        if (!ovl::SetProp(ov, "path", AbsPath(typed), &err)) { *status = err; return false; }
        break;
    case 3: {
        const std::vector<HrWebcamDevice> cams = HrEnumerateWebcams();
        if (cams.empty()) { *status = T(Msg::NoWebcam); return false; }
        const MenuResult c = term.Menu(cams.size(), [&](size_t i, bool s) {
            return tui::MenuItemLine(std::to_string(cams[i].index) + "  " + cams[i].name, s, fancy);
        }, 0);
        if (c.index < 0) return false;
        ov = ovl::MakeDefault("webcam");
        ov.webcam_index = cams[(size_t)c.index].index;
        ov.webcam_name = cams[(size_t)c.index].name;
        break;
    }
    default: {
        if (!term.ReadLine("  " + std::string(T(Msg::AskJson)), &typed) || typed.empty()) return false;
        ov = ovl::MakeDefault("input_overlay");
        if (!ovl::SetProp(ov, "json", AbsPath(typed), &err)) { *status = err; return false; }
        if (!term.ReadLine("  " + std::string(T(Msg::AskPng)), &typed) || typed.empty()) return false;
        if (!ovl::SetProp(ov, "png", AbsPath(typed), &err)) { *status = err; return false; }
        HrInputOverlayLayout layout;
        if (!layout.Load(ov.input_json_path)) { *status = "that .json file could not be read as an input-overlay layout"; return false; }
        if (layout.width > 0)  ov.w = layout.width;
        if (layout.height > 0) ov.h = layout.height;
        break;
    }
    }
    ovl::AddOverlay(state.overlays, std::move(ov));
    if (!SaveOverlayState(state, &err)) { *status = err; return false; }
    CliLog("INFO", "overlay added: " + Describe(state.overlays.front()));
    *status = T(Msg::OverlayAdded);
    return true;
}

// 'homrec overlay edit' and the "Overlays" entry of the menu: the list of overlays with
// Enter = edit, A = add, D = delete, V = show/hide. Every change is saved at once.
int CmdOverlayEdit(const Args &args) {
    if (!args.empty()) return Fail("overlay edit takes no arguments");
    Term term;
    if (!term.can_ask()) return Fail("the overlay editor needs a console window - use 'homrec overlay list' / 'set' in scripts");

    AppState state;
    std::string err;
    if (!LoadOverlayState(state, &err)) return Fail(err);

    const bool fancy = term.fancy();
    std::string status;
    bool failed = false;
    size_t sel = 0;
    for (;;) {
        const size_t n = state.overlays.size();
        if (sel >= n) sel = n ? n - 1 : 0;
        term.ClearScreen();
        Header(term, std::string(T(Msg::OverlayTitle)) + Paint(fancy, tui::kDim, "   " + SceneName(state)));
        term.Line("  " + Paint(fancy, tui::kDim, T(Msg::OverlayKeys)));
        if (!status.empty()) { if (failed) Bad(term, status); else Ok(term, status); }
        else term.Line("");
        status.clear();
        failed = false;

        const size_t cols = term.Cols();
        const MenuResult r = term.Menu(n ? n : 1, [&](size_t i, bool s) {
            if (n == 0) return "    " + Paint(fancy, tui::kDim, T(Msg::OverlayNone));
            return ovl::Row(i + 1, state.overlays[i], s, cols, fancy);
        }, sel, L"ADVE");
        if (r.index < 0) break;
        sel = (size_t)r.index;

        if (r.key == L'A' || n == 0) {
            if (n == 0 && r.key != 0 && r.key != L'A') continue;   // only 'A' (or Enter on the placeholder) adds
            failed = !AddOverlayInteractive(term, state, &status);
            if (failed && status.empty()) failed = false;          // cancelled - nothing to report
            sel = 0;
        } else if (r.key == L'D') {
            if (term.Confirm("  " + std::string(T(Msg::DeleteQ)) + " " + Describe(state.overlays[sel]), false)) {
                const std::string what = Describe(state.overlays[sel]);
                state.overlays.erase(state.overlays.begin() + (ptrdiff_t)sel);
                if (!SaveOverlayState(state, &err)) { status = err; failed = true; }
                else { status = T(Msg::OverlayRemoved); CliLog("INFO", "overlay removed: " + what); }
            }
        } else if (r.key == L'V') {
            state.overlays[sel].visible = !state.overlays[sel].visible;
            if (!SaveOverlayState(state, &err)) { status = err; failed = true; }
            else status = T(Msg::OverlayChanged);
        } else {   // Enter or E
            EditOverlay(term, state, sel);
        }
    }
    term.ClearScreen();
    return 0;
}

#else   // !_WIN32 -------------------------------------------------------------------------------

int WindowsOnly() { return Fail("this command is only available on Windows"); }
int CmdSetup(const Args &)         { return WindowsOnly(); }
int CmdOverlayList(const Args &)   { return WindowsOnly(); }
int CmdOverlayAdd(const Args &)    { return WindowsOnly(); }
int CmdOverlaySet(const Args &)    { return WindowsOnly(); }
int CmdOverlayShow(const Args &)   { return WindowsOnly(); }
int CmdOverlayHide(const Args &)   { return WindowsOnly(); }
int CmdOverlayRemove(const Args &) { return WindowsOnly(); }
int CmdOverlayEdit(const Args &)   { return WindowsOnly(); }

#endif  // _WIN32

#ifdef _WIN32

// ---------------------------------------------------------------------------
// Windows: 'start' / 'pause'
// ---------------------------------------------------------------------------

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
    if (!rec.ffmpeg_found()) CliLog("WARN", "ffmpeg was not found (only the built-in encoder can be used)");

    // Same 3-2-1 as the main window when 'countdown' is on in the config.
    if (state.countdown_enabled) {
        for (int n = 3; n > 0 && !g_stop.load(); --n) {
            term.Live(std::string(T(Msg::Starting)) + " " + std::to_string(n) + "...");
            WaitForSingleObject(g_stop_ev, 1000);
        }
        if (g_stop.load()) { term.Line(T(Msg::Cancelled)); CliLog("INFO", "start cancelled during the countdown"); return 0; }
    }

    std::wstring err;
    if (!rec.Start(err)) {
        term.Clear();
        CliLog("ERROR", "recording did not start: " + WideToUtf8(err));
        return Fail(WideToUtf8(err));
    }
    session.SetState(kStateRecording);
    CliLog("INFO", "recording started");
    term.Line("  " + Paint(fancy, tui::kDim, "Ctrl+C = stop and save   |   homrec pause = pause / resume"));

    // -- recording: ~2 wake-ups a second (PollStats() wants 250-500 ms), or at once on pause / Ctrl+C.
    //    The line is only written again when its text changed. --
    bool crashed = false;
    std::string last_line;
    const HANDLE waits[2] = {session.pause_event(), g_stop_ev};
    while (!g_stop.load()) {
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 500);
        if (w == WAIT_OBJECT_0) {            // 'homrec pause'
            rec.TogglePause();
            session.SetState(rec.paused() ? kStatePaused : kStateRecording);
            CliLog("INFO", rec.paused() ? "recording paused" : "recording resumed");
        } else if (w == WAIT_OBJECT_0 + 1) { // Ctrl+C
            break;
        }
        rec.PollStats();
        if (rec.crashed()) {                 // ffmpeg / the encoder died - save what there is
            rec.AcknowledgeCrash();
            crashed = true;
            CliLog("ERROR", "the encoder stopped unexpectedly");
            break;
        }
        const std::string line = tui::RecLine(fancy, g_lang, rec.paused(), rec.elapsed_seconds(), rec.output_size_mb(),
                                              rec.current_fps(), rec.overloaded());
        if (line != last_line) { term.Live("  " + line); last_line = line; }
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
        term.Live("  " + tui::SavingLine(fancy, g_lang, tick, sec, g_stop_hits.load() > hits0));
        Sleep(100);
    }
    term.Live("  " + std::string(T(Msg::Saved)) + "   " + tui::DoneBar(fancy));
    term.EndLive();

    // -- summary --
    const std::wstring out = rec.last_output_path();
    const bool have_file = !out.empty() && GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (have_file) {
        char dims[32] = "";
        if (rec.output_width() > 0)
            std::snprintf(dims, sizeof(dims), "    %dx%d", rec.output_width(), rec.output_height());
        term.Line("  " + Paint(fancy, tui::kDim, "file    ") + WideToUtf8(out));
        term.Line("  " + Paint(fancy, tui::kDim, "length  ") + WideToUtf8(rec.last_duration_formatted()) + "    size " +
                  tui::FormatSize(rec.last_output_size_mb()) + dims);
        CliLog("INFO", "saved: " + WideToUtf8(out) + " (" + WideToUtf8(rec.last_duration_formatted()) + ", " +
                       tui::FormatSize(rec.last_output_size_mb()) + ")");
    } else {
        term.Line(Paint(fancy, tui::kYellow, "No output file was found - see logs\\cli\\homrec.log"));
        CliLog("ERROR", "no output file was found after the recording");
    }
    if (crashed) {
        term.Line(Paint(fancy, tui::kYellow, "The encoder stopped unexpectedly; what was recorded up to then was saved."));
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
            return Fail("config '" + shown + "' not found - run 'homrec setup', or start HomRec once to create it, "
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

    // The overlays of the active scene, like the HomRec window loads them (and like 'homrec overlay' edits them).
    HrcConfig::LoadScenes(state);

    HrWgcLog::SetEnabled(state.wgc_logging_enabled);
    state.disable_preview = true;   // there is no window to show the live preview in; never saved back

    Term term;
    const bool fancy = term.fancy();
    const fs::path out_dir = ResolveOutputFolder(state.output_folder);
    size_t visible = 0;
    for (const OverlayDef &o : state.overlays) if (o.visible) ++visible;

    const size_t cols = term.Cols();
    Header(term, std::string("HomRec ") + HR_APP_VERSION);
    term.Line(tui::InfoRow("config", cfg.u8string(), cols, fancy));
    term.Line(tui::InfoRow("output", out_dir.u8string(), cols, fancy));
    term.Line(tui::InfoRow(T(Msg::Overlays), state.overlays.empty() ? std::string("-")
                           : std::to_string(visible) + " / " + std::to_string(state.overlays.size()) + "  (" + SceneName(state) + ")",
                           cols, fancy));
    term.Line("  " + tui::Rule(std::min<size_t>(cols - 2, 56), fancy));
    CliLog("INFO", "start: config=" + cfg.u8string() + " output=" + out_dir.u8string() +
                   " overlays=" + std::to_string(visible) + "/" + std::to_string(state.overlays.size()));

    const int rc = Record(state, term, session);
    hr_pl_wait_all_detached(5000);
    CliLog("INFO", "start finished, exit code " + std::to_string(rc));
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
        Leaf("setup", "", "First-run setup: config, recordings folder, format, fps", CmdSetup),
        Leaf("start", "[config-path]", "Record the screen; Ctrl+C stops and saves", CmdStart),
        Leaf("pause", "", "Pause / resume the running recording", CmdPause),
        Group("config", {
            Leaf("path",   "",       "Show the .hrc config homrec is bound to", CmdConfigPath),
            Leaf("change", "<path>", "Bind homrec to another .hrc config",      CmdConfigChange),
        }),
        Group("overlay", {
            Leaf("list",   "",                               "List the overlays of the active scene (row 1 is in front)", CmdOverlayList),
            Leaf("add",    "<type> <value...> [key=value]",  "Add text | image | gif | webcam | input (e.g. add text \"Hi\" x=50 color=#FFAA00)", CmdOverlayAdd),
            Leaf("set",    "<row|id|name> <key=value...>",   "Change x y w h opacity visible locked name text color font path ...", CmdOverlaySet),
            Leaf("show",   "<row|id|name>",                  "Show an overlay", CmdOverlayShow),
            Leaf("hide",   "<row|id|name>",                  "Hide an overlay", CmdOverlayHide),
            Leaf("remove", "<row|id|name>",                  "Delete an overlay", CmdOverlayRemove),
            Leaf("edit",   "",                               "Interactive overlay editor", CmdOverlayEdit),
        }),
    };
}

#ifdef _WIN32

// 'homrec' without arguments in a console: a small menu instead of the usage text.
int RunHub() {
    for (;;) {
        Term term;
        const bool fancy = term.fancy();
        Header(term, std::string("HomRec ") + HR_APP_VERSION);
        const std::vector<std::string> items = {T(Msg::HubStart), T(Msg::HubOverlays), T(Msg::HubSetup), T(Msg::HubExit)};
        const MenuResult r = term.Menu(items.size(), [&](size_t i, bool s) { return tui::MenuItemLine(items[i], s, fancy); }, 0);
        if (r.index < 0 || r.index == 3) return 0;
        if (r.index == 0) return CmdStart({});
        if (r.index == 1) CmdOverlayEdit({});
        if (r.index == 2) SetupAndRemember(term);
    }
}

// The setup is offered once, on the first run in a real console - not for scripts, not for help.
bool WantsFirstRun(const Args &args) {
    if (IsSetupDone()) return false;
    {
        Term probe;
        if (!probe.can_ask()) return false;
    }
    if (args.empty()) return true;
    static const char *kSkip[] = {"setup", "--version", "-v", "-V", "help", "--help", "-h", "config", "pause"};
    for (const char *s : kSkip) if (args[0] == s) return false;
    for (const std::string &a : args) if (a == "-h" || a == "--help") return false;
    return true;
}

#endif

int Run(const Args &args) {
    const std::string banner = std::string("homrec ") + HR_APP_VERSION + " -- HomRec command-line interface";
#ifdef _WIN32
    HrLogPaths::SetLogsSubdir(L"cli");   // every log of this process: logs\cli\ (before anything logs)
    {
        std::error_code ec;
        g_start_cwd = fs::current_path(ec);
    }
    g_lang = DetectLang();

    // Asking for the version / help / the config path (often done from scripts) is not logged.
    const bool quiet = !args.empty() && (args[0] == "--version" || args[0] == "-v" || args[0] == "-V" || args[0] == "help" ||
                                         args[0] == "--help" || args[0] == "-h" ||
                                         (args[0] == "config" && args.size() >= 2 && args[1] == "path"));
    if (!quiet) {
        std::string joined;
        for (const std::string &a : args) joined += (joined.empty() ? "" : " ") + a;
        CliLog("INFO", std::string("homrec ") + HR_APP_VERSION + (joined.empty() ? "" : " " + joined));
    }

    if (WantsFirstRun(args)) {
        Term term;
        SetupAndRemember(term);
        g_lang = DetectLang();   // the setup may have bound another config
    }
    if (args.empty()) {
        bool console = false;
        { Term probe; console = probe.can_ask(); }
        if (console) return RunHub();
    }
#endif
    const int rc = Dispatch(banner, "homrec", BuildCommands(), args);
#ifdef _WIN32
    if (!quiet) CliLog(rc == 0 ? "INFO" : "WARN", "exit code " + std::to_string(rc));
#endif
    return rc;
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
