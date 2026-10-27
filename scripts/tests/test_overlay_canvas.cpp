// scripts/tests/test_overlay_canvas.cpp - standalone unit tests for src/utils/overlay_canvas.h
// (the transparent picture HomRec feeds to ffmpeg's `overlay` filter on a ddagrab recording, item 8).
//
// One case runs the REAL ffmpeg `overlay` filter on a canvas written by the code under test and checks a pixel; it is
// skipped when `ffmpeg` is not on PATH.
//
// Build (any OS with a C++17 compiler):
//   g++ -std=c++17 -O1 -fsanitize=address,undefined -I src/utils scripts/tests/test_overlay_canvas.cpp -o t && ./t

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "overlay_canvas.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

std::vector<uint8_t> Solid(int w, int h, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    std::vector<uint8_t> v((size_t)w * h * 4);
    for (size_t i = 0; i < v.size(); i += 4) { v[i] = b; v[i + 1] = g; v[i + 2] = r; v[i + 3] = a; }
    return v;
}
const uint8_t *Px(const std::vector<uint8_t> &c, int cw, int x, int y) { return &c[((size_t)y * cw + x) * 4]; }

HrCanvasLayer Layer(const std::vector<uint8_t> &bm, int w, int h, int x, int y, int opacity = 255) {
    return HrCanvasLayer{bm.data(), w, h, x, y, w, h, opacity};
}

bool HaveFfmpeg() { return std::system("ffmpeg -version > /dev/null 2>&1") == 0; }

} // namespace

TEST_CASE("an opaque layer is copied, the rest stays transparent") {
    std::vector<uint8_t> canvas(8 * 6 * 4, 0xCC);
    hr_canvas_clear(canvas.data(), 8, 6);
    const auto bm = Solid(3, 2, 10, 20, 30, 255);
    hr_canvas_blend(canvas.data(), 8, 6, Layer(bm, 3, 2, 2, 1));
    CHECK(Px(canvas, 8, 2, 1)[0] == 10);
    CHECK(Px(canvas, 8, 2, 1)[2] == 30);
    CHECK(Px(canvas, 8, 2, 1)[3] == 255);
    CHECK(Px(canvas, 8, 4, 2)[3] == 255);
    CHECK(Px(canvas, 8, 5, 2)[3] == 0);     // right of the layer
    CHECK(Px(canvas, 8, 1, 1)[3] == 0);     // left of it
    CHECK(Px(canvas, 8, 2, 3)[3] == 0);     // below it
}

TEST_CASE("a half transparent layer on an empty canvas keeps its colour and its alpha") {
    std::vector<uint8_t> canvas(4 * 4 * 4);
    hr_canvas_clear(canvas.data(), 4, 4);
    const auto bm = Solid(4, 4, 200, 100, 50, 128);
    hr_canvas_blend(canvas.data(), 4, 4, Layer(bm, 4, 4, 0, 0));
    CHECK(Px(canvas, 4, 1, 1)[0] == 200);
    CHECK(Px(canvas, 4, 1, 1)[1] == 100);
    CHECK(Px(canvas, 4, 1, 1)[2] == 50);
    CHECK(Px(canvas, 4, 1, 1)[3] == 128);
}

TEST_CASE("layer opacity multiplies the bitmap alpha; zero draws nothing") {
    std::vector<uint8_t> canvas(2 * 2 * 4);
    hr_canvas_clear(canvas.data(), 2, 2);
    const auto bm = Solid(2, 2, 9, 9, 9, 255);
    hr_canvas_blend(canvas.data(), 2, 2, Layer(bm, 2, 2, 0, 0, 0));
    CHECK(Px(canvas, 2, 0, 0)[3] == 0);
    hr_canvas_blend(canvas.data(), 2, 2, Layer(bm, 2, 2, 0, 0, 100));
    CHECK(Px(canvas, 2, 0, 0)[3] == 100);
}

TEST_CASE("two stacked translucent layers match the float 'over' formula") {
    std::vector<uint8_t> canvas(1 * 1 * 4);
    hr_canvas_clear(canvas.data(), 1, 1);
    const auto lo = Solid(1, 1, 255, 0, 0, 160);     // blue-ish, alpha .627
    const auto hi = Solid(1, 1, 0, 0, 255, 100);     // red,     alpha .392
    hr_canvas_blend(canvas.data(), 1, 1, Layer(lo, 1, 1, 0, 0));
    hr_canvas_blend(canvas.data(), 1, 1, Layer(hi, 1, 1, 0, 0));
    const double a1 = 160 / 255.0, a2 = 100 / 255.0;
    const double oa = a2 + a1 * (1 - a2);
    const double ob = (0 * a2 + 255 * a1 * (1 - a2)) / oa;
    const double orr = (255 * a2 + 0 * a1 * (1 - a2)) / oa;
    CHECK(std::fabs(canvas[3] - oa * 255) <= 1.5);
    CHECK(std::fabs(canvas[0] - ob) <= 2.0);
    CHECK(std::fabs(canvas[2] - orr) <= 2.0);
}

