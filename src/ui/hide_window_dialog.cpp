#include "hide_window_dialog.h"
#include "win32_theme.h"
#include <dwmapi.h>
#include <string>
#include <vector>
#include <algorithm>

#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
// Added in the Windows 10 2004 (build 19041) SDK - defined by number here
// the same way window_picker_dialog.cpp handles DWMWA_EXTENDED_FRAME_
// BOUNDS/DWMWA_CLOAKED, so this still builds against older SDK headers.
// SetWindowDisplayAffinity() itself will just fail (return FALSE) at
// runtime on pre-2004 Windows, which ApplyExclusion() below treats as
// "not supported" rather than silently claiming success - see its
// comment for why that distinction matters here specifically.
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

namespace {

constexpr wchar_t kClassName[] = L"HomRecHideWindowDialog";
enum { IDC_HW_LIST = 8401, IDC_HW_CLEAR_ALL, IDC_HW_CLOSE, IDC_HW_HINT };

constexpr int kRowHeight = 26;

// Deliberately looser than window_picker_dialog.cpp's IsCapturableWindow:
// that one is curating a "pick ONE thing to record" list (tool windows,
// owned popups, etc. filtered out as noise); this one is "what could
// plausibly be sitting on top of/next to your recording that you'd want
// gone" - a notification toast or a small owned popup is exactly the
// kind of thing someone might want to hide, so this only excludes what
// genuinely can't be given display affinity meaningfully (invisible or
// cloaked windows).
bool IsHideCandidate(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    int len = GetWindowTextLengthW(hwnd);
    return len > 0;
}

struct WindowRow {
    HWND hwnd = nullptr;
    std::wstring title;
    bool hidden = false;
};

std::vector<WindowRow> EnumRows(HWND self, AppState *state) {
    struct Ctx {
        std::vector<WindowRow> *rows;
        HWND self;
        AppState *state;
    };
    std::vector<WindowRow> rows;
    Ctx ctx{&rows, self, state};

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lp);
        if (hwnd == c->self) return TRUE;
        if (!IsHideCandidate(hwnd)) return TRUE;

        int len = GetWindowTextLengthW(hwnd);
        std::wstring title(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(hwnd, title.data(), len + 1);
        title.resize(wcslen(title.c_str()));
        if (title.empty()) return TRUE;

        WindowRow row;
        row.hwnd = hwnd;
        row.title = std::move(title);
        row.hidden = std::find(c->state->hidden_capture_windows.begin(),
                                c->state->hidden_capture_windows.end(),
                                hwnd) != c->state->hidden_capture_windows.end();
        c->rows->push_back(std::move(row));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return rows;
}

// Applies or clears the OS-level exclusion for one window and keeps
// state.hidden_capture_windows in sync with what's actually applied.
// Returns false if the OS call itself failed (e.g. pre-Windows-10-2004,
// or the window closed between the list being built and the click) -
// callers must treat that as "still visible in captures", not silently
// assume it worked.
bool ApplyExclusion(AppState &state, HWND hwnd, bool hide) {
    if (!IsWindow(hwnd)) return false;
    BOOL ok = SetWindowDisplayAffinity(hwnd, hide ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    auto &v = state.hidden_capture_windows;
    if (ok && hide) {
        if (std::find(v.begin(), v.end(), hwnd) == v.end()) v.push_back(hwnd);
    } else {
        v.erase(std::remove(v.begin(), v.end(), hwnd), v.end());
    }
    return ok != 0;
}

struct DlgCtx {
    AppState *state;
    std::vector<WindowRow> *rows;
};

void RefreshList(HWND hwnd, DlgCtx *ctx) {
    HWND list = GetDlgItem(hwnd, IDC_HW_LIST);
    *ctx->rows = EnumRows(hwnd, ctx->state);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (auto &r : *ctx->rows) {
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)r.title.c_str());
    }
    InvalidateRect(list, nullptr, TRUE);
}

