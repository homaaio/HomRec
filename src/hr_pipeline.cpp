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
#include <queue>
#include <condition_variable>
#include <string>
#include "hr_log.h"

static constexpr int HR_DX_OK      =  0;
static constexpr int HR_DX_TIMEOUT =  1;
static constexpr int HR_DX_LOST    =  2;
static constexpr int HR_DX_ERROR   = -1;

// ---------------------------------------------------------------------------
// Dynamic loader for helper DLLs
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// PATCHED for static linking (see INTEGRATION_NOTES.md "internal DLL
// loading" finding): this used to LoadLibraryW/GetProcAddress
// hr_dxgi_capture.dll / hr_encoder_helpers.dll / hr_stopwatch.dll by
// filename at runtime. Under the C++ port those three files are compiled
// directly into the same exe as this one (see the root Makefile), so
// those DLLs never exist on disk and ensure_libs() would silently fail
// (dx_create/bgra_to_yuv/etc. all null, capture never starts, no error
// surfaced anywhere obvious). Fixed by declaring the real exported
// functions extern "C" and pointing the same g_libs fields at them
// directly — every call site below (g_libs.dx_create(...), g_libs.
// bgra_to_yuv(...), etc.) is UNCHANGED, only how the fields get populated.
// If you still want the DLL-split architecture instead, revert this hunk
// and go back to building hr_dxgi_capture.dll/hr_encoder_helpers.dll/
// hr_stopwatch.dll via build_native.py.
#ifdef _WIN32
extern "C" {
    void *hr_dx_create(int adapter_idx, int output_idx);
    void  hr_dx_destroy(void *handle);
    int   hr_dx_get_size(void *handle, int *out_w, int *out_h);
    int   hr_dx_capture(void *handle, uint8_t *out_bgra, int timeout_ms);
    int   hr_dx_reset(void *handle);
    void  hr_bgra_to_yuv420p(const uint8_t *bgra, uint8_t *yuv, int w, int h);
    void *hr_sw_create();
    void  hr_sw_destroy(void *handle);
    void  hr_sw_start(void *handle);
    void  hr_sw_sleep_until_ns(void *handle, int64_t target_ns);
    int64_t hr_sw_elapsed_ns(void *handle);
}

struct LibHandles {
    bool loaded = false;

    void *(*dx_create)(int, int)                       = &hr_dx_create;
    void  (*dx_destroy)(void*)                          = &hr_dx_destroy;
    int   (*dx_capture)(void*, uint8_t*, int)           = &hr_dx_capture;
    int   (*dx_get_size)(void*, int*, int*)             = &hr_dx_get_size;
    int   (*dx_reset)(void*)                            = &hr_dx_reset;
    void  (*bgra_to_yuv)(const uint8_t*, uint8_t*, int, int) = &hr_bgra_to_yuv420p;
    void *(*sw_create)()                                = &hr_sw_create;
    void  (*sw_destroy)(void*)                          = &hr_sw_destroy;
    void  (*sw_start)(void*)                            = &hr_sw_start;
    void  (*sw_sleep_until)(void*, int64_t)             = &hr_sw_sleep_until_ns;
    int64_t (*sw_elapsed_ns)(void*)                     = &hr_sw_elapsed_ns;

    bool load(const wchar_t* /*base_dir*/) {
        loaded = true; // all statically linked — nothing can fail to "load" anymore
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

    // Preview
    std::vector<uint8_t> pv_buf;
    std::mutex           pv_mtx;
    int  pv_actual_w = 0, pv_actual_h = 0;
    bool pv_ready    = false;

    std::thread       capture_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};
    bool logged_lost_ = false; // edge-trigger for the DX_LOST diagnostic below

    std::atomic<int64_t> frames_captured{0};
    std::atomic<int64_t> frames_dropped{0};
    std::atomic<double>  fps_actual{0.0};

    // ====== FRAME QUEUE FOR ASYNC WRITING ======
    std::queue<std::vector<uint8_t>> pipe_queue;
    std::mutex pipe_queue_mtx;
    std::condition_variable pipe_queue_cv;
    std::thread writer_thread;
    std::atomic<bool> writer_running{false};
    static constexpr size_t MAX_QUEUE_SIZE = 3;  // Max frames in queue - reduced for lower latency

