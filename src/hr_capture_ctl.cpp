/*
 * hr_capture_ctl.cpp  —  HomRec capture session controller  (v1.6.1)
 *
 * Replaces the recording state-machine in homrec.py:
 *   start_recording() / stop_recording() / toggle_pause() and the
 *   per-frame elapsed-time + file-size accounting.
 *
 * The Python UI layer calls the C API; the heavy capture/encode work is
 * delegated to hr_pipeline.dll (already exists).
 *
 * Build (MinGW-w64):
 *   g++ -O2 -std=c++17 -shared -static-libgcc -static-libstdc++ ^
 *       -o hr_capture_ctl.dll hr_capture_ctl.cpp -lwinmm
 */

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #define HR_EXPORT extern "C" __declspec(dllexport)
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
  #include <ctime>
  #include <unistd.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <mutex>
#include <string>

/* ── Recording states ────────────────────────────────────────────────────── */
enum HrState : int {
    HR_STATE_IDLE      = 0,
    HR_STATE_RECORDING = 1,
    HR_STATE_PAUSED    = 2,
};

/* ── Callbacks supplied by Python ────────────────────────────────────────── */
typedef void (*HR_STATE_CB)(int state);          /* called on state change    */
typedef void (*HR_STATS_CB)(double elapsed_sec,  /* called every ~500 ms      */
                             double file_size_mb,
                             int    frame_count);

/* ── Session context ─────────────────────────────────────────────────────── */
struct CaptureSession {
    std::atomic<HrState>  state{HR_STATE_IDLE};

    /* timing */
    int64_t   start_ns{0};       /* wall-clock ns when recording began      */
    int64_t   pause_accum_ns{0}; /* total ns spent in PAUSED state          */
    int64_t   pause_enter_ns{0}; /* ns when last pause began (0 = not paused) */

    /* stats */
    std::atomic<int>   frame_count{0};
    std::atomic<int64_t> file_bytes{0};  /* updated by hr_ctl_update_stats() */

    /* output path (UTF-8) */
    char output_path[512]{};

    /* callbacks */
    HR_STATE_CB state_cb{nullptr};
    HR_STATS_CB stats_cb{nullptr};

    std::mutex mu;

    /* ── helpers ── */
    int64_t _now_ns() const {
#ifdef _WIN32
        LARGE_INTEGER cnt, freq;
        QueryPerformanceCounter(&cnt);
        QueryPerformanceFrequency(&freq);
        return (int64_t)((__int128)cnt.QuadPart * 1000000000LL / freq.QuadPart);
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
    }

    double elapsed_sec() const {
        if (state == HR_STATE_IDLE) return 0.0;
        int64_t now = _now_ns();
        int64_t run = (now - start_ns) - pause_accum_ns;
        if (state == HR_STATE_PAUSED && pause_enter_ns > 0)
            run -= (now - pause_enter_ns);
        if (run < 0) run = 0;
        return (double)run / 1e9;
    }
};

/* ── Public API ───────────────────────────────────────────────────────────── */

HR_EXPORT void *hr_ctl_create() {
    return new(std::nothrow) CaptureSession{};
}

HR_EXPORT void hr_ctl_destroy(void *handle) {
    delete static_cast<CaptureSession *>(handle);
}

HR_EXPORT void hr_ctl_set_callbacks(void *handle,
                                     HR_STATE_CB state_cb,
                                     HR_STATS_CB stats_cb) {
    if (!handle) return;
    auto *s = static_cast<CaptureSession *>(handle);
    std::lock_guard<std::mutex> lk(s->mu);
    s->state_cb = state_cb;
    s->stats_cb = stats_cb;
}

HR_EXPORT void hr_ctl_set_output_path(void *handle, const char *path) {
    if (!handle || !path) return;
    auto *s = static_cast<CaptureSession *>(handle);
    strncpy(s->output_path, path, sizeof(s->output_path) - 1);
}

/*
 * hr_ctl_start
 * Transitions IDLE → RECORDING.
 * Returns 1 on success, 0 if already recording / paused.
 */
HR_EXPORT int hr_ctl_start(void *handle) {
    if (!handle) return 0;
    auto *s = static_cast<CaptureSession *>(handle);
    std::lock_guard<std::mutex> lk(s->mu);
    if (s->state != HR_STATE_IDLE) return 0;

    s->start_ns        = s->_now_ns();
    s->pause_accum_ns  = 0;
    s->pause_enter_ns  = 0;
    s->frame_count     = 0;
    s->file_bytes      = 0;
    s->state           = HR_STATE_RECORDING;

    if (s->state_cb) s->state_cb((int)HR_STATE_RECORDING);
    return 1;
}

