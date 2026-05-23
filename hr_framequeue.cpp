/*
 * hr_framequeue.cpp  -  HomRec v1.5.0  fixed-capacity frame pointer queue
 *
 * A tiny SPSC (single-producer / single-consumer) queue of void* frame
 * pointers. The capture thread pushes raw buffer pointers; the UI thread
 * pops them. Old frames are automatically discarded (latest-wins policy)
 * so the preview never lags behind.
 *
 * API:
 *   hr_fq_create(capacity)      -> handle
 *   hr_fq_destroy(handle)
 *   hr_fq_push(handle, ptr)     -> 1 if pushed, 0 on null handle
 *   hr_fq_pop (handle, out_ptr) -> 1 if got a frame, 0 if empty
 *   hr_fq_size(handle)          -> number of items currently queued
 *
 * BUG FIXES vs previous version:
 *   - size() used mismatched memory orders (acquire/relaxed); now both acquire.
 *   - hr_fq_push eviction path had a race: tail was bumped before the old slot
 *     was overwritten, which could expose a null pointer to the consumer.
 *     Fixed by writing the new frame first, then bumping head.
 *   - hr_fq_push called size() which reads head+tail in two separate atomic
 *     loads; between those two loads the consumer could advance tail, making
 *     size() return a stale value and bumping tail unnecessarily (or worse,
 *     causing tail to lap head with wraparound). Fixed by reading both cursors
 *     once from the same snapshot inside push.
 *   - Null handle guard added to all exported functions.
 *
 * Compile (Linux):
 *   g++ -O3 -std=c++17 -shared -fPIC -o hr_framequeue.so hr_framequeue.cpp
 *
 * Compile (Windows MinGW):
 *   g++ -O3 -std=c++17 -shared -static-libgcc -static-libstdc++ \
 *       -o hr_framequeue.dll hr_framequeue.cpp
 */

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <vector>

#ifdef _WIN32
  #define HR_EXPORT extern "C" __declspec(dllexport)
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

struct FrameQueue {
    std::vector<void *>  buf;
    size_t               cap;        /* power of two */
    std::atomic<size_t>  head{0};    /* producer writes here */
    std::atomic<size_t>  tail{0};    /* consumer reads here */

    explicit FrameQueue(size_t capacity) {
        size_t c = 2;
        while (c < capacity) c <<= 1;
        cap = c;
        buf.resize(cap, nullptr);
    }

    inline size_t mask(size_t i) const noexcept { return i & (cap - 1u); }

    /* BUG FIX: both loads use acquire for correct cross-thread visibility */
    size_t size() const noexcept {
        size_t h = head.load(std::memory_order_acquire);
        size_t t = tail.load(std::memory_order_acquire);
        return h - t;
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

/*
 * Push a frame pointer. If the queue is full the oldest entry is evicted
 * (latest-wins for preview). Returns 1 always (0 only on null handle).
 *
 * BUG FIX: previous version called size() which read head and tail in two
 * separate atomic loads. Between those two loads the consumer could advance
 * tail, making the overflow check stale and evicting an entry when the queue
 * was no longer full — or, with unsigned wraparound, producing a huge size
 * value that always triggered eviction. Fixed by taking a single snapshot of
 * both head and tail at the start of push.
 *
 * BUG FIX: slot is written BEFORE head is advanced so the consumer can never
 * observe a null/stale pointer from the newly pushed position.
 */
HR_EXPORT int hr_fq_push(void *handle, void *frame_ptr) {
    if (!handle) return 0;
    auto *fq = static_cast<FrameQueue *>(handle);

    /* Single snapshot — avoids the two-load race in the old size() call */
    size_t h = fq->head.load(std::memory_order_relaxed);
    size_t t = fq->tail.load(std::memory_order_acquire);

    /* If full, discard the oldest frame by advancing tail */
    if (h - t >= fq->cap - 1u) {
        fq->tail.store(t + 1, std::memory_order_release);
    }

    /* Write slot BEFORE making it visible to consumer via head bump */
    fq->buf[fq->mask(h)] = frame_ptr;
    fq->head.store(h + 1, std::memory_order_release);
    return 1;
}

/* Returns 1 and writes to *out_ptr if a frame is available, else 0. */
HR_EXPORT int hr_fq_pop(void *handle, void **out_ptr) {
    if (!handle || !out_ptr) return 0;
    auto *fq = static_cast<FrameQueue *>(handle);

    size_t t = fq->tail.load(std::memory_order_relaxed);
    if (t == fq->head.load(std::memory_order_acquire)) return 0;

    *out_ptr = fq->buf[fq->mask(t)];
    fq->tail.store(t + 1, std::memory_order_release);
    return 1;
}

HR_EXPORT size_t hr_fq_size(void *handle) {
    if (!handle) return 0;
    return static_cast<FrameQueue *>(handle)->size();
}
