// scripts/tests/test_encoder_helpers.cpp - Standalone unit tests for the pure pixel-
// conversion helpers in src/core/encoder_helpers.c (BGRA<->YUV420p/NV12, gamma LUT,
// thumbnail downscale).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
    void hr_bgra_to_yuv420p(const uint8_t *bgra, uint8_t *yuv, int w, int h);
    void hr_bgra_to_nv12(const uint8_t *bgra, uint8_t *nv12, int w, int h);
    void hr_yuv420p_to_rgb(const uint8_t *yuv, uint8_t *rgb, int w, int h);
    void hr_gamma_lut_apply(uint8_t *pixels, size_t n_bytes, int gamma_x100);
    int  hr_build_thumbnail_lq(const uint8_t *src, uint8_t *dst,
                                int sw, int sh, int dw, int dh);
    int  hr_csc_set_max_simd(int cap);
    void hr_bgra_to_nv12_band(const uint8_t *bgra, uint8_t *nv12, int w, int h, int y0, int y1);
    void hr_bgra_to_yuv420p_band(const uint8_t *bgra, uint8_t *yuv, int w, int h, int y0, int y1);
    void hr_bgra_to_nv12_band_s(const uint8_t *bgra, size_t stride, uint8_t *nv12, int w, int h, int y0, int y1);
    void hr_bgra_to_yuv420p_band_s(const uint8_t *bgra, size_t stride, uint8_t *yuv, int w, int h, int y0, int y1);
    void hr_blend_bgra(uint8_t *dst, size_t dst_stride, const uint8_t *src, size_t src_stride, int w, int h, int opacity);
    void hr_bgra_to_thumb(const uint8_t *bgra, size_t stride, uint8_t *rgb, int sw, int sh, int dw, int dh);
}

namespace {
std::vector<uint8_t> SolidBgra(int w, int h, uint8_t b, uint8_t g, uint8_t r) {
    std::vector<uint8_t> buf((size_t)w * h * 4);
    for (size_t i = 0; i < buf.size(); i += 4) {
        buf[i + 0] = b; buf[i + 1] = g; buf[i + 2] = r; buf[i + 3] = 255;
    }
    return buf;
}
} // namespace

TEST_CASE("hr_bgra_to_yuv420p: solid black -> Y=0, chroma neutral (full range, software path)") {
    const int w = 4, h = 4;
    auto bgra = SolidBgra(w, h, 0, 0, 0);
    std::vector<uint8_t> yuv((size_t)w * h * 3 / 2);
    hr_bgra_to_yuv420p(bgra.data(), yuv.data(), w, h);

    for (int i = 0; i < w * h; ++i) CHECK(yuv[i] == 0);
    for (size_t i = (size_t)w * h; i < yuv.size(); ++i) CHECK(yuv[i] == 128);
}

TEST_CASE("hr_bgra_to_yuv420p: solid white -> Y=255 (full range, software x264/x265 path)") {
    const int w = 4, h = 4;
    auto bgra = SolidBgra(w, h, 255, 255, 255);
    std::vector<uint8_t> yuv((size_t)w * h * 3 / 2);
    hr_bgra_to_yuv420p(bgra.data(), yuv.data(), w, h);
    for (int i = 0; i < w * h; ++i) CHECK(yuv[i] == 255);
}

TEST_CASE("hr_bgra_to_nv12: solid black -> Y should be 16 in limited/tv range (NVENC/QSV/AMF)") {
    const int w = 4, h = 4;
    auto bgra = SolidBgra(w, h, 0, 0, 0);
    std::vector<uint8_t> nv12((size_t)w * h * 3 / 2);
    hr_bgra_to_nv12(bgra.data(), nv12.data(), w, h);
    CHECK(nv12[0] == 16);
}

TEST_CASE("hr_bgra_to_nv12: solid white -> Y should be 235 in limited/tv range") {
    const int w = 4, h = 4;
    auto bgra = SolidBgra(w, h, 255, 255, 255);
    std::vector<uint8_t> nv12((size_t)w * h * 3 / 2);
    hr_bgra_to_nv12(bgra.data(), nv12.data(), w, h);
    CHECK(nv12[0] == 235);
}

