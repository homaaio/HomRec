// NOMINMAX must come before any Windows header to avoid min/max macro conflicts
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #define HR_EXPORT extern "C" __declspec(dllexport)
  #include <windows.h>
  #include <objbase.h>  // CoInitializeEx / CoUninitialize / COINIT_MULTITHREADED
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
#include <queue>
#include <condition_variable>
#include <string>
#include <cstdio>
#include <exception>
#include <functional>
#include "../utils/log.h"
#include "overlay_render.h"
#include "wgc_capture.h"   // 2.4: Windows.Graphics.Capture window source
#include "nut_mux.h"       // timestamped pipe to ffmpeg (see hr_pl_set_timestamped_pipe)

static constexpr int HR_DX_OK      =  0;
static constexpr int HR_DX_TIMEOUT =  1;
static constexpr int HR_DX_LOST    =  2;
static constexpr int HR_DX_ERROR   = -1;

// ---------------------------------------------------------------------------
// Dynamic loader for helper DLLs
// ---------------------------------------------------------------------------

// PATCHED for static linking (see INTEGRATION_NOTES.md "internal DLL loading" finding):
// this used to LoadLibraryW/GetProcAddress hr_dxgi_capture.dll / hr_encoder_helpers.dll /
// hr_stopwatch.dll by filename at runtime.
#ifdef _WIN32
extern "C" {
    void *hr_dx_create(int adapter_idx, int output_idx);
    void  hr_dx_destroy(void *handle);
    int   hr_dx_get_size(void *handle, int *out_w, int *out_h);
    int   hr_dx_capture(void *handle, uint8_t *out_bgra, int timeout_ms);
    int   hr_dx_reset(void *handle);
    void  hr_dx_set_output_size(void *handle, int w, int h);
    int   hr_dx_output_desc(int adapter_idx, int output_idx, int *out_x, int *out_y,
                             int *out_w, int *out_h, char *name_buf, int name_buf_len);
    unsigned long hr_dx_last_error(void);
    void  hr_composite_cursor(uint8_t *bgra, int width, int height, int origin_x, int origin_y);
    void  hr_composite_cursor_nv12(uint8_t *nv12, int width, int height, int origin_x, int origin_y,
                                    float scale_x, float scale_y);
    void  hr_composite_cursor_yuv(uint8_t *yuv, int width, int height, int origin_x, int origin_y,
                                   float scale_x, float scale_y, int nv12);
    void  hr_composite_cursor_rgb24(uint8_t *rgb, int width, int height, int origin_x, int origin_y,
                                     float scale_x, float scale_y);
    int   hr_cursor_state(int *visible, int *x, int *y, unsigned long long *handle);
    int   hr_dx_get_dirty_rows(void *handle, int *y0, int *y1);
    void  hr_dx_get_stats(void *handle, unsigned long long *acquired, unsigned long long *skipped,
                           unsigned long long *partial, unsigned long long *full,
                           unsigned long long *gpu_frames);
    int   hr_dx_gpu_enable(void *handle, int out_w, int out_h, const uint8_t *cur_bgra);
    int   hr_dx_gpu_ready(void *handle);
    int   hr_dx_capture_nv12(void *handle, uint8_t *out_nv12, int timeout_ms,
                              int src_l, int src_t, int src_r, int src_b);
    void  hr_bgra_to_yuv420p(const uint8_t *bgra, uint8_t *yuv, int w, int h);
    void  hr_bgra_to_yuv420p_band(const uint8_t *bgra, uint8_t *yuv, int w, int h, int y0, int y1);
    void  hr_bgra_to_nv12_band(const uint8_t *bgra, uint8_t *yuv, int w, int h, int y0, int y1);
    void *hr_sw_create();
    void  hr_sw_destroy(void *handle);
    void  hr_sw_start(void *handle);
    void  hr_sw_sleep_until_ns(void *handle, int64_t target_ns);
    int64_t hr_sw_elapsed_ns(void *handle);
}

// 2.4 - the DXGI duplication source and the Windows.Graphics.Capture window source share one
// create / capture / size / reset / destroy shape. These wrappers pick the right one from the
// handle itself, so every existing g_libs.dx_* call site in this file keeps working untouched.
static void PlDxDestroy(void *h) { if (HrWgcIsHandle(h)) HrWgcDestroy(h); else hr_dx_destroy(h); }
static int  PlDxCapture(void *h, uint8_t *buf, int timeout_ms) {
    return HrWgcIsHandle(h) ? HrWgcCapture(h, buf, timeout_ms) : hr_dx_capture(h, buf, timeout_ms);
}
static int  PlDxGetSize(void *h, int *w, int *hh) {
    return HrWgcIsHandle(h) ? HrWgcGetSize(h, w, hh) : hr_dx_get_size(h, w, hh);
}
static int  PlDxReset(void *h) { return HrWgcIsHandle(h) ? HrWgcReset(h) : hr_dx_reset(h); }
static void PlDxSetOutputSize(void *h, int w, int hh) {
    if (HrWgcIsHandle(h)) return;     // the window canvas has a fixed size
    hr_dx_set_output_size(h, w, hh);
}

struct LibHandles {
    bool loaded = false;

    void *(*dx_create)(int, int)                       = &hr_dx_create;
    void  (*dx_destroy)(void*)                          = &PlDxDestroy;
    int   (*dx_capture)(void*, uint8_t*, int)           = &PlDxCapture;
    int   (*dx_get_size)(void*, int*, int*)             = &PlDxGetSize;
    int   (*dx_reset)(void*)                            = &PlDxReset;
    void  (*dx_set_output_size)(void*, int, int)        = &PlDxSetOutputSize;
    unsigned long (*dx_last_error)(void)                = &hr_dx_last_error;
    void  (*bgra_to_yuv)(const uint8_t*, uint8_t*, int, int) = &hr_bgra_to_yuv420p;
    void *(*sw_create)()                                = &hr_sw_create;
    void  (*sw_destroy)(void*)                          = &hr_sw_destroy;
    void  (*sw_start)(void*)                            = &hr_sw_start;
    void  (*sw_sleep_until)(void*, int64_t)             = &hr_sw_sleep_until_ns;
    int64_t (*sw_elapsed_ns)(void*)                     = &hr_sw_elapsed_ns;

    bool load(const wchar_t* /*base_dir*/) {
        loaded = true; // all statically linked - nothing can fail to "load" anymore
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
    g_libs.load(nullptr);
    return g_libs.loaded;
}

static void bgra_downscale_band(const uint8_t* __restrict src,
                                 uint8_t*       __restrict dst,
                                 int sw, int sh, int dw, int dh,
                                 int dy0, int dy1);

// Generic scanline-band worker pool.

class Yuv420pWorkerPool {
public:
    ~Yuv420pWorkerPool() { Stop(); }

    void SetUseNv12(bool nv12) {
        convert_band_ = nv12 ? &hr_bgra_to_nv12_band : &hr_bgra_to_yuv420p_band;
    }

    // PERF: the capture thread (HIGHEST while recording) blocks in Convert()
    void SetBoost(bool boost) { boost_.store(boost, std::memory_order_relaxed); }

    void Stop() {
        if (threads_.empty()) return;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_start_.notify_all();
        for (auto &t : threads_) if (t.joinable()) t.join();
        threads_.clear();
    }

    // Splits [0,total_rows) into worker_count()+1 bands (rounded up to a multiple of row_align
    // - 2 for YUV's chroma-paired rows, 1 for anything else) and blocks until every worker-
    // owned band AND the caller's own last band have run fn(y0,y1).
    template <class Fn>
    void RunBanded(long long total_pixels_hint, int total_rows, int row_align, Fn &&fn) {
        EnsureStarted(total_pixels_hint);
        int n = (int)threads_.size();
        if (n == 0) {
            fn(0, total_rows);
            return;
        }
        int n_bands = n + 1;
        int band_h = (total_rows / n_bands) + 1;
        if (row_align > 1) band_h = (band_h + (row_align - 1)) & ~(row_align - 1);
        if (band_h < row_align) band_h = row_align;

        {
            std::lock_guard<std::mutex> lk(mtx_);
            job_        = std::function<void(int, int)>(std::forward<Fn>(fn));
            total_rows_ = total_rows;
            band_h_     = band_h;
            pending_    = n;
            ++round_;
        }
        cv_start_.notify_all();

        // Main/calling thread does the last band itself, same as before.
        int y0 = band_h * n;
        if (y0 < total_rows) job_(y0, total_rows);

        std::unique_lock<std::mutex> lk(mtx_);
        cv_done_.wait(lk, [this] { return pending_ == 0; });
    }

    void Convert(const uint8_t *bgra, uint8_t *yuv, int w, int h) {
        auto band_fn = convert_band_;
        RunBanded((long long)w * h, h, 2,
                  [bgra, yuv, w, h, band_fn](int y0, int y1) {
                      band_fn(bgra, yuv, w, h, y0, y1);
                  });
    }

    // Parallel replacement for the old single-threaded bgra_downscale() -
    // banded by DESTINATION row (each output row is fully independent, no
    // chroma-pairing constraint, hence row_align=1). See bgra_downscale_band()
    void ConvertRows(const uint8_t *bgra, uint8_t *yuv, int w, int h, int y0, int y1) {
        if (y1 <= y0) return;
        auto band_fn = convert_band_;
        RunBanded((long long)w * (y1 - y0), y1 - y0, 2,
                  [bgra, yuv, w, h, y0, band_fn](int a, int b) {
                      band_fn(bgra, yuv, w, h, y0 + a, y0 + b);
                  });
    }

    bool IsNv12() const { return convert_band_ == &hr_bgra_to_nv12_band; }

    void Downscale(const uint8_t *src, uint8_t *dst, int sw, int sh, int dw, int dh) {
        RunBanded((long long)dw * dh, dh, 1,
                  [src, dst, sw, sh, dw, dh](int y0, int y1) {
                      bgra_downscale_band(src, dst, sw, sh, dw, dh, y0, y1);
                  });
    }

private:
    void EnsureStarted(long long total_pixels) {
        if (started_) return;
        started_ = true;
        static constexpr long long kMinPixelsForThreads = 640LL * 480LL;
        unsigned hw = std::thread::hardware_concurrency();
        int n_bands = 1;
        if (total_pixels >= kMinPixelsForThreads && hw > 1) {
            static constexpr unsigned kReservedCores = 2;
            unsigned usable = (hw > kReservedCores) ? (hw - kReservedCores) : 1;
            n_bands = (int)std::min<unsigned>(usable, 4);
            n_bands = std::max(n_bands, 1);
        }
        int n_workers = n_bands - 1; // main/calling thread does one band itself
        threads_.reserve((size_t)std::max(0, n_workers));
        for (int i = 0; i < n_workers; ++i) threads_.emplace_back([this, i] { WorkerLoop(i); });
    }

