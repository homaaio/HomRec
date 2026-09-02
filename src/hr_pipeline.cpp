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
    unsigned long hr_dx_last_error(void);
    void  hr_composite_cursor(uint8_t *bgra, int width, int height, int origin_x, int origin_y);
    void  hr_bgra_to_yuv420p(const uint8_t *bgra, uint8_t *yuv, int w, int h);
    void  hr_bgra_to_yuv420p_band(const uint8_t *bgra, uint8_t *yuv, int w, int h, int y0, int y1);
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

// ---------------------------------------------------------------------------
// BGRA→YUV420p, split across a few worker threads by scanline band.
//----------------------------------------------------------------------------

class Yuv420pWorkerPool {
public:
    ~Yuv420pWorkerPool() { Stop(); }

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

    // Splits [0,h) into worker_count()+1 horizontal bands (same split the
    // old per-frame version used) and blocks until every worker-owned band
    // is done. Falls back to converting the whole frame on the calling
    // thread if the pool ends up with zero workers (single-core machine,
    // or a frame small enough that kMinPixelsForThreads decided threading
    // wasn't worth it for this session).
    void Convert(const uint8_t *bgra, uint8_t *yuv, int w, int h) {
        EnsureStarted((long long)w * h);
        int n = (int)threads_.size();
        if (n == 0) {
            hr_bgra_to_yuv420p_band(bgra, yuv, w, h, 0, h);
            return;
        }
        int n_bands = n + 1;
        int band_h = ((h / n_bands) + 1) & ~1;
        if (band_h < 2) band_h = 2;

        {
            std::lock_guard<std::mutex> lk(mtx_);
            bgra_ = bgra; yuv_ = yuv; w_ = w; h_ = h; band_h_ = band_h;
            pending_ = n;
            ++round_;
        }
        cv_start_.notify_all();

        // Main/calling thread does the last band itself, same as before.
        int y0 = band_h * n;
        if (y0 < h) hr_bgra_to_yuv420p_band(bgra, yuv, w, h, y0, h);

        std::unique_lock<std::mutex> lk(mtx_);
        cv_done_.wait(lk, [this] { return pending_ == 0; });
    }

private:
    // No-op after the first call - sizing is decided once, from the first
    // frame actually converted, rather than re-evaluated (and the pool
    // restarted) if dimensions later change slightly mid-session, which
    // isn't worth the thread churn this class exists to avoid in the
    // first place.
    void EnsureStarted(long long total_pixels) {
        if (started_) return;
        started_ = true;
        static constexpr long long kMinPixelsForThreads = 640LL * 480LL;
        unsigned hw = std::thread::hardware_concurrency();
        int n_bands = 1;
        if (total_pixels >= kMinPixelsForThreads && hw > 1) {
            n_bands = (int)std::min<unsigned>(hw - 1, 4);
            n_bands = std::max(n_bands, 1);
        }
        int n_workers = n_bands - 1; // main/calling thread does one band itself
        threads_.reserve((size_t)std::max(0, n_workers));
        for (int i = 0; i < n_workers; ++i) threads_.emplace_back([this, i] { WorkerLoop(i); });
    }

