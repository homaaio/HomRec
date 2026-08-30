#include "language.h"
#include "version.h"
#include <windows.h>
#include <vector>
#include <sstream>
#include <cstdio>

extern "C" {
    int hr_hrc_read(const char *path, int expected_type, char *out_json, int out_len);
    int hr_hrc_write(const char *path, const char *json_body, int file_type);
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

std::unordered_map<std::string, std::string> BuiltinEnglish() {
    return {
        {"app_title", "HomRec v" HR_APP_VERSION}, {"live_preview", "PREVIEW"}, {"ready", "Ready"},
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
    // terminated list - build that byte layout from kLangRequiredKeys.
    std::string blob;
    for (const auto &k : kLangRequiredKeys) {
        blob.append(k);
        blob.push_back('\0');
    }
    blob.push_back('\0');
    return blob;
}

bool ReadLanguageJson(const std::string &path, std::string &outJson, bool *outWasHrl = nullptr) {
    int needed = hr_hrc_read(path.c_str(), 1, nullptr, 0);
    if (needed < 0) {
        std::vector<char> buf(-needed);
        if (hr_hrc_read(path.c_str(), 1, buf.data(), (int)buf.size()) > 0) {
            outJson = buf.data();
            if (outWasHrl) *outWasHrl = true;
            return true;
        }
    }

    if (outWasHrl) *outWasHrl = false;

    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string raw;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) raw.append(chunk, n);
    fclose(f);

    size_t i = 0;
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) i = 3;
    while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\r' || raw[i] == '\n')) ++i;

    if (i >= raw.size() || raw[i] != '{') return false;
    outJson = raw;
    return true;
}

// CreateDirectoryA only ever creates the *last* path component - it
// fails outright if any parent is missing too, which "Assets" always
// was on a fresh checkout (nothing before ImportLanguageFile below ever
// had a reason to create it; every other reader of "Assets\L" only
// ever read from it, never created it). Walks `dir` component by
// component, creating each one, so "Assets\L" succeeds even when
// neither "Assets" nor "Assets\L" exist yet.
bool EnsureDirectoryRecursive(const std::string &dir) {
    std::string partial;
    size_t pos = 0;
    while (pos < dir.size()) {
        size_t next = dir.find_first_of("\\/", pos);
        std::string component = (next == std::string::npos) ? dir.substr(pos) : dir.substr(pos, next - pos);
        partial += component;
        if (!partial.empty()) {
            if (!CreateDirectoryA(partial.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
        }
        if (next == std::string::npos) break;
        partial += dir[next];
        pos = next + 1;
    }
    return true;
}

} // namespace

LanguageTable LanguageTable::Load(const std::string &code, const std::string &langsDir) {
    LanguageTable table;
    table.strings_ = BuiltinEnglish();
    if (code == "en") return table;

    std::string path = langsDir + "\\" + code + ".hrl";

    std::string json;
    if (!ReadLanguageJson(path, json)) return table; // not found / unreadable -> English fallback

    // Overlay every required key found in the file on top of the English
    // defaults, exactly like `result = dict(LANGUAGES["en"]); result.update(data)`.
    for (const auto &key : kLangRequiredKeys) {
        char buf[512] = {};
        if (hr_lang_get_value(json.c_str(), key.c_str(), buf, sizeof(buf)) == 1 && buf[0] != '\0') {
            table.strings_[key] = buf;
        }
    }

    std::string required_blob = JoinRequiredKeysNullSeparated();
    int missing = hr_lang_count_missing_keys(json.c_str(), required_blob.c_str());
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

        std::string json;
        std::string display_name = code;
        if (ReadLanguageJson(full, json)) {
            char buf[256] = {};
            if (hr_lang_get_value(json.c_str(), "lang_name", buf, sizeof(buf)) == 1 && buf[0] != '\0') {
                display_name = buf;
            }
        }
        result.emplace_back(code, display_name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return result;
}

namespace {

// Keeps only [a-z0-9_] (ASCII), lower-cased, so the result is always a
// safe filename component and a safe .hrl "code" - matches what
// ScanCustomLanguages()/Load() above expect (langsDir + "\" + code +
// ".hrl", no path separators or exotic characters allowed through).
std::string SanitizeLangCode(const std::string &raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') out.push_back(c);
    }
    return out;
}

std::string DeriveCodeFromPath(const std::string &srcPath) {
    // Strip directory.
    std::string base = srcPath;
    size_t slash = base.find_last_of("\\/");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    // Strip extension.
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);

    std::string code = SanitizeLangCode(base);
    if (code.empty() || code == "en") code = "custom";
    return code;
}

} // namespace

bool LanguageTable::ImportLanguageFile(const std::string &srcPath, const std::string &langsDir,
                                        std::string &outCode, std::string &outDisplayName,
                                        std::string &outError) {
    std::string json;
    bool wasHrl = false;
    if (!ReadLanguageJson(srcPath, json, &wasHrl)) {
        outError = "Not a valid language file - expected either a HomRec .hrl "
                   "export, or a plain JSON file with the language's keys.";
        return false;
    }

    std::string code = DeriveCodeFromPath(srcPath);

    // langsDir ("Assets\L") may not exist yet on a fresh install, and
    // neither may "Assets" itself - this is the first thing that ever
    // writes there, everything else so far has only ever read from it.
    if (!EnsureDirectoryRecursive(langsDir)) {
        outError = "Couldn't create the \"" + langsDir + "\" folder. Check that "
                    "HomRec has permission to create folders next to hr.exe, or "
                    "create it by hand and try again.";
        return false;
    }

    std::string destPath = langsDir + "\\" + code + ".hrl";
    bool wrote;
    if (wasHrl) {
        // Already in the on-disk .hrl format - copy the bytes as-is.
        wrote = CopyFileA(srcPath.c_str(), destPath.c_str(), FALSE) != 0;
    } else {
        // Plain JSON someone typed by hand - wrap it into a proper .hrl
        // right now (file_type 1, same convention Load() expects), so
        // what ends up on disk looks exactly like something the app
        // itself wrote, and any future re-scan/re-import of this exact
        // file sees a normal gzip HRL rather than needing this same
        // plain-JSON fallback again.
        wrote = hr_hrc_write(destPath.c_str(), json.c_str(), 1) != 0;
    }
    if (!wrote) {
        outError = "Failed to write the language file into " + langsDir + ".";
        return false;
    }

    outCode = code;
    char nameBuf[256] = {};
    if (hr_lang_get_value(json.c_str(), "lang_name", nameBuf, sizeof(nameBuf)) == 1 && nameBuf[0] != '\0') {
        outDisplayName = nameBuf;
    } else {
        outDisplayName = code;
    }
    return true;
}

const std::string &LanguageTable::Get(const std::string &key) const {
    static const std::string kEmpty;
    auto it = strings_.find(key);
    return it != strings_.end() ? it->second : kEmpty;
}