    void WorkerLoop(int idx) {
        int seen_round = 0;
        int applied_prio = THREAD_PRIORITY_NORMAL;
        for (;;) {
            std::function<void(int, int)> job;
            int total_rows, band_h;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_start_.wait(lk, [&] { return stop_ || round_ != seen_round; });
                if (stop_) return;
                seen_round = round_;
                // Copy the job (and the small ints alongside it) out while
                // still under the lock, same as the old raw-pointer version
                // did - then run it unlocked below.
                job = job_; total_rows = total_rows_; band_h = band_h_;
            }
            {
                int want = boost_.load(std::memory_order_relaxed) ? THREAD_PRIORITY_HIGHEST
                                                                  : THREAD_PRIORITY_NORMAL;
                if (want != applied_prio) { SetThreadPriority(GetCurrentThread(), want); applied_prio = want; }
            }
            int y0 = band_h * idx;
            int y1 = std::min(total_rows, y0 + band_h);
            if (y0 < y1) job(y0, y1);
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (--pending_ == 0) cv_done_.notify_one();
            }
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mtx_;
    std::condition_variable cv_start_, cv_done_;
    std::atomic<bool> boost_{false};
    bool stop_ = false;
    bool started_ = false;
    int round_ = 0;
    int pending_ = 0;
    std::function<void(int, int)> job_;
    int total_rows_ = 0, band_h_ = 0;
    void (*convert_band_)(const uint8_t*, uint8_t*, int, int, int, int) = &hr_bgra_to_yuv420p_band;
};
#endif  // _WIN32

// ---------------------------------------------------------------------------
// BGRA→thumbnail (box-filter, no intermediate RGB buffer)
// ---------------------------------------------------------------------------
static void bgra_to_thumb(const uint8_t* __restrict bgra,
                           uint8_t*       __restrict dst,
                           int sw, int sh, int dw, int dh)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    if ((sw % dw) == 0 && (sh % dh) == 0) {
        // Integer ratio. The full box filter below reads EVERY source pixel (8 MB at 1080p, scalar, on
        // the capture thread, every Preview-FPS tick - a measurable share of a weak CPU while the
        // preview is on). A thumbnail does not need that: average at most 2x2 samples per block
        // (spread over it), which touches ~1/(rx*ry/4) of the memory for the same picture.
        const int rx0 = sw / dw, ry0 = sh / dh;
        if (rx0 * ry0 > 4) {
            const int nx = std::min(rx0, 2), ny = std::min(ry0, 2);
            const int ox[2] = {rx0 / 4, (rx0 * 3) / 4};
            const int oy[2] = {ry0 / 4, (ry0 * 3) / 4};
            const uint32_t cnt = (uint32_t)(nx * ny);
            for (int y = 0; y < dh; ++y) {
                uint8_t* orow = dst + (size_t)y * dw * 3;
                const int sy0 = y * ry0;
                for (int x = 0; x < dw; ++x) {
                    uint32_t r = 0, g = 0, b = 0;
                    const int sx0 = x * rx0;
                    for (int j = 0; j < ny; ++j) {
                        const uint8_t* row = bgra + ((size_t)(sy0 + oy[j]) * sw + sx0) * 4;
                        for (int i = 0; i < nx; ++i) {
                            const uint8_t* px = row + (size_t)ox[i] * 4;
                            b += px[0]; g += px[1]; r += px[2];
                        }
                    }
                    uint8_t* o = orow + (size_t)x * 3;
                    o[0] = (uint8_t)(r / cnt);
                    o[1] = (uint8_t)(g / cnt);
                    o[2] = (uint8_t)(b / cnt);
                }
            }
            return;
        }
        // Fast integer-ratio box filter
        int rx = sw / dw, ry = sh / dh;
        int bsz = rx * ry;
        for (int y = 0; y < dh; ++y) {
            // hoist the destination row base out of the x loop -
            // was recomputed (y*dw+x)*3 from scratch for every pixel.
            uint8_t* orow = dst + (size_t)y * dw * 3;
            int sy0 = y * ry;
            for (int x = 0; x < dw; ++x) {
                uint32_t r = 0, g = 0, b = 0;
                int sx0 = x * rx;
                for (int by = 0; by < ry; ++by) {
                    const uint8_t* row = bgra + ((size_t)(sy0 + by) * sw + sx0) * 4;
                    for (int bx = 0; bx < rx; ++bx) {
                        b += row[bx*4+0];
                        g += row[bx*4+1];
                        r += row[bx*4+2];
                    }
                }
                uint8_t* o = orow + (size_t)x * 3;
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

// NV12 -> thumbnail (RGB), nearest-neighbour.
static void nv12_to_thumb(const uint8_t* __restrict nv12,
                           uint8_t*       __restrict dst,
                           int sw, int sh, int dw, int dh)
{
    if (sw <= 1 || sh <= 1 || dw <= 0 || dh <= 0) return;
    const uint8_t* Y  = nv12;
    const uint8_t* UV = nv12 + (size_t)sw * (size_t)sh;
    auto c8 = [](int v) -> uint8_t { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    for (int y = 0; y < dh; ++y) {
        int sy = (int)((int64_t)y * sh / dh); if (sy >= sh) sy = sh - 1;
        const uint8_t* yrow  = Y  + (size_t)sy * sw;
        const uint8_t* uvrow = UV + (size_t)(sy / 2) * sw;
        uint8_t* o = dst + (size_t)y * dw * 3;
        for (int x = 0; x < dw; ++x) {
            int sx = (int)((int64_t)x * sw / dw); if (sx >= sw) sx = sw - 1;
            const int l  = ((int)yrow[sx] - 16) * 298;
            const int cb = (int)uvrow[(sx & ~1) + 0] - 128;
            const int cr = (int)uvrow[(sx & ~1) + 1] - 128;
            o[0] = c8((l + 409 * cr + 128) >> 8);
            o[1] = c8((l - 100 * cb - 208 * cr + 128) >> 8);
            o[2] = c8((l + 516 * cb + 128) >> 8);
            o += 3;
        }
    }
}

static void bgra_downscale_band(const uint8_t* __restrict src,
                                 uint8_t*       __restrict dst,
                                 int sw, int sh, int dw, int dh,
                                 int dy0, int dy1)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if (dy0 < 0) dy0 = 0;
    if (dy1 > dh) dy1 = dh;
    if (dy0 >= dy1) return;

    if (sw == dw && sh == dh) {
        // Whole-frame identity copy - independent per row, so still just copies this band's rows
        // rather than special-casing a single whole-buffer memcpy (this path can now be entered
        // from several worker threads at once, each with its own [dy0,dy1)).
        std::memcpy(dst + (size_t)dy0 * dw * 4,
                    src + (size_t)dy0 * sw * 4,
                    (size_t)(dy1 - dy0) * dw * 4);
        return;
    }

    if ((sw % dw) == 0 && (sh % dh) == 0) {
        int rx = sw / dw, ry = sh / dh;
        int bsz = rx * ry;
        for (int y = dy0; y < dy1; ++y) {
            // same row-base hoist as bgra_to_thumb() above.
            uint8_t* orow = dst + (size_t)y * dw * 4;
            int sy0 = y * ry;
            for (int x = 0; x < dw; ++x) {
                uint32_t b = 0, g = 0, r = 0;
                int sx0 = x * rx;
                for (int by = 0; by < ry; ++by) {
                    const uint8_t* row = src + ((size_t)(sy0 + by) * sw + sx0) * 4;
                    for (int bx = 0; bx < rx; ++bx) {
                        b += row[bx*4+0];
                        g += row[bx*4+1];
                        r += row[bx*4+2];
                    }
                }
                uint8_t* o = orow + (size_t)x * 4;
                o[0] = (uint8_t)(b / (uint32_t)bsz);
                o[1] = (uint8_t)(g / (uint32_t)bsz);
                o[2] = (uint8_t)(r / (uint32_t)bsz);
                o[3] = 255;
            }
        }
    } else {
        // Nearest-neighbour fallback (non-integer ratio, e.g.
        float rx = (float)sw / dw, ry = (float)sh / dh;
        for (int y = dy0; y < dy1; ++y) {
            int sy = (int)(y * ry); if (sy >= sh) sy = sh - 1;
            const uint8_t* srow = src + (size_t)sy * sw * 4;
            uint8_t*       orow = dst + (size_t)y  * dw * 4;
            for (int x = 0; x < dw; ++x) {
                int sx = (int)(x * rx); if (sx >= sw) sx = sw - 1;
                const uint8_t* s = srow + (size_t)sx * 4;
                uint8_t*       d = orow + (size_t)x  * 4;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
            }
        }
    }
}

[[maybe_unused]] static void bgra_downscale(const uint8_t* __restrict src,
                            uint8_t*       __restrict dst,
                            int sw, int sh, int dw, int dh)
{
    bgra_downscale_band(src, dst, sw, sh, dw, dh, 0, dh);
}

// Outstanding "handed-off" Pipelines - see hr_pl_destroy()'s handed_off/
// leak_pl comments below for what this refers to.
static std::atomic<int> g_handed_off_pipelines{0};

// Process-wide counter behind Pipeline::pv_seq.
static std::atomic<uint64_t> g_pv_seq_counter{0};

// ---------------------------------------------------------------------------
// Pipeline state
// ---------------------------------------------------------------------------
struct Pipeline {
    int src_w = 0, src_h = 0;
    std::atomic<int> fps{30};
    int pv_w  = 960, pv_h = 540;
    std::atomic<int> preview_fps{15};

    // Whether anything is actually going to consume the live-preview thumbnail this pipeline
    // produces (hr_pl_get_preview()).
    std::atomic<bool> preview_needed{true};

    // pipe_handle and `recording` are written by the UI thread
    // (hr_pl_set_recording()/hr_pl_end_recording_segment()) while the capture and writer
    // threads read - and the writer thread also zeroes - them, with no synchronisation at all.
    std::atomic<intptr_t> pipe_handle{0};
    std::atomic<bool> recording{false};   // pipe open → encode YUV; false → preview only

    std::atomic<bool> boost_priority{false};
    std::atomic<bool> light_cpu_path{false};

    int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
    // Guards the four crop_* fields above: hr_pl_set_capture_rect() runs on the UI thread
    // while capture_loop() may be mid-frame on this pipeline (Start() re-targets an already-
    // running preview pipeline).
    std::mutex crop_mtx;

    // "Effective" frame dimensions for THIS frame: src_w/src_h when crop_w==0, or
    // crop_w/crop_h when a crop rect is active.
    int eff_w = 0, eff_h = 0;

    std::atomic<int> out_w{0}, out_h{0};
    std::vector<uint8_t> scaled_buf;

    void* dx_ctx = nullptr;
    void* sw_ctx = nullptr;
    // 2.4: dx_ctx is a Windows.Graphics.Capture window source (wgc_capture.cpp), not a DXGI
    // duplication. It has no GPU-NV12 path, no dirty rectangles and draws the cursor itself.
    bool  wgc_mode = false;

    Yuv420pWorkerPool yuv_pool;

    // "Cursor" setting: whether to draw the live system cursor into each
    // captured frame (see hr_composite_cursor() in ui_utils.cpp).
    std::atomic<bool> include_cursor{false};
    int cap_origin_x = 0, cap_origin_y = 0;

    std::vector<uint8_t> bgra_buf;  // src_w * src_h * 4 -- ALWAYS the pristine, untouched last full-desktop frame

    // Per-tick work frame (cropped window rect and/or overlays/cursor baked in).
    std::vector<uint8_t> work_buf;
    // Pointer to the frame every downstream step reads this tick:
    // bgra_buf.data() (no crop/overlay/cursor -- zero extra cost) or
    // work_buf.data(). Set each tick in capture_loop(); read by update_preview().
    const uint8_t* frame_ptr = nullptr;
    // The CPU path no longer bakes the pointer into the capture buffer (it is blended into the
    // converted YUV frame), so the live preview stamps it onto its own small thumbnail instead.
    bool pv_cursor = false;
    int  pv_cursor_ox = 0, pv_cursor_oy = 0;   // thumbnail source's top-left on the virtual desktop

    // Overlays configured from the UI (RecordingController pushes the
    // current list in via hr_pl_set_overlays whenever it changes -- drag,
    // resize, or edited through the Overlay Manager dialog).
    std::mutex overlays_mtx;
    std::vector<HrOverlayDesc> overlays;
    OverlayCompositor overlay_compositor;
    // OPT: bumped every time hr_pl_set_overlays() actually changes the list.
    std::atomic<uint64_t> overlays_gen{0};

    // Preview
    std::vector<uint8_t> pv_buf;
    std::mutex           pv_mtx;
    int  pv_actual_w = 0, pv_actual_h = 0;
    int  pv_native_w = 0, pv_native_h = 0;
    bool pv_ready    = false;
    // Bumped (from g_pv_seq_counter, so it is unique across pipelines) every time
    // update_preview() publishes a new thumbnail.
    std::atomic<uint64_t> pv_seq{0};

    std::thread       capture_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};

    std::mutex              finish_mtx;
    std::condition_variable finish_cv;
    std::atomic<bool>       writer_thread_done{false};
    std::atomic<bool>       capture_thread_done{false};

    std::atomic<bool>       pending_close_requested{false};
    std::atomic<bool>       pending_close_done{false};

#ifdef _WIN32
    // Manual-reset event for overlapped writes to ffmpeg's stdin pipe - see write_pipe()'s own
    // comment for why this exists.
    HANDLE write_evt = nullptr;
#endif

    // ====== SELF-CLEANUP ON A TIMED-OUT SHUTDOWN ======

    std::atomic<int>  threads_remaining{2};
    std::atomic<bool> handed_off{false};

    void finish_thread() {
        if (threads_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
            handed_off.load(std::memory_order_acquire)) {
#ifdef _WIN32
            if (dx_ctx && g_libs.dx_destroy) g_libs.dx_destroy(dx_ctx);
            if (sw_ctx && g_libs.sw_destroy) g_libs.sw_destroy(sw_ctx);
            if (write_evt) { CloseHandle(write_evt); write_evt = nullptr; }
#endif
            delete this;
            g_handed_off_pipelines.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
    bool logged_lost_ = false; // edge-trigger for the DX_LOST diagnostic below
    bool logged_reset_fail_ = false; // edge-trigger for the reset-failed diagnostic below
    std::chrono::steady_clock::time_point next_reset_attempt_{};

    std::atomic<int64_t> frames_captured{0};
    std::atomic<int64_t> frames_dropped{0};
    std::atomic<int64_t> frames_stalled{0};
    std::atomic<double>  fps_actual{0.0};

    // ====== FRAME QUEUE FOR ASYNC WRITING ====== One queue entry = one converted YUV frame
    // plus how many consecutive times it must be written to the pipe.
    struct QueuedFrame {
        std::vector<uint8_t> data;
        int repeat = 1;          // raw pipe: how many times the writer sends `data`
        // Timestamped pipe (ts_fps > 0): slot on the constant-fps timeline this picture starts at, or
        // -1 for the raw pipe. `repeat` is always 1 then - ffmpeg fills the gap to the next frame.
        int64_t pts = -1;
        int  w = 0, h = 0;       // size / layout of `data` (needed for the NUT stream header)
        bool nv12 = false;
    };
    // ---- timestamped pipe (static-screen frames are not sent at all) ----
    // > 0: frames go to ffmpeg inside a NUT stream (1/ts_fps time base) with explicit slot numbers
    // instead of as back-to-back raw frames; a frame is only sent when the picture changed.
    // Set by hr_pl_set_timestamped_pipe() BEFORE hr_pl_set_recording(.., 1, ..).
    std::atomic<int>     ts_fps{0};
    // Capture thread -> writer: first timeline slot that has NOT elapsed yet. The writer uses it to
    // finish the stream with the last picture held until the real end, and as a once-a-second
    // heartbeat on a static screen (see writer_loop()).
    std::atomic<int64_t> ts_end_slot{0};
    std::queue<QueuedFrame> pipe_queue;
    std::mutex pipe_queue_mtx;
    std::condition_variable pipe_queue_cv;
    std::thread writer_thread;
    std::atomic<bool> writer_running{false};
    static constexpr size_t MAX_QUEUE_SIZE = 8;  // ~25MB at 1080p NV12. Was 3: any 100ms hiccup in ffmpeg/QSV/disk (or a
                                                  // game stealing the CPU) evicted frames -> "Recording overloaded".
                                                  // Latency is irrelevant when recording to a file.

    std::vector<uint8_t> last_yuv;
    bool last_yuv_valid = false;
    int  last_yuv_w = 0, last_yuv_h = 0;
    // true when last_yuv was converted straight from bgra_buf (no crop,
    // overlay, cursor or scaling in between) - only then can DXGI's "these
    // rows changed" hint be applied on top of it (partial re-conversion).
    bool last_yuv_direct = false;

    std::atomic<bool> gpu_convert{false};
    bool gpu_failed = false;                  // capture thread only: give up for this pipeline
    bool gpu_fail_transient = false;          // gpu_failed came from a runtime error that is worth retrying
    int  gpu_fail_count = 0;                  // runtime GPU errors so far (retry limit 3)
    std::chrono::steady_clock::time_point gpu_retry_at{};
    bool logged_gpu_scale_ = false;           // "desktop size changed, GPU path scales" logged once
    bool nv12_valid = false;                  // a complete GPU-converted frame has been received at least once
    bool nv12_clean = false;                  // ...and nv12_buf still holds it (false after a zero-copy hand-over to the writer)
    std::vector<uint8_t> nv12_buf;            // latest clean GPU-converted frame
    const uint8_t* frame_nv12 = nullptr;      // set on GPU ticks so update_preview() can use it
    std::vector<uint8_t> pv_nv12;             // GPU ticks with overlays: preview copy that has them baked in
    int  nv12_w = 0, nv12_h = 0;
    uint64_t st_partial_conv = 0, st_gpu_ticks = 0;

    std::queue<std::vector<uint8_t>> free_bufs;
    std::mutex free_bufs_mtx;
    static constexpr size_t MAX_FREE_BUFS = MAX_QUEUE_SIZE + 2;

    // -------------------------------------------------------------------------
    // Write raw bytes to pipe
    // -------------------------------------------------------------------------
    bool write_pipe(const uint8_t* data, size_t total) {
        const intptr_t ph = pipe_handle.load(std::memory_order_acquire);
        if (ph == 0 || ph == -1) return false;
        size_t written = 0;
#ifdef _WIN32
        HANDLE h = reinterpret_cast<HANDLE>(ph);
        if (!write_evt) write_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        while (written < total) {
            OVERLAPPED ov{};
            ov.hEvent = write_evt;
            ResetEvent(write_evt);
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(total - written, 1u << 20));
            DWORD w = 0;
            BOOL ok = WriteFile(h, data + written, chunk, nullptr, &ov);
            if (!ok && GetLastError() != ERROR_IO_PENDING) return false;

            for (;;) {
                if (GetOverlappedResult(h, &ov, &w, FALSE)) break;
                DWORD err = GetLastError();
                if (err != ERROR_IO_INCOMPLETE) { CancelIoEx(h, &ov); return false; }
                if (!writer_running.load(std::memory_order_relaxed)) {
                    CancelIoEx(h, &ov);
                    GetOverlappedResult(h, &ov, &w, TRUE); // let the cancel land before reusing ov
                    return false;
                }
                WaitForSingleObject(write_evt, 50);
            }
            if (w == 0) return false;
            written += w;
        }
#else
        while (written < total) {
            ssize_t r = ::write(static_cast<int>(ph),
                                data + written, total - written);
            if (r <= 0) return false;
            written += static_cast<size_t>(r);
        }
#endif
        return true;
    }

    // -------------------------------------------------------------------------
    // Writer thread - consumes frames from queue and writes to pipe
    // -------------------------------------------------------------------------
    void writer_loop() {
#ifdef _WIN32
        bool writer_was_boosted = boost_priority.load(std::memory_order_relaxed) &&
                                  !light_cpu_path.load(std::memory_order_relaxed);
        SetThreadPriority(GetCurrentThread(), writer_was_boosted
                                                   ? THREAD_PRIORITY_HIGHEST
                                                   : THREAD_PRIORITY_ABOVE_NORMAL);
#if defined(HR_HAS_SET_THREAD_DESC)
        typedef HRESULT (WINAPI *PFN_SET_THREAD_DESC)(HANDLE, PCWSTR);
        static PFN_SET_THREAD_DESC set_thread_desc =
            (PFN_SET_THREAD_DESC)GetProcAddress(
                GetModuleHandleW(L"KernelBase.dll"), "SetThreadDescription");
        if (set_thread_desc) {
            set_thread_desc(GetCurrentThread(), L"HomRec Writer");
        }
#endif
#endif

        writer_running.store(true, std::memory_order_relaxed);
        bool logged_write_fail = false;   // edge-trigger for the write-failure diagnostic below

        // ------ timestamped (NUT) pipe state, only touched by this thread ------
        std::vector<uint8_t> nut_prefix;    // sync point + frame header scratch
        std::vector<uint8_t> nut_last;      // the picture most recently written (kept for the tail/heartbeat)
        int64_t  nut_last_pts = -1;
        bool     nut_header_done = false;
        intptr_t nut_pipe = 0;              // the pipe handle the header went to
        auto nut_write = [&](const uint8_t *data, size_t n, int64_t pts) -> bool {
            hrnut::make_frame_prefix(nut_prefix, pts, n);
            return write_pipe(nut_prefix.data(), nut_prefix.size()) && write_pipe(data, n);
        };
        auto nut_reset = [&]() {
            nut_header_done = false;
            nut_last_pts = -1;
            nut_last.clear();
        };
        // Hold the last picture until the end of the timeline: a frame lasts until the next one, so
        // without this the final stretch of a static screen would be cut off at the last change.
        auto nut_finish = [&]() {
            if (nut_header_done && !nut_last.empty() && nut_last_pts >= 0) {
                const int64_t end_slot = ts_end_slot.load(std::memory_order_acquire);
                if (end_slot - 1 > nut_last_pts && nut_write(nut_last.data(), nut_last.size(), end_slot - 1))
                    nut_last_pts = end_slot - 1;
            }
            nut_reset();
        };
        // On a static screen nothing is queued, and ffmpeg only emits a frame once it sees the next one.
        // Repeat the last picture about once a second so the encoder / Instant Replay segments never lag
        // further behind than that.
        auto nut_heartbeat_due = [&]() -> bool {
            const int tf = ts_fps.load(std::memory_order_relaxed);
            if (tf <= 0 || !nut_header_done || nut_last.empty() || nut_last_pts < 0) return false;
            if (!recording.load(std::memory_order_relaxed)) return false;
            return ts_end_slot.load(std::memory_order_acquire) - 1 - nut_last_pts >= tf;
        };

        try {
        while (writer_running.load(std::memory_order_relaxed)) {
#ifdef _WIN32
            // See the matching comment above (~line 620) - boost_priority can change independently of
            // this thread's lifetime, so this needs to be checked every iteration, not just once at
            // thread-start, exactly like capture_loop()'s own want_boost check.
            bool writer_want_boost = boost_priority.load(std::memory_order_relaxed) &&
                                     !light_cpu_path.load(std::memory_order_relaxed);
            if (writer_want_boost != writer_was_boosted) {
                writer_was_boosted = writer_want_boost;
                SetThreadPriority(GetCurrentThread(), writer_want_boost
                                      ? THREAD_PRIORITY_HIGHEST
                                      : THREAD_PRIORITY_ABOVE_NORMAL);
            }
#endif
            std::vector<uint8_t> frame;
            int frame_repeat = 1;
            int64_t frame_pts = -1;
            int  frame_w = 0, frame_h = 0;
            bool frame_nv12 = false;
            bool heartbeat = false;

            {
                std::unique_lock<std::mutex> lock(pipe_queue_mtx);

                // Wait for frames, a shutdown signal or a request to close the pipe. Every one of
                // those is signalled through pipe_queue_cv (enqueue_frame(), hr_pl_stop(),
                // hr_pl_destroy(), hr_pl_end_recording_segment()), so the timeout is only a safety
                // net against a missed wake-up. It used to be 10 ms with a predicate that ignored
                // pending_close_requested: an idle preview-only pipeline woke the writer 100x per
                // second forever, and closing a segment always cost up to a full poll interval.
                pipe_queue_cv.wait_for(lock, std::chrono::milliseconds(50), [this]() {
                    return !pipe_queue.empty() ||
                           !writer_running.load(std::memory_order_relaxed) ||
                           pending_close_requested.load(std::memory_order_acquire);
                });

                // Exit if shutting down and queue is empty
                if (!writer_running.load(std::memory_order_relaxed) && pipe_queue.empty()) {
                    break;
                }

                if (pipe_queue.empty()) {
                    if (pending_close_requested.load(std::memory_order_acquire)) {
                        nut_finish();     // timestamped pipe: flush the held tail picture before EOF
                        const intptr_t ph = pipe_handle.exchange(0, std::memory_order_acq_rel);
                        if (ph != 0 && ph != -1) {
#ifdef _WIN32
                            CloseHandle(reinterpret_cast<HANDLE>(ph));
#else
                            ::close(static_cast<int>(ph));
#endif
                        }
                        pending_close_requested.store(false, std::memory_order_release);
                        {
                            std::lock_guard<std::mutex> lk(finish_mtx);
                            pending_close_done.store(true, std::memory_order_release);
                        }
                        finish_cv.notify_all();
                    }
                    if (!nut_heartbeat_due()) continue;
                    heartbeat = true;
                } else {
                // Move frame out of queue (no copy)
                frame = std::move(pipe_queue.front().data);
                frame_repeat = pipe_queue.front().repeat;
                frame_pts = pipe_queue.front().pts;
                frame_w = pipe_queue.front().w;
                frame_h = pipe_queue.front().h;
                frame_nv12 = pipe_queue.front().nv12;
                pipe_queue.pop();
                }
            }

            if (heartbeat) {
                const int64_t hb_pts = ts_end_slot.load(std::memory_order_acquire) - 1;
                if (hb_pts > nut_last_pts && nut_write(nut_last.data(), nut_last.size(), hb_pts))
                    nut_last_pts = hb_pts;
                continue;
            }

            // Write to pipe outside the lock
            if (!frame.empty()) {
                bool ok = true;
                if (frame_pts >= 0) {
                    // Timestamped pipe: stream header once per pipe, then one packet per picture.
                    const intptr_t cur_pipe = pipe_handle.load(std::memory_order_acquire);
                    if (nut_header_done && cur_pipe != nut_pipe) nut_reset();   // a new recording's pipe
                    if (!nut_header_done) {
                        const std::vector<uint8_t> hdr = hrnut::make_header(
                            frame_w, frame_h, frame_nv12 ? "NV12" : "I420",
                            ts_fps.load(std::memory_order_relaxed));
                        ok = write_pipe(hdr.data(), hdr.size());
                        nut_header_done = true;       // even on failure: don't re-send into a dead pipe
                        nut_pipe = cur_pipe;
                    }
                    if (ok) ok = nut_write(frame.data(), frame.size(), frame_pts);
                    if (ok) {
                        // keep this picture for the tail / heartbeat; recycle the one it replaces
                        nut_last.swap(frame);
                        nut_last_pts = frame_pts;
                    }
                } else {
                    for (int rep = 0; rep < frame_repeat && ok; ++rep) {
                        ok = write_pipe(frame.data(), frame.size());
                    }
                }

                // Hand the buffer back to the free-list instead of letting
                // it fall out of scope and get freed - the capture thread
                // will reuse it for the next frame (no realloc/memcpy).
                if (!frame.empty()) {
                    std::lock_guard<std::mutex> lock(free_bufs_mtx);
                    if (free_bufs.size() < MAX_FREE_BUFS)
                        free_bufs.push(std::move(frame));
                }

                // A failed write used to be swallowed without a trace, so "the recording just stops
                // growing" (ffmpeg died / disk full / pipe broken) left nothing in the log.
                if (!ok && !logged_write_fail && recording.load(std::memory_order_relaxed) &&
                    writer_running.load(std::memory_order_relaxed)) {
                    logged_write_fail = true;
                    HrLog::Warn("Writer thread: writing a frame to the encoder's stdin pipe failed (the encoder "
                                "process has probably exited, or the disk is full) - frames are being discarded.");
                } else if (ok) {
                    logged_write_fail = false;
                }
            }
        }
        } catch (const std::exception &e) {
            HrLog::Error(std::string("Writer thread: uncaught exception (") + e.what() +
                         ") -- this pipeline is stopping instead of crashing the app.");
        } catch (...) {
            HrLog::Error("Writer thread: uncaught unknown exception -- this pipeline is "
                         "stopping instead of crashing the app.");
        }

        // pipe_handle is the same OS
        // handle as ffmpeg's stdin. hr_ff_stop_graceful() (ffmpeg_runner.cpp)
        if (writer_running.load(std::memory_order_relaxed) == false && nut_header_done) {
            try { nut_finish(); }     // normal shutdown with a pipe still open (not after a crash)
            catch (...) {}
        }
        {
            const intptr_t ph = pipe_handle.exchange(0, std::memory_order_acq_rel);
            if (ph != 0 && ph != -1) {
#ifdef _WIN32
                CloseHandle(reinterpret_cast<HANDLE>(ph));
#else
                ::close(static_cast<int>(ph));
#endif
            }
        }

        // Portable completion signal - see writer_thread_done's declaration
        // for why this replaced a WaitForSingleObject-on-native_handle()
        // check on the waiting side.
        {
            std::lock_guard<std::mutex> lk(finish_mtx);
            writer_thread_done.store(true, std::memory_order_release);
        }
        finish_cv.notify_all();

        finish_thread();
    }

    // Update preview thumbnail - directly from BGRA, no intermediate RGB copy
    // OPT: устранён bgra_to_rgb_inplace() и буфер rgb_pv
    void update_preview() {
        std::lock_guard<std::mutex> lock(pv_mtx);   // also guards pv_w/pv_h (hr_pl_set_preview_size)
        int tw = pv_w, th = pv_h;
        if (eff_w > 0 && eff_h > 0) {
            float ar = (float)eff_w / (float)eff_h;
            if (tw > (int)(th * ar)) tw = (int)(th * ar);
            else                      th = (int)(tw / ar);
        }
        tw = std::max(tw & ~1, 2);
        th = std::max(th & ~1, 2);

        size_t pv_sz = (size_t)tw * th * 3;
        if (pv_buf.size() != pv_sz) pv_buf.resize(pv_sz);

        if (frame_nv12 && nv12_w > 0 && nv12_h > 0)
            nv12_to_thumb(frame_nv12, pv_buf.data(), nv12_w, nv12_h, tw, th);
        else {
            bgra_to_thumb(frame_ptr ? frame_ptr : bgra_buf.data(), pv_buf.data(), eff_w, eff_h, tw, th);
#ifdef _WIN32
            if (pv_cursor && eff_w > 0 && eff_h > 0)
                hr_composite_cursor_rgb24(pv_buf.data(), tw, th, pv_cursor_ox, pv_cursor_oy,
                                          (float)tw / (float)eff_w, (float)th / (float)eff_h);
#endif
        }
        pv_actual_w = tw;
        pv_actual_h = th;
        pv_native_w = eff_w;
        pv_native_h = eff_h;
        pv_ready    = true;
        pv_seq.store(g_pv_seq_counter.fetch_add(1, std::memory_order_relaxed) + 1,
                     std::memory_order_release);
    }

    // -------------------------------------------------------------------------
    // Main capture loop
    // -------------------------------------------------------------------------
    void capture_loop() {
#ifdef _WIN32
        // WIC (used by overlay_render.cpp to decode image overlays)
        // requires COM on the calling thread. Harmless if overlays are
        // never used -- CoUninitialize() below just undoes this.
        bool com_inited = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        // Start at a moderate priority; bumped to TIME_CRITICAL only while
        // actually recording (see the dynamic adjustment in the loop below).
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#if defined(HR_HAS_SET_THREAD_DESC)
        typedef HRESULT (WINAPI *PFN_SET_THREAD_DESC)(HANDLE, PCWSTR);
        static PFN_SET_THREAD_DESC set_thread_desc =
            (PFN_SET_THREAD_DESC)GetProcAddress(
                GetModuleHandleW(L"KernelBase.dll"), "SetThreadDescription");
        if (set_thread_desc) {
            set_thread_desc(GetCurrentThread(), L"HomRec Capture");
        }
#endif
#endif
        auto compute_frame_ns_recording = [&]() -> int64_t {
            const int f = fps.load(std::memory_order_relaxed);
            return (f > 0) ? (1'000'000'000LL / f) : (1'000'000'000LL / 30);
        };
        int64_t frame_ns_recording = compute_frame_ns_recording();
        // Preview-only mode (not recording) never needs more than a
        // handful of frames/sec - the thumbnail shown in the UI is
        // already throttled by the preview-fps check further down.
        auto compute_frame_ns_idle = [&]() {
            int want_fps = preview_fps.load(std::memory_order_relaxed);
            if (want_fps < 1) want_fps = 1;
            return std::max(frame_ns_recording, (int64_t)(1'000'000'000LL / want_fps));
        };
        int64_t frame_ns_idle = compute_frame_ns_idle();
        bool was_recording = recording.load(std::memory_order_acquire);
        // Start at the pace matching the CURRENT mode.
        int64_t frame_ns = was_recording ? frame_ns_recording : frame_ns_idle;
        bool was_boosted = boost_priority.load(std::memory_order_relaxed);
        bool thread_is_high_prio = was_boosted;   // mirrors the SetThreadPriority() just below
        yuv_pool.SetBoost(was_boosted);
#ifdef _WIN32
        // This used to jump straight to THREAD_PRIORITY_TIME_CRITICAL the moment recording started
        // (both here and in the is_recording_now transition below).
        SetThreadPriority(GetCurrentThread(), boost_priority.load(std::memory_order_relaxed)
                                                  ? THREAD_PRIORITY_HIGHEST
                                                  : THREAD_PRIORITY_ABOVE_NORMAL);
#endif

        int64_t last_preview_ns = 0;

        // timeout_ms = 2/3 frame (was 1/2) → fewer TIMEOUT drops
        int timeout_ms = static_cast<int>(frame_ns * 2 / 3'000'000LL);
        if (timeout_ms < 8)  timeout_ms = 8;
        if (timeout_ms > 33) timeout_ms = 33;

        int64_t fps_acc_frames = 0, fps_acc_start_ns = 0;

#ifdef _WIN32
        if (sw_ctx && g_libs.sw_start) { g_libs.sw_start(sw_ctx); last_preview_ns = 0; }  // the clock restarts at 0: a preview stamp from the OLD clock would freeze the preview until the new one caught up
#endif
        int64_t next_frame_ns = frame_ns;

        // OPT: see overlays_gen's declaration -- this snapshot is only
        // refreshed (under overlays_mtx) when the generation counter has
        // actually moved, instead of every single captured frame.
        std::vector<HrOverlayDesc> overlays_snapshot;
        // True only if some overlay would actually be DRAWN (same visibility/size test
        // OverlayCompositor::Apply() uses).
        bool overlays_active = false;
        // 8th pass: some overlay reads / blanks the captured monitor picture (region / window / hidden
        // Desktop layer) - that picture only exists as BGRA on the CPU, so no GPU colour conversion.
        bool overlays_need_base = false;
        uint64_t last_overlays_gen = (uint64_t)-1; // sentinel: forces the first copy below
        bool warned_bad_crop = false;              // see the crop clamp below - log it once, not per frame

        // The whole loop below now runs inside a try/catch.
        struct CursorSig {
            int vis = 0, x = 0, y = 0;
            unsigned long long handle = 0;
            int64_t bucket = -1;
            bool operator==(const CursorSig &o) const {
                return vis == o.vis && x == o.x && y == o.y && handle == o.handle && bucket == o.bucket;
            }
        };
        auto read_cursor_sig = [&]() -> CursorSig {
            CursorSig sg;
#ifdef _WIN32
            hr_cursor_state(&sg.vis, &sg.x, &sg.y, &sg.handle);
#endif
            sg.bucket = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
            return sg;
        };
        CursorSig gpu_emit_sig;      // pointer state baked into the last GPU-path frame we queued
        bool gpu_emit_valid = false; // the picture at the back of the queue came from the GPU path
        CursorSig cpu_emit_sig;
        bool cpu_emit_valid = false;
        int  cpu_emit_w = 0, cpu_emit_h = 0;

        // ---- timestamped pipe bookkeeping (capture thread only) ----
        // Every recording tick owns one slot on the constant-fps timeline (plus the catch-up slots
        // when the loop ran late). A picture that changed is queued with its slot; an unchanged tick
        // queues nothing and ffmpeg repeats the previous picture across the gap.
        int64_t ts_next_slot = 0;
        bool    ts_have_frame = false;   // at least one picture was queued in this recording
        auto ts_on = [&]() -> bool {
            return ts_fps.load(std::memory_order_relaxed) > 0 && recording.load(std::memory_order_acquire);
        };
        auto ts_take_slot = [&](int extra) -> int64_t {
            // The picture is "now": the `extra` slots the loop was late by still show the previous one.
            const int64_t pts = ts_next_slot + extra;
            ts_next_slot = pts + 1;
            ts_end_slot.store(ts_next_slot, std::memory_order_release);
            return pts;
        };
        bool preview_suppressed = false;   // GPU zero-copy hand-over moved the buffer the preview reads

        // Make the frame at the back of the queue last `1 + extra` slots longer.
        auto try_merge_last = [&](int extra) -> bool {
            std::lock_guard<std::mutex> lock(pipe_queue_mtx);
            if (pipe_queue.empty()) return false;
            int r = pipe_queue.back().repeat + 1 + extra;
            pipe_queue.back().repeat = std::min(r, 240);
            return true;
        };
        // Conversion writes into a buffer recycled from the free-list (filled by
        // the writer thread once it's done with a frame) - no per-frame heap
        // allocation.
        auto take_free_buf = [&](size_t needed) -> std::vector<uint8_t> {
            std::vector<uint8_t> b;
            {
                std::lock_guard<std::mutex> lock(free_bufs_mtx);
                if (!free_bufs.empty()) {
                    b = std::move(free_bufs.front());
                    free_bufs.pop();
                }
            }
            if (b.size() != needed) b.resize(needed);
            return b;
        };
        // Copy rows [y0,y1) (both even) of a planar-I420 / NV12 frame.
        auto copy_yuv_rows = [](uint8_t *dst, const uint8_t *src, int w, int h, int y0, int y1, bool nv12) {
            if (y1 <= y0) return;
            std::memcpy(dst + (size_t)y0 * w, src + (size_t)y0 * w, (size_t)(y1 - y0) * w);
            const size_t base = (size_t)w * h;
            if (nv12) {
                std::memcpy(dst + base + (size_t)(y0 / 2) * w, src + base + (size_t)(y0 / 2) * w,
                            (size_t)((y1 - y0) / 2) * w);
            } else {
                const size_t cw = (size_t)w / 2, plane = cw * (size_t)(h / 2);
                for (int pl = 0; pl < 2; ++pl) {
                    const size_t off = base + (size_t)pl * plane + (size_t)(y0 / 2) * cw;
                    std::memcpy(dst + off, src + off, (size_t)((y1 - y0) / 2) * cw);
                }
            }
        };
        // Hand a finished frame to the writer thread (moved, not copied).
        auto enqueue_frame = [&](std::vector<uint8_t> &yuv_frame, int extra_slots_,
                                 int fw, int fh, bool fnv12, int64_t pts) {
            std::vector<uint8_t> dropped;  // popped outside free_bufs_mtx to avoid nested locks
            int carried_repeat = 0;        // timeline slots owned by an evicted entry
            {
                std::lock_guard<std::mutex> lock(pipe_queue_mtx);

                // Backpressure: the queue is capped at MAX_QUEUE_SIZE entries.
                if (pipe_queue.size() >= MAX_QUEUE_SIZE) {
                    dropped = std::move(pipe_queue.front().data);
                    // Raw pipe: the evicted picture's slots move to the new one. Timestamped pipe: its
                    // slots are simply covered by the picture before it (ffmpeg fills the gap).
                    carried_repeat = (pts < 0) ? pipe_queue.front().repeat : 0;
                    pipe_queue.pop();
                    frames_dropped.fetch_add(1, std::memory_order_relaxed);
                    frames_stalled.fetch_add(1, std::memory_order_relaxed);
                }

                QueuedFrame qf;
                qf.data   = std::move(yuv_frame);
                qf.repeat = (pts < 0) ? std::min(1 + extra_slots_ + carried_repeat, 240) : 1;
                qf.pts    = pts;
                qf.w = fw; qf.h = fh; qf.nv12 = fnv12;
                pipe_queue.push(std::move(qf));
                pipe_queue_cv.notify_one();
            }

            if (!dropped.empty()) {
                std::lock_guard<std::mutex> lock(free_bufs_mtx);
                if (free_bufs.size() < MAX_FREE_BUFS)
                    free_bufs.push(std::move(dropped));
            }
        };

        // ---- stall diagnostics ------------------------------------------------- A benchmark of
        // a game with the recorder running showed a 0.1 FPS minimum that the logs could not
        // attribute to anything.
        using hitch_clock = std::chrono::steady_clock;
        hitch_clock::time_point hitch_last_tick{};
        bool   hitch_last_valid = false;
        int    hitch_count = 0, hitch_logged = 0;
        double hitch_worst_ms = 0.0, last_cap_ms = 0.0;
        // Where the previous tick spent its time (benchmark logs: a 6 s hitch of which the DXGI reset was only 2.5 s):
        // everything before the capture call (GPU path size/enable checks), the DXGI reset, and - by subtraction - the rest.
        double last_prep_ms = 0.0, last_reset_ms = 0.0;

        // Preview tick (Preview FPS setting). A lambda so the GPU zero-copy branch can run it BEFORE it
        // hands nv12_buf over to the writer - the thumbnail reads that buffer.
        auto preview_if_due = [&]() {
#ifdef _WIN32
            if (preview_suppressed) return;
            int64_t now_ns = (sw_ctx && g_libs.sw_elapsed_ns)
                                  ? g_libs.sw_elapsed_ns(sw_ctx)
                                  : next_frame_ns; // fallback: still monotonic
            int want_fps = preview_fps.load(std::memory_order_relaxed);
            if (want_fps < 1) want_fps = 1;
            int64_t preview_interval_ns = 1'000'000'000LL / want_fps;
            if (preview_needed.load(std::memory_order_relaxed) &&
                now_ns - last_preview_ns >= preview_interval_ns) {
                last_preview_ns = now_ns;
                update_preview();
            }
#endif
        };

        try {
        while (running.load(std::memory_order_relaxed)) {
            preview_suppressed = false;
            if (paused.load(std::memory_order_relaxed)) {
                hitch_last_valid = false;   // a pause is not a stall
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                fps_acc_frames = 0; fps_acc_start_ns = 0;   // same stale-clock problem for the measured-fps stat
#ifdef _WIN32
                if (sw_ctx && g_libs.sw_start) { g_libs.sw_start(sw_ctx); last_preview_ns = 0; }  // the clock restarts at 0: a preview stamp from the OLD clock would freeze the preview until the new one caught up
#endif
                next_frame_ns = frame_ns;
                continue;
            }

            // Recording can start/stop mid-flight on this same pipeline now (see
            // hr_pl_set_recording()/RecordingController's preview reuse) - react to that instead of
            // only sizing pacing/ priority once when the thread was first created.
            {
                const bool is_recording_now = recording.load(std::memory_order_acquire);
                // Live-apply Target FPS / Preview FPS changes (cheap atomic
                // loads) even when the recording state itself didn't flip.
                {
                    const int64_t new_rec_ns = compute_frame_ns_recording();
                    if (new_rec_ns != frame_ns_recording) {
                        frame_ns_recording = new_rec_ns;
                        next_frame_ns = 0; // resync pacing to the new cadence
                    }
                    frame_ns_idle = compute_frame_ns_idle();
                    const int64_t want_ns = is_recording_now ? frame_ns_recording : frame_ns_idle;
                    if (want_ns != frame_ns) {
                        frame_ns = want_ns;
                        next_frame_ns = 0;
                    }
                }
                if (is_recording_now != was_recording) {
                    was_recording = is_recording_now;
                    if (!is_recording_now && hitch_count > 0) {
                        HrLog::Info("Capture hitches this recording: " + std::to_string(hitch_count) +
                                    " (worst " + std::to_string((long long)hitch_worst_ms) + " ms between two capture ticks)");
                    }
                    hitch_count = 0; hitch_logged = 0; hitch_worst_ms = 0.0; hitch_last_valid = false;
                    last_yuv_valid = false;   // pixel format / size may differ next recording
                    cpu_emit_valid = false;
                    gpu_emit_valid = false;
                    ts_next_slot = 0;         // a new recording starts a new timeline
                    ts_have_frame = false;
                    ts_end_slot.store(0, std::memory_order_release);
                    gpu_fail_count = 0;       // a new recording gets a fresh GPU-path retry budget
                    if (gpu_fail_transient) { gpu_failed = false; gpu_fail_transient = false; }
                    if (!is_recording_now) frame_ns_idle = compute_frame_ns_idle();
                    frame_ns = is_recording_now ? frame_ns_recording : frame_ns_idle;
                    next_frame_ns = 0; // resync pacing to "now" rather than an old cadence
                    // fps_acc_frames/fps_acc_start_ns (the
                    // "measured actual capture fps" stat shown in the UI)
                    fps_acc_frames = 0;
                    fps_acc_start_ns = 0;
#ifdef _WIN32
                    if (sw_ctx && g_libs.sw_start) { g_libs.sw_start(sw_ctx); last_preview_ns = 0; }  // the clock restarts at 0: a preview stamp from the OLD clock would freeze the preview until the new one caught up
#endif
                }
            }

            {
                const auto t_now = hitch_clock::now();
                if (hitch_last_valid && was_recording) {
                    const double gap_ms = std::chrono::duration<double, std::milli>(t_now - hitch_last_tick).count();
                    const double limit_ms = std::max(250.0, (double)frame_ns / 1e6 * 6.0);
                    if (gap_ms > limit_ms) {
                        ++hitch_count;
                        if (gap_ms > hitch_worst_ms) hitch_worst_ms = gap_ms;
                        if (hitch_logged < 15) {
                            ++hitch_logged;
                            HrLog::Warn("Capture hitch: " + std::to_string((long long)gap_ms) +
                                        " ms between two capture ticks (expected ~" +
                                        std::to_string((long long)(frame_ns / 1000000)) + " ms); the capture call before it took " +
                                        std::to_string((long long)last_cap_ms) + " ms" +
                                        (logged_lost_ ? ", DXGI capture was lost at that time" : "") +
                                        " [that tick: " + std::to_string((long long)last_prep_ms) + " ms before the capture call, " +
                                        std::to_string((long long)last_reset_ms) + " ms DXGI reset, " +
                                        std::to_string((long long)std::max(0.0, gap_ms - last_prep_ms - last_cap_ms - last_reset_ms)) +
                                        " ms after it (convert/queue/pacing)]" +
                                        (hitch_logged == 15 ? " (further hitches are only counted)" : ""));
                        }
                    }
                }
                hitch_last_tick = t_now;
                hitch_last_valid = true;
            }

#ifdef _WIN32
            bool want_boost = boost_priority.load(std::memory_order_relaxed);
            const bool want_high_prio = want_boost && !light_cpu_path.load(std::memory_order_relaxed);
            if (want_boost != was_boosted) {
                was_boosted = want_boost;
                yuv_pool.SetBoost(want_boost);
            }
            if (want_high_prio != thread_is_high_prio) {
                thread_is_high_prio = want_high_prio;
                SetThreadPriority(GetCurrentThread(), want_high_prio
                                      ? THREAD_PRIORITY_HIGHEST
                                      : THREAD_PRIORITY_ABOVE_NORMAL);
            }
#endif

#ifdef _WIN32
            if (sw_ctx && g_libs.sw_sleep_until) {
                for (;;) {
                    int64_t now_ns = g_libs.sw_elapsed_ns ? g_libs.sw_elapsed_ns(sw_ctx) : next_frame_ns;
                    int64_t remaining = next_frame_ns - now_ns;
                    if (remaining <= 15'000'000LL) {
                        g_libs.sw_sleep_until(sw_ctx, next_frame_ns);
                        break;
                    }
                    if (!running.load(std::memory_order_relaxed)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(15));
                }
            } else {
                std::this_thread::sleep_for(std::chrono::nanoseconds(frame_ns));
            }
#else
            std::this_thread::sleep_for(std::chrono::nanoseconds(frame_ns));
#endif
            next_frame_ns += frame_ns;

            if (!running.load(std::memory_order_relaxed)) break;

            // Overlay list: refreshed here (once per overlay-config change, see
            // overlays_gen) rather than after the capture, because whether any
            // overlay is active decides which capture path this tick can use.
            {
                uint64_t gen = overlays_gen.load(std::memory_order_relaxed);
                if (gen != last_overlays_gen) {
                    std::lock_guard<std::mutex> lock(overlays_mtx);
                    overlays_snapshot = overlays;
                    last_overlays_gen = gen;
                    overlays_active = false;
                    overlays_need_base = false;
                    for (const auto &o : overlays_snapshot) {
                        if (HrOverlayDrawsAnything(o)) overlays_active = true;
                        if (HrOverlayNeedsBgraBase(o)) overlays_need_base = true;
                    }
                }
            }

            int c_x, c_y, c_w, c_h;
            {
                std::lock_guard<std::mutex> crop_lk(crop_mtx);
                c_x = crop_x; c_y = crop_y; c_w = crop_w; c_h = crop_h;
            }
            // If a crop was requested but doesn't fit the frame DXGI is actually delivering (e.g.
            if (c_w > 0 && c_h > 0 &&
                (c_x < 0 || c_y < 0 || c_x + c_w > src_w || c_y + c_h > src_h)) {
                if (!warned_bad_crop) {
                    warned_bad_crop = true;
                    HrLog::Warn("Capture crop " + std::to_string(c_w) + "x" + std::to_string(c_h) + " at " +
                                std::to_string(c_x) + "," + std::to_string(c_y) +
                                " doesn't fit the captured frame " + std::to_string(src_w) + "x" +
                                std::to_string(src_h) + " - clamping it (wrong monitor/size resolved?).");
                }
                if (c_x < 0) { c_w += c_x; c_x = 0; }
                if (c_y < 0) { c_h += c_y; c_y = 0; }
                if (c_x + c_w > src_w) c_w = src_w - c_x;
                if (c_y + c_h > src_h) c_h = src_h - c_y;
                c_w &= ~1; c_h &= ~1;
            }
            const bool do_crop = c_w > 0 && c_h > 0 && c_x >= 0 && c_y >= 0 &&
                                 c_x + c_w <= src_w && c_y + c_h <= src_h;

            bool gpu_tick = false;
            int  gpu_enc_w = 0, gpu_enc_h = 0;
            // Source rectangle in the DESKTOP's CURRENT pixel space (see the mode-change
            // note below) and the size of the region it covers - used for the capture
            // call and for placing the mouse pointer.
            int  gpu_sl = 0, gpu_st = 0, gpu_sr = 0, gpu_sb = 0;
#ifdef _WIN32
            // A GPU error that may be transient (display mode switch, driver hiccup) is
            // retried a few times instead of giving up for the whole recording.
            if (gpu_failed && gpu_fail_transient && gpu_fail_count < 3 &&
                std::chrono::steady_clock::now() >= gpu_retry_at) {
                gpu_failed = false;
                gpu_fail_transient = false;
            }
            if (!gpu_failed && !wgc_mode && !overlays_need_base && gpu_convert.load(std::memory_order_relaxed) &&
                recording.load(std::memory_order_acquire) && yuv_pool.IsNv12() &&
                g_libs.dx_get_size) {
                int real_w = 0, real_h = 0;
                g_libs.dx_get_size(dx_ctx, &real_w, &real_h);
                const int src_rw = do_crop ? c_w : src_w;
                const int src_rh = do_crop ? c_h : src_h;
                const int rq_w = out_w.load(std::memory_order_relaxed);
                const int rq_h = out_h.load(std::memory_order_relaxed);
                gpu_enc_w = (rq_w > 0) ? rq_w : src_rw;
                gpu_enc_h = (rq_h > 0) ? rq_h : src_rh;
                // Overlays are blended into the finished NV12 frame (OverlayCompositor::ApplyNv12), which
                // only works when that frame is the same size as the space overlay coordinates live in (no
                // GPU downscale).
                const bool ov_blocks_gpu = overlays_active && (gpu_enc_w != src_rw || gpu_enc_h != src_rh);
                // DISPLAY MODE CHANGED MID-RECORDING (typical: a game switches the desktop to its own
                // resolution right after Start): the buffers and the encoder were sized for src_w x src_h,
                // but DXGI now delivers real_w x real_h.
                const bool size_changed = (real_w != src_w || real_h != src_h);
                if (!ov_blocks_gpu &&
                    real_w > 0 && real_h > 0 && src_w > 0 && src_h > 0 &&
                    src_rw > 0 && src_rh > 0 &&
                    gpu_enc_w >= 2 && gpu_enc_h >= 2 && !(gpu_enc_w & 1) && !(gpu_enc_h & 1)) {
                    if (!size_changed) {
                        gpu_sl = do_crop ? c_x : 0;           gpu_st = do_crop ? c_y : 0;
                        gpu_sr = do_crop ? c_x + c_w : src_w; gpu_sb = do_crop ? c_y + c_h : src_h;
                    } else {
                        auto mx = [&](int v) { return (int)(((long long)v * real_w) / src_w); };
                        auto my = [&](int v) { return (int)(((long long)v * real_h) / src_h); };
                        gpu_sl = do_crop ? mx(c_x) : 0;           gpu_st = do_crop ? my(c_y) : 0;
                        gpu_sr = do_crop ? mx(c_x + c_w) : real_w; gpu_sb = do_crop ? my(c_y + c_h) : real_h;
                        if (gpu_sr > real_w) gpu_sr = real_w;
                        if (gpu_sb > real_h) gpu_sb = real_h;
                        if (gpu_sr - gpu_sl < 2 || gpu_sb - gpu_st < 2) {
                            gpu_sl = 0; gpu_st = 0; gpu_sr = real_w; gpu_sb = real_h;
                        }
                        if (!logged_gpu_scale_) {
                            logged_gpu_scale_ = true;
                            HrLog::Info("GPU path: desktop is now " + std::to_string(real_w) + "x" + std::to_string(real_h) +
                                        " (recording was started at " + std::to_string(src_w) + "x" + std::to_string(src_h) +
                                        ") - the video processor scales it to " + std::to_string(gpu_enc_w) + "x" +
                                        std::to_string(gpu_enc_h) + " on the GPU (no CPU fallback).");
                        }
                    }
                    if (!size_changed) logged_gpu_scale_ = false;
                    if (!hr_dx_gpu_enable(dx_ctx, gpu_enc_w, gpu_enc_h, bgra_buf.data())) {
                        gpu_failed = true;
                        HrLog::Warn("GPU colour conversion unavailable on this adapter (D3D11 video "
                                    "processor cannot do BGRA->NV12) -- using the CPU converter.");
                    } else if (hr_dx_gpu_ready(dx_ctx)) {
                        const size_t nv_need = (size_t)gpu_enc_w * gpu_enc_h * 3 / 2;
                        if (nv12_buf.size() != nv_need) { nv12_buf.assign(nv_need, 0); nv12_valid = false; nv12_clean = false; }
                        gpu_tick = true;
                    }
                }
            }
#endif

            // Capture
#ifdef _WIN32
            // Tell the priority logic (this thread, next iteration, and the
            // writer thread) whether this tick is on the cheap GPU path.
            if (light_cpu_path.load(std::memory_order_relaxed) != gpu_tick)
                light_cpu_path.store(gpu_tick, std::memory_order_relaxed);
            if (!g_libs.dx_capture) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            int ret;
            const auto t_cap0 = hitch_clock::now();
            last_prep_ms  = std::chrono::duration<double, std::milli>(t_cap0 - hitch_last_tick).count();
            last_reset_ms = 0.0;
            if (gpu_tick) {
                ret = hr_dx_capture_nv12(dx_ctx, nv12_buf.data(), timeout_ms, gpu_sl, gpu_st, gpu_sr, gpu_sb);
                if (ret == HR_DX_ERROR) {
                    // Not fatal for the pipeline: stop using the GPU path and let
                    // the classic one (which resyncs from the GPU-side desktop copy)
                    ++gpu_fail_count;
                    gpu_failed = true;
                    gpu_fail_transient = true;
                    gpu_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                    HrLog::Warn(std::string("GPU colour conversion failed at runtime -- falling back to the CPU converter") +
                                (gpu_fail_count < 3 ? " (will retry the GPU path in 3 s)." : " for the rest of this recording."));
                    continue;
                }
            } else {
                ret = g_libs.dx_capture(dx_ctx, bgra_buf.data(), timeout_ms);
            }
#else
            int ret = HR_DX_ERROR;
#endif
#ifdef _WIN32
            last_cap_ms = std::chrono::duration<double, std::milli>(hitch_clock::now() - t_cap0).count();
#endif
            if (ret == HR_DX_TIMEOUT) {
                // A timeout here does NOT mean capture failed -- DXGI's AcquireNextFrame() times out by
                // design whenever the desktop simply hasn't changed since the last frame (e.g.
                frames_dropped.fetch_add(1, std::memory_order_relaxed);
                logged_lost_ = false;
                // Deliberately NOT `continue`-ing here (see comment above) -- fall through to the shared
                // capture-success path below, which will re-convert/re-queue whatever's still in bgra_buf
                // from the last real frame.
            } else if (ret == HR_DX_LOST) {
                if (!logged_lost_) {
                    HrLog::Warn(wgc_mode ? "Window capture lost (GPU reset) -- resetting"
                                         : "DXGI capture lost (display mode change, UAC prompt, or GPU reset) -- resetting");
                    logged_lost_ = true;
                }
#ifdef _WIN32
                auto now = std::chrono::steady_clock::now();
                if (g_libs.dx_reset && now >= next_reset_attempt_) {
                    // CRASHFIX: this return value used to be thrown away ("whether or not that succeeded" -
                    // see below), so a reset that failed to re-acquire the duplication interface (e.g.
                    const auto t_reset0 = hitch_clock::now();
                    int reset_ok = g_libs.dx_reset(dx_ctx);
                    const double reset_ms = std::chrono::duration<double, std::milli>(hitch_clock::now() - t_reset0).count();
                    last_reset_ms = reset_ms;
                    if (reset_ms >= 100.0)
                        HrLog::Warn("DXGI reset took " + std::to_string((long long)reset_ms) + " ms (" +
                                    (reset_ok ? "ok" : "failed") + ") - the capture thread was busy that long.");
                    if (!reset_ok) {
                        if (!logged_reset_fail_) {
                            HrLog::Warn("DXGI reset failed (capture surface still unavailable - "
                                        "common right after the mode change that caused the loss, "
                                        "e.g. a game briefly holding exclusive fullscreen) -- will "
                                        "keep retrying once a second.");
                            logged_reset_fail_ = true;
                        }
                    } else {
                        logged_reset_fail_ = false;
                    }
                    // Don't try again for a full second regardless of the outcome above - if it failed, back-
                    // to-back retries just burn GPU/driver time neither of us can spare.
                    next_reset_attempt_ = now + std::chrono::seconds(1);
                }
#endif
                frames_dropped.fetch_add(1, std::memory_order_relaxed);
            } else if (ret != HR_DX_OK) {
                HrLog::Error("Capture pipeline stopped: dx_capture() returned a fatal error (ret=" + std::to_string(ret) + ")");
                running.store(false);
                break;
            } else {
                logged_lost_ = false;
            }

            // ====== WINDOW CROP (optional) ======
            // See the crop_x/crop_w/eff_w comments on the Pipeline struct.
            int extra_slots = 0;
            if (gpu_tick) {
                // ====== GPU PATH: the frame is already a finished NV12 picture ======
                eff_w = do_crop ? c_w : src_w;
                eff_h = do_crop ? c_h : src_h;
                if (ret == HR_DX_OK) { nv12_valid = true; nv12_clean = true; }
                if (!nv12_valid) continue;           // nothing to send yet
                frame_ptr  = nullptr;
                frame_nv12 = nv12_buf.data();        // live preview reads this
                nv12_w = gpu_enc_w; nv12_h = gpu_enc_h;
                last_yuv_valid = false;              // classic path's cached picture is now stale
                cpu_emit_valid = false;
#ifdef _WIN32
                if (sw_ctx && g_libs.sw_elapsed_ns) {
                    static constexpr int kMaxCatchupFrames = 90;
                    int64_t now_ns = g_libs.sw_elapsed_ns(sw_ctx);
                    int64_t behind = now_ns - next_frame_ns;
                    if (behind > frame_ns) {
                        extra_slots = (int)std::min<int64_t>(behind / frame_ns, kMaxCatchupFrames);
                        next_frame_ns += (int64_t)extra_slots * frame_ns;
                    }
                }
#endif
                const bool cur_on = include_cursor.load(std::memory_order_relaxed);
                const CursorSig cur_now = cur_on ? read_cursor_sig() : CursorSig{};
                const size_t needed = (size_t)gpu_enc_w * gpu_enc_h * 3 / 2;
                // Nothing changed on screen AND the pointer looks the same as in the
                // picture already queued -> just hold that picture one slot longer.
                const bool tsm = ts_on();
                bool merged = false;
                if (ret != HR_DX_OK && gpu_emit_valid && !overlays_active &&
                    (!cur_on || cur_now == gpu_emit_sig))
                    merged = tsm ? ts_have_frame : try_merge_last(extra_slots);
                else if (ret != HR_DX_OK && !nv12_clean && tsm && ts_have_frame)
                    merged = true;    // the clean picture went to the writer (see hand_over): keep showing it
                const int64_t pts = tsm ? ts_take_slot(extra_slots) : -1;
                if (!merged) {
                    std::vector<uint8_t> yuv_frame;
                    // Zero-copy hand-over: with nothing to draw on it (no pointer, no overlays) the readback
                    // buffer IS the outgoing frame - no 3 MB nv12_buf -> yuv_frame copy. Only with the
                    // timestamped pipe: the raw pipe may need to re-send the clean picture on a later static
                    // tick, and a handed-over buffer is gone.
                    const bool hand_over = tsm && ret == HR_DX_OK && !overlays_active && !cur_on;
                    if (hand_over) {
                        preview_if_due();                 // the thumbnail reads nv12_buf, which leaves now
                        preview_suppressed = true;
                        yuv_frame = std::move(nv12_buf);
                        nv12_buf = take_free_buf(needed); // next readback lands in a recycled buffer
                        nv12_clean = false;
                        frame_nv12 = nullptr;
                    } else {
                        yuv_frame = take_free_buf(needed);
                        std::memcpy(yuv_frame.data(), nv12_buf.data(), needed);
                    }
#ifdef _WIN32
                    if (overlays_active) {
                        // Same try/catch rationale as the classic path's Apply()
                        // call: this is a bare std::thread, an escaping throw
                        // would terminate the process mid-recording.
                        try {
                            overlay_compositor.ApplyNv12(yuv_frame.data(), gpu_enc_w, gpu_enc_h,
                                                         overlays_snapshot,
                                                         fps_actual.load(std::memory_order_relaxed));
                        } catch (const std::exception &e) {
                            HrLog::Error(std::string("Overlay compositing threw (") + e.what() +
                                         ") -- this frame's overlays were skipped, recording continues.");
                        } catch (...) {
                            HrLog::Error("Overlay compositing threw an unknown exception -- this frame's "
                                         "overlays were skipped, recording continues.");
                        }
                        // The live preview reads nv12_buf (the clean GPU frame).
                        // With overlays on, hand it a copy that has them baked in
                        // (before the pointer, like the clean frame it replaces)
                        if (preview_needed.load(std::memory_order_relaxed)) {
                            if (pv_nv12.size() != needed) pv_nv12.resize(needed);
                            std::memcpy(pv_nv12.data(), yuv_frame.data(), needed);
                            frame_nv12 = pv_nv12.data();
                        }
                    }
                    if (cur_on) {
                        // gpu_s* is the captured rectangle in the desktop's CURRENT pixel space
                        // (equals the old c_x / eff_w values unless the display mode changed).
                        hr_composite_cursor_nv12(yuv_frame.data(), gpu_enc_w, gpu_enc_h,
                                                  cap_origin_x + gpu_sl,
                                                  cap_origin_y + gpu_st,
                                                  (float)gpu_enc_w / (float)std::max(1, gpu_sr - gpu_sl),
                                                  (float)gpu_enc_h / (float)std::max(1, gpu_sb - gpu_st));
                    }
#endif
                    enqueue_frame(yuv_frame, extra_slots, gpu_enc_w, gpu_enc_h, true, pts);
                    if (pts >= 0) ts_have_frame = true;
                }
                gpu_emit_valid = true;
                gpu_emit_sig   = cur_now;
                ++st_gpu_ticks;
            } else {
            gpu_emit_valid = false;
            frame_nv12 = nullptr;
            const bool cursor_on_now = include_cursor.load(std::memory_order_relaxed);
            // Sampled BEFORE the pointer is drawn (see CursorSig): if it moves in
            // between, the recorded signature is the older one and the next tick
            // simply re-converts - never the other way round.
            const CursorSig cur_sig_now = cursor_on_now ? read_cursor_sig() : CursorSig{};
            uint8_t* frame = bgra_buf.data();   // what overlays/cursor/encode read this tick
            if (do_crop) {
                eff_w = c_w;
                eff_h = c_h;
                const size_t need = (size_t)c_w * c_h * 4;
                if (work_buf.size() != need) work_buf.resize(need);
                const uint8_t* full = bgra_buf.data();
                uint8_t* wb = work_buf.data();
                for (int y = 0; y < c_h; ++y) {
                    std::memcpy(wb + (size_t)y * c_w * 4,
                                full + ((size_t)(c_y + y) * src_w + c_x) * 4,
                                (size_t)c_w * 4);
                }
                frame = wb;
            } else {
                eff_w = src_w;
                eff_h = src_h;
            }

            // Copy-on-write for the non-cropped case: overlays/cursor draw in place, so if `frame`
            // still points at the pristine bgra_buf, duplicate it into work_buf first.
            auto ensure_work_copy = [&](uint8_t*& f) {
                if (f != bgra_buf.data()) return;
                const size_t need = (size_t)eff_w * eff_h * 4;
                if (work_buf.size() != need) work_buf.resize(need);
                std::memcpy(work_buf.data(), bgra_buf.data(), need);
                f = work_buf.data();
            };

            {
                // (overlays_snapshot itself is refreshed before the capture
                // call above - the GPU path needs to know whether anything
                // has to be composited on the CPU BEFORE choosing a path.)
                if (!overlays_snapshot.empty()) {
                    if (overlays_active) ensure_work_copy(frame);   // hidden-only: nothing is drawn, no 8MB copy
                    try {
                        // The pristine capture buffer is the source of region / window layers (it is
                        // untouched while `frame` is the work copy of it and nothing was cropped).
                        const uint8_t *pristine = (!do_crop && frame != bgra_buf.data()) ? bgra_buf.data() : nullptr;
                        overlay_compositor.Apply(frame, eff_w, eff_h, eff_w * 4, overlays_snapshot,
                                                  fps_actual.load(std::memory_order_relaxed), pristine);
                    } catch (const std::exception &e) {
                        HrLog::Error(std::string("Overlay compositing threw (") + e.what() +
                                     ") -- this frame's overlays were skipped, recording continues.");
                    } catch (...) {
                        HrLog::Error("Overlay compositing threw an unknown exception -- this frame's "
                                     "overlays were skipped, recording continues.");
                    }
                }
            }

            // ====== CURSOR ("Cursor" setting) ====== The pointer is NOT drawn into this BGRA frame any
            // more: that forced an 8 MB copy of the whole capture buffer every tick and turned every
            // conversion into a full-frame one. It is blended into the converted YUV frame below (after
            // overlays, so it stays on top as before) and onto the preview thumbnail (update_preview()).
            // The origin uses the clamped crop values the frame was actually cropped with (c_x/c_y), not
            // the raw crop_x/crop_y members the UI thread may be rewriting.
            pv_cursor    = cursor_on_now;
            pv_cursor_ox = cap_origin_x + (do_crop ? c_x : 0);
            pv_cursor_oy = cap_origin_y + (do_crop ? c_y : 0);
            frame_ptr = frame;
#ifdef _WIN32
            if (recording.load(std::memory_order_acquire) && sw_ctx && g_libs.sw_elapsed_ns) {
                static constexpr int kMaxCatchupFrames = 90; // ~1.5-3s of duplicate frames, whichever fps
                int64_t now_ns = g_libs.sw_elapsed_ns(sw_ctx);
                int64_t behind = now_ns - next_frame_ns;
                if (behind > frame_ns) {
                    extra_slots = (int)std::min<int64_t>(behind / frame_ns, kMaxCatchupFrames);
                    next_frame_ns += (int64_t)extra_slots * frame_ns;
                }
            }
#endif

#ifdef _WIN32
            if (recording.load(std::memory_order_acquire) && g_libs.bgra_to_yuv) {
                int req_w = out_w.load(std::memory_order_relaxed);
                int req_h = out_h.load(std::memory_order_relaxed);
                const uint8_t* enc_src = frame;
                int enc_w = eff_w, enc_h = eff_h;
                const bool scaling = req_w > 0 && req_h > 0 && (req_w != eff_w || req_h != eff_h);
                if (scaling) { enc_w = req_w; enc_h = req_h; }
                const bool overlays_on = overlays_active;
                // `frame` still IS bgra_buf: no crop, overlay or pointer was
                // baked in, and (below) no scaling -> the converted picture is a
                // pure function of bgra_buf, so DXGI's changed-rows hint applies. (The pointer is no
                // longer baked in, so this now holds with the cursor on as well.)
                const bool direct = (frame == bgra_buf.data()) && !scaling;

                // PERF: static desktop (DXGI timeout) -> last_yuv (pointer-free) is still the right base.
                const bool same_base = (ret == HR_DX_TIMEOUT) && last_yuv_valid && !overlays_on &&
                                       last_yuv_w == enc_w && last_yuv_h == enc_h;
                // ...and the frame we queued last already shows the pointer exactly as it is now.
                const bool same_emit = cpu_emit_valid && cpu_emit_w == enc_w && cpu_emit_h == enc_h &&
                                       (!cursor_on_now || cur_sig_now == cpu_emit_sig);
                const bool tsm = ts_on();
                bool merged_into_queue = false;
                if (same_base && same_emit) {
                    // Nothing to send. Timestamped pipe: just let the timeline run (ffmpeg repeats the last
                    // picture). Raw pipe: make the frame still waiting in the queue last one slot longer.
                    merged_into_queue = tsm ? ts_have_frame : try_merge_last(extra_slots);
                }
                const int64_t pts = tsm ? ts_take_slot(extra_slots) : -1;

                std::vector<uint8_t> yuv_frame;
                if (!merged_into_queue) {
                    const size_t needed = (size_t)enc_w * enc_h * 3 / 2;
                    yuv_frame = take_free_buf(needed);

                    if (same_base) {
                        std::memcpy(yuv_frame.data(), last_yuv.data(), needed);   // 3MB copy vs full convert
                    } else {
                        // PERF: DXGI reported exactly which rows changed since the
                        // previous delivery, and last_yuv is the conversion of that
                        // previous picture -> keep it and re-convert only those rows.
                        int dy0 = 0, dy1 = 0;
                        const bool partial = !wgc_mode && direct && ret == HR_DX_OK && !overlays_on &&
                                             last_yuv_valid && last_yuv_direct &&
                                             last_yuv_w == enc_w && last_yuv_h == enc_h &&
                                             hr_dx_get_dirty_rows(dx_ctx, &dy0, &dy1);
                        if (partial) {
                            dy0 = std::max(0, dy0) & ~1;
                            dy1 = std::min(enc_h, (dy1 + 1) & ~1);
                            std::memcpy(yuv_frame.data(), last_yuv.data(), needed);
                            yuv_pool.ConvertRows(enc_src, yuv_frame.data(), enc_w, enc_h, dy0, dy1);
                            ++st_partial_conv;
                            // Keep the cached picture current with just the touched rows.
                            copy_yuv_rows(last_yuv.data(), yuv_frame.data(), enc_w, enc_h, dy0, dy1,
                                          yuv_pool.IsNv12());
                        } else {
                            if (scaling) {
                                const size_t scaled_needed = (size_t)req_w * req_h * 4;
                                if (scaled_buf.size() != scaled_needed) scaled_buf.resize(scaled_needed);
                                // PERF: was bgra_downscale(...) - a single-threaded
                                // pass on this (already highest/boosted-priority)
                                yuv_pool.Downscale(frame, scaled_buf.data(), eff_w, eff_h, req_w, req_h);
                                enc_src = scaled_buf.data();
                            }
                            yuv_pool.Convert(enc_src, yuv_frame.data(), enc_w, enc_h);
                            // Keep a private (pointer-free) copy so a following static tick can reuse
                            // it - for ANY conversion whose result depends only on bgra_buf (i.e. no
                            // overlays: those animate).
                            if (!overlays_on) {
                                last_yuv.assign(yuv_frame.begin(), yuv_frame.end());
                                last_yuv_valid = true;
                                last_yuv_w = enc_w; last_yuv_h = enc_h;
                                last_yuv_direct = direct;
                            } else {
                                last_yuv_valid = false;
                            }
                        }
                    }
                    // Pointer: blended into the outgoing copy only, so last_yuv stays clean. Scale maps the
                    // capture area's pixels onto the encoded size (Settings > Resolution).
                    if (cursor_on_now) {
                        hr_composite_cursor_yuv(yuv_frame.data(), enc_w, enc_h,
                                                 cap_origin_x + (do_crop ? c_x : 0),
                                                 cap_origin_y + (do_crop ? c_y : 0),
                                                 (float)enc_w / (float)std::max(1, eff_w),
                                                 (float)enc_h / (float)std::max(1, eff_h),
                                                 yuv_pool.IsNv12() ? 1 : 0);
                    }
                    enqueue_frame(yuv_frame, extra_slots, enc_w, enc_h, yuv_pool.IsNv12(), pts);
                    if (pts >= 0) ts_have_frame = true;
                }
                // What the newest queued picture shows (overlays animate, so they never match).
                cpu_emit_valid = !overlays_on;
                cpu_emit_sig = cur_sig_now;
                cpu_emit_w = enc_w; cpu_emit_h = enc_h;
            }
#endif
            }   // else (classic BGRA path)
            frames_captured.fetch_add(1 + extra_slots, std::memory_order_relaxed);
            if (extra_slots > 0) {
                frames_dropped.fetch_add(extra_slots, std::memory_order_relaxed);
                frames_stalled.fetch_add(extra_slots, std::memory_order_relaxed);
            }

            // Preview: dynamic frequency, driven by the Preview FPS setting
            // (see last_preview_ns's declaration above for why time-based).
            preview_if_due();

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
        } catch (const std::exception &e) {
            HrLog::Error(std::string("Capture thread: uncaught exception (") + e.what() +
                         ") -- this pipeline is stopping instead of crashing the app.");
            running.store(false, std::memory_order_relaxed);
        } catch (...) {
            HrLog::Error("Capture thread: uncaught unknown exception -- this pipeline is "
                         "stopping instead of crashing the app.");
            running.store(false, std::memory_order_relaxed);
        }

#ifdef _WIN32
        // One line of capture-efficiency numbers per pipeline lifetime, so it is
        // visible in the log how much work the "skip unchanged"/dirty-rect/GPU
        // paths actually saved (only when something was captured at all).
        if (dx_ctx && !wgc_mode) {
            unsigned long long acq = 0, skp = 0, prt = 0, ful = 0, gpf = 0;
            hr_dx_get_stats(dx_ctx, &acq, &skp, &prt, &ful, &gpf);
            if (acq > 0 || gpf > 0) {
                HrLog::Info("Capture stats: DXGI frames " + std::to_string(acq) +
                            ", pointer-only (skipped, no copy) " + std::to_string(skp) +
                            ", partial readbacks " + std::to_string(prt) +
                            ", full readbacks " + std::to_string(ful) +
                            ", GPU NV12 frames " + std::to_string(gpf) +
                            ", partial YUV conversions " + std::to_string((unsigned long long)st_partial_conv) +
                            ", GPU-path ticks " + std::to_string((unsigned long long)st_gpu_ticks));
            }
        }
#endif
        if (hitch_count > 0)
            HrLog::Info("Capture hitches before this pipeline stopped: " + std::to_string(hitch_count) +
                        " (worst " + std::to_string((long long)hitch_worst_ms) + " ms between two capture ticks)");
        // Signal writer thread to stop
        writer_running.store(false, std::memory_order_relaxed);
        pipe_queue_cv.notify_all();
#ifdef _WIN32
        if (com_inited) CoUninitialize();
#endif
        // Portable completion signal - see capture_thread_done's declaration
        // (mirrors writer_thread_done above).
        {
            std::lock_guard<std::mutex> lk(finish_mtx);
            capture_thread_done.store(true, std::memory_order_release);
        }
        finish_cv.notify_all();

        finish_thread();
    }
};

// ============================================================================
// Exported API
// ============================================================================

HR_EXPORT void* hr_pl_create(int w, int h, int fps,
                               intptr_t pipe_fd, int pv_w, int pv_h, int output_idx) {
#ifndef _WIN32
    (void)w; (void)h; (void)fps; (void)pipe_fd; (void)pv_w; (void)pv_h; (void)output_idx;
    return nullptr;
#else
    if (!ensure_libs()) return nullptr;
    if (output_idx < 0) output_idx = 0; // defensive - callers pass a resolved 0-based index

    auto* pl = new Pipeline();
    pl->src_w       = w;
    pl->src_h       = h;
    pl->eff_w       = w;   // no crop yet -- see hr_pl_set_capture_rect()
    pl->eff_h       = h;
    pl->fps         = fps;
    pl->pv_w        = pv_w;
    pl->pv_h        = pv_h;
    pl->pipe_handle.store(pipe_fd, std::memory_order_release);
    pl->recording.store(pipe_fd != 0 && pipe_fd != -1, std::memory_order_release);

    pl->dx_ctx = g_libs.dx_create(0, output_idx);
    if (!pl->dx_ctx) {
        // This used to just say "returned null" with no way to tell which of the "common causes"
        // it actually was - now logs the real HRESULT (hex, so it's directly greppable against the
        // DXGI/WinError docs) alongside the guess.
        char hres_buf[32];
        std::snprintf(hres_buf, sizeof(hres_buf), "0x%08lX", g_libs.dx_last_error ? g_libs.dx_last_error() : 0ul);
        HrLog::Error(std::string("Pipeline create failed: dx_create() returned null, HRESULT ") + hres_buf +
                     " (DXGI desktop duplication init failed -- common causes: running over RDP/a virtual "
                     "display, a just-changed display mode, or insufficient permissions)");
        delete pl; return nullptr;
    }
    // Matches dx_create(0, output_idx) above - needed so
    // hr_composite_cursor() can translate GetCursorInfo()'s virtual-
    // desktop coordinates into this capture buffer's local coordinates.
    {
        int ox = 0, oy = 0, ow = 0, oh = 0;
        if (hr_dx_output_desc(0, output_idx, &ox, &oy, &ow, &oh, nullptr, 0)) {
            pl->cap_origin_x = ox;
            pl->cap_origin_y = oy;
        }
    }

    pl->sw_ctx = g_libs.sw_create();
    if (!pl->sw_ctx) {
        HrLog::Error("Pipeline create failed: sw_create() (frame pacing/stopwatch) returned null");
        g_libs.dx_destroy(pl->dx_ctx); delete pl; return nullptr;
    }

    return pl;
#endif
}

// hr_pl_create_window  (2.4)
HR_EXPORT void* hr_pl_create_window(int w, int h, int fps, intptr_t pipe_fd, int pv_w, int pv_h,
                                    void* hwnd, int client_only, int draw_cursor,
                                    int smart, int fps_cap) {
#ifndef _WIN32
    (void)w; (void)h; (void)fps; (void)pipe_fd; (void)pv_w; (void)pv_h; (void)hwnd; (void)client_only; (void)draw_cursor;
    (void)smart; (void)fps_cap;
    return nullptr;
#else
    if (!ensure_libs() || !hwnd) return nullptr;
    HrWgcOptions wopt;
    wopt.smart   = smart != 0;
    wopt.fps_cap = fps_cap < 0 ? 0 : fps_cap;
    void* src = HrWgcCreate(static_cast<HWND>(hwnd), w, h, client_only != 0, draw_cursor != 0, &wopt);
    if (!src) return nullptr;

    auto* pl = new Pipeline();
    pl->src_w       = w & ~1;
    pl->src_h       = h & ~1;
    pl->eff_w       = pl->src_w;
    pl->eff_h       = pl->src_h;
    pl->fps         = fps;
    pl->pv_w        = pv_w;
    pl->pv_h        = pv_h;
    pl->pipe_handle.store(pipe_fd, std::memory_order_release);
    pl->recording.store(pipe_fd != 0 && pipe_fd != -1, std::memory_order_release);
    pl->dx_ctx   = src;
    pl->wgc_mode = true;
    pl->cap_origin_x = pl->cap_origin_y = 0;

    pl->sw_ctx = g_libs.sw_create();
    if (!pl->sw_ctx) {
        HrLog::Error("Pipeline create failed: sw_create() (frame pacing/stopwatch) returned null");
        HrWgcDestroy(src);
        delete pl;
        return nullptr;
    }
    return pl;
#endif
}

// hr_pl_window_retarget  (7th pass)
HR_EXPORT int hr_pl_window_retarget(void* handle, void* hwnd) {
    if (!handle || !hwnd) return 0;
#ifdef _WIN32
    auto* pl = static_cast<Pipeline*>(handle);
    if (!pl->wgc_mode || !pl->dx_ctx) return 0;
    return HrWgcSetTarget(pl->dx_ctx, static_cast<HWND>(hwnd)) ? 1 : 0;
#else
    return 0;
#endif
}

/*  hr_pl_window_mode  (7th pass)
    What the window source is doing: 0 = not a window pipeline, 1 = native Windows.Graphics.Capture,
    2 = exact window capture (PrintWindow), 3 = smart mode reading the (cheap) screen duplication. */
HR_EXPORT int hr_pl_window_mode(void* handle) {
    if (!handle) return 0;
#ifdef _WIN32
    auto* pl = static_cast<Pipeline*>(handle);
    if (!pl->wgc_mode || !pl->dx_ctx) return 0;
    return HrWgcMode(pl->dx_ctx);
#else
    return 0;
#endif
}

HR_EXPORT void hr_pl_destroy(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    auto* pl = static_cast<Pipeline*>(handle);

    // Stop all threads
    pl->writer_running.store(false, std::memory_order_relaxed);
    pl->pipe_queue_cv.notify_all();
    pl->running.store(false, std::memory_order_relaxed);

    // Both worker threads capture the raw `pl` pointer, so if either one is still running
    // after we give up waiting, force-detaching it and then deleting `pl` below would leave
    // that thread touching freed memory the next time it wakes up.
    bool leak_pl = false;

    if (pl->writer_thread.joinable()) {
        bool finished;
        {
            std::unique_lock<std::mutex> lk(pl->finish_mtx);
            finished = pl->finish_cv.wait_for(lk, std::chrono::milliseconds(1000),
                [pl] { return pl->writer_thread_done.load(std::memory_order_acquire); });
        }
        if (finished) {
            pl->writer_thread.join();
        } else {
            // Force detach if stuck.
            HrLog::Error("Pipeline destroy: writer thread did not stop in time - handing off cleanup to it instead of leaking Pipeline forever");
            bool was_off = false;
            if (pl->handed_off.compare_exchange_strong(was_off, true, std::memory_order_release))
                g_handed_off_pipelines.fetch_add(1, std::memory_order_acq_rel);
            pl->writer_thread.detach();
            leak_pl = true;
        }
    }

    // Wait for capture thread with timeout - see the writer-thread branch
    // above for why this uses finish_cv instead of native_handle().
    if (pl->capture_thread.joinable()) {
        bool finished;
        {
            std::unique_lock<std::mutex> lk(pl->finish_mtx);
            finished = pl->finish_cv.wait_for(lk, std::chrono::milliseconds(1000),
                [pl] { return pl->capture_thread_done.load(std::memory_order_acquire); });
        }
        if (finished) {
            pl->capture_thread.join();
        } else {
            // Force detach if stuck - see the writer-thread branch above
            // for why handed_off is set right here rather than afterward.
            HrLog::Error("Pipeline destroy: capture thread did not stop in time - handing off cleanup to it instead of leaking Pipeline forever");
            bool was_off = false;
            if (pl->handed_off.compare_exchange_strong(was_off, true, std::memory_order_release))
                g_handed_off_pipelines.fetch_add(1, std::memory_order_acq_rel);
            pl->capture_thread.detach();
            leak_pl = true;
        }
    }

    if (pl->handed_off.load(std::memory_order_acquire)) leak_pl = true;

    if (leak_pl) return;

    // Clear remaining queue to free memory
    {
        std::lock_guard<std::mutex> lock(pl->pipe_queue_mtx);
        while (!pl->pipe_queue.empty()) {
            pl->pipe_queue.pop();
        }
    }
    {
        std::lock_guard<std::mutex> lock(pl->free_bufs_mtx);
        while (!pl->free_bufs.empty()) {
            pl->free_bufs.pop();
        }
    }

    // Cleanup resources
    // The writer thread closes the pipe on its way out. If it never ran (pipeline created but never
    // started) the handle is still open here - close it so ffmpeg's stdin write end isn't leaked.
    {
        const intptr_t ph = pl->pipe_handle.exchange(0, std::memory_order_acq_rel);
        if (ph != 0 && ph != -1) CloseHandle(reinterpret_cast<HANDLE>(ph));
    }
    if (pl->dx_ctx && g_libs.dx_destroy) g_libs.dx_destroy(pl->dx_ctx);
    if (pl->sw_ctx && g_libs.sw_destroy) g_libs.sw_destroy(pl->sw_ctx);
    if (pl->write_evt) { CloseHandle(pl->write_evt); pl->write_evt = nullptr; }
    delete pl;
#endif
}

HR_EXPORT int hr_pl_wait_all_detached(int timeout_ms) {
#ifdef _WIN32
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (g_handed_off_pipelines.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() >= deadline) return 0;
        Sleep(20);
    }
    return 1;
#else
    (void)timeout_ms;
    return 1;
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
    constexpr int kMaxReasonableDim = 16384;
    if (real_w > 0 && real_h > 0 &&
        real_w <= kMaxReasonableDim && real_h <= kMaxReasonableDim) {
        pl->src_w = real_w; pl->src_h = real_h;
    } else if (real_w > kMaxReasonableDim || real_h > kMaxReasonableDim) {
        HrLog::Warn("Pipeline start: dx_get_size() returned an unreasonable size (" +
                    std::to_string(real_w) + "x" + std::to_string(real_h) +
                    ") -- ignoring it and keeping the previous " +
                    std::to_string(pl->src_w) + "x" + std::to_string(pl->src_h));
    }

    pl->bgra_buf.resize((size_t)pl->src_w * pl->src_h * 4);
    // YUV conversion buffers are now lazily sized from the free-list pool
    // the first time a frame is converted (see capture_loop()).
    if (g_libs.dx_set_output_size) g_libs.dx_set_output_size(pl->dx_ctx, pl->src_w, pl->src_h);

    // Pre-allocate preview buffer
    int tw = pl->pv_w, th = pl->pv_h;
    if (pl->src_w > 0 && pl->src_h > 0) {
        float ar = (float)pl->src_w / (float)pl->src_h;
        if (tw > (int)(th * ar)) tw = (int)(th * ar);
        else                      th = (int)(tw / ar);
    }
    pl->pv_buf.resize((size_t)(std::max(tw & ~1, 2)) * std::max(th & ~1, 2) * 3);

    // Start writer thread first
    pl->writer_running.store(true, std::memory_order_relaxed);
    pl->writer_thread = std::thread([pl]() { pl->writer_loop(); });

    // Start capture thread
    pl->running.store(true, std::memory_order_relaxed);
    pl->capture_thread = std::thread([pl]() { pl->capture_loop(); });

    return 1;
#endif
}

HR_EXPORT void hr_pl_stop(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    auto* pl = static_cast<Pipeline*>(handle);

    // Signal stop
    pl->running.store(false, std::memory_order_relaxed);
    pl->writer_running.store(false, std::memory_order_relaxed);
    pl->pipe_queue_cv.notify_all();

    if (pl->capture_thread.joinable()) {
        bool finished;
        {
            std::unique_lock<std::mutex> lk(pl->finish_mtx);
            finished = pl->finish_cv.wait_for(lk, std::chrono::milliseconds(1000),
                [pl] { return pl->capture_thread_done.load(std::memory_order_acquire); });
        }
        if (finished) {
            pl->capture_thread.join();
        } else {
            HrLog::Error("Pipeline stop: capture thread did not stop in time - handing off "
                         "cleanup to it instead of leaking Pipeline forever");
            bool was_off = false;
            if (pl->handed_off.compare_exchange_strong(was_off, true, std::memory_order_release))
                g_handed_off_pipelines.fetch_add(1, std::memory_order_acq_rel);
            pl->capture_thread.detach();
        }
    }

    // Wait for writer thread with timeout - same reasoning as above.
    if (pl->writer_thread.joinable()) {
        bool finished;
        {
            std::unique_lock<std::mutex> lk(pl->finish_mtx);
            finished = pl->finish_cv.wait_for(lk, std::chrono::milliseconds(1000),
                [pl] { return pl->writer_thread_done.load(std::memory_order_acquire); });
        }
        if (finished) {
            pl->writer_thread.join();
        } else {
            HrLog::Error("Pipeline stop: writer thread did not stop in time - handing off "
                         "cleanup to it instead of leaking Pipeline forever");
            bool was_off = false;
            if (pl->handed_off.compare_exchange_strong(was_off, true, std::memory_order_release))
                g_handed_off_pipelines.fetch_add(1, std::memory_order_acq_rel);
            pl->writer_thread.detach();
        }
    }

    if (pl->handed_off.load(std::memory_order_acquire)) return;

    // Clear queue
    {
        std::lock_guard<std::mutex> lock(pl->pipe_queue_mtx);
        while (!pl->pipe_queue.empty()) {
            pl->pipe_queue.pop();
        }
    }
#endif
}

HR_EXPORT void hr_pl_pause(void* handle, int flag) {
    if (!handle) return;
#ifdef _WIN32
    static_cast<Pipeline*>(handle)->paused.store(flag != 0, std::memory_order_relaxed);
#endif
}

// "Cursor" setting - see hr_composite_cursor()'s comment for what this
// actually does. Safe to flip at any time (preview or recording); takes
// effect on the next captured frame.
HR_EXPORT void hr_pl_set_include_cursor(void* handle, int flag) {
    if (!handle) return;
#ifdef _WIN32
    auto *pl = static_cast<Pipeline*>(handle);
    if (pl->wgc_mode) {
        // Windows.Graphics.Capture draws the cursor into the window's own picture (and only
        // when it is over that window); the pipeline's desktop-coordinate compositor stays off.
        HrWgcSetCursor(pl->dx_ctx, flag != 0);
        return;
    }
    pl->include_cursor.store(flag != 0, std::memory_order_relaxed);
#endif
}

// Window-capture crop rect, in pixels relative to the captured monitor's own frame (i.e.
HR_EXPORT void hr_pl_set_capture_rect(void* handle, int x, int y, int w, int h) {
    if (!handle) return;
#ifdef _WIN32
    auto* pl = static_cast<Pipeline*>(handle);
    int cx = 0, cy = 0, cw = 0, ch = 0;   // 0/0/0/0 == "no crop"
    if (w > 0 && h > 0) {
        // Clamp to the monitor frame - a window that's partially off-screen
        // (dragged half onto another monitor, etc.) must not make the crop
        // step in capture_loop() read outside bgra_buf.
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > pl->src_w) w = pl->src_w - x;
        if (y + h > pl->src_h) h = pl->src_h - y;
        if (w % 2) w--;
        if (h % 2) h--;
        if (w > 0 && h > 0) { cx = x; cy = y; cw = w; ch = h; }
    }
    std::lock_guard<std::mutex> lk(pl->crop_mtx);
    pl->crop_x = cx; pl->crop_y = cy; pl->crop_w = cw; pl->crop_h = ch;
#endif
}

// Settings > Resolution - the final encoded size (see ComputeOutputDims()
HR_EXPORT void hr_pl_set_output_size(void* handle, int w, int h) {
    if (!handle) return;
#ifdef _WIN32
    auto* pl = static_cast<Pipeline*>(handle);
    if (w > 0 && (w % 2)) w--;
    if (h > 0 && (h % 2)) h--;
    pl->out_w.store(w > 0 ? w : 0, std::memory_order_relaxed);
    pl->out_h.store(h > 0 ? h : 0, std::memory_order_relaxed);
#endif
}

HR_EXPORT void hr_pl_set_output_pixfmt(void* handle, int nv12) {
    if (!handle) return;
#ifdef _WIN32
    static_cast<Pipeline*>(handle)->yuv_pool.SetUseNv12(nv12 != 0);
#endif
}

// Settings > Video > "GPU colour conversion": lets the capture loop use the D3D11 video
// processor (BGRA->NV12 + scale on the GPU) whenever recording with NV12 output and
// nothing has to be composited on the CPU.
HR_EXPORT void hr_pl_set_gpu_convert(void* handle, int enabled) {
    if (!handle) return;
#ifdef _WIN32
    static_cast<Pipeline*>(handle)->gpu_convert.store(enabled != 0, std::memory_order_relaxed);
#else
    (void)enabled;
#endif
}

HR_EXPORT void hr_pl_set_timestamped_pipe(void* handle, int fps) {
    if (!handle) return;
#ifdef _WIN32
    static_cast<Pipeline*>(handle)->ts_fps.store(fps > 0 ? fps : 0, std::memory_order_relaxed);
#else
    (void)fps;
#endif
}

HR_EXPORT void hr_pl_set_recording(void* handle, int active, intptr_t pipe_fd) {
    if (!handle) return;
#ifdef _WIN32
    auto* pl = static_cast<Pipeline*>(handle);
    // A close request that timed out earlier (see hr_pl_end_recording_segment())
    if (active != 0) {
        pl->pending_close_requested.store(false, std::memory_order_release);
        // A frame the capture thread queued just after the previous segment's pipe was closed (it can
        // be mid-iteration when end_recording_segment() flips `recording`) would otherwise be written
        // into the NEXT recording's pipe - at the old size, if the resolution changed in between,
        // which shifts every following frame of the raw stream. Start every segment on an empty queue.
        // Only when the target pipe actually changes, so a repeated call with the same handle can't
        // throw away frames that are legitimately queued.
        if (pl->pipe_handle.load(std::memory_order_acquire) != pipe_fd) {
            std::lock_guard<std::mutex> lock(pl->pipe_queue_mtx);
            std::queue<Pipeline::QueuedFrame>().swap(pl->pipe_queue);
        }
    }
    pl->pipe_handle.store(pipe_fd, std::memory_order_release);
    pl->recording.store((active != 0) && (pipe_fd != 0) && (pipe_fd != -1), std::memory_order_release);
    // YUV conversion buffers are lazily sized from the free-list pool.
#endif
}

// (a manual recording, or an Instant Replay segment, never finalizes on its own whenever
// the pipeline stays alive afterward - i.e.
HR_EXPORT int hr_pl_end_recording_segment(void* handle, int timeout_ms) {
    if (!handle) return 1;
#ifndef _WIN32
    return 1;
#else
    auto* pl = static_cast<Pipeline*>(handle);
    {
        const intptr_t ph = pl->pipe_handle.load(std::memory_order_acquire);
        if (ph == 0 || ph == -1) return 1; // nothing to close
    }
    pl->recording.store(false, std::memory_order_release); // stop the capture thread queuing more frames for this segment
    pl->pending_close_done.store(false, std::memory_order_relaxed);
    pl->pending_close_requested.store(true, std::memory_order_release);
    pl->pipe_queue_cv.notify_all(); // wake the writer thread now instead of its next 10ms poll
    std::unique_lock<std::mutex> lk(pl->finish_mtx);
    bool done = pl->finish_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 3000),
        [pl] { return pl->pending_close_done.load(std::memory_order_acquire); });
    if (!done) {
        HrLog::Warn("Pipeline: writer thread didn't close the previous recording's pipe in time "
                    "(it may be stuck writing) - the ffmpeg process for it likely won't finalize cleanly yet, "
                    "still waiting for it in the background.");
        // (recordings always took the full ~30s "still finalizing" / "force-stopping" path above
        // AND still came out corrupt - every single time the writer thread had any backlog left to
        // drain at Stop, e.g.
    }
    return done ? 1 : 0;
#endif
}

// See Pipeline::boost_priority's own comment.
HR_EXPORT void hr_pl_set_priority_boost(void* handle, int flag) {
    if (!handle) return;
#ifdef _WIN32
    static_cast<Pipeline*>(handle)->boost_priority.store(flag != 0, std::memory_order_relaxed);
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

// Bounds-checked, change-aware replacement for hr_pl_get_preview().
HR_EXPORT int hr_pl_get_preview_ex(void* handle, uint8_t* out_rgb, size_t out_cap,
                                    int* out_w, int* out_h,
                                    int* out_native_w, int* out_native_h,
                                    uint64_t* seq_io) {
    if (!handle || !out_rgb || !out_w || !out_h) return 0;
#ifndef _WIN32
    (void)out_cap; (void)out_native_w; (void)out_native_h; (void)seq_io;
    return 0;
#else
    auto* pl = static_cast<Pipeline*>(handle);
    std::lock_guard<std::mutex> lock(pl->pv_mtx);
    if (!pl->pv_ready || pl->pv_buf.empty()) return 0;
    const uint64_t cur = pl->pv_seq.load(std::memory_order_acquire);
    if (seq_io && *seq_io == cur) return 2;
    if (pl->pv_buf.size() > out_cap) return 0;
    *out_w = pl->pv_actual_w;
    *out_h = pl->pv_actual_h;
    if (out_native_w) *out_native_w = pl->pv_native_w;
    if (out_native_h) *out_native_h = pl->pv_native_h;
    memcpy(out_rgb, pl->pv_buf.data(), pl->pv_buf.size());
    if (seq_io) *seq_io = cur;
    return 1;
#endif
}

// Lock-free "did the thumbnail change?" probe for the UI's paint timer.
// 0 until the first thumbnail exists.
HR_EXPORT uint64_t hr_pl_get_preview_seq(void* handle) {
    if (!handle) return 0;
#ifndef _WIN32
    return 0;
#else
    return static_cast<Pipeline*>(handle)->pv_seq.load(std::memory_order_acquire);
#endif
}

// Native (full-resolution, pre-thumbnail-downscale) size the most recent
// hr_pl_get_preview() thumbnail was generated from -- i.e.
HR_EXPORT int hr_pl_get_native_size(void* handle, int* out_w, int* out_h) {
    if (!handle || !out_w || !out_h) return 0;
#ifndef _WIN32
    return 0;
#else
    auto* pl = static_cast<Pipeline*>(handle);
    std::lock_guard<std::mutex> lock(pl->pv_mtx);
    if (!pl->pv_ready || pl->pv_native_w <= 0 || pl->pv_native_h <= 0) return 0;
    *out_w = pl->pv_native_w;
    *out_h = pl->pv_native_h;
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
    if (out_drops)  *out_drops  = pl->frames_stalled .load(std::memory_order_relaxed);
    if (out_fps)    *out_fps    = pl->fps_actual      .load(std::memory_order_relaxed);
#else
    if (out_frames) *out_frames = 0;
    if (out_drops)  *out_drops  = 0;
    if (out_fps)    *out_fps    = 0.0;
#endif
}

HR_EXPORT void hr_pl_set_overlays(void* handle, const HrOverlayDesc* items, int count) {
    auto* pl = reinterpret_cast<Pipeline*>(handle);
    if (!pl) return;
    std::lock_guard<std::mutex> lock(pl->overlays_mtx);

    // This used to fetch_add() the generation counter on every single call, regardless of
    // whether the list actually changed.
    bool changed;
    if (!items || count <= 0) {
        changed = !pl->overlays.empty();
        pl->overlays.clear();
    } else {
        changed = pl->overlays.size() != (size_t)count ||
                  std::memcmp(pl->overlays.data(), items, sizeof(HrOverlayDesc) * (size_t)count) != 0;
        pl->overlays.assign(items, items + count);
    }
    if (changed) {
        pl->overlays_gen.fetch_add(1, std::memory_order_relaxed);
    }
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
#endif
}

// Settings > General "Preview FPS" - how often the live preview thumbnail is refreshed.
HR_EXPORT void hr_pl_set_preview_fps(void* handle, int fps) {
    if (!handle || fps <= 0) return;
#ifdef _WIN32
    static_cast<Pipeline*>(handle)->preview_fps.store(fps, std::memory_order_relaxed);
#endif
}

// See Pipeline::preview_needed's own comment. Call with enabled=0 whenever
// nothing is going to read hr_pl_get_preview() for this pipeline (Settings
// > "Disable live preview" is on) so capture_loop() skips update_preview()
HR_EXPORT void hr_pl_set_preview_needed(void* handle, int enabled) {
    if (!handle) return;
#ifdef _WIN32
    static_cast<Pipeline*>(handle)->preview_needed.store(enabled != 0, std::memory_order_relaxed);
#endif
}
