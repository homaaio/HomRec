// theme.h
//
// Port of `HomRecScreen.BUILTIN_THEMES` / `get_theme_colors()`
// (homrec_app/mixins/ui_mixin.py). Same two built-in themes, same hex
// values, just materialized as GDI COLORREFs instead of Tk hex strings so
// paint code doesn't re-parse a string every frame.
//
// Custom .hrt themes were removed - that support is discontinued (there
// was never any UI to install one anyway; LoadCustomTheme() used to be
// wired up to a gzip+JSON .hrt reader in hr_profile_io.cpp but nothing
// called it). Only the two built-in themes below remain.
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
// falls back to "dark".
const ThemeColors &GetBuiltinTheme(const std::string &name);

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
