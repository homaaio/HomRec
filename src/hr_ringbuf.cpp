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

/* BUG FIX: `restrict` is C99 only; use compiler-specific spelling in C++ */
#if defined(__GNUC__) || defined(__clang__)
  #define HR_RESTRICT __restrict__
#elif defined(_MSC_VER)
  #define HR_RESTRICT __restrict
#else
  #define HR_RESTRICT
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

HR_EXPORT size_t hr_rb_write(void *handle, const uint8_t * HR_RESTRICT data, size_t nbytes) {
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

HR_EXPORT size_t hr_rb_read(void *handle, uint8_t * HR_RESTRICT dst, size_t nbytes) {
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

/*
 * BUG FIX: previous version only zeroed the cursors but left stale PCM data
 * in the buffer. A consumer racing with reset could read garbage from the
 * previous recording session. Now the buffer contents are explicitly cleared.
 * seq_cst ensures both producer and consumer see the reset immediately.
 */
HR_EXPORT void hr_rb_reset(void *handle) {
    if (!handle) return;
    auto *rb = static_cast<RingBuf *>(handle);
    rb->head.store(0, std::memory_order_seq_cst);
    rb->tail.store(0, std::memory_order_seq_cst);
    memset(rb->buf.data(), 0, rb->cap);
}
