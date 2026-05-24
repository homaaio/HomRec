/*
 * hr_stopwatch.cpp  -  HomRec v1.5.0  high-precision frame-pacing timer
 *
 * Provides nanosecond-resolution monotonic timers used by the capture loop
 * to maintain accurate frame intervals. Python's time.sleep() has ~15 ms
 * granularity on Windows; this module uses QueryPerformanceCounter (Win) or
 * clock_gettime(CLOCK_MONOTONIC) (POSIX) to achieve sub-millisecond accuracy.
 *
 * API:
 *   hr_sw_create()          -> handle (void*)
 *   hr_sw_destroy(handle)
 *   hr_sw_start(handle)     -> reset and start
 *   hr_sw_elapsed_ns(handle)-> nanoseconds since last start
 *   hr_sw_elapsed_ms(handle)-> milliseconds since last start (float)
 *   hr_sw_sleep_until_ns(handle, target_ns)
 *       Spin/sleep until elapsed_ns >= target_ns.
 *       Uses a hybrid: coarse sleep to ~2 ms before target, then spin.
 *       This gives accurate frame pacing with minimal CPU waste.
 *   hr_sw_now_ns()          -> absolute monotonic time in nanoseconds
 *
 * BUG PREVENTION:
 *   - On Windows, timeBeginPeriod(1) is called at create-time to enable 1 ms
 *     sleep resolution for the duration of the timer's life. Without this,
 *     Sleep() can overshoot by 14-15 ms, causing severe frame-rate jitter.
 *
 * Compile (Linux):
 *   g++ -O3 -std=c++17 -shared -fPIC -o hr_stopwatch.so hr_stopwatch.cpp -lrt
 *
 * Compile (Windows MinGW):
 *   g++ -O3 -std=c++17 -shared -static-libgcc -static-libstdc++ \
 *       -o hr_stopwatch.dll hr_stopwatch.cpp -lwinmm
 */

#include <cstdint>
#include <cstddef>
#include <cstring>

#ifdef _WIN32
  #define HR_EXPORT extern "C" __declspec(dllexport)
  #include <windows.h>
  #include <timeapi.h>          /* timeBeginPeriod / timeEndPeriod */
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
  #include <time.h>
  #include <unistd.h>
#endif

/* -------------------------------------------------------------------------
 * Platform-specific now()
 * ---------------------------------------------------------------------- */
static inline int64_t _hw_now_ns() {
#ifdef _WIN32
    LARGE_INTEGER cnt, freq;
    QueryPerformanceCounter(&cnt);
    QueryPerformanceFrequency(&freq);
    /* Avoid overflow: multiply first in 128-bit if possible */
    return (int64_t)(((__int128)cnt.QuadPart * 1'000'000'000LL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1'000'000'000LL + (int64_t)ts.tv_nsec;
#endif
}

static inline void _hw_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (ms % 1000) * 1'000'000L };
    nanosleep(&ts, nullptr);
#endif
}

/* -------------------------------------------------------------------------
 * Stopwatch state
 * ---------------------------------------------------------------------- */
struct Stopwatch {
    int64_t start_ns{0};

#ifdef _WIN32
    bool timer_period_set{false};
#endif

    Stopwatch() {
#ifdef _WIN32
        /* Enable 1 ms timer resolution */
        TIMECAPS tc;
        if (timeGetDevCaps(&tc, sizeof(tc)) == MMSYSERR_NOERROR) {
            UINT period = (tc.wPeriodMin < 1) ? 1 : tc.wPeriodMin;
            if (timeBeginPeriod(period) == TIMERR_NOERROR)
                timer_period_set = true;
        }
#endif
        start_ns = _hw_now_ns();
    }

    ~Stopwatch() {
#ifdef _WIN32
        if (timer_period_set) timeEndPeriod(1);
#endif
    }
};

HR_EXPORT void *hr_sw_create() {
    try { return new Stopwatch(); }
    catch (...) { return nullptr; }
}

HR_EXPORT void hr_sw_destroy(void *handle) {
    delete static_cast<Stopwatch *>(handle);
}

HR_EXPORT void hr_sw_start(void *handle) {
    if (!handle) return;
    static_cast<Stopwatch *>(handle)->start_ns = _hw_now_ns();
}

HR_EXPORT int64_t hr_sw_elapsed_ns(void *handle) {
    if (!handle) return 0;
    return _hw_now_ns() - static_cast<Stopwatch *>(handle)->start_ns;
}

HR_EXPORT double hr_sw_elapsed_ms(void *handle) {
    if (!handle) return 0.0;
    return (double)hr_sw_elapsed_ns(handle) / 1'000'000.0;
}

/*
 * Hybrid sleep-then-spin until elapsed >= target_ns.
 * Sleeps in 1 ms increments until ~2 ms before target, then spins.
 * This keeps CPU usage low while still being accurate to <100 µs.
 */
HR_EXPORT void hr_sw_sleep_until_ns(void *handle, int64_t target_ns) {
    if (!handle) return;
    Stopwatch *sw = static_cast<Stopwatch *>(handle);

    /* Coarse sleep phase: sleep while more than 2 ms remain */
    for (;;) {
        int64_t remaining = target_ns - (_hw_now_ns() - sw->start_ns);
        if (remaining <= 2'000'000LL) break;    /* 2 ms threshold */
        int sleep_ms = (int)((remaining - 2'000'000LL) / 1'000'000LL);
        if (sleep_ms < 1) sleep_ms = 1;
        _hw_sleep_ms(sleep_ms);
    }

    /* Spin phase: busy-wait for sub-ms precision */
    while ((_hw_now_ns() - sw->start_ns) < target_ns) {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("pause" ::: "memory");
#endif
    }
}

HR_EXPORT int64_t hr_sw_now_ns() {
    return _hw_now_ns();
}
