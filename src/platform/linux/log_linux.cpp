// log_linux.cpp - the Linux side of src/utils/log*.h, pc_log.h, plugin_log.h and wgc_log.h.
//
// The Windows versions of these files talk to Win32 directly (CreateFileW, GetModuleFileNameW ...),
// so the Linux build gets its own copy of the same API here. Same names, same behaviour: one
// folder with homrec.log / pc.log / plugins.log / wgc.log, each capped in size. The folder is
// platform::LogDir() (~/.local/state/homrec/logs, or <exe dir>/logs in portable mode).
#include "../paths.h"
#include "../../utils/log.h"
#include "../../utils/log_paths.h"
#include "../../utils/pc_log.h"
#include "../../utils/plugin_log.h"
#include "../../utils/wgc_log.h"
#include "../../utils/str_convert.h"
#include "../../ui/version.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

// ui_utils.cpp - same layout as HrSysStats there (see also pc_analytics_dialog.cpp).
extern "C" {
struct HrSysStats {
    float    cpu_percent;
    uint64_t ram_total_mb;
    uint64_t ram_avail_mb;
    float    ram_percent;
    uint64_t disk_total_gb;
    uint64_t disk_free_gb;
    float    disk_percent;
    int      cpu_count;
};
int hr_get_sys_stats(const char *disk_path, HrSysStats *out);
}

namespace {

std::mutex &LogMutex() {
    static std::mutex m;
    return m;
}

std::string Stamp() {
    time_t t = time(nullptr);
    tm lt{};
    localtime_r(&t, &lt);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &lt);
    return ts;
}

bool IsFile(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string Lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

}  // namespace

// ---- HrLogPaths ------------------------------------------------------------------------------

