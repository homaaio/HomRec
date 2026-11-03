// pic_pool.h - converted YUV pictures shared between the capture thread and the writer thread, so that an unchanged
// screen costs no 3 MB copy and a partly changed one only copies the rows that differ.
//
//   * The capture thread owns the pool; the writer only holds read-only references (std::shared_ptr) to pictures
//     it was handed. A buffer is rewritten only when the pool is its sole owner, so a picture in flight never changes.
//   * Every buffer remembers which picture number it holds. Every transition (picture N-1 -> N) records the row
//     range that changed, so refreshing an older buffer to the current picture copies just the union of those ranges.
//   * Layout: planar I420 or NV12, even width and height. Row ranges are in luma rows, rounded to even.
//
// Typical use (capture thread):
//     pool.Reset(w, h, nv12);                       // when the output size or format changes
//     if (!pool.Has() || everything_changed) { auto b = pool.Full();  convert_all(*b);          pool.Done(b, 0, h); }
//     else if (rows_changed)                 { auto b = pool.Next(y0, y1); convert_rows(*b, y0, y1); pool.Done(b, y0, y1); }
//     send(pool.Cur());                             // shared, no copy
//     // private editable copy (cursor): auto c = pool.Copy(); draw(*c); pool.Taint(c, rows); send(c);   - never Done()
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

class PicPool {
public:
    using Buf = std::shared_ptr<std::vector<uint8_t>>;

    void Reset(int w, int h, bool nv12) {
        if (w == w_ && h == h_ && nv12 == nv12_) return;
        w_ = w; h_ = h; nv12_ = nv12;
        slots_.clear(); cur_.reset(); seq_ = 0;
        for (auto &r : hist_) r = Range{0, 0};
    }
    // Forget the current picture (the source changed in a way the row ranges can't describe). Buffers stay.
    void Drop() { cur_.reset(); }
    bool Has() const { return (bool)cur_; }
    const Buf &Cur() const { return cur_; }
    size_t Bytes() const { return bytes(); }

    // Idle buffer for a whole new picture (content undefined).
    Buf Full() {
        Slot *s = pick(false, 0, 0);
        if (!s) return make_temp();
        s->seq = 0;
        return s->buf;
    }

    // Idle buffer that equals Cur() except luma rows [y0,y1), which the caller overwrites before Done().
    Buf Next(int y0, int y1) {
        if (!cur_) return Full();
        norm(y0, y1);
        Slot *s = pick(true, y0, y1);
        if (!s) { Buf t = make_temp(); copy_rows(*t, *cur_, 0, h_); return t; }
        refresh(*s, y0, y1);
        return s->buf;
    }

    // Private buffer equal to Cur() (for drawing the cursor on). It is not a clean picture: never pass it to Done().
    // Call Taint() after drawing so the next Copy() only has to repair the rows that were drawn on.
    Buf Copy() {
        if (!cur_) return Full();
        Slot *s = pick(true, 0, 0);
        if (!s) { Buf t = make_temp(); copy_rows(*t, *cur_, 0, h_); return t; }
        refresh(*s, 0, 0);
        s->seq = 0;
        return s->buf;
    }

    // Luma rows [y0,y1) of `b` (from Copy()) were drawn on after it was copied from the current picture.
    void Taint(const Buf &b, int y0, int y1) {
        norm(y0, y1);
        for (auto &s : slots_) if (s.buf == b) { s.seq = seq_; s.t0 = y0; s.t1 = y1; return; }
    }

    // `b` (from Full()/Next()) is now the current picture; luma rows [y0,y1) differ from the previous one.
    void Done(const Buf &b, int y0, int y1) {
        norm(y0, y1);
        ++seq_;
        hist_[seq_ % kHist] = Range{y0, y1};
        for (auto &s : slots_) if (s.buf == b) { s.seq = seq_; s.t0 = s.t1 = 0; break; }
        cur_ = b;
    }

private:
    static constexpr size_t   kMaxSlots = 10;
    static constexpr uint64_t kHist     = 16;
    struct Range { int y0, y1; };
    struct Slot  { Buf buf; uint64_t seq = 0; int t0 = 0, t1 = 0; };   // t0..t1: rows drawn on after the copy

    int  w_ = 0, h_ = 0;
    bool nv12_ = false;
    Buf  cur_;
    uint64_t seq_ = 0;
    Range hist_[kHist] = {};
    std::vector<Slot> slots_;

    size_t bytes() const { return (size_t)w_ * h_ * 3 / 2; }
    void norm(int &y0, int &y1) const {
        y0 = std::max(0, y0 & ~1);
        y1 = std::min(h_, (y1 + 1) & ~1);
        if (y1 < y0) y1 = y0;
    }
    Buf make_temp() const { return std::make_shared<std::vector<uint8_t>>(bytes()); }