    void WorkerLoop(int idx) {
        int seen_round = 0;
        for (;;) {
            const uint8_t *bgra; uint8_t *yuv; int w, h, band_h;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_start_.wait(lk, [&] { return stop_ || round_ != seen_round; });
                if (stop_) return;
                seen_round = round_;
                bgra = bgra_; yuv = yuv_; w = w_; h = h_; band_h = band_h_;
            }
            int y0 = band_h * idx;
            int y1 = std::min(h, y0 + band_h);
            if (y0 < y1) hr_bgra_to_yuv420p_band(bgra, yuv, w, h, y0, y1);
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (--pending_ == 0) cv_done_.notify_one();
            }
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mtx_;
    std::condition_variable cv_start_, cv_done_;
    bool stop_ = false;
    bool started_ = false;
    int round_ = 0;
    int pending_ = 0;
    const uint8_t *bgra_ = nullptr;
    uint8_t *yuv_ = nullptr;
    int w_ = 0, h_ = 0, band_h_ = 0;
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

// ---------------------------------------------------------------------------
// BGRA->BGRA downscale (box filter on integer ratios, nearest-neighbour
// otherwise) - used to shrink a captured frame down to the user's chosen
// output resolution (Settings > Resolution) BEFORE color conversion and
// encoding, instead of capturing/converting at full native size and only
// scaling down afterward (previously done via ffmpeg's own -vf scale, see
// hr_ff_set_output_size()'s comment in hr_ffmpeg_runner.cpp for the old
// path). On weak/old hardware this matters a lot: color conversion
// (bgra_to_yuv) and the bytes actually written to the pipe both now scale
// with the OUTPUT size instead of the native capture size, and ffmpeg
// never has to run its own scaler at all since what arrives on the pipe
// already matches the encode size (hr_ff_build_cmd() already skips -vf
// scale whenever input/output sizes match). Same structure as
// bgra_to_thumb() above, just BGRA-in/BGRA-out instead of BGRA-in/RGB-out
// (thumb is display-only, this feeds the encoder so channel order must be
// preserved); alpha is irrelevant either way (hr_bgra_to_yuv420p never
// reads it) so it's just written as opaque.
// ---------------------------------------------------------------------------
static void bgra_downscale(const uint8_t* __restrict src,
                            uint8_t*       __restrict dst,
                            int sw, int sh, int dw, int dh)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if (sw == dw && sh == dh) {
        std::memcpy(dst, src, (size_t)sw * sh * 4);
        return;
    }

