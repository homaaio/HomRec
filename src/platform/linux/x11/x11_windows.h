// x11_windows.h - IWindowService on X11 (EWMH), phase 2 of plan-linux.md.
#pragma once

#include "../../window_backend.h"

namespace hr {
namespace platform {

// Opens its own connection to $DISPLAY. nullptr + *why_not when that fails.
// The service may be used from one thread at a time (calls are serialised by a mutex).
std::unique_ptr<IWindowService> CreateX11WindowService(std::string *why_not);

}  // namespace platform
}  // namespace hr