TEST_CASE("hr_yuv420p_to_rgb: neutral mid-gray input doesn't clip to black") {
    const int w = 2, h = 2;
    std::vector<uint8_t> yuv = {128, 128, 128, 128, 128, 128}; // Y x4, Cb, Cr
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    hr_yuv420p_to_rgb(yuv.data(), rgb.data(), w, h);
    for (int i = 0; i < w * h * 3; ++i) CHECK(rgb[i] > 100);
}

TEST_CASE("hr_gamma_lut_apply: gamma 1.0 (100) is a documented no-op") {
    uint8_t pixels[4] = {0, 64, 128, 255};
    uint8_t before[4];
    memcpy(before, pixels, sizeof(pixels));
    hr_gamma_lut_apply(pixels, sizeof(pixels), 100);
    CHECK(memcmp(pixels, before, sizeof(pixels)) == 0);
}

TEST_CASE("hr_gamma_lut_apply: gamma < 1.0 brightens midtones, doesn't touch 0 or 255") {
    uint8_t pixels[3] = {0, 128, 255};
    hr_gamma_lut_apply(pixels, sizeof(pixels), 50); // gamma 0.5
    CHECK(pixels[0] == 0);
    CHECK(pixels[1] > 128);
    CHECK(pixels[2] == 255);
}

TEST_CASE("hr_build_thumbnail_lq: uniform 4x4 block downscales to 2x2 of the same color") {
    const int sw = 4, sh = 4, dw = 2, dh = 2;
    std::vector<uint8_t> src((size_t)sw * sh * 3);
    for (size_t i = 0; i < src.size(); i += 3) { src[i] = 10; src[i+1] = 20; src[i+2] = 30; }
    std::vector<uint8_t> dst((size_t)dw * dh * 3);

    REQUIRE(hr_build_thumbnail_lq(src.data(), dst.data(), sw, sh, dw, dh) == 1);
    for (size_t i = 0; i < dst.size(); i += 3) {
        CHECK(dst[i] == 10);
        CHECK(dst[i+1] == 20);
        CHECK(dst[i+2] == 30);
    }
}

TEST_CASE("hr_build_thumbnail_lq: rejects a non-integer scale factor instead of corrupting memory") {
    std::vector<uint8_t> src(5 * 5 * 3, 0), dst(2 * 2 * 3, 0);
    CHECK(hr_build_thumbnail_lq(src.data(), dst.data(), 5, 5, 2, 2) == 0); // 5 % 2 != 0
}

// Partial re-conversion (capture_loop(): DXGI dirty rows -> re-convert only those rows of
// the previously converted picture).
extern "C" {
    void hr_bgra_to_yuv420p_band(const uint8_t *bgra, uint8_t *yuv, int w, int h, int y0, int y1);
    void hr_bgra_to_nv12_band(const uint8_t *bgra, uint8_t *nv12, int w, int h, int y0, int y1);
}

