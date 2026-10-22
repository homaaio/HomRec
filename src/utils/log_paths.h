// log_paths.h - shared "where do logs live" helper.
#pragma once

#include <string>

namespace HrLogPaths {
    std::wstring LogsDir();

    // LogsDir() + "\" + filename. Does NOT sanitize filename - callers
    // that accept a filename from outside the app's own code (i.e.
    // plugins, see homrec.log_to() in lua_api.cpp) must sanitize first;
    std::wstring LogFilePath(const std::wstring &filename);

    std::wstring SanitizeLogFilename(const std::wstring &requested);

    // Keeps a log file from growing without bound over a long-running session - every logger
    // (homrec.log, pc.log, plugins.log, and plugins' own custom files via homrec.log_to())
    // calls this right before it appends a line.
    void CapFileSize(const std::wstring &path, long long maxBytes);
}
