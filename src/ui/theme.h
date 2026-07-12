// theme.h — Phase 1
//
// Port of `HomRecScreen.BUILTIN_THEMES` / `get_theme_colors()`
// (homrec_app/mixins/ui_mixin.py). Same two built-in themes, same hex
// values, just materialized as GDI COLORREFs instead of Tk hex strings so
// paint code doesn't re-parse a string every frame.
//
// Custom themes: the README says "plugin system coming soon" for themes;
// the .hrt (gzip+JSON) container and `hr_theme_get_color()` reader already
// exist in hr_profile_io.cpp, so LoadCustomTheme() below is wired up now
// even though no UI to install one exists yet.
#pragma once

#include <windows.h>
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
// falls back to "dark" (matches the Python `get_theme_colors` behavior).
const ThemeColors &GetBuiltinTheme(const std::string &name);

// Loads a color table out of a gzip+JSON `.hrt` file written by
// hr_hrc_write(..., file_type=2). Falls back to the "dark" built-in on any
// read/parse failure. Returns true if the file loaded cleanly.
bool LoadCustomTheme(const std::string &path, ThemeColors &out);

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
