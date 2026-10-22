// log.h - app-wide file logger.
#pragma once

#include <string>

namespace HrLog {
    // level is a short plain tag, e.g. "INFO", "WARN", "ERROR" - kept as a
    // free-form string rather than an enum so call sites stay readable.
    void Write(const char *level, const std::string &message);

    inline void Info(const std::string &message)  { Write("INFO", message); }
    inline void Warn(const std::string &message)  { Write("WARN", message); }
    inline void Error(const std::string &message) { Write("ERROR", message); }
}
