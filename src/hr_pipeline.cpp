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
#include <exception>
#include "hr_log.h"
#include "hr_overlay_render.h"

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
// directly - every call site below (g_libs.dx_create(...), g_libs.
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
    int   hr_dx_output_desc(int adapter_idx, int output_idx, int *out_x, int *out_y,
                             int *out_w, int *out_h, char *name_buf, int name_buf_len);
    void  hr_composite_cursor(uint8_t *bgra, int width, int height, int origin_x, int origin_y);
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
#endif  // _WIN32

// ---------------------------------------------------------------------------
// BGRA→thumbnail (box-filter, no intermediate RGB buffer)
// OPT: устраняет bgra_to_rgb_inplace() + rgb_pv буфер целиком.
// Работает только при целочисленных кратностях (быстрый путь).
// При нецелочисленных - nearest-neighbour прямо из BGRA.
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

    // "Cursor" setting: whether to draw the live system cursor into each
    // captured frame (see hr_composite_cursor() in hr_ui_utils.cpp).
    // cap_origin_x/y is the captured output's virtual-desktop offset
    // (from hr_dx_output_desc), needed to translate GetCursorInfo()'s
    // virtual-desktop coordinates into this buffer's local coordinates.
    std::atomic<bool> include_cursor{false};
    int cap_origin_x = 0, cap_origin_y = 0;

    std::vector<uint8_t> bgra_buf;  // src_w * src_h * 4

    // Overlays configured from the UI (RecordingController pushes the
    // current list in via hr_pl_set_overlays whenever it changes -- drag,
    // resize, or edited through the Overlay Manager dialog).
    std::mutex overlays_mtx;
    std::vector<HrOverlayDesc> overlays;
    OverlayCompositor overlay_compositor;
    // OPT: bumped every time hr_pl_set_overlays() actually changes the
    // list. The capture loop (running at recording fps, so this is a
    // hot path) used to lock overlays_mtx and copy the whole vector every
    // single frame even though the overlay configuration itself changes
    // rarely (drag/resize/edit, not per-frame) -- it only needs the
    // lock+copy again once this generation counter has moved.
    std::atomic<uint64_t> overlays_gen{0};

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
    // The writer thread returns finished conversion buffers here so the
    // capture thread can reuse them via move instead of allocating a fresh
    // buffer and copying the full YUV frame (~3 MB at 1080p) every frame -
    // steady-state adds zero heap allocations and zero extra memcpy per
    // frame.
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

                // Hand the buffer back to the free-list instead of letting
                // it fall out of scope and get freed - the capture thread
                // will reuse it for the next frame (no realloc/memcpy).
                {
                    std::lock_guard<std::mutex> lock(free_bufs_mtx);
                    if (free_bufs.size() < MAX_FREE_BUFS)
                        free_bufs.push(std::move(frame));
                }

                if (!ok) {
                    // Pipe write failed - this is EXPECTED and routine the
                    // moment a recording stops (Stop() closes ffmpeg's
                    // stdin while this thread may still have one last
                    // queued frame mid-write), not just a genuine
                    // mid-recording failure.
                    //
                    // BUGFIX: this used to also do
                    // `writer_running.store(false, ...); break;` here,
                    // which permanently ended writer_loop() - i.e. this
                    // whole thread - the very first time a pipe write
                    // failed. That happens on essentially every normal
                    // Stop() (see above), which is harmless for the
                    // *first* recording of a session because hr_pl_start()
                    // had just spun the thread up fresh. But since the
                    // pipeline now stays alive afterward for continued
                    // live preview instead of being destroyed (see
                    // hr_pl_set_recording()/RecordingController's preview
                    // handling), writer_thread is never recreated - it's
                    // the same thread for the whole app session. So the
                    // *second* recording (and every one after) started
                    // with the pipe re-pointed at a brand new ffmpeg
                    // process via hr_pl_set_recording(), but with nobody
                    // left alive to ever call write_pipe() again: every
                    // captured frame just piled up in pipe_queue and got
                    // dropped, so that ffmpeg process received zero bytes
                    // on stdin and produced only header/placeholder data -
                    // exactly the "Windows says unsupported codec" empty
                    // file, on every recording after the first.
                    //
                    // Now: just drop this one frame (its buffer's already
                    // back on the free-list above) and keep the thread
                    // alive, waiting on the queue as normal - the capture
                    // thread already stops enqueueing new frames itself
                    // the moment 'recording' goes false (see the
                    // `if (recording && ...)` gate below), so there's
                    // nothing left to write anyway until the next
                    // recording's hr_pl_set_recording() re-arms it with a
                    // live pipe.
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Update preview thumbnail - directly from BGRA, no intermediate RGB copy
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
        // WIC (used by hr_overlay_render.cpp to decode image overlays)
        // requires COM on the calling thread. Harmless if overlays are
        // never used -- CoUninitialize() below just undoes this.
        bool com_inited = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        // Start at a moderate priority; bumped to TIME_CRITICAL only while
        // actually recording (see the dynamic adjustment in the loop below).
        // Pinning this to TIME_CRITICAL unconditionally - including for the
        // entire time the app just sits open with a live preview and
        // nothing being recorded - was needless system-wide contention for
        // no benefit preview capture actually needs.
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
        const int64_t frame_ns_recording = (fps > 0)
                                 ? (1'000'000'000LL / fps)
                                 : (1'000'000'000LL / 30);
        // Preview-only mode (not recording) never needs more than a
        // handful of frames/sec - the thumbnail shown in the UI is
        // already throttled far below capture rate (see PREVIEW_EVERY).
        // Capturing at the full target fps (often 60+) just to immediately
        // discard nearly all of it was pure waste. Cap at ~15fps while
        // idle; never *slow down* a deliberately-low target fps though.
        const int64_t frame_ns_idle = std::max(frame_ns_recording, 1'000'000'000LL / 15);
        int64_t frame_ns = frame_ns_recording;
        bool was_recording = recording;
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), was_recording ? THREAD_PRIORITY_TIME_CRITICAL
                                                             : THREAD_PRIORITY_ABOVE_NORMAL);
#endif

        // Dynamic preview frequency: fps/20, minimum 1 (of *captured*
        // frames - at the idle 15fps cap this is already a low absolute
        // rate, so also updating every idle-captured frame directly
        // below keeps things simple rather than compounding two throttles).
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

        // OPT: see overlays_gen's declaration -- this snapshot is only
        // refreshed (under overlays_mtx) when the generation counter has
        // actually moved, instead of every single captured frame.
        std::vector<HrOverlayDesc> overlays_snapshot;
        uint64_t last_overlays_gen = (uint64_t)-1; // sentinel: forces the first copy below

        while (running.load(std::memory_order_relaxed)) {
            if (paused.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
#ifdef _WIN32
                if (sw_ctx && g_libs.sw_start) g_libs.sw_start(sw_ctx);
#endif
                next_frame_ns = frame_ns;
                continue;
            }

            // Recording can start/stop mid-flight on this same pipeline
            // now (see hr_pl_set_recording()/RecordingController's preview
            // reuse) - react to that instead of only sizing pacing/
            // priority once when the thread was first created.
            {
                bool is_recording_now = recording;
                if (is_recording_now != was_recording) {
                    was_recording = is_recording_now;
                    frame_ns = is_recording_now ? frame_ns_recording : frame_ns_idle;
                    next_frame_ns = 0; // resync pacing to "now" rather than an old cadence
#ifdef _WIN32
                    SetThreadPriority(GetCurrentThread(), is_recording_now
                                          ? THREAD_PRIORITY_TIME_CRITICAL
                                          : THREAD_PRIORITY_ABOVE_NORMAL);
                    if (sw_ctx && g_libs.sw_start) g_libs.sw_start(sw_ctx);
#endif
                }
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
                // A timeout here does NOT
                // mean capture failed -- DXGI's AcquireNextFrame() times out
                // by design whenever the desktop simply hasn't changed since
                // the last frame (e.g. the user is talking but not moving the
                // mouse/typing), which happens constantly during a normal
                // recording. The old code did `continue` here, which skipped
                // this entire frame_ns interval -- nothing was captured,
                // converted, or queued for it. Since ffmpeg is told a fixed
                // -framerate, every skipped interval is one less frame in the
                // output for the same amount of *real* elapsed time, so the
                // finished file's timeline ends up shorter than the actual
                // recording (a 5-second recording plays back in 3 seconds).
                // bgra_buf still holds the last successfully captured frame
                // (dx_capture() never touched it on a timeout), so fall
                // through and re-encode/re-send that same frame instead of
                // skipping the slot -- this keeps one frame going out per
                // frame_ns tick, so the output duration matches wall-clock
                // time regardless of how static the screen is. Still counted
                // in frames_dropped so the stats reflect that it's a
                // duplicate, not a freshly captured frame.
                frames_dropped.fetch_add(1, std::memory_order_relaxed);
                logged_lost_ = false;
                // Deliberately NOT `continue`-ing here (see comment above) --
                // fall through to the shared capture-success path below,
                // which will re-convert/re-queue whatever's still in
                // bgra_buf from the last real frame.
            } else if (ret == HR_DX_LOST) {
                if (!logged_lost_) {
                    HrLog::Warn("DXGI capture lost (display mode change, UAC prompt, or GPU reset) -- resetting");
                    logged_lost_ = true;
                }
#ifdef _WIN32
                if (g_libs.dx_reset) g_libs.dx_reset(dx_ctx);
#endif
                continue;
            } else if (ret != HR_DX_OK) {
                HrLog::Error("Capture pipeline stopped: dx_capture() returned a fatal error (ret=" + std::to_string(ret) + ")");
                running.store(false);
                break;
            } else {
                logged_lost_ = false;
            }

            // ====== OVERLAY COMPOSITING ======
            // Bakes any configured text/image/input overlays directly into
            // bgra_buf, in place, before it's used for either the preview
            // thumbnail or YUV conversion/encoding below -- so what's shown
            // in the live preview is exactly what ends up in the recording.
            // (Previously overlays were UI-only state that never touched
            // the actual captured frame at all.)
            //
            // Only lock overlays_mtx and copy the overlays vector when the
            // overlay configuration has actually changed (drag/resize/
            // add/remove -- see hr_pl_set_overlays()), rather than on every
            // single captured frame (i.e. up to the full recording fps) even
            // though it rarely changes. overlays_snapshot and
            // last_overlays_gen live outside this block/loop
            // iteration, and the lock+copy only happens again once
            // overlays_gen has actually moved -- steady state is a single
            // relaxed atomic load per frame instead of a mutex acquisition
            // and a std::vector<HrOverlayDesc> copy.
            {
                uint64_t gen = overlays_gen.load(std::memory_order_relaxed);
                if (gen != last_overlays_gen) {
                    std::lock_guard<std::mutex> lock(overlays_mtx);
                    overlays_snapshot = overlays;
                    last_overlays_gen = gen;
                }
                if (!overlays_snapshot.empty()) {
                    // BUGFIX: belt-and-suspenders alongside the try/catch
                    // now inside OverlayCompositor::Apply() itself (see
                    // hr_overlay_render.cpp) -- this loop runs on a bare
                    // std::thread with no exception handler anywhere above
                    // it, so *any* uncaught throw here calls std::terminate()
                    // and kills the whole process mid-recording (ffmpeg's
                    // stdin pipe never gets a clean EOF, so the output file
                    // is left truncated/unplayable). Catching here too means
                    // even a future change to this call site can't silently
                    // reopen that hole.
                    try {
                        overlay_compositor.Apply(bgra_buf.data(), src_w, src_h, src_w * 4, overlays_snapshot);
                    } catch (const std::exception &e) {
                        HrLog::Error(std::string("Overlay compositing threw (") + e.what() +
                                     ") -- this frame's overlays were skipped, recording continues.");
                    } catch (...) {
                        HrLog::Error("Overlay compositing threw an unknown exception -- this frame's "
                                     "overlays were skipped, recording continues.");
                    }
                }
            }

            // ====== CURSOR COMPOSITING ("Cursor" setting) ======
            // Drawn after overlays so the pointer stays visually on top,
            // matching what a viewer would actually see on the real
            // screen. See hr_composite_cursor()'s own comment
            // (hr_ui_utils.cpp) for how/why.
            if (include_cursor.load(std::memory_order_relaxed)) {
                hr_composite_cursor(bgra_buf.data(), src_w, src_h, cap_origin_x, cap_origin_y);
            }

            // ====== YUV CONVERSION ======
            // A frame is only dropped when the queue is genuinely full (the
            // writer really is behind), matching the MAX_QUEUE_SIZE
            // backpressure the queue is designed to provide -- with
            // MAX_QUEUE_SIZE == 3, dropping on *any* frame being present
            // instead would silently discard most frames even when the
            // writer isn't behind, since there's almost always >=1 frame in
            // flight, causing visibly choppy recordings for little CPU
            // benefit.
            //
            // Conversion writes into a buffer recycled from the free-list
            // (filled by the writer thread once it's done with a frame) and
            // is moved -- not copied -- into pipe_queue. This avoids
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
#ifdef _WIN32
        if (com_inited) CoUninitialize();
#endif
    }
};

// ============================================================================
// Exported API
// ============================================================================

HR_EXPORT void* hr_pl_create(int w, int h, int fps,
                               intptr_t pipe_fd, int pv_w, int pv_h) {
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
    pl->pipe_handle = pipe_fd;
    pl->recording   = (pipe_fd != 0 && pipe_fd != -1);

    pl->dx_ctx = g_libs.dx_create(0, 0);
    if (!pl->dx_ctx) {
        HrLog::Error("Pipeline create failed: dx_create() returned null (DXGI desktop duplication init failed -- "
                     "common causes: running over RDP/a virtual display, a just-changed display mode, or "
                     "insufficient permissions)");
        delete pl; return nullptr;
    }
    // Matches the hardcoded dx_create(0, 0) above -- needed so
    // hr_composite_cursor() can translate GetCursorInfo()'s virtual-
    // desktop coordinates into this capture buffer's local coordinates.
    // Defaults to (0,0) (i.e. "assume the captured output starts at the
    // desktop origin") if the lookup fails for some reason, which is
    // right for the common single/primary-monitor case and only wrong
    // for a secondary monitor positioned elsewhere - same blind spot as
    // dx_create(0, 0) itself always capturing output 0 regardless of the
    // "monitor" setting.
    {
        int ox = 0, oy = 0, ow = 0, oh = 0;
        if (hr_dx_output_desc(0, 0, &ox, &oy, &ow, &oh, nullptr, 0)) {
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

HR_EXPORT void hr_pl_destroy(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    auto* pl = static_cast<Pipeline*>(handle);
    
    // Stop all threads
    pl->writer_running.store(false, std::memory_order_relaxed);
    pl->pipe_queue_cv.notify_all();
    pl->running.store(false, std::memory_order_relaxed);
    
    // Both worker threads capture the raw `pl` pointer, so if either one is
    // still running after we give up waiting, force-detaching it and then
    // deleting `pl` below would leave that thread touching freed memory the
    // next time it wakes up. A leaked Pipeline is recoverable; a
    // heap-use-after-free from a zombie thread is not, so track that case
    // and skip the delete (and the resource teardown below, which the
    // still-running thread may also touch) rather than risk it.
    bool leak_pl = false;

    // Wait for writer thread with timeout
    if (pl->writer_thread.joinable()) {
        HANDLE hThread = reinterpret_cast<HANDLE>(pl->writer_thread.native_handle());
        if (WaitForSingleObject(hThread, 1000) == WAIT_OBJECT_0) {
            pl->writer_thread.join();
        } else {
            // Force detach if stuck
            HrLog::Error("Pipeline destroy: writer thread did not stop in time - detaching and leaking Pipeline to avoid use-after-free");
            pl->writer_thread.detach();
            leak_pl = true;
        }
    }
    
    // Wait for capture thread with timeout
    if (pl->capture_thread.joinable()) {
        HANDLE hThread = reinterpret_cast<HANDLE>(pl->capture_thread.native_handle());
        if (WaitForSingleObject(hThread, 1000) == WAIT_OBJECT_0) {
            pl->capture_thread.join();
        } else {
            // Force detach if stuck
            HrLog::Error("Pipeline destroy: capture thread did not stop in time - detaching and leaking Pipeline to avoid use-after-free");
            pl->capture_thread.detach();
            leak_pl = true;
        }
    }

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

// "Cursor" setting - see hr_composite_cursor()'s comment for what this
// actually does. Safe to flip at any time (preview or recording); takes
// effect on the next captured frame.
HR_EXPORT void hr_pl_set_include_cursor(void* handle, int flag) {
    if (!handle) return;
#ifdef _WIN32
    static_cast<Pipeline*>(handle)->include_cursor.store(flag != 0, std::memory_order_relaxed);
#endif
}

HR_EXPORT void hr_pl_set_recording(void* handle, int active, intptr_t pipe_fd) {
    if (!handle) return;
#ifdef _WIN32
    auto* pl = static_cast<Pipeline*>(handle);
    pl->pipe_handle = pipe_fd;
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

HR_EXPORT void hr_pl_set_overlays(void* handle, const HrOverlayDesc* items, int count) {
    auto* pl = reinterpret_cast<Pipeline*>(handle);
    if (!pl) return;
    std::lock_guard<std::mutex> lock(pl->overlays_mtx);

    // BUGFIX: this used to fetch_add() the generation counter on every
    // single call, regardless of whether the list actually changed. The
    // UI calls hr_pl_set_overlays() from the preview timer tick (see
    // RecordingController::SyncOverlays(), invoked ~20-60x/sec any time the
    // app window is open, not just while recording), so overlays_gen was
    // moving every tick even when nothing about the overlays changed.
    // That completely defeated the capture loop's "only re-copy the
    // snapshot when overlays_gen has actually moved" optimization
    // described above -- it was taking overlays_mtx and copying the whole
    // vector on every single captured frame, exactly the cost this counter
    // exists to avoid. Comparing against the previous contents (cheap:
    // these lists are tiny, and HrOverlayDesc is POD) means the gen only
    // moves on a real add/remove/drag/resize/edit, restoring the intended
    // "steady state costs one relaxed atomic load" behavior and cutting a
    // real, continuous, needless chunk of per-frame CPU work.
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
    pl->pv_ready = false;
#endif
}