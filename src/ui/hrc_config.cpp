#include "hrc_config.h"
#include "../hr_str_convert.h"
#include "../hr_settings_registry.h"

#include <windows.h>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cctype>
#include <cstdlib>

namespace {

std::string Trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ToBool()/FromBool() used to be duplicated here; they now live in
// hr_str_convert.h so this file and hr_settings_registry.cpp can't drift
// apart on what "1"/"true"/"yes" mean. Local aliases so the overlay code
// below (which isn't part of the registry - see WriteOverlaysSection()'s
// comment) doesn't need an `Hr` prefix sprinkled through it.
inline bool ToBool(const std::string &v) { return HrToBool(v); }
inline std::string FromBool(bool b) { return HrFromBool(b); }

// Overlay text/name fields are free-form (a user could type anything,
// including newlines, into the "Text" overlay's content or a Rename), but
// this file format is one-value-per-line -- an embedded newline would
// silently truncate/corrupt whatever comes after it. Collapse them to
// spaces on the way out; this is the same tradeoff every other free-text
// field in this file already makes (capture_window_title, custom_ffmpeg_args,
// etc. aren't escaped either), just made explicit here since overlay text is
// the one place someone's likely to actually paste multi-line content.
std::string OneLine(const std::string &s) {
    std::string out = s;
    for (char &c : out) if (c == '\n' || c == '\r') c = ' ';
    return out;
}

// Shared by Save()/SaveOverlaysOnly() and Load()/LoadOverlaysOnly() below -
// identical "overlay_N_field=value" line format either way, just written
// to/read from a different file. Keeping this in one place means the two
// serializers can't drift apart from each other.
void WriteOverlaysSection(std::ofstream &f, const std::vector<OverlayDef> &overlays) {
    f << "[overlays]\n"
      << "overlay_count=" << overlays.size() << "\n\n";
    for (size_t i = 0; i < overlays.size(); ++i) {
        const OverlayDef &ov = overlays[i];
        std::string p = "overlay_" + std::to_string(i) + "_";
        f << p << "id=" << ov.id << "\n"
          << p << "type=" << ov.type << "\n"
          << p << "name=" << OneLine(ov.name) << "\n"
          << p << "x=" << ov.x << "\n"
          << p << "y=" << ov.y << "\n"
          << p << "w=" << ov.w << "\n"
          << p << "h=" << ov.h << "\n"
          << p << "text=" << OneLine(ov.text) << "\n"
          << p << "text_color=" << ov.text_color << "\n"
          << p << "image_path=" << ov.image_path << "\n"
          << p << "webcam_index=" << ov.webcam_index << "\n"
          << p << "webcam_name=" << OneLine(ov.webcam_name) << "\n"
          << p << "visible=" << FromBool(ov.visible) << "\n"
          << p << "input_json_path=" << ov.input_json_path << "\n"
          << p << "input_png_path=" << ov.input_png_path << "\n\n";
    }
}

void ReadOverlaysSection(const std::unordered_map<std::string, std::string> &kv,
                          std::vector<OverlayDef> &overlays) {
    auto has = [&](const char *k) { return kv.find(k) != kv.end(); };
    auto get = [&](const char *k) -> std::string { auto it = kv.find(k); return it == kv.end() ? std::string() : it->second; };

    overlays.clear();
    if (!has("overlay_count")) return;
    int n = atoi(get("overlay_count").c_str());
    for (int i = 0; i < n; ++i) {
        std::string p = "overlay_" + std::to_string(i) + "_";
        if (!has((p + "id").c_str())) continue; // tolerate a hand-edited/corrupt file
        OverlayDef ov;
        ov.id = get((p + "id").c_str());
        if (has((p + "type").c_str())) ov.type = get((p + "type").c_str());
        if (has((p + "name").c_str())) ov.name = get((p + "name").c_str());
        if (has((p + "x").c_str())) ov.x = atoi(get((p + "x").c_str()).c_str());
        if (has((p + "y").c_str())) ov.y = atoi(get((p + "y").c_str()).c_str());
        if (has((p + "w").c_str())) ov.w = atoi(get((p + "w").c_str()).c_str());
        if (has((p + "h").c_str())) ov.h = atoi(get((p + "h").c_str()).c_str());
        if (has((p + "text").c_str())) ov.text = get((p + "text").c_str());
        if (has((p + "text_color").c_str())) ov.text_color = get((p + "text_color").c_str());
        if (has((p + "image_path").c_str())) ov.image_path = get((p + "image_path").c_str());
        if (has((p + "webcam_index").c_str())) ov.webcam_index = atoi(get((p + "webcam_index").c_str()).c_str());
        if (has((p + "webcam_name").c_str())) ov.webcam_name = get((p + "webcam_name").c_str());
        if (has((p + "visible").c_str())) ov.visible = ToBool(get((p + "visible").c_str()));
        if (has((p + "input_json_path").c_str())) ov.input_json_path = get((p + "input_json_path").c_str());
        if (has((p + "input_png_path").c_str())) ov.input_png_path = get((p + "input_png_path").c_str());
        overlays.push_back(ov);
    }
}

} // namespace

