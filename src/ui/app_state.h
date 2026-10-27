// app_state.h - All of the recorder's session/UI state, collected into one flat struct.
// The recording controller, audio panel, settings dialog, and other pieces
// read/write this struct directly.
#pragma once

#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <windows.h>

enum class CaptureMode { Desktop, Window, Region };
enum class RecordingMode { Ultra, Turbo, Balanced, Eco };
// Both = record to .mkv (crash-safe container) and remux a stream-copied .mp4
// next to it when the recording is finalized (2.4).
enum class VideoFormat { Mp4, Mkv, Both };
// "Resolution:" setting in Settings > General - Percent keeps the old scale_factor-of-
// native behavior (25/50/75/100%), Absolute lets the user type an exact target
// width/height (e.g.
enum class ResolutionMode { Percent, Absolute };

struct OverlayDef {
    std::string id;
    std::string type;      // "text" | "image" | "webcam" | "input_overlay"
    // User-assigned display name (set via the overlays panel's right-click "Rename..." -- see
    // overlays_dock_panel.cpp).
    std::string name;
    int x = 0, y = 0, w = 0, h = 0;
    std::string text;
    std::string text_color = "#FFFFFF";  // "#RRGGBB", used for type == "text"
    // Font family for type == "text" - see overlay_render.cpp's RenderTextBgra().
    std::string font_family = "Segoe UI";
    // 0-100, applies to every overlay type (text/image/gif/webcam/
    // input_overlay) on top of whatever per-pixel alpha the overlay
    // content already has - see overlay_render.cpp's CompositeBgra().
    int opacity = 100;
    std::string image_path;              // for type == "image"; also reused for
                                          // type == "gif"'s .gif file path
    int webcam_index = -1;
    std::string webcam_name;
    bool visible = true;
    // 7th pass (Layers panel): "Lock" in the row's context menu - a locked overlay can't be
    // dragged, resized or nudged on the preview by accident (the editor dialog and the numeric
    // fields still work).
    bool locked = false;
    bool lock_aspect = false;

    // For type == "input_overlay" (the "External Overlay" import, see overlays_dock_panel.cpp
    // / input_overlay.h): a keyboard/mouse overlay driven by a JSON layout + PNG spritesheet,
    // installed from a .hrp plugin package.
    std::string input_json_path;
    std::string input_png_path;

    // 8th pass (Desktop split): "window" and "region" are LAYERS now (the Desktop layer only
    // shows the monitor).
    std::string win_title, win_exe, win_class;
    bool        win_match_exe   = true;
    bool        win_match_class = true;
    bool        win_match_title = false;
    std::string win_title_mode  = "exact";    // "exact" | "prefix" | "contains"
    bool        win_follow      = true;       // keep the very same window while its title changes
    std::string win_method      = "screen";   // "screen" (cut out of the desktop picture) | "wgc" (the window itself)
    std::string win_area        = "window";   // "window" | "client"
    int         win_fps_cap     = 0;          // exact capture only; 0 = no limit beyond the recording fps
    int         rx = 0, ry = 0, rw = 0, rh = 0;
    HWND        win_hwnd = nullptr;           // runtime only (a handle means nothing after a restart)
};

