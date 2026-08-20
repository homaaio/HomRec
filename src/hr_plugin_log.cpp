#include "hr_plugin_log.h"
#include "hr_log_paths.h"

#include <windows.h>
#include <fstream>
#include <mutex>
#include <ctime>

namespace {

std::mutex &LogMutex() {
    static std::mutex m;
    return m;
}

} // namespace

namespace HrPluginLog {

void Write(const std::string &plugin_id, const char *level, const std::string &message) {
    std::lock_guard<std::mutex> lock(LogMutex());

    std::wstring path = HrLogPaths::LogFilePath(L"plugins.log");
    HrLogPaths::CapFileSize(path, 5 * 1024 * 1024);
    std::ofstream f(path.c_str(), std::ios::app | std::ios::binary);
    if (!f) return;

    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &lt);

    f << "[" << ts << "] [" << (level ? level : "INFO") << "] "
      << (plugin_id.empty() ? "[engine]" : ("[" + plugin_id + "]")) << " "
      << message << "\n";
}

} // namespace HrPluginLog