namespace {
std::vector<uint8_t> NoisyBgra(int w, int h, unsigned seed) {
    std::vector<uint8_t> buf((size_t)w * h * 4);
    unsigned s = seed;
    for (size_t i = 0; i < buf.size(); ++i) { s = s * 1664525u + 1013904223u; buf[i] = (uint8_t)(s >> 24); }
    return buf;
}

// Same row-copy rule as PicPool::copy_rows() in src/core/pic_pool.h.
void CopyYuvRows(uint8_t *dst, const uint8_t *src, int w, int h, int y0, int y1, bool nv12) {
    std::memcpy(dst + (size_t)y0 * w, src + (size_t)y0 * w, (size_t)(y1 - y0) * w);
    const size_t base = (size_t)w * h;
    if (nv12) {
        std::memcpy(dst + base + (size_t)(y0 / 2) * w, src + base + (size_t)(y0 / 2) * w,
                    (size_t)((y1 - y0) / 2) * w);
    } else {
        const size_t cw = (size_t)w / 2, plane = cw * (size_t)(h / 2);
        for (int pl = 0; pl < 2; ++pl) {
            const size_t off = base + (size_t)pl * plane + (size_t)(y0 / 2) * cw;
            std::memcpy(dst + off, src + off, (size_t)((y1 - y0) / 2) * cw);
        }
    }
}

void CheckPartialMatchesFull(bool nv12) {
    const int w = 64, h = 48;
    const size_t yuv_bytes = (size_t)w * h * 3 / 2;
    auto convert_band = nv12 ? hr_bgra_to_nv12_band : hr_bgra_to_yuv420p_band;

    auto frame_a = NoisyBgra(w, h, 1);
    auto frame_b = frame_a;
    // Change a block of rows [10, 21) in the new frame.
    for (int y = 10; y < 21; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c)
                frame_b[((size_t)y * w + x) * 4 + c] = (uint8_t)(frame_b[((size_t)y * w + x) * 4 + c] + 77);

    std::vector<uint8_t> yuv_a(yuv_bytes), yuv_full_b(yuv_bytes);
    convert_band(frame_a.data(), yuv_a.data(), w, h, 0, h);
    convert_band(frame_b.data(), yuv_full_b.data(), w, h, 0, h);

    // What capture_loop() does: baseline copy + band re-convert, rows rounded
    // outward to even boundaries, then the cached picture patched row-wise.
    int dy0 = 10, dy1 = 21;
    dy0 &= ~1; dy1 = (dy1 + 1) & ~1;                // -> [10, 22)
    std::vector<uint8_t> yuv_partial = yuv_a;
    convert_band(frame_b.data(), yuv_partial.data(), w, h, dy0, dy1);
    CHECK(yuv_partial == yuv_full_b);

    std::vector<uint8_t> cached = yuv_a;
    CopyYuvRows(cached.data(), yuv_partial.data(), w, h, dy0, dy1, nv12);
    CHECK(cached == yuv_full_b);
}
} // namespace

TEST_CASE("partial re-conversion of dirty rows == full conversion (NV12)") {
    CheckPartialMatchesFull(true);
}

TEST_CASE("partial re-conversion of dirty rows == full conversion (planar I420)") {
    CheckPartialMatchesFull(false);
}

// Every SIMD level must give EXACTLY the scalar result (bit for bit), for any width / height / band.
TEST_CASE("BGRA->YUV: SSE4.1 and AVX2 are bit-identical to the scalar converter") {
    const int best = hr_csc_set_max_simd(-1);
    MESSAGE("highest SIMD level on this CPU: " << best << " (0 scalar, 1 SSE4.1, 2 AVX2)");

    uint32_t rng = 12345u;
    auto next = [&rng]() { rng = rng * 1664525u + 1013904223u; return (uint8_t)(rng >> 24); };

    const int widths[]  = {2, 6, 8, 14, 16, 18, 30, 32, 34, 46, 64, 70, 130, 1366, 1920};
    const int heights[] = {2, 4, 6, 8, 18, 36};   // even: encoders need even sizes
    for (int w : widths) {
        for (int h : heights) {
            std::vector<uint8_t> src((size_t)w * h * 4);
            for (auto &b : src) b = next();
            // saturated / extreme pixels too (clamping paths)
            for (size_t i = 0; i + 3 < src.size() && i < 4096; i += 64) { src[i] = 255; src[i + 1] = 0; src[i + 2] = 255; }

            const size_t out_sz = (size_t)w * h * 3 / 2 + 64;
            for (int nv12 = 0; nv12 < 2; ++nv12) {
                auto run = [&](int cap, int y0, int y1) {
                    hr_csc_set_max_simd(cap);
                    std::vector<uint8_t> out(out_sz, 0xCD);
                    if (nv12) hr_bgra_to_nv12_band(src.data(), out.data(), w, h, y0, y1);
                    else      hr_bgra_to_yuv420p_band(src.data(), out.data(), w, h, y0, y1);
                    return out;
                };
                const int y0 = (h > 4) ? 2 : 0;   // a band that doesn't start at row 0
                for (int level = 1; level <= best; ++level) {
                    CHECK(run(level, 0, h) == run(0, 0, h));
                    CHECK(run(level, y0, h) == run(0, y0, h));
                }
            }
        }
    }
    hr_csc_set_max_simd(-1);
}


