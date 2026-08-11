#include "theme.h"
#include <cstdlib>
#include <cstring>

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
    return kDark; // default
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
