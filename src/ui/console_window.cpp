#include "console_window.h"
#include "version.h"
#include "recording_controller.h"
#include "win32_theme.h"
#include "hrc_config.h"
#include "../plugins/lua_engine.h"
#include "../hr_log_paths.h"
#include "../hr_settings_registry.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>
#include <cmath>
#include <cwchar>
#include <thread>       // reader thread in RunCapturedProcess() (CmdHom)
#include <richedit.h>   // CHARFORMAT2W / EM_SETCHARFORMAT / EM_SETBKGNDCOLOR
#include <uxtheme.h>    // SetWindowTheme (dark scrollbars, Win10 1809+)

extern "C" {
    void *hr_di_create();
    void hr_di_destroy(void *handle);
    void hr_di_refresh(void *handle);
    int hr_di_count(void *handle);
    int hr_di_get(void *handle, int index, int *out_x, int *out_y,
                   int *out_w, int *out_h, float *out_dpi);
}

namespace {

std::wstring Trim(const std::wstring &s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

// UTF-16 -> UTF-8, for handing command text to the Lua plugin layer
// (DispatchCommand()/homrec.register_command()), which speaks narrow
// UTF-8 strings throughout, same as the rest of the Lua API.
std::string NarrowFromWide(const std::wstring &w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? len - 1 : 0, '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

std::wstring WideFromNarrow(const std::string &s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len > 0 ? len - 1 : 0, L'\0');
    if (len > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

// Everything in `raw` after the first whitespace-delimited token (the
// setting's name, as originally typed) and an optional '=' - so
// "disable_preview = true", "disable_preview=true", and
// "disable_preview true" are all accepted, matching how a person would
// reasonably type any of the three. Returns "" (not an error - see
// ConsoleWindow::TryRunSetting) if nothing follows the name, which is how
// a bare "disable_preview" queries the current value instead of setting
// it - the same "no args = show current state" convention env/alias/sec
// above already use.
std::wstring ExtractSettingValue(const std::wstring &raw) {
    size_t i = 0;
    while (i < raw.size() && !iswspace(raw[i])) ++i;      // skip the name itself
    while (i < raw.size() && iswspace(raw[i])) ++i;       // skip spaces after it
    if (i < raw.size() && raw[i] == L'=') {
        ++i;
        while (i < raw.size() && iswspace(raw[i])) ++i;   // skip spaces after '='
    }
    return Trim(raw.substr(i));
}

// Should this resolved (post-alias-expansion) command require the
// "inwid" confirmation prefix before RunCommand() actually runs it? See
// the class comment at the top of console_window.h for the concept.
// `cmd` is the already-lowercased first token; `raw` is the full line
// (needed for "hom", where it's the *subcommand* - update/remove vs.
// install - that decides this, not "hom" itself).
//
// Deliberately NOT gated: version/ping/echo/clear/env/alias/history/
// info/status/log/hide/sec*/hrc/clip/repeat/batch/ls, "hom install"/
// "hom --version"/"hom ping"/bare "hom", and querying a setting (no
// value). None of those write anything persistent or delete anything.
bool CommandNeedsInwid(const std::wstring &cmd, const std::wstring &raw) {
    if (cmd == L"rm") return true;
    if (cmd == L"hom") {
        std::wistringstream iss(raw);
        std::wstring first, sub;
        iss >> first >> sub;
        std::transform(sub.begin(), sub.end(), sub.begin(), ::towlower);
        // "update" re-downloads and swaps hom.exe itself; "remove"/
        // "uninstall" delete a plugin. "install" (including the
        // "update-hrp" special-case) and everything else (--version,
        // ping, bare help) stay ungated - that's what a package manager
        // is *for*, it shouldn't need a second confirmation every time.
        return sub == L"update" || sub == L"remove" || sub == L"uninstall";
    }
    return false;
}

std::wstring GetBaseDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full = path;
    size_t pos = full.find_last_of(L"\\/");
    return pos == std::wstring::npos ? full : full.substr(0, pos);
}

bool RemoveDirRecursive(const std::wstring &path) {
    std::wstring pattern = path + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring full = path + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirRecursive(full);
            } else {
                SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(path.c_str()) != 0;
}

bool DirExists(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool FileExists(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Runs `cmdline` as a child process with working directory `cwd`,
// capturing its combined stdout+stderr. Used by CmdHom() to run hom.exe
// in-process instead of requiring a separate PowerShell/cmd window.
//
// The reader thread (draining the pipe while we wait, not after) exists
// for the same reason hr_tools.cpp's run_cmd() has one: CreatePipe()'s
// default buffer is small, and a child that writes more than that before
// anyone reads it will block on WriteFile() forever if we only start
// reading after WaitForSingleObject() returns - see that file's comment
// for the full story.
//
// Returns false only if the process couldn't be started at all (missing
// exe, etc.) - a timeout or non-zero exit still returns true, with
// *out_code set to (DWORD)-2 for "killed after timing out" so the caller
// can tell that apart from a real exit code.
bool RunCapturedProcess(const std::wstring &cmdline, const std::wstring &cwd,
                         DWORD timeout_ms, std::wstring *out_text, DWORD *out_code) {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mut_cmd(cmdline.begin(), cmdline.end());
    mut_cmd.push_back(L'\0');
    if (!CreateProcessW(nullptr, mut_cmd.data(), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW, nullptr,
                         cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return false;
    }
    CloseHandle(hWrite);

    std::string raw;
    std::thread reader([&]() {
        char buf[4096];
        DWORD br = 0;
        while (ReadFile(hRead, buf, sizeof(buf) - 1, &br, nullptr) && br) {
            buf[br] = '\0';
            raw += buf;
        }
    });

    DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
    bool timed_out = (wait == WAIT_TIMEOUT);
    if (timed_out) TerminateProcess(pi.hProcess, 1);

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // The process exiting (naturally, or via TerminateProcess just above)
    // closes its inherited handle to the write end, which is what lets
    // the reader thread's ReadFile loop see EOF and return.
    reader.join();
    CloseHandle(hRead);

    if (!raw.empty()) {
        int wl = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, nullptr, 0);
        std::wstring w(wl > 0 ? wl - 1 : 0, L'\0');
        if (wl > 1) MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, w.data(), wl);
        *out_text = w;
    } else {
        *out_text = L"";
    }
    *out_code = timed_out ? (DWORD)-2 : code;
    return true;
}

enum { IDC_CONSOLE_INPUT = 9001, IDC_CONSOLE_OUTPUT };

} // namespace

// ---------------------------------------------------------------------------
// Parsing helpers - direct ports of _parse_named / _parse_flags.
// ---------------------------------------------------------------------------

namespace ConsoleParse {

std::wstring ParseNamed(const std::wstring &raw, const std::wstring &key) {
    std::wstring needleQ = L"--" + key + L"=\"";
    size_t pos = raw.find(needleQ);
    if (pos != std::wstring::npos) {
        size_t start = pos + needleQ.size();
        size_t end = raw.find(L'"', start);
        if (end != std::wstring::npos) return raw.substr(start, end - start);
    }
    std::wstring needleApos = L"--" + key + L"='";
    pos = raw.find(needleApos);
    if (pos != std::wstring::npos) {
        size_t start = pos + needleApos.size();
        size_t end = raw.find(L'\'', start);
        if (end != std::wstring::npos) return raw.substr(start, end - start);
    }
    std::wstring needle = L"--" + key + L"=";
    pos = raw.find(needle);
    if (pos != std::wstring::npos) {
        size_t start = pos + needle.size();
        size_t end = start;
        while (end < raw.size() && raw[end] != L' ' && raw[end] != L'\t' &&
               raw[end] != L'"' && raw[end] != L'\'') {
            ++end;
        }
        if (end > start) return raw.substr(start, end - start);
    }
    return L"";
}

std::set<std::wstring> ParseFlags(const std::wstring &raw) {
    std::set<std::wstring> flags;
    std::wistringstream iss(raw);
    std::wstring tok;
    while (iss >> tok) {
        if (tok.size() > 1 && tok[0] == L'-' && iswalpha(tok[1])) {
            bool allAlpha = true;
            for (size_t i = 1; i < tok.size(); ++i) {
                if (!iswalpha(tok[i])) { allAlpha = false; break; }
            }
            if (allAlpha) flags.insert(tok);
        }
    }
    flags.erase(L"-return");
    flags.erase(L"-ret");
    return flags;
}

} // namespace ConsoleParse

// ---------------------------------------------------------------------------
// ConsoleWindow
// ---------------------------------------------------------------------------

ConsoleWindow::ConsoleWindow(AppState &state, RecordingController *rec, HWND main_window,
                             LuaPluginEngine *plugins)
    : state_(state), rec_(rec), main_window_(main_window), plugins_(plugins) {}

ConsoleWindow::~ConsoleWindow() {
    JoinPendingHomThread();
    if (hwnd_) DestroyWindow(hwnd_);
}

LRESULT CALLBACK ConsoleWindow::WindowProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ConsoleWindow *self = nullptr;
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        self = reinterpret_cast<ConsoleWindow *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = reinterpret_cast<ConsoleWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT ConsoleWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            OnSize(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wParam), HIWORD(wParam), (HWND)lParam);
            return 0;
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE); // console is a tool window, not app-exiting - matches Ctrl+Shift+T toggle behavior
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            static HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
            SetBkColor(hdc, RGB(0, 0, 0));
            // Prompt text is the brightest thing in the window - the part
            // of a shell prompt you'd normally see colored - everything
            // else (log output) is plain terminal green.
            SetTextColor(hdc, (HWND)lParam == prompt_ ? kColPrompt : kColOk);
            return (LRESULT)blackBrush;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            static HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
            SetBkColor(hdc, RGB(0, 0, 0));
            SetTextColor(hdc, kColOk);
            return (LRESULT)blackBrush;
        }
        case kWmHomDone: {
            // Posted by CmdHom()'s background thread right before it
            // returns - see hom_thread_'s declaration. The thread has
            // already finished all its work by the time this message is
            // even queued, so hom_thread_.join() below is effectively
            // instant, never a real wait.
            HomResult res;
            { std::lock_guard<std::mutex> lk(hom_result_mtx_); res = hom_result_; }
            if (hom_thread_.joinable()) hom_thread_.join();

            if (!res.started) {
                PrintErr(L"hom: couldn't start " + res.hom_path);
                return 0;
            }
            // Batch the redraw across every output line instead of one
            // reflow per SendMessageW(EM_REPLACESEL) - `hom install
            // update-hrp` (or any command producing many lines) was
            // visibly choppy printing them one at a time, each forcing
            // its own RichEdit layout pass.
            if (rich_edit_) SendMessageW(output_, WM_SETREDRAW, FALSE, 0);
            std::wistringstream oss(res.output);
            std::wstring line;
            while (std::getline(oss, line)) {
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                if (!line.empty()) Print(line, kColText);
            }
            if (res.code == static_cast<DWORD>(-2)) {
                PrintErr(L"hom: timed out after 30s and was killed (network hang?)");
            } else if (res.code != 0) {
                PrintWarn(L"hom: exited with code " + std::to_wstring(res.code));
            }
            if (rich_edit_) {
                SendMessageW(output_, WM_SETREDRAW, TRUE, 0);
                InvalidateRect(output_, nullptr, FALSE);
            }
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void ConsoleWindow::EnsureCreated(HINSTANCE hInst) {
    if (hwnd_) return;
    static const wchar_t kClass[] = L"HomRecConsoleWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProcThunk;
    wc.hInstance = hInst;
    wc.lpszClassName = kClass;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    int x, y, w, h;
    HrWin32Theme::CenteredWindowRect(760, 480, WS_OVERLAPPEDWINDOW, x, y, w, h);
    hwnd_ = CreateWindowExW(0, kClass, L"HomRec Console",
                             WS_OVERLAPPEDWINDOW,
                             x, y, w, h,
                             main_window_, nullptr, hInst, this);
    // Deliberately no ShowWindow() here - CreateWindowExW() without
    // WS_VISIBLE already leaves it hidden, which is the whole point (see
    // this function's header comment).
    HrWin32Theme::ApplyDarkTitleBar(hwnd_);
    OnCreate(hInst);
}

void ConsoleWindow::Show(HINSTANCE hInst) {
    EnsureCreated(hInst);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    RefreshPrompt();
    SetFocus(input_);
}

void ConsoleWindow::RefreshPrompt() {
    if (!prompt_) return;

    // Mirrors RecordingController::ResolveCaptureSize()'s logic (same
    // 1-based monitor_id convention, same fallback-to-primary, same
    // even-dimension rounding) so the prompt shows the actual resolution
    // a recording would start at right now, not just the monitor's raw
    // native size.
    int w = 1920, h = 1080;
    void *di = hr_di_create();
    if (di) {
        hr_di_refresh(di);
        int mx = 0, my = 0, mw = 1920, mh = 1080;
        float dpi = 96.0f;
        int idx = state_.monitor_id > 0 ? state_.monitor_id - 1 : 0;
        if (hr_di_get(di, idx, &mx, &my, &mw, &mh, &dpi)) {
            w = mw; h = mh;
        }
        hr_di_destroy(di);
    }
    w = (int)(w * state_.scale_factor);
    h = (int)(h * state_.scale_factor);
    if (w % 2) w--;
    if (h % 2) h--;

    wchar_t buf[64];
    swprintf(buf, 64, L"%dx%d@%dfps # ", w, h, state_.target_fps);
    SetWindowTextW(prompt_, buf);

    // The prompt/input split used to assume a fixed
    // 160px-wide prompt (see OnSize()'s old hardcoded promptW). That's
    // wide enough for "1920x1080@60fps # ", but a higher-res monitor or a
    // high refresh-rate target ("3840x2160@144fps # ") renders noticeably
    // wider than 160px in Consolas, so the tail of the prompt text got
    // clipped and ran into the input box. Measure the real string with
    // the font that's actually applied and use that width instead, with
    // a sane floor/ceiling so it never disappears or eats the whole row.
    HDC dc = GetDC(hwnd_);
    HFONT oldFont = mono_font_ ? (HFONT)SelectObject(dc, mono_font_) : nullptr;
    SIZE extent{};
    GetTextExtentPoint32W(dc, buf, (int)wcslen(buf), &extent);
    if (oldFont) SelectObject(dc, oldFont);
    ReleaseDC(hwnd_, dc);

    prompt_width_ = std::clamp((int)extent.cx + 12, 120, 320);

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    OnSize(rc.right - rc.left, rc.bottom - rc.top);
}

void ConsoleWindow::OnCreate(HINSTANCE hInst) {
    // No WS_EX_CLIENTEDGE here - that's the Win32 sunken-3D-bevel look,
    // which is the opposite of what "make it look Linux-terminal-like"
    // means. Flat, flush-with-the-window edges instead.
    //
    // OPT/UI: plain EDIT controls can only show ONE text color for their
    // entire contents (WM_CTLCOLORSTATIC sets it once, globally), so
    // Ok/Info/Warn/Err lines used to differ only by a leading glyph, not
    // color - which is the opposite of what any real terminal (or
    // journalctl/git status, etc.) looks like. RichEdit is a standard,
    // already-on-every-Windows-install common control (Msftedit.dll,
    // shipped since XP) that supports per-run color via
    // EM_SETCHARFORMAT while remaining just as lightweight as EDIT for
    // this use case - it's still one native control doing its own
    // rendering, not a custom owner-draw box, so this adds real color
    // without adding any per-frame CPU cost. Falls back to plain EDIT
    // (still fully functional, just single-color) if the library can't
    // be loaded for some reason.
    static HMODULE richedit_lib = LoadLibraryW(L"Msftedit.dll");
    rich_edit_ = (richedit_lib != nullptr);
    const wchar_t *outputClass = rich_edit_ ? MSFTEDIT_CLASS : L"EDIT";
    DWORD outputStyle = WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL;
    output_ = CreateWindowExW(0, outputClass, L"",
                               outputStyle,
                               8, 8, 740, 380, hwnd_, (HMENU)IDC_CONSOLE_OUTPUT, hInst, nullptr);
    if (rich_edit_) {
        // RichEdit doesn't route through WM_CTLCOLOREDIT/STATIC at all -
        // background is set directly, once, via its own message.
        SendMessageW(output_, EM_SETBKGNDCOLOR, 0, (LPARAM)RGB(0, 0, 0));
        // Dark-mode scrollbar to match the black background - the default
        // light-gray Win32 scrollbar next to a black terminal box was one
        // of the more jarring "this looks bad" details. No-op on pre-1809
        // Windows builds, safe to call unconditionally.
        SetWindowTheme(output_, L"DarkMode_Explorer", nullptr);
    }

    HFONT monoFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
    mono_font_ = monoFont;
    SendMessageW(output_, WM_SETFONT, (WPARAM)monoFont, TRUE);

    // Static "1920x1080@60fps # " prompt, à la a shell prompt ending in a
    // root-style "#" - sits immediately left of the input box instead of
    // the input box just being a bare, unlabeled text field.
    prompt_ = CreateWindowExW(0, L"STATIC", L"",
                               WS_CHILD | WS_VISIBLE | SS_LEFT,
                               8, 416, 160, 24, hwnd_, nullptr, hInst, nullptr);
    SendMessageW(prompt_, WM_SETFONT, (WPARAM)monoFont, TRUE);

    input_ = CreateWindowExW(0, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                              172, 416, 576, 24, hwnd_, (HMENU)IDC_CONSOLE_INPUT, hInst, nullptr);
    SendMessageW(input_, WM_SETFONT, (WPARAM)monoFont, TRUE);

    RefreshPrompt();

    // Subclass the input box so Enter runs the command and Up/Down walk
    // history - done via a simple WNDPROC swap rather than a separate
    // subclass file, since it's the only control that needs it.
    SetWindowLongPtrW(input_, GWLP_USERDATA, (LONG_PTR)this);
    static WNDPROC origInputProc = nullptr;
    origInputProc = (WNDPROC)GetWindowLongPtrW(input_, GWLP_WNDPROC);
    SetWindowLongPtrW(input_, GWLP_WNDPROC, (LONG_PTR)(+[](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        auto *self = reinterpret_cast<ConsoleWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        // The console window is created as a child/owned window of
        // the app's wxWidgets main frame (main_window_ / GetHWND()), and
        // wxWidgets pre-processes keyboard messages for the whole app --
        // Enter/Tab/Esc are normally treated as dialog-navigation keys and
        // can be consumed before they ever reach a plain child control's
        // own WNDPROC, unless that control tells Windows it wants them
        // itself. Without this, pressing Enter here could silently do
        // nothing (swallowed upstream) while ordinary character typing
        // still worked, which is exactly the "can't run commands, Enter
        // does nothing" symptom. Returning DLGC_WANTALLKEYS here is the
        // standard fix for a raw Win32 edit control embedded in this kind
        // of host.
        if (msg == WM_GETDLGCODE) {
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;
        }
        if (msg == WM_KEYDOWN && self) {
            if (wParam == VK_RETURN) {
                wchar_t buf[1024] = {};
                GetWindowTextW(hwnd, buf, 1024);
                std::wstring cmd = Trim(buf);
                if (!cmd.empty()) {
                    self->history_.push_back(cmd);
                    self->history_pos_ = (int)self->history_.size();
                    wchar_t promptBuf[64] = {};
                    GetWindowTextW(self->prompt_, promptBuf, 64);
                    self->Print(std::wstring(promptBuf) + cmd);
                    self->RunCommand(cmd);
                }
                SetWindowTextW(hwnd, L"");
                return 0;
            } else if (wParam == VK_UP) {
                if (!self->history_.empty() && self->history_pos_ > 0) {
                    self->history_pos_--;
                    SetWindowTextW(hwnd, self->history_[(size_t)self->history_pos_].c_str());
                    SendMessageW(hwnd, EM_SETSEL, 0, -1);
                    SendMessageW(hwnd, EM_SETSEL, (WPARAM)-1, -1);
                }
                return 0;
            } else if (wParam == VK_DOWN) {
                if (!self->history_.empty() && self->history_pos_ < (int)self->history_.size() - 1) {
                    self->history_pos_++;
                    SetWindowTextW(hwnd, self->history_[(size_t)self->history_pos_].c_str());
                } else {
                    self->history_pos_ = (int)self->history_.size();
                    SetWindowTextW(hwnd, L"");
                }
                return 0;
            }
        }
        return CallWindowProcW(origInputProc, hwnd, msg, wParam, lParam);
    }));
}

void ConsoleWindow::OnSize(int w, int h) {
    if (output_) SetWindowPos(output_, nullptr, 8, 8, w - 16, h - 64, SWP_NOZORDER);
    int promptW = prompt_ ? prompt_width_ : 0;
    if (prompt_) SetWindowPos(prompt_, nullptr, 8, h - 40, promptW, 24, SWP_NOZORDER);
    if (input_) {
        int inputX = 8 + promptW + 4;
        int inputW = std::max(40, w - 20 - promptW);
        SetWindowPos(input_, nullptr, inputX, h - 40, inputW, 24, SWP_NOZORDER);
    }
}

void ConsoleWindow::OnCommand(int, int, HWND) {}

void ConsoleWindow::Print(const std::wstring &line, COLORREF color) {
    int len = GetWindowTextLengthW(output_);
    SendMessageW(output_, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    std::wstring toAppend = (len > 0 ? L"\r\n" : L"") + line;

    if (rich_edit_) {
        // Set the color that new typed/inserted text will use *before*
        // inserting it (EM_SETCHARFORMAT with SCF_SELECTION applies to
        // the zero-length selection at the caret we just set above, i.e.
        // "what comes next"), then insert. This is what actually makes
        // errors show red and warnings amber instead of the single
        // uniform green plain EDIT was stuck with.
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = color;
        SendMessageW(output_, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    }
    SendMessageW(output_, EM_REPLACESEL, FALSE, (LPARAM)toAppend.c_str());
}

void ConsoleWindow::RunCommand(const std::wstring &raw, bool confirmed) {
    std::wistringstream iss(raw);
    std::wstring cmd;
    iss >> cmd;
    if (cmd.empty()) return;

    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::towlower);

    // Alias expansion (simple one-level substitution of the command word).
    auto aliasIt = aliases_.find(cmd);
    if (aliasIt != aliases_.end()) cmd = aliasIt->second;

    // "inwid <command...>" - the confirmation prefix (see the class
    // comment in console_window.h). Only actually consumed as a prefix
    // when what follows is something CommandNeedsInwid() (or a settings
    // assignment) would otherwise refuse - "inwid status" and "inwid
    // settings ..." fall straight through to the `cmd == "inwid"` case
    // below unchanged, which is whatever a loaded plugin registered
    // under the literal name "inwid" (bter's own admin prefix, see
    // Hom/plugins/bter-src/entry.lua) - this keeps that pre-existing
    // usage working exactly as before.
    if (!confirmed && cmd == L"inwid") {
        size_t sp = raw.find(L' ');
        std::wstring rest = (sp == std::wstring::npos) ? L"" : Trim(raw.substr(sp + 1));
        if (!rest.empty()) {
            std::wistringstream riss(rest);
            std::wstring rcmd;
            riss >> rcmd;
            std::transform(rcmd.begin(), rcmd.end(), rcmd.begin(), ::towlower);
            auto rAliasIt = aliases_.find(rcmd);
            if (rAliasIt != aliases_.end()) rcmd = rAliasIt->second;

            bool settingAssignment = !ExtractSettingValue(rest).empty() &&
                                      HrSettingsRegistry::Find(NarrowFromWide(rcmd)) != nullptr;

            if (CommandNeedsInwid(rcmd, rest) || settingAssignment) {
                RunCommand(rest, /*confirmed=*/true);
                return;
            }
        }
        // Bare "inwid", "inwid status", "inwid settings ...", or
        // anything else that isn't a gated command - fall through below
        // and dispatch the *original* line starting with "inwid" as an
        // ordinary command word.
    }

    if (cmd == L"version") CmdVersion(raw);
    else if (cmd == L"ver") CmdVer(raw);
    else if (cmd == L"ping") CmdPing(raw);
    else if (cmd == L"echo") CmdEcho(raw);
    else if (cmd == L"clear") CmdClear(raw);
    else if (cmd == L"env") CmdEnv(raw);
    else if (cmd == L"alias") CmdAlias(raw);
    else if (cmd == L"history") CmdHistory(raw);
    else if (cmd == L"info") CmdInfo(raw);
    else if (cmd == L"status") CmdStatus(raw);
    else if (cmd == L"log") CmdLog(raw);
    else if (cmd == L"hide") CmdHide(raw);
    else if (cmd == L"sec") CmdSec(raw);
    else if (cmd == L"secui") CmdSecUi(raw);
    else if (cmd == L"secp") CmdSecP(raw);
    else if (cmd == L"hrc") CmdHrc(raw);
    else if (cmd == L"sethrc") CmdSetHrc(raw);
    else if (cmd == L"clip") CmdClip(raw);
    else if (cmd == L"repeat") CmdRepeat(raw);
    else if (cmd == L"batch") CmdBatch(raw);
    else if (cmd == L"ls") CmdLs(raw);
    else if (cmd == L"hom") {
        if (!confirmed && CommandNeedsInwid(cmd, raw)) { RefuseNeedsInwid(raw); return; }
        CmdHom(raw);
    } else if (cmd == L"rm") {
        if (!confirmed && CommandNeedsInwid(cmd, raw)) { RefuseNeedsInwid(raw); return; }
        // Route the two ported rm forms; anything else under rm (--ui,
        // @ts, bare rm_vid, etc.) isn't implemented yet.
        if (raw.find(L"--system@homrec.files") != std::wstring::npos) CmdRmSystemFiles(raw);
        else if (raw.find(L"@homrec") != std::wstring::npos) CmdRmSelfApp(raw);
        else PrintWarn(L"rm: this form isn't supported yet.");
    } else {
        // Not a built-in - see if a loaded plugin registered this name via
        // homrec.register_command() before giving up on it.
        std::vector<std::string> plugin_cmd_lines;
        if (plugins_ && plugins_->DispatchCommand(NarrowFromWide(cmd), NarrowFromWide(raw), plugin_cmd_lines)) {
            for (const auto &line : plugin_cmd_lines) PrintInfo(WideFromNarrow(line));
        } else if (TryRunSetting(cmd, raw, confirmed)) {
            // Handled - TryRunSetting() already printed its own output.
        } else {
            PrintWarn(L"Unknown command: " + cmd);
        }
    }
}

void ConsoleWindow::RefuseNeedsInwid(const std::wstring &raw) {
    PrintWarn(L"blocked - needs the \"inwid\" confirmation prefix (I Know What I'm Doing). Run: inwid " + raw);
}

bool ConsoleWindow::TryRunSetting(const std::wstring &cmd, const std::wstring &raw, bool confirmed) {
    std::string key = NarrowFromWide(cmd);
    std::wstring wvalue = ExtractSettingValue(raw);
    std::string value = NarrowFromWide(wvalue);

    if (const auto *def = HrSettingsRegistry::Find(key)) {
        // custom_ffmpeg_args is the one sensitive field (see
        // hr_settings_registry.cpp) - gated behind the same "sec" fuse
        // CmdSetHrc()'s import of it above already respects, so an
        // unattended cfg file (or a console line typed by someone who
        // isn't you) can't quietly rewrite ffmpeg's real command line.
        if (def->sensitive && !CoreUnlocked()) {
            PrintWarn(WideFromNarrow(def->key) +
                      L": blocked - protected by core security (run \"sec 0\" first if you trust this).");
            return true;
        }
        if (value.empty()) {
            // Bare "<setting>", no value - query, same convention env/
            // alias/sec/secui/secp already use for "show current state".
            // Never gated - reading a value back isn't the kind of thing
            // "inwid" needs to be involved in.
            PrintInfo(WideFromNarrow(def->key) + L" = " + WideFromNarrow(def->get(state_)));
            return true;
        }
        if (!confirmed) {
            PrintWarn(WideFromNarrow(def->key) +
                      L": blocked - setting a value needs the \"inwid\" confirmation prefix. Run: inwid " + raw);
            return true;
        }
        if (!def->set(state_, value)) {
            PrintWarn(WideFromNarrow(def->key) + L": invalid value '" + wvalue + L"'");
            return true;
        }
        // Same persistence behavior as homrec.set_setting() (lua_api.cpp) -
        // a setting assignment is meant to feel identical whether it came
        // from a plugin, the console, or a cfg file (autoexec/config/
        // startrec - see RunCfgFile() above, same RunCommand() either way).
        std::wstring target = HrcConfig::ResolveSettingsPath(state_);
        HrcConfig::Save(state_, target);
        if (target != HrcConfig::kDefaultSettingsPath) HrcConfig::Save(state_, HrcConfig::kDefaultSettingsPath);
        PrintOk(WideFromNarrow(def->key) + L" = " + wvalue);
        return true;
    }

    // Not a built-in setting either - last stop before "Unknown command":
    // a plugin's own homrec.register_setting(). NOTE: not currently
    // gated by "inwid" - DispatchSetting() doesn't expose a way to check
    // "is this actually a known plugin setting" separately from doing
    // the query/set itself, so gating it here would mean guessing at a
    // key that might not even exist. Known limitation - see commands.md.
    if (plugins_) {
        std::vector<std::string> lines;
        if (plugins_->DispatchSetting(key, value, lines)) {
            for (const auto &line : lines) PrintInfo(WideFromNarrow(line));
            return true;
        }
    }
    return false;
}

int ConsoleWindow::RunCfgFile(const std::wstring &name) {
    std::wstring cfg_dir = GetBaseDir() + L"\\cfg";
    // Auto-create cfg/ (harmless no-op if it already exists) purely for
    // discoverability - so there's an obvious, visible folder right next
    // to the exe for someone to drop autoexec.cfg into, the same way
    // LuaPluginEngine's constructor auto-creates plugins/ for the same
    // reason. Doesn't affect whether the file itself is found below.
    CreateDirectoryW(cfg_dir.c_str(), nullptr);

    std::wstring path = cfg_dir + L"\\" + name + L".cfg";
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return 0;  // not present -- opt-in feature, not an error

    // Lenient decode, same reasoning/fallback as log_viewer_dialog.cpp's
    // ReadLogFileLenient(): a .cfg file is something a person hand-edits
    // in whatever text editor they've got, which on Windows still quite
    // often means "ANSI", not UTF-8.
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return 0;

    std::wstring text;
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (needed <= 0) needed = MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (needed <= 0) {
        PrintErr(L"cfg/" + name + L".cfg: unable to decode file, skipped.");
        return 0;
    }
    text.resize(needed);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), text.data(), needed) <= 0) {
        MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), text.data(), needed);
    }

    int ran = 0;
    std::wistringstream stream(text);
    std::wstring line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();  // CRLF
        std::wstring trimmed = Trim(line);
        if (trimmed.empty()) continue;
        // "//" (Source/Quake-style cfg convention) and "#" (real Linux
        // shell script comment convention) both work as comment markers,
        // so people can use whichever they're already used to.
        if (trimmed.compare(0, 2, L"//") == 0 || trimmed[0] == L'#') continue;

        // confirmed=true: a cfg file already sitting on disk in this
        // install (autoexec/config/startrec, hand-placed by whoever
        // owns this HomRec folder) is trusted local content, not a
        // one-off interactively-typed line - same reasoning .bashrc
        // doesn't re-prompt for sudo on every line it runs. Without
        // this, "disable_preview = true" (or any other setting) in an
        // existing config.cfg would silently stop applying the moment
        // the "inwid" gate shipped.
        RunCommand(trimmed, /*confirmed=*/true);
        ++ran;
    }

    PrintInfo(L"cfg/" + name + L".cfg: ran " + std::to_wstring(ran) +
              (ran == 1 ? L" command." : L" commands."));
    return ran;
}

