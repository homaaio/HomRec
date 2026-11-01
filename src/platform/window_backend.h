// window_backend.h - listing and querying top-level windows (the "Select Window..." picker, window
// layers that follow a program, "which program is this sound from").
//
// X11 can do all of this (EWMH). Wayland deliberately cannot: a client may not list or inspect other
// clients' windows. There CreateWindowService() returns nullptr and window capture goes through the
// ScreenCast portal's own picker instead (see capture_backend.h, needs_user_prompt).
#pragma once

#include "platform.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hr {
namespace platform {

struct WindowInfo {
    uint64_t    id  = 0;           // X11 window id (HWND on Windows)
    uint64_t    pid = 0;
    std::string title;             // UTF-8 (_NET_WM_NAME)
    std::string exe;               // executable base name, "firefox"
    std::string wm_class;          // WM_CLASS class part - the stable matcher on Linux (Windows: window class)
    int         x = 0, y = 0, w = 0, h = 0;   // frame rectangle in screen pixels
    bool        visible   = true;
    bool        minimized = false;
};

class IWindowService {
public:
    virtual ~IWindowService() = default;

    // Visible top-level windows with a title, HomRec's own excluded, top of the z-order first.
    virtual std::vector<WindowInfo> EnumerateWindows() = 0;
    virtual bool Query(uint64_t id, WindowInfo *out) = 0;     // false = the window is gone
    virtual bool IsAlive(uint64_t id) = 0;
};

// nullptr when the session cannot list windows (Wayland, headless); *why_not says why.
std::unique_ptr<IWindowService> CreateWindowService(DisplayServer ds, std::string *why_not);

}  // namespace platform
}  // namespace hr
