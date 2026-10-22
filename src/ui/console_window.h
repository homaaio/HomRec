// console_window.h - The HomRec developer console: a scrollable output log + input line with
// command history (Up/Down arrows), plus a small built-in command set.
// Bare command names only (Linux-shell style) - "rm", not "$rm" or "!rm".
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include "app_state.h"

class RecordingController;
class LuaPluginEngine;

class ConsoleWindow {
public:
    ConsoleWindow(AppState &state, RecordingController *rec, HWND main_window,
                  LuaPluginEngine *plugins = nullptr);
    ~ConsoleWindow();

    void Show(HINSTANCE hInst);
    // Creates the underlying window (and runs OnCreate(), including the controls
    // Print()/RunCommand() need) WITHOUT making it visible - the part of Show() before its
    // trailing ShowWindow(SW_SHOW).
    void EnsureCreated(HINSTANCE hInst);
    HWND hwnd() const { return hwnd_; }

    // Reads cfg/<name>.cfg (relative to the app's own folder, via GetBaseDir()) and feeds each
    // non-blank, non-comment line through RunCommand() as if typed directly - including any
    // command a plugin (e.g.
    int RunCfgFile(const std::wstring &name);

    void RunSingleCommand(const std::wstring &raw, bool confirmed = true) {
        RunCommand(raw, confirmed);
    }

private:
    static LRESULT CALLBACK WindowProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);

    void OnCreate(HINSTANCE hInst);
    void OnCommand(int id, int notifyCode, HWND ctrl);
    void OnSize(int w, int h);
    // Rebuilds the "1920x1080@60fps # " prompt label from the currently selected monitor +
    // resolution scale + target fps, so it stays accurate if those were changed in Settings
    // since the console was last opened.
    void RefreshPrompt();

    // Parses and dispatches one line as a command, same as if it were typed directly at the
    // prompt.
    void RunCommand(const std::wstring &raw, bool confirmed = false);
    bool TryRunSetting(const std::wstring &cmd, const std::wstring &raw, bool confirmed);
    // color is a COLORREF; only meaningful when the output control is a
    // RichEdit (rich_edit_ == true) -- see the color constants and the
    // OPT comment above OnCreate()'s control creation in the .cpp for why.
    void Print(const std::wstring &line, COLORREF color = kColText);
    void PrintOk(const std::wstring &s)   { Print(L"  \u2714  " + s, kColOk); }
    void PrintInfo(const std::wstring &s) { Print(L"  \u00b7  " + s, kColInfo); }
    void PrintWarn(const std::wstring &s) { Print(L"  \u26a0  " + s, kColWarn); }
    void PrintErr(const std::wstring &s)  { Print(L"  \u2716  " + s, kColErr); }

    // Terminal-style palette (roughly matching common ANSI green/yellow/ red conventions, e.g.
    static constexpr COLORREF kColOk   = RGB(80, 220, 120);   // green  - success
    static constexpr COLORREF kColInfo = RGB(150, 200, 220);  // pale cyan - neutral log
    static constexpr COLORREF kColWarn = RGB(230, 200, 90);   // amber  - warning
    static constexpr COLORREF kColErr  = RGB(235, 100, 100);  // red    - error
    static constexpr COLORREF kColPrompt = RGB(120, 220, 255);
    static constexpr COLORREF kColText = RGB(225, 225, 225);

    // --- ported commands ---
    void CmdVersion(const std::wstring &raw);
    void CmdVer(const std::wstring &raw);
    void CmdPing(const std::wstring &raw);
    void CmdEcho(const std::wstring &raw);
    void CmdClear(const std::wstring &raw);
    void CmdEnv(const std::wstring &raw);
    void CmdAlias(const std::wstring &raw);
    // `history` lists, `history clear` (or -c) empties it - in memory and the file.
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
    void CmdPreset(const std::wstring &raw);
    void CmdSetHrc(const std::wstring &raw);
    void CmdClip(const std::wstring &raw);
    void CmdRepeat(const std::wstring &raw);
    void CmdBatch(const std::wstring &raw);
    void CmdLs(const std::wstring &raw);
    // Unix-flavor additions - purely informational, no "inwid" gating
    // needed since neither reads/writes anything sensitive.
    void CmdPwd(const std::wstring &raw);
    void CmdWhoami(const std::wstring &raw);
    void CmdHom(const std::wstring &raw);
    // hom.exe does real network I/O - running it straight from CmdHom() (i.e.
    static constexpr UINT kWmHomDone  = WM_APP + 1;
    static constexpr UINT kWmHomLines = WM_APP + 2;
    struct HomResult {
        std::wstring hom_path;   // for the "couldn't start" message
        DWORD        code = 0;
        bool         started = false;       // false -> hom.exe couldn't be started at all
        bool         restore_failed = false; // true -> hom.exe was missing and couldn't be downloaded
        bool         idle_timeout = false;   // killed: no output for too long
    };
    // kind of one queued output line (see DrainHomLines())
    enum HomLineKind { kHomPlain = 0, kHomInfo, kHomOk, kHomWarn, kHomErr, kHomProgress };
    struct HomLine { HomLineKind kind; std::wstring text; };
    void JoinPendingHomThread();
    void QueueHomLine(HWND target, HomLineKind kind, std::wstring text);
    void DrainHomLines();
    std::thread hom_thread_;
    std::mutex  hom_result_mtx_;
    HomResult   hom_result_;
    std::mutex             hom_lines_mtx_;
    std::deque<HomLine>    hom_lines_;
    bool                   hom_lines_pending_ = false; // guarded by hom_lines_mtx_
    std::atomic<bool>      hom_busy_{false};
    std::atomic<bool>      hom_cancel_{false};

    void ScrollToBottom();
    // One line that is rewritten in place (download progress). The first call
    // appends a line; the following calls replace that line's text until
    // anything else is printed (Print() ends it) or EndProgressLine().
    void SetProgressLine(const std::wstring &text);
    void EndProgressLine() { progress_active_ = false; }
    bool progress_active_ = false;

    // --- command history (Up/Down, `history`, `history clear`) --- Persisted to
    // cfg\console_history.txt next to hr.exe (UTF-8, one command per line, newest last, at
    // most kHistoryMax kept) unless the setting console_history_save is off.
    static constexpr size_t kHistoryMax = 500;
    std::wstring HistoryFilePath() const;
    void LoadHistory();
    void RememberCommand(const std::wstring &cmd);
    void ClearHistory();
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
    // True once output_ was successfully created as a RichEdit control (Msftedit.dll loaded
    // OK).
    bool rich_edit_ = false;
    // Actual pixel width the prompt text needs (recomputed by RefreshPrompt() from the real
    // string, e.g.
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