// --- commands ---------------------------------------------------------------

void ConsoleWindow::CmdVersion(const std::wstring &) {
    PrintInfo(L"HomRec v" HR_APP_VERSION_W L" (developer console)");
}

// Bare version number, no "HomRec v"/"(developer console)" decoration -
// for scripting/copy-pasting the number itself (e.g. a batch.cfg line
// that wants to log just "2.0" somewhere) without having to parse it back
// out of CmdVersion()'s human-readable line above.
void ConsoleWindow::CmdVer(const std::wstring &) {
    PrintInfo(HR_APP_VERSION_W);
}

void ConsoleWindow::CmdPing(const std::wstring &) {
    PrintOk(L"pong");
}

void ConsoleWindow::CmdEcho(const std::wstring &raw) {
    size_t sp = raw.find(L' ');
    std::wstring rest = sp == std::wstring::npos ? L"" : Trim(raw.substr(sp + 1));
    // Strip a leading --ok/--warn/--err flag (in any position at the start)
    // and print with the matching icon, matching commands.md's documented
    // `echo [--ok|--warn|--err] <text>` form.
    auto strip = [&](const std::wstring &flag) -> bool {
        if (rest.compare(0, flag.size(), flag) == 0 &&
            (rest.size() == flag.size() || rest[flag.size()] == L' ')) {
            rest = Trim(rest.substr(flag.size()));
            return true;
        }
        return false;
    };
    if (strip(L"--ok"))        PrintOk(rest);
    else if (strip(L"--warn")) PrintWarn(rest);
    else if (strip(L"--err"))  PrintErr(rest);
    else Print(rest);
}

