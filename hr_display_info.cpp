/*
 * hr_display_info.cpp  -  HomRec v1.6.0  display enumeration helpers
 *
 * Provides fast, ctypes-callable wrappers around OS display APIs.
 * Avoids the performance cost of calling Python's tkinter or the Windows
 * DISPLAY API through ctypes every frame; results are cached in the struct.
 *
 * API:
 *   hr_di_create()        -> handle
 *   hr_di_destroy(handle)
 *   hr_di_refresh(handle) -> enumerate connected monitors, update cache
 *   hr_di_count(handle)   -> number of monitors detected
 *   hr_di_get(handle, index, out_x, out_y, out_w, out_h, out_dpi)
 *       Fill out_* with monitor geometry and DPI.
 *       Returns 1 on success, 0 if index is out of range.
 *   hr_di_primary(handle, out_x, out_y, out_w, out_h, out_dpi)
 *       Convenience: fill with primary monitor info.
 *
 * Non-Windows builds return a single synthetic monitor (0,0 + screen size).
 *
 * Compile (Linux):
 *   g++ -O3 -std=c++17 -shared -fPIC -o hr_display_info.so hr_display_info.cpp
 *
 * Compile (Windows MinGW):
 *   g++ -O3 -std=c++17 -shared -static-libgcc -static-libstdc++ \
 *       -o hr_display_info.dll hr_display_info.cpp
 */

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

#ifdef _WIN32
  #define HR_EXPORT extern "C" __declspec(dllexport)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <shellscalingapi.h>  /* GetDpiForMonitor – requires Win 8.1+ */
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
  #include <cstdio>             /* FILE, popen */
#endif

struct MonitorInfo {
    int x, y, w, h;
    float dpi;
    int   is_primary;
};

struct DisplayInfo {
    std::vector<MonitorInfo> monitors;
};

HR_EXPORT void *hr_di_create() {
    try {
        auto *d = new DisplayInfo();
        return d;
    } catch (...) { return nullptr; }
}

HR_EXPORT void hr_di_destroy(void *handle) {
    delete static_cast<DisplayInfo *>(handle);
}

#ifdef _WIN32
/* EnumDisplayMonitors callback */
struct _EnumCtx {
    std::vector<MonitorInfo> *out;
};

static BOOL CALLBACK _MonitorEnum(HMONITOR hmon, HDC /*hdc*/,
                                   LPRECT /*rect*/, LPARAM lp)
{
    _EnumCtx *ctx = reinterpret_cast<_EnumCtx *>(lp);
    MONITORINFOEX mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, &mi)) return TRUE;

    MonitorInfo info{};
    info.x = mi.rcMonitor.left;
    info.y = mi.rcMonitor.top;
    info.w = mi.rcMonitor.right  - mi.rcMonitor.left;
    info.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    info.is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;

    /* Try GetDpiForMonitor (Win 8.1+) */
    UINT dpi_x = 96, dpi_y = 96;
    using GetDpiFunc = HRESULT (WINAPI *)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
    static auto fn = reinterpret_cast<GetDpiFunc>(
        GetProcAddress(GetModuleHandleW(L"shcore.dll"), "GetDpiForMonitor"));
    if (fn) fn(hmon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
    info.dpi = (float)dpi_x;

    ctx->out->push_back(info);
    return TRUE;
}
#endif /* _WIN32 */

HR_EXPORT void hr_di_refresh(void *handle) {
    if (!handle) return;
    auto *d = static_cast<DisplayInfo *>(handle);
    d->monitors.clear();

#ifdef _WIN32
    _EnumCtx ctx{&d->monitors};
    EnumDisplayMonitors(nullptr, nullptr, _MonitorEnum,
                        reinterpret_cast<LPARAM>(&ctx));
    /* Sort: primary first */
    std::stable_sort(d->monitors.begin(), d->monitors.end(),
        [](const MonitorInfo &a, const MonitorInfo &b){
            return a.is_primary > b.is_primary;
        });
#else
    /* Non-Windows: synthetic single monitor from xrandr or /proc */
    MonitorInfo m{0, 0, 1920, 1080, 96.0f, 1};
    /* Try reading primary resolution via xrandr */
    FILE *f = popen("xrandr 2>/dev/null | grep ' connected primary' | "
                    "awk '{print $4}' | grep -oP '\\d+x\\d+'", "r");
    if (f) {
        char buf[64] = {};
        if (fgets(buf, sizeof(buf), f)) {
            int w = 0, h = 0;
            if (sscanf(buf, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                m.w = w; m.h = h;
            }
        }
        pclose(f);
    }
    d->monitors.push_back(m);
#endif
}

HR_EXPORT int hr_di_count(void *handle) {
    if (!handle) return 0;
    return (int)static_cast<DisplayInfo *>(handle)->monitors.size();
}

HR_EXPORT int hr_di_get(void *handle, int index,
                         int *out_x, int *out_y,
                         int *out_w, int *out_h,
                         float *out_dpi)
{
    if (!handle) return 0;
    auto *d = static_cast<DisplayInfo *>(handle);
    if (index < 0 || (size_t)index >= d->monitors.size()) return 0;
    const MonitorInfo &m = d->monitors[(size_t)index];
    if (out_x) *out_x = m.x;
    if (out_y) *out_y = m.y;
    if (out_w) *out_w = m.w;
    if (out_h) *out_h = m.h;
    if (out_dpi) *out_dpi = m.dpi;
    return 1;
}

HR_EXPORT int hr_di_primary(void *handle,
                              int *out_x, int *out_y,
                              int *out_w, int *out_h,
                              float *out_dpi)
{
    return hr_di_get(handle, 0, out_x, out_y, out_w, out_h, out_dpi);
}
