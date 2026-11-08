#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "hrc_config.h"
#include "../utils/str_convert.h"
#include "../core/settings_registry.h"
#ifdef _WIN32
#include "window_picker_dialog.h"   // HrMigrateCaptureSourceToLayers()
#include <windows.h>
#else
#include "../platform/paths.h"
#include <cstdio>
#include <sys/stat.h>
// Linux: there are no window / region layers yet (see ui/linux/stubs.cpp).
void HrMigrateCaptureSourceToLayers(AppState &state);
#endif
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cctype>
#include <algorithm>
#include <cstdlib>

namespace {

std::string Trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ToBool()/FromBool() used to be duplicated here; they now live in str_convert.h so this
// file and settings_registry.cpp can't drift apart on what "1"/"true"/"yes" mean.
inline bool ToBool(const std::string &v) { return HrToBool(v); }
inline std::string FromBool(bool b) { return HrFromBool(b); }

std::string OneLine(const std::string &s) {
    std::string out = s;
    for (char &c : out) if (c == '\n' || c == '\r') c = ' ';
    return out;
}

// Shared by Save()/SaveOverlaysOnly() and Load()/LoadOverlaysOnly() below - identical
// "overlay_N_field=value" line format either way, just written to/read from a different
// file.
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
          << p << "font_family=" << OneLine(ov.font_family) << "\n"
          << p << "opacity=" << ov.opacity << "\n"
          << p << "image_path=" << ov.image_path << "\n"
          << p << "webcam_index=" << ov.webcam_index << "\n"
          << p << "webcam_name=" << OneLine(ov.webcam_name) << "\n"
          << p << "visible=" << FromBool(ov.visible) << "\n"
          << p << "locked=" << FromBool(ov.locked) << "\n"
          << p << "lock_aspect=" << FromBool(ov.lock_aspect) << "\n"
          << p << "input_json_path=" << ov.input_json_path << "\n"
          << p << "input_png_path=" << ov.input_png_path << "\n"
          << p << "win_title=" << OneLine(ov.win_title) << "\n"
          << p << "win_exe=" << OneLine(ov.win_exe) << "\n"
          << p << "win_class=" << OneLine(ov.win_class) << "\n"
          << p << "win_match_exe=" << FromBool(ov.win_match_exe) << "\n"
          << p << "win_match_class=" << FromBool(ov.win_match_class) << "\n"
          << p << "win_match_title=" << FromBool(ov.win_match_title) << "\n"
          << p << "win_title_mode=" << ov.win_title_mode << "\n"
          << p << "win_follow=" << FromBool(ov.win_follow) << "\n"
          << p << "win_method=" << ov.win_method << "\n"
          << p << "win_area=" << ov.win_area << "\n"
          << p << "win_fps_cap=" << ov.win_fps_cap << "\n"
          << p << "rx=" << ov.rx << "\n"
          << p << "ry=" << ov.ry << "\n"
          << p << "rw=" << ov.rw << "\n"
          << p << "rh=" << ov.rh << "\n\n";
    }
}