// ---- crop without copy: converting a rectangle in place (stride) == copying it out first --------------------
TEST_CASE("BGRA->YUV with a row pitch: a rectangle inside a bigger frame converts like its packed copy") {
    const int best = hr_csc_set_max_simd(-1);
    const int FW = 130, FH = 70;
    auto frame = NoisyBgra(FW, FH, 9);
    const int rects[][4] = {{0, 0, 130, 70}, {10, 6, 64, 32}, {2, 2, 18, 8}, {50, 20, 70, 48}, {1, 1, 34, 14}};
    for (const auto &r : rects) {
        const int cx = r[0], cy = r[1], cw = r[2] & ~1, ch = r[3] & ~1;
        std::vector<uint8_t> packed((size_t)cw * ch * 4);
        for (int y = 0; y < ch; ++y)
            std::memcpy(&packed[(size_t)y * cw * 4], &frame[((size_t)(cy + y) * FW + cx) * 4], (size_t)cw * 4);
        const uint8_t *base = frame.data() + ((size_t)cy * FW + cx) * 4;
        for (int level = 0; level <= best; ++level) {
            hr_csc_set_max_simd(level);
            for (int nv12 = 0; nv12 < 2; ++nv12) {
                std::vector<uint8_t> a((size_t)cw * ch * 3 / 2), b(a.size());
                if (nv12) { hr_bgra_to_nv12_band(packed.data(), a.data(), cw, ch, 0, ch);
                            hr_bgra_to_nv12_band_s(base, (size_t)FW * 4, b.data(), cw, ch, 0, ch); }
                else      { hr_bgra_to_yuv420p_band(packed.data(), a.data(), cw, ch, 0, ch);
                            hr_bgra_to_yuv420p_band_s(base, (size_t)FW * 4, b.data(), cw, ch, 0, ch); }
                CHECK(a == b);
            }
        }
    }
    hr_csc_set_max_simd(-1);
}

// ---- overlay blend ----------------------------------------------------------------------------------------
namespace {
// The old per-pixel loop of CompositeBgra() in overlay_render.cpp, verbatim.
void RefBlend(uint8_t *base, int bw, const uint8_t *ov, int w, int h, int opacity) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t *o = ov + ((size_t)y * w + x) * 4;
            uint8_t *d = base + ((size_t)y * bw + x) * 4;
            uint32_t a = o[3];
            if (opacity != 255) a = (a * (uint32_t)opacity + 127u) / 255u;
            if (a == 0) continue;
            if (a == 255) { d[0] = o[0]; d[1] = o[1]; d[2] = o[2]; continue; }
            uint32_t a1 = 255u - a;
            d[0] = (uint8_t)((o[0] * a + d[0] * a1 + 128u) >> 8u);
            d[1] = (uint8_t)((o[1] * a + d[1] * a1 + 128u) >> 8u);
            d[2] = (uint8_t)((o[2] * a + d[2] * a1 + 128u) >> 8u);
        }
}
} // namespace

