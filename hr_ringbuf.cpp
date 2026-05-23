/*
 * hr_ringbuf.cpp  -  HomRec v1.5.0  lock-free audio ring-buffer
 *
 * A single-producer / single-consumer (SPSC) ring buffer for raw PCM chunks.
 * The audio capture thread writes; the encoder flush reads - no mutex needed.
 *
 * Exported C API (callable from Python ctypes):
 *
 *   hr_rb_create(capacity_bytes)  -> handle (void*)
 *   hr_rb_destroy(handle)
 *   hr_rb_write(handle, data, nbytes) -> bytes actually written (0 if full)
 *   hr_rb_read (handle, dst,  nbytes) -> bytes actually read   (0 if empty)
 *   hr_rb_available_read (handle) -> bytes ready to consume
 *   hr_rb_available_write(handle) -> free space
 *   hr_rb_reset(handle)           -> clear buffer (call before new recording)
 *
 * BUG FIXES vs previous version:
 *   - available_read() used mismatched memory orders; fixed to acquire/acquire.
 *   - hr_rb_reset() now uses seq_cst to ensure visibility across both threads.
 *   - Write/read wrap-around memcpy path had an off-by-one for exact-power-of-2
 *     boundaries; fixed by computing `first` from mask(h) not raw h.
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

/* -------------------------------------------------------------------------
 * Internal state
 * cap must be a power of two so that (idx & (cap-1)) replaces (idx % cap).
 * ---------------------------------------------------------------------- */
struct RingBuf {
    std::vector<uint8_t>  buf;
    size_t                cap;         /* always a power of two */
    std::atomic<size_t>   head{0};     /* write cursor (ever-increasing) */
    std::atomic<size_t>   tail{0};     /* read  cursor (ever-increasing) */

    explicit RingBuf(size_t capacity) {
        /* Round up to next power of two, minimum 4096 bytes */
        size_t c = 4096;
        while (c < capacity) c <<= 1;
        cap = c;
        buf.resize(cap, 0);
    }

    inline size_t mask(size_t i) const noexcept { return i & (cap - 1u); }

    /* BUG FIX: both loads now use acquire semantics for correct SPSC ordering */
    size_t available_read() const noexcept {
        return head.load(std::memory_order_acquire)
             - tail.load(std::memory_order_acquire);
    }
    size_t available_write() const noexcept {
        return cap - available_read();
    }
};

HR_EXPORT void *hr_rb_create(size_t capacity_bytes) {
    try {
        return new RingBuf(capacity_bytes ? capacity_bytes : 65536u);
    } catch (...) {
        return nullptr;
    }
}

HR_EXPORT void hr_rb_destroy(void *handle) {
    delete static_cast<RingBuf *>(handle);
}

HR_EXPORT size_t hr_rb_write(void *handle, const uint8_t *data, size_t nbytes) {
    if (!handle || !data || nbytes == 0) return 0;
    auto *rb = static_cast<RingBuf *>(handle);

    size_t avail = rb->available_write();
    if (avail == 0) return 0;

    size_t n = (nbytes < avail) ? nbytes : avail;
    size_t h = rb->head.load(std::memory_order_relaxed);

    /* BUG FIX: compute contiguous space from masked position, not raw cursor */
    size_t mh    = rb->mask(h);
    size_t first = rb->cap - mh;    /* bytes from mh to end of buffer */

    if (first >= n) {
        memcpy(rb->buf.data() + mh, data, n);
    } else {
        memcpy(rb->buf.data() + mh, data,         first);
        memcpy(rb->buf.data(),      data + first,  n - first);
    }

    rb->head.store(h + n, std::memory_order_release);
    return n;
}

HR_EXPORT size_t hr_rb_read(void *handle, uint8_t *dst, size_t nbytes) {
    if (!handle || !dst || nbytes == 0) return 0;
    auto *rb = static_cast<RingBuf *>(handle);

    size_t avail = rb->available_read();
    if (avail == 0) return 0;

    size_t n = (nbytes < avail) ? nbytes : avail;
    size_t t = rb->tail.load(std::memory_order_relaxed);

    /* BUG FIX: same wrap-around fix as in write */
    size_t mt    = rb->mask(t);
    size_t first = rb->cap - mt;

    if (first >= n) {
        memcpy(dst,         rb->buf.data() + mt, n);
    } else {
        memcpy(dst,         rb->buf.data() + mt, first);
        memcpy(dst + first, rb->buf.data(),       n - first);
    }

    rb->tail.store(t + n, std::memory_order_release);
    return n;
}

HR_EXPORT size_t hr_rb_available_read(void *handle) {
    if (!handle) return 0;
    return static_cast<RingBuf *>(handle)->available_read();
}

HR_EXPORT size_t hr_rb_available_write(void *handle) {
    if (!handle) return 0;
    return static_cast<RingBuf *>(handle)->available_write();
}

/* BUG FIX: use seq_cst to guarantee both producer and consumer see the reset */
HR_EXPORT void hr_rb_reset(void *handle) {
    if (!handle) return;
    auto *rb = static_cast<RingBuf *>(handle);
    rb->head.store(0, std::memory_order_seq_cst);
    rb->tail.store(0, std::memory_order_seq_cst);
}
