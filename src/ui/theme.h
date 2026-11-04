// theme.h - Port of `HomRecScreen.BUILTIN_THEMES` / `get_theme_colors()`
// (homrec_app/mixins/ui_mixin.py).
#pragma once

#ifdef _WIN32
#include <windows.h>
#else
// Linux: the colour part of the Win32 API as plain integers (0x00BBGGRR), so ThemeColors and the
// FromColorref() helper stay the same on both systems. There are no GDI brushes here.
typedef unsigned long COLORREF;
#define RGB(r, g, b)    ((COLORREF)(((unsigned char)(r)) | (((unsigned long)(unsigned char)(g)) << 8) | (((unsigned long)(unsigned char)(b)) << 16)))
#define GetRValue(c)    ((unsigned char)((c) & 0xFF))
#define GetGValue(c)    ((unsigned char)(((c) >> 8) & 0xFF))
#define GetBValue(c)    ((unsigned char)(((c) >> 16) & 0xFF))
#endif
#include <string>

struct ThemeColors {
    COLORREF bg;
    COLORREF fg;
    COLORREF accent;
    COLORREF success;
    COLORREF warning;
    COLORREF error;
    COLORREF surface;
    COLORREF surface_light;
    COLORREF preview_bg;
    COLORREF text;
    COLORREF text_secondary;
};

// Returns the "dark" or "light" built-in palette. Any unrecognized name
// falls back to "dark".
const ThemeColors &GetBuiltinTheme(const std::string &name);

#ifdef _WIN32
// Cached solid brushes for the current theme, rebuilt on ApplyTheme().
// Owned globally (single top-level window app) and released on rebuild.
struct ThemeBrushes {
    HBRUSH bg = nullptr;
    HBRUSH surface = nullptr;
    HBRUSH surface_light = nullptr;
    HBRUSH preview_bg = nullptr;

    void Rebuild(const ThemeColors &c);
    void Release();
    ~ThemeBrushes() { Release(); }
};
#endif // _WIN32