TEST_CASE("hr_blend_bgra: every SIMD level is bit-identical to the old per-pixel loop") {
    const int best = hr_csc_set_max_simd(-1);
    uint32_t rng = 777u;
    auto next = [&rng]() { rng = rng * 1664525u + 1013904223u; return (uint8_t)(rng >> 24); };
    const int widths[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 33, 64, 100, 257};
    const int opacities[] = {255, 254, 200, 128, 77, 3, 1};
    for (int w : widths) {
        const int h = 5, bw = w + 7;                     // base wider than the layer: tests the row pitch
        for (int mode = 0; mode < 4; ++mode) {
            std::vector<uint8_t> ov((size_t)w * h * 4), base((size_t)bw * h * 4);
            for (auto &b : base) b = next();
            for (size_t i = 0; i < ov.size(); i += 4) {
                ov[i] = next(); ov[i + 1] = next(); ov[i + 2] = next();
                const uint8_t r = next();
                // mode 0: random alpha; 1: mostly transparent; 2: mostly opaque; 3: all values of alpha in turn
                ov[i + 3] = mode == 0 ? r : mode == 1 ? (r < 230 ? 0 : r) : mode == 2 ? (r < 230 ? 255 : r) : (uint8_t)((i / 4) & 255);
            }
            for (int op : opacities) {
                std::vector<uint8_t> want = base;
                RefBlend(want.data(), bw, ov.data(), w, h, op);
                for (int level = 0; level <= best; ++level) {
                    hr_csc_set_max_simd(level);
                    std::vector<uint8_t> got = base;
                    hr_blend_bgra(got.data(), (size_t)bw * 4, ov.data(), (size_t)w * 4, w, h, op);
                    CHECK(got == want);
                }
            }
        }
    }
    hr_csc_set_max_simd(-1);
}

TEST_CASE("hr_blend_bgra: opacity 0 and a fully transparent layer leave the picture alone") {
    std::vector<uint8_t> base(64 * 4, 200), ov(64 * 4, 90), keep = base;
    hr_blend_bgra(base.data(), 64 * 4, ov.data(), 64 * 4, 64, 1, 0);
    CHECK(base == keep);
    for (size_t i = 3; i < ov.size(); i += 4) ov[i] = 0;
    hr_blend_bgra(base.data(), 64 * 4, ov.data(), 64 * 4, 64, 1, 255);
    CHECK(base == keep);
}

// ---- preview thumbnail ---------------------------------------------------------------------------------
namespace {
// The thumbnail sampler as it was in pipeline.cpp (bgra_to_thumb), kept here as the reference.
std::vector<uint8_t> RefThumb(const std::vector<uint8_t> &bgra, int sw, int sh, int dw, int dh) {
    std::vector<uint8_t> out((size_t)dw * dh * 3);
    auto spread = [](int dst, int src, int n) {
        std::vector<int> v((size_t)dst * n);
        for (int d = 0; d < dst; ++d)
            for (int k = 0; k < n; ++k) {
                int64_t t = ((int64_t)(2 * n * d + 2 * k + 1) * src) / ((int64_t)2 * n * dst);
                v[(size_t)d * n + k] = (int)(t >= src ? src - 1 : t);
            }
        return v;
    };
    if ((sw % dw) == 0 && (sh % dh) == 0) {
        const int rx0 = sw / dw, ry0 = sh / dh;
        if (rx0 * ry0 > 4) {
            const int nx = std::min(rx0, 2), ny = std::min(ry0, 2);
            const int ox[2] = {rx0 / 4, (rx0 * 3) / 4}, oy[2] = {ry0 / 4, (ry0 * 3) / 4};
            for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x) {
                uint32_t r = 0, g = 0, b = 0;
                for (int j = 0; j < ny; ++j) for (int i = 0; i < nx; ++i) {
                    const uint8_t *px = &bgra[(((size_t)(y * ry0 + oy[j])) * sw + x * rx0 + ox[i]) * 4];
                    b += px[0]; g += px[1]; r += px[2];
                }
                uint8_t *o = &out[((size_t)y * dw + x) * 3];
                o[0] = (uint8_t)(r / (nx * ny)); o[1] = (uint8_t)(g / (nx * ny)); o[2] = (uint8_t)(b / (nx * ny));
            }
            return out;
        }
        for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x) {
            uint32_t r = 0, g = 0, b = 0;
            for (int j = 0; j < ry0; ++j) for (int i = 0; i < rx0; ++i) {
                const uint8_t *px = &bgra[(((size_t)(y * ry0 + j)) * sw + x * rx0 + i) * 4];
                b += px[0]; g += px[1]; r += px[2];
            }
            uint8_t *o = &out[((size_t)y * dw + x) * 3];
            o[0] = (uint8_t)(r / (rx0 * ry0)); o[1] = (uint8_t)(g / (rx0 * ry0)); o[2] = (uint8_t)(b / (rx0 * ry0));
        }
        return out;
    }
    const int nx = std::min(4, std::max(1, (sw + dw - 1) / dw)), ny = std::min(4, std::max(1, (sh + dh - 1) / dh));
    auto xs = spread(dw, sw, nx), ys = spread(dh, sh, ny);
    for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x) {
        uint32_t r = 0, g = 0, b = 0;
        for (int j = 0; j < ny; ++j) for (int i = 0; i < nx; ++i) {
            const uint8_t *px = &bgra[((size_t)ys[(size_t)y * ny + j] * sw + xs[(size_t)x * nx + i]) * 4];
            b += px[0]; g += px[1]; r += px[2];
        }
        uint8_t *o = &out[((size_t)y * dw + x) * 3];
        o[0] = (uint8_t)(r / (nx * ny)); o[1] = (uint8_t)(g / (nx * ny)); o[2] = (uint8_t)(b / (nx * ny));
    }
    return out;
}
} // namespace