void ReadOverlaysSection(const std::unordered_map<std::string, std::string> &kv,
                          std::vector<OverlayDef> &overlays) {
    auto has = [&](const char *k) { return kv.find(k) != kv.end(); };
    auto get = [&](const char *k) -> std::string { auto it = kv.find(k); return it == kv.end() ? std::string() : it->second; };

    overlays.clear();
    if (!has("overlay_count")) return;
    int n = atoi(get("overlay_count").c_str());
    if (n < 0) n = 0;
    if (n > 10000) n = 10000; // generous; a real overlay list is a handful
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
        // font_family / opacity were never written or read here, so every restart silently reset
        // them to their defaults (Segoe UI / 100) - an overlay the user had faded to opacity 0
        // (invisible) came back fully visible.
        if (has((p + "font_family").c_str())) {
            std::string ff = get((p + "font_family").c_str());
            if (!ff.empty()) ov.font_family = ff;
        }
        if (has((p + "opacity").c_str())) {
            int op = atoi(get((p + "opacity").c_str()).c_str());
            ov.opacity = op < 0 ? 0 : (op > 100 ? 100 : op);
        }
        if (has((p + "image_path").c_str())) ov.image_path = get((p + "image_path").c_str());
        if (has((p + "webcam_index").c_str())) ov.webcam_index = atoi(get((p + "webcam_index").c_str()).c_str());
        if (has((p + "webcam_name").c_str())) ov.webcam_name = get((p + "webcam_name").c_str());
        if (has((p + "visible").c_str())) ov.visible = ToBool(get((p + "visible").c_str()));
        if (has((p + "locked").c_str())) ov.locked = ToBool(get((p + "locked").c_str()));
        if (has((p + "lock_aspect").c_str())) ov.lock_aspect = ToBool(get((p + "lock_aspect").c_str()));
        if (has((p + "input_json_path").c_str())) ov.input_json_path = get((p + "input_json_path").c_str());
        if (has((p + "input_png_path").c_str())) ov.input_png_path = get((p + "input_png_path").c_str());
        // 8th pass: window / region layers
        if (has((p + "win_title").c_str())) ov.win_title = get((p + "win_title").c_str());
        if (has((p + "win_exe").c_str())) ov.win_exe = get((p + "win_exe").c_str());
        if (has((p + "win_class").c_str())) ov.win_class = get((p + "win_class").c_str());
        if (has((p + "win_match_exe").c_str())) ov.win_match_exe = ToBool(get((p + "win_match_exe").c_str()));
        if (has((p + "win_match_class").c_str())) ov.win_match_class = ToBool(get((p + "win_match_class").c_str()));
        if (has((p + "win_match_title").c_str())) ov.win_match_title = ToBool(get((p + "win_match_title").c_str()));
        if (has((p + "win_title_mode").c_str())) ov.win_title_mode = get((p + "win_title_mode").c_str());
        if (has((p + "win_follow").c_str())) ov.win_follow = ToBool(get((p + "win_follow").c_str()));
        if (has((p + "win_method").c_str())) ov.win_method = get((p + "win_method").c_str());
        if (has((p + "win_area").c_str())) ov.win_area = get((p + "win_area").c_str());
        if (has((p + "win_fps_cap").c_str())) ov.win_fps_cap = atoi(get((p + "win_fps_cap").c_str()).c_str());
        if (has((p + "rx").c_str())) ov.rx = atoi(get((p + "rx").c_str()).c_str());
        if (has((p + "ry").c_str())) ov.ry = atoi(get((p + "ry").c_str()).c_str());
        if (has((p + "rw").c_str())) ov.rw = atoi(get((p + "rw").c_str()).c_str());
        if (has((p + "rh").c_str())) ov.rh = atoi(get((p + "rh").c_str()).c_str());
        overlays.push_back(ov);
    }
}

// Same pattern as Write/ReadOverlaysSection above, for AppState::custom_hotkeys (see its
// declaration in app_state.h and ConfigureHotkeysFromState() in main_frame.cpp for how
// these get turned into actual OS hotkeys).
void WriteHotkeysSection(std::ofstream &f, const std::vector<std::pair<std::string, std::string>> &hotkeys) {
    f << "[custom_hotkeys]\n"
      << "custom_hotkey_count=" << hotkeys.size() << "\n\n";
    for (size_t i = 0; i < hotkeys.size(); ++i) {
        std::string p = "custom_hotkey_" + std::to_string(i) + "_";
        f << p << "action=" << OneLine(hotkeys[i].first) << "\n"
          << p << "keys=" << OneLine(hotkeys[i].second) << "\n\n";
    }
}