    // Rows that differ between the picture a slot holds and the current one: [lo,hi); whole frame when unknown.
    Range stale(const Slot &s) const {
        if (s.seq == 0 || seq_ - s.seq >= kHist) return Range{0, h_};
        Range r{h_, 0};
        if (s.t1 > s.t0) { r.y0 = s.t0; r.y1 = s.t1; }
        for (uint64_t q = s.seq + 1; q <= seq_; ++q) {
            const Range &h = hist_[q % kHist];
            if (h.y1 <= h.y0) continue;
            r.y0 = std::min(r.y0, h.y0); r.y1 = std::max(r.y1, h.y1);
        }
        if (r.y1 <= r.y0) r = Range{0, 0};
        return r;
    }

    // A slot nobody else holds. With `want_cur` the one needing the fewest rows copied is preferred.
    Slot *pick(bool want_cur, int ex0, int ex1) {
        Slot *best = nullptr; long best_cost = 0;
        for (auto &s : slots_) {
            if (s.buf.use_count() != 1) continue;           // the writer / the current picture still holds it
            long cost = 0;
            if (want_cur) {
                Range r = stale(s);
                cost = r.y1 - r.y0;
                cost -= std::max(0, std::min(r.y1, ex1) - std::max(r.y0, ex0));   // rows the caller rewrites anyway
            }
            if (!best || cost < best_cost) { best = &s; best_cost = cost; }
        }
        if (best) { std::atomic_thread_fence(std::memory_order_acquire); return best; }   // pairs with the writer's release
        if (slots_.size() < kMaxSlots) {
            slots_.push_back(Slot{make_temp(), 0});
            return &slots_.back();
        }
        return nullptr;                                      // everything is in flight: caller gets an unpooled buffer
    }

    void refresh(Slot &s, int ex0, int ex1) {
        Range r = stale(s);
        s.seq = 0; s.t0 = s.t1 = 0;                          // content is mixed until Done()/Taint()
        if (r.y1 <= r.y0) return;
        if (r.y0 < ex0) copy_rows(*s.buf, *cur_, r.y0, std::min(r.y1, ex0));
        if (r.y1 > ex1) copy_rows(*s.buf, *cur_, std::max(r.y0, ex1), r.y1);
    }

    // Copy luma rows [y0,y1) (even) of a picture: the Y rows and the matching chroma rows.
    void copy_rows(std::vector<uint8_t> &dstv, const std::vector<uint8_t> &srcv, int y0, int y1) const {
        if (y1 <= y0) return;
        uint8_t *dst = dstv.data(); const uint8_t *src = srcv.data();
        const size_t w = (size_t)w_;
        std::memcpy(dst + (size_t)y0 * w, src + (size_t)y0 * w, (size_t)(y1 - y0) * w);
        const size_t base = w * (size_t)h_;
        if (nv12_) {
            std::memcpy(dst + base + (size_t)(y0 / 2) * w, src + base + (size_t)(y0 / 2) * w, (size_t)((y1 - y0) / 2) * w);
        } else {
            const size_t cw = w / 2, plane = cw * (size_t)(h_ / 2);
            for (int pl = 0; pl < 2; ++pl) {
                const size_t off = base + (size_t)pl * plane + (size_t)(y0 / 2) * cw;
                std::memcpy(dst + off, src + off, (size_t)((y1 - y0) / 2) * cw);
            }
        }
    }
};

// Which luma rows [*d0,*d1) of an encoded picture change when rows [sy0,sy1) of the capture buffer change?
// The picture is the crop rectangle that starts at capture row `crop_y` and is `eff_h` rows high, scaled to `enc_h`
// rows (enc_h == eff_h: no vertical scaling). The result is even-aligned. Returns false when no row of the picture
// is affected. Scaled: output row y reads source rows around y*eff_h/enc_h (box average of a block, or the nearest
// row), so the range is widened by 2 rows on each side.
inline bool PicDirtyRows(int sy0, int sy1, int crop_y, int eff_h, int enc_h, int *d0, int *d1)
{
    *d0 = *d1 = 0;
    int r0 = std::max(sy0, crop_y) - crop_y;
    int r1 = std::min(sy1, crop_y + eff_h) - crop_y;
    if (r1 <= r0 || eff_h <= 0 || enc_h <= 0) return false;
    if (enc_h != eff_h) {
        const double k = (double)eff_h / (double)enc_h;
        r0 = (int)std::floor((double)r0 / k) - 2;
        r1 = (int)std::ceil((double)r1 / k) + 2;
    }
    *d0 = std::max(0, r0) & ~1;
    *d1 = std::min(enc_h, (r1 + 1) & ~1);
    return *d1 > *d0;
}
