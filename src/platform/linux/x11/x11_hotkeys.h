// x11_hotkeys.h - IHotkeyService on X11 (XGrabKey), phase 2 of plan-linux.md.
#pragma once

#include "../../hotkey_backend.h"

namespace hr {
namespace platform {

// Same key strings as the Windows parser (hr_hk_parse_keystring in src/core/hotkey.cpp):
// modifiers Ctrl/Control, Shift, Alt, Win/Super (case-insensitive), then F1..F24, Escape, Space,
// Return/Enter, Tab, Backspace, Delete, Insert, Home, End, Prior/PageUp, Next/PageDown, the arrow
// keys, or one letter / digit. Unlike Windows, an unknown modifier is an error, not ignored.
// *mods is an X11 modifier mask (ShiftMask, ControlMask, Mod1Mask = Alt, Mod4Mask = Win/Super),
// *keysym an X11 KeySym. Does not talk to the display.
bool ParseHotkeyString(const std::string &s, unsigned *mods, unsigned long *keysym, std::string *err);

// Opens its own connection to $DISPLAY. nullptr + *why_not when that fails.
// Register() / Unregister() work before Start() (and after Stop()); Unregister() while running does
// nothing. The callback runs on the service's thread.
std::unique_ptr<IHotkeyService> CreateX11HotkeyService(std::string *why_not);

// True when $DISPLAY can be opened.
bool X11HotkeysAvailable(std::string *why_not);

}  // namespace platform
}  // namespace hr