namespace HrcConfig {

bool Save(const AppState &state, const std::wstring &path) {
    std::ofstream f(path.c_str(), std::ios::trunc | std::ios::binary);
    if (!f) return false;

    f << "# HomRec Config (.hrc) v1\n"
      << "# Lines starting with # are comments. Format: key=value\n";

    // Walks HrSettingsRegistry::All() instead of the hand-written per-field
    // list this used to be - that list, and a second one in Load() below,
    // and a THIRD one in lua_api.cpp's old plugin whitelist, were three
    // separate hand-maintained copies of "which fields exist and what
    // they're called" that had already drifted apart (see
    // hr_settings_registry.h's header comment). All() is declared in the
    // same section order the old hand-written blocks used, so this
    // reproduces the same [section] grouping - the only cosmetic
    // difference is a section header is now followed by its own blank
    // line rather than preceding one, which Load() ignores either way
    // (blank lines and lines starting with '[' are just skipped).
    std::string last_section;
    for (const auto &def : HrSettingsRegistry::All()) {
        if (def.section != last_section) {
            f << "\n[" << def.section << "]\n";
            last_section = def.section;
        }
        f << def.key << "=" << def.get(state) << "\n";
    }
    f << "\n";

    // Overlays used to not be saved at all -- anything set up in
    // the Overlays panel silently vanished the moment the profile was
    // reloaded (or the app restarted), with no warning that it was about
    // to happen. Serialized here with one flat "overlay_N_field" key per
    // field (rather than a real [overlay:N] section) because Load() below
    // parses this whole file into one flat key/value map and ignores
    // section headers entirely -- giving every overlay's "x" key the same
    // name would just have the last one clobber all the others. Not part
    // of the registry above since it's a vector of records, not a scalar
    // field - see hr_settings_registry.h's "deliberately excludes" note.
    WriteOverlaysSection(f, state.overlays);

    return true;
}

bool Load(AppState &state, const std::wstring &path, bool allow_sensitive_fields) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;

    std::unordered_map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == '[') continue;
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(trimmed.substr(0, eq));
        std::string val = Trim(trimmed.substr(eq + 1));
        if (!key.empty()) kv[key] = val;
    }

    // Same registry HrcConfig::Save() above now walks - see its comment.
    // `has()`-gated per-field behavior (only touch a field if its key was
    // actually present in the file) and the custom_ffmpeg_args sensitive-
    // field gating are preserved exactly: a key simply absent from `kv`
    // (hand-edited file, older version, etc.) leaves that AppState field
    // untouched, same as every original per-field `if (has(...))` line did.
    for (const auto &def : HrSettingsRegistry::All()) {
        if (def.sensitive && !allow_sensitive_fields) continue;
        auto it = kv.find(def.key);
        if (it == kv.end()) continue;
        def.set(state, it->second);
    }

    // See the matching comment in Save() -- overlays weren't
    // persisted at all before.
    ReadOverlaysSection(kv, state.overlays);

    return true;
}

// See hrc_config.h's comment on why this is a separate small file/function
// pair from Save()/Load() above, rather than folding overlays into either
// HrcConfig::Save() or homrec_settings.json directly.
bool SaveOverlaysOnly(const std::vector<OverlayDef> &overlays, const std::wstring &path) {
    std::ofstream f(path.c_str(), std::ios::trunc | std::ios::binary);
    if (!f) return false;
    f << "# HomRec Overlays (auto-saved) v1\n"
      << "# Lines starting with # are comments. Format: key=value\n\n";
    WriteOverlaysSection(f, overlays);
    return true;
}

bool LoadOverlaysOnly(std::vector<OverlayDef> &overlays, const std::wstring &path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false; // no autosave yet (fresh install, or never had an overlay) - not an error

    std::unordered_map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == '[') continue;
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(trimmed.substr(0, eq));
        std::string val = Trim(trimmed.substr(eq + 1));
        if (!key.empty()) kv[key] = val;
    }

    ReadOverlaysSection(kv, overlays);
    return true;
}

bool RenameSettingsFile(const std::wstring &old_path, const std::wstring &new_path) {
    if (old_path == new_path) return false;
    DWORD oldAttrs = GetFileAttributesW(old_path.c_str());
    if (oldAttrs == INVALID_FILE_ATTRIBUTES) return false; // nothing there yet to move
    DWORD newAttrs = GetFileAttributesW(new_path.c_str());
    if (newAttrs != INVALID_FILE_ATTRIBUTES) return false; // don't clobber an existing file
    // No MOVEFILE_REPLACE_EXISTING (see the check above) and no COPY_ALLOWED
    // needed - both old_path and new_path are always plain filenames/relative
    // paths resolved next to the exe (see kDefaultSettingsPath / the Advanced
    // tab's Browse dialog), i.e. the same volume, so a plain rename suffices.
    return MoveFileW(old_path.c_str(), new_path.c_str()) != 0;
}

std::wstring ResolveSettingsPath(const AppState &state) {
    if (state.settings_path.empty()) return kDefaultSettingsPath;
    // settings_path is stored as UTF-8 (like every other free-text field
    // in this file - see OneLine()'s comment above), converted here rather
    // than at each call site so main_frame.cpp and settings_dialog.cpp
    // can't disagree on how.
    const std::string &s = state.settings_path;
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len > 0 ? len - 1 : 0, L'\0');
    if (len > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

} // namespace HrcConfig
