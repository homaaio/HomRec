// crash_handler.h - last-resort crash handler.
#pragma once

namespace HrCrashHandler {
    // Call once, as early as possible in startup (before any window/thread
    // that could crash exists) - see win_main.cpp.
    void Install();
}
