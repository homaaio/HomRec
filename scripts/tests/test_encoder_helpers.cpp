// scripts/tests/test_encoder_helpers.cpp - Standalone unit tests for the pure pixel-
// conversion helpers in src/core/encoder_helpers.c (BGRA<->YUV420p/NV12, gamma LUT,
// thumbnail downscale).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstdint>
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

// Same row-copy rule as copy_yuv_rows() in pipeline.cpp.
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
