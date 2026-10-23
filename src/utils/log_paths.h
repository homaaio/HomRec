// log_paths.h - shared "where do logs live" helper.
#pragma once

#include <string>
#include <vector>

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

    // Empties one log file in place (the file stays, only its contents go). Returns 1 = cleared,
    // 0 = the file doesn't exist (nothing to do), -1 = it exists but couldn't be opened for writing.
    int TruncateLogFile(const std::wstring &path);

    // Empties every *.log file in LogsDir() (homrec, pc, plugins, wgc, ffmpeg and plugins' own custom
    // logs). Guard files (*.flag) and crash dumps are left alone. Returns how many files were
    // emptied; their names are appended to `names` when it isn't null.
    int TruncateAllLogs(std::vector<std::wstring> *names);
}
