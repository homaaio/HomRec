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
//     hide.
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

    void RunCommand(const std::wstring &raw);
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
    void CmdClip(const std::wstring &raw);
    void CmdRepeat(const std::wstring &raw);
    void CmdBatch(const std::wstring &raw);
    void CmdLs(const std::wstring &raw);
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
