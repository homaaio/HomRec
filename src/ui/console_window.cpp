#include "console_window.h"
#include "version.h"
#include "recording_controller.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

namespace {

std::wstring Trim(const std::wstring &s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
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
// Parsing helpers — direct ports of _parse_named / _parse_flags.
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

ConsoleWindow::ConsoleWindow(AppState &state, RecordingController *rec, HWND main_window)
    : state_(state), rec_(rec), main_window_(main_window) {}

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
    if (self) return self->HandleMessage(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT ConsoleWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            OnSize(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wParam), HIWORD(wParam), (HWND)lParam);
            return 0;
        case WM_CLOSE:
            ShowWindow(hwnd_, SW_HIDE); // console is a tool window, not app-exiting — matches Ctrl+Shift+T toggle behavior
            return 0;
        default:
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
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

        hwnd_ = CreateWindowExW(0, kClass, L"HomRec Console",
                                 WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 760, 480,
                                 main_window_, nullptr, hInst, this);
        OnCreate(hInst);
    }
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(input_);
}

void ConsoleWindow::OnCreate(HINSTANCE hInst) {
    output_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                               8, 8, 740, 400, hwnd_, (HMENU)IDC_CONSOLE_OUTPUT, hInst, nullptr);
    HFONT monoFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
    SendMessageW(output_, WM_SETFONT, (WPARAM)monoFont, TRUE);

    input_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                              8, 416, 740, 24, hwnd_, (HMENU)IDC_CONSOLE_INPUT, hInst, nullptr);
    SendMessageW(input_, WM_SETFONT, (WPARAM)monoFont, TRUE);

    PrintInfo(L"HomRec Console — try $version, $status, $info, $env, $sec.");

    // Subclass the input box so Enter runs the command and Up/Down walk
    // history — done via a simple WNDPROC swap rather than a separate
    // subclass file, since it's the only control that needs it.
    SetWindowLongPtrW(input_, GWLP_USERDATA, (LONG_PTR)this);
    static WNDPROC origInputProc = nullptr;
    origInputProc = (WNDPROC)GetWindowLongPtrW(input_, GWLP_WNDPROC);
    SetWindowLongPtrW(input_, GWLP_WNDPROC, (LONG_PTR)(+[](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        auto *self = reinterpret_cast<ConsoleWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_KEYDOWN && self) {
            if (wParam == VK_RETURN) {
                wchar_t buf[1024] = {};
                GetWindowTextW(hwnd, buf, 1024);
                std::wstring cmd = Trim(buf);
                if (!cmd.empty()) {
                    self->history_.push_back(cmd);
                    self->history_pos_ = (int)self->history_.size();
                    self->Print(L"> " + cmd);
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
    if (input_) SetWindowPos(input_, nullptr, 8, h - 40, w - 16, 24, SWP_NOZORDER);
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

    // "$" prefix is optional, matching the Python dispatcher's note that it
    // strips a leading "$" before matching.
    if (cmd[0] == L'$') cmd = cmd.substr(1);
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
    else if (cmd == L"rm") {
        // Route the two ported $rm forms; anything else under $rm
        // (--ui, @ts, bare $rm_vid, etc.) is deferred — see README_PHASE8.md.
        if (raw.find(L"--system@homrec.files") != std::wstring::npos) CmdRmSystemFiles(raw);
        else if (raw.find(L"@homrec") != std::wstring::npos) CmdRmSelfApp(raw);
        else PrintWarn(L"$rm: this form isn't ported yet — see README_PHASE8.md (deferred commands).");
    } else {
        PrintWarn(L"Unknown or not-yet-ported command: " + cmd + L" — see README_PHASE8.md for what's deferred.");
    }
}

// --- commands ---------------------------------------------------------------

void ConsoleWindow::CmdVersion(const std::wstring &) {
    PrintInfo(L"HomRec v" HR_APP_VERSION_W L" (native C++ port, Phase 8 console)");
}

void ConsoleWindow::CmdPing(const std::wstring &) {
    PrintOk(L"pong");
}

void ConsoleWindow::CmdEcho(const std::wstring &raw) {
    size_t sp = raw.find(L' ');
    Print(sp == std::wstring::npos ? L"" : raw.substr(sp + 1));
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
        PrintInfo((state_.paused ? L"PAUSED" : L"RECORDING") + std::wstring(L" — ") +
                  rec_->elapsed_formatted() + L", frame " + std::to_wstring(rec_->frame_count()));
    } else {
        PrintInfo(L"idle");
    }
}

void ConsoleWindow::CmdLog(const std::wstring &raw) {
    size_t sp = raw.find(L' ');
    std::wstring msg = sp == std::wstring::npos ? L"" : raw.substr(sp + 1);
    std::wofstream f(GetBaseDir() + L"\\homrec.log", std::ios::app);
    f << L"[console] " << msg << L"\n";
    PrintOk(L"logged");
}

void ConsoleWindow::CmdHide(const std::wstring &) {
    if (main_window_) ShowWindow(main_window_, SW_HIDE);
    PrintOk(L"main window hidden — use the tray icon to restore it");
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
        PrintWarn(L"$rm --system@homrec.files: blocked — core protection is ON. Run `$sec 0` first.");
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
        PrintWarn(L"$rm @homrec: blocked — core protection is ON. Run `$sec 0` first.");
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

    // NOTE (ported verbatim from the Python comment): the tasklist check
    // matches the process name "HomRec.exe" — if you're running this
    // straight from a debugger/different exe name, it won't detect exit
    // correctly and the loop will spin until manually killed.
    std::wofstream f(batPath, std::ios::trunc);
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
