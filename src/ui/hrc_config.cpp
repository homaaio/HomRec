#include "hrc_config.h"

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

bool ToBool(const std::string &v) { return v == "1" || v == "true" || v == "yes"; }
std::string FromBool(bool b) { return b ? "1" : "0"; }

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

std::string RecordingModeToStr(RecordingMode m) {
    switch (m) {
        case RecordingMode::Ultra: return "ultra";
        case RecordingMode::Turbo: return "turbo";
        case RecordingMode::Eco:   return "eco";
        default:                  return "balanced";
    }
}
RecordingMode RecordingModeFromStr(const std::string &s) {
    if (s == "ultra") return RecordingMode::Ultra;
    if (s == "turbo") return RecordingMode::Turbo;
    if (s == "eco")   return RecordingMode::Eco;
    return RecordingMode::Balanced;
}

std::string CaptureModeToStr(CaptureMode m) { return m == CaptureMode::Window ? "window" : "desktop"; }
CaptureMode CaptureModeFromStr(const std::string &s) { return s == "window" ? CaptureMode::Window : CaptureMode::Desktop; }

std::string VideoFormatToStr(VideoFormat f) { return f == VideoFormat::Mkv ? "mkv" : "mp4"; }
VideoFormat VideoFormatFromStr(const std::string &s) { return s == "mkv" ? VideoFormat::Mkv : VideoFormat::Mp4; }

} // namespace