void ReadHotkeysSection(const std::unordered_map<std::string, std::string> &kv,
                         std::vector<std::pair<std::string, std::string>> &hotkeys) {
    auto has = [&](const std::string &k) { return kv.find(k) != kv.end(); };
    auto get = [&](const std::string &k) -> std::string { auto it = kv.find(k); return it == kv.end() ? std::string() : it->second; };

    hotkeys.clear();
    if (!has("custom_hotkey_count")) return;
    int n = atoi(get("custom_hotkey_count").c_str());
    // Same unbounded-count hang as overlay_count above.
    if (n < 0) n = 0;
    if (n > 10000) n = 10000;
    for (int i = 0; i < n; ++i) {
        std::string p = "custom_hotkey_" + std::to_string(i) + "_";
        if (!has(p + "action")) continue; // tolerate a hand-edited/corrupt file
        hotkeys.emplace_back(get(p + "action"), get(p + "keys"));
    }
}

// 2.4: Audio Mixer extra channels (AppState::audio_sources) - another microphone,
// a browser / window, an audio file. Same flat "audio_source_N_field" scheme as
// the overlays above.
void WriteAudioSourcesSection(std::ofstream &f, const std::vector<AudioSourceDef> &srcs) {
    f << "[audio_sources]\n"
      << "audio_source_count=" << srcs.size() << "\n\n";
    for (size_t i = 0; i < srcs.size(); ++i) {
        const AudioSourceDef &a = srcs[i];
        std::string p = "audio_source_" + std::to_string(i) + "_";
        f << p << "id=" << a.id << "\n"
          << p << "kind=" << a.kind << "\n"
          << p << "name=" << OneLine(a.name) << "\n"
          << p << "target=" << OneLine(a.target) << "\n"
          << p << "window_title=" << OneLine(a.window_title) << "\n"
          << p << "volume=" << a.volume << "\n"
          << p << "muted=" << FromBool(a.muted) << "\n\n";
    }
}

void ReadAudioSourcesSection(const std::unordered_map<std::string, std::string> &kv,
                              std::vector<AudioSourceDef> &srcs) {
    auto has = [&](const std::string &k) { return kv.find(k) != kv.end(); };
    auto get = [&](const std::string &k) -> std::string { auto it = kv.find(k); return it == kv.end() ? std::string() : it->second; };
    if (!has("audio_source_count")) return;   // older file: keep whatever the session already has
    srcs.clear();
    int n = atoi(get("audio_source_count").c_str());
    if (n < 0) n = 0;
    if (n > 64) n = 64;
    for (int i = 0; i < n; ++i) {
        std::string p = "audio_source_" + std::to_string(i) + "_";
        if (!has(p + "kind")) continue;
        AudioSourceDef a;
        a.id = has(p + "id") ? get(p + "id") : ("as_" + std::to_string(i + 1));
        a.kind = get(p + "kind");
        if (a.kind != "mic" && a.kind != "app" && a.kind != "file") continue;
        a.name = get(p + "name");
        a.target = get(p + "target");
        a.window_title = get(p + "window_title");
        if (has(p + "volume")) a.volume = std::max(0, std::min(150, atoi(get(p + "volume").c_str())));
        a.muted = ToBool(get(p + "muted"));
        srcs.push_back(a);
    }
}

} // namespace

