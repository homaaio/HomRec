// app_state.h — Phase 1
//
// Direct port of the field set that `HomRecScreen.__init__` builds up in
// homrec_app/app.py. In the Python version these are plain instance
// attributes (plus a few tk.Variable wrappers); here they are one flat
// struct. Later phases (recording controller, audio panel, settings dialog)
// read/write this struct directly instead of going through Tk variable
// traces.
//
// Deliberately NOT using tk.Variable-style indirection: Win32 controls pull
// state on WM_COMMAND / WM_NOTIFY and push it back on user action, so we
// don't need an observer pattern here.
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
    std::string type;      // "text" | "image" | "webcam"
    int x = 0, y = 0, w = 0, h = 0;
    std::string text;
    std::string image_path;
    int webcam_index = -1;
    bool visible = true;
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

    // -- UI toggles (were tk.BooleanVar in Python) -----------------------
    bool always_on_top      = false;
    bool minimize_to_tray   = true;
    bool countdown_enabled  = true;
    bool timestamp_enabled  = false;
    bool cursor_enabled     = false;
    bool show_audio_panel    = true;
    bool show_overlays_panel = true;

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
    bool        first_launch = false;

    // Logical-name -> HWND registry, replaces Python's `ui_registry: dict`.
    std::unordered_map<std::string, HWND> ui_registry;
};
