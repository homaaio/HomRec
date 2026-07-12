#include "language.h"
#include <windows.h>
#include <vector>
#include <sstream>

extern "C" {
    int hr_hrc_read(const char *path, int expected_type, char *out_json, int out_len);
    int hr_lang_get_value(const char *json_body, const char *key, char *out, int out_len);
    int hr_lang_count_missing_keys(const char *json_body, const char *required_keys);
}

// Ported verbatim from LANG_REQUIRED_KEYS in homrec_app/core/constants.py.
const std::vector<std::string> kLangRequiredKeys = {
    "app_title","live_preview","ready","recording","paused","fps","resolution",
    "start","pause","stop","resume","recording_btn","audio_mixer","microphone",
    "desktop_audio","mute","unmute","vol","level","enable_audio","ffmpeg_found",
    "ffmpeg_not_found","file_menu","open_recordings","exit","view_menu",
    "always_on_top","fullscreen","pc_analytics","cpu_info","ram_info","disk_info",
    "help_menu","check_updates","report_issue","capture_source","full_desktop",
    "select_window","minimize_tray","language","english","russian","theme","dark",
    "light","settings_menu","preferences","performance_menu","ultra","turbo",
    "balanced","eco","stats","time","status","warning","error","info",
    "folder_not_exist","recording_failed","settings_saved","recording_saved",
    "open_folder","ffmpeg_not_found_msg","saved","recording_status","file","size",
    "duration","audio","merged","separate","no_audio","save","cancel","browse",
    "output_folder","settings_title","video_settings","quality","mode","advanced",
    "monitor","output","countdown","timestamp","cursor","notification","made_by","audio_file",
};

namespace {

// homrec_app/core/languages.py LANGUAGES["en"]. NOTE: this hardcodes
// "app_title": "HomRec v1.7.1" to match the Python source exactly today —
// see version.h for the plan to route this through one shared version
// constant instead of a literal here.
std::unordered_map<std::string, std::string> BuiltinEnglish() {
    return {
        {"app_title", "HomRec v1.7.1"}, {"live_preview", "PREVIEW"}, {"ready", "Ready"},
        {"recording", "Recording"}, {"paused", "Paused"}, {"fps", "FPS:"}, {"resolution", "Resolution:"},
        {"start", "\u25B6 START"}, {"pause", "\u23F8 PAUSE"}, {"stop", "\u25A0 STOP"}, {"resume", "\u25B6 RESUME"},
        {"recording_btn", "\u23FA RECORDING"}, {"audio_mixer", "Audio Mixer"}, {"microphone", "Microphone"},
        {"desktop_audio", "Desktop Audio"}, {"mute", "Mute"}, {"unmute", "Unmute"}, {"vol", "Vol:"},
        {"level", "Level:"}, {"enable_audio", "Enable Audio"}, {"ffmpeg_found", "FFmpeg: \u2705 Found"},
        {"ffmpeg_not_found", "FFmpeg: \u274C Not Found"}, {"file_menu", "File"},
        {"open_recordings", "Open Recordings Folder"}, {"exit", "Exit"}, {"view_menu", "View"},
        {"always_on_top", "Always on Top"}, {"fullscreen", "Fullscreen  F11"},
        {"pc_analytics", "PC Analytics"}, {"cpu_info", "CPU Info"}, {"ram_info", "RAM Info"},
        {"disk_info", "Disk Info"}, {"help_menu", "Help"}, {"check_updates", "Check for Updates"},
        {"report_issue", "Report Issue"}, {"capture_source", "Capture Source"},
        {"full_desktop", "Full Desktop"}, {"select_window", "Select Window..."},
        {"minimize_tray", "Minimize to tray on close"}, {"language", "Language"},
        {"english", "English"}, {"russian", "\u0420\u0443\u0441\u0441\u043A\u0438\u0439"}, {"theme", "Theme"}, {"dark", "Dark"},
        {"light", "Light"}, {"settings_menu", "Settings"}, {"preferences", "Preferences..."},
        {"performance_menu", "Performance"}, {"ultra", "Ultra (60 FPS)"}, {"turbo", "Turbo (30 FPS)"},
        {"balanced", "Balanced (15 FPS)"}, {"eco", "Eco (8 FPS)"}, {"stats", "STATS"},
        {"time", "TIME"}, {"status", "STATUS"}, {"warning", "Warning"}, {"error", "Error"},
        {"info", "Info"}, {"folder_not_exist", "Folder doesn't exist!"},
        {"recording_failed", "Recording failed!"}, {"settings_saved", "Settings saved!"},
        {"recording_saved", "\u2705 Recording Saved!"}, {"open_folder", "Open folder?"},
        {"ffmpeg_not_found_msg", "\u26A0\uFE0F FFmpeg not found - audio separate"},
        {"saved", "\u2705 Saved: {size:.1f} MB | {duration:.1f}s"},
        {"recording_status", "Recording: {size:.1f} MB | {frames} frames"},
        {"file", "\U0001F4C1 File:"}, {"size", "\U0001F4CA Size:"}, {"duration", "\u23F1\uFE0F Duration:"},
        {"audio", "\U0001F3A4 Audio:"}, {"merged", "Merged"}, {"separate", "Separate"}, {"no_audio", "No"},
        {"save", "Save"}, {"cancel", "Cancel"}, {"browse", "Browse"},
        {"output_folder", "Output folder:"}, {"settings_title", "Settings"},
        {"video_settings", "Video"}, {"quality", "Quality:"}, {"mode", "Mode:"},
        {"advanced", "Advanced"}, {"monitor", "Monitor:"}, {"output", "Output:"},
        {"countdown", "Countdown (3s)"}, {"timestamp", "Timestamp"}, {"cursor", "Cursor"},
        {"notification", "Show summary"}, {"made_by", "Homa4ella"}, {"audio_file", "\U0001F3B5 Audio file:"},
        {"show_log", "Show Log"},
    };
}

std::string JoinRequiredKeysNullSeparated() {
    // hr_lang_count_missing_keys expects a null-separated, double-null
    // terminated list — build that byte layout from kLangRequiredKeys.
    std::string blob;
    for (const auto &k : kLangRequiredKeys) {
        blob.append(k);
        blob.push_back('\0');
    }
    blob.push_back('\0');
    return blob;
}

} // namespace

