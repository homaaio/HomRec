#include "win32_theme.h"

// dwmapi.h ships with the MinGW-w64 headers used by this project's
// toolchain; DWMWA_USE_IMMERSIVE_DARK_MODE itself is only defined in very
// recent SDK headers, so it's given by number here (20) with a fallback
// name for clarity — this is the same numeric value Microsoft's own docs
// use for older SDKs that predate the named constant.
#include <dwmapi.h>

#ifndef HR_DWMWA_USE_IMMERSIVE_DARK_MODE
#define HR_DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace HrWin32Theme {

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
