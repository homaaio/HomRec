// NOMINMAX must come before any Windows header to avoid min/max macro conflicts
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #define HR_EXPORT extern "C" __declspec(dllexport)
  #include <windows.h>
  // SetThreadDescription requires Win10 1607+ SDK; guard for older MinGW
  #if defined(NTDDI_WIN10_RS1) || (_WIN32_WINNT >= 0x0A00)
    #include <processthreadsapi.h>
    #define HR_HAS_SET_THREAD_DESC 1
  #endif
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
  #include <unistd.h>
#endif

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <chrono>
#include <algorithm>

static constexpr int HR_DX_OK      =  0;
static constexpr int HR_DX_TIMEOUT =  1;
static constexpr int HR_DX_LOST    =  2;
static constexpr int HR_DX_ERROR   = -1;

// ---------------------------------------------------------------------------
// Dynamic loader for helper DLLs
// ---------------------------------------------------------------------------
#ifdef _WIN32
struct LibHandles {
    HMODULE dxgi = nullptr;
    HMODULE enc  = nullptr;
    HMODULE sw   = nullptr;

    typedef void*   (*F_dx_create)(int, int);
    typedef void    (*F_dx_destroy)(void*);
    typedef int     (*F_dx_capture)(void*, uint8_t*, int);
    typedef int     (*F_dx_get_size)(void*, int*, int*);
    typedef int     (*F_dx_reset)(void*);
    typedef void    (*F_bgra_to_yuv)(const uint8_t*, uint8_t*, int, int);
    typedef void*   (*F_sw_create)();
    typedef void    (*F_sw_destroy)(void*);
    typedef void    (*F_sw_start)(void*);
    typedef void    (*F_sw_sleep_until)(void*, int64_t);
    typedef int64_t (*F_sw_elapsed_ns)(void*);

    F_dx_create      dx_create      = nullptr;
    F_dx_destroy     dx_destroy     = nullptr;
    F_dx_capture     dx_capture     = nullptr;
    F_dx_get_size    dx_get_size    = nullptr;
    F_dx_reset       dx_reset       = nullptr;
    F_bgra_to_yuv    bgra_to_yuv    = nullptr;
    F_sw_create      sw_create      = nullptr;
    F_sw_destroy     sw_destroy     = nullptr;
    F_sw_start       sw_start       = nullptr;
    F_sw_sleep_until sw_sleep_until = nullptr;
    F_sw_elapsed_ns  sw_elapsed_ns  = nullptr;
    bool loaded = false;

    bool load(const wchar_t* base_dir) {
        auto try_load = [&](const wchar_t* name) -> HMODULE {
            wchar_t path[MAX_PATH];
            if (base_dir && base_dir[0])
                swprintf_s(path, MAX_PATH, L"%s\\%s", base_dir, name);
            else
                wcscpy_s(path, MAX_PATH, name);
            return LoadLibraryW(path);
        };

        dxgi = try_load(L"hr_dxgi_capture.dll");
        enc  = try_load(L"hr_encoder_helpers.dll");
        sw   = try_load(L"hr_stopwatch.dll");
        if (!dxgi || !enc || !sw) return false;

        dx_create      = (F_dx_create)     GetProcAddress(dxgi, "hr_dx_create");
        dx_destroy     = (F_dx_destroy)    GetProcAddress(dxgi, "hr_dx_destroy");
        dx_capture     = (F_dx_capture)    GetProcAddress(dxgi, "hr_dx_capture");
        dx_get_size    = (F_dx_get_size)   GetProcAddress(dxgi, "hr_dx_get_size");
        dx_reset       = (F_dx_reset)      GetProcAddress(dxgi, "hr_dx_reset");
        bgra_to_yuv    = (F_bgra_to_yuv)   GetProcAddress(enc,  "hr_bgra_to_yuv420p");
        sw_create      = (F_sw_create)     GetProcAddress(sw,   "hr_sw_create");
        sw_destroy     = (F_sw_destroy)    GetProcAddress(sw,   "hr_sw_destroy");
        sw_start       = (F_sw_start)      GetProcAddress(sw,   "hr_sw_start");
        sw_sleep_until = (F_sw_sleep_until)GetProcAddress(sw,   "hr_sw_sleep_until_ns");
        sw_elapsed_ns  = (F_sw_elapsed_ns) GetProcAddress(sw,   "hr_sw_elapsed_ns");

        loaded = dx_create && dx_destroy && dx_capture && dx_get_size && dx_reset
              && bgra_to_yuv
              && sw_create && sw_destroy && sw_start && sw_sleep_until && sw_elapsed_ns;
        return loaded;
    }
};

