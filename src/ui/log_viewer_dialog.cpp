#include "log_viewer_dialog.h"
#include <windowsx.h>
#include <string>
#include <vector>
#include <fstream>

namespace {

constexpr wchar_t kClassName[] = L"HomRecLogViewer";
enum { IDC_LOG_EDIT = 8201, IDC_LOG_REFRESH, IDC_LOG_OPENFOLDER, IDC_LOG_CLOSE, IDC_LOG_PATH_LABEL };

// Duplicated from console_window.cpp's GetBaseDir() (anonymous-namespace,
// not exported) — same small helper, same convention as this codebase's
// other per-file NarrowFromWide/WideFromNarrow duplication.
std::wstring GetBaseDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full = path;
    size_t pos = full.find_last_of(L"\\/");
    return pos == std::wstring::npos ? full : full.substr(0, pos);
}

// Reads the log file leniently, same spirit as Python's
// open(path, "r", encoding="utf-8", errors="replace"): if the bytes
// aren't valid UTF-8 (console.cpp's std::wofstream writer doesn't
// guarantee that for non-ASCII text), fall back to the system codepage
// rather than failing outright.
std::wstring ReadLogFileLenient(const std::wstring &path, bool &found) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { found = false; return L"Log file not found:\n" + path; }
    found = true;

    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return L"";

    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (needed <= 0) {
        // Not valid UTF-8 — fall back to the active codepage without the
        // strict flag so it can't fail outright (closest Win32 equivalent
        // to errors="replace").
        needed = MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
        if (needed <= 0) return L"(unable to decode log file)";
        std::wstring w(needed, L'\0');
        MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), w.data(), needed);
        return w;
    }
    std::wstring w(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), w.data(), needed);
    return w;
}

void LoadLogInto(HWND edit, const std::wstring &logPath) {
    bool found = false;
    std::wstring text = ReadLogFileLenient(logPath, found);
    // Text controls choke on bare \n; CRLF-ize like most Win32 EDIT
    // consumers expect (Python's Tk Text widget handles \n natively, so
    // this wasn't a concern there).
    std::wstring crlf;
    crlf.reserve(text.size());
    for (wchar_t c : text) {
        if (c == L'\n' && (crlf.empty() || crlf.back() != L'\r')) crlf += L'\r';
        crlf += c;
    }
    SetWindowTextW(edit, crlf.c_str());
    SendMessageW(edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1); // scroll to end, mirrors txt.see("end")
    SendMessageW(edit, EM_SCROLLCARET, 0, 0);
}

LRESULT CALLBACK LogViewerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static std::wstring *logPath = nullptr; // set once at WM_CREATE, lives for the window's lifetime
    switch (msg) {
        case WM_CREATE: {
            HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
            logPath = new std::wstring(GetBaseDir() + L"\\homrec.log");

            CreateWindowExW(0, L"STATIC", logPath->c_str(), WS_CHILD | WS_VISIBLE,
                             12, 10, 660, 18, hwnd, (HMENU)IDC_LOG_PATH_LABEL, hInst, nullptr);

            HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                         ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                         12, 34, 660, 360, hwnd, (HMENU)IDC_LOG_EDIT, hInst, nullptr);
            HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
            SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);

            CreateWindowExW(0, L"BUTTON", L"\U0001F504 Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             12, 404, 110, 28, hwnd, (HMENU)IDC_LOG_REFRESH, hInst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"\U0001F4C2 Open Folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             128, 404, 130, 28, hwnd, (HMENU)IDC_LOG_OPENFOLDER, hInst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                             562, 404, 110, 28, hwnd, (HMENU)IDC_LOG_CLOSE, hInst, nullptr);

            LoadLogInto(edit, *logPath);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_LOG_REFRESH) {
                LoadLogInto(GetDlgItem(hwnd, IDC_LOG_EDIT), *logPath);
            } else if (LOWORD(wParam) == IDC_LOG_OPENFOLDER) {
                ShellExecuteW(hwnd, L"open", GetBaseDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (LOWORD(wParam) == IDC_LOG_CLOSE) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            delete logPath;
            logPath = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

void ShowLogViewerDialog(HWND parent, HINSTANCE hInst) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = LogViewerProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClassName;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    const int W = 700, H = 480;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, L"HomRec Log",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                                 (sw - W) / 2, (sh - H) / 2, W, H,
                                 parent, nullptr, hInst, nullptr);

    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (!IsWindow(hwnd)) break;
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}
