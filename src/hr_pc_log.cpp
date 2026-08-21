#include "hr_pc_log.h"
#include "hr_log_paths.h"

#include <windows.h>
#include <psapi.h>
#include <fstream>
#include <mutex>
#include <ctime>
#include <cstdio>
#include <cstdint>
#include <string>

// hr_ui_utils.cpp's C ABI - no shared header, same pattern
// recording_controller.cpp already uses for this exact function.
extern "C" {
    int hr_get_free_disk_mb(const char *path, uint64_t *out_free_mb);
}

namespace {

// One line every ~10s is plenty of resolution for "is this machine
// struggling" without turning pc.log into a multi-MB file over a long
// recording session the way a line-per-stats_timer_-tick (500ms) would.
constexpr double kIntervalSeconds = 10.0;

std::mutex &LogMutex() {
    static std::mutex m;
    return m;
}

bool g_enabled = true;

// CPU usage needs two samples and the delta between them - both this
// process's and the whole system's, so pc.log can distinguish "HomRec
// itself is pegging a core" from "something else on this PC is."
// FILETIME-as-uint64 helper, and the running previous-sample state
// (protected by LogMutex() since MaybeLogSnapshot() already holds it for
// the whole call anyway).
uint64_t FileTimeToU64(const FILETIME &ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

struct CpuSample {
    uint64_t proc_kernel = 0, proc_user = 0;
    uint64_t sys_idle = 0, sys_kernel = 0, sys_user = 0;
    bool valid = false;
};

CpuSample TakeCpuSample() {
    CpuSample s;
    FILETIME creation, exit, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        s.proc_kernel = FileTimeToU64(kernel);
        s.proc_user   = FileTimeToU64(user);
        s.valid = true;
    }
    FILETIME idle, sysKernel, sysUser;
    if (GetSystemTimes(&idle, &sysKernel, &sysUser)) {
        s.sys_idle   = FileTimeToU64(idle);
        s.sys_kernel = FileTimeToU64(sysKernel);
        s.sys_user   = FileTimeToU64(sysUser);
    } else {
        s.valid = false;
    }
    return s;
}

} // namespace

namespace HrPcLog {

void SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(LogMutex());
    g_enabled = enabled;
}

bool IsEnabled() {
    std::lock_guard<std::mutex> lock(LogMutex());
    return g_enabled;
}

void MaybeLogSnapshot(bool is_recording, double current_fps, bool hw_encoder_active,
                       const std::string &recordings_folder) {
    static double accumulated_seconds = kIntervalSeconds; // log immediately on first call
    static ULONGLONG last_tick = 0;
    static CpuSample prev_sample;
    static bool have_prev_sample = false;

    std::lock_guard<std::mutex> lock(LogMutex());
    if (!g_enabled) return;

    ULONGLONG now_tick = GetTickCount64();
    if (last_tick != 0) accumulated_seconds += (now_tick - last_tick) / 1000.0;
    last_tick = now_tick;
    if (accumulated_seconds < kIntervalSeconds) return;
    accumulated_seconds = 0.0;

    CpuSample cur = TakeCpuSample();
    double proc_cpu_pct = -1.0, sys_cpu_pct = -1.0;
    if (have_prev_sample && cur.valid) {
        uint64_t proc_delta = (cur.proc_kernel - prev_sample.proc_kernel) +
                               (cur.proc_user   - prev_sample.proc_user);
        uint64_t sys_busy_delta = (cur.sys_kernel - prev_sample.sys_kernel) +
                                   (cur.sys_user   - prev_sample.sys_user) -
                                   (cur.sys_idle   - prev_sample.sys_idle);
        uint64_t sys_total_delta = (cur.sys_kernel - prev_sample.sys_kernel) +
                                    (cur.sys_user   - prev_sample.sys_user);
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        int cores = si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
        if (sys_total_delta > 0) {
            // proc_delta is summed across all cores the process used;
            // sys_total_delta is one core's worth of wall-clock time
            // (GetSystemTimes already reports system-wide totals, not
            // per-core), so dividing by cores here puts both percentages
            // on the same 0-100 scale instead of process-CPU reading up
            // to cores*100%.
            proc_cpu_pct = 100.0 * (double)proc_delta / (double)sys_total_delta / cores;
            sys_cpu_pct  = 100.0 * (double)sys_busy_delta / (double)sys_total_delta;
        }
    }
    prev_sample = cur;
    have_prev_sample = cur.valid;

    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    SIZE_T working_set_mb = 0;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        working_set_mb = pmc.WorkingSetSize / (1024 * 1024);

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    DWORD sys_mem_load_pct = 0;
    ULONGLONG sys_mem_total_mb = 0, sys_mem_avail_mb = 0;
    if (GlobalMemoryStatusEx(&ms)) {
        sys_mem_load_pct = ms.dwMemoryLoad;
        sys_mem_total_mb = ms.ullTotalPhys / (1024 * 1024);
        sys_mem_avail_mb = ms.ullAvailPhys / (1024 * 1024);
    }

    // Same check + same folder RecordingController::Start() already uses
    // to warn about low disk space before recording - the number that
    // actually matters for this app, not just "whatever drive HomRec is
    // installed on".
    uint64_t free_mb = 0;
    double free_disk_gb = -1.0;
    if (!recordings_folder.empty() && hr_get_free_disk_mb(recordings_folder.c_str(), &free_mb))
        free_disk_gb = (double)free_mb / 1024.0;

    std::wstring path = HrLogPaths::LogFilePath(L"pc.log");
    HrLogPaths::CapFileSize(path, 5 * 1024 * 1024);
    std::ofstream f(path.c_str(), std::ios::app | std::ios::binary);
    if (!f) return;

    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &lt);

    char proc_cpu_buf[16], sys_cpu_buf[16], disk_buf[24];
    if (proc_cpu_pct >= 0) snprintf(proc_cpu_buf, sizeof(proc_cpu_buf), "%.1f%%", proc_cpu_pct);
    else                   snprintf(proc_cpu_buf, sizeof(proc_cpu_buf), "n/a");
    if (sys_cpu_pct >= 0)  snprintf(sys_cpu_buf, sizeof(sys_cpu_buf), "%.1f%%", sys_cpu_pct);
    else                   snprintf(sys_cpu_buf, sizeof(sys_cpu_buf), "n/a");
    if (free_disk_gb >= 0) snprintf(disk_buf, sizeof(disk_buf), "%.1fGB", free_disk_gb);
    else                   snprintf(disk_buf, sizeof(disk_buf), "n/a");

    char line[512];
    snprintf(line, sizeof(line),
             "[%s] cpu_proc=%s cpu_sys=%s mem_proc=%lluMB sys_mem=%lu%% "
             "(%lluMB free of %lluMB) disk_free=%s state=%s fps=%.1f gpu_encoder=%s",
             ts, proc_cpu_buf, sys_cpu_buf,
             (unsigned long long)working_set_mb,
             (unsigned long)sys_mem_load_pct,
             (unsigned long long)sys_mem_avail_mb, (unsigned long long)sys_mem_total_mb,
             disk_buf,
             is_recording ? "recording" : "idle",
             current_fps,
             hw_encoder_active ? "yes" : "no");
    f << line << "\n";
}

} // namespace HrPcLog
