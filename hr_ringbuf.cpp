/*
 * hr_ringbuf.cpp  —  HomRec v1.5.0  lock-free audio ring-buffer
 *
 * A single-producer / single-consumer (SPSC) ring buffer for raw PCM chunks.
 * The audio capture thread writes; the encoder flush reads — no mutex needed.
 *
 * Exported C API (callable from Python ctypes):
 *
 *   hr_rb_create(capacity_bytes)  → handle (void*)
 *   hr_rb_destroy(handle)
 *   hr_rb_write(handle, data, nbytes) → bytes actually written (0 if full)
 *   hr_rb_read (handle, dst,  nbytes) → bytes actually read   (0 if empty)
 *   hr_rb_available_read (handle) → bytes ready to consume
 *   hr_rb_available_write(handle) → free space
 *   hr_rb_reset(handle)           → clear buffer (call before new recording)
 *
 * Compile (Linux):
 *   g++ -O3 -std=c++17 -shared -fPIC -o hr_ringbuf.so hr_ringbuf.cpp
 *
 * Compile (Windows MinGW):
 *   g++ -O3 -std=c++17 -shared -o hr_ringbuf.dll hr_ringbuf.cpp
 */

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <atomic>
#include <vector>
#include <new>

#ifdef _WIN32
  #define HR_EXPORT extern "C" __declspec(dllexport)
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

struct RingBuf {
    std::vector<uint8_t>   buf;
    size_t                 cap;          /* must be power of two for mask trick */
    std::atomic<size_t>    head{0};      /* write cursor */
    std::atomic<size_t>    tail{0};      /* read  cursor */

    explicit RingBuf(size_t capacity) {
        /* round up to next power of two */
        size_t c = 1;
        while (c < capacity) c <<= 1;
        cap = c;
        buf.resize(cap, 0);
    }

    inline size_t mask(size_t idx) const { return idx & (cap - 1); }

    size_t available_read()  const {
        return head.load(std::memory_order_acquire)
             - tail.load(std::memory_order_relaxed);
    }
    size_t available_write() const {
        return cap - available_read();
    }
};

HR_EXPORT void *hr_rb_create(size_t capacity_bytes) {
    try {
        return new RingBuf(capacity_bytes ? capacity_bytes : 65536);
    } catch (...) { return nullptr; }
}

HR_EXPORT void hr_rb_destroy(void *handle) {
    delete static_cast<RingBuf *>(handle);
}

HR_EXPORT size_t hr_rb_write(void *handle, const uint8_t *data, size_t nbytes) {
    auto *rb = static_cast<RingBuf *>(handle);
    size_t avail = rb->available_write();
    if (avail == 0 || nbytes == 0) return 0;
    size_t n = (nbytes < avail) ? nbytes : avail;

    size_t h = rb->head.load(std::memory_order_relaxed);
    size_t first = rb->cap - rb->mask(h);           /* contiguous bytes till wrap */
    if (first >= n) {
        memcpy(rb->buf.data() + rb->mask(h), data, n);
    } else {
        memcpy(rb->buf.data() + rb->mask(h), data,          first);
        memcpy(rb->buf.data(),               data + first,  n - first);
    }
    rb->head.store(h + n, std::memory_order_release);
    return n;
}

HR_EXPORT size_t hr_rb_read(void *handle, uint8_t *dst, size_t nbytes) {
    auto *rb = static_cast<RingBuf *>(handle);
    size_t avail = rb->available_read();
    if (avail == 0 || nbytes == 0) return 0;
    size_t n = (nbytes < avail) ? nbytes : avail;

    size_t t = rb->tail.load(std::memory_order_relaxed);
    size_t first = rb->cap - rb->mask(t);
    if (first >= n) {
        memcpy(dst,         rb->buf.data() + rb->mask(t), n);
    } else {
        memcpy(dst,         rb->buf.data() + rb->mask(t), first);
        memcpy(dst + first, rb->buf.data(),                n - first);
    }
    rb->tail.store(t + n, std::memory_order_release);
    return n;
}

HR_EXPORT size_t hr_rb_available_read(void *handle) {
    return static_cast<RingBuf *>(handle)->available_read();
}

HR_EXPORT size_t hr_rb_available_write(void *handle) {
    return static_cast<RingBuf *>(handle)->available_write();
}

HR_EXPORT void hr_rb_reset(void *handle) {
    auto *rb = static_cast<RingBuf *>(handle);
    rb->head.store(0, std::memory_order_relaxed);
    rb->tail.store(0, std::memory_order_relaxed);
}
