#include "theme.h"
#include <cstdlib>
#include <cstring>
#include <vector>

// Exported from hr_profile_io.cpp (linked directly now — no ctypes/DLL
// boundary needed since UI and core are one binary).
extern "C" {
    int hr_hrc_read(const char *path, int expected_type, char *out_json, int out_len);
    int hr_theme_get_color(const char *json_body, const char *key, char *out_color, int out_len);
}

namespace {

// "#RRGGBB" -> COLORREF (0x00BBGGRR). Falls back to black on malformed input.
COLORREF HexToColorRef(const char *hex) {
    if (!hex || hex[0] != '#' || strlen(hex) < 7) return RGB(0, 0, 0);
    auto hexPair = [](const char *p) -> int {
        auto nyb = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        return (nyb(p[0]) << 4) | nyb(p[1]);
    };
    int r = hexPair(hex + 1);
    int g = hexPair(hex + 3);
    int b = hexPair(hex + 5);
    return RGB(r, g, b);
}

// Same hex values as BUILTIN_THEMES in homrec_app/mixins/ui_mixin.py.
const ThemeColors kDark = {
    HexToColorRef("#1e1e2e"), // bg
    HexToColorRef("#cdd6f4"), // fg
    HexToColorRef("#89b4fa"), // accent
    HexToColorRef("#a6e3a1"), // success
    HexToColorRef("#f9e2af"), // warning
    HexToColorRef("#f38ba8"), // error
    HexToColorRef("#313244"), // surface
    HexToColorRef("#45475a"), // surface_light
    HexToColorRef("#11111b"), // preview_bg
    HexToColorRef("#cdd6f4"), // text
    HexToColorRef("#a6adc8"), // text_secondary
};

const ThemeColors kLight = {
    HexToColorRef("#f5f5f5"),
    HexToColorRef("#2c3e50"),
    HexToColorRef("#3498db"),
    HexToColorRef("#27ae60"),
    HexToColorRef("#f39c12"),
    HexToColorRef("#e74c3c"),
    HexToColorRef("#ecf0f1"),
    HexToColorRef("#bdc3c7"),
    HexToColorRef("#ffffff"),
    HexToColorRef("#2c3e50"),
    HexToColorRef("#7f8c8d"),
};

} // namespace

const ThemeColors &GetBuiltinTheme(const std::string &name) {
    if (name == "light") return kLight;
    return kDark; // default, mirrors Python's fallback
}

bool LoadCustomTheme(const std::string &path, ThemeColors &out) {
    out = kDark;

    // file_type 2 == HRT per hr_hrc_write's convention (0=hrc,1=hrl,2=hrt).
    int needed = hr_hrc_read(path.c_str(), /*expected_type=*/2, nullptr, 0);
    if (needed >= 0) return false; // 0 = not found/bad magic; no positive-without-buffer case
    std::vector<char> json(-needed);
    if (hr_hrc_read(path.c_str(), 2, json.data(), (int)json.size()) <= 0) return false;

    auto readColor = [&](const char *key, COLORREF fallback) -> COLORREF {
        char buf[16] = {};
        if (hr_theme_get_color(json.data(), key, buf, sizeof(buf)) == 1) {
            return HexToColorRef(buf);
        }
        return fallback;
    };

    out.bg             = readColor("bg", kDark.bg);
    out.fg             = readColor("fg", kDark.fg);
    out.accent         = readColor("accent", kDark.accent);
    out.success        = readColor("success", kDark.success);
    out.warning        = readColor("warning", kDark.warning);
    out.error          = readColor("error", kDark.error);
    out.surface        = readColor("surface", kDark.surface);
    out.surface_light  = readColor("surface_light", kDark.surface_light);
    out.preview_bg     = readColor("preview_bg", kDark.preview_bg);
    out.text           = readColor("text", kDark.text);
    out.text_secondary = readColor("text_secondary", kDark.text_secondary);
    return true;
}

void ThemeBrushes::Rebuild(const ThemeColors &c) {
    Release();
    bg = CreateSolidBrush(c.bg);
    surface = CreateSolidBrush(c.surface);
    surface_light = CreateSolidBrush(c.surface_light);
    preview_bg = CreateSolidBrush(c.preview_bg);
}

void ThemeBrushes::Release() {
    if (bg) { DeleteObject(bg); bg = nullptr; }
    if (surface) { DeleteObject(surface); surface = nullptr; }
    if (surface_light) { DeleteObject(surface_light); surface_light = nullptr; }
    if (preview_bg) { DeleteObject(preview_bg); preview_bg = nullptr; }
}
