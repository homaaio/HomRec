// hr_log.h — app-wide file logger.
//
// Before this, the only thing that ever wrote to homrec.log was the
// console's manual "$log <message>" command — nothing in the app logged
// automatically, so Help > Log was always empty unless a user typed a
// console command first. This gives every part of the app a one-line call
// to record real events (recording start/stop/errors, settings saves,
// startup) into the same homrec.log the Log Viewer already reads.
#pragma once

#include <string>

namespace HrLog {
    // level is a short plain tag, e.g. "INFO", "WARN", "ERROR" — kept as a
    // free-form string rather than an enum so call sites stay readable.
    // Appends one timestamped UTF-8 line to <exe-dir>\homrec.log. Cheap
    // (opens, appends, closes — no held handle or background thread), so
    // it's safe to call from occasional UI-thread events without adding
    // steady-state overhead to the capture/encode path.
    void Write(const char *level, const std::string &message);

    inline void Info(const std::string &message)  { Write("INFO", message); }
    inline void Warn(const std::string &message)  { Write("WARN", message); }
    inline void Error(const std::string &message) { Write("ERROR", message); }
}
