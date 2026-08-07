// console_window.h
//
// The HomRec developer console: a scrollable output log + input line with
// command history (Up/Down arrows), plus a small built-in command set.
//
// Supported commands:
//   - Command parsing: ParseNamed (#key="value"/#key=value), ParseFlags
//     (-flag tokens, excluding -return/-ret).
//   - The three-tier security fuse ($sec/$secui/$secp) - core/UI/plugin
//     lock state and the associated unlock-gating logic.
//   - $version, $ping, $echo, clear/$clear, $env, $alias, $history, $info,
//     $status, $log, $hide.
//   - $rm --system@homrec.files (clears recordings/plugins/logs/cache,
//     gated by $sec 0) and $rm @homrec (schedules self-uninstall via a
//     generated .bat, gated by $sec 0 + interactive confirmation) - these
//     two are the most consequential commands in the file, so treat any
//     change to them carefully.
//
// Not yet implemented:
//   $rule, $connect, $disconnect, $start --window, $rename, $create, $edit,
//   $ls, $watch, $batch, $run, $repeat, $timer, $clip, $check_er, $homrec,
//   generic $rm/$rm_vid/$rm_ui/$rm_ui_self, $fs@plugins, $fs@settings, $do
//   (self-update), $edit_terminal.
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

class ConsoleWindow {
public:
    ConsoleWindow(AppState &state, RecordingController *rec, HWND main_window);
    ~ConsoleWindow();

    void Show(HINSTANCE hInst);
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
    void Print(const std::wstring &line);
    void PrintOk(const std::wstring &s)   { Print(L"  \u2714  " + s); }
    void PrintInfo(const std::wstring &s) { Print(L"  \u00b7  " + s); }
    void PrintWarn(const std::wstring &s) { Print(L"  \u26a0  " + s); }
    void PrintErr(const std::wstring &s)  { Print(L"  \u2716  " + s); }

    // --- ported commands ---
    void CmdVersion(const std::wstring &raw);
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

    HWND hwnd_ = nullptr;
    HWND output_ = nullptr;
    HWND input_ = nullptr;
    HWND prompt_ = nullptr;
    HFONT mono_font_ = nullptr;
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

    bool sec_core_ = true;   // $sec    - master fuse, protected by default
    bool sec_ui_ = true;     // $secui  - UI-removal protection
    bool sec_plugin_ = true; // $secp   - plugin version-check / RAM watchdog
};

// Parsing helpers - direct ports of _parse_named/_parse_flags, exposed so
// they can be unit-exercised or reused if more commands get added later.
namespace ConsoleParse {
    // #key="value" | #key='value' | #key=value  →  value, or empty if absent.
    std::wstring ParseNamed(const std::wstring &raw, const std::wstring &key);
    // All "-flag" tokens in raw, except -return/-ret (matches _parse_flags).
    std::set<std::wstring> ParseFlags(const std::wstring &raw);
}
