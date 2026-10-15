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
    "ffmpeg_not_found","file_menu","open_recordings","open_program_files","exit","view_menu",
    "always_on_top","fullscreen","pc_analytics","cpu_info","ram_info","disk_info",
    "help_menu","check_updates","report_issue","capture_source","full_desktop",
    "select_window","select_region","minimize_tray","language","english","russian","theme","dark",
    "light","settings_menu","preferences","performance_menu","ultra","turbo",
    "balanced","eco","stats","time","status","warning","error","info",
    "folder_not_exist","recording_failed","settings_saved","recording_saved",
    "open_folder","ffmpeg_not_found_msg","saved","recording_status","file","size",
    "duration","audio","merged","separate","no_audio","save","cancel","browse",
    "output_folder","settings_title","video_settings","quality","mode","advanced",
    "monitor","output","countdown","timestamp","cursor","notification","made_by","audio_file",
    "show_log",
    "hide_window","export_hrc","import_hrc","import_hrp","set_preset","overlays_panel","console","show_welcome","about",
    "tab_general","tab_video","tab_audio","tab_hotkeys","tab_security","tab_system",
    "quality_label","target_fps_label","resolution_mode_label","resolution_mode_percent",
    "resolution_mode_custom","resolution_full","add_language",
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
        {"open_recordings", "Open Recordings Folder"}, {"open_program_files", "Open Program Files"}, {"exit", "Exit"}, {"view_menu", "View"},
        {"always_on_top", "Always on Top"},
        {"fullscreen", "Fullscreen"},
        {"pc_analytics", "PC Analytics"}, {"cpu_info", "CPU Info"}, {"ram_info", "RAM Info"},
        {"disk_info", "Disk Info"}, {"help_menu", "Help"}, {"check_updates", "Check for Updates"},
        {"report_issue", "Report Issue"}, {"capture_source", "Capture Source"},
        {"full_desktop", "Full Desktop"},
        {"select_window", "Select Window to Record..."},
        {"select_region", "Select Region to Record..."},
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

        {"hide_window", "Hide Windows From Recording..."},
        {"export_hrc", "Export Settings (.hrc)..."},
        {"import_hrc", "Import Settings (.hrc)..."},
        {"import_hrp", "Import Plugin (.hrp)..."},
        {"set_preset", "Set Preset..."},
        {"overlays_panel", "Overlays Panel"},
        {"console", "Console"}, {"show_welcome", "Show Welcome Screen"}, {"about", "About"},
        {"tab_general", "General"},
        {"tab_video", "Video && Codec"}, {"tab_audio", "Audio"}, {"tab_hotkeys", "Hotkeys"},
        {"tab_security", "Security"}, {"tab_system", "System"},
        {"quality_label", "Encoding quality:"}, {"target_fps_label", "Target FPS (limit):"},
        {"resolution_mode_label", "Resolution mode:"},
        {"resolution_mode_percent", "Percent of native"},
        {"resolution_mode_custom", "Custom (width x height)"},
        {"resolution_full", "Full screen (Native, 100%)"},
        {"add_language", "Add Language..."},

        // ---- 7th pass: Layers panel / capture source (keys below are NOT in kLangRequiredKeys: a language file
        // that predates them keeps working - English, or the built-in Russian table, fills the gaps) ----
        {"layers_panel", "Layers"},
        {"src_busy", "A recording is running - it keeps the source it was started with. Stop the recording to change the capture source."},
        {"layers_empty", "No overlays yet. Click + to add one."},
        {"settings_source_moved", "What is recorded (monitor, window, region) and how a window is captured now lives in the Layers panel: double-click its first row (\"Capture source\")."},
        {"src_dialog_title", "Capture source"},
        {"src_what", "What to record"},
        {"src_desktop", "The whole desktop"},
        {"src_window", "One window"},
        {"src_region", "A region of the screen"},
        {"src_desktop_head", "Desktop"},
        {"src_monitor", "Monitor:"},
        {"src_monitor_word", "Monitor"},
        {"src_primary", "(main)"},
        {"src_window_head", "Window"},
        {"src_pick_window", "Pick window…"},
        {"src_window_none", "(no window picked yet)"},
        {"src_find_by", "How to find the window again (everything ticked must match):"},
        {"src_match_exe", "Program"},
        {"src_match_class", "Window class"},
        {"src_match_title", "Title"},
        {"src_title_exact", "is exactly"},
        {"src_title_prefix", "starts with"},
        {"src_title_contains", "contains"},
        {"src_follow_hwnd", "Keep recording the same window while it exists, even when its title changes (browser tabs)"},
        {"src_find_hint", "Tip: for a browser keep \"Program\" ticked and \"Title\" off - the title changes with every tab."},
        {"src_test_match", "Test: find it now"},
        {"src_test_found", "Found:"},
        {"src_test_notfound", "Not found right now (closed or minimized?)"},
        {"src_how", "How the window is recorded"},
        {"src_method_window", "The window itself - windows on top of it do not get into the recording"},
        {"src_method_screen", "A part of the screen - the lightest way, but anything on top of the window is recorded too"},
        {"src_area_window", "Whole window (title bar and borders included)"},
        {"src_area_client", "Only the content area"},
        {"src_smart", "Smart mode"},
        {"src_smart_hint", "While nothing covers the window it is cut out of the screen (cheap, like a desktop recording); the exact window capture is used only while something lies on top of it."},
        {"src_fps_cap", "Limit of the exact capture, frames per second (0 = as Target FPS):"},
        {"src_live_now", "Right now:"},
        {"src_live_native", "native Windows capture"},
        {"src_live_exact", "exact window capture (PrintWindow)"},
        {"src_live_smart", "smart mode - reading the screen (cheap)"},
        {"src_retry_native", "Try the native Windows capture again…"},
        {"src_retry_native_msg", "The native Windows capture (Windows.Graphics.Capture) failed or crashed on this PC earlier, so HomRec uses the compatible window capture instead.\n\nRe-enable it only to experiment. If it crashes again, HomRec switches it off by itself at the next start.\n\nTry it again?"},
        {"src_region_head", "Region"},
        {"src_pick_region", "Select region…"},
        {"src_region_none", "(no region selected yet)"},
        {"src_ok", "OK"},
        {"src_need_window", "Pick a window first (or choose another source)."},
        {"src_need_criteria", "Tick at least one way to find the window (program, class, title or \"keep recording the same window\")."},
        {"src_need_region", "Select a region first (or choose another source)."},
        {"src_row_desktop", "Desktop"},
        {"src_row_window", "Window"},
        {"src_row_region", "Region"},
        {"src_tag_native", "native"},
        {"src_tag_exact", "exact"},
        {"src_tag_smart", "smart"},
        {"src_tag_screen", "screen crop"},
        {"ctx_hide", "Hide"},
        {"ctx_show", "Show"},
        {"ctx_rename", "Rename…"},
        {"ctx_settings", "Settings…"},
        {"ctx_duplicate", "Duplicate"},
        {"ctx_forward", "Move up"},
        {"ctx_backward", "Move down"},
        {"ctx_center", "Center on the picture"},
        {"ctx_lock", "Lock position and size"},
        {"ctx_keep_prop", "Keep proportions when resizing"},
        {"ctx_delete", "Delete"},
        {"ctx_src_settings", "Capture source…"},
        {"ctx_src_desktop", "Record the whole desktop"},
        {"ctx_src_window", "Pick a window…"},
        {"ctx_src_region", "Select a region…"},
    };
}

