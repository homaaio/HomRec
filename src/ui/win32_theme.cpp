#include "win32_theme.h"

// dwmapi.h ships with the MinGW-w64 headers used by this project's
// toolchain; DWMWA_USE_IMMERSIVE_DARK_MODE itself is only defined in very
// recent SDK headers, so it's given by number here (20) with a fallback
// name for clarity - this is the same numeric value Microsoft's own docs
// use for older SDKs that predate the named constant.
#include <dwmapi.h>
#include <unordered_map>
#include <string>

#ifndef HR_DWMWA_USE_IMMERSIVE_DARK_MODE
#define HR_DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace HrWin32Theme {

namespace {

// Per-button bookkeeping for the manual subclass below (original WNDPROC
// to chain to, plus hover/pressed state for the hand-drawn look).
struct ButtonThemeData {
    WNDPROC orig = nullptr;
    bool hot = false;
    bool tracking = false;
};

std::unordered_map<HWND, ButtonThemeData> &ButtonMap() {
    static std::unordered_map<HWND, ButtonThemeData> m;
    return m;
}

LRESULT CALLBACK ThemedButtonProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto &map = ButtonMap();
    auto it = map.find(hwnd);
    if (it == map.end()) return DefWindowProcW(hwnd, msg, wParam, lParam);
    ButtonThemeData &data = it->second;
    WNDPROC orig = data.orig;

    switch (msg) {
        case WM_ERASEBKGND:
            return 1; // painted fully in WM_PAINT below, no flicker-erase needed

        case WM_MOUSEMOVE:
            if (!data.tracking) {
                data.tracking = true;
                data.hot = true;
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;

        case WM_MOUSELEAVE:
            data.tracking = false;
            data.hot = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            break;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
            InvalidateRect(hwnd, nullptr, FALSE);
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);

            LONG style = GetWindowLongW(hwnd, GWL_STYLE);
            bool isDefault = (style & 0xF) == BS_DEFPUSHBUTTON;
            bool enabled = IsWindowEnabled(hwnd);
            bool pressed = enabled &&
                (SendMessageW(hwnd, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;

            COLORREF bg = pressed ? kSurface : (data.hot && enabled ? kAccent : kSurfaceLight);
            COLORREF fg = !enabled ? kTextDim : (pressed || (data.hot && enabled) ? kBg : kText);

            HBRUSH bgBrush = CreateSolidBrush(bg);
            FillRect(hdc, &rc, bgBrush);
            DeleteObject(bgBrush);

            HPEN borderPen = CreatePen(PS_SOLID, isDefault ? 2 : 1,
                                        isDefault ? kAccent : kSurfaceLight);
            HGDIOBJ oldPen = SelectObject(hdc, borderPen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(borderPen);

            wchar_t text[256] = {};
            GetWindowTextW(hwnd, text, 256);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, fg);
            HFONT font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
            HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;
            DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (oldFont) SelectObject(hdc, oldFont);

            if (GetFocus() == hwnd) {
                RECT focusRc = rc;
                InflateRect(&focusRc, -4, -4);
                DrawFocusRect(hdc, &focusRc);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_NCDESTROY: {
            LRESULT res = CallWindowProcW(orig, hwnd, msg, wParam, lParam);
            map.erase(hwnd);
            return res;
        }
    }
    return CallWindowProcW(orig, hwnd, msg, wParam, lParam);
}

} // namespace

void ThemeButton(HWND hwndButton) {
    if (!hwndButton) return;
    auto &map = ButtonMap();
    if (map.count(hwndButton)) return; // already themed

    ButtonThemeData data;
    data.orig = (WNDPROC)SetWindowLongPtrW(hwndButton, GWLP_WNDPROC, (LONG_PTR)ThemedButtonProc);
    map[hwndButton] = data;
    InvalidateRect(hwndButton, nullptr, TRUE);
}

HBRUSH BgBrush() {
    static HBRUSH b = CreateSolidBrush(kBg);
    return b;
}

HBRUSH SurfaceBrush() {
    static HBRUSH b = CreateSolidBrush(kSurface);
    return b;
}

void ApplyDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, HR_DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

LRESULT ColorStatic(HDC hdc) {
    SetTextColor(hdc, kText);
    SetBkColor(hdc, kBg);
    return (LRESULT)BgBrush();
}

LRESULT ColorEdit(HDC hdc) {
    SetTextColor(hdc, kText);
    SetBkColor(hdc, kSurface);
    return (LRESULT)SurfaceBrush();
}

void CenteredWindowRect(int clientW, int clientH, DWORD style, int &x, int &y, int &w, int &h) {
    RECT r = {0, 0, clientW, clientH};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    w = r.right - r.left;
    h = r.bottom - r.top;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    x = (sw - w) / 2;
    y = (sh - h) / 2;
}

} // namespace HrWin32Theme