namespace HrLogPaths {

std::wstring LogsDir() {
    const std::string dir = hr::platform::LogDir();
    hr::platform::EnsureDir(dir);   // best effort - a failed write later is simply not logged
    return HrWide(dir);
}

std::wstring LogFilePath(const std::wstring &filename) {
    return LogsDir() + L"/" + filename;
}

std::wstring SanitizeLogFilename(const std::wstring &requested) {
    // Same rules as the Windows build (keep the last path component, drop odd characters, force a
    // .log / .txt extension, never collide with HomRec's own log files).
    std::string name = HrNarrow(requested);
    size_t pos = name.find_last_of("\\/");
    if (pos != std::string::npos) name = name.substr(pos + 1);

    std::string cleaned;
    for (char c : name) {
        if (c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') continue;
        cleaned += c;
    }
    while (!cleaned.empty() && (cleaned.front() == '.' || cleaned.front() == ' ')) cleaned.erase(cleaned.begin());
    while (!cleaned.empty() && (cleaned.back() == '.' || cleaned.back() == ' ')) cleaned.pop_back();
    if (cleaned.empty()) return L"plugin.log";

    const std::string low = Lower(cleaned);
    auto has_ext = [&](const char *ext) {
        const size_t n = std::char_traits<char>::length(ext);
        return low.size() >= n && low.compare(low.size() - n, n, ext) == 0;
    };
    if (!has_ext(".log") && !has_ext(".txt")) cleaned += ".log";
    const std::string low2 = Lower(cleaned);
    if (low2 == "homrec.log" || low2 == "pc.log" || low2 == "plugins.log" || low2 == "wgc.log")
        cleaned.insert(cleaned.size() - 4, "_custom");
    return HrWide(cleaned);
}

void CapFileSize(const std::wstring &path, long long maxBytes) {
    const std::string p = HrNarrow(path);
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return;              // doesn't exist yet - nothing to cap
    if ((long long)st.st_size < maxBytes) return;         // the common case: one stat() and done

    // Keep only the second half, on a line boundary, so the recent history survives.
    std::string data;
    {
        std::ifstream in(p, std::ios::binary);
        if (!in) return;
        data.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    size_t keep_from = data.size() / 2;
    const size_t nl = data.find('\n', keep_from);
    if (nl != std::string::npos) keep_from = nl + 1;

    std::ofstream out(p, std::ios::trunc | std::ios::binary);
    if (!out) return;
    out << "[... earlier entries trimmed to keep this file small ...]\n";
    out.write(data.data() + keep_from, (std::streamsize)(data.size() - keep_from));
}

int TruncateLogFile(const std::wstring &path) {
    const std::string p = HrNarrow(path);
    if (!IsFile(p)) return 0;
    FILE *f = std::fopen(p.c_str(), "wb");
    if (!f) return -1;
    std::fclose(f);
    return 1;
}

int TruncateAllLogs(std::vector<std::wstring> *names) {
    const std::wstring wdir = LogsDir();
    const std::string dir = HrNarrow(wdir);
    DIR *d = opendir(dir.c_str());
    if (!d) return 0;
    int n = 0;
    while (dirent *e = readdir(d)) {
        const std::string fn = e->d_name;
        if (fn.size() < 5 || fn.compare(fn.size() - 4, 4, ".log") != 0) continue;
        if (TruncateLogFile(wdir + L"/" + HrWide(fn)) == 1) {
            ++n;
            if (names) names->push_back(HrWide(fn));
        }
    }
    closedir(d);
    if (TruncateLogFile(wdir + L"/ffmpeg_ddagrab_progress.txt") == 1) {
        ++n;
        if (names) names->push_back(L"ffmpeg_ddagrab_progress.txt");
    }
    return n;
}

}  // namespace HrLogPaths

// ---- HrLog -----------------------------------------------------------------------------------

namespace HrLog {

void Write(const char *level, const std::string &message) {
    std::lock_guard<std::mutex> lock(LogMutex());
    const std::wstring path = HrLogPaths::LogFilePath(L"homrec.log");
    HrLogPaths::CapFileSize(path, 5 * 1024 * 1024);
    std::ofstream f(HrNarrow(path), std::ios::app | std::ios::binary);
    if (!f) return;
    f << "[" << Stamp() << "] [" << (level ? level : "INFO") << "] " << message << "\n";
}

}  // namespace HrLog

// ---- HrPluginLog -----------------------------------------------------------------------------

namespace HrPluginLog {

namespace { std::atomic<bool> g_enabled{true}; }

void SetEnabled(bool enabled) { g_enabled.store(enabled); }
bool IsEnabled() { return g_enabled.load(); }

void Write(const std::string &plugin_id, const char *level, const std::string &message) {
    if (!g_enabled.load()) return;
    std::lock_guard<std::mutex> lock(LogMutex());
    const std::wstring path = HrLogPaths::LogFilePath(L"plugins.log");
    HrLogPaths::CapFileSize(path, 5 * 1024 * 1024);
    std::ofstream f(HrNarrow(path), std::ios::app | std::ios::binary);
    if (!f) return;
    f << "[" << Stamp() << "] [" << (level ? level : "INFO") << "] ["
      << (plugin_id.empty() ? std::string("engine") : plugin_id) << "] " << message << "\n";
}

}  // namespace HrPluginLog

// ---- HrPcLog ---------------------------------------------------------------------------------

namespace HrPcLog {

namespace { std::atomic<bool> g_enabled{true}; }

void SetEnabled(bool enabled) { g_enabled.store(enabled); }
bool IsEnabled() { return g_enabled.load(); }

void MaybeLogSnapshot(bool is_recording, double current_fps, bool hw_encoder_active,
                      const std::string &recordings_folder) {
    if (!g_enabled.load()) return;
    static time_t last = 0;
    const time_t now = time(nullptr);
    if (now - last < 10) return;     // one line every 10 seconds at most
    last = now;

    HrSysStats s{};
    if (!hr_get_sys_stats(recordings_folder.c_str(), &s)) return;

    std::lock_guard<std::mutex> lock(LogMutex());
    const std::wstring path = HrLogPaths::LogFilePath(L"pc.log");
    HrLogPaths::CapFileSize(path, 5 * 1024 * 1024);
    std::ofstream f(HrNarrow(path), std::ios::app | std::ios::binary);
    if (!f) return;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "cpu=%.0f%% ram=%.0f%% (%llu/%llu MB free) disk_free=%llu GB rec=%s fps=%.1f hw_enc=%s",
                  s.cpu_percent, s.ram_percent, (unsigned long long)s.ram_avail_mb,
                  (unsigned long long)s.ram_total_mb, (unsigned long long)s.disk_free_gb,
                  is_recording ? "yes" : "no", current_fps, hw_encoder_active ? "yes" : "no");
    f << "[" << Stamp() << "] " << line << "\n";
}

}  // namespace HrPcLog

// ---- HrWgcLog (window / screen capture diagnostics; there is no WGC on Linux, the file keeps its name) -----

namespace HrWgcLog {

namespace {
std::atomic<bool> g_enabled{true};

void AppendLine(const char *level, const std::string &message) {
    std::lock_guard<std::mutex> lock(LogMutex());
    const std::wstring path = HrLogPaths::LogFilePath(L"wgc.log");
    HrLogPaths::CapFileSize(path, 5 * 1024 * 1024);
    std::ofstream f(HrNarrow(path), std::ios::app | std::ios::binary);
    if (!f) return;
    f << "[" << Stamp() << "] [" << (level ? level : "INFO") << "] [t" << (long)::syscall(SYS_gettid) << "] "
      << message << "\n";
}
}  // namespace

void SetEnabled(bool enabled) { g_enabled.store(enabled); }
bool IsEnabled() { return g_enabled.load(); }

void Write(const char *level, const std::string &message, bool mirror) {
    const std::string lv = level ? level : "INFO";
    if (g_enabled.load()) AppendLine(level, message);
    if (mirror || lv == "WARN" || lv == "ERROR") HrLog::Write(level, message);
}

void WriteSessionHeaderOnce() {
    static std::once_flag once;
    std::call_once(once, []() {
        if (!g_enabled.load()) return;
        AppendLine("INFO", std::string("HomRec ") + HR_APP_VERSION + " on Linux, session: " +
                           (std::getenv("XDG_SESSION_TYPE") ? std::getenv("XDG_SESSION_TYPE") : "unknown"));
    });
}

}  // namespace HrWgcLog