namespace HrcConfig {

static bool SaveDirect(const AppState &state, const std::wstring &path);
static bool SaveOverlaysDirect(const std::vector<OverlayDef> &overlays, const std::wstring &path);

// Crash/power-loss safety: the old code opened the real file with `trunc` and streamed
// into it, so dying mid-write left a truncated .hrc / overlays file (= lost settings).
template <typename WriteFn>
static bool WriteAtomically(const std::wstring &path, WriteFn write) {
#ifdef _WIN32
    const std::wstring tmp = path + L".tmp";
    if (write(tmp)) {
        if (MoveFileExW(tmp.c_str(), path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
    }
    DeleteFileW(tmp.c_str());
#else
    // Linux: the config folder may not exist yet (first start), then write next to it and rename().
    const std::string npath = HrNarrow(path);
    const size_t slash = npath.rfind('/');
    if (slash != std::string::npos && slash > 0) hr::platform::EnsureDir(npath.substr(0, slash));
    const std::wstring tmp = path + L".tmp";
    if (write(tmp)) {
        if (std::rename(HrNarrow(tmp).c_str(), npath.c_str()) == 0) return true;
        std::remove(HrNarrow(tmp).c_str());
    }
#endif
    return write(path); // fallback: previous behaviour
}

bool Save(const AppState &state, const std::wstring &path) {
    return WriteAtomically(path, [&](const std::wstring &p) { return SaveDirect(state, p); });
}

static bool SaveDirect(const AppState &state, const std::wstring &path) {
    std::ofstream f(HrFsPath(path).c_str(), std::ios::trunc | std::ios::binary);
    if (!f) return false;

    f << "# HomRec Config (.hrc) v1\n"
      << "# Lines starting with # are comments. Format: key=value\n";

    std::string last_section;
    for (const auto &def : HrSettingsRegistry::All()) {
        if (def.section != last_section) {
            f << "\n[" << def.section << "]\n";
            last_section = def.section;
        }
        f << def.key << "=" << def.get(state) << "\n";
    }
    f << "\n";

    // Overlays used to not be saved at all -- anything set up in the Overlays panel silently
    // vanished the moment the profile was reloaded (or the app restarted), with no warning
    // that it was about to happen.
    WriteOverlaysSection(f, state.overlays);
    WriteHotkeysSection(f, state.custom_hotkeys);
    WriteAudioSourcesSection(f, state.audio_sources);

    return true;
}

bool Load(AppState &state, const std::wstring &path, bool allow_sensitive_fields) {
    std::ifstream f(HrFsPath(path).c_str(), std::ios::binary);
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
    for (const auto &def : HrSettingsRegistry::All()) {
        if (def.sensitive && !allow_sensitive_fields) continue;
        auto it = kv.find(def.key);
        if (it == kv.end()) continue;
        def.set(state, it->second);
    }

    // See the matching comment in Save() -- overlays weren't
    // persisted at all before.
    ReadOverlaysSection(kv, state.overlays);
    ReadHotkeysSection(kv, state.custom_hotkeys);
    ReadAudioSourcesSection(kv, state.audio_sources);

    // 8th pass: a file saved before the Desktop split may still say "record this window / region".
    // That is a layer now - convert it once (capture_mode goes back to desktop).
    HrMigrateCaptureSourceToLayers(state);

    return true;
}

// See hrc_config.h's comment on why this is a separate small file/function
// pair from Save()/Load() above, rather than folding overlays into either
// HrcConfig::Save() or homrec_settings.json directly.
bool SaveOverlaysOnly(const std::vector<OverlayDef> &overlays, const std::wstring &path) {
    return WriteAtomically(path, [&](const std::wstring &p) { return SaveOverlaysDirect(overlays, p); });
}

static bool SaveOverlaysDirect(const std::vector<OverlayDef> &overlays, const std::wstring &path) {
    std::ofstream f(HrFsPath(path).c_str(), std::ios::trunc | std::ios::binary);
    if (!f) return false;
    f << "# HomRec Overlays (auto-saved) v1\n"
      << "# Lines starting with # are comments. Format: key=value\n\n";
    WriteOverlaysSection(f, overlays);
    return true;
}

bool LoadOverlaysOnly(std::vector<OverlayDef> &overlays, const std::wstring &path) {
    std::ifstream f(HrFsPath(path).c_str(), std::ios::binary);
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

// ---- Scenes --------------------------------------------------------------------------------------

static std::wstring SceneDir() {
#ifdef _WIN32
    return L"scenes";   // relative to the working directory, like homrec_overlays.hrc
#else
    return HrWide(hr::platform::ConfigDir() + "/scenes");
#endif
}
static std::wstring SceneIndexPath() { return SceneDir() + L"/scenes.index"; }
static std::wstring SceneFilePath(const std::string &id) { return SceneDir() + L"/" + HrWide(id) + L".hrc"; }

static bool FileExistsW(const std::wstring &p) {
#ifdef _WIN32
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return ::stat(HrNarrow(p).c_str(), &st) == 0;
#endif
}

static void EnsureSceneDir() {
#ifdef _WIN32
    CreateDirectoryW(SceneDir().c_str(), nullptr);   // already there: fine
#else
    hr::platform::EnsureDir(HrNarrow(SceneDir()));
#endif
}

static void RemoveFileW(const std::wstring &p) {
#ifdef _WIN32
    DeleteFileW(p.c_str());
#else
    std::remove(HrNarrow(p).c_str());
#endif
}

// The id becomes part of a file name, and scenes.index can be edited by hand.
static bool ValidSceneId(const std::string &id) {
    if (id.empty() || id.size() > 64) return false;
    for (unsigned char c : id)
        if (!std::isalnum(c) && c != '_' && c != '-') return false;
    return true;
}

static bool HasScene(const AppState &state, const std::string &id) {
    for (const auto &sc : state.scenes) if (sc.id == id) return true;
    return false;
}

static bool SaveSceneIndex(const AppState &state) {
    EnsureSceneDir();
    return WriteAtomically(SceneIndexPath(), [&](const std::wstring &p) {
        std::ofstream f(HrFsPath(p).c_str(), std::ios::trunc | std::ios::binary);
        if (!f) return false;
        f << "# HomRec scenes v1\n"
          << "active=" << state.active_scene_id << "\n"
          << "scene_count=" << state.scenes.size() << "\n\n";
        for (size_t i = 0; i < state.scenes.size(); ++i) {
            f << "scene_" << i << "_id=" << state.scenes[i].id << "\n"
              << "scene_" << i << "_name=" << OneLine(state.scenes[i].name) << "\n";
        }
        return true;
    });
}

static bool ReadSceneIndex(AppState &state) {
    std::ifstream f(HrFsPath(SceneIndexPath()).c_str(), std::ios::binary);
    if (!f) return false;
    std::unordered_map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line)) {
        std::string t = Trim(line);
        if (t.empty() || t[0] == '#') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        kv[Trim(t.substr(0, eq))] = Trim(t.substr(eq + 1));
    }
    auto get = [&](const std::string &k) { auto it = kv.find(k); return it == kv.end() ? std::string() : it->second; };

    int n = atoi(get("scene_count").c_str());
    if (n < 0) n = 0;
    if (n > 64) n = 64;   // as many as the Scenes menu has ids for
    state.scenes.clear();
    for (int i = 0; i < n; ++i) {
        SceneInfo sc;
        sc.id = get("scene_" + std::to_string(i) + "_id");
        if (!ValidSceneId(sc.id) || HasScene(state, sc.id)) continue;
        sc.name = get("scene_" + std::to_string(i) + "_name");
        if (sc.name.empty()) sc.name = sc.id;
        state.scenes.push_back(sc);
    }
    if (state.scenes.empty()) return false;
    state.active_scene_id = get("active");
    if (!HasScene(state, state.active_scene_id)) state.active_scene_id = state.scenes[0].id;
    return true;
}

void LoadScenes(AppState &state) {
    if (ReadSceneIndex(state)) {
        // A missing file leaves state.overlays as it is (what homrec.hrc had).
        LoadOverlaysOnly(state.overlays, SceneFilePath(state.active_scene_id));
        return;
    }
    // First start with scenes: the old single overlay list becomes "Scene 1".
    LoadOverlaysOnly(state.overlays, kOverlaysAutosavePath);
    state.scenes.assign(1, SceneInfo{"scene_1", "Scene 1"});
    state.active_scene_id = "scene_1";
    SaveActiveOverlays(state);
    SaveSceneIndex(state);
}

bool SaveActiveOverlays(const AppState &state) {
    if (state.active_scene_id.empty())   // LoadScenes() has not run: the old single file
        return SaveOverlaysOnly(state.overlays, kOverlaysAutosavePath);
    EnsureSceneDir();
    return SaveOverlaysOnly(state.overlays, SceneFilePath(state.active_scene_id));
}

bool SwitchScene(AppState &state, const std::string &id) {
    if (id == state.active_scene_id) return true;
    if (!HasScene(state, id)) return false;
    // Read the target first, so a damaged file leaves the current scene alone.
    std::vector<OverlayDef> next;
    if (!LoadOverlaysOnly(next, SceneFilePath(id))) return false;
    if (!SaveActiveOverlays(state)) return false;   // switching must not lose what was just edited
    state.overlays = std::move(next);
    state.active_scene_id = id;
    SaveSceneIndex(state);
    return true;
}

static std::string CleanSceneName(const AppState &state, const std::string &name) {
    std::string n = Trim(OneLine(name));
    if (n.size() > 64) n.resize(64);
    if (n.empty()) n = "Scene " + std::to_string(state.scenes.size() + 1);
    return n;
}

std::string AddScene(AppState &state, const std::string &name, bool copy_active) {
    if (state.scenes.size() >= 64) return "";
    std::string id;
    for (int n = 1;; ++n) {
        id = "scene_" + std::to_string(n);
        if (!HasScene(state, id) && !FileExistsW(SceneFilePath(id))) break;
    }
    EnsureSceneDir();
    const std::vector<OverlayDef> none;
    if (!SaveOverlaysOnly(copy_active ? state.overlays : none, SceneFilePath(id))) return "";
    state.scenes.push_back(SceneInfo{id, CleanSceneName(state, name)});
    SaveSceneIndex(state);
    return id;
}

bool RenameScene(AppState &state, const std::string &id, const std::string &name) {
    for (auto &sc : state.scenes) {
        if (sc.id != id) continue;
        sc.name = CleanSceneName(state, name);
        SaveSceneIndex(state);
        return true;
    }
    return false;
}

bool DeleteScene(AppState &state, const std::string &id) {
    if (id == state.active_scene_id || state.scenes.size() <= 1) return false;
    for (size_t i = 0; i < state.scenes.size(); ++i) {
        if (state.scenes[i].id != id) continue;
        state.scenes.erase(state.scenes.begin() + (std::ptrdiff_t)i);
        RemoveFileW(SceneFilePath(id));
        SaveSceneIndex(state);
        return true;
    }
    return false;
}

bool RenameSettingsFile(const std::wstring &old_path, const std::wstring &new_path) {
    if (old_path == new_path) return false;
#ifndef _WIN32
    struct stat st;
    if (::stat(HrNarrow(old_path).c_str(), &st) != 0) return false;   // nothing there yet to move
    if (::stat(HrNarrow(new_path).c_str(), &st) == 0) return false;   // don't clobber an existing file
    const std::string nnew = HrNarrow(new_path);
    const size_t slash = nnew.rfind('/');
    if (slash != std::string::npos && slash > 0) hr::platform::EnsureDir(nnew.substr(0, slash));
    return std::rename(HrNarrow(old_path).c_str(), nnew.c_str()) == 0;
#else
    DWORD oldAttrs = GetFileAttributesW(old_path.c_str());
    if (oldAttrs == INVALID_FILE_ATTRIBUTES) return false; // nothing there yet to move
    DWORD newAttrs = GetFileAttributesW(new_path.c_str());
    if (newAttrs != INVALID_FILE_ATTRIBUTES) return false; // don't clobber an existing file
    return MoveFileW(old_path.c_str(), new_path.c_str()) != 0;
#endif
}

std::wstring ResolveSettingsPath(const AppState &state) {
    if (state.settings_path.empty()) return kDefaultSettingsPath;
    // settings_path is stored as UTF-8 (like every other free-text field in this file - see
    // OneLine()'s comment above), converted here rather than at each call site so
    // main_frame.cpp and settings_dialog.cpp can't disagree on how.
    const std::string &s = state.settings_path;
#ifndef _WIN32
    return HrWide(s);
#else
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len > 0 ? len - 1 : 0, L'\0');
    if (len > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
#endif
}

#ifndef _WIN32
const std::wstring kDefaultSettingsPath   = HrWide(hr::platform::ConfigDir() + "/homrec.hrc");
const std::wstring kOverlaysAutosavePath  = HrWide(hr::platform::ConfigDir() + "/homrec_overlays.hrc");
#endif

} // namespace HrcConfig