LanguageTable LanguageTable::Load(const std::string &code, const std::string &langsDir) {
    LanguageTable table;
    table.strings_ = BuiltinEnglish();
    if (code == "en") return table;

    std::string path = langsDir + "\\" + code + ".hrl";

    // file_type 1 == HRL per hr_hrc_write's convention.
    int needed = hr_hrc_read(path.c_str(), 1, nullptr, 0);
    if (needed >= 0) return table; // not found / bad magic -> English fallback

    std::vector<char> json(-needed);
    if (hr_hrc_read(path.c_str(), 1, json.data(), (int)json.size()) <= 0) return table;

    // Overlay every required key found in the file on top of the English
    // defaults, exactly like `result = dict(LANGUAGES["en"]); result.update(data)`.
    for (const auto &key : kLangRequiredKeys) {
        char buf[512] = {};
        if (hr_lang_get_value(json.data(), key.c_str(), buf, sizeof(buf)) == 1 && buf[0] != '\0') {
            table.strings_[key] = buf;
        }
    }

    std::string required_blob = JoinRequiredKeysNullSeparated();
    int missing = hr_lang_count_missing_keys(json.data(), required_blob.c_str());
    if (missing > 0) {
        std::ostringstream msg;
        msg << "Language " << code << ": " << missing << " missing keys\n";
        OutputDebugStringA(msg.str().c_str());
    }
    return table;
}

std::vector<std::pair<std::string, std::string>> LanguageTable::ScanCustomLanguages(const std::string &langsDir) {
    std::vector<std::pair<std::string, std::string>> result;
    std::string pattern = langsDir + "\\*.hrl";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return result;
    do {
        std::string fname = fd.cFileName;
        std::string code = fname.substr(0, fname.size() - 4); // strip ".hrl"
        std::string full = langsDir + "\\" + fname;

        int needed = hr_hrc_read(full.c_str(), 1, nullptr, 0);
        std::string display_name = code;
        if (needed < 0) {
            std::vector<char> json(-needed);
            if (hr_hrc_read(full.c_str(), 1, json.data(), (int)json.size()) > 0) {
                char buf[256] = {};
                if (hr_lang_get_value(json.data(), "lang_name", buf, sizeof(buf)) == 1 && buf[0] != '\0') {
                    display_name = buf;
                }
            }
        }
        result.emplace_back(code, display_name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return result;
}

const std::string &LanguageTable::Get(const std::string &key) const {
    static const std::string kEmpty;
    auto it = strings_.find(key);
    return it != strings_.end() ? it->second : kEmpty;
}
