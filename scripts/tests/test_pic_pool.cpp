// scripts/tests/test_pic_pool.cpp - unit tests for src/core/pic_pool.h (shared converted pictures).
//   g++ -std=c++17 -O1 -fsanitize=address,undefined -I src/core -I scripts/tests scripts/tests/test_pic_pool.cpp -o t && ./t
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "pic_pool.h"

#include <random>

namespace {
// A "screen": each luma row/chroma row gets a value derived from (row, version); version bumps when the row changes.
struct Screen {
    int w, h; bool nv12;
    std::vector<int> ver;                                   // per luma row
    Screen(int w_, int h_, bool n) : w(w_), h(h_), nv12(n), ver((size_t)h_, 1) {}
    void touch(int y0, int y1) { for (int y = y0; y < y1; ++y) ver[(size_t)y] += 1; }
    uint8_t val(int row, int plane) const { return (uint8_t)((row * 7 + ver[(size_t)row] * 31 + plane * 59) & 255); }
    // "Convert" rows [y0,y1) of the screen into a picture buffer.
    void convert(std::vector<uint8_t> &b, int y0, int y1) const {
        for (int y = y0; y < y1; ++y) std::memset(&b[(size_t)y * w], val(y, 0), (size_t)w);
        const size_t base = (size_t)w * h;
        for (int y = y0 / 2; y < y1 / 2; ++y) {                 // a chroma row depends on luma rows 2y, 2y+1
            const uint8_t u = (uint8_t)(val(2 * y, 1) ^ val(2 * y + 1, 1));
            if (nv12) std::memset(&b[base + (size_t)y * w], u, (size_t)w);
            else {
                const size_t cw = (size_t)w / 2, plane = cw * (size_t)(h / 2);
                std::memset(&b[base + (size_t)y * cw], u, cw);
                std::memset(&b[base + plane + (size_t)y * cw], (uint8_t)(u + 1), cw);
            }
        }
    }
    std::vector<uint8_t> whole() const { std::vector<uint8_t> b((size_t)w * h * 3 / 2); convert(b, 0, h); return b; }
};
}

TEST_CASE("PicPool: partial pictures always equal a full conversion (random edits, random hold times)") {
    for (int nv12 = 0; nv12 < 2; ++nv12) {
        const int W = 32, H = 40;
        Screen scr(W, H, nv12 != 0);
        PicPool pool; pool.Reset(W, H, nv12 != 0);
        std::mt19937 rng(1234 + nv12);
        std::vector<PicPool::Buf> held;                      // pictures a "writer" still holds
        for (int step = 0; step < 4000; ++step) {
            const int kind = (int)(rng() % 10);
            if (!pool.Has() || kind == 0) {                  // full conversion
                scr.touch(0, H);
                auto b = pool.Full(); scr.convert(*b, 0, H); pool.Done(b, 0, H);
            } else if (kind <= 6) {                          // partial change
                int y0 = (int)(rng() % H), y1 = y0 + 1 + (int)(rng() % 12); y1 = std::min(y1, H);
                scr.touch(y0, y1);
                const int a = y0 & ~1, e = std::min(H, (y1 + 1) & ~1);
                auto b = pool.Next(a, e); scr.convert(*b, a, e); pool.Done(b, a, e);
            } else if (kind == 7) {                          // nothing changed: just re-send
            } else if (kind == 8) {                          // private copy (cursor): must equal the current picture, then get scribbled on
                auto c = pool.Copy();
                REQUIRE(*c == *pool.Cur());
                const int a = (int)(rng() % (H - 4)) & ~1, e = a + 2 + (int)(rng() % 3) * 2;       // draw on rows [a,e)
                Screen junk = scr; junk.touch(a, std::min(e, H)); junk.convert(*c, a, std::min(e, H));
                REQUIRE(*c != *pool.Cur());
                pool.Taint(c, a, std::min(e, H));
                held.push_back(c);
            }
            if (pool.Has()) {
                REQUIRE(*pool.Cur() == scr.whole());
                if (rng() % 3 == 0) held.push_back(pool.Cur());        // the writer keeps this one for a while
            }
            while (held.size() > (size_t)(rng() % 12)) held.erase(held.begin());
        }
    }
}

TEST_CASE("PicPool: a picture the writer holds is never rewritten") {
    const int W = 16, H = 16;
    Screen scr(W, H, false);
    PicPool pool; pool.Reset(W, H, false);
    auto b0 = pool.Full(); scr.convert(*b0, 0, H); pool.Done(b0, 0, H);
    PicPool::Buf held = pool.Cur();
    const std::vector<uint8_t> snapshot = *held;
    for (int i = 0; i < 50; ++i) {
        scr.touch(0, H);
        auto b = pool.Next(0, 8); scr.convert(*b, 0, 8); pool.Done(b, 0, 8);
        CHECK(b != held);
        CHECK(*held == snapshot);
    }
}

