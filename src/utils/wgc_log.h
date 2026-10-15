// wgc_log.h - dedicated log for window capture (logs\wgc.log).
//
// "Record one window" (native Windows.Graphics.Capture, the PrintWindow backend, the smart screen-crop
// mode, window matching/re-acquiring) has cost more debugging time than any other part of HomRec, and
// its story used to be scattered through homrec.log between unrelated events. Everything about it now
// goes to its own file as well: which backend was chosen and why, every switch between the cheap and
// the exact capture, how long each stage takes, window matching decisions, errors.
//
// Warn/Error lines are ALSO copied into homrec.log (they are what a bug report needs first); Info and
// Debug lines stay in wgc.log only unless mirror is requested.
//
// Thread-safe, cheap (open/append/close per call, same approach as log.cpp / plugin_log.cpp), size-capped
// (see HrLogPaths::CapFileSize). Settings > Security > "Window capture logging" switches it off.
#pragma once

#include <string>

namespace HrWgcLog {
    void SetEnabled(bool enabled);
    bool IsEnabled();

    // level: "INFO" / "WARN" / "ERROR" / "DEBUG". Lines carry a millisecond timestamp and the thread id.
    // mirror = also write the line to homrec.log (Warn and Error always do).
    void Write(const char *level, const std::string &message, bool mirror = false);

    inline void Debug(const std::string &m) { Write("DEBUG", m); }
    inline void Info(const std::string &m)  { Write("INFO", m); }
    inline void Warn(const std::string &m)  { Write("WARN", m); }
    inline void Error(const std::string &m) { Write("ERROR", m); }
    // Info line that is also copied to homrec.log (state changes a user reading homrec.log should see).
    inline void Note(const std::string &m)  { Write("INFO", m, true); }

    // Writes a one-off block describing this PC / session (Windows build, DPI, monitors, crash-guard
    // flags) the first time it is called in a process; later calls do nothing. Cheap to call repeatedly.
    void WriteSessionHeaderOnce();
}
