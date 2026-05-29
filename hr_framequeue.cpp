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