static LibHandles g_libs;
static bool g_libs_done = false;
static std::mutex g_libs_mutex;

static bool ensure_libs() {
    std::lock_guard<std::mutex> lk(g_libs_mutex);
    if (g_libs_done) return g_libs.loaded;
    g_libs_done = true;
    wchar_t this_path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ensure_libs), &self);
    if (self) GetModuleFileNameW(self, this_path, MAX_PATH);
    wchar_t* sep = wcsrchr(this_path, L'\\');
    if (sep) *sep = L'\0';
    g_libs.load(this_path[0] ? this_path : nullptr);
    return g_libs.loaded;
}
#endif  // _WIN32

// ---------------------------------------------------------------------------
// BGRA→thumbnail (box-filter, no intermediate RGB buffer)
// OPT: устраняет bgra_to_rgb_inplace() + rgb_pv буфер целиком.
// Работает только при целочисленных кратностях (быстрый путь).
// При нецелочисленных — nearest-neighbour прямо из BGRA.
// ---------------------------------------------------------------------------
static void bgra_to_thumb(const uint8_t* __restrict bgra,
                           uint8_t*       __restrict dst,
                           int sw, int sh, int dw, int dh)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    if ((sw % dw) == 0 && (sh % dh) == 0) {
        // Fast integer-ratio box filter
        int rx = sw / dw, ry = sh / dh;
        int bsz = rx * ry;
        for (int y = 0; y < dh; ++y) {
            for (int x = 0; x < dw; ++x) {
                uint32_t r = 0, g = 0, b = 0;
                int sy0 = y * ry, sx0 = x * rx;
                for (int by = 0; by < ry; ++by) {
                    const uint8_t* row = bgra + ((size_t)(sy0 + by) * sw + sx0) * 4;
                    for (int bx = 0; bx < rx; ++bx) {
                        b += row[bx*4+0];
                        g += row[bx*4+1];
                        r += row[bx*4+2];
                    }
                }
                uint8_t* o = dst + ((size_t)y * dw + x) * 3;
                o[0] = (uint8_t)(r / (uint32_t)bsz);
                o[1] = (uint8_t)(g / (uint32_t)bsz);
                o[2] = (uint8_t)(b / (uint32_t)bsz);
            }
        }
    } else {
        // Nearest-neighbour fallback (non-integer ratio)
        float rx = (float)sw / dw, ry = (float)sh / dh;
        for (int y = 0; y < dh; ++y) {
            int sy = (int)(y * ry); if (sy >= sh) sy = sh - 1;
            for (int x = 0; x < dw; ++x) {
                int sx = (int)(x * rx); if (sx >= sw) sx = sw - 1;
                const uint8_t* s = bgra + ((size_t)sy * sw + sx) * 4;
                uint8_t*       d = dst  + ((size_t)y  * dw + x ) * 3;
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0];  // BGR→RGB
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Pipeline state
// ---------------------------------------------------------------------------
struct Pipeline {
    int src_w = 0, src_h = 0;
    int fps   = 30;
    int pv_w  = 960, pv_h = 540;
    intptr_t pipe_handle = 0;
    bool recording = false;   // pipe open → encode YUV; false → preview only

    void* dx_ctx = nullptr;
    void* sw_ctx = nullptr;

    std::vector<uint8_t> bgra_buf;  // src_w * src_h * 4
    std::vector<uint8_t> yuv_buf;   // src_w * src_h * 3/2  (only when recording)

    // Preview
    std::vector<uint8_t> pv_buf;
    std::mutex           pv_mtx;
    int  pv_actual_w = 0, pv_actual_h = 0;
    bool pv_ready    = false;

    std::thread       capture_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};

    std::atomic<int64_t> frames_captured{0};
    std::atomic<int64_t> frames_dropped{0};
    std::atomic<double>  fps_actual{0.0};

    // -------------------------------------------------------------------------
    // Write raw bytes to pipe
    // -------------------------------------------------------------------------
    bool write_pipe(const uint8_t* data, size_t total) {
        if (pipe_handle == 0 || pipe_handle == -1) return false;
        size_t written = 0;
#ifdef _WIN32
        HANDLE h = reinterpret_cast<HANDLE>(pipe_handle);
        while (written < total) {
            DWORD w = 0;
            if (!WriteFile(h, data + written,
                           static_cast<DWORD>(total - written), &w, nullptr)
                || w == 0) return false;
            written += w;
        }
#else
        while (written < total) {
            ssize_t r = ::write(static_cast<int>(pipe_handle),
                                data + written, total - written);
            if (r <= 0) return false;
            written += static_cast<size_t>(r);
        }
#endif
        return true;
    }

    // -------------------------------------------------------------------------
    // Update preview thumbnail — directly from BGRA, no intermediate RGB copy
    // OPT: устранён bgra_to_rgb_inplace() и буфер rgb_pv
    // -------------------------------------------------------------------------
    void update_preview() {
        int tw = pv_w, th = pv_h;
        if (src_w > 0 && src_h > 0) {
            float ar = (float)src_w / (float)src_h;
            if (tw > (int)(th * ar)) tw = (int)(th * ar);
            else                      th = (int)(tw / ar);
        }
        tw = std::max(tw & ~1, 2);
        th = std::max(th & ~1, 2);

        size_t pv_sz = (size_t)tw * th * 3;
        std::lock_guard<std::mutex> lock(pv_mtx);
        if (pv_buf.size() != pv_sz) pv_buf.resize(pv_sz);

        bgra_to_thumb(bgra_buf.data(), pv_buf.data(), src_w, src_h, tw, th);
        pv_actual_w = tw;
        pv_actual_h = th;
        pv_ready    = true;
    }

    // -------------------------------------------------------------------------
    // Main capture loop
    // -------------------------------------------------------------------------
    void capture_loop() {
#ifdef _WIN32
        // OPT: TIME_CRITICAL вместо ABOVE_NORMAL — защита от 15 мс вытеснений
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
{
    typedef HRESULT (WINAPI *PFN_STD)(HANDLE, PCWSTR);
    static auto fn = (PFN_STD)GetProcAddress(
        GetModuleHandleW(L"KernelBase.dll"), "SetThreadDescription");
    if (fn) fn(GetCurrentThread(), L"HomRec Capture");
}
#endif
        const int64_t frame_ns = (fps > 0)
                                 ? (1'000'000'000LL / fps)
                                 : (1'000'000'000LL / 30);

        // OPT: PREVIEW_EVERY динамический — fps/20, минимум 1
        // 30fps → каждые ~1-2 кадра; 60fps → каждые 3
        const int PREVIEW_EVERY = std::max(1, fps / 20);

        // OPT: timeout_ms = 2/3 frame (было 1/2) → меньше TIMEOUT-пропусков
        int timeout_ms = static_cast<int>(frame_ns * 2 / 3'000'000LL);
        if (timeout_ms < 8)  timeout_ms = 8;
        if (timeout_ms > 33) timeout_ms = 33;

        int64_t fps_acc_frames = 0, fps_acc_start_ns = 0;

#ifdef _WIN32
        if (sw_ctx && g_libs.sw_start) g_libs.sw_start(sw_ctx);
#endif
        int64_t next_frame_ns = frame_ns;
        int frame_idx = 0;

        while (running.load(std::memory_order_relaxed)) {
            if (paused.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
#ifdef _WIN32
                if (sw_ctx && g_libs.sw_start) g_libs.sw_start(sw_ctx);
#endif
                next_frame_ns = frame_ns;
                continue;
            }

            // Frame pacing
#ifdef _WIN32
            if (sw_ctx && g_libs.sw_sleep_until)
                g_libs.sw_sleep_until(sw_ctx, next_frame_ns);
            else
                std::this_thread::sleep_for(std::chrono::nanoseconds(frame_ns));
#else
            std::this_thread::sleep_for(std::chrono::nanoseconds(frame_ns));
#endif
            next_frame_ns += frame_ns;

            // Capture
#ifdef _WIN32
            if (!g_libs.dx_capture) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            int ret = g_libs.dx_capture(dx_ctx, bgra_buf.data(), timeout_ms);
#else
            int ret = HR_DX_ERROR;
#endif
            if (ret == HR_DX_TIMEOUT) {
                frames_dropped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (ret == HR_DX_LOST) {
#ifdef _WIN32
                if (g_libs.dx_reset) g_libs.dx_reset(dx_ctx);
#endif
                continue;
            }
            if (ret != HR_DX_OK) {
                running.store(false);
                break;
            }

            // OPT: YUV конвертация только при активной записи
#ifdef _WIN32
            if (recording && g_libs.bgra_to_yuv) {
                g_libs.bgra_to_yuv(bgra_buf.data(), yuv_buf.data(), src_w, src_h);
                if (!write_pipe(yuv_buf.data(), yuv_buf.size())) {
                    running.store(false);
                    break;
                }
            }
#endif
            frames_captured.fetch_add(1, std::memory_order_relaxed);

            // Preview: динамическая частота
            if (++frame_idx % PREVIEW_EVERY == 0)
                update_preview();

            // FPS tracking
#ifdef _WIN32
            if (sw_ctx && g_libs.sw_elapsed_ns) {
                int64_t now_ns = g_libs.sw_elapsed_ns(sw_ctx);
                fps_acc_frames++;
                if (fps_acc_start_ns == 0) fps_acc_start_ns = now_ns;
                int64_t acc_ns = now_ns - fps_acc_start_ns;
                if (acc_ns >= 1'000'000'000LL) {
                    fps_actual.store(
                        (double)fps_acc_frames * 1e9 / (double)acc_ns,
                        std::memory_order_relaxed);
                    fps_acc_frames   = 0;
                    fps_acc_start_ns = now_ns;
                }
            }
#endif
        }
    }
};

// ============================================================================
// Exported API
// ============================================================================

HR_EXPORT void* hr_pl_create(int w, int h, int fps,
                               int pipe_fd, int pv_w, int pv_h) {
#ifndef _WIN32
    (void)w; (void)h; (void)fps; (void)pipe_fd; (void)pv_w; (void)pv_h;
    return nullptr;
#else
    if (!ensure_libs()) return nullptr;

    auto* pl = new Pipeline();
    pl->src_w       = w;
    pl->src_h       = h;
    pl->fps         = fps;
    pl->pv_w        = pv_w;
    pl->pv_h        = pv_h;
    pl->pipe_handle = static_cast<intptr_t>(pipe_fd);
    pl->recording   = (pipe_fd != 0 && pipe_fd != -1);

    pl->dx_ctx = g_libs.dx_create(0, 0);
    if (!pl->dx_ctx) { delete pl; return nullptr; }

    pl->sw_ctx = g_libs.sw_create();
    if (!pl->sw_ctx) { g_libs.dx_destroy(pl->dx_ctx); delete pl; return nullptr; }

    return pl;
#endif
}

HR_EXPORT void hr_pl_destroy(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    auto* pl = static_cast<Pipeline*>(handle);
    pl->running.store(false);
    if (pl->capture_thread.joinable()) pl->capture_thread.join();
    if (pl->dx_ctx && g_libs.dx_destroy) g_libs.dx_destroy(pl->dx_ctx);
    if (pl->sw_ctx && g_libs.sw_destroy) g_libs.sw_destroy(pl->sw_ctx);
    delete pl;
#endif
}

HR_EXPORT int hr_pl_start(void* handle) {
    if (!handle) return 0;
#ifndef _WIN32
    return 0;
#else
    auto* pl = static_cast<Pipeline*>(handle);

    int real_w = pl->src_w, real_h = pl->src_h;
    if (g_libs.dx_get_size) g_libs.dx_get_size(pl->dx_ctx, &real_w, &real_h);
    if (real_w > 0 && real_h > 0) { pl->src_w = real_w; pl->src_h = real_h; }

    pl->bgra_buf.resize((size_t)pl->src_w * pl->src_h * 4);
    if (pl->recording)
        pl->yuv_buf.resize((size_t)pl->src_w * pl->src_h * 3 / 2);

    // OPT: pre-allocate preview buffer once
    int tw = pl->pv_w, th = pl->pv_h;
    if (pl->src_w > 0 && pl->src_h > 0) {
        float ar = (float)pl->src_w / (float)pl->src_h;
        if (tw > (int)(th * ar)) tw = (int)(th * ar);
        else                      th = (int)(tw / ar);
    }
    pl->pv_buf.resize((size_t)(std::max(tw & ~1, 2)) * std::max(th & ~1, 2) * 3);

    pl->running.store(true);
    pl->capture_thread = std::thread([pl]{ pl->capture_loop(); });
    return 1;
#endif
}

HR_EXPORT void hr_pl_stop(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    auto* pl = static_cast<Pipeline*>(handle);
    pl->running.store(false);
    if (pl->capture_thread.joinable()) pl->capture_thread.join();
#endif
}

HR_EXPORT void hr_pl_pause(void* handle, int flag) {
    if (!handle) return;
#ifdef _WIN32
    static_cast<Pipeline*>(handle)->paused.store(flag != 0);
#endif
}

// OPT: новая функция — переключить запись на лету без пересоздания pipeline
HR_EXPORT void hr_pl_set_recording(void* handle, int active, int pipe_fd) {
    if (!handle) return;
#ifdef _WIN32
    auto* pl = static_cast<Pipeline*>(handle);
    pl->pipe_handle = static_cast<intptr_t>(pipe_fd);
    pl->recording   = (active != 0) && (pipe_fd != 0) && (pipe_fd != -1);
    if (pl->recording && pl->yuv_buf.empty())
        pl->yuv_buf.resize((size_t)pl->src_w * pl->src_h * 3 / 2);
#endif
}

HR_EXPORT int hr_pl_get_preview(void* handle, uint8_t* out_rgb,
                                  int* out_w, int* out_h) {
    if (!handle || !out_rgb || !out_w || !out_h) return 0;
#ifndef _WIN32
    return 0;
#else
    auto* pl = static_cast<Pipeline*>(handle);
    std::lock_guard<std::mutex> lock(pl->pv_mtx);
    if (!pl->pv_ready || pl->pv_buf.empty()) return 0;
    *out_w = pl->pv_actual_w;
    *out_h = pl->pv_actual_h;
    memcpy(out_rgb, pl->pv_buf.data(), pl->pv_buf.size());
    return 1;
#endif
}

HR_EXPORT void hr_pl_stats(void* handle,
                             int64_t* out_frames, int64_t* out_drops,
                             double* out_fps) {
#ifdef _WIN32
    if (!handle) return;
    auto* pl = static_cast<Pipeline*>(handle);
    if (out_frames) *out_frames = pl->frames_captured.load(std::memory_order_relaxed);
    if (out_drops)  *out_drops  = pl->frames_dropped .load(std::memory_order_relaxed);
    if (out_fps)    *out_fps    = pl->fps_actual      .load(std::memory_order_relaxed);
#else
    if (out_frames) *out_frames = 0;
    if (out_drops)  *out_drops  = 0;
    if (out_fps)    *out_fps    = 0.0;
#endif
}

HR_EXPORT void hr_pl_set_fps(void* handle, int fps) {
    if (!handle || fps <= 0) return;
#ifdef _WIN32
    static_cast<Pipeline*>(handle)->fps = fps;
#endif
}

HR_EXPORT void hr_pl_set_preview_size(void* handle, int pw, int ph) {
    if (!handle || pw <= 0 || ph <= 0) return;
#ifdef _WIN32
    auto* pl = static_cast<Pipeline*>(handle);
    std::lock_guard<std::mutex> lock(pl->pv_mtx);
    pl->pv_w    = pw;
    pl->pv_h    = ph;
    pl->pv_ready = false;
#endif
}
