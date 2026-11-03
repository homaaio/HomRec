// scripts/tests/bench.cpp - micro benchmark of the per-frame CPU hot spots (no Windows API needed).
//
// Prints "[bench] <name> <ms per call>" lines (lower = faster); tools/bench_compare.py compares two runs.
//   scripts/linux/bench.sh > before.txt   ...change code...   scripts/linux/bench.sh > after.txt
//   python tools/bench_compare.py before.txt after.txt
//
// Windows (MSYS2):  gcc -O3 -c src/core/encoder_helpers.c -o eh.o
//                   g++ -std=c++17 -O3 -Isrc scripts/tests/bench.cpp eh.o -o bench.exe && bench.exe

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
void hr_bgra_to_nv12(const uint8_t *bgra, uint8_t *nv12_out, int width, int height);
void hr_bgra_to_yuv420p(const uint8_t *bgra, uint8_t *yuv_out, int width, int height);
}

// The overlay scaler as it was (divide per pixel) and as it is now (column table) - copies of
// ScaleBgraNearest() in src/core/overlay_render.cpp, so the gain can be measured off-Windows.
static void scale_div(const std::vector<uint8_t> &src, int sw, int sh, std::vector<uint8_t> &dst, int dw, int dh) {
    dst.assign((size_t)dw * dh * 4, 0);
    for (int y = 0; y < dh; ++y) {
        int sy = (int)((int64_t)y * sh / dh); if (sy >= sh) sy = sh - 1;
        const uint8_t *srow = src.data() + (size_t)sy * sw * 4;
        uint8_t *drow = dst.data() + (size_t)y * dw * 4;
        for (int x = 0; x < dw; ++x) {
            int sx = (int)((int64_t)x * sw / dw); if (sx >= sw) sx = sw - 1;
            std::memcpy(drow + (size_t)x * 4, srow + (size_t)sx * 4, 4);
        }
    }
}
static void scale_tab(const std::vector<uint8_t> &src, int sw, int sh, std::vector<uint8_t> &dst, int dw, int dh) {
    dst.resize((size_t)dw * dh * 4);
    std::vector<int> xt((size_t)dw);
    for (int x = 0; x < dw; ++x) { int sx = (int)((int64_t)x * sw / dw); xt[x] = (sx >= sw ? sw - 1 : sx) * 4; }
    for (int y = 0; y < dh; ++y) {
        int sy = (int)((int64_t)y * sh / dh); if (sy >= sh) sy = sh - 1;
        const uint8_t *srow = src.data() + (size_t)sy * sw * 4;
        uint8_t *drow = dst.data() + (size_t)y * dw * 4;
        for (int x = 0; x < dw; ++x) std::memcpy(drow + (size_t)x * 4, srow + xt[x], 4);
    }
}

template <class F>
static void run(const char *name, F f, int iters = 60) {
    for (int i = 0; i < 5; ++i) f();   // warm up
    double best = 1e9;                  // best of 5 batches: least noise from other processes
    for (int b = 0; b < 5; ++b) {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) f();
        auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count() / iters);
    }
    std::printf("[bench] %s %.3f\n", name, best);
}

int main() {
    const int W = 1920, H = 1080;
    std::vector<uint8_t> bgra((size_t)W * H * 4);
    uint32_t s = 12345;
    for (auto &b : bgra) { s = s * 1664525u + 1013904223u; b = (uint8_t)(s >> 24); }

    std::vector<uint8_t> nv12((size_t)W * H * 3 / 2), i420((size_t)W * H * 3 / 2);
    run("cvt_nv12_1080p",  [&] { hr_bgra_to_nv12(bgra.data(), nv12.data(), W, H); });
    run("cvt_i420_1080p",  [&] { hr_bgra_to_yuv420p(bgra.data(), i420.data(), W, H); });

    // A webcam / window layer (1280x720) scaled into a 480x270 overlay, the usual case.
    std::vector<uint8_t> cam((size_t)1280 * 720 * 4, 77), out;
    run("ovl_scale_div_720p_to_270p", [&] { scale_div(cam, 1280, 720, out, 480, 270); });
    run("ovl_scale_tab_720p_to_270p", [&] { scale_tab(cam, 1280, 720, out, 480, 270); });
    // Big layer (the whole desktop shown as a region overlay).
    run("ovl_scale_div_1080p_to_540p", [&] { scale_div(bgra, W, H, out, 960, 540); });
    run("ovl_scale_tab_1080p_to_540p", [&] { scale_tab(bgra, W, H, out, 960, 540); });
    return 0;
}
