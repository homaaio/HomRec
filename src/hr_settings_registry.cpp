#include "hr_settings_registry.h"
#include "hr_str_convert.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace {

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Small helpers so the table below reads as "field, key, section" instead
// of a wall of repeated lambda boilerplate for the common cases.
using Def = HrSettingsRegistry::SettingDef;

Def StrField(std::string key, std::string section, std::string AppState::*field,
             std::vector<std::string> aliases = {}, bool sensitive = false) {
    Def d;
    d.key = std::move(key);
    d.section = std::move(section);
    d.aliases = std::move(aliases);
    d.kind = HrSettingsRegistry::Kind::String;
    d.sensitive = sensitive;
    d.get = [field](const AppState &s) { return s.*field; };
    d.set = [field](AppState &s, const std::string &v) { s.*field = v; return true; };
    return d;
}

Def IntField(std::string key, std::string section, int AppState::*field,
             std::vector<std::string> aliases = {}) {
    Def d;
    d.key = std::move(key);
    d.section = std::move(section);
    d.aliases = std::move(aliases);
    d.kind = HrSettingsRegistry::Kind::Int;
    d.get = [field](const AppState &s) { return std::to_string(s.*field); };
    d.set = [field](AppState &s, const std::string &v) {
        if (v.empty()) return false;
        s.*field = atoi(v.c_str());
        return true;
    };
    return d;
}

Def DblField(std::string key, std::string section, double AppState::*field,
             std::vector<std::string> aliases = {}) {
    Def d;
    d.key = std::move(key);
    d.section = std::move(section);
    d.aliases = std::move(aliases);
    d.kind = HrSettingsRegistry::Kind::Double;
    d.get = [field](const AppState &s) { return std::to_string(s.*field); };
    d.set = [field](AppState &s, const std::string &v) {
        if (v.empty()) return false;
        s.*field = atof(v.c_str());
        return true;
    };
    return d;
}

Def BoolField(std::string key, std::string section, bool AppState::*field,
              std::vector<std::string> aliases = {}) {
    Def d;
    d.key = std::move(key);
    d.section = std::move(section);
    d.aliases = std::move(aliases);
    d.kind = HrSettingsRegistry::Kind::Bool;
    d.get = [field](const AppState &s) { return HrFromBool(s.*field); };
    d.set = [field](AppState &s, const std::string &v) { s.*field = HrToBool(v); return true; };
    return d;
}

} // namespace