void ConsoleWindow::CmdClear(const std::wstring &) {
    SetWindowTextW(output_, L"");
}

void ConsoleWindow::CmdEnv(const std::wstring &raw) {
    std::wistringstream iss(raw);
    std::wstring cmd, rest;
    iss >> cmd;
    std::getline(iss, rest);
    rest = Trim(rest);
    if (rest.empty()) {
        if (env_vars_.empty()) { PrintInfo(L"(no session env vars set)"); return; }
        for (const auto &kv : env_vars_) PrintInfo(kv.first + L"=" + kv.second);
        return;
    }
    size_t eq = rest.find(L'=');
    if (eq == std::wstring::npos) {
        auto it = env_vars_.find(rest);
        PrintInfo(rest + L"=" + (it != env_vars_.end() ? it->second : L"(unset)"));
    } else {
        std::wstring key = rest.substr(0, eq), val = rest.substr(eq + 1);
        env_vars_[key] = val;
        PrintOk(L"set " + key + L"=" + val);
    }
}

void ConsoleWindow::CmdAlias(const std::wstring &raw) {
    std::wistringstream iss(raw);
    std::wstring cmd, rest;
    iss >> cmd;
    std::getline(iss, rest);
    rest = Trim(rest);
    if (rest.empty()) {
        if (aliases_.empty()) { PrintInfo(L"(no aliases defined)"); return; }
        for (const auto &kv : aliases_) PrintInfo(kv.first + L" -> " + kv.second);
        return;
    }
    size_t eq = rest.find(L'=');
    if (eq == std::wstring::npos) { PrintWarn(L"alias: usage is `alias name=target`"); return; }
    std::wstring name = Trim(rest.substr(0, eq)), target = Trim(rest.substr(eq + 1));
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    aliases_[name] = target;
    PrintOk(L"alias " + name + L" -> " + target);
}

