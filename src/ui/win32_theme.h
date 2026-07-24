// win32_theme.h
//
// Several dialogs (PC Analytics, Log Viewer, Window Picker, Overlay
// Manager) are plain raw-Win32 windows registered with the default
// COLOR_BTNFACE system background — light gray, regardless of the app's
// own dark theme. Console and Welcome already hardcode a dark palette of
// their own. This header gives all of them one shared palette/helper set
// so every window in the app reads as the same product instead of a
// mix of a themed main window and gray system dialogs.
//
// Scope note: this recolors backgrounds, static labels, edit fields, and
// the OS-drawn title bar. Native pushbuttons/checkboxes still render with
// the system theme — fully recoloring those needs BS_OWNERDRAW, which is
// a much bigger change and out of scope here.
#pragma once

#include <windows.h>

namespace HrWin32Theme {

// Matches ThemeColors::GetBuiltinTheme("dark") in theme.cpp. Duplicated as
// plain constants (rather than including theme.h) so these Win32-only
// files don't pick up a dependency on the wx-facing theme code.
constexpr COLORREF kBg      = RGB(0x1e, 0x1e, 0x2e);
constexpr COLORREF kSurface = RGB(0x31, 0x32, 0x44);
constexpr COLORREF kText    = RGB(0xcd, 0xd6, 0xf4);
constexpr COLORREF kTextDim = RGB(0xa6, 0xad, 0xc8);
constexpr COLORREF kAccent  = RGB(0x89, 0xb4, 0xfa);

// Solid brushes cached for the process lifetime (Win32 background brushes
// must outlive every window painted with them, so these are never freed).
HBRUSH BgBrush();
HBRUSH SurfaceBrush();

// Windows 10 1809+ / 11: paints the OS-drawn title bar dark to match the
// dark client area. Silently does nothing on older Windows builds that
// don't support the attribute, so it's always safe to call.
void ApplyDarkTitleBar(HWND hwnd);

// Call from a dialog's WM_CTLCOLORSTATIC case:
//   case WM_CTLCOLORSTATIC: return (INT_PTR)HrWin32Theme::ColorStatic((HDC)wParam);
LRESULT ColorStatic(HDC hdc);
// Call from WM_CTLCOLOREDIT / WM_CTLCOLORLISTBOX:
LRESULT ColorEdit(HDC hdc);

// Computes an outer window x/y/w/h that will produce the given CLIENT
// area for the given style, centered on the primary monitor. Use this
// instead of passing a desired client size straight to CreateWindowExW
// (which silently clips whatever control sits nearest the bottom/right --
// worse at higher display scaling) and instead of CW_USEDEFAULT for
// WS_POPUP windows (Windows collapses position *and* size to zero for
// CW_USEDEFAULT on anything that isn't WS_OVERLAPPED).
void CenteredWindowRect(int clientW, int clientH, DWORD style, int &x, int &y, int &w, int &h);

} // namespace HrWin32Theme
