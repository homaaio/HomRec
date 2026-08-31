// console_window.h
//
// The HomRec developer console: a scrollable output log + input line with
// command history (Up/Down arrows), plus a small built-in command set.
// Bare command names only (Linux-shell style) - "rm", not "$rm" or "!rm".
// Options are passed the same way a real shell CLI would: "--flag" /
// "--key=value" tokens, not the old "#key=value" named-parameter syntax.
//
// Supported commands:
//   - Command parsing: ParseNamed ("--key=\"value\""/"--key=value"),
//     ParseFlags (-flag tokens, excluding -return/-ret).
//   - The three-tier security fuse (sec/secui/secp) - core/UI/plugin
//     lock state and the associated unlock-gating logic.
//   - version, ping, echo, clear, env, alias, history, info, status, log,
//     hide, pwd, whoami, hom (forwards to hom.exe, the plugin package
//     manager - see CmdHom()).
//   - The "inwid" confirmation gate: a handful of commands that reach
//     the network and rewrite files (hom update/remove), delete things
//     (rm), or persist a settings change (any "<setting> <value>", not
//     a bare query) are refused unless prefixed with "inwid" ("I Know
//     What I'm Doing") - "disable_preview true" is blocked, "inwid
//     disable_preview true" runs it. See CommandNeedsInwid() in the
//     .cpp. This is intentionally separate from sec/secui/secp below -
//     inwid is a per-line "yes I meant that", not a session-wide unlock.
//     A cfg file (RunCfgFile()) is exempt from this gate: it's local
//     content the person already put on disk, not a one-off typed line.
//   - rm --system@homrec.files (clears recordings/plugins/logs/cache,
//     gated by sec 0) and rm @homrec (schedules self-uninstall via a
//     generated .bat, gated by sec 0 + interactive confirmation) - these
//     two are the most consequential commands in the file, so treat any
//     change to them carefully.
//
// Not yet implemented:
//   rule, connect, disconnect, start --window, rename, create, edit, ls,
//   watch, batch, run, repeat, timer, clip, check_er, homrec, generic
//   rm/rm_vid/rm_ui/rm_ui_self, fs@plugins, fs@settings, do (self-update),
//   edit_terminal.
// Most of these depend on a "created window / rule" registry that doesn't
// exist anywhere else yet - implementing them needs that subsystem
// designed first, rather than guessing at it inside this file.
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <thread>
#include <mutex>
#include "app_state.h"

class RecordingController;
class LuaPluginEngine;

class ConsoleWindow {
public:
    ConsoleWindow(AppState &state, RecordingController *rec, HWND main_window,
                  LuaPluginEngine *plugins = nullptr);
    ~ConsoleWindow();

    void Show(HINSTANCE hInst);
    // Creates the underlying window (and runs OnCreate(), including the
    // controls Print()/RunCommand() need) WITHOUT making it visible - the
    // part of Show() before its trailing ShowWindow(SW_SHOW). Called at
    // app startup so cfg/autoexec.cfg (see RunCfgFile()) has somewhere to
    // print its output to, without flashing the console open every time
    // the app launches. Show() itself now just calls this, then makes it
    // visible - safe/idempotent to call either one first. A window
    // created this way stays hidden until Show() (or a real toggle -
    // Ctrl+Shift+T, the Help menu) is used for the first time.
    void EnsureCreated(HINSTANCE hInst);
    HWND hwnd() const { return hwnd_; }