/*
 * hr_ctl_stop
 * Transitions RECORDING / PAUSED → IDLE.
 * Returns elapsed seconds for final summary.
 */
HR_EXPORT double hr_ctl_stop(void *handle) {
    if (!handle) return 0.0;
    auto *s = static_cast<CaptureSession *>(handle);
    std::lock_guard<std::mutex> lk(s->mu);
    if (s->state == HR_STATE_IDLE) return 0.0;

    double elapsed = s->elapsed_sec();
    s->state = HR_STATE_IDLE;

    if (s->state_cb) s->state_cb((int)HR_STATE_IDLE);
    return elapsed;
}

/*
 * hr_ctl_pause_toggle
 * Toggles between RECORDING ↔ PAUSED.
 * Returns new state (HR_STATE_RECORDING or HR_STATE_PAUSED), or -1 if IDLE.
 */
HR_EXPORT int hr_ctl_pause_toggle(void *handle) {
    if (!handle) return -1;
    auto *s = static_cast<CaptureSession *>(handle);
    std::lock_guard<std::mutex> lk(s->mu);

    if (s->state == HR_STATE_RECORDING) {
        s->pause_enter_ns = s->_now_ns();
        s->state = HR_STATE_PAUSED;
        if (s->state_cb) s->state_cb((int)HR_STATE_PAUSED);
        return (int)HR_STATE_PAUSED;
    }
    if (s->state == HR_STATE_PAUSED) {
        int64_t now = s->_now_ns();
        if (s->pause_enter_ns > 0)
            s->pause_accum_ns += now - s->pause_enter_ns;
        s->pause_enter_ns = 0;
        s->state = HR_STATE_RECORDING;
        if (s->state_cb) s->state_cb((int)HR_STATE_RECORDING);
        return (int)HR_STATE_RECORDING;
    }
    return -1;
}

/*
 * hr_ctl_get_state
 * Returns 0=IDLE, 1=RECORDING, 2=PAUSED.
 */
HR_EXPORT int hr_ctl_get_state(const void *handle) {
    if (!handle) return (int)HR_STATE_IDLE;
    return (int)static_cast<const CaptureSession *>(handle)->state.load();
}

/*
 * hr_ctl_get_elapsed_sec
 * Returns wall-clock seconds of active (non-paused) recording.
 */
HR_EXPORT double hr_ctl_get_elapsed_sec(const void *handle) {
    if (!handle) return 0.0;
    return static_cast<const CaptureSession *>(handle)->elapsed_sec();
}

/*
 * hr_ctl_inc_frame
 * Called by the capture thread after each frame is written.
 */
HR_EXPORT void hr_ctl_inc_frame(void *handle) {
    if (!handle) return;
    static_cast<CaptureSession *>(handle)->frame_count.fetch_add(1, std::memory_order_relaxed);
}

HR_EXPORT int hr_ctl_get_frame_count(const void *handle) {
    if (!handle) return 0;
    return static_cast<const CaptureSession *>(handle)->frame_count.load();
}

/*
 * hr_ctl_update_stats
 * Python bridge calls this with the current output file size (bytes)
 * and the library emits the stats_cb if registered.
 */
HR_EXPORT void hr_ctl_update_stats(void *handle, int64_t file_bytes) {
    if (!handle) return;
    auto *s = static_cast<CaptureSession *>(handle);
    s->file_bytes.store(file_bytes, std::memory_order_relaxed);
    if (s->stats_cb) {
        double elapsed = s->elapsed_sec();
        double mb      = (double)file_bytes / (1024.0 * 1024.0);
        s->stats_cb(elapsed, mb, s->frame_count.load());
    }
}

/*
 * hr_ctl_format_elapsed
 * Writes "HH:MM:SS" into buf.  Returns characters written.
 */
HR_EXPORT int hr_ctl_format_elapsed(const void *handle, char *buf, int buf_len) {
    if (!handle || !buf || buf_len < 9) return 0;
    double sec = static_cast<const CaptureSession *>(handle)->elapsed_sec();
    int total = (int)sec;
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int ss = total % 60;
    return snprintf(buf, (size_t)buf_len, "%02d:%02d:%02d", h, m, ss);
}
