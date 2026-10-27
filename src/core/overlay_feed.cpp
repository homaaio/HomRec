// overlay_feed.cpp - the "overlay feeder" of a ddagrab recording. ffmpeg grabs the screen itself, so HomRec's
// overlays (text / image / gif / webcam / input) reach it as a SECOND video input: a transparent full-frame BGRA
// picture, `fps` pictures per second, written to a named pipe that ffmpeg reads (see the overlay graph in
// ffmpeg_runner.cpp). The picture is drawn here with the same cached layer bitmaps the capture pipeline uses
// (OverlayCompositor::CollectGpuLayers) and blended onto a transparent canvas (utils/overlay_canvas.h, unit-tested).
//
// The pipe itself (server end) is created by ffmpeg_runner.cpp BEFORE ffmpeg is launched; hr_ff_take_overlay_pipe()
// hands the HANDLE over to hr_ovf_create().
//
// Pacing (checked against a real ffmpeg in a simulation, see CHANGELOG): the feeder stays ONE picture ahead of real
// time, so ffmpeg's overlay filter never has to wait for the next picture; if the feeder falls behind it catches up
// without sleeping; if it dies ffmpeg keeps the last picture (eof_action=pass, repeatlast=1) and the recording goes on.
//
// Threading: one thread does all the blocking I/O and is the only user of the OverlayCompositor. hr_ovf_set_overlays()
// only stores a copy of the list. A blocked ConnectNamedPipe / WriteFile is interrupted with CancelSynchronousIo().

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>

  #include <atomic>
  #include <chrono>
  #include <mutex>
  #include <thread>
  #include <vector>

  #include "gpu_overlay.h"
  #include "overlay_render.h"
  #include "../utils/log.h"
  #include "../utils/overlay_canvas.h"

namespace {

struct Feed {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    int    w = 0, h = 0, fps = 15;

    std::mutex                 mu;
    std::vector<HrOverlayDesc> descs;            // lock: mu - the list the next picture is drawn from
    bool                       descs_changed = true;

    std::thread                th;
    std::atomic<bool>          stop{false};
    std::atomic<bool>          done{false};      // the thread has left
    std::atomic<int>           state{0};         // 0 waiting for ffmpeg to open the pipe, 1 feeding, -1 ended
    std::atomic<long long>     pictures{0};
    std::atomic<double>        cur_fps{0.0};     // the recording's real FPS (text overlays can show it)
    // Real Win32 handle of the thread (std::thread::native_handle() is a pthread_t with MinGW's posix threads);
    // set by the thread itself first thing, used for CancelSynchronousIo().
    std::atomic<HANDLE>        thread_handle{nullptr};

    OverlayCompositor          comp;             // touched by the feeder thread only

    void cancel_io() {
        HANDLE t = thread_handle.load();
        if (t && !done.load()) CancelSynchronousIo(t);
    }

    bool write_all(const uint8_t *p, size_t left) {
        while (left > 0) {
            DWORD wr = 0;
            const DWORD want = (DWORD)(left > (1u << 20) ? (1u << 20) : left);
            if (!WriteFile(pipe, p, want, &wr, nullptr) || wr == 0) return false;
            p += wr;
            left -= wr;
        }
        return true;
    }

    void run() {
        {
            HANDLE dup = nullptr;
            if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &dup, 0, FALSE,
                                DUPLICATE_SAME_ACCESS))
                thread_handle.store(dup);
        }
        // ---- wait for ffmpeg to open its end --------------------------------------------------------------
        BOOL ok = ConnectNamedPipe(pipe, nullptr);
        if (!ok && GetLastError() == ERROR_PIPE_CONNECTED) ok = TRUE;      // ffmpeg was faster than us
        if (!ok || stop.load()) {
            state.store(-1);
            done.store(true);
            return;
        }
        state.store(1);

