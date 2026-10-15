#include "wgc_log.h"
#include "log.h"
#include "log_paths.h"
#include "../ui/version.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

namespace {

std::mutex &LogMutex() {
    static std::mutex m;
    return m;
}

std::atomic<bool> g_enabled{true};

std::string Narrow(const std::wstring &w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool FileExists(const std::wstring &p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

struct MonitorDump {
    std::string text;
    int idx = 0;
};

BOOL CALLBACK MonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto *d = reinterpret_cast<MonitorDump *>(lp);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "    monitor %d: %ldx%ld at %ld,%ld%s\n", ++d->idx,
                  (long)(mi.rcMonitor.right - mi.rcMonitor.left), (long)(mi.rcMonitor.bottom - mi.rcMonitor.top),
                  (long)mi.rcMonitor.left, (long)mi.rcMonitor.top,
                  (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "");
    d->text += buf;
    return TRUE;
}

std::string WindowsVersionText() {
    using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto fn = nt ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void *>(GetProcAddress(nt, "RtlGetVersion")))
                 : nullptr;
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (!fn || fn(&vi) != 0) return "unknown";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lu.%lu build %lu", (unsigned long)vi.dwMajorVersion,
                  (unsigned long)vi.dwMinorVersion, (unsigned long)vi.dwBuildNumber);
    return buf;
}

void AppendLine(const char *level, const std::string &message) {
    std::lock_guard<std::mutex> lock(LogMutex());
    const std::wstring path = HrLogPaths::LogFilePath(L"wgc.log");
    HrLogPaths::CapFileSize(path, 4 * 1024 * 1024);
    std::ofstream f(path.c_str(), std::ios::app | std::ios::binary);
    if (!f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[40];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03d", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond, st.wMilliseconds);
    f << "[" << ts << "] [" << (level ? level : "INFO") << "] [t" << GetCurrentThreadId() << "] " << message << "\n";
}

} // namespace

namespace HrWgcLog {

void SetEnabled(bool enabled) { g_enabled.store(enabled, std::memory_order_relaxed); }
bool IsEnabled() { return g_enabled.load(std::memory_order_relaxed); }

void Write(const char *level, const std::string &message, bool mirror) {
    const bool is_warn = level && (level[0] == 'W' || level[0] == 'E');
    // homrec.log keeps getting the lines it always got (and everything that is a problem), whatever the
    // wgc.log switch says - that switch only controls the dedicated file.
    if (is_warn || mirror) HrLog::Write(level, message);
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    AppendLine(level, message);
}

void WriteSessionHeaderOnce() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    if (!g_enabled.load(std::memory_order_relaxed)) return;

    std::string s = "---- window capture session ----\n";
    s += "    HomRec " HR_APP_VERSION "\n";
    s += "    Windows " + WindowsVersionText() + "\n";
    {
        MonitorDump md;
        EnumDisplayMonitors(nullptr, nullptr, MonitorProc, reinterpret_cast<LPARAM>(&md));
        s += "    monitors: " + std::to_string(md.idx) + "\n" + md.text;
    }
    s += std::string("    flag logs\\wgc_guard.flag: ") +
         (FileExists(HrLogPaths::LogFilePath(L"wgc_guard.flag")) ? "PRESENT (previous run crashed in native WGC)" : "absent") + "\n";
    s += std::string("    flag logs\\wgc_native_bad.flag: ") +
         (FileExists(HrLogPaths::LogFilePath(L"wgc_native_bad.flag")) ? "PRESENT (native WGC faulted on this PC, PrintWindow is used)" : "absent");
    AppendLine("INFO", s);
}

} // namespace HrWgcLog
