// app_state.h
//
// All of the recorder's session/UI state, collected into one flat struct.
// The recording controller, audio panel, settings dialog, and other pieces
// read/write this struct directly.
//
// Deliberately not using variable-indirection/observer patterns: Win32
// controls pull state on WM_COMMAND / WM_NOTIFY and push it back on user
// action, so there's no need for one here.
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <windows.h>

enum class CaptureMode { Desktop, Window };
enum class RecordingMode { Ultra, Turbo, Balanced, Eco };
enum class VideoFormat { Mp4, Mkv };

struct OverlayDef {
    std::string id;
    std::string type;      // "text" | "image" | "webcam" | "input_overlay"
    // User-assigned display name (set via the overlays panel's right-click
    // "Rename..." -- see overlays_dock_panel.cpp). Empty means "no custom
    // name set yet", in which case the panel falls back to its old
    // auto-generated label (truncated text/filename/cam index).
    std::string name;
    int x = 0, y = 0, w = 0, h = 0;
    std::string text;
    std::string text_color = "#FFFFFF";  // "#RRGGBB", used for type == "text"
    std::string image_path;
    int webcam_index = -1;
    // Friendly device name captured when the webcam was picked from the
    // enumerated device list (see hr_webcam_enum.h / overlays_dock_panel.cpp's
    // AddWebcamOverlay) -- purely cosmetic (shown in the panel row and the
    // "Edit Parameters" dialog instead of a bare index), never fed back into
    // the actual capture; webcam_index above is still what's used to open
    // the device. Empty for overlays added before this existed.
    std::string webcam_name;
    bool visible = true;

    // For type == "input_overlay" (the "External Overlay" import, see
    // overlays_dock_panel.cpp / hr_input_overlay.h): a keyboard/mouse
    // overlay driven by a JSON layout + PNG spritesheet, installed from a
    // .hrp plugin package. input_json_path/input_png_path point at the
    // extracted files under plugins/input_overlays/<name>/.
    std::string input_json_path;
    std::string input_png_path;
};

struct AppState {
    // -- identity / language / theme -----------------------------------
    std::string current_language = "en";
    std::string current_theme    = "dark";     // "dark" | "light"
    std::string ui_font          = "Segoe UI";
    double      ui_scale         = 1.0;

    // -- window geometry (mirrors root.geometry("1300x750") / minsize) --
    int window_w = 1300, window_h = 750;
    int window_min_w = 1200, window_min_h = 650;

    // -- capture / recording settings ------------------------------------
    double        scale_factor       = 0.75;
    std::string   output_folder;                 // set to <root>/recordings at startup
    int           quality            = 70;
    int           target_fps         = 15;
    RecordingMode recording_mode     = RecordingMode::Balanced;
    bool          show_summary       = true;

    std::string hotkey_start_stop = "F9";
    std::string hotkey_pause      = "F10";
    std::string hotkey_fullscreen = "F11";
    bool        notify_sound      = true;
    bool        notify_flash      = true;
    bool        auto_save_profile = false;

    std::string video_codec        = "libx264";
    std::string hw_accel           = "auto";
    std::string enc_preset         = "ultrafast";
    int         enc_crf            = 18;
    std::string custom_ffmpeg_args;
    std::string pix_fmt            = "yuv420p";

    int         audio_sample_rate   = 44100;
    std::string audio_aac_bitrate   = "192k";
    int         audio_out_channels  = 2;

    std::string filename_template = "HomRec_{date}_{time}";
    int         auto_stop_min      = 0;
    int         replay_buffer_sec  = 0;
    VideoFormat video_format       = VideoFormat::Mp4;
    bool        separate_audio_mp3 = false;

    // -- UI toggles -------------------------------------------------------
    bool always_on_top      = false;
    bool minimize_to_tray   = true;
    bool countdown_enabled  = true;
    bool timestamp_enabled  = false;
    bool cursor_enabled     = false;
    bool show_audio_panel    = true;
    bool show_overlays_panel = true;
    bool disable_preview     = false; // skip capturing/rendering the live preview, for lower-end machines

    // -- runtime / recording status --------------------------------------
    bool   recording    = false;
    bool   paused       = false;
    long   frame_count  = 0;
    double start_time   = 0.0;
    double last_frame_time = 0.0;
    bool   stop_flag    = false;

    // -- monitor / capture source -----------------------------------------
    int          monitor_id    = 1;
    int          monitor_left  = 0;
    int          monitor_top   = 0;
    CaptureMode  capture_mode  = CaptureMode::Desktop;
    std::string  capture_window_title;

    // -- preview ------------------------------------------------------------
    int preview_width  = 900;
    int preview_height = 500;

    // -- overlays -------------------------------------------------------------
    std::vector<OverlayDef> overlays;

    // -- misc -------------------------------------------------------------------
    std::string ffmpeg_path;      // resolved at startup via hr_find_ffmpeg
    // WASAPI endpoint ID of the microphone to record from (see
    // hr_mic_enum.h's HrEnumerateMics() and settings_dialog.cpp's picker).
    // Empty (the default) keeps the previous behavior of always using
    // whichever device Windows currently considers the default recording
    // device.
    std::string mic_device_id;
    bool        first_launch = false;

    // Logical-name -> HWND registry, for looking up windows/controls by name.
    std::unordered_map<std::string, HWND> ui_registry;
};