LRESULT CALLBACK HideProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = reinterpret_cast<DlgCtx *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_HW_LIST && HIWORD(wParam) == LBN_DBLCLK) {
                HWND list = GetDlgItem(hwnd, IDC_HW_LIST);
                int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)ctx->rows->size()) {
                    WindowRow &r = (*ctx->rows)[(size_t)sel];
                    bool ok = ApplyExclusion(*ctx->state, r.hwnd, !r.hidden);
                    if (ok) r.hidden = !r.hidden;
                    InvalidateRect(list, nullptr, TRUE);
                }
            } else if (id == IDC_HW_CLEAR_ALL) {
                ClearAllHiddenCaptureWindows(*ctx->state);
                RefreshList(hwnd, ctx);
            } else if (id == IDC_HW_CLOSE) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_MEASUREITEM: {
            auto *mis = reinterpret_cast<MEASUREITEMSTRUCT *>(lParam);
            if (mis->CtlID == IDC_HW_LIST) { mis->itemHeight = kRowHeight; return TRUE; }
            return FALSE;
        }
        case WM_DRAWITEM: {
            auto *dis = reinterpret_cast<DRAWITEMSTRUCT *>(lParam);
            if (dis->CtlID != IDC_HW_LIST || dis->itemID == (UINT)-1 ||
                !ctx || dis->itemID >= ctx->rows->size()) {
                return FALSE;
            }
            const WindowRow &r = (*ctx->rows)[dis->itemID];
            bool selected = (dis->itemState & ODS_SELECTED) != 0;

            HBRUSH rowBrush = CreateSolidBrush(selected ? HrWin32Theme::kSurfaceLight : HrWin32Theme::kBg);
            FillRect(dis->hDC, &dis->rcItem, rowBrush);
            DeleteObject(rowBrush);

            SetBkMode(dis->hDC, TRANSPARENT);

            // Status tag on the left ("HIDDEN" in amber, or a blank slot
            // of the same width so titles still line up).
            RECT tagRect{dis->rcItem.left + 10, dis->rcItem.top,
                         dis->rcItem.left + 90, dis->rcItem.bottom};
            if (r.hidden) {
                SetTextColor(dis->hDC, RGB(230, 200, 90));  // amber, matches console's "warn" color
                DrawTextW(dis->hDC, L"HIDDEN", -1, &tagRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            }

            RECT textRect{dis->rcItem.left + 96, dis->rcItem.top,
                          dis->rcItem.right - 10, dis->rcItem.bottom};
            SetTextColor(dis->hDC, selected ? HrWin32Theme::kText : HrWin32Theme::kTextDim);
            DrawTextW(dis->hDC, r.title.c_str(), -1, &textRect,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            return TRUE;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_CTLCOLORSTATIC:
            return (LRESULT)HrWin32Theme::ColorStatic((HDC)wParam);
        case WM_CTLCOLORLISTBOX:
            return (LRESULT)HrWin32Theme::ColorEdit((HDC)wParam);
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

void ShowHideWindowDialog(HWND parent, HINSTANCE hInst, AppState &state) {
    std::vector<WindowRow> rows;  // filled by RefreshList() below
    DlgCtx ctx{&state, &rows};

    WNDCLASSW wc = {};
    wc.lpfnWndProc = HideProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClassName;
    wc.hbrBackground = HrWin32Theme::BgBrush();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    const int W = 480, H = 460;
    int wx, wy, ww, wh;
    HrWin32Theme::CenteredWindowRect(W, H, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, wx, wy, ww, wh);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, L"Hide Windows From Recording",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                                 wx, wy, ww, wh, parent, nullptr, hInst, &ctx);
    HrWin32Theme::ApplyDarkTitleBar(hwnd);

    CreateWindowExW(0, L"STATIC",
        L"Double-click a window to hide/unhide it from any screen capture "
        L"(this app's recording, or any other capture tool) - it stays "
        L"perfectly normal on your own screen.",
        WS_CHILD | WS_VISIBLE, 15, 10, W - 32, 44, hwnd, (HMENU)IDC_HW_HINT, hInst, nullptr);

    CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                     WS_CHILD | WS_VISIBLE | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                     LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
                     15, 60, W - 32, H - 130, hwnd, (HMENU)IDC_HW_LIST, hInst, nullptr);

    HrWin32Theme::ThemeButton(CreateWindowExW(0, L"BUTTON", L"Show all (clear)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     15, H - 56, 160, 30, hwnd, (HMENU)IDC_HW_CLEAR_ALL, hInst, nullptr));
    HrWin32Theme::ThemeButton(CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                     W - 115, H - 56, 100, 30, hwnd, (HMENU)IDC_HW_CLOSE, hInst, nullptr));

    RefreshList(hwnd, &ctx);

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

void ClearAllHiddenCaptureWindows(AppState &state) {
    // Copy first - ApplyExclusion() mutates state.hidden_capture_windows
    // as it goes, which would otherwise invalidate the iteration.
    std::vector<HWND> handles = state.hidden_capture_windows;
    for (HWND h : handles) {
        if (IsWindow(h)) SetWindowDisplayAffinity(h, WDA_NONE);
    }
    state.hidden_capture_windows.clear();
}
