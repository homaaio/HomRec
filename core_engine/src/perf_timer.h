/*
 * perf_timer.h — HomRec 2.0
 * Lightweight QueryPerformanceCounter wrapper for Windows.
 *
 * Usage (C or C++):
 *
 *   PerfTimer t;
 *   perf_timer_start(&t);
 *   // ... work ...
 *   double ms = perf_timer_elapsed_ms(&t);
 *
 * Thread-safe: each PerfTimer is independent (no globals after init).
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <windows.h>

typedef struct {
    LARGE_INTEGER start;
    LARGE_INTEGER freq;
} PerfTimer;

static inline void perf_timer_start(PerfTimer* t) {
    QueryPerformanceFrequency(&t->freq);
    QueryPerformanceCounter(&t->start);
}

static inline double perf_timer_elapsed_ms(const PerfTimer* t) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - t->start.QuadPart)
         * 1000.0
         / (double)t->freq.QuadPart;
}

static inline double perf_timer_elapsed_us(const PerfTimer* t) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - t->start.QuadPart)
         * 1e6
         / (double)t->freq.QuadPart;
}

/* Restart the timer and return the elapsed milliseconds since start. */
static inline double perf_timer_lap_ms(PerfTimer* t) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (double)(now.QuadPart - t->start.QuadPart)
                   * 1000.0 / (double)t->freq.QuadPart;
    t->start = now;
    return elapsed;
}

#ifdef __cplusplus
}
#endif