TEST_CASE("hr_bgra_to_thumb: every SIMD level gives the same picture as the old sampler") {
    const int best = hr_csc_set_max_simd(-1);
    const int cases[][4] = {            // sw, sh, dw, dh
        {64, 48, 32, 24},  {66, 40, 33, 20}, {64, 48, 16, 12}, {1920, 1080, 960, 540}, {1920, 1080, 480, 270},
        {1920, 1080, 800, 450}, {1366, 768, 683, 384}, {1366, 768, 640, 360}, {100, 80, 7, 5}, {40, 30, 40, 30},
        {80, 60, 20, 60}, {30, 40, 10, 8}, {2560, 1440, 1279, 720}, {64, 64, 63, 62}, {12, 8, 14, 10}, {8, 8, 2, 2},
        {20, 20, 10, 10}, {18, 12, 6, 6}, {128, 8, 32, 4}};
    for (const auto &c : cases) {
        const int sw = c[0], sh = c[1], dw = c[2], dh = c[3];
        auto src = NoisyBgra(sw, sh, (unsigned)(sw * 31 + dw));
        const auto want = RefThumb(src, sw, sh, dw, dh);
        for (int level = 0; level <= best; ++level) {
            hr_csc_set_max_simd(level);
            std::vector<uint8_t> got((size_t)dw * dh * 3 + 16, 0xAB);      // guard bytes: no write past the end
            hr_bgra_to_thumb(src.data(), 0, got.data(), sw, sh, dw, dh);
            CHECK(std::equal(want.begin(), want.end(), got.begin()));
            for (size_t i = want.size(); i < got.size(); ++i) CHECK(got[i] == 0xAB);
        }
    }
    hr_csc_set_max_simd(-1);
}

TEST_CASE("hr_bgra_to_thumb: a rectangle inside a bigger frame (row pitch) == its packed copy") {
    const int FW = 200, FH = 120, cx = 14, cy = 10, cw = 96, ch = 64;
    auto frame = NoisyBgra(FW, FH, 5);
    std::vector<uint8_t> packed((size_t)cw * ch * 4);
    for (int y = 0; y < ch; ++y) std::memcpy(&packed[(size_t)y * cw * 4], &frame[((size_t)(cy + y) * FW + cx) * 4], (size_t)cw * 4);
    for (int d : {48, 40, 31}) {
        const int dh = d * ch / cw;
        std::vector<uint8_t> a((size_t)d * dh * 3), b(a.size());
        hr_bgra_to_thumb(packed.data(), 0, a.data(), cw, ch, d, dh);
        hr_bgra_to_thumb(frame.data() + ((size_t)cy * FW + cx) * 4, (size_t)FW * 4, b.data(), cw, ch, d, dh);
        CHECK(a == b);
    }
}