TEST_CASE("PicPool: unchanged screen and an idle pool never allocate or copy") {
    const int W = 16, H = 16;
    Screen scr(W, H, true);
    PicPool pool; pool.Reset(W, H, true);
    auto b0 = pool.Full(); scr.convert(*b0, 0, H); pool.Done(b0, 0, H);
    const uint8_t *p = pool.Cur()->data();
    for (int i = 0; i < 10; ++i) CHECK(pool.Cur()->data() == p);   // Cur() is the same shared buffer
}

TEST_CASE("PicPool: when every buffer is in flight it still hands out a correct picture") {
    const int W = 16, H = 16;
    Screen scr(W, H, false);
    PicPool pool; pool.Reset(W, H, false);
    std::vector<PicPool::Buf> all;
    auto b0 = pool.Full(); scr.convert(*b0, 0, H); pool.Done(b0, 0, H); all.push_back(pool.Cur());
    for (int i = 0; i < 40; ++i) {
        scr.touch(2, 6);
        auto b = pool.Next(2, 6); scr.convert(*b, 2, 6); pool.Done(b, 2, 6);
        all.push_back(pool.Cur());                                // never released
        REQUIRE(*pool.Cur() == scr.whole());
    }
}

TEST_CASE("PicPool: Reset to a new size starts clean; Drop forces a full picture") {
    PicPool pool; pool.Reset(16, 16, false);
    Screen a(16, 16, false);
    auto b = pool.Full(); a.convert(*b, 0, 16); pool.Done(b, 0, 16);
    CHECK(pool.Has());
    pool.Drop(); CHECK(!pool.Has());
    pool.Reset(32, 16, true); CHECK(!pool.Has());
    CHECK(pool.Bytes() == 32u * 16 * 3 / 2);
    auto c = pool.Full(); CHECK(c->size() == pool.Bytes());
}

// ---- dirty rows through crop and scaling --------------------------------------------------------------------
TEST_CASE("PicDirtyRows: covers every output row that reads a changed source row (box and nearest downscale)") {
    const int cases[][3] = {{1080, 1080, 0}, {1080, 540, 0}, {1080, 360, 0}, {1080, 720, 0}, {1080, 1440, 0}, {1440, 1080, 0},
                            {1600, 900, 0}, {768, 540, 0}, {500, 250, 0}, {250, 500, 0}, {1081, 540, 0}, {200, 66, 0}};
    for (const auto &c : cases) {
        const int eff_h = c[0], enc_h = c[1];
        for (int crop_y : {0, 6, 33}) {
            const int src_h = eff_h + crop_y + 10;
            // which source rows output row y reads - exactly as bgra_downscale_band() does
            auto reads = [&](int y, int &lo, int &hi) {
                if (enc_h == eff_h) { lo = y; hi = y + 1; return; }
                if (eff_h % enc_h == 0) { lo = y * (eff_h / enc_h); hi = lo + eff_h / enc_h; return; }      // box
                const float ry = (float)eff_h / enc_h;
                int sy = (int)(y * ry); if (sy >= eff_h) sy = eff_h - 1;
                lo = sy; hi = sy + 1;                                                                        // nearest
            };
            for (int trial = 0; trial < 400; ++trial) {
                const int a = (trial * 37 + crop_y * 11) % src_h, b = std::min(src_h, a + 1 + (trial * 13) % 70);
                int d0, d1;
                const bool any = PicDirtyRows(a, b, crop_y, eff_h, enc_h, &d0, &d1);
                CHECK((d0 % 2) == 0);
                CHECK(d1 <= enc_h);
                for (int y = 0; y < enc_h; ++y) {
                    int lo, hi; reads(y, lo, hi);
                    const bool touched = (lo + crop_y) < b && (hi + crop_y) > a;      // reads a changed capture row
                    if (touched) { CHECK(any); CHECK(y >= d0); CHECK(y < d1); }
                }
                if (!any) { /* nothing of the picture changed: no output row may touch the range */
                    for (int y = 0; y < enc_h; ++y) { int lo, hi; reads(y, lo, hi); CHECK(!((lo + crop_y) < b && (hi + crop_y) > a)); }
                }
            }
        }
    }
}

TEST_CASE("PicDirtyRows: no scaling is exact (even-aligned), changes outside the crop are ignored") {
    int d0, d1;
    CHECK(PicDirtyRows(100, 131, 0, 1080, 1080, &d0, &d1)); CHECK(d0 == 100); CHECK(d1 == 132);
    CHECK(PicDirtyRows(101, 102, 0, 1080, 1080, &d0, &d1)); CHECK(d0 == 100); CHECK(d1 == 102);
    CHECK(PicDirtyRows(0, 1080, 0, 1080, 1080, &d0, &d1)); CHECK(d0 == 0); CHECK(d1 == 1080);
    CHECK(!PicDirtyRows(0, 100, 200, 300, 300, &d0, &d1));        // above the crop
    CHECK(!PicDirtyRows(500, 700, 200, 300, 300, &d0, &d1));      // below the crop
    CHECK(PicDirtyRows(150, 260, 200, 300, 300, &d0, &d1)); CHECK(d0 == 0); CHECK(d1 == 60);       // rows 200..259 of the capture = rows 0..59 of the crop
}