namespace HrcConfig {

bool Save(const AppState &state, const std::wstring &path) {
    std::ofstream f(path.c_str(), std::ios::trunc | std::ios::binary);
    if (!f) return false;

    f << "# HomRec Config (.hrc) v1\n"
      << "# Lines starting with # are comments. Format: key=value\n\n";

    f << "[identity]\n"
      << "language=" << state.current_language << "\n"
      << "theme=" << state.current_theme << "\n"
      << "ui_font=" << state.ui_font << "\n"
      << "ui_scale=" << state.ui_scale << "\n\n";

    f << "[window]\n"
      << "window_w=" << state.window_w << "\n"
      << "window_h=" << state.window_h << "\n\n";

    f << "[capture]\n"
      << "output_folder=" << state.output_folder << "\n"
      << "quality=" << state.quality << "\n"
      << "target_fps=" << state.target_fps << "\n"
      << "scale_factor=" << state.scale_factor << "\n"
      << "resolution_mode=" << (state.resolution_mode == ResolutionMode::Absolute ? 1 : 0) << "\n"
      << "resolution_w=" << state.resolution_w << "\n"
      << "resolution_h=" << state.resolution_h << "\n"
      << "recording_mode=" << RecordingModeToStr(state.recording_mode) << "\n"
      << "show_summary=" << FromBool(state.show_summary) << "\n"
      << "monitor_id=" << state.monitor_id << "\n"
      << "capture_mode=" << CaptureModeToStr(state.capture_mode) << "\n"
      << "capture_window_title=" << state.capture_window_title << "\n"
      << "preview_width=" << state.preview_width << "\n"
      << "preview_height=" << state.preview_height << "\n"
      << "preview_quality_pct=" << state.preview_quality_pct << "\n"
      << "preview_fps=" << state.preview_fps << "\n"
      << "disable_preview=" << FromBool(state.disable_preview) << "\n\n";

    f << "[video]\n"
      << "video_codec=" << state.video_codec << "\n"
      << "hw_accel=" << state.hw_accel << "\n"
      << "enc_preset=" << state.enc_preset << "\n"
      << "enc_crf=" << state.enc_crf << "\n"
      << "custom_ffmpeg_args=" << state.custom_ffmpeg_args << "\n"
      << "pix_fmt=" << state.pix_fmt << "\n"
      << "video_format=" << VideoFormatToStr(state.video_format) << "\n\n";

    f << "[audio]\n"
      << "audio_sample_rate=" << state.audio_sample_rate << "\n"
      << "audio_aac_bitrate=" << state.audio_aac_bitrate << "\n"
      << "audio_out_channels=" << state.audio_out_channels << "\n"
      << "separate_audio_mp3=" << FromBool(state.separate_audio_mp3) << "\n"
      << "mic_device_id=" << state.mic_device_id << "\n\n";

    f << "[hotkeys]\n"
      << "hotkey_start_stop=" << state.hotkey_start_stop << "\n"
      << "hotkey_pause=" << state.hotkey_pause << "\n"
      << "hotkey_fullscreen=" << state.hotkey_fullscreen << "\n\n";

    f << "[recording_extra]\n"
      << "filename_template=" << state.filename_template << "\n"
      << "auto_stop_min=" << state.auto_stop_min << "\n"
      << "replay_buffer_sec=" << state.replay_buffer_sec << "\n\n";

    f << "[ui_toggles]\n"
      << "always_on_top=" << FromBool(state.always_on_top) << "\n"
      << "minimize_to_tray=" << FromBool(state.minimize_to_tray) << "\n"
      << "countdown_enabled=" << FromBool(state.countdown_enabled) << "\n"
      << "timestamp_enabled=" << FromBool(state.timestamp_enabled) << "\n"
      << "cursor_enabled=" << FromBool(state.cursor_enabled) << "\n"
      << "show_audio_panel=" << FromBool(state.show_audio_panel) << "\n"
      << "show_overlays_panel=" << FromBool(state.show_overlays_panel) << "\n"
      << "notify_sound=" << FromBool(state.notify_sound) << "\n"
      << "notify_flash=" << FromBool(state.notify_flash) << "\n"
      << "hint_no_overlay=" << FromBool(state.hint_no_overlay) << "\n";

    f << "[security]\n"
      << "system_logging_enabled=" << FromBool(state.system_logging_enabled) << "\n"
      << "plugin_logging_enabled=" << FromBool(state.plugin_logging_enabled) << "\n\n";

    // BUGFIX: overlays used to not be saved at all -- anything set up in
    // the Overlays panel silently vanished the moment the profile was
    // reloaded (or the app restarted), with no warning that it was about
    // to happen. Serialized here with one flat "overlay_N_field" key per
    // field (rather than a real [overlay:N] section) because Load() below
    // parses this whole file into one flat key/value map and ignores
    // section headers entirely -- giving every overlay's "x" key the same
    // name would just have the last one clobber all the others.
    f << "[overlays]\n"
      << "overlay_count=" << state.overlays.size() << "\n\n";
    for (size_t i = 0; i < state.overlays.size(); ++i) {
        const OverlayDef &ov = state.overlays[i];
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

    return true;
}

bool Load(AppState &state, const std::wstring &path) {
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

    auto has = [&](const char *k) { return kv.find(k) != kv.end(); };
    auto get = [&](const char *k) -> const std::string & { return kv[k]; };

    if (has("language")) state.current_language = get("language");
    if (has("theme")) state.current_theme = get("theme");
    if (has("ui_font")) state.ui_font = get("ui_font");
    if (has("ui_scale")) state.ui_scale = atof(get("ui_scale").c_str());

    if (has("window_w")) state.window_w = atoi(get("window_w").c_str());
    if (has("window_h")) state.window_h = atoi(get("window_h").c_str());

    if (has("output_folder")) state.output_folder = get("output_folder");
    if (has("quality")) state.quality = atoi(get("quality").c_str());
    if (has("target_fps")) state.target_fps = atoi(get("target_fps").c_str());
    if (has("scale_factor")) state.scale_factor = atof(get("scale_factor").c_str());
    // BUGFIX: see the matching comment in Save() -- resolution_mode/_w/_h
    // and the preview_* fields below weren't round-tripped through .hrc
    // profiles at all before this fix.
    if (has("resolution_mode")) state.resolution_mode =
        (atoi(get("resolution_mode").c_str()) != 0) ? ResolutionMode::Absolute : ResolutionMode::Percent;
    if (has("resolution_w")) state.resolution_w = atoi(get("resolution_w").c_str());
    if (has("resolution_h")) state.resolution_h = atoi(get("resolution_h").c_str());
    if (has("recording_mode")) state.recording_mode = RecordingModeFromStr(get("recording_mode"));
    if (has("show_summary")) state.show_summary = ToBool(get("show_summary"));
    if (has("monitor_id")) state.monitor_id = atoi(get("monitor_id").c_str());
    if (has("capture_mode")) state.capture_mode = CaptureModeFromStr(get("capture_mode"));
    if (has("capture_window_title")) state.capture_window_title = get("capture_window_title");
    if (has("preview_width")) state.preview_width = atoi(get("preview_width").c_str());
    if (has("preview_height")) state.preview_height = atoi(get("preview_height").c_str());
    if (has("preview_quality_pct")) state.preview_quality_pct = atoi(get("preview_quality_pct").c_str());
    if (has("preview_fps")) state.preview_fps = atoi(get("preview_fps").c_str());
    if (has("disable_preview")) state.disable_preview = ToBool(get("disable_preview"));

    if (has("video_codec")) state.video_codec = get("video_codec");
    if (has("hw_accel")) state.hw_accel = get("hw_accel");
    if (has("enc_preset")) state.enc_preset = get("enc_preset");
    if (has("enc_crf")) state.enc_crf = atoi(get("enc_crf").c_str());
    if (has("custom_ffmpeg_args")) state.custom_ffmpeg_args = get("custom_ffmpeg_args");
    if (has("pix_fmt")) state.pix_fmt = get("pix_fmt");
    if (has("video_format")) state.video_format = VideoFormatFromStr(get("video_format"));

    if (has("audio_sample_rate")) state.audio_sample_rate = atoi(get("audio_sample_rate").c_str());
    if (has("audio_aac_bitrate")) state.audio_aac_bitrate = get("audio_aac_bitrate");
    if (has("audio_out_channels")) state.audio_out_channels = atoi(get("audio_out_channels").c_str());
    if (has("separate_audio_mp3")) state.separate_audio_mp3 = ToBool(get("separate_audio_mp3"));
    if (has("mic_device_id")) state.mic_device_id = get("mic_device_id");

    if (has("hotkey_start_stop")) state.hotkey_start_stop = get("hotkey_start_stop");
    if (has("hotkey_pause")) state.hotkey_pause = get("hotkey_pause");
    if (has("hotkey_fullscreen")) state.hotkey_fullscreen = get("hotkey_fullscreen");

    if (has("filename_template")) state.filename_template = get("filename_template");
    if (has("auto_stop_min")) state.auto_stop_min = atoi(get("auto_stop_min").c_str());
    if (has("replay_buffer_sec")) state.replay_buffer_sec = atoi(get("replay_buffer_sec").c_str());

    if (has("always_on_top")) state.always_on_top = ToBool(get("always_on_top"));
    if (has("minimize_to_tray")) state.minimize_to_tray = ToBool(get("minimize_to_tray"));
    if (has("countdown_enabled")) state.countdown_enabled = ToBool(get("countdown_enabled"));
    if (has("timestamp_enabled")) state.timestamp_enabled = ToBool(get("timestamp_enabled"));
    if (has("cursor_enabled")) state.cursor_enabled = ToBool(get("cursor_enabled"));
    if (has("show_audio_panel")) state.show_audio_panel = ToBool(get("show_audio_panel"));
    if (has("show_overlays_panel")) state.show_overlays_panel = ToBool(get("show_overlays_panel"));
    if (has("notify_sound")) state.notify_sound = ToBool(get("notify_sound"));
    if (has("notify_flash")) state.notify_flash = ToBool(get("notify_flash"));
    if (has("hint_no_overlay")) state.hint_no_overlay = ToBool(get("hint_no_overlay"));

    if (has("system_logging_enabled")) state.system_logging_enabled = ToBool(get("system_logging_enabled"));
    if (has("plugin_logging_enabled")) state.plugin_logging_enabled = ToBool(get("plugin_logging_enabled"));

    // BUGFIX: see the matching comment in Save() -- overlays weren't
    // persisted at all before.
    state.overlays.clear();
    if (has("overlay_count")) {
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
            state.overlays.push_back(ov);
        }
    }

    return true;
}

} // namespace HrcConfig
