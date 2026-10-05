#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <algorithm>

#ifdef _WIN32
  #define HR_EXPORT extern "C" __declspec(dllexport)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <shellscalingapi.h>
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
  #include <cstdio>
#endif

struct MonitorInfo {
    int   x, y, w, h;
    float dpi;
    int   is_primary;
};

struct DisplayInfo {
    std::vector<MonitorInfo> monitors;
};

HR_EXPORT void *hr_di_create() {
    try   { return new DisplayInfo(); }
    catch (...) { return nullptr; }
}

HR_EXPORT void hr_di_destroy(void *handle) {
    delete static_cast<DisplayInfo *>(handle);
}

#ifdef _WIN32
struct _EnumCtx { std::vector<MonitorInfo> *out; };

static BOOL CALLBACK _MonitorEnum(HMONITOR hmon, HDC, LPRECT, LPARAM lp)
{
    _EnumCtx *ctx = reinterpret_cast<_EnumCtx *>(lp);

    MONITORINFOEX mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, &mi)) return TRUE;

    MonitorInfo info = {};
    info.x          = mi.rcMonitor.left;
    info.y          = mi.rcMonitor.top;
    info.w          = mi.rcMonitor.right  - mi.rcMonitor.left;
    info.h          = mi.rcMonitor.bottom - mi.rcMonitor.top;
    info.is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;

    UINT dpi_x = 96, dpi_y = 96;
    using GetDpiFunc = HRESULT(WINAPI *)(HMONITOR, MONITOR_DPI_TYPE, UINT *, UINT *);
    static auto fn = reinterpret_cast<GetDpiFunc>(
        GetProcAddress(GetModuleHandleW(L"shcore.dll"), "GetDpiForMonitor"));
    if (fn) fn(hmon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
    info.dpi = (float)dpi_x;

    ctx->out->push_back(info);
    return TRUE;
}
#endif

HR_EXPORT void hr_di_refresh(void *handle) {
    if (!handle) return;
    auto *d = static_cast<DisplayInfo *>(handle);
    d->monitors.clear();

#ifdef _WIN32
    _EnumCtx ctx{&d->monitors};
    EnumDisplayMonitors(nullptr, nullptr, _MonitorEnum,
                        reinterpret_cast<LPARAM>(&ctx));

    /* FIX: std::stable_sort now visible via <algorithm> */
    std::stable_sort(d->monitors.begin(), d->monitors.end(),
        [](const MonitorInfo &a, const MonitorInfo &b) {
            return a.is_primary > b.is_primary;
        });
#else
    MonitorInfo m = {};
    m.x = 0; m.y = 0; m.w = 1920; m.h = 1080; m.dpi = 96.0f; m.is_primary = 1;

    /* FIX: guard FILE* before reading */
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
    if (out_x)   *out_x   = m.x;
    if (out_y)   *out_y   = m.y;
    if (out_w)   *out_w   = m.w;
    if (out_h)   *out_h   = m.h;
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
