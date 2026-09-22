// scripts/tests/test_encoder_helpers.cpp
//
// Standalone unit tests for the pure pixel-conversion helpers in
// src/hr_encoder_helpers.c (BGRA<->YUV420p/NV12, gamma LUT, thumbnail
// downscale). No wxWidgets, no DXGI, no AppState - just the math, so this
// builds and runs in a couple seconds without the full MSYS2/wx toolchain.
//
// One-time setup (doctest is a single MIT-licensed header, not a library):
//   curl -o scripts/tests/doctest.h \
//     https://raw.githubusercontent.com/doctest/doctest/master/doctest/doctest.h
//
// Build & run (any g++/MinGW works - this file doesn't touch Windows APIs):
//   g++ -O2 -c src/hr_encoder_helpers.c -o /tmp/hr_encoder_helpers.o
//   g++ -std=c++17 -Iscripts/tests scripts/tests/test_encoder_helpers.cpp \
//       /tmp/hr_encoder_helpers.o -o /tmp/hr_tests.exe
//   /tmp/hr_tests.exe
//
// One universal file, not one-per-function: add new TEST_CASEs here as
// more pure logic gets pulled out (settings clamping, console parsing,
// etc.) rather than starting a second test binary.

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
    // Per CHANGELOG.txt and ffmpeg/hr_ffmpeg_runner.cpp's "-color_range tv"
    // for the hardware-encoder path, hr_bgra_to_nv12 is supposed to emit
    // limited range (16-235), not full range (0-255) - that mismatch was
    // exactly the earlier "dark/dull, blackness effect" NVENC bug.
    // If this CHECK fails: hr_bgra_to_nv12_band in hr_encoder_helpers.c is
    // currently using the same full-range coefficients as
    // hr_bgra_to_yuv420p (I checked - as of this archive it is), which
    // means either that fix never made it into this tree, or it got
    // dropped in a merge. Worth flagging to whoever's on this file, since
    // ffmpeg_runner's comment says the fix is already in place.
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
