#include "hr_log.h"

#include <windows.h>
#include <fstream>
#include <mutex>
#include <ctime>
#include <cstdio>

namespace {

std::wstring LogPath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full = path;
    size_t pos = full.find_last_of(L"\\/");
    std::wstring dir = pos == std::wstring::npos ? full : full.substr(0, pos);
    return dir + L"\\homrec.log";
}

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

    std::ofstream f(LogPath(), std::ios::app | std::ios::binary);
    if (!f) return;

    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &lt);

    f << "[" << ts << "] [" << (level ? level : "INFO") << "] " << message << "\n";
}

} // namespace HrLog