    // ====== RECYCLED BUFFER POOL ======
    // PERF FIX: previously every frame allocated a fresh conversion buffer and
    // pipe_queue.push(yuv_buf) *copied* the full YUV frame (~3 MB at 1080p) on
    // top of that. The writer thread now returns finished buffers here so the
    // capture thread can reuse them via move — steady-state adds zero heap
    // allocations and zero extra memcpy per frame.
    std::queue<std::vector<uint8_t>> free_bufs;
    std::mutex free_bufs_mtx;
    static constexpr size_t MAX_FREE_BUFS = MAX_QUEUE_SIZE + 2;

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
    // Writer thread - consumes frames from queue and writes to pipe
    // -------------------------------------------------------------------------
    void writer_loop() {
#ifdef _WIN32
        // Set high priority for writer to keep pipe full
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
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

        while (writer_running.load(std::memory_order_relaxed)) {
            std::vector<uint8_t> frame;
            
            {
                std::unique_lock<std::mutex> lock(pipe_queue_mtx);
                
                // Wait for frames or shutdown signal
                pipe_queue_cv.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                    return !pipe_queue.empty() || 
                           !writer_running.load(std::memory_order_relaxed);
                });
                
                // Exit if shutting down and queue is empty
                if (!writer_running.load(std::memory_order_relaxed) && pipe_queue.empty()) {
                    break;
                }
                
                if (pipe_queue.empty()) {
                    continue;
                }
                
                // Move frame out of queue (no copy)
                frame = std::move(pipe_queue.front());
                pipe_queue.pop();
            }
            
