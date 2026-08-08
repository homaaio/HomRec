#include "console_window.h"
#include "version.h"
#include "recording_controller.h"
#include "win32_theme.h"
#include "hrc_config.h"
#include "../plugins/lua_engine.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>
#include <cmath>
#include <cwchar>

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

enum { IDC_CONSOLE_INPUT = 9001, IDC_CONSOLE_OUTPUT };

} // namespace

// ---------------------------------------------------------------------------
// Parsing helpers - direct ports of _parse_named / _parse_flags.
// ---------------------------------------------------------------------------

namespace ConsoleParse {

std::wstring ParseNamed(const std::wstring &raw, const std::wstring &key) {
    std::wstring needleQ = L"#" + key + L"=\"";
    size_t pos = raw.find(needleQ);
    if (pos != std::wstring::npos) {
        size_t start = pos + needleQ.size();
        size_t end = raw.find(L'"', start);
        if (end != std::wstring::npos) return raw.substr(start, end - start);
    }
    std::wstring needleApos = L"#" + key + L"='";
    pos = raw.find(needleApos);
    if (pos != std::wstring::npos) {
        size_t start = pos + needleApos.size();
        size_t end = raw.find(L'\'', start);
        if (end != std::wstring::npos) return raw.substr(start, end - start);
    }
    std::wstring needle = L"#" + key + L"=";
    pos = raw.find(needle);
    if (pos != std::wstring::npos) {
        size_t start = pos + needle.size();
        size_t end = start;
        while (end < raw.size() && raw[end] != L' ' && raw[end] != L'\t' &&
               raw[end] != L'#' && raw[end] != L'"' && raw[end] != L'\'') {
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
            SetTextColor(hdc, (HWND)lParam == prompt_ ? RGB(120, 220, 255) : RGB(80, 220, 120));
            return (LRESULT)blackBrush;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            static HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
            SetBkColor(hdc, RGB(0, 0, 0));
            SetTextColor(hdc, RGB(80, 220, 120));
            return (LRESULT)blackBrush;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void ConsoleWindow::Show(HINSTANCE hInst) {
    if (!hwnd_) {
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
        HrWin32Theme::ApplyDarkTitleBar(hwnd_);
        OnCreate(hInst);
    }
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

    // BUGFIX (design): the prompt/input split used to assume a fixed
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
    output_ = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                               8, 8, 740, 380, hwnd_, (HMENU)IDC_CONSOLE_OUTPUT, hInst, nullptr);
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
    PrintInfo(L"HomRec Console - try $version, $status, $info, $env, $sec.");

    // Subclass the input box so Enter runs the command and Up/Down walk
    // history - done via a simple WNDPROC swap rather than a separate
    // subclass file, since it's the only control that needs it.
    SetWindowLongPtrW(input_, GWLP_USERDATA, (LONG_PTR)this);
    static WNDPROC origInputProc = nullptr;
    origInputProc = (WNDPROC)GetWindowLongPtrW(input_, GWLP_WNDPROC);
    SetWindowLongPtrW(input_, GWLP_WNDPROC, (LONG_PTR)(+[](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        auto *self = reinterpret_cast<ConsoleWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        // BUGFIX: the console window is created as a child/owned window of
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

void ConsoleWindow::Print(const std::wstring &line) {
    int len = GetWindowTextLengthW(output_);
    SendMessageW(output_, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    std::wstring toAppend = (len > 0 ? L"\r\n" : L"") + line;
    SendMessageW(output_, EM_REPLACESEL, FALSE, (LPARAM)toAppend.c_str());
}

void ConsoleWindow::RunCommand(const std::wstring &raw) {
    std::wistringstream iss(raw);
    std::wstring cmd;
    iss >> cmd;
    if (cmd.empty()) return;

    // both "$" and "!" prefixes are documented (see CHANGELOG.txt
    // 1.7.1: "The ! and $ command prefixes are now optional everywhere...
    // create --window ... works exactly like !create --window ...") as
    // optional/equivalent -- but this only ever stripped "$". Anyone typing
    // commands with a leading "!" (the older, still-documented style) had
    // every single command miss the dispatch table below (e.g. "!info"
    // never matches "info") and fall through to "Unknown or not-yet-ported
    // command" every time -- indistinguishable from the console not
    // accepting any input at all.
    if (cmd[0] == L'$' || cmd[0] == L'!') cmd = cmd.substr(1);
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::towlower);

    // Alias expansion (simple one-level substitution of the command word).
    auto aliasIt = aliases_.find(cmd);
    if (aliasIt != aliases_.end()) cmd = aliasIt->second;

    if (cmd == L"version") CmdVersion(raw);
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
    else if (cmd == L"clip") CmdClip(raw);
    else if (cmd == L"repeat") CmdRepeat(raw);
    else if (cmd == L"batch") CmdBatch(raw);
    else if (cmd == L"ls") CmdLs(raw);
    else if (cmd == L"rm") {
        // Route the two ported $rm forms; anything else under $rm
        // (--ui, @ts, bare $rm_vid, etc.) isn't implemented yet.
        if (raw.find(L"--system@homrec.files") != std::wstring::npos) CmdRmSystemFiles(raw);
        else if (raw.find(L"@homrec") != std::wstring::npos) CmdRmSelfApp(raw);
        else PrintWarn(L"$rm: this form isn't supported yet.");
    } else {
        // Not a built-in - see if a loaded plugin registered this name via
        // homrec.register_command() before giving up on it.
        std::vector<std::string> plugin_cmd_lines;
        if (plugins_ && plugins_->DispatchCommand(NarrowFromWide(cmd), NarrowFromWide(raw), plugin_cmd_lines)) {
            for (const auto &line : plugin_cmd_lines) PrintInfo(WideFromNarrow(line));
        } else {
            PrintWarn(L"Unknown command: " + cmd + L" (try help or !help)");
        }
    }
}

// --- commands ---------------------------------------------------------------

void ConsoleWindow::CmdVersion(const std::wstring &) {
    PrintInfo(L"HomRec v" HR_APP_VERSION_W L" (developer console)");
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
    if (eq == std::wstring::npos) { PrintWarn(L"$alias: usage is `alias name=target`"); return; }
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
    std::wstring log_path = GetBaseDir() + L"\\homrec.log";

    size_t sp = raw.find(L' ');
    std::wstring rest = sp == std::wstring::npos ? L"" : Trim(raw.substr(sp + 1));
    std::wstring first_word = rest;
    size_t sp2 = rest.find(L' ');
    if (sp2 != std::wstring::npos) first_word = rest.substr(0, sp2);

    if (first_word == L"clear") {
        // Truncate rather than delete -- matches $rm's convention of being
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
        PrintWarn(L"usage: $hrc save [path] | $hrc load [path]  (default path: homrec_config.hrc next to the exe)");
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
    // Strip the "#count=N" token itself out of the command to run.
    size_t hashPos = rest.find(L"#count=");
    if (hashPos != std::wstring::npos) {
        size_t end = rest.find(L' ', hashPos);
        rest = Trim(rest.substr(0, hashPos) + (end == std::wstring::npos ? L"" : rest.substr(end)));
    }
    if (count <= 0 || rest.empty()) {
        PrintWarn(L"usage: repeat #count=N <command>");
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

void ConsoleWindow::CmdSec(const std::wstring &raw) {
    std::wistringstream iss(raw);
    std::wstring cmd, val;
    iss >> cmd >> val;
    if (val.empty()) { PrintInfo(sec_core_ ? L"1 (protected)" : L"0 (ALL protections disabled)"); return; }
    sec_core_ = !(val == L"0" || val == L"off" || val == L"false");
    PrintWarn(L"$sec " + val + L": MASTER fuse " + (sec_core_ ? L"ENABLED" : L"DISABLED (everything unlocked)"));
}

void ConsoleWindow::CmdSecUi(const std::wstring &raw) {
    std::wistringstream iss(raw);
    std::wstring cmd, val;
    iss >> cmd >> val;
    if (val.empty()) { PrintInfo(sec_ui_ ? L"1 (protected)" : L"0 (UI protection disabled)"); return; }
    sec_ui_ = !(val == L"0" || val == L"off" || val == L"false");
    PrintWarn(L"$secui " + val + L": UI protection " + (sec_ui_ ? L"ENABLED" : L"DISABLED"));
}

void ConsoleWindow::CmdSecP(const std::wstring &raw) {
    std::wistringstream iss(raw);
    std::wstring cmd, val;
    iss >> cmd >> val;
    if (val.empty()) { PrintInfo(sec_plugin_ ? L"1 (protected)" : L"0 (plugin checks disabled)"); return; }
    sec_plugin_ = !(val == L"0" || val == L"off" || val == L"false");
    PrintWarn(L"$secp " + val + L": plugin version-check / RAM watchdog " + (sec_plugin_ ? L"ENABLED" : L"DISABLED"));
}

void ConsoleWindow::CmdRmSystemFiles(const std::wstring &raw) {
    std::wstring perm = ConsoleParse::ParseNamed(raw, L"permission");
    if (perm != L"core") { PrintWarn(L"$rm --system@homrec.files: requires #permission=core"); return; }
    if (!CoreUnlocked()) {
        PrintWarn(L"$rm --system@homrec.files: blocked - core protection is ON. Run `$sec 0` first.");
        return;
    }

    size_t typePos = raw.find(L"#type={");
    if (typePos == std::wstring::npos) { PrintWarn(L"$rm --system@homrec.files: #type={...} not specified"); return; }
    size_t typeEnd = raw.find(L'}', typePos);
    if (typeEnd == std::wstring::npos) { PrintWarn(L"$rm --system@homrec.files: malformed #type={...}"); return; }
    std::wstring typesRaw = raw.substr(typePos + 7, typeEnd - typePos - 7);

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
            std::wstring p = base + L"\\homrec.log";
            if (FileExists(p) && DeleteFileW(p.c_str())) cleared.push_back(type);
        } else if (type == L"cache") {
            wchar_t tempPath[MAX_PATH];
            GetTempPathW(MAX_PATH, tempPath);
            std::wstring createDir = base + L"\\create";
            std::wstring pluginTemp = std::wstring(tempPath) + L"homrec_plugins";
            if (DirExists(createDir)) RemoveDirRecursive(createDir);
            if (DirExists(pluginTemp)) RemoveDirRecursive(pluginTemp);
            cleared.push_back(type);
        } else {
            PrintWarn(L"$rm --system@homrec.files: unknown #type entry '" + type + L"'");
        }
    }

    std::wstring clearedStr;
    for (size_t i = 0; i < cleared.size(); ++i) clearedStr += (i ? L", " : L"") + cleared[i];
    PrintWarn(L"$rm --system@homrec.files: done. Cleared: " + (clearedStr.empty() ? L"(nothing found)" : clearedStr));
}

void ConsoleWindow::CmdRmSelfApp(const std::wstring &raw) {
    if (!CoreUnlocked()) {
        PrintWarn(L"$rm @homrec: blocked - core protection is ON. Run `$sec 0` first.");
        return;
    }
    auto flags = ConsoleParse::ParseFlags(raw);
    bool quiet = flags.count(L"-q") || flags.count(L"-y");

    if (!quiet) {
        int result = MessageBoxW(hwnd_,
            L"This will permanently uninstall HomRec from this computer once the app closes. "
            L"This cannot be undone.\n\nAre you sure you want to continue?",
            L"Uninstall HomRec", MB_YESNO | MB_ICONWARNING);
        if (result != IDYES) {
            PrintInfo(L"$rm @homrec: cancelled");
            return;
        }
    }

    PrintWarn(L"$rm @homrec: HomRec will delete itself once this process exits.");
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
        PrintOk(L"$rm @homrec: uninstall script scheduled at " + batPath);
    } else {
        PrintErr(L"$rm @homrec: failed to schedule self-delete (CreateProcess failed)");
    }
}