// Russian text for the 7th pass keys: used when the language code starts with "ru" and the .hrl has no value for
// the key yet (a Russian file written before the Layers panel simply doesn't know them).
std::unordered_map<std::string, std::string> BuiltinRussianNew() {
    return {
        {"layers_panel", "Слои"},
        {"src_busy", "Идёт запись — она продолжает с тем источником, с которым началась. Останови запись, чтобы сменить источник захвата."},
        {"layers_empty", "Оверлеев пока нет. Нажмите +, чтобы добавить."},
        {"settings_source_moved", "Что записывается (монитор, окно, область) и как захватывается окно — теперь в панели «Слои»: двойной клик по первой строке («Источник захвата»)."},
        {"src_dialog_title", "Источник захвата"},
        {"src_what", "Что записывать"},
        {"src_desktop", "Весь рабочий стол"},
        {"src_window", "Одно окно"},
        {"src_region", "Область экрана"},
        {"src_desktop_head", "Рабочий стол"},
        {"src_monitor", "Монитор:"},
        {"src_monitor_word", "Монитор"},
        {"src_primary", "(основной)"},
        {"src_window_head", "Окно"},
        {"src_pick_window", "Выбрать окно…"},
        {"src_window_none", "(окно ещё не выбрано)"},
        {"src_find_by", "Как снова найти окно (должно совпасть всё, что отмечено):"},
        {"src_match_exe", "Программа"},
        {"src_match_class", "Класс окна"},
        {"src_match_title", "Название"},
        {"src_title_exact", "точно такое"},
        {"src_title_prefix", "начинается с"},
        {"src_title_contains", "содержит"},
        {"src_follow_hwnd", "Пока окно существует — писать именно его, даже если название поменялось (вкладки браузера)"},
        {"src_find_hint", "Совет: для браузера оставь «Программа» и сними «Название» — оно меняется с каждой вкладкой."},
        {"src_test_match", "Проверить: найти сейчас"},
        {"src_test_found", "Найдено:"},
        {"src_test_notfound", "Сейчас не найдено (закрыто или свёрнуто?)"},
        {"src_how", "Как записывать окно"},
        {"src_method_window", "Само окно — другие окна поверх него в запись не попадут"},
        {"src_method_screen", "Кусок экрана — самый лёгкий способ, но всё, что лежит поверх окна, тоже попадёт в запись"},
        {"src_area_window", "Всё окно (с заголовком и рамками)"},
        {"src_area_client", "Только содержимое окна"},
        {"src_smart", "Умный режим"},
        {"src_smart_hint", "Пока окно ничем не закрыто — оно вырезается из экрана (дёшево, как запись рабочего стола); точный захват окна включается только пока поверх него что-то лежит."},
        {"src_fps_cap", "Лимит точного захвата, кадров/с (0 = как Target FPS):"},
        {"src_live_now", "Сейчас:"},
        {"src_live_native", "системный захват Windows"},
        {"src_live_exact", "точный захват окна (PrintWindow)"},
        {"src_live_smart", "умный режим — читает экран (дёшево)"},
        {"src_retry_native", "Снова попробовать системный захват Windows…"},
        {"src_retry_native_msg", "Системный захват Windows (Windows.Graphics.Capture) раньше падал или не работал на этом ПК, поэтому HomRec использует совместимый захват окна.\n\nВключай снова только для эксперимента. Если он опять упадёт, HomRec сам выключит его при следующем запуске.\n\nПопробовать ещё раз?"},
        {"src_region_head", "Область"},
        {"src_pick_region", "Выбрать область…"},
        {"src_region_none", "(область ещё не выбрана)"},
        {"src_ok", "OK"},
        {"src_need_window", "Сначала выбери окно (или другой источник)."},
        {"src_need_criteria", "Отметь хотя бы один способ находить окно (программа, класс, название или «писать именно его»)."},
        {"src_need_region", "Сначала выбери область (или другой источник)."},
        {"src_row_desktop", "Рабочий стол"},
        {"src_row_window", "Окно"},
        {"src_row_region", "Область"},
        {"src_tag_native", "системный"},
        {"src_tag_exact", "точный"},
        {"src_tag_smart", "умный"},
        {"src_tag_screen", "кусок экрана"},
        {"ctx_hide", "Скрыть"},
        {"ctx_show", "Показать"},
        {"ctx_rename", "Переименовать…"},
        {"ctx_settings", "Настройки…"},
        {"ctx_duplicate", "Дублировать"},
        {"ctx_forward", "Выше"},
        {"ctx_backward", "Ниже"},
        {"ctx_center", "По центру кадра"},
        {"ctx_lock", "Закрепить положение и размер"},
        {"ctx_keep_prop", "Сохранять пропорции при изменении размера"},
        {"ctx_delete", "Удалить"},
        {"ctx_src_settings", "Источник захвата…"},
        {"ctx_src_desktop", "Записывать весь рабочий стол"},
        {"ctx_src_window", "Выбрать окно…"},
        {"ctx_src_region", "Выбрать область…"},
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

    // 7th pass keys: the built-in Russian text goes in first for any "ru..." code, so it is there even when the
    // file is missing / unreadable; a value found in the .hrl below replaces it.
    const bool is_ru = code.size() >= 2 && (code[0] == 'r' || code[0] == 'R') && (code[1] == 'u' || code[1] == 'U');
    const auto ru_new = BuiltinRussianNew();
    if (is_ru) {
        for (const auto &kv : ru_new) table.strings_[kv.first] = kv.second;
    }

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

    // 7th pass keys (not "required"): taken from the file when it has them.
    for (const auto &kv : ru_new) {
        char buf[1024] = {};
        if (hr_lang_get_value(json.c_str(), kv.first.c_str(), buf, sizeof(buf)) == 1 && buf[0] != '\0') {
            table.strings_[kv.first] = buf;
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
