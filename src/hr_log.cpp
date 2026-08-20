#include "hr_log.h"
#include "hr_log_paths.h"

#include <windows.h>
#include <fstream>
#include <mutex>
#include <ctime>
#include <cstdio>

namespace {

// Guards the log file against concurrent writes (audio callback thread,
// capture pipeline thread, and the UI thread can all log).
std::mutex &LogMutex() {
    static std::mutex m;
    return m;
}

} // namespace

namespace HrLog {

void Write(const char *level, const std::string &message) {
    std::lock_guard<std::mutex> lock(LogMutex());

    // BUGFIX: every log file used to sit loose in <exe-dir> (homrec.log
    // right next to the .exe). Now that pc.log and plugins.log exist
    // too, all three - and any custom per-plugin log a plugin opens via
    // homrec.log_to() - live under <exe-dir>\logs\ instead, so a user
    // reporting a bug can just zip up one folder. HrLogPaths::LogFilePath()
    // creates logs\ on demand, so no separate one-time setup is needed.
    // Keeps homrec.log from growing without bound over a long session -
    // see HrLogPaths::CapFileSize()'s comment for why this is cheap on
    // every call that doesn't actually need to rotate.
    std::wstring path = HrLogPaths::LogFilePath(L"homrec.log");
    HrLogPaths::CapFileSize(path, 5 * 1024 * 1024);
    std::ofstream f(path.c_str(), std::ios::app | std::ios::binary);
    if (!f) return;
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &lt);

    f << "[" << ts << "] [" << (level ? level : "INFO") << "] " << message << "\n";
}

} // namespace HrLog