void ConsoleWindow::CmdHistory(const std::wstring &) {
    if (history_.empty()) { PrintInfo(L"(no history yet)"); return; }
    for (size_t i = 0; i < history_.size(); ++i) {
        Print(std::to_wstring(i + 1) + L"  " + history_[i]);
    }
}

void ConsoleWindow::CmdInfo(const std::wstring &) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    PrintInfo(L"CPU cores: " + std::to_wstring(si.dwNumberOfProcessors));
    PrintInfo(L"RAM: " + std::to_wstring(ms.ullTotalPhys / (1024 * 1024)) + L" MB total, " +
              std::to_wstring(ms.dwMemoryLoad) + L"% used");
    PrintInfo(L"FFmpeg: " + std::wstring(rec_ && rec_->ffmpeg_found() ? L"found" : L"NOT found"));
    // The single biggest lever on recording CPU load: whether a GPU
    // encoder (h264_qsv/nvenc/amf) actually got picked up at startup, or
    // it's silently falling back to software libx264 - same info that's
    // already in homrec.log's startup lines, just somewhere a user
    // actually looking for "why is this eating my CPU" would check first.
    if (rec_) {
        const std::wstring &hw = rec_->resolved_hw_encoder();
        PrintInfo(hw.empty()
            ? L"Encoder: software (libx264) - no GPU encoder found. Heaviest on CPU; "
              L"see homrec.log's startup lines for why the GPU probe failed."
            : L"Encoder: hardware (" + hw + L") - GPU-accelerated, light on CPU.");
    }
}

