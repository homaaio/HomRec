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
#include <utility>
#include <unordered_map>
#include <windows.h>

enum class CaptureMode { Desktop, Window, Region };
enum class RecordingMode { Ultra, Turbo, Balanced, Eco };
enum class VideoFormat { Mp4, Mkv };
// "Resolution:" setting in Settings > General - Percent keeps the old
// scale_factor-of-native behavior (25/50/75/100%), Absolute lets the user
// type an exact target width/height (e.g. 1280x720) instead. See
// RecordingController::ComputeOutputDims().
enum class ResolutionMode { Percent, Absolute };

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
    // Font family for type == "text" - see hr_overlay_render.cpp's
    // RenderTextBgra(). If the named font isn't actually installed,
    // Windows' GDI silently substitutes its own default rather than
    // failing, so an unavailable choice here degrades gracefully instead
    // of breaking the overlay.
    std::string font_family = "Segoe UI";
    // 0-100, applies to every overlay type (text/image/gif/webcam/
    // input_overlay) on top of whatever per-pixel alpha the overlay
    // content already has - see hr_overlay_render.cpp's CompositeBgra().
    // 100 = fully opaque (the old, only, behavior before this existed).
    int opacity = 100;
    std::string image_path;              // for type == "image"; also reused for
                                          // type == "gif"'s .gif file path
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
    double         scale_factor       = 0.75;
    ResolutionMode resolution_mode    = ResolutionMode::Percent;
    // Only used when resolution_mode == Absolute; ignored otherwise.
    // Defaults to a common "old machine friendly" 720p target.
    int            resolution_w       = 1280;
    int            resolution_h       = 720;
    std::string   output_folder;                 // set to <root>/recordings at startup
    int           quality            = 70;
    int           target_fps         = 15;
    RecordingMode recording_mode     = RecordingMode::Balanced;
    bool          show_summary       = true;

    std::string hotkey_start_stop = "F9";
    std::string hotkey_pause      = "F10";
    std::string hotkey_fullscreen = "F11";
    std::string hotkey_save_replay = "F8"; // Instant Replay - see recording_controller.h's SaveReplay()
    std::vector<std::pair<std::string, std::string>> custom_hotkeys;
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
    // Name of the preset (.hrc file, see preset_dialog.cpp) currently
    // switched to - "default" until the user ever uses File > Set
    // Preset... or the console's `preset <name>` command (see
    // console_window.cpp's CmdPreset). Purely descriptive: switching
    // presets already fully applies the target file's own settings via
    // HrcConfig::Load(); this field just remembers *which one* for the
    // {preset} filename-template token and for `status`/`preset` to
    // report back.
    std::string active_preset_name = "default";

    // -- Scheduled recording start (todo2.3.md section 3) -----------------
    // Separate from auto_stop_min (which ends an already-running
    // recording after N minutes): this instead delays the *start*.
    // scheduled_start_time is "HH:MM" (24h, local time); enabling it with
    // a time already in the past today is treated as "tomorrow at that
    // time" by the checker (main_frame.cpp), not "start immediately".
    bool        scheduled_start_enabled = false;
    std::string scheduled_start_time    = "";

    // -- Auto-pause/resume on microphone silence (todo2.3.md section 3) ---
    // Independent of scheduled_start above; both can be used together
    // (start at a set time, then only actually record while someone's
    // talking). silence_threshold_db is the RMS level (dBFS, so this is
    // normally a negative number, e.g. -40) below which the mic is
    // considered silent; silence_duration_sec is how long it has to stay
    // that quiet before RecordingController auto-pauses, so normal short
    // gaps between sentences don't constantly toggle pause on/off.
    bool        auto_pause_on_silence  = false;
    double      silence_threshold_db   = -40.0;
    int         silence_duration_sec   = 3;

    // -- Post-recording hook (todo2.3.md section 3) ------------------------
    // Runs once StopFinalizeTail() has produced the final output file.
    // "script" launches post_record_hook_path with the finished file's
    // full path as argv[1] (fire-and-forget, not awaited - see
    // RecordingController::RunPostRecordHook()); "move" moves the file
    // into post_record_hook_path (a folder) instead of leaving it in
    // output_folder; "open_folder" just opens output_folder in Explorer,
    // same as the existing "Open Folder" button, so post_record_hook_path
    // is unused for that mode.
    enum class PostRecordHookType { None, Script, Move, OpenFolder };
    bool               post_record_hook_enabled = false;
    PostRecordHookType post_record_hook_type    = PostRecordHookType::None;
    std::string        post_record_hook_path;
    // Whether Instant Replay's background buffer should be running -
    // RecordingController::EnableInstantReplay()/DisableInstantReplay()
    // are the actual on/off switch; this is just the persisted "user
    // wants it on" choice, applied once at startup (see main_frame.cpp).
    bool        instant_replay_enabled = false;
    VideoFormat video_format       = VideoFormat::Mp4;
    bool        separate_audio_mp3 = false;
    int         level_meter_fps    = 30;

    // -- UI toggles -------------------------------------------------------
    bool always_on_top      = false;
    bool minimize_to_tray   = true;
    bool countdown_enabled  = true;
    bool timestamp_enabled  = false;
    bool cursor_enabled     = false;
    bool show_audio_panel    = true;
    bool show_overlays_panel = true;
    bool disable_preview     = false; // skip capturing/rendering the live preview, for lower-end machines
    bool hint_no_overlay     = true;  // show the "don't see your overlay?" hint under the disabled-preview ':)' -
                                       // togglable from the bter plugin console ("edit settings hint-no-overlay false")
    // Live-preview panel (not the recording itself): how big and how often
    // it's redrawn. Lower values cost real CPU/GPU time on weak machines
    // even though they never end up in the actual recording.
    int  preview_quality_pct = 100; // 25/50/75/100 - % of preview_width/height below to actually render at
    int  preview_fps         = 15;  // how many times/sec the preview thumbnail is refreshed

    // -- Settings > Security -----------------------------------------------
    // Independent on/off switches for the two always-on-by-default log
    // files (see hr_pc_log.h / hr_plugin_log.h) - homrec.log itself
    // (hr_log.h, events/errors) isn't covered by either of these, only
    // the periodic hardware-sampling log and the plugin-system log.
    bool system_logging_enabled = true; // logs/pc.log
    bool plugin_logging_enabled = true; // logs/plugins.log

    // -- Settings > System (the Welcome wizard no longer has its own copy
    // of this step - now offered instead as install-time Tasks in
    // installer/HomRec.iss) ------------------------------------------------
    bool        desktop_shortcut_enabled = false;
    // Empty = "use the real Desktop folder"
    // (HrSystemIntegration::GetDefaultDesktopPath()) - only set to
    // something else when the user picks a custom folder via Browse.
    std::string desktop_shortcut_path;
    bool        autostart_enabled = false;

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
    // The exact HWND the person picked in File > Select Window... Runtime-
    // only (deliberately NOT in HrSettingsRegistry - a handle is meaningless
    // after a restart, where capture_window_title is all that's left to go
    // on). Window titles change constantly (browser tabs, "*Untitled" in an
    // editor, "Recording..." in chat apps), so resolving by exact title
    // alone used to silently miss the window between picking it and pressing
    // Start, which quietly fell back to recording the whole desktop.
    // HR_ResolveCaptureWindow() tries this handle first and only falls back
    // to the title when it's null or the window is gone.
    HWND         capture_window_hwnd = nullptr;
    // -- Region capture (todo2.3.md section 3, "Захват произвольной
    // области экрана") ---------------------------------------------------
    // Virtual-desktop pixel coordinates (same coordinate space as a
    // window's RECT from GetWindowRect - i.e. NOT relative to any one
    // monitor), set by the region-picker overlay (see
    // ShowRegionPickerOverlay() in window_picker_dialog.h/.cpp) or
    // hand-edited in homrec.hrc. Only meaningful when capture_mode is
    // CaptureMode::Region; ResolveCaptureSize() (recording_controller.cpp)
    // turns this into the same crop_x_/y_/w_/h_ pipeline that window
    // capture already uses, so region capture gets monitor-resolution,
    // hardware-encoder, and overlay support for free instead of needing
    // a second capture path.
    int          region_x = 0, region_y = 0, region_w = 0, region_h = 0;
    std::vector<HWND> hidden_capture_windows;

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

    // -- Phase 1 settings-storage migration (see commands.md / hrc_config.h)
    // Empty = use HrcConfig::kDefaultSettingsPath ("homrec.hrc" next to the
    // exe). Settings dialog's Advanced tab lets this be pointed elsewhere;
    // whichever path is here is both read at startup and written on Save.
    // std::string (not std::wstring) to match every other free-text field
    // .hrc already serializes (capture_window_title, desktop_shortcut_path,
    // etc.) - converted to std::wstring only at the point of use, same as
    // those.
    std::string settings_path;

    // Logical-name -> HWND registry, for looking up windows/controls by name.
    std::unordered_map<std::string, HWND> ui_registry;
};
