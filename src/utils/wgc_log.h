// wgc_log.h - dedicated log for window capture (logs\wgc.log).
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