namespace HrSettingsRegistry {

const std::vector<SettingDef> &All() {
    static const std::vector<SettingDef> kAll = [] {
        std::vector<SettingDef> v;

        // -- [identity] --------------------------------------------------
        v.push_back(StrField("language", "identity", &AppState::current_language));
        v.push_back(StrField("theme", "identity", &AppState::current_theme));
        v.push_back(StrField("ui_font", "identity", &AppState::ui_font));
        v.push_back(DblField("ui_scale", "identity", &AppState::ui_scale));

        // -- [storage] (Phase 1 settings-storage migration, see commands.md) --
        v.push_back(StrField("settings_path", "storage", &AppState::settings_path));

        // -- [window] ------------------------------------------------------
        v.push_back(IntField("window_w", "window", &AppState::window_w));
        v.push_back(IntField("window_h", "window", &AppState::window_h));

        // -- [capture] -------------------------------------------------------
        v.push_back(StrField("output_folder", "capture", &AppState::output_folder));
        v.push_back(IntField("quality", "capture", &AppState::quality));
        v.push_back(IntField("target_fps", "capture", &AppState::target_fps));
        v.push_back(DblField("scale_factor", "capture", &AppState::scale_factor));
        {
            Def d;
            d.key = "resolution_mode"; d.section = "capture";
            d.kind = HrSettingsRegistry::Kind::Enum;
            d.get = [](const AppState &s) { return HrResolutionModeToStr(s.resolution_mode); };
            d.set = [](AppState &s, const std::string &v) { s.resolution_mode = HrResolutionModeFromStr(v); return true; };
            v.push_back(d);
        }
        v.push_back(IntField("resolution_w", "capture", &AppState::resolution_w));
        v.push_back(IntField("resolution_h", "capture", &AppState::resolution_h));
        {
            Def d;
            d.key = "recording_mode"; d.section = "capture";
            d.kind = HrSettingsRegistry::Kind::Enum;
            d.get = [](const AppState &s) { return HrRecordingModeToStr(s.recording_mode); };
            d.set = [](AppState &s, const std::string &v) { s.recording_mode = HrRecordingModeFromStr(v); return true; };
            v.push_back(d);
        }
        v.push_back(BoolField("show_summary", "capture", &AppState::show_summary));
        v.push_back(IntField("monitor_id", "capture", &AppState::monitor_id));
        {
            Def d;
            d.key = "capture_mode"; d.section = "capture";
            d.kind = HrSettingsRegistry::Kind::Enum;
            d.get = [](const AppState &s) { return HrCaptureModeToStr(s.capture_mode); };
            d.set = [](AppState &s, const std::string &v) { s.capture_mode = HrCaptureModeFromStr(v); return true; };
            v.push_back(d);
        }
        v.push_back(StrField("capture_window_title", "capture", &AppState::capture_window_title));
        v.push_back(IntField("preview_width", "capture", &AppState::preview_width));
        v.push_back(IntField("preview_height", "capture", &AppState::preview_height));
        v.push_back(IntField("preview_quality_pct", "capture", &AppState::preview_quality_pct));
        v.push_back(IntField("preview_fps", "capture", &AppState::preview_fps));
        v.push_back(BoolField("disable_preview", "capture", &AppState::disable_preview));

        // -- [video] ---------------------------------------------------------
        v.push_back(StrField("video_codec", "video", &AppState::video_codec));
        v.push_back(StrField("hw_accel", "video", &AppState::hw_accel));
        v.push_back(StrField("enc_preset", "video", &AppState::enc_preset));
        v.push_back(IntField("enc_crf", "video", &AppState::enc_crf));
        // Sensitive: written verbatim onto ffmpeg's real command line (see
        // hr_ffmpeg_runner.cpp) - gated behind "sec 0" for unattended/
        // scripted writes (console assignment, cfg files, sethrc's import),
        // same as HrcConfig::Load()'s allow_sensitive_fields already did.
        v.push_back(StrField("custom_ffmpeg_args", "video", &AppState::custom_ffmpeg_args, {}, /*sensitive=*/true));
        v.push_back(StrField("pix_fmt", "video", &AppState::pix_fmt));
        {
            Def d;
            d.key = "video_format"; d.section = "video";
            d.kind = HrSettingsRegistry::Kind::Enum;
            d.get = [](const AppState &s) { return HrVideoFormatToStr(s.video_format); };
            d.set = [](AppState &s, const std::string &v) { s.video_format = HrVideoFormatFromStr(v); return true; };
            v.push_back(d);
        }

        // -- [audio] ---------------------------------------------------------
        v.push_back(IntField("audio_sample_rate", "audio", &AppState::audio_sample_rate));
        v.push_back(StrField("audio_aac_bitrate", "audio", &AppState::audio_aac_bitrate));
        v.push_back(IntField("audio_out_channels", "audio", &AppState::audio_out_channels));
        v.push_back(BoolField("separate_audio_mp3", "audio", &AppState::separate_audio_mp3));
        v.push_back(StrField("mic_device_id", "audio", &AppState::mic_device_id));

        // -- [hotkeys] -------------------------------------------------------
        v.push_back(StrField("hotkey_start_stop", "hotkeys", &AppState::hotkey_start_stop));
        v.push_back(StrField("hotkey_pause", "hotkeys", &AppState::hotkey_pause));
        v.push_back(StrField("hotkey_fullscreen", "hotkeys", &AppState::hotkey_fullscreen));

        // -- [recording_extra] -------------------------------------------------
        v.push_back(StrField("filename_template", "recording_extra", &AppState::filename_template));
        v.push_back(IntField("auto_stop_min", "recording_extra", &AppState::auto_stop_min));
        v.push_back(IntField("replay_buffer_sec", "recording_extra", &AppState::replay_buffer_sec));

        // -- [ui_toggles] ------------------------------------------------------
        v.push_back(BoolField("always_on_top", "ui_toggles", &AppState::always_on_top));
        // alias: lua_api.cpp's old plugin-facing name was "minimize_tray".
        v.push_back(BoolField("minimize_to_tray", "ui_toggles", &AppState::minimize_to_tray, {"minimize_tray"}));
        // alias: old plugin-facing name was "countdown".
        v.push_back(BoolField("countdown_enabled", "ui_toggles", &AppState::countdown_enabled, {"countdown"}));
        // alias: old plugin-facing name was "timestamp".
        v.push_back(BoolField("timestamp_enabled", "ui_toggles", &AppState::timestamp_enabled, {"timestamp"}));
        // alias: old plugin-facing name was "cursor".
        v.push_back(BoolField("cursor_enabled", "ui_toggles", &AppState::cursor_enabled, {"cursor"}));
        v.push_back(BoolField("show_audio_panel", "ui_toggles", &AppState::show_audio_panel));
        v.push_back(BoolField("show_overlays_panel", "ui_toggles", &AppState::show_overlays_panel));
        v.push_back(BoolField("notify_sound", "ui_toggles", &AppState::notify_sound));
        v.push_back(BoolField("notify_flash", "ui_toggles", &AppState::notify_flash));
        v.push_back(BoolField("hint_no_overlay", "ui_toggles", &AppState::hint_no_overlay));

        // -- [security] --------------------------------------------------------
        v.push_back(BoolField("system_logging_enabled", "security", &AppState::system_logging_enabled));
        v.push_back(BoolField("plugin_logging_enabled", "security", &AppState::plugin_logging_enabled));

        // -- [system] ------------------------------------------------------------
        // NEW section: desktop_shortcut_enabled/_path and autostart_enabled
        // were real AppState fields with real Settings-dialog checkboxes,
        // but were never round-tripped through .hrc at all (Save()/Load()
        // simply never mentioned them) - so a profile export/import, or
        // this new console/cfg mechanism, silently couldn't touch them.
        // Persisting the flag here just records the user's last choice;
        // it does NOT re-run the actual shortcut/registry-autostart side
        // effect on Load() (that only happens from the Settings dialog's
        // explicit Apply/OK and the Welcome wizard - see
        // HrSystemIntegration's call sites in settings_dialog.cpp/
        // welcome_dialog.cpp), so loading an .hrc with autostart_enabled=1
        // reflects that this profile *wants* autostart, without silently
        // reaching into the Windows registry on your behalf just because
        // a cfg file loaded.
        v.push_back(BoolField("desktop_shortcut_enabled", "system", &AppState::desktop_shortcut_enabled));
        v.push_back(StrField("desktop_shortcut_path", "system", &AppState::desktop_shortcut_path));
        v.push_back(BoolField("autostart_enabled", "system", &AppState::autostart_enabled));

        return v;
    }();
    return kAll;
}

const SettingDef *Find(const std::string &key) {
    std::string k = ToLowerAscii(key);
    for (const auto &def : All()) {
        if (def.key == k) return &def;
        for (const auto &alias : def.aliases) {
            if (alias == k) return &def;
        }
    }
    return nullptr;
}

} // namespace HrSettingsRegistry