    // Reads cfg/<name>.cfg (relative to the app's own folder, via
    // GetBaseDir()) and feeds each non-blank, non-comment line through
    // RunCommand() as if typed directly - including any command a plugin
    // (e.g. Bter, via homrec.register_command()) has registered, since
    // this goes through the exact same dispatcher. Comment lines start
    // with "//" or "#" (leading whitespace allowed). Missing file is not
    // an error - this is opt-in by simply dropping the file in, matching
    // e.g. Source engine's autoexec.cfg convention. Returns the number of
    // commands actually run (0 if the file doesn't exist or is empty).
    int RunCfgFile(const std::wstring &name);

private:
    static LRESULT CALLBACK WindowProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);

    void OnCreate(HINSTANCE hInst);
    void OnCommand(int id, int notifyCode, HWND ctrl);
    void OnSize(int w, int h);
    // Rebuilds the "1920x1080@60fps # " prompt label from the currently
    // selected monitor + resolution scale + target fps, so it stays
    // accurate if those were changed in Settings since the console was
    // last opened.
    void RefreshPrompt();

    // Parses and dispatches one line as a command, same as if it were
    // typed directly at the prompt. `confirmed` is true only on the
    // recursive call RunCommand() makes to itself after stripping a
    // leading "inwid" prefix (see the "inwid gate" block at the top of
    // the .cpp) and on every call coming from RunCfgFile() below (a cfg
    // file already sitting on disk in this install is trusted content,
    // the same way .bashrc doesn't re-ask for sudo on every line) -
    // ordinary typed input always starts as false. When false, a command
    // CommandNeedsInwid() flags as gated (hom update/remove, rm, or any
    // settings *assignment* - see TryRunSetting()) is refused instead of
    // run, with a message telling the person to prefix it with "inwid".
    void RunCommand(const std::wstring &raw, bool confirmed = false);
    // Fallback tried when `cmd` didn't match a built-in command or a
    // plugin-registered command (homrec.register_command()): treats the
    // line as "<setting> [=] <value>" (or a bare "<setting>" to query),
    // against both HrSettingsRegistry's built-in .hrc-backed settings and
    // any plugin-registered ones (homrec.register_setting()) - see
    // commands.md's "Settings as commands" section. Returns false (line
    // untouched, caller falls through to "Unknown command") if `cmd`
    // isn't a known setting name either. Prints its own PrintOk/PrintWarn/
    // PrintInfo output when it returns true, same convention every other
    // Cmd* handler follows. A *query* (bare "<setting>", no value) never
    // needs `confirmed`; *setting* a value does, same "inwid" gate
    // RunCommand() applies to hom/rm - see CommandNeedsInwid() in the
    // .cpp. Known limitation: this gate only covers built-in
    // (HrSettingsRegistry) settings, not plugin-registered ones reached
    // through DispatchSetting() below - see commands.md.
    bool TryRunSetting(const std::wstring &cmd, const std::wstring &raw, bool confirmed);
    // color is a COLORREF; only meaningful when the output control is a
    // RichEdit (rich_edit_ == true) -- see the color constants and the
    // OPT comment above OnCreate()'s control creation in the .cpp for why.
    // Plain-EDIT fallback ignores it (ES_READONLY EDIT can't do per-run
    // color at all -- everything in the box is one uniform color) and the
    // leading glyph below is still what carries the distinction there.
    void Print(const std::wstring &line, COLORREF color = kColText);
    void PrintOk(const std::wstring &s)   { Print(L"  \u2714  " + s, kColOk); }
    void PrintInfo(const std::wstring &s) { Print(L"  \u00b7  " + s, kColInfo); }
    void PrintWarn(const std::wstring &s) { Print(L"  \u26a0  " + s, kColWarn); }
    void PrintErr(const std::wstring &s)  { Print(L"  \u2716  " + s, kColErr); }

    // Terminal-style palette (roughly matching common ANSI green/yellow/
    // red conventions, e.g. `journalctl`/`git status` coloring) so
    // errors/warnings actually stand out from normal output instead of
    // everything being the same green with only a leading glyph to tell
    // them apart.
    static constexpr COLORREF kColOk   = RGB(80, 220, 120);   // green  - success
    static constexpr COLORREF kColInfo = RGB(150, 200, 220);  // pale cyan - neutral log
    static constexpr COLORREF kColWarn = RGB(230, 200, 90);   // amber  - warning
    static constexpr COLORREF kColErr  = RGB(235, 100, 100);  // red    - error
    static constexpr COLORREF kColPrompt = RGB(120, 220, 255);
    // Default for plain Print() calls with no explicit level (echoed
    // input, echo, history listing, clipboard paste) - a neutral
    // near-white so it reads as "plain terminal text", distinct from any
    // of the four status colors above, and never silently reuses
    // whatever color happened to be set by the previous line.
    static constexpr COLORREF kColText = RGB(225, 225, 225);

    // --- ported commands ---
    void CmdVersion(const std::wstring &raw);
    void CmdVer(const std::wstring &raw);
    void CmdPing(const std::wstring &raw);
    void CmdEcho(const std::wstring &raw);
    void CmdClear(const std::wstring &raw);
    void CmdEnv(const std::wstring &raw);
    void CmdAlias(const std::wstring &raw);
    void CmdHistory(const std::wstring &raw);
    void CmdInfo(const std::wstring &raw);
    void CmdStatus(const std::wstring &raw);
    void CmdLog(const std::wstring &raw);
    void CmdHide(const std::wstring &raw);
    void CmdSec(const std::wstring &raw);
    void CmdSecUi(const std::wstring &raw);
    void CmdSecP(const std::wstring &raw);
    void CmdRmSystemFiles(const std::wstring &raw);
    void CmdRmSelfApp(const std::wstring &raw);
    void CmdHrc(const std::wstring &raw);
    // "sethrc <path> <1|true|0|false>" - cfg-only combinator (see
    // commands.md): merges another .hrc file's fields into the *current*
    // in-memory settings right where this line sits in a cfg script (same
    // "only touch keys the file actually has" semantics as `hrc load`), so
    // a hand-written cfg/config.cfg can start from "sethrc homrec.hrc 1"
    // and then override just the handful of fields it cares about on the
    // lines that follow - ordinary sequential command execution is what
    // gives later lines priority, no separate exception syntax needed.
    // custom_ffmpeg_args is only carried over while the "sec" fuse is
    // disabled (see HrcConfig::Load's allow_sensitive_fields parameter).
    void CmdSetHrc(const std::wstring &raw);
    void CmdClip(const std::wstring &raw);
    void CmdRepeat(const std::wstring &raw);
    void CmdBatch(const std::wstring &raw);
    void CmdLs(const std::wstring &raw);
    // Unix-flavor additions - purely informational, no "inwid" gating
    // needed since neither reads/writes anything sensitive.
    void CmdPwd(const std::wstring &raw);
    void CmdWhoami(const std::wstring &raw);
    // Forwards everything typed after "hom" to hom.exe (the standalone
    // plugin package manager, tools/hom/hom.cpp) as a child process, with
    // its working directory set to HomRec's own folder so "hom install x"
    // writes to the same .\plugins\ HomRec itself reads from - same
    // behaviour as running hom.exe from PowerShell/cmd in that folder,
    // just without having to leave the app. hom.exe's own stdout/stderr
    // are printed back verbatim (it already formats its own success/error
    // text), not re-wrapped in PrintOk/PrintErr.
    void CmdHom(const std::wstring &raw);
    // hom.exe does real network I/O (up to CmdHom()'s own 30s timeout) -
    // running RunCapturedProcess() straight from CmdHom() (i.e. on this
    // window's own message-loop thread, the same one wx/win32 pumps the
    // whole app's UI on - see the .cpp for why there's no separate thread
    // for this window) used to freeze all of HomRec, not just the
    // console, for as long as hom.exe's network request took. Fixed by
    // running the actual process on hom_thread_ and posting kWmHomDone
    // back to this window once it's done (HandleMessage() picks that up
    // and prints the result) - RunCommand() itself now just kicks the
    // thread off and returns immediately. hom_result_mtx_ guards the one
    // in-flight HomResult handed from that thread back to the UI thread;
    // hom_thread_ is joined (near-instant, since kWmHomDone only fires
    // after the thread's already done its work and is about to return)
    // before starting a new one and in the destructor, so a hom command
    // and app shutdown/exit can never race a still-detached thread.
    static constexpr UINT kWmHomDone = WM_APP + 1;
    struct HomResult {
        std::wstring hom_path;   // for the "couldn't start" message
        std::wstring output;
        DWORD        code = 0;
        bool         started = false; // false -> RunCapturedProcess() itself failed
    };
    void JoinPendingHomThread();
    std::thread hom_thread_;
    std::mutex  hom_result_mtx_;
    HomResult   hom_result_;
    // Prints the standard "blocked, prefix with inwid" refusal for a
    // gated command - see CommandNeedsInwid() in the .cpp and the class
    // comment above.
    void RefuseNeedsInwid(const std::wstring &raw);
    void ScheduleSelfDelete(const std::wstring &base_dir);

    bool CoreUnlocked() const { return !sec_core_; }
    bool UiUnlocked() const { return !sec_core_ || !sec_ui_; }

    AppState &state_;
    RecordingController *rec_;
    HWND main_window_;
    LuaPluginEngine *plugins_;

    HWND hwnd_ = nullptr;
    HWND output_ = nullptr;
    HWND input_ = nullptr;
    HWND prompt_ = nullptr;
    HFONT mono_font_ = nullptr;
    // True once output_ was successfully created as a RichEdit control
    // (Msftedit.dll loaded OK). Falls back to a plain read-only EDIT
    // control -- still fully usable, just single-colored -- if that load
    // ever fails, which is why every richedit-only call below is gated on
    // this instead of assumed.
    bool rich_edit_ = false;
    // Actual pixel width the prompt text needs (recomputed by
    // RefreshPrompt() from the real string, e.g. "3840x2160@144fps # " is
    // noticeably wider than "1920x1080@60fps # "). OnSize() uses this
    // instead of a fixed guess so the input box never overlaps/clips the
    // prompt at higher resolutions or frame rates.
    int prompt_width_ = 160;

    std::vector<std::wstring> history_;
    int history_pos_ = -1;
    std::unordered_map<std::wstring, std::wstring> aliases_;
    std::unordered_map<std::wstring, std::wstring> env_vars_; // session-scoped, NOT the OS environment

    bool sec_core_ = true;   // sec    - master fuse, protected by default
    bool sec_ui_ = true;     // secui  - UI-removal protection
    bool sec_plugin_ = true; // secp   - plugin version-check / RAM watchdog
};

// Parsing helpers - direct ports of _parse_named/_parse_flags, exposed so
// they can be unit-exercised or reused if more commands get added later.
namespace ConsoleParse {
    // --key="value" | --key='value' | --key=value  ->  value, or empty if
    // absent (shell-style long option, not the old "#key=value" form).
    std::wstring ParseNamed(const std::wstring &raw, const std::wstring &key);
    // All "-flag" tokens in raw, except -return/-ret (matches _parse_flags).
    std::set<std::wstring> ParseFlags(const std::wstring &raw);
}