    if ((sw % dw) == 0 && (sh % dh) == 0) {
        // Fast integer-ratio box filter - same cost class as the color
        // conversion it feeds into, and much cheaper than ffmpeg's
        // general-purpose swscale for this common case (exact ratios like
        // 100%->75%->50%->25% of a 16:9/16:10 native resolution almost
        // always land here).
        int rx = sw / dw, ry = sh / dh;
        int bsz = rx * ry;
        for (int y = 0; y < dh; ++y) {
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
        // Nearest-neighbour fallback (non-integer ratio, e.g. a custom
        // absolute target resolution that doesn't evenly divide the
        // monitor's native size) - cheaper than a general box filter and
        // plenty for a downscale destined for lossy video encoding.
        float rx = (float)sw / dw, ry = (float)sh / dh;
        for (int y = 0; y < dh; ++y) {
            int sy = (int)(y * ry); if (sy >= sh) sy = sh - 1;
            for (int x = 0; x < dw; ++x) {
                int sx = (int)(x * rx); if (sx >= sw) sx = sw - 1;
                const uint8_t* s = src + ((size_t)sy * sw + sx) * 4;
                uint8_t*       d = dst + ((size_t)y  * dw + x ) * 4;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Outstanding "handed-off" Pipelines - see hr_pl_destroy()'s handed_off/
// leak_pl comments below for what this refers to.
//
// A Pipeline whose worker threads didn't stop within hr_pl_destroy()'s
// 1s-per-thread budget gets detached rather than joined, and finishes
// tearing itself down (dx_destroy/sw_destroy/delete this, from
// finish_thread()) on its own time, on its own thread, once it actually
// exits - anywhere from a few hundred ms to (per hr_dx_capture.cpp's own
// retry budget) several seconds later. Nothing previously waited for that
// to actually finish before the app itself was allowed to close: main()
// returning (or WinMain falling out the bottom of the message loop) starts
// unloading DLLs and tearing down process-wide state (including the very
// g_libs function pointers and HrLog machinery those lingering threads are
// still calling into) out from under them - exactly the
// "[CRASH] unhandled C++ exception / std::terminate() -- (no dump written)"
// users hit right around closing the app (or Start()/Stop() quickly
// recreating a pipeline) shortly after a "did not stop in time" message.
// Track how many Pipelines are currently in that handed-off-but-not-yet-
// self-deleted state so shutdown can wait on it via hr_pl_wait_all_detached()
// below instead of racing it blind.
static std::atomic<int> g_handed_off_pipelines{0};

// ---------------------------------------------------------------------------
// Pipeline state
// ---------------------------------------------------------------------------
struct Pipeline {
    int src_w = 0, src_h = 0;
    int fps   = 30;
    int pv_w  = 960, pv_h = 540;
    // "Preview:" settings (Settings > General) - separate from the
    // recording fps/size above, since the live preview thumbnail costs
    // real CPU/GPU time on weak machines even when nothing is being
    // recorded and ideally shouldn't cost more than the user actually
    // wants it to. Read every capture-loop iteration so a live change
    // (hr_pl_set_preview_fps()) takes effect immediately, not just on
    // the next hr_pl_create().
    std::atomic<int> preview_fps{15};
    intptr_t pipe_handle = 0;
    bool recording = false;   // pipe open → encode YUV; false → preview only

    // ====== WINDOW CAPTURE (crop rect) ======
    // src_w/src_h above MUST stay the full monitor's native resolution --
    // that's what DXGI Desktop Duplication actually hands back per frame
    // into bgra_buf, independent of what we do with it afterward (see the
    // big comment on this in RecordingController::ResolveCaptureSize()).
    // "Record just this window" is implemented as a crop applied AFTER
    // capture, not by trying to make DXGI itself capture a smaller area
    // (it can't). crop_w == 0 means "no crop" -- the original/default
    // full-desktop behavior, at zero extra cost per frame. Set via
    // hr_pl_set_capture_rect(); resolved once from the target window's
    // screen rect by RecordingController at Start()/EnsurePreview() time,
    // not re-resolved live if the window moves mid-recording (a known,
    // documented limitation -- see recording_controller.cpp).
    int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;

    // "Effective" frame dimensions for THIS frame: src_w/src_h when
    // crop_w==0, or crop_w/crop_h when a crop rect is active. Every
    // downstream step (overlay/cursor compositing, YUV conversion, the
    // live preview) reads eff_w/eff_h instead of src_w/src_h directly, so
    // window-capture mode doesn't need every one of those call sites to
    // separately know about cropping. Set once per captured frame, right
    // after the crop step in capture_loop() below.
    int eff_w = 0, eff_h = 0;

    // ====== OUTPUT-SIZE DOWNSCALE (Settings > Resolution) ======
    // 0/0 (the default) means "no override, encode at eff_w/eff_h" - the
    // capture_w_==output_w_ common case at 100%/Native costs nothing extra
    // (bgra_downscale() above short-circuits to a memcpy... actually not
    // even that, see the `enc_src` fast path in capture_loop() below,
    // which skips the call entirely when sizes match). Set via
    // hr_pl_set_output_size(), called once by RecordingController::Start()
    // right after hr_pl_set_capture_rect(). Read every frame (capture_loop
    // is the hot path) so it must stay atomic-safe even though in
    // practice it's only ever written once before recording starts.
    std::atomic<int> out_w{0}, out_h{0};
    // Scratch buffer the downscaled BGRA frame is written into ahead of
    // YUV conversion - separate from bgra_buf (rather than downscaling in
    // place) so update_preview()'s thumbnail keeps reading the full
    // eff_w/eff_h frame with cursor/overlays already composited into it,
    // completely unaffected by the encode-side output size. Only ever
    // touched by the capture thread, so no lock needed.
    std::vector<uint8_t> scaled_buf;

    void* dx_ctx = nullptr;
    void* sw_ctx = nullptr;

    Yuv420pWorkerPool yuv_pool;

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
    int  pv_native_w = 0, pv_native_h = 0;
    bool pv_ready    = false;

    std::thread       capture_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};

    std::mutex              finish_mtx;
    std::condition_variable finish_cv;
    std::atomic<bool>       writer_thread_done{false};
    std::atomic<bool>       capture_thread_done{false};

#ifdef _WIN32
    // Manual-reset event for overlapped writes to ffmpeg's stdin pipe - see
    // write_pipe()'s own comment for why this exists. Created lazily (only
    // ever needed while actually recording) and closed alongside dx_ctx/
    // sw_ctx wherever those are (both hr_pl_destroy()'s direct-delete path
    // and finish_thread()'s handed-off path).
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
            // Must be decremented after the delete above, not
            // before - hr_pl_wait_all_detached() uses this counter hitting
            // zero as its "safe to let the process exit now" signal, and
            // callers only want that signal once the Pipeline (and the
            // dx_/sw_ handles it owned) are actually gone, not just about
            // to be.
            g_handed_off_pipelines.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
    bool logged_lost_ = false; // edge-trigger for the DX_LOST diagnostic below
    std::chrono::steady_clock::time_point next_reset_attempt_{};

    std::atomic<int64_t> frames_captured{0};
    std::atomic<int64_t> frames_dropped{0};
    std::atomic<int64_t> frames_stalled{0};
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

        // Same reasoning as the try/catch wrapped around
        // capture_loop()'s while-loop above - this is also a bare
        // std::thread with nothing above it on the call stack to catch an
        // uncaught exception, so any throw in here (write_pipe(), the
        // free-list bookkeeping, etc.) was another std::terminate() path.
        try {
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

    // -------------------------------------------------------------------------
    // Update preview thumbnail - directly from BGRA, no intermediate RGB copy
    // OPT: устранён bgra_to_rgb_inplace() и буфер rgb_pv
    // -------------------------------------------------------------------------
    void update_preview() {
        int tw = pv_w, th = pv_h;
        if (eff_w > 0 && eff_h > 0) {
            float ar = (float)eff_w / (float)eff_h;
            if (tw > (int)(th * ar)) tw = (int)(th * ar);
            else                      th = (int)(tw / ar);
        }
        tw = std::max(tw & ~1, 2);
        th = std::max(th & ~1, 2);

        size_t pv_sz = (size_t)tw * th * 3;
        std::lock_guard<std::mutex> lock(pv_mtx);
        if (pv_buf.size() != pv_sz) pv_buf.resize(pv_sz);

        bgra_to_thumb(bgra_buf.data(), pv_buf.data(), eff_w, eff_h, tw, th);
        pv_actual_w = tw;
        pv_actual_h = th;
        pv_native_w = eff_w;
        pv_native_h = eff_h;
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
        // already throttled by the preview-fps check further down.
        // Capturing at the full target fps (often 60+) just to immediately
        // discard nearly all of it was pure waste. Cap idle capture at the
        // user's Preview FPS setting instead of a fixed 15 - never *slow
        // down* a deliberately-low target fps though. Recomputed on every
        // is_recording_now transition below so a live change to the
        // Preview FPS setting is picked up without restarting the pipeline.
        auto compute_frame_ns_idle = [&]() {
            int want_fps = preview_fps.load(std::memory_order_relaxed);
            if (want_fps < 1) want_fps = 1;
            return std::max(frame_ns_recording, (int64_t)(1'000'000'000LL / want_fps));
        };
        int64_t frame_ns_idle = compute_frame_ns_idle();
        int64_t frame_ns = frame_ns_recording;
        bool was_recording = recording;
#ifdef _WIN32
        // This used to jump straight to THREAD_PRIORITY_TIME_CRITICAL
        // the moment recording started (both here and in the is_recording_now
        // transition below). TIME_CRITICAL is the highest priority Windows
        // has -- above every normal-priority thread in the system, not just
        // this process -- so on anything less than a beefy multi-core
        // machine it could starve the UI thread of the CPU time it needs to
        // pump messages and repaint, which is exactly what "the preview
        // freezes the moment recording starts" looks like from the user's
        // side (the capture thread itself was fine; the UI just couldn't
        // get scheduled to show it). HIGHEST is still well above normal/
        // idle-process-class threads (enough to keep capture timing tight
        // under load) without reserving the CPU ahead of literally
        // everything else, including our own UI thread.
        SetThreadPriority(GetCurrentThread(), was_recording ? THREAD_PRIORITY_HIGHEST
                                                             : THREAD_PRIORITY_ABOVE_NORMAL);
#endif

        // Dynamic preview frequency: driven by the "Preview FPS" setting
        // (AppState::preview_fps / hr_pl_set_preview_fps()) rather than a
        // fixed fraction of the recording fps - time-based (elapsed ns
        // since the last preview update) instead of a frame-count modulo
        // so it stays accurate to the requested rate regardless of how
        // fast the capture loop itself is ticking (recording vs idle
        // pacing above already differ by 2-4x).
        int64_t last_preview_ns = 0;

        // timeout_ms = 2/3 frame (was 1/2) → fewer TIMEOUT drops
        int timeout_ms = static_cast<int>(frame_ns * 2 / 3'000'000LL);
        if (timeout_ms < 8)  timeout_ms = 8;
        if (timeout_ms > 33) timeout_ms = 33;

        int64_t fps_acc_frames = 0, fps_acc_start_ns = 0;

#ifdef _WIN32
        if (sw_ctx && g_libs.sw_start) g_libs.sw_start(sw_ctx);
#endif
        int64_t next_frame_ns = frame_ns;

        // OPT: see overlays_gen's declaration -- this snapshot is only
        // refreshed (under overlays_mtx) when the generation counter has
        // actually moved, instead of every single captured frame.
        std::vector<HrOverlayDesc> overlays_snapshot;
        uint64_t last_overlays_gen = (uint64_t)-1; // sentinel: forces the first copy below

        // The whole loop below now runs inside a try/catch. This
        // thread is started bare (std::thread([pl]{ pl->capture_loop(); }),
        // see hr_pl_start()) with no exception handler anywhere above it on
        // the call stack - the overlay-compositing try/catch further down
        // was already added for exactly this reason (see its own comment),
        // but it only covered that one call site. Any OTHER uncaught throw
        // in this loop (a bad_alloc from a resize with a bogus size, a
        // vector::at, anything) was still an unhandled C++ exception on a
        // std::thread with nothing above it to catch it - which per the
        // standard calls std::terminate() and takes the whole process down
        // with it, matching the "[CRASH] unhandled C++ exception /
        // std::terminate()" entries this app was hitting. Catching here
        // turns that into "this one pipeline stops, logged, app keeps
        // running" instead - same outcome a clean Stop()/TeardownPreview()
        // would have produced.
        try {
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
                    if (!is_recording_now) frame_ns_idle = compute_frame_ns_idle();
                    frame_ns = is_recording_now ? frame_ns_recording : frame_ns_idle;
                    next_frame_ns = 0; // resync pacing to "now" rather than an old cadence
#ifdef _WIN32
                    // See the matching comment above (~line 702) -
                    // TIME_CRITICAL here starved the UI thread the moment
                    // recording started on an already-running preview
                    // pipeline, which is the more common of the two cases
                    // (see RecordingController::Start()'s reused_preview_pipeline
                    // path) and so the more common way to see the freeze.
                    SetThreadPriority(GetCurrentThread(), is_recording_now
                                          ? THREAD_PRIORITY_HIGHEST
                                          : THREAD_PRIORITY_ABOVE_NORMAL);
                    if (sw_ctx && g_libs.sw_start) g_libs.sw_start(sw_ctx);
#endif
                }
            }

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
                auto now = std::chrono::steady_clock::now();
                if (g_libs.dx_reset && now >= next_reset_attempt_) {
                    g_libs.dx_reset(dx_ctx);
                    // Whether or not that succeeded, don't try again for a
                    // full second - if it failed because something (e.g. a
                    // game) still holds exclusive fullscreen, back-to-back
                    // retries just burn GPU/driver time neither of us can
                    // spare. A successful reset also isn't free to redo
                    // constantly, and the very next AcquireNextFrame() will
                    // tell us immediately if it actually worked anyway.
                    next_reset_attempt_ = now + std::chrono::seconds(1);
                }
#endif
                // Same reasoning as the HR_DX_TIMEOUT branch above: fall
                // through and re-encode whatever's still in bgra_buf from
                // the last real frame rather than skipping this slot
                // entirely, so the output timeline doesn't fall behind
                // wall-clock time for however long capture stays lost.
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
            // Compacts just the target window's sub-rectangle of the full
            // monitor frame down to a tightly-packed buffer at bgra_buf's
            // start, in place, and points eff_w/eff_h at ITS dimensions --
            // every step below (overlays, cursor, YUV conversion, preview)
            // reads eff_w/eff_h, not src_w/src_h, so this one spot is the
            // only place that needs to know about cropping. Zero-cost
            // no-op when crop_w==0 (the default, full-desktop case).
            if (crop_w > 0 && crop_h > 0) {
                eff_w = crop_w;
                eff_h = crop_h;
                uint8_t* buf = bgra_buf.data();
                // memmove, not memcpy: source and destination rows can
                // overlap (e.g. crop_x==0 && crop_y==0 makes row 0
                // identical), and memmove is the one of the two that's
                // defined to handle that correctly.
                for (int y = 0; y < crop_h; ++y) {
                    const uint8_t* srow = buf + ((size_t)(crop_y + y) * src_w + crop_x) * 4;
                    uint8_t*       drow = buf + (size_t)y * crop_w * 4;
                    std::memmove(drow, srow, (size_t)crop_w * 4);
                }
            } else {
                eff_w = src_w;
                eff_h = src_h;
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
                    // Belt-and-suspenders alongside the try/catch
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
                        overlay_compositor.Apply(bgra_buf.data(), eff_w, eff_h, eff_w * 4, overlays_snapshot);
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
            // (hr_ui_utils.cpp) for how/why. Origin is offset by
            // crop_x/crop_y (both 0 when not cropping) since cap_origin_x/y
            // is the *monitor's* virtual-desktop offset, but bgra_buf now
            // holds just the cropped window sub-rectangle of it.
            if (include_cursor.load(std::memory_order_relaxed)) {
                hr_composite_cursor(bgra_buf.data(), eff_w, eff_h,
                                     cap_origin_x + crop_x, cap_origin_y + crop_y);
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
                int req_w = out_w.load(std::memory_order_relaxed);
                int req_h = out_h.load(std::memory_order_relaxed);
                const uint8_t* enc_src = bgra_buf.data();
                int enc_w = eff_w, enc_h = eff_h;
                if (req_w > 0 && req_h > 0 && (req_w != eff_w || req_h != eff_h)) {
                    const size_t scaled_needed = (size_t)req_w * req_h * 4;
                    if (scaled_buf.size() != scaled_needed) scaled_buf.resize(scaled_needed);
                    bgra_downscale(bgra_buf.data(), scaled_buf.data(), eff_w, eff_h, req_w, req_h);
                    enc_src = scaled_buf.data();
                    enc_w = req_w; enc_h = req_h;
                }

                std::vector<uint8_t> yuv_frame;
                {
                    std::lock_guard<std::mutex> lock(free_bufs_mtx);
                    if (!free_bufs.empty()) {
                        yuv_frame = std::move(free_bufs.front());
                        free_bufs.pop();
                    }
                }

                const size_t needed = (size_t)enc_w * enc_h * 3 / 2;
                if (yuv_frame.size() != needed) yuv_frame.resize(needed);

                yuv_pool.Convert(enc_src, yuv_frame.data(), enc_w, enc_h);

                std::vector<uint8_t> dropped;  // popped outside free_bufs_mtx to avoid nested locks
                {
                    std::lock_guard<std::mutex> lock(pipe_queue_mtx);

                    // Only drop when the writer is genuinely behind (queue full)
                    if (pipe_queue.size() >= MAX_QUEUE_SIZE) {
                        dropped = std::move(pipe_queue.front());
                        pipe_queue.pop();
                        frames_dropped.fetch_add(1, std::memory_order_relaxed);
                        frames_stalled.fetch_add(1, std::memory_order_relaxed);
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

            // Preview: dynamic frequency, driven by the Preview FPS setting
            // (see last_preview_ns's declaration above for why time-based).
#ifdef _WIN32
            {
                int64_t now_ns = (sw_ctx && g_libs.sw_elapsed_ns)
                                      ? g_libs.sw_elapsed_ns(sw_ctx)
                                      : next_frame_ns; // fallback: still monotonic
                int want_fps = preview_fps.load(std::memory_order_relaxed);
                if (want_fps < 1) want_fps = 1;
                int64_t preview_interval_ns = 1'000'000'000LL / want_fps;
                if (now_ns - last_preview_ns >= preview_interval_ns) {
                    last_preview_ns = now_ns;
                    update_preview();
                }
            }
#endif

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
    pl->pipe_handle = pipe_fd;
    pl->recording   = (pipe_fd != 0 && pipe_fd != -1);

    // This used to be a hardcoded dx_create(0, 0), so the
    // "Monitor:" setting only ever affected capture *sizing*
    // (RecordingController::ResolveCaptureSize()) and never which
    // physical display DXGI actually duplicated - picking any monitor
    // but the primary silently kept recording the primary one anyway.
    // output_idx is RecordingController's resolved (0-based) monitor
    // index; adapter_idx stays 0 (single-adapter assumption, i.e. all
    // monitors on the same GPU - the common case; multi-adapter setups
    // are a separate, bigger fix).
    pl->dx_ctx = g_libs.dx_create(0, output_idx);
    if (!pl->dx_ctx) {
        // This used to just say "returned null" with no way to
        // tell which of the "common causes" it actually was - now logs
        // the real HRESULT (hex, so it's directly greppable against the
        // DXGI/WinError docs) alongside the guess. See hr_dx_last_error()
        // in hr_dxgi_capture.cpp for where this comes from.
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
    // Defaults to (0,0) (i.e. "assume the captured output starts at the
    // desktop origin") if the lookup fails for some reason.
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
    // next time it wakes up. A leaked Pipeline used to be permanent (see
    // the finish_thread()/handed_off comment on the Pipeline struct); now
    // it's only temporary - detaching sets handed_off, and whichever
    // thread finishes last does the actual free once it's really safe,
    // instead of nobody ever doing it. leak_pl here just means "don't also
    // do it ourselves below", not "it's gone forever".
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
            // Force detach if stuck. handed_off must be set *immediately*,
            // before we go on to (possibly) wait up to another second on
            // capture_thread below - if this writer thread finishes just
            // after this WaitForSingleObject gave up on it (very plausible;
            // it timed out by definition, not by a huge margin) and we
            // hadn't set handed_off yet, its finish_thread() call would see
            // handed_off still false and skip the cleanup it's now
            // responsible for, right before we set it true a moment too
            // late - leaking for real, forever, again. Setting it here,
            // per-detach, closes that window.
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

    // Defensive: if handed_off somehow ended up set without leak_pl being
    // set in this same call - the expected way that happens now is
    // hr_pl_stop() (see its own comment) already having detached one or
    // both threads before this function ever ran, in which case both
    // joinable() checks above are false and this call never touched
    // leak_pl at all - treat it the same as detaching right now, rather
    // than freeing `this` out from under a thread that might still be
    // alive.
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
    if (pl->dx_ctx && g_libs.dx_destroy) g_libs.dx_destroy(pl->dx_ctx);
    if (pl->sw_ctx && g_libs.sw_destroy) g_libs.sw_destroy(pl->sw_ctx);
    if (pl->write_evt) { CloseHandle(pl->write_evt); pl->write_evt = nullptr; }
    delete pl;
#endif
}

// ---------------------------------------------------------------------------
// hr_pl_wait_all_detached
//
// Blocks (sleeping, not busy-looping) until every Pipeline that
// hr_pl_destroy() ever had to hand off (see g_handed_off_pipelines' own
// comment above) has actually finished tearing itself down, or until
// timeout_ms elapses - whichever comes first. Returns 1 if everything was
// clear (either nothing was ever handed off, or it finished in time), 0 on
// timeout, so the caller can log that something is still stuck rather than
// silently racing it.
//
// Meant to be called once, late in app shutdown (after the window/recording
// UI has already told everything to stop, right before returning from
// WinMain/main) - see main_frame.cpp's OnClose(). Safe to call even if
// nothing was ever handed off (returns 1 immediately).
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
    // dx_get_size() reads back whatever DXGI's output-duplication
    // desc last reported, queried right after dx_create()/reset() - under
    // exactly the conditions this whole file's "common causes" comment
    // keeps citing (RDP, a virtual display, a display mode that changes
    // mid-query) that can come back as some huge/bogus positive value
    // instead of failing outright. real_w/real_h > 0 alone doesn't catch
    // that. Before this clamp, a bogus size sailed straight into the
    // bgra_buf.resize() below with no try/catch anywhere above this call
    // (hr_pl_start() runs on the caller's thread - RecordingController's,
    // i.e. the UI thread, via EnsurePreview()/Start()) - a multi-gigabyte
    // resize() request throws std::length_error/std::bad_alloc, uncaught,
    // which is exactly an "unhandled C++ exception" crossing back out
    // through wx's message loop: the std::terminate() this app was seen
    // hitting. Clamp to a generous real-world bound (same idea as
    // kMaxOverlayCanvasDim in hr_overlay_render.cpp) and fall back to
    // whatever size the Pipeline already had instead.
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
    
    // This used to WaitForSingleObject() on each thread and then
    // just fall through without ever calling .join() or .detach() on
    // either - the std::thread objects stayed "joinable" (from the C++
    // object's point of view) no matter how the wait came out. That's
    // harmless *today* only because RecordingController::Stop() (the one
    // caller) always follows this up with hr_pl_destroy() on the same
    // handle a little later, which redundantly re-waits and does the
    // real join()/detach() itself - up to another 2s of needless waiting
    // on top of the 2s already spent here, and a stuck pipeline sat
    // completely unaccounted-for in g_handed_off_pipelines (see its own
    // comment) for however long RecordingController::Stop() then spends
    // finalizing ffmpeg/merging audio (documented up to ~30s) before it
    // finally reaches that hr_pl_destroy() call. Any code path that ever
    // called hr_pl_stop() without a guaranteed follow-up hr_pl_destroy()
    // would leave a joinable-but-abandoned std::thread sitting in the
    // Pipeline - destroying that Pipeline via any route other than
    // hr_pl_destroy()'s own careful handling would destruct a joinable
    // std::thread and call std::terminate() outright. Doing the real
    // join()/detach() (with the same handed_off/g_handed_off_pipelines
    // bookkeeping hr_pl_destroy() uses) here instead removes that
    // fragility, makes hr_pl_destroy()'s later call on the same handle an
    // instant no-op (both threads already non-joinable), and starts the
    // handed-off accounting as soon as this function actually gives up
    // instead of only once hr_pl_destroy() eventually runs.
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

    // If either thread got handed off above, hr_pl_destroy() (the only
    // caller of this function today, a little further down in
    // RecordingController::Stop()) will see handed_off already set and
    // its own capture_thread/writer_thread.joinable() checks will both be
    // false (already consumed by join()/detach() here) - it'll fall
    // straight through to its own `if (pl->handed_off.load()) leak_pl =
    // true;` and return without touching pl again, exactly as if it had
    // done the detaching itself. Safe to keep going here either way.

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

// Window-capture crop rect, in pixels relative to the captured monitor's
// own frame (i.e. window's screen rect minus the monitor's origin - the
// caller, RecordingController, already has both). w<=0 or h<=0 clears
// the crop (back to full-desktop). See the Pipeline::crop_x/crop_w
// comment for the full explanation.
//
// Safe to call at any time (including while the pipeline is already
// running, e.g. RecordingController re-resolving it on every Start()
// call even when reusing an existing preview pipeline) - same caveat as
// hr_pl_set_recording()'s plain fields elsewhere in this file: not
// synchronized against the capture thread, so a change can take a frame
// or two to visibly land rather than being atomic with the very next
// frame, which is fine for a rect that only changes when the user picks
// a different window, not every frame.
HR_EXPORT void hr_pl_set_capture_rect(void* handle, int x, int y, int w, int h) {
    if (!handle) return;
#ifdef _WIN32
    auto* pl = static_cast<Pipeline*>(handle);
    if (w <= 0 || h <= 0) {
        pl->crop_x = pl->crop_y = pl->crop_w = pl->crop_h = 0;
        return;
    }
    // Clamp to the monitor frame - a window that's partially off-screen
    // (dragged half onto another monitor, etc.) must not make the crop
    // step below read outside bgra_buf.
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > pl->src_w) w = pl->src_w - x;
    if (y + h > pl->src_h) h = pl->src_h - y;
    if (w <= 0 || h <= 0) {
        pl->crop_x = pl->crop_y = pl->crop_w = pl->crop_h = 0;
        return;
    }
    if (w % 2) w--;  // keep it even, same reasoning as capture_w_/h_ elsewhere (YUV420 needs even dims)
    if (h % 2) h--;
    pl->crop_x = x; pl->crop_y = y; pl->crop_w = w; pl->crop_h = h;
#endif
}

// Settings > Resolution - the final encoded size (see ComputeOutputDims()
// in recording_controller.cpp). Pass 0,0 (or never call this) for "no
// downscale, encode at the captured/cropped size" - the default. Must be
// called with the actual output size whenever it differs from the capture
// size, otherwise ffmpeg's rawvideo demuxer (told this same size via
// hr_ff_set_video_params()) will misinterpret the pipe's byte stream.
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

// Native (full-resolution, pre-thumbnail-downscale) size the most recent
// hr_pl_get_preview() thumbnail was generated from -- i.e. the coordinate
// space OverlayDef::x/y/w/h are actually composited in (see
// overlay_compositor.Apply(bgra_buf.data(), eff_w, eff_h, ...) in
// capture_loop()). The overlay placement dialog shows the user the
// thumbnail from hr_pl_get_preview() but must convert drag positions
// through *this* size, not the thumbnail's own w/h, or every placement
// ends up scaled down by however much the thumbnail was downscaled by.
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

    // This used to fetch_add() the generation counter on every
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

// Settings > General "Preview FPS" - how often the live preview thumbnail
// is refreshed. Takes effect immediately (read every capture-loop
// iteration, see preview_fps's declaration on the Pipeline struct), no
// need to restart the pipeline.
HR_EXPORT void hr_pl_set_preview_fps(void* handle, int fps) {
    if (!handle || fps <= 0) return;
#ifdef _WIN32
    static_cast<Pipeline*>(handle)->preview_fps.store(fps, std::memory_order_relaxed);
#endif
}