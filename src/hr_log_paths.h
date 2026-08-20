// hr_log_paths.h - shared "where do logs live" helper.
//
// Every logger in the app (hr_log.cpp's homrec.log, hr_pc_log.cpp's
// pc.log, hr_plugin_log.cpp's plugins.log, the crash handler's note-in-
// homrec.log, the console window's "log --tail" etc, and now plugins'
// own custom log files via homrec.log_to()) used to each hard-code
// "<exe-dir>\homrec.log" independently. Pulling the directory resolution
// into one place means moving everything into a logs\ subfolder (this
// change) is a one-line edit here instead of a dozen scattered ones, and
// keeps every caller agreeing on the same path from now on.
#pragma once

#include <string>

namespace HrLogPaths {
    // <exe-dir>\logs - created on first call if it doesn't exist yet
    // (CreateDirectoryW on an already-existing directory is a harmless
    // no-op, so this is safe to call from every logger on every write
    // without tracking "did I already make this" state anywhere).
    std::wstring LogsDir();

    // LogsDir() + "\" + filename. Does NOT sanitize filename - callers
    // that accept a filename from outside the app's own code (i.e.
    // plugins, see homrec.log_to() in lua_api.cpp) must sanitize first;
    // this helper is also used internally with fixed, trusted names
    // (homrec.log, pc.log, plugins.log) where sanitizing would be
    // pointless overhead.
    std::wstring LogFilePath(const std::wstring &filename);

    // Strips path separators, drive letters, and ".." segments down to a
    // single safe filename component, and falls back to "plugin.log" if
    // that leaves nothing usable - the sanitizer homrec.log_to() runs
    // every plugin-supplied filename through before ever touching disk,
    // so a plugin can't point a write outside logs\ (e.g.
    // "..\..\startup.bat") or at another plugin's/the app's own log
    // files it has no business touching.
    std::wstring SanitizeLogFilename(const std::wstring &requested);

    // Keeps a log file from growing without bound over a long-running
    // session - every logger (homrec.log, pc.log, plugins.log, and
    // plugins' own custom files via homrec.log_to()) calls this right
    // before it appends a line. If the file is already at/over
    // maxBytes, the existing content is dropped to just its last half
    // (keeps recent history instead of a hard wipe) rather than left to
    // grow forever; a resulting multi-MB file re-checked and re-written
    // every append would itself become the "logging adds overhead"
    // problem this is meant to prevent, so this only actually touches
    // the file (reads it back in, rewrites it) once it's actually over
    // the limit - a plain size stat (cheap) is all that runs on every
    // other call.
    void CapFileSize(const std::wstring &path, long long maxBytes);
}
