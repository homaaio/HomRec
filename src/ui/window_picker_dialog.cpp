#include "window_picker_dialog.h"
#include <string>
#include <vector>

extern "C" {
    int hr_enum_windows(char *buf, int buf_len);
}

namespace {

constexpr wchar_t kClassName[] = L"HomRecWindowPicker";
enum { IDC_WP_LIST = 8301, IDC_WP_RECORD, IDC_WP_DESKTOP, IDC_WP_COUNT_LABEL };

std::wstring WideFromNarrow(const std::string &s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}
std::string NarrowFromWide(const std::wstring &w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// hr_enum_windows fills a null-separated, double-null-terminated UTF-8
// buffer (see hr_app_logic.cpp). It has no way to report "buffer was too
// small" — it just silently drops any title that doesn't fit and returns
// the count of titles it did write — so there's nothing meaningful to
// retry against. This uses a generously-sized fixed buffer instead
// (16 KB is enough for hundreds of window titles), same tradeoff ctypes
// callers made in the Python version.
std::vector<std::wstring> EnumOpenWindows() {
    std::vector<char> buf(16384);
    int count = hr_enum_windows(buf.data(), (int)buf.size());
    if (count < 0) return {};

    std::vector<std::wstring> titles;
    const char *p = buf.data();
    const char *end = buf.data() + buf.size();
    while (p < end && *p) {
        std::string s(p);
        titles.push_back(WideFromNarrow(s));
        p += s.size() + 1;
    }
    return titles;
}

struct PickerCtx {
    AppState *state;
    std::vector<std::wstring> *windows;
};

LRESULT CALLBACK PickerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = reinterpret_cast<PickerCtx *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_WP_RECORD) {
                HWND list = GetDlgItem(hwnd, IDC_WP_LIST);
                int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)ctx->windows->size()) {
                    ctx->state->capture_window_title = NarrowFromWide((*ctx->windows)[(size_t)sel]);
                    ctx->state->capture_mode = CaptureMode::Window;
                    DestroyWindow(hwnd);
                }
            } else if (id == IDC_WP_DESKTOP) {
                ctx->state->capture_mode = CaptureMode::Desktop;
                ctx->state->capture_window_title.clear();
                DestroyWindow(hwnd);
            } else if (id == IDC_WP_LIST && HIWORD(wParam) == LBN_DBLCLK) {
                // Double-click a row = same as "Record this window", matches
                // the natural Tk Listbox double-click expectation even
                // though the Python version only wired the single button.
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_WP_RECORD, 0), 0);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

void ShowWindowPickerDialog(HWND parent, HINSTANCE hInst, AppState &state) {
    std::vector<std::wstring> windows = EnumOpenWindows();
    if (windows.empty()) {
        MessageBoxW(parent, L"No open windows found.", L"Info", MB_OK | MB_ICONINFORMATION);
        return;
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = PickerProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClassName;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    const int W = 520, H = 420;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    PickerCtx ctx;
    ctx.state = &state;
    ctx.windows = &windows;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, L"Select Window to Record",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                                 (sw - W) / 2, (sh - H) / 2, W, H,
                                 parent, nullptr, hInst, &ctx);

    std::wstring countLabel = std::to_wstring(windows.size()) + L" windows found";
    CreateWindowExW(0, L"STATIC", countLabel.c_str(), WS_CHILD | WS_VISIBLE,
                     15, 10, 300, 18, hwnd, (HMENU)IDC_WP_COUNT_LABEL, hInst, nullptr);

    HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                 WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
                                 15, 34, W - 32, H - 100, hwnd, (HMENU)IDC_WP_LIST, hInst, nullptr);
    int preselect = -1;
    for (size_t i = 0; i < windows.size(); ++i) {
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)windows[i].c_str());
        if (NarrowFromWide(windows[i]) == state.capture_window_title) preselect = (int)i;
    }
    if (preselect >= 0) {
        SendMessageW(list, LB_SETCURSEL, (WPARAM)preselect, 0);
    }

    CreateWindowExW(0, L"BUTTON", L"Record this window", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                     15, H - 56, 180, 30, hwnd, (HMENU)IDC_WP_RECORD, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Use full desktop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     205, H - 56, 150, 30, hwnd, (HMENU)IDC_WP_DESKTOP, hInst, nullptr);

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