        // ---- stream ------------------------------------------------------------------------------------
        using clk = std::chrono::steady_clock;
        const auto period = std::chrono::microseconds(1000000 / (fps > 0 ? fps : 15));
        const auto t0 = clk::now();
        std::vector<uint8_t>       canvas((size_t)w * (size_t)h * 4, 0);
        std::vector<HrOverlayDesc> local;
        std::vector<HrGpuLayer>    layers;
        // What the canvas currently shows, so an unchanged overlay set is not re-drawn (only re-sent).
        struct Sig { int key; unsigned long long gen; int x, y, w, h, op; };
        std::vector<Sig> sig;
        bool have_canvas = false;
        long long n = 0;
        for (;;) {
            if (stop.load()) break;
            // Picture n is due at t0 + (n-1)*period: pictures 0 and 1 go out at once, then one per period -
            // always one picture ahead of real time.
            const auto due = t0 + period * (n > 0 ? n - 1 : 0);
            for (;;) {
                const auto now = clk::now();
                if (now >= due || stop.load()) break;
                auto d = due - now;
                if (d > std::chrono::milliseconds(20)) d = std::chrono::milliseconds(20);
                std::this_thread::sleep_for(d);
            }
            if (stop.load()) break;

            bool changed;
            {
                std::lock_guard<std::mutex> lk(mu);
                changed = descs_changed;
                if (changed) { local = descs; descs_changed = false; }
            }
            // Layers are re-collected every picture (a gif / webcam changes the bitmap by itself); the canvas is
            // re-drawn only when something in the signature differs.
            comp.CollectGpuLayers(local, cur_fps.load(std::memory_order_relaxed), layers);
            std::vector<Sig> now_sig;
            now_sig.reserve(layers.size());
            for (const auto &L : layers) now_sig.push_back({L.key, L.gen, L.dst_x, L.dst_y, L.dst_w, L.dst_h, L.opacity});
            bool same = have_canvas && now_sig.size() == sig.size();
            for (size_t i = 0; same && i < sig.size(); ++i)
                same = sig[i].key == now_sig[i].key && sig[i].gen == now_sig[i].gen && sig[i].x == now_sig[i].x &&
                       sig[i].y == now_sig[i].y && sig[i].w == now_sig[i].w && sig[i].h == now_sig[i].h &&
                       sig[i].op == now_sig[i].op;
            if (!same) {
                hr_canvas_clear(canvas.data(), w, h);
                for (const auto &L : layers) {
                    HrCanvasLayer c{L.bgra, L.w, L.h, L.dst_x, L.dst_y, L.dst_w, L.dst_h, L.opacity};
                    hr_canvas_blend(canvas.data(), w, h, c);
                }
                sig.swap(now_sig);
                have_canvas = true;
            }
            if (!write_all(canvas.data(), canvas.size())) break;     // ffmpeg went away (or Cancel on stop)
            ++n;
            pictures.store(n, std::memory_order_relaxed);
        }
        state.store(-1);
        done.store(true);
    }
};

} // namespace

extern "C" {

// Takes ownership of the pipe's server HANDLE. Returns nullptr on failure (the handle is then NOT consumed).
void *hr_ovf_create(intptr_t pipe_handle, int w, int h, int fps) {
    if (pipe_handle == 0 || pipe_handle == -1 || w <= 0 || h <= 0) return nullptr;
    try {
        auto *f = new Feed();
        f->pipe = reinterpret_cast<HANDLE>(pipe_handle);
        f->w = w;
        f->h = h;
        f->fps = (fps >= 1 && fps <= 60) ? fps : 15;
        f->th = std::thread([f]() { f->run(); });
        return f;
    } catch (...) {
        return nullptr;
    }
}

// Replaces the overlay list the pictures are drawn from (same list the capture pipeline gets, see SyncOverlays()).
void hr_ovf_set_overlays(void *h, const HrOverlayDesc *items, int count) {
    if (!h) return;
    auto *f = static_cast<Feed *>(h);
    std::lock_guard<std::mutex> lk(f->mu);
    f->descs.assign(items, items + (items && count > 0 ? count : 0));
    f->descs_changed = true;
}

// The recording's current FPS, for text overlays that show it (PollStats pushes it about once a second).
void hr_ovf_set_current_fps(void *h, double fps) {
    if (h) static_cast<Feed *>(h)->cur_fps.store(fps, std::memory_order_relaxed);
}

// 0 = waiting for ffmpeg to open the pipe, 1 = feeding, -1 = ended (ffmpeg closed the pipe / error / stopped).
int hr_ovf_state(const void *h) {
    return h ? static_cast<const Feed *>(h)->state.load() : -1;
}

long long hr_ovf_pictures(const void *h) {
    return h ? static_cast<const Feed *>(h)->pictures.load() : 0;
}

// Stops the thread (interrupting a blocked pipe call) and closes the pipe.
void hr_ovf_destroy(void *h) {
    if (!h) return;
    auto *f = static_cast<Feed *>(h);
    f->stop.store(true);
    // The thread may not have reached its blocking call yet when the first cancel arrives: repeat until it is out.
    for (int i = 0; i < 300 && !f->done.load(); ++i) {
        f->cancel_io();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (f->th.joinable()) f->th.join();
    HANDLE t = f->thread_handle.exchange(nullptr);
    if (t) CloseHandle(t);
    if (f->pipe != INVALID_HANDLE_VALUE) CloseHandle(f->pipe);
    delete f;
}

} // extern "C"

#endif // _WIN32
