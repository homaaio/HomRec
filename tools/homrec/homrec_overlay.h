// homrec_overlay.h - overlay editing for the homrec CLI: what a new overlay looks like, which
// properties each type has, how an overlay is found by number / id / name, and how a row of the
// overlay table is drawn.
//
// No console and no Win32 here (OverlayDef comes from app_state.h, which compiles everywhere), so
// scripts/tests/test_homrec_cli.cpp can run all of it. Loading and saving the scene file is done by
// the caller (HrcConfig::LoadScenes / SaveActiveOverlays).
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../../src/ui/app_state.h"
#include "homrec_tui.h"

namespace hrcli {
namespace overlay {

// ---------------------------------------------------------------------------------------------------
// Small parsers
// ---------------------------------------------------------------------------------------------------

inline std::string Lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// 1/0, true/false, yes/no, on/off, da/net (and the Russian words) - anything else is an error.
inline bool ParseBool(const std::string &in, bool *out) {
    const std::string v = Lower(in);
    if (v == "1" || v == "true" || v == "yes" || v == "on" || v == "\xD0\xB4\xD0\xB0") { *out = true; return true; }
    if (v == "0" || v == "false" || v == "no" || v == "off" || v == "\xD0\xBD\xD0\xB5\xD1\x82") { *out = false; return true; }
    return false;
}

// Whole string must be an integer within [lo, hi].
inline bool ParseInt(const std::string &in, int lo, int hi, int *out) {
    if (in.empty()) return false;
    char *end = nullptr;
    const long v = std::strtol(in.c_str(), &end, 10);
    if (!end || *end != '\0' || v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

// "#RRGGBB" or "RRGGBB" -> "#RRGGBB" (upper case).
inline bool NormalizeColor(const std::string &in, std::string *out) {
    std::string v = in;
    if (!v.empty() && v[0] == '#') v.erase(0, 1);
    if (v.size() != 6) return false;
    for (char &c : v) {
        if (!std::isxdigit((unsigned char)c)) return false;
        c = (char)std::toupper((unsigned char)c);
    }
    *out = "#" + v;
    return true;
}

// ---------------------------------------------------------------------------------------------------
// Types and defaults
// ---------------------------------------------------------------------------------------------------

// Types the CLI can create. ("window" / "region" layers are made in the HomRec window, but any layer
// can still be listed, moved, resized, hidden and removed from here.)
inline bool IsCreatableType(const std::string &t) {
    return t == "text" || t == "image" || t == "gif" || t == "webcam" || t == "input_overlay";
}

// Same start values as the Add buttons of the HomRec window (overlays_dock_panel.cpp).
inline OverlayDef MakeDefault(const std::string &type) {
    OverlayDef ov;
    ov.type = type;
    ov.visible = true;
    ov.x = 40;
    ov.y = 40;
    if (type == "text")          { ov.w = 200; ov.h = 60; }
    else if (type == "webcam")   { ov.w = 240; ov.h = 180; }
    else if (type == "input_overlay") { ov.w = 300; ov.h = 150; }
    else                         { ov.w = 200; ov.h = 150; }   // image, gif
    return ov;
}

// "ov_N" with the smallest N that no overlay of the list uses.
inline std::string NewId(const std::vector<OverlayDef> &list) {
    for (int n = 1;; ++n) {
        const std::string id = "ov_" + std::to_string(n);
        bool taken = false;
        for (const OverlayDef &o : list) if (o.id == id) { taken = true; break; }
        if (!taken) return id;
    }
}

// New overlays go to the top of the list = in front, like in the HomRec window.
inline void AddOverlay(std::vector<OverlayDef> &list, OverlayDef ov) {
    ov.id = NewId(list);
    list.insert(list.begin(), std::move(ov));
}

// ---------------------------------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------------------------------

struct Prop {
    const char *key;     // what the user types: x, opacity, text ...
    const char *types;   // "*" = every type, otherwise the types that have it, comma separated
    const char *hint;    // shown next to the key in help
};

inline const std::vector<Prop> &AllProps() {
    static const std::vector<Prop> props = {
        {"name",    "*",             "display name"},
        {"x",       "*",             "left edge, px of the video"},
        {"y",       "*",             "top edge, px of the video"},
        {"w",       "*",             "width, px (1-16384)"},
        {"h",       "*",             "height, px (1-16384)"},
        {"opacity", "*",             "0-100"},
        {"visible", "*",             "yes/no"},
        {"locked",  "*",             "yes/no (locked in the HomRec window)"},
        {"text",    "text",          "the text"},
        {"color",   "text",          "#RRGGBB"},
        {"font",    "text",          "font family"},
        {"path",    "image,gif",     "file path"},
        {"webcam",  "webcam",        "camera index (see 'overlay add webcam')"},
        {"json",    "input_overlay", "layout .json"},
        {"png",     "input_overlay", "spritesheet .png"},
    };
    return props;
}

inline bool TypeHas(const char *types, const std::string &type) {
    const std::string list = std::string(",") + types + ",";
    return std::string(types) == "*" || list.find("," + type + ",") != std::string::npos;
}

// Property keys that exist for `type`, in display order.
inline std::vector<const Prop *> PropsFor(const std::string &type) {
    std::vector<const Prop *> out;
    for (const Prop &p : AllProps()) if (TypeHas(p.types, type)) out.push_back(&p);
    return out;
}

inline std::string FileMustExist(const std::string &path, const char *what, std::string *err) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(path), ec)) {
        *err = std::string(what) + " '" + path + "' not found";
        return std::string();
    }
    return path;
}

// Current value of a property as text (what the editor shows next to the key).
inline std::string GetProp(const OverlayDef &ov, const std::string &key) {
    if (key == "name")    return ov.name;
    if (key == "x")       return std::to_string(ov.x);
    if (key == "y")       return std::to_string(ov.y);
    if (key == "w")       return std::to_string(ov.w);
    if (key == "h")       return std::to_string(ov.h);
    if (key == "opacity") return std::to_string(ov.opacity);
    if (key == "visible") return ov.visible ? "yes" : "no";
    if (key == "locked")  return ov.locked ? "yes" : "no";
    if (key == "text")    return ov.text;
    if (key == "color")   return ov.text_color;
    if (key == "font")    return ov.font_family;
    if (key == "path")    return ov.image_path;
    if (key == "webcam")  return ov.webcam_name.empty() ? std::to_string(ov.webcam_index)
                                                        : std::to_string(ov.webcam_index) + " (" + ov.webcam_name + ")";
    if (key == "json")    return ov.input_json_path;
    if (key == "png")     return ov.input_png_path;
    return std::string();
}

// Sets one property. On failure the overlay is left as it was and *err says why.
inline bool SetProp(OverlayDef &ov, const std::string &key_in, const std::string &value, std::string *err) {
    const std::string key = Lower(key_in);
    const Prop *prop = nullptr;
    for (const Prop &p : AllProps()) if (key == p.key) { prop = &p; break; }
    if (!prop) { *err = "unknown property '" + key_in + "'"; return false; }
    if (!TypeHas(prop->types, ov.type)) {
        *err = "'" + key + "' does not apply to a '" + ov.type + "' overlay";
        return false;
    }

    int n = 0;
    bool b = false;
    std::string s;
    if (key == "name")    { ov.name = value; return true; }
    if (key == "x" || key == "y") {
        if (!ParseInt(value, -32768, 32768, &n)) { *err = "'" + key + "' must be a whole number"; return false; }
        (key == "x" ? ov.x : ov.y) = n;
        return true;
    }
    if (key == "w" || key == "h") {
        if (!ParseInt(value, 1, 16384, &n)) { *err = "'" + key + "' must be a whole number from 1 to 16384"; return false; }
        (key == "w" ? ov.w : ov.h) = n;
        return true;
    }
    if (key == "opacity") {
        if (!ParseInt(value, 0, 100, &n)) { *err = "'opacity' must be a whole number from 0 to 100"; return false; }
        ov.opacity = n;
        return true;
    }
    if (key == "visible" || key == "locked") {
        if (!ParseBool(value, &b)) { *err = "'" + key + "' must be yes or no"; return false; }
        (key == "visible" ? ov.visible : ov.locked) = b;
        return true;
    }
    if (key == "text") {
        if (value.empty() || value.find_first_of("\r\n") != std::string::npos) { *err = "'text' must be one non-empty line"; return false; }
        ov.text = value;
        return true;
    }
    if (key == "color") {
        if (!NormalizeColor(value, &s)) { *err = "'color' must look like #RRGGBB"; return false; }
        ov.text_color = s;
        return true;
    }
    if (key == "font") {
        if (value.empty() || value.find_first_of("\r\n") != std::string::npos) { *err = "'font' must be a font family name"; return false; }
        ov.font_family = value;
        return true;
    }
    if (key == "path") {
        s = FileMustExist(value, "file", err);
        if (s.empty()) return false;
        ov.image_path = s;
        return true;
    }
    if (key == "webcam") {
        if (!ParseInt(value, 0, 63, &n)) { *err = "'webcam' must be a camera index (0 = the first camera)"; return false; }
        ov.webcam_index = n;
        ov.webcam_name.clear();   // the old name belongs to the old index
        return true;
    }
    if (key == "json" || key == "png") {
        s = FileMustExist(value, key == "json" ? "layout" : "spritesheet", err);
        if (s.empty()) return false;
        (key == "json" ? ov.input_json_path : ov.input_png_path) = s;
        return true;
    }
    *err = "unknown property '" + key_in + "'";
    return false;
}

// ---------------------------------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------------------------------

constexpr int kNotFound = -1;
constexpr int kAmbiguous = -2;

// `token` is the row number shown by 'overlay list' (1 = top row), the id (ov_3), or the name (any case).
inline int Find(const std::vector<OverlayDef> &list, const std::string &token) {
    if (token.empty()) return kNotFound;
    int n = 0;
    if (ParseInt(token, 1, 1000000, &n)) return (size_t)n <= list.size() ? n - 1 : kNotFound;
    for (size_t i = 0; i < list.size(); ++i) if (list[i].id == token) return (int)i;
    const std::string want = Lower(token);
    int hit = kNotFound;
    for (size_t i = 0; i < list.size(); ++i) {
        if (!list[i].name.empty() && Lower(list[i].name) == want) {
            if (hit != kNotFound) return kAmbiguous;
            hit = (int)i;
        }
    }
    return hit;
}

// ---------------------------------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------------------------------

inline std::string FileName(const std::string &path) {
    if (path.empty()) return path;
    return std::filesystem::u8path(path).filename().u8string();
}

// What identifies the overlay besides its type: its name, else its content.
inline std::string Summary(const OverlayDef &ov) {
    if (!ov.name.empty()) return ov.name;
    if (ov.type == "text")   return "\"" + ov.text + "\"";
    if (ov.type == "image" || ov.type == "gif") return FileName(ov.image_path);
    if (ov.type == "webcam") return ov.webcam_name.empty() ? "camera " + std::to_string(ov.webcam_index) : ov.webcam_name;
    if (ov.type == "input_overlay") return FileName(ov.input_json_path);
    if (ov.type == "window") return ov.win_title.empty() ? ov.win_exe : ov.win_title;
    if (ov.type == "region") return "region";
    return std::string();
}

constexpr size_t kTypeCol = 13;   // "input_overlay"

// One row: "  1  ● text           "New Text"                 40,40  200x60  100%"
// `cols` is the console width; the summary column takes what the fixed columns leave.
inline std::string Row(size_t number, const OverlayDef &ov, bool selected, size_t cols, bool fancy) {
    using namespace tui;
    char geo[48];
    std::snprintf(geo, sizeof(geo), "%d,%d  %dx%d  %d%%", ov.x, ov.y, ov.w, ov.h, ov.opacity);
    const std::string geometry = geo;

    const size_t fixed = 2 + 3 + 2 + 2 + kTypeCol + 2 + 2 + Width(geometry);   // marks, number, type, gaps, geometry
    const size_t room = cols > fixed + 8 ? cols - fixed : 8;

    const std::string mark = ov.visible ? Paint(fancy, kGreen, fancy ? "\xE2\x97\x8F" : "*")
                                        : Paint(fancy, kDim, fancy ? "\xE2\x97\x8B" : "o");
    std::string line = (selected ? Paint(fancy, kBoldCyan, fancy ? "\xE2\x9D\xAF" : ">") : std::string(" ")) + " " +
                       PadLeft(std::to_string(number), 3) + "  " + mark + " " + PadRight(ov.type, kTypeCol) + "  " +
                       PadRight(Fit(Summary(ov), room, fancy), room) + "  " + Paint(fancy, kDim, geometry);
    return line;
}

} // namespace overlay
} // namespace hrcli