void ConsoleWindow::CmdStatus(const std::wstring &) {
    if (!rec_) { PrintInfo(L"status: n/a"); return; }
    if (state_.recording) {
        PrintInfo((state_.paused ? L"PAUSED" : L"RECORDING") + std::wstring(L" - ") +
                  rec_->elapsed_formatted() + L", frame " + std::to_wstring(rec_->frame_count()));
    } else {
        PrintInfo(L"idle");
    }
}

void ConsoleWindow::CmdLog(const std::wstring &raw) {
    std::wstring log_path = HrLogPaths::LogFilePath(L"homrec.log");

    size_t sp = raw.find(L' ');
    std::wstring rest = sp == std::wstring::npos ? L"" : Trim(raw.substr(sp + 1));
    std::wstring first_word = rest;
    size_t sp2 = rest.find(L' ');
    if (sp2 != std::wstring::npos) first_word = rest.substr(0, sp2);

    if (first_word == L"clear") {
        // Truncate rather than delete -- matches rm's convention of being
        // the only command family that actually removes files; "clear"
        // reads as "empty it out", not "get rid of the log file itself".
        std::wofstream f(log_path.c_str(), std::ios::trunc);
        if (f) PrintOk(L"log cleared (" + log_path + L")");
        else   PrintErr(L"couldn't open " + log_path + L" for writing");
        return;
    }
    if (first_word == L"open") {
        HINSTANCE r = ShellExecuteW(main_window_, L"open", log_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)r > 32) PrintOk(L"opened " + log_path);
        else PrintErr(L"couldn't open " + log_path + L" (no default text editor associated with .log?)");
        return;
    }

    // Anything else: the original behaviour, log `rest` as a message.
    std::wofstream f(log_path.c_str(), std::ios::app);
    f << L"[console] " << rest << L"\n";
    PrintOk(L"logged");
}