// One extra audio source in the Audio Mixer (2.4). The built-in "Microphone"
// and "Desktop Audio" strips are NOT in this list - they always exist.
struct AudioSourceDef {
    std::string id;            // "as_1", "as_2", ... (unique within the list)
    std::string kind;          // "mic" | "app" | "file"
                               // mic  = another input device (target = WASAPI endpoint id)
                               // app  = one program's sound (browser / a specific window);
    std::string name;          // label shown in the mixer
    std::string target;
    std::string window_title;
    int         volume = 100;  // 0-150 (%)
    bool        muted  = false;
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
    // GPU colour conversion: with a hardware encoder (NV12 output) and nothing to composite on
    // the CPU, crop/scale/BGRA->NV12 run on the GPU via the D3D11 video processor and only 1.5
    // bytes/pixel are read back.
    bool        gpu_convert        = true;
    // Item 9: with the GPU path, overlays (text / image / gif / webcam / input) are alpha-blended by a D3D11 pixel
    // shader BEFORE the NV12 conversion instead of per frame on the CPU. Falls back to the CPU compositor by itself
    // (no shader compiler, driver refusal, error). Terminal / cfg only (`gpu_overlay=0` = always CPU).
    bool        gpu_overlay        = true;
    // Static screen = no frame traffic: the capture pipeline sends ffmpeg a frame only when the picture
    // changed, stamped with its slot on the constant-fps timeline (NUT stream), and ffmpeg repeats the
    // previous one across the gap. Output is still constant frame rate. Terminal / cfg only
    // (`skip_static_frames=0` falls back to the raw pipe that writes every slot in full).
    bool        skip_static_frames = true;
    // ffmpeg "ddagrab" fast path (ffmpeg 6.0+, hardware encoders only): ffmpeg captures AND
    // encodes entirely on the GPU.
    bool        gpu_capture_ddagrab = false;
    //                        ddagrab, the two things it used to lack. Terminal / cfg only.
    //   ddagrab_pause    : Pause works during a ddagrab recording. ffmpeg owns the capture and cannot be paused, so Pause
    //                      ends the current ffmpeg piece and Resume starts the next one (same container as the output); Stop joins the pieces with a
    //                      stream copy (no re-encode). `ddagrab_pause=0` = Pause stays unavailable (old behaviour).
    //   ddagrab_overlays : HomRec overlays (text / image / gif / webcam / input) on a ddagrab recording. HomRec draws them
    //                      into a transparent picture that ffmpeg gets as a second input and blends onto the grabbed
    //                      frame. This needs the frames in system memory (hwdownload) for the blend - it is an opt-in
    //                      because on a weak PC the normal pipeline with GPU-blended overlays (item 9) is usually the
    //                      cheaper choice. NVENC / AMF only. `ddagrab_overlays=1` turns it on.
    bool        ddagrab_pause    = true;
    bool        ddagrab_overlays = false;
    // In-process hardware encoder (Windows Media Foundation, no ffmpeg process / pipe) for H.264 on
    // NVENC / QSV / AMF with the .mp4 container. Experimental: terminal / cfg only (`mf_encoder=1`).
    // Falls back to the ffmpeg pipe by itself when it cannot start. Audio is still merged by ffmpeg.
    bool        mf_encoder = false;
    // With mf_encoder: hand the finished NV12 GPU texture to the encoder (no GPU->CPU->GPU round trip).
    // Used only while the pointer is not drawn and no overlay is drawn by the CPU (GPU-blended overlays, item 9, are fine);
    // `mf_gpu_texture=0` turns it off.
    bool        mf_gpu_texture = true;
    bool        inproc_audio       = true;
    bool        game_friendly_priority = false;
    bool        qsv_low_power          = false;
    bool        gpu_low_priority       = false;

    int         audio_sample_rate   = 44100;
    std::string audio_aac_bitrate   = "192k";
    int         audio_out_channels  = 2;

    std::string filename_template = "HomRec_{date}_{time}";
    int         auto_stop_min      = 0;
    int         replay_buffer_sec  = 0;
    bool        replay_in_ram      = true;
    int         replay_ram_mb      = 1024;
    // Name of the preset (.hrc file, see preset_dialog.cpp) currently switched to - "default"
    // until the user ever uses File > Set Preset...
    std::string active_preset_name = "default";

    bool        scheduled_start_enabled = false;
    std::string scheduled_start_time    = "";

    bool        auto_pause_on_silence  = false;
    double      silence_threshold_db   = -40.0;
    int         silence_duration_sec   = 3;

