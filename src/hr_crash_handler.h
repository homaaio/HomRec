// hr_crash_handler.h - last-resort crash handler.
//
// Before this, an unhandled exception (bad pointer, stack overflow, an
// exception escaping a callback wx/DXGI/ffmpeg-IPC calls into, etc.) meant
// HomRec just vanished - default Windows behavior for an unhandled SEH
// exception is either a silent process exit or the generic "HomRec has
// stopped working" WER dialog, neither of which leaves anything a user
// could actually attach to a bug report. This installs a last-resort
// handler that, on the way down, writes a .dmp (via dbghelp.dll, loaded
// dynamically so a missing/older dbghelp doesn't turn a real crash into a
// second one) and a one-line entry in the same homrec.log everything else
// already logs to, then tells the user where to find it before letting the
// process actually terminate.
//
// This is not a way to keep running after a crash - the process is already
// in an unknown state by the time this runs. It only makes the crash
// diagnosable instead of silent.
#pragma once

namespace HrCrashHandler {
    // Call once, as early as possible in startup (before any window/thread
    // that could crash exists) - see win_main.cpp.
    void Install();
}