void ConsoleWindow::CmdHide(const std::wstring &) {
    if (main_window_) ShowWindow(main_window_, SW_HIDE);
    PrintOk(L"main window hidden - use the tray icon to restore it");
}

void ConsoleWindow::CmdHrc(const std::wstring &raw) {
    std::wistringstream iss(raw);
    std::wstring cmd, sub, path;
    iss >> cmd >> sub;
    std::getline(iss, path);
    path = Trim(path);
    if (path.empty()) path = GetBaseDir() + L"\\homrec_config.hrc";

    if (sub == L"save") {
        if (HrcConfig::Save(state_, path)) PrintOk(L"saved settings to " + path);
        else PrintErr(L"couldn't write " + path);
    } else if (sub == L"load") {
        if (HrcConfig::Load(state_, path)) PrintOk(L"loaded settings from " + path + L" (restart may be needed for some fields to take effect)");
        else PrintErr(L"couldn't read " + path);
    } else {
        PrintWarn(L"usage: hrc save [path] | hrc load [path]  (default path: homrec_config.hrc next to the exe)");
    }
}

void ConsoleWindow::CmdSetHrc(const std::wstring &raw) {
    std::wistringstream iss(raw);
    std::wstring cmd, path, flag;
    iss >> cmd >> path >> flag;

    if (path.empty()) {
        PrintWarn(L"usage: sethrc <path> <1|true|0|false>  (e.g. \"sethrc homrec.hrc 1\" as the "
                   L"first line of cfg/config.cfg to seed it from an .hrc profile - see commands.md)");
        return;
    }
    // Match hrc_config.cpp's own ToBool(): "1"/"true"/"yes" are the only
    // truthy spellings, anything else (including a missing flag) is a
    // no-op read - lets a cfg author write "sethrc homrec.hrc 0" to
    // document *where* a baseline would come from without applying it yet.
    bool enable = (flag == L"1" || flag == L"true" || flag == L"yes");
    if (!enable) {
        PrintInfo(L"sethrc " + path + L": flag not set (1/true) - nothing imported");
        return;
    }

    if (HrcConfig::Load(state_, path, /*allow_sensitive_fields=*/!sec_core_)) {
        PrintOk(L"sethrc: merged settings from " + path + L" into the running config");
        if (sec_core_) {
            // Only worth a note if the file actually had one to skip -
            // otherwise every "sethrc" call prints a warning nobody asked
            // about (see hrc_config.cpp's own comment on this parameter).
            std::wifstream probe(path.c_str());
            std::wstring line;
            bool file_sets_ffmpeg_args = false;
            while (std::getline(probe, line)) {
                if (line.rfind(L"custom_ffmpeg_args=", 0) == 0) { file_sets_ffmpeg_args = true; break; }
            }
            if (file_sets_ffmpeg_args)
                PrintWarn(L"custom_ffmpeg_args in " + path + L" was NOT imported (sec fuse is on - "
                           L"run \"sec 0\" first if this file is trusted)");
        }
    } else {
        PrintErr(L"sethrc: couldn't read " + path);
    }
}