            // Write to pipe outside the lock
            if (!frame.empty()) {
                bool ok = write_pipe(frame.data(), frame.size());

                // PERF FIX: hand the buffer back to the free-list instead of
                // letting it fall out of scope and get freed — the capture
                // thread will reuse it for the next frame (no realloc/memcpy).
                {
                    std::lock_guard<std::mutex> lock(free_bufs_mtx);
                    if (free_bufs.size() < MAX_FREE_BUFS)
                        free_bufs.push(std::move(frame));
                }

                if (!ok) {
                    // Pipe write failed - stop everything
                    recording = false;
                    writer_running.store(false, std::memory_order_relaxed);
                    running.store(false, std::memory_order_relaxed);
                    break;
                }
            }
        }
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
        // CRITICAL priority to avoid 15ms preemptions
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
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
        const int64_t frame_ns = (fps > 0)
                                 ? (1'000'000'000LL / fps)
                                 : (1'000'000'000LL / 30);

        // Dynamic preview frequency: fps/20, minimum 1
        const int PREVIEW_EVERY = std::max(1, fps / 20);

        // timeout_ms = 2/3 frame (was 1/2) → fewer TIMEOUT drops
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
                if (!logged_lost_) {
                    HrLog::Warn("DXGI capture lost (display mode change, UAC prompt, or GPU reset) -- resetting");
                    logged_lost_ = true;
                }
#ifdef _WIN32
                if (g_libs.dx_reset) g_libs.dx_reset(dx_ctx);
#endif
                continue;
            }
            logged_lost_ = false;
            if (ret != HR_DX_OK) {
                HrLog::Error("Capture pipeline stopped: dx_capture() returned a fatal error (ret=" + std::to_string(ret) + ")");
                running.store(false);
                break;
            }

            // ====== YUV CONVERSION ======
            // BUG FIX: the previous version skipped conversion (and dropped the
            // frame) whenever the queue held *any* frame at all — with
            // MAX_QUEUE_SIZE == 3 that meant most frames were silently dropped
            // even though the writer was nowhere close to falling behind, since
            // there is almost always ≥1 frame in flight. That caused visibly
            // choppy recordings while barely saving any CPU. We now only drop
            // when the queue is genuinely full (the writer really is behind),
            // matching the MAX_QUEUE_SIZE backpressure the queue was designed
            // to provide.
            //
            // PERF FIX: conversion now writes into a buffer recycled from the
            // free-list (filled by the writer thread once it's done with a
            // frame) and is moved — not copied — into pipe_queue. This removes
            // both the per-frame heap allocation and the full-frame memcpy
            // that pipe_queue.push(yuv_buf) used to perform.
#ifdef _WIN32
            if (recording && g_libs.bgra_to_yuv) {
                std::vector<uint8_t> yuv_frame;
                {
                    std::lock_guard<std::mutex> lock(free_bufs_mtx);
                    if (!free_bufs.empty()) {
                        yuv_frame = std::move(free_bufs.front());
                        free_bufs.pop();
                    }
                }

                const size_t needed = (size_t)src_w * src_h * 3 / 2;
                if (yuv_frame.size() != needed) yuv_frame.resize(needed);

                g_libs.bgra_to_yuv(bgra_buf.data(), yuv_frame.data(), src_w, src_h);

                std::vector<uint8_t> dropped;  // popped outside free_bufs_mtx to avoid nested locks
                {
                    std::lock_guard<std::mutex> lock(pipe_queue_mtx);

                    // Only drop when the writer is genuinely behind (queue full)
                    if (pipe_queue.size() >= MAX_QUEUE_SIZE) {
                        dropped = std::move(pipe_queue.front());
                        pipe_queue.pop();
                        frames_dropped.fetch_add(1, std::memory_order_relaxed);
                    }

                    pipe_queue.push(std::move(yuv_frame));
                    pipe_queue_cv.notify_one();
                }

                if (!dropped.empty()) {
                    std::lock_guard<std::mutex> lock(free_bufs_mtx);
                    if (free_bufs.size() < MAX_FREE_BUFS)
                        free_bufs.push(std::move(dropped));
                }
            }
#endif
            frames_captured.fetch_add(1, std::memory_order_relaxed);

            // Preview: dynamic frequency
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
        
        // Signal writer thread to stop
        writer_running.store(false, std::memory_order_relaxed);
        pipe_queue_cv.notify_all();
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
    if (!pl->dx_ctx) {
        HrLog::Error("Pipeline create failed: dx_create() returned null (DXGI desktop duplication init failed -- "
                     "common causes: running over RDP/a virtual display, a just-changed display mode, or "
                     "insufficient permissions)");
        delete pl; return nullptr;
    }

    pl->sw_ctx = g_libs.sw_create();
    if (!pl->sw_ctx) {
        HrLog::Error("Pipeline create failed: sw_create() (frame pacing/stopwatch) returned null");
        g_libs.dx_destroy(pl->dx_ctx); delete pl; return nullptr;
    }

    return pl;
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
    
    // Wait for writer thread with timeout
    if (pl->writer_thread.joinable()) {
        HANDLE hThread = reinterpret_cast<HANDLE>(pl->writer_thread.native_handle());
        if (WaitForSingleObject(hThread, 1000) == WAIT_OBJECT_0) {
            pl->writer_thread.join();
        } else {
            // Force detach if stuck
            pl->writer_thread.detach();
        }
    }
    
    // Wait for capture thread with timeout
    if (pl->capture_thread.joinable()) {
        HANDLE hThread = reinterpret_cast<HANDLE>(pl->capture_thread.native_handle());
        if (WaitForSingleObject(hThread, 1000) == WAIT_OBJECT_0) {
            pl->capture_thread.join();
        } else {
            // Force detach if stuck
            pl->capture_thread.detach();
        }
    }
    
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
    // YUV conversion buffers are now lazily sized from the free-list pool
    // the first time a frame is converted (see capture_loop()).

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
    
    // Wait for capture thread with timeout
    if (pl->capture_thread.joinable()) {
        HANDLE hThread = reinterpret_cast<HANDLE>(pl->capture_thread.native_handle());
        if (WaitForSingleObject(hThread, 1000) == WAIT_OBJECT_0) {
            pl->capture_thread.join();
        } else {
            pl->capture_thread.detach();
        }
    }
    
    // Wait for writer thread with timeout
    if (pl->writer_thread.joinable()) {
        HANDLE hThread = reinterpret_cast<HANDLE>(pl->writer_thread.native_handle());
        if (WaitForSingleObject(hThread, 1000) == WAIT_OBJECT_0) {
            pl->writer_thread.join();
        } else {
            pl->writer_thread.detach();
        }
    }
    
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

HR_EXPORT void hr_pl_set_recording(void* handle, int active, int pipe_fd) {
    if (!handle) return;
#ifdef _WIN32
    auto* pl = static_cast<Pipeline*>(handle);
    pl->pipe_handle = static_cast<intptr_t>(pipe_fd);
    pl->recording   = (active != 0) && (pipe_fd != 0) && (pipe_fd != -1);
    // YUV conversion buffers are lazily sized from the free-list pool.
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