TEST_CASE("clipping: partly and fully outside layers") {
    std::vector<uint8_t> canvas(4 * 4 * 4);
    hr_canvas_clear(canvas.data(), 4, 4);
    const auto bm = Solid(3, 3, 1, 2, 3, 255);
    hr_canvas_blend(canvas.data(), 4, 4, Layer(bm, 3, 3, -2, -2));   // only its bottom-right pixel is inside
    CHECK(Px(canvas, 4, 0, 0)[3] == 255);
    CHECK(Px(canvas, 4, 1, 0)[3] == 0);
    hr_canvas_blend(canvas.data(), 4, 4, Layer(bm, 3, 3, 3, 3));     // only its top-left pixel is inside
    CHECK(Px(canvas, 4, 3, 3)[3] == 255);
    hr_canvas_blend(canvas.data(), 4, 4, Layer(bm, 3, 3, 50, 50));   // nowhere near
    hr_canvas_blend(canvas.data(), 4, 4, Layer(bm, 3, 3, -50, 0));
    CHECK(Px(canvas, 4, 2, 2)[3] == 0);
}

TEST_CASE("a layer drawn at another size is scaled (nearest) and stays inside its rectangle") {
    std::vector<uint8_t> canvas(8 * 8 * 4);
    hr_canvas_clear(canvas.data(), 8, 8);
    std::vector<uint8_t> bm = {255, 0, 0, 255,   0, 255, 0, 255};     // 2x1: blue | green
    HrCanvasLayer L{bm.data(), 2, 1, 1, 1, 4, 2, 255};                // drawn 4x2 at (1,1)
    hr_canvas_blend(canvas.data(), 8, 8, L);
    CHECK(Px(canvas, 8, 1, 1)[0] == 255);   // left half blue
    CHECK(Px(canvas, 8, 2, 2)[0] == 255);
    CHECK(Px(canvas, 8, 3, 1)[1] == 255);   // right half green
    CHECK(Px(canvas, 8, 4, 2)[1] == 255);
    CHECK(Px(canvas, 8, 5, 1)[3] == 0);     // outside the 4x2 rectangle
    CHECK(Px(canvas, 8, 1, 3)[3] == 0);
}

TEST_CASE("null / empty input is ignored") {
    std::vector<uint8_t> canvas(2 * 2 * 4, 7);
    HrCanvasLayer none{nullptr, 2, 2, 0, 0, 2, 2, 255};
    hr_canvas_blend(canvas.data(), 2, 2, none);
    hr_canvas_blend(nullptr, 2, 2, none);
    hr_canvas_clear(nullptr, 2, 2);
    CHECK(canvas[0] == 7);
}

TEST_CASE("ffmpeg's overlay filter blends this canvas the way the formula says") {
    if (!HaveFfmpeg()) return;
    const int W = 64, H = 48;
    // A 20x10 layer: red at 50% alpha, at (10, 8) on an otherwise transparent canvas.
    std::vector<uint8_t> canvas((size_t)W * H * 4);
    hr_canvas_clear(canvas.data(), W, H);
    const auto bm = Solid(20, 10, 0, 0, 255, 128);
    hr_canvas_blend(canvas.data(), W, H, Layer(bm, 20, 10, 10, 8));
    FILE *f = std::fopen("/tmp/hr_canvas.bgra", "wb");
    REQUIRE(f != nullptr);
    std::fwrite(canvas.data(), 1, canvas.size(), f);
    std::fclose(f);
    // Background = solid green; the canvas is the 2nd input exactly as in the ddagrab command line.
    const int rc = std::system(
        "ffmpeg -v error -f lavfi -i color=c=0x00FF00:s=64x48:r=30:d=1 "
        "-f rawvideo -pixel_format bgra -video_size 64x48 -framerate 15 -i /tmp/hr_canvas.bgra "
        "-filter_complex \"[0:v]format=bgra[bg];[1:v]format=bgra[ov];"
        "[bg][ov]overlay=x=0:y=0:eof_action=pass:repeatlast=1:format=auto[o]\" -map \"[o]\" "
        "-frames:v 1 -f rawvideo -pix_fmt bgra -y /tmp/hr_canvas_out.bgra");
    REQUIRE(rc == 0);
    std::vector<uint8_t> out((size_t)W * H * 4);
    f = std::fopen("/tmp/hr_canvas_out.bgra", "rb");
    REQUIRE(f != nullptr);
    REQUIRE(std::fread(out.data(), 1, out.size(), f) == out.size());
    std::fclose(f);
    auto at = [&](int x, int y) { return &out[((size_t)y * W + x) * 4]; };
    // inside the layer: 50% red over green -> (r ~128, g ~127, b 0)
    CHECK(std::abs(at(15, 12)[2] - 128) <= 3);
    CHECK(std::abs(at(15, 12)[1] - 127) <= 3);
    CHECK(at(15, 12)[0] <= 2);
    // outside the layer: untouched green
    CHECK(at(2, 2)[1] >= 253);
    CHECK(at(2, 2)[2] <= 2);
    CHECK(at(40, 30)[1] >= 253);
}
