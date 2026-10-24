// pcm_ring.h - FIFO of interleaved int16 samples backed by one circular array.
//
// Replaces the std::vector<int16_t> the audio workers used to append to:
//   * vector::insert() at the end re-allocated and copied the WHOLE recording-so-far whenever
//     capacity ran out (hundreds of MB late in a long recording, under the mutex the flusher needs);
//   * the Instant Replay trim was vector::erase(begin, ...) = a memmove of the entire window;
//   * every flush swapped the vector out, so the next 10 ms chunk started again from capacity 0.
// Here the storage grows geometrically and is then kept (never shrinks), pushing is a memcpy into
// at most two spans, and dropping the oldest samples just moves the read index.
//
// Not thread-safe: the owner locks its own mutex around every call (as it did for the vector).
// Sizes are in int16 samples, not frames. All counts are kept even so stereo pairs never split.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

class PcmRing {
public:
    // 0 = unlimited (grows as needed). Otherwise the oldest samples are dropped so that
    // size() <= limit. Rounded down to an even count.
    void set_limit(size_t samples) {
        limit_ = samples & ~(size_t)1;
        enforce_limit();
    }
    size_t limit() const { return limit_; }

    size_t size()  const { return size_; }
    bool   empty() const { return size_ == 0; }

    // Forget the contents, keep the storage.
    void clear() { head_ = 0; size_ = 0; }

    void push(const int16_t *src, size_t n) {
        if (!src || n == 0) return;
        if (limit_ && n >= limit_) {            // the chunk alone fills the whole window
            src += (n - limit_);
            n = limit_;
            head_ = 0; size_ = 0;
        }
        if (limit_ && size_ + n > limit_) drop_oldest(size_ + n - limit_);   // make room first
        reserve_for(size_ + n);
        const size_t cap = buf_.size();
        size_t tail = head_ + size_;
        if (tail >= cap) tail -= cap;
        const size_t first = std::min(n, cap - tail);
        std::memcpy(buf_.data() + tail, src, first * sizeof(int16_t));
        if (n > first) std::memcpy(buf_.data(), src + first, (n - first) * sizeof(int16_t));
        size_ += n;
    }

    // Copy the newest min(max_samples, size()) samples (0 = everything) to `out`, oldest first.
    // `out` is resized to exactly that many samples (its capacity is kept). Nothing is removed.
    void copy_tail(std::vector<int16_t> &out, size_t max_samples = 0) const {
        size_t n = size_;
        if (max_samples && max_samples < n) n = max_samples & ~(size_t)1;
        out.resize(n);
        copy_range(out.data(), size_ - n, n);
    }

    // Move everything into `out` (resized to size(), capacity kept) and empty the ring.
    void drain(std::vector<int16_t> &out) {
        out.resize(size_);
        copy_range(out.data(), 0, size_);
        clear();
    }

private:
    void copy_range(int16_t *dst, size_t skip, size_t n) const {
        if (n == 0) return;
        const size_t cap = buf_.size();
        size_t from = head_ + skip;
        if (from >= cap) from -= cap;
        const size_t first = std::min(n, cap - from);
        std::memcpy(dst, buf_.data() + from, first * sizeof(int16_t));
        if (n > first) std::memcpy(dst + first, buf_.data(), (n - first) * sizeof(int16_t));
    }

    void drop_oldest(size_t n) {
        n = (n + 1) & ~(size_t)1;
        if (n > size_) n = size_;
        head_ += n;
        if (buf_.size() && head_ >= buf_.size()) head_ -= buf_.size();
        size_ -= n;
        if (size_ == 0) head_ = 0;
    }
    void enforce_limit() {
        if (limit_ && size_ > limit_) drop_oldest(size_ - limit_);
    }

    // Make the array at least `need` samples long (unwrapping the content to index 0 on a grow).
    void reserve_for(size_t need) {
        if (need <= buf_.size()) return;
        size_t cap = buf_.empty() ? (size_t)1 << 16 : buf_.size();
        while (cap < need) cap *= 2;
        if (limit_ && cap > limit_) cap = limit_;   // a capped window never needs more than its limit
        if (cap < need) cap = need;
        std::vector<int16_t> nb(cap);
        if (size_) copy_range(nb.data(), 0, size_);
        buf_.swap(nb);
        head_ = 0;
    }

    std::vector<int16_t> buf_;
    size_t head_  = 0;
    size_t size_  = 0;
    size_t limit_ = 0;
};
