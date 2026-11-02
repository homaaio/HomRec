// session_detect.cpp - which display server / sound server is this Linux session running?
// Environment variables and a couple of stat() calls only: never connects to X or Wayland.
#include "../platform.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

namespace hr {
namespace platform {

namespace {

std::string Env(const char *name) {
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string Lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool Exists(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

AudioServer DetectAudio() {
    // pipewire-pulse also creates pulse/native, so PipeWire has to be checked first.
    std::string run = Env("XDG_RUNTIME_DIR");
    if (run.empty()) return AudioServer::None;
    if (Exists(run + "/pipewire-0")) return AudioServer::PipeWire;
    if (Exists(run + "/pulse/native")) return AudioServer::PulseAudio;
    return AudioServer::None;
}

}  // namespace

const char *DisplayServerName(DisplayServer ds) {
    switch (ds) {
        case DisplayServer::Win32:   return "win32";
        case DisplayServer::X11:     return "x11";
        case DisplayServer::Wayland: return "wayland";
        default:                     return "unknown";
    }
}

const char *AudioServerName(AudioServer as) {
    switch (as) {
        case AudioServer::Wasapi:     return "wasapi";
        case AudioServer::PipeWire:   return "pipewire";
        case AudioServer::PulseAudio: return "pulseaudio";
        default:                      return "none";
    }
}

const char *PlatformName() { return "linux"; }

SessionInfo DetectSession() {
    SessionInfo s;
    s.session_type_raw = Env("XDG_SESSION_TYPE");
    s.desktop          = Env("XDG_CURRENT_DESKTOP");
    s.audio_server     = DetectAudio();

    const bool has_wayland = !Env("WAYLAND_DISPLAY").empty();
    const bool has_x11     = !Env("DISPLAY").empty();
    const std::string type = Lower(s.session_type_raw);

    // $XDG_SESSION_TYPE is what the login manager decided; WAYLAND_DISPLAY wins when it disagrees
    // with a leftover "x11" (e.g. a terminal started inside a nested compositor).
    if (type == "wayland" || (has_wayland && type != "x11")) {
        s.display_server     = DisplayServer::Wayland;
        s.xwayland_available = has_x11;
    } else if (type == "x11" || has_x11) {
        s.display_server = DisplayServer::X11;
    } else {
        s.display_server = DisplayServer::Unknown;
    }
    return s;
}

DisplayServer DetectDisplayServer() { return DetectSession().display_server; }

}  // namespace platform
}  // namespace hr
