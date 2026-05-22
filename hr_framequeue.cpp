/*
 * hr_framequeue.cpp  —  HomRec v1.5.0  fixed-capacity frame pointer queue
 *
 * A tiny SPSC (single-producer / single-consumer) queue of void* frame
 * pointers.  The capture thread pushes raw buffer pointers; the UI thread
 * pops them.  Old frames are automatically discarded (latest-wins policy)
 * so the preview never lags behind.
 *
 * API:
 *   hr_fq_create(capacity)  → handle
 *   hr_fq_destroy(handle)
 *   hr_fq_push(handle, ptr)  → 1 if pushed, 0 if full (old frame evicted)
 *   hr_fq_pop (handle, out_ptr) → 1 if got a frame, 0 if empty
 *   hr_fq_size(handle)       → number of items currently queued
 *
 * Compile (Linux):
 *   g++ -O3 -std=c++17 -shared -fPIC -o hr_framequeue.so hr_framequeue.cpp
 *
 * Compile (Windows MinGW):
 *   g++ -O3 -std=c++17 -shared -o hr_framequeue.dll hr_framequeue.cpp
 */

#include <cstddef>
#include <atomic>
#include <vector>
#include <cstdint>

#ifdef _WIN32
  #define HR_EXPORT extern "C" __declspec(dllexport)
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

struct FrameQueue {
    std::vector<void*>   buf;
    size_t               cap;
    std::atomic<size_t>  head{0};
    std::atomic<size_t>  tail{0};

    explicit FrameQueue(size_t capacity) {
        size_t c = 1;
        while (c < capacity) c <<= 1;
        cap = c;
        buf.resize(cap, nullptr);
    }

    size_t mask(size_t i) const { return i & (cap - 1); }
    size_t size() const {
        return head.load(std::memory_order_acquire)
             - tail.load(std::memory_order_relaxed);
    }
};

HR_EXPORT void *hr_fq_create(size_t capacity) {
    if (capacity < 2) capacity = 2;
    try { return new FrameQueue(capacity); }
    catch (...) { return nullptr; }
}

HR_EXPORT void hr_fq_destroy(void *handle) {
    delete static_cast<FrameQueue *>(handle);
}

/* Returns 1 always; if queue was full the oldest frame is evicted. */
HR_EXPORT int hr_fq_push(void *handle, void *frame_ptr) {
    auto *fq = static_cast<FrameQueue *>(handle);
    /* If full, advance tail (drop oldest) */
    if (fq->size() >= fq->cap - 1) {
        fq->tail.fetch_add(1, std::memory_order_acq_rel);
    }
    size_t h = fq->head.load(std::memory_order_relaxed);
    fq->buf[fq->mask(h)] = frame_ptr;
    fq->head.store(h + 1, std::memory_order_release);
    return 1;
}

/* Returns 1 and writes to *out_ptr if a frame is available, else 0. */
HR_EXPORT int hr_fq_pop(void *handle, void **out_ptr) {
    auto *fq = static_cast<FrameQueue *>(handle);
    size_t t = fq->tail.load(std::memory_order_relaxed);
    if (t == fq->head.load(std::memory_order_acquire)) return 0;
    *out_ptr = fq->buf[fq->mask(t)];
    fq->tail.store(t + 1, std::memory_order_release);
    return 1;
}

HR_EXPORT size_t hr_fq_size(void *handle) {
    return static_cast<FrameQueue *>(handle)->size();
}
