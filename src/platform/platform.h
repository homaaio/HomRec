// platform.h - compile-time and run-time platform identification for HomRec's portable layer.
//
// This is the entry point of src/platform/. The headers next to it (capture_backend.h,
// audio_backend.h, window_backend.h, hotkey_backend.h, process_backend.h, paths.h) describe WHAT
// the engine needs from the operating system; src/platform/<os>/ provides HOW. Nothing in the
// existing Windows code includes these headers yet - they are the target the Windows code will be
// migrated onto step by step (see docs/LINUX_PORT_PLAN.md).
#pragma once

#include <string>

#if defined(_WIN32)
  #define HR_PLATFORM_WINDOWS 1
  #define HR_PLATFORM_LINUX   0
#elif defined(__linux__)
  #define HR_PLATFORM_WINDOWS 0
  #define HR_PLATFORM_LINUX   1
#else
  #error "HomRec: unsupported platform (only Windows and Linux are planned)"
#endif

namespace hr {
namespace platform {

// Which windowing system the current session runs on. On Linux this is a RUN-time property
// (one binary has to cope with both X11 and Wayland sessions), on Windows it is always Win32.
enum class DisplayServer {
    Unknown,   // headless / tty / SSH without a display
    Win32,
    X11,
    Wayland,
};

// Which sound server is reachable. Linux only; Windows reports WASAPI.
enum class AudioServer {
    None,
    Wasapi,
    PipeWire,
    PulseAudio,   // a real PulseAudio daemon (pipewire-pulse is reported as PipeWire)
};

struct SessionInfo {
    DisplayServer display_server    = DisplayServer::Unknown;
    bool          xwayland_available = false;   // Wayland session that also has $DISPLAY (X clients work)
    AudioServer   audio_server      = AudioServer::None;
    std::string   desktop;                      // $XDG_CURRENT_DESKTOP, e.g. "GNOME", "KDE" (may be empty)
    std::string   session_type_raw;             // $XDG_SESSION_TYPE as found (may be empty)
};

const char *DisplayServerName(DisplayServer ds);   // "x11", "wayland", "win32", "unknown"
const char *AudioServerName(AudioServer as);       // "pipewire", "pulseaudio", "wasapi", "none"
const char *PlatformName();                        // "windows" / "linux"

// Looks at the running session. Cheap (environment variables + a couple of stat() calls), never
// throws, never talks to the display server, safe to call from any thread and as often as needed.
SessionInfo DetectSession();

// Convenience: DetectSession().display_server.
DisplayServer DetectDisplayServer();

    }  // namespace platform
}  // namespace hr