void ConsoleWindow::CmdClip(const std::wstring &raw) {
    std::wistringstream iss(raw);
    std::wstring cmd, sub;
    iss >> cmd >> sub;

    if (sub == L"--copy") {
        std::wstring rest;
        std::getline(iss, rest);
        rest = Trim(rest);
        // Strip one layer of surrounding quotes, matching how the other
        // commands here treat a quoted text argument.
        if (rest.size() >= 2 && rest.front() == L'"' && rest.back() == L'"')
            rest = rest.substr(1, rest.size() - 2);
        if (!OpenClipboard(hwnd_)) { PrintErr(L"couldn't open the clipboard"); return; }
        EmptyClipboard();
        size_t bytes = (rest.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hMem) {
            memcpy(GlobalLock(hMem), rest.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
        CloseClipboard();
        PrintOk(L"copied to clipboard");
    } else if (sub == L"--paste") {
        if (!OpenClipboard(hwnd_)) { PrintErr(L"couldn't open the clipboard"); return; }
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            wchar_t *text = static_cast<wchar_t *>(GlobalLock(hData));
            if (text) { Print(text); GlobalUnlock(hData); }
        } else {
            PrintInfo(L"(clipboard has no text)");
        }
        CloseClipboard();
    } else if (sub == L"--clear") {
        if (!OpenClipboard(hwnd_)) { PrintErr(L"couldn't open the clipboard"); return; }
        EmptyClipboard();
        CloseClipboard();
        PrintOk(L"clipboard cleared");
    } else {
        PrintWarn(L"usage: clip --copy \"text\" | clip --paste | clip --clear");
    }
}

void ConsoleWindow::CmdRepeat(const std::wstring &raw) {
    std::wstring count_str = ConsoleParse::ParseNamed(raw, L"count");
    std::wistringstream iss(raw);
    std::wstring cmd;
    iss >> cmd;
    std::wstring rest;
    std::getline(iss, rest);
    rest = Trim(rest);
    int count = count_str.empty() ? 0 : _wtoi(count_str.c_str());
    // Strip the "--count=N" token itself out of the command to run.
    size_t hashPos = rest.find(L"--count=");
    if (hashPos != std::wstring::npos) {
        size_t end = rest.find(L' ', hashPos);
        rest = Trim(rest.substr(0, hashPos) + (end == std::wstring::npos ? L"" : rest.substr(end)));
    }
    if (count <= 0 || rest.empty()) {
        PrintWarn(L"usage: repeat --count=N <command>");
        return;
    }
    if (count > 1000) {
        PrintWarn(L"repeat: capped at 1000 iterations (asked for " + std::to_wstring(count) + L")");
        count = 1000;
    }
    for (int i = 0; i < count; ++i) RunCommand(rest);
}

void ConsoleWindow::CmdBatch(const std::wstring &raw) {
    size_t sp = raw.find(L' ');
    std::wstring rest = sp == std::wstring::npos ? L"" : raw.substr(sp + 1);
    bool stop_on_error = false;
    for (auto flag : {std::wstring(L" -x"), std::wstring(L" --stop-on-error")}) {
        size_t pos = rest.find(flag);
        if (pos != std::wstring::npos) { rest.erase(pos, flag.size()); stop_on_error = true; }
    }
    // Split on "&&", same separator commands.md documents for batch.
    std::vector<std::wstring> steps;
    size_t start = 0;
    while (true) {
        size_t pos = rest.find(L"&&", start);
        std::wstring step = Trim(pos == std::wstring::npos ? rest.substr(start) : rest.substr(start, pos - start));
        if (!step.empty()) steps.push_back(step);
        if (pos == std::wstring::npos) break;
        start = pos + 2;
    }
    if (steps.empty()) { PrintWarn(L"usage: batch <cmd1> && <cmd2> && ... [-x]"); return; }
    for (const auto &step : steps) {
        RunCommand(step);
        // NOTE: stop_on_error is honored only loosely here - most Cmd*
        // handlers print their own error but don't yet return a pass/fail
        // status RunCommand could check. Wired up so -x is at least not
        // silently ignored once that plumbing exists.
        (void)stop_on_error;
    }
}

void ConsoleWindow::CmdLs(const std::wstring &raw) {
    // Windows/rules/AE objects aren't ported (they'd need the registry
    // subsystem described at the top of console_window.h) - this lists
    // the parts of "the registry" that DO exist natively in this port:
    // aliases and session env vars.
    bool wantAliases = raw.find(L"--aliases") != std::wstring::npos;
    bool wantEnv = raw.find(L"--env") != std::wstring::npos;
    bool showAll = !wantAliases && !wantEnv;

    if (showAll || wantAliases) {
        PrintInfo(L"-- aliases (" + std::to_wstring(aliases_.size()) + L") --");
        for (const auto &kv : aliases_) PrintInfo(L"  " + kv.first + L" -> " + kv.second);
    }
    if (showAll || wantEnv) {
        PrintInfo(L"-- env vars (" + std::to_wstring(env_vars_.size()) + L") --");
        for (const auto &kv : env_vars_) PrintInfo(L"  " + kv.first + L"=" + kv.second);
    }
    if (showAll) {
        PrintInfo(L"(windows/rules/ae objects aren't in this native port yet)");
    }
}

// `hom` -- previously only usable from a separate PowerShell/cmd window
// (see tools/hom/README.md); this runs the exact same hom.exe as a child
// process so it works from the in-app console too, without duplicating
// hom's own WinHTTP/install/remove logic here.
//
// Runs on hom_thread_, not this function's own (UI) thread - see
// hom_thread_'s declaration in console_window.h for why a synchronous
// RunCapturedProcess() call here used to freeze the whole app for up to
// 30s on any slow/stalled network request, not just the console window.
void ConsoleWindow::JoinPendingHomThread() {
    if (hom_thread_.joinable()) hom_thread_.join();
}

void ConsoleWindow::CmdHom(const std::wstring &raw) {
    // Strip the leading "hom" token itself; everything after it is
    // forwarded to hom.exe verbatim ("hom install input-overlay" ->
    // argv = {"install", "input-overlay"}, same as typing it in a shell).
    std::wstring args;
    size_t sp = raw.find(L' ');
    if (sp != std::wstring::npos) args = Trim(raw.substr(sp + 1));

    std::wstring base_dir = GetBaseDir();
    std::wstring hom_path = base_dir + L"\\hom.exe";
    if (!FileExists(hom_path)) {
        PrintErr(L"hom: hom.exe not found in " + base_dir);
        PrintInfo(L"  build it with `make hom` from the repo root, or copy hom.exe "
                  L"from the Hom\\ folder, then place it next to hr.exe.");
        return;
    }

    std::wstring cmdline = L"\"" + hom_path + L"\"" + (args.empty() ? L"" : L" " + args);

    // A previous hom_thread_ is only still joinable() here if its
    // kWmHomDone message hasn't been pumped yet (it's already finished
    // running by the time it posts that message) - join is a formality,
    // not a real wait, and guards against starting a second thread while
    // one is (nominally) still attached.
    JoinPendingHomThread();

    PrintInfo(L"hom: running " + (args.empty() ? std::wstring(L"hom") : L"hom " + args) + L"...");

    HWND self_hwnd = hwnd_;
    hom_thread_ = std::thread([this, self_hwnd, cmdline, base_dir, hom_path]() {
        // hom does real network I/O for update/ping/install/remove (all
        // of hom's own commands hit raw.githubusercontent.com), so give
        // it much more room than the few-second timeouts used elsewhere
        // in this file for local, non-networked work - 30s is generous
        // for a single plugin download without hanging forever if the
        // repo is unreachable.
        //
        // Run with cwd = base_dir so "hom install <name>" writes to
        // <base_dir>\plugins\, the same folder HomRec's own plugin
        // loader reads from - matching "run hom from the same folder as
        // hr.exe" in tools/hom/README.md, regardless of what HomRec's
        // own process cwd happens to be.
        HomResult res;
        res.hom_path = hom_path;
        res.started  = RunCapturedProcess(cmdline, base_dir, 30000, &res.output, &res.code);
        {
            std::lock_guard<std::mutex> lk(hom_result_mtx_);
            hom_result_ = std::move(res);
        }
        // self_hwnd, not hwnd_ - hwnd_ itself is only ever touched on the
        // UI thread, but this background thread reading it directly
        // right as the window might be closing/destroyed would still be
        // a race. The value was captured up front instead, before
        // JoinPendingHomThread() in the destructor can run.
        if (self_hwnd) PostMessageW(self_hwnd, kWmHomDone, 0, 0);
    });
}

void ConsoleWindow::CmdSec(const std::wstring &raw) {
    std::wistringstream iss(raw);
    std::wstring cmd, val;
    iss >> cmd >> val;
    if (val.empty()) { PrintInfo(sec_core_ ? L"1 (protected)" : L"0 (ALL protections disabled)"); return; }
    sec_core_ = !(val == L"0" || val == L"off" || val == L"false");
    PrintWarn(L"sec " + val + L": MASTER fuse " + (sec_core_ ? L"ENABLED" : L"DISABLED (everything unlocked)"));
}

void ConsoleWindow::CmdSecUi(const std::wstring &raw) {
    std::wistringstream iss(raw);
    std::wstring cmd, val;
    iss >> cmd >> val;
    if (val.empty()) { PrintInfo(sec_ui_ ? L"1 (protected)" : L"0 (UI protection disabled)"); return; }
    sec_ui_ = !(val == L"0" || val == L"off" || val == L"false");
    PrintWarn(L"secui " + val + L": UI protection " + (sec_ui_ ? L"ENABLED" : L"DISABLED"));
}

void ConsoleWindow::CmdSecP(const std::wstring &raw) {
    std::wistringstream iss(raw);
    std::wstring cmd, val;
    iss >> cmd >> val;
    if (val.empty()) { PrintInfo(sec_plugin_ ? L"1 (protected)" : L"0 (plugin checks disabled)"); return; }
    sec_plugin_ = !(val == L"0" || val == L"off" || val == L"false");
    PrintWarn(L"secp " + val + L": plugin version-check / RAM watchdog " + (sec_plugin_ ? L"ENABLED" : L"DISABLED"));
}

void ConsoleWindow::CmdRmSystemFiles(const std::wstring &raw) {
    std::wstring perm = ConsoleParse::ParseNamed(raw, L"permission");
    if (perm != L"core") { PrintWarn(L"rm --system@homrec.files: requires --permission=core"); return; }
    if (!CoreUnlocked()) {
        PrintWarn(L"rm --system@homrec.files: blocked - core protection is ON. Run `sec 0` first.");
        return;
    }
    // -r required on top of --permission=core + sec 0: this deletes
    // recordings/plugins/logs/cache wholesale, so (like `rm -r`) it
    // needs an explicit "yes, recursively" rather than firing off of
    // --type={...} alone.
    if (!ConsoleParse::ParseFlags(raw).count(L"-r")) {
        PrintWarn(L"rm --system@homrec.files: requires -r to confirm (e.g. `inwid rm --system@homrec.files --permission=core --type={...} -r`).");
        return;
    }

    size_t typePos = raw.find(L"--type={");
    if (typePos == std::wstring::npos) { PrintWarn(L"rm --system@homrec.files: --type={...} not specified"); return; }
    size_t typeEnd = raw.find(L'}', typePos);
    if (typeEnd == std::wstring::npos) { PrintWarn(L"rm --system@homrec.files: malformed --type={...}"); return; }
    std::wstring typesRaw = raw.substr(typePos + 8, typeEnd - typePos - 8);

    std::vector<std::wstring> types;
    std::wistringstream tss(typesRaw);
    std::wstring t;
    while (std::getline(tss, t, L',')) {
        t = Trim(t);
        if (!t.empty()) types.push_back(t);
    }

    std::wstring base = GetBaseDir();
    std::vector<std::wstring> cleared;
    for (const auto &type : types) {
        if (type == L"recordings") {
            std::wstring p = base + L"\\recordings";
            if (DirExists(p) && RemoveDirRecursive(p)) cleared.push_back(type);
        } else if (type == L"plugins") {
            std::wstring p = base + L"\\plugins";
            if (DirExists(p) && RemoveDirRecursive(p)) cleared.push_back(type);
        } else if (type == L"logs") {
            std::wstring p = HrLogPaths::LogsDir();
            if (DirExists(p) && RemoveDirRecursive(p)) cleared.push_back(type);
        } else if (type == L"cache") {
            wchar_t tempPath[MAX_PATH];
            GetTempPathW(MAX_PATH, tempPath);
            std::wstring createDir = base + L"\\create";
            std::wstring pluginTemp = std::wstring(tempPath) + L"homrec_plugins";
            if (DirExists(createDir)) RemoveDirRecursive(createDir);
            if (DirExists(pluginTemp)) RemoveDirRecursive(pluginTemp);
            cleared.push_back(type);
        } else {
            PrintWarn(L"rm --system@homrec.files: unknown --type entry '" + type + L"'");
        }
    }

    std::wstring clearedStr;
    for (size_t i = 0; i < cleared.size(); ++i) clearedStr += (i ? L", " : L"") + cleared[i];
    PrintWarn(L"rm --system@homrec.files: done. Cleared: " + (clearedStr.empty() ? L"(nothing found)" : clearedStr));
}

void ConsoleWindow::CmdRmSelfApp(const std::wstring &raw) {
    if (!CoreUnlocked()) {
        PrintWarn(L"rm @homrec: blocked - core protection is ON. Run `sec 0` first.");
        return;
    }
    auto flags = ConsoleParse::ParseFlags(raw);
    // -r required on top of sec 0: this uninstalls HomRec entirely, so
    // (like `rm -r`) it needs an explicit "yes, recursively", same as
    // rm --system@homrec.files above - -y/-q alone (see below) only
    // skips the interactive MessageBox, it doesn't mean "and yes I want
    // to delete everything" on its own.
    if (!flags.count(L"-r")) {
        PrintWarn(L"rm @homrec: requires -r to confirm (e.g. `inwid rm @homrec -r -y`).");
        return;
    }
    bool quiet = flags.count(L"-q") || flags.count(L"-y");

    if (!quiet) {
        int result = MessageBoxW(hwnd_,
            L"This will permanently uninstall HomRec from this computer once the app closes. "
            L"This cannot be undone.\n\nAre you sure you want to continue?",
            L"Uninstall HomRec", MB_YESNO | MB_ICONWARNING);
        if (result != IDYES) {
            PrintInfo(L"rm @homrec: cancelled");
            return;
        }
    }

    PrintWarn(L"rm @homrec: HomRec will delete itself once this process exits.");
    ScheduleSelfDelete(GetBaseDir());

    if (main_window_) {
        PostMessageW(main_window_, WM_CLOSE, 0, 0);
    }
}

void ConsoleWindow::ScheduleSelfDelete(const std::wstring &base) {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring batPath = std::wstring(tempPath) + L"homrec_uninstall.bat";

    // NOTE: the tasklist check matches the process name "HomRec.exe" -- if
    // you're running this straight from a debugger/different exe name, it
    // won't detect exit correctly and the loop will spin until manually
    // killed.
    std::wofstream f(batPath.c_str(), std::ios::trunc);
    f << L"@echo off\r\n"
      << L":wait_loop\r\n"
      << L"tasklist | findstr /i \"HomRec\" >nul 2>&1\r\n"
      << L"if not errorlevel 1 (\r\n"
      << L"  timeout /t 1 /nobreak >nul\r\n"
      << L"  goto wait_loop\r\n"
      << L")\r\n"
      << L"rmdir /s /q \"" << base << L"\"\r\n"
      << L"(goto) 2>nul & del \"%~f0\"\r\n";
    f.close();

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::wstring cmdLine = L"cmd /c start \"\" /min \"" + batPath + L"\"";
    std::vector<wchar_t> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back(L'\0');
    if (CreateProcessW(nullptr, cmdLineBuf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        PrintOk(L"rm @homrec: uninstall script scheduled at " + batPath);
    } else {
        PrintErr(L"rm @homrec: failed to schedule self-delete (CreateProcess failed)");
    }
}
