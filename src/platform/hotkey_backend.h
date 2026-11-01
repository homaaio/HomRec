// hotkey_backend.h - global hotkeys (start/stop, pause, save replay, custom bindings).
//
// Windows: RegisterHotKey on a message thread (src/core/hotkey.cpp). Linux/X11: XGrabKey.
// Linux/Wayland: apps cannot grab keys - the org.freedesktop.portal.GlobalShortcuts portal is the
// only sanctioned way and is not available on every compositor (Available() tells).
//
// Key strings use the format HomRec already stores in .hrc files: "F9", "Ctrl+Shift+R", "Alt+F10".
#pragma once

#include "platform.h"

#include <memory>
#include <string>

namespace hr {
namespace platform {

// Called on the service's own thread when binding `id` was pressed.
typedef void (*HotkeyCallback)(void *user, int id);

class IHotkeyService {
public:
    virtual ~IHotkeyService() = default;

    // Same rule as hr_hk_*: register everything, then Start(). Register() fails (false + *err) for
    // an unparsable string or a combination another program already owns.
    virtual bool Register(int id, const std::string &keystring, std::string *err) = 0;
    virtual void Unregister(int id) = 0;
    virtual void SetCallback(HotkeyCallback cb, void *user) = 0;
    virtual bool Start() = 0;
    virtual void Stop() = 0;
};

// Is a global-hotkey mechanism usable in this session? (*why_not filled when not.)
bool HotkeysAvailable(DisplayServer ds, std::string *why_not);
std::unique_ptr<IHotkeyService> CreateHotkeyService(DisplayServer ds, std::string *why_not);

}  // namespace platform
}  // namespace hr
