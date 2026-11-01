// backend_stubs.cpp - the factories of every platform interface, answering "not implemented yet".
//
// This file is the checklist of the Linux port: each function below becomes real code in its own
// file (x11/x11_capture.cpp, wayland/portal_capture.cpp, audio_pipewire.cpp, ...) and the
// matching stub is deleted. Until then callers get nullptr / empty results plus a reason string
// that points to the plan, so `homrec doctor` can report honestly instead of crashing.
#include "../capture_backend.h"
#include "../audio_backend.h"
#include "../window_backend.h"
#include "../hotkey_backend.h"
#include "../process_backend.h"

namespace hr {
namespace platform {

namespace {

const char *kPlan = " (not implemented yet - see docs/LINUX_PORT_PLAN.md)";

void Why(std::string *out, const std::string &msg) {
    if (out) *out = msg + kPlan;
}

}  // namespace

// ---- capture ---------------------------------------------------------------------------------

CaptureCaps QueryCaptureCaps(DisplayServer ds) {
    CaptureCaps c;
    c.backend_name = "none";
    switch (ds) {
        case DisplayServer::X11:
            c.unavailable_reason = std::string("X11 capture backend (XShm / XComposite)") + kPlan;
            break;
        case DisplayServer::Wayland:
            c.unavailable_reason = std::string("Wayland capture backend (xdg-desktop-portal ScreenCast + PipeWire)") + kPlan;
            break;
        default:
            c.unavailable_reason = "no display server found ($DISPLAY / $WAYLAND_DISPLAY are not set)";
            break;
    }
    return c;
}

std::unique_ptr<IScreenCapture> CreateScreenCapture(DisplayServer ds, std::string *why_not) {
    if (why_not) *why_not = QueryCaptureCaps(ds).unavailable_reason;
    return nullptr;
}

std::vector<MonitorInfo> EnumerateMonitors(std::string *err) {
    Why(err, "monitor enumeration (XRandR / xdg-output)");
    return {};
}

// ---- audio -----------------------------------------------------------------------------------

AudioCaps QueryAudioCaps() {
    AudioCaps c;
    c.backend_name = "none";
    c.unavailable_reason = std::string("PipeWire / PulseAudio capture backend") + kPlan;
    return c;
}

std::vector<AudioDeviceInfo> EnumerateAudioDevices(AudioSourceKind, std::string *err) {
    Why(err, "audio device enumeration");
    return {};
}

std::vector<AudioAppInfo> EnumerateAudioApps(bool, std::string *err) {
    Why(err, "per-application audio enumeration");
    return {};
}

std::unique_ptr<IAudioCapture> CreateAudioCapture(std::string *why_not) {
    Why(why_not, "audio capture");
    return nullptr;
}

// ---- windows ---------------------------------------------------------------------------------

std::unique_ptr<IWindowService> CreateWindowService(DisplayServer ds, std::string *why_not) {
    if (ds == DisplayServer::Wayland) {
        if (why_not) *why_not = "Wayland does not let applications list other windows; "
                                "window capture has to go through the ScreenCast portal picker";
        return nullptr;
    }
    Why(why_not, "X11 window enumeration (EWMH)");
    return nullptr;
}

// ---- hotkeys ---------------------------------------------------------------------------------

bool HotkeysAvailable(DisplayServer ds, std::string *why_not) {
    if (ds == DisplayServer::Wayland) {
        Why(why_not, "global hotkeys via the GlobalShortcuts portal");
    } else {
        Why(why_not, "global hotkeys via XGrabKey");
    }
    return false;
}

std::unique_ptr<IHotkeyService> CreateHotkeyService(DisplayServer ds, std::string *why_not) {
    HotkeysAvailable(ds, why_not);
    return nullptr;
}

// ---- child processes -------------------------------------------------------------------------

std::unique_ptr<IChildProcess> CreateChildProcess() {
    return nullptr;   // phase 1 of the plan: fork/exec + pipes (the Linux twin of ffmpeg_runner.cpp)
}

}  // namespace platform
}  // namespace hr