    enum class PostRecordHookType { None, Script, Move, OpenFolder };
    bool               post_record_hook_enabled = false;
    PostRecordHookType post_record_hook_type    = PostRecordHookType::None;
    std::string        post_record_hook_path;
    // Whether Instant Replay's background buffer should be running -
    // RecordingController::EnableInstantReplay()/DisableInstantReplay()
    bool        instant_replay_enabled = false;
    VideoFormat video_format       = VideoFormat::Mp4;
    bool        separate_audio_mp3 = false;
    int         level_meter_fps    = 30;
    // 2.4: Audio Mixer look. "horizontal" (bars next to the slider, the
    // pre-2.4 look) or "vertical" (OBS-style channel strips, meter beside a
    // vertical fader).
    std::string audio_meter_style  = "horizontal";
    // 2.4: extra mixer channels (another mic, a browser, a window, an audio
    // file). Persisted by HrcConfig in its own [audio_sources] section.
    std::vector<AudioSourceDef> audio_sources;

    // -- 2.4 window capture ----------------------------------------------
    // How a selected window is captured:
    std::string window_capture_method = "screen";
    bool WindowCaptureWantsWgc() const { return window_capture_method == "wgc"; }
    // What part of the window is recorded: "window" (title bar + borders +
    // content - the WHOLE window) or "client" (only the content area).
    std::string window_capture_area   = "window";

    // -- 7th pass capture source (Layers panel > Capture source...) ------------------------------------
    // How the recorded window is FOUND again (after a restart, or when its handle is gone). Window
    // titles change constantly (every browser tab switch), so the title is only one optional criterion:
    std::string capture_window_exe;
    std::string capture_window_class;
    bool        window_match_exe   = true;
    bool        window_match_class = true;
    bool        window_match_title = false;
    std::string window_title_mode  = "exact";
    bool        window_follow_hwnd = true;
    // "Smart" window capture (only used with window_capture_method == "wgc", PrintWindow backend):
    bool        window_capture_smart = true;
    // Upper limit (frames/s) for the exact per-window capture, which is expensive. 0 = Target FPS.
    int         window_capture_fps_cap = 0;
    // Settings > Security: logs/wgc.log (window capture diagnostics).
    bool        wgc_logging_enabled = true;

    // -- 2.4 dockable UI ---------------------------------------------------
    // Freezes the panel layout (no dragging, floating or resizing of panels).
    bool        ui_locked = false;
    // wxAuiManager perspective, hex-encoded so it survives the .hrc format.
    // Empty = default layout.
    std::string ui_layout;

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
                                       // In-app console (console_window.cpp):
    bool console_hom_log      = false;
    bool console_history_save = true;
    bool cmd_sound            = false; // Windows "ding" when Enter is pressed in the console (terminal/cfg only, not in the Settings window)
    // Live-preview panel (not the recording itself): how big and how often
    // it's redrawn. Lower values cost real CPU/GPU time on weak machines
    // even though they never end up in the actual recording.
    int  preview_quality_pct = 100; // 25/50/75/100 - % of preview_width/height below to actually render at
    int  preview_fps         = 15;  // how many times/sec the preview thumbnail is refreshed

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
    // 8th pass: since the Desktop split this stays Desktop (window / region recording = layers, see
    // OverlayDef::type "window" / "region"). Old config files that still say window / region are converted
    // to a layer when they are loaded (HrMigrateCaptureSourceToLayers).
    CaptureMode  capture_mode  = CaptureMode::Desktop;
    // The Desktop layer's eye: false = the monitor picture is not drawn (the other layers still can use it
    // as their source, e.g. a region layer).
    bool         desktop_visible = true;
    std::string  capture_window_title;
    // The exact HWND the person picked in File > Select Window...
    HWND         capture_window_hwnd = nullptr;
    // -- Region capture (todo2.3.md section 3, "Захват произвольной области экрана")
    // --------------------------------------------------- Virtual-desktop pixel coordinates
    // (same coordinate space as a window's RECT from GetWindowRect - i.e.
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
    // mic_enum.h's HrEnumerateMics() and settings_dialog.cpp's picker).
    std::string mic_device_id;
    bool        first_launch = false;

    // -- Phase 1 settings-storage migration (see commands.md / hrc_config.h)
    // Empty = use HrcConfig::kDefaultSettingsPath ("homrec.hrc" next to the
    // exe). Settings dialog's Advanced tab lets this be pointed elsewhere;
    std::string settings_path;

    // Logical-name -> HWND registry, for looking up windows/controls by name.
    std::unordered_map<std::string, HWND> ui_registry;
};
