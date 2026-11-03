// scripts/tests/bench.cpp - micro benchmark of the per-frame CPU hot spots (no Windows API needed).
//
// Prints "[bench] <name> <ms per call>" lines (lower = faster) and "[pair] <label> <old> <new>" lines that name an
// old/new implementation pair measured in the same run. tools/bench_compare.py reads both:
//   scripts/linux/bench.sh > before.txt   ...change code...   scripts/linux/bench.sh > after.txt
//   python tools/bench_compare.py before.txt after.txt     (run vs run)
//   python tools/bench_compare.py after.txt                (old vs new inside one run)
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
void hr_bgra_to_nv12_band(const uint8_t *bgra, uint8_t *nv12_out, int width, int height, int y0, int y1);
void hr_bgra_to_nv12_band_s(const uint8_t *bgra, size_t stride, uint8_t *nv12_out, int width, int height, int y0, int y1);
void hr_blend_bgra(uint8_t *dst, size_t dst_stride, const uint8_t *src, size_t src_stride, int w, int h, int opacity);
void hr_bgra_to_thumb(const uint8_t *bgra, size_t stride, uint8_t *rgb, int sw, int sh, int dw, int dh);
}

// The overlay blend as it was: one pixel at a time (the loop CompositeBgra() in overlay_render.cpp used to run).
static void blend_old(uint8_t *base, int bw, const uint8_t *ov, int w, int h, int opacity) {
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

// The preview thumbnail as it was in pipeline.cpp for the two common cases: integer ratio with a 2x2 box
// (reads every pixel) and the spread 4x4 sampler for other sizes (scalar, one division per channel).
static void thumb_old(const std::vector<uint8_t> &bgra, int sw, int sh, std::vector<uint8_t> &out, int dw, int dh) {
    out.resize((size_t)dw * dh * 3);
    const bool integer = (sw % dw) == 0 && (sh % dh) == 0;
    const int nx = integer ? sw / dw : std::min(4, (sw + dw - 1) / dw), ny = integer ? sh / dh : std::min(4, (sh + dh - 1) / dh);
    const uint32_t cnt = (uint32_t)(nx * ny);
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            uint32_t r = 0, g = 0, b = 0;
            for (int j = 0; j < ny; ++j) {
                const int sy = integer ? y * ny + j : (int)(((int64_t)(2 * ny * y + 2 * j + 1) * sh) / ((int64_t)2 * ny * dh));
                for (int i = 0; i < nx; ++i) {
                    const int sx = integer ? x * nx + i : (int)(((int64_t)(2 * nx * x + 2 * i + 1) * sw) / ((int64_t)2 * nx * dw));
                    const uint8_t *px = &bgra[((size_t)sy * sw + sx) * 4];
                    b += px[0]; g += px[1]; r += px[2];
                }
            }
            uint8_t *o = &out[((size_t)y * dw + x) * 3];
            o[0] = (uint8_t)(r / cnt); o[1] = (uint8_t)(g / cnt); o[2] = (uint8_t)(b / cnt);
        }
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
    // A partly changed screen: only 120 rows are converted (partial convert).
    run("cvt_nv12_120rows", [&] { hr_bgra_to_nv12_band(bgra.data(), nv12.data(), W, H, 400, 520); });

    // A webcam / window layer (1280x720) scaled into a 480x270 overlay, the usual case.
    std::vector<uint8_t> cam((size_t)1280 * 720 * 4, 77), out;
    run("ovl_scale_div_720p_to_270p", [&] { scale_div(cam, 1280, 720, out, 480, 270); });
    run("ovl_scale_tab_720p_to_270p", [&] { scale_tab(cam, 1280, 720, out, 480, 270); });
    // Big layer (the whole desktop shown as a region overlay).
    run("ovl_scale_div_1080p_to_540p", [&] { scale_div(bgra, W, H, out, 960, 540); });
    run("ovl_scale_tab_1080p_to_540p", [&] { scale_tab(bgra, W, H, out, 960, 540); });
    std::printf("[pair] overlay_scale_270p ovl_scale_div_720p_to_270p ovl_scale_tab_720p_to_270p\n");
    std::printf("[pair] overlay_scale_540p ovl_scale_div_1080p_to_540p ovl_scale_tab_1080p_to_540p\n");

    // Overlay blend onto the 1080p picture: a 480x270 webcam-like layer (opaque, 80% opacity), and a text layer
    // that is ~92% transparent with opaque glyph pixels.
    {
        const int ow = 480, oh = 270;
        std::vector<uint8_t> cam_l((size_t)ow * oh * 4), txt_l((size_t)ow * oh * 4);
        for (size_t i = 0; i < cam_l.size(); i += 4) {
            s = s * 1664525u + 1013904223u; cam_l[i] = (uint8_t)(s >> 24); cam_l[i + 1] = (uint8_t)(s >> 16); cam_l[i + 2] = (uint8_t)(s >> 8); cam_l[i + 3] = 255;
            s = s * 1664525u + 1013904223u; const bool ink = (s >> 24) < 20;
            txt_l[i] = 230; txt_l[i + 1] = 230; txt_l[i + 2] = 230; txt_l[i + 3] = ink ? ((s >> 12) & 1 ? 255 : 128) : 0;
        }
        std::vector<uint8_t> pic = bgra;
        const size_t pitch = (size_t)W * 4;
        run("blend_old_cam_480x270",  [&] { blend_old(pic.data(), W, cam_l.data(), ow, oh, 204); });
        run("blend_new_cam_480x270",  [&] { hr_blend_bgra(pic.data(), pitch, cam_l.data(), (size_t)ow * 4, ow, oh, 204); });
        run("blend_old_text_480x270", [&] { blend_old(pic.data(), W, txt_l.data(), ow, oh, 255); });
        run("blend_new_text_480x270", [&] { hr_blend_bgra(pic.data(), pitch, txt_l.data(), (size_t)ow * 4, ow, oh, 255); });
        std::printf("[pair] overlay_blend_cam blend_old_cam_480x270 blend_new_cam_480x270\n");
        std::printf("[pair] overlay_blend_text blend_old_text_480x270 blend_new_text_480x270\n");
    }

    // Preview thumbnail (what the capture thread builds every Preview-FPS tick).
    {
        std::vector<uint8_t> rgb((size_t)960 * 540 * 3);
        run("thumb_old_1080p_to_540p", [&] { thumb_old(bgra, W, H, rgb, 960, 540); });
        run("thumb_new_1080p_to_540p", [&] { hr_bgra_to_thumb(bgra.data(), 0, rgb.data(), W, H, 960, 540); });
        run("thumb_old_1080p_to_800x450", [&] { thumb_old(bgra, W, H, rgb, 800, 450); });
        run("thumb_new_1080p_to_800x450", [&] { hr_bgra_to_thumb(bgra.data(), 0, rgb.data(), W, H, 800, 450); });
        std::printf("[pair] preview_thumb_540p thumb_old_1080p_to_540p thumb_new_1080p_to_540p\n");
        std::printf("[pair] preview_thumb_450p thumb_old_1080p_to_800x450 thumb_new_1080p_to_800x450\n");
    }

    // Window / crop capture: a 1280x720 rectangle of the 1080p frame, copied out first (old) or read in place (new).
    {
        const int cx = 200, cy = 100, cw = 1280, ch = 720;
        std::vector<uint8_t> work((size_t)cw * ch * 4), cn((size_t)cw * ch * 3 / 2);
        run("crop_copy_then_cvt_720p", [&] {
            for (int y = 0; y < ch; ++y) std::memcpy(&work[(size_t)y * cw * 4], &bgra[((size_t)(cy + y) * W + cx) * 4], (size_t)cw * 4);
            hr_bgra_to_nv12_band(work.data(), cn.data(), cw, ch, 0, ch);
        });
        run("crop_inplace_cvt_720p", [&] {
            hr_bgra_to_nv12_band_s(&bgra[((size_t)cy * W + cx) * 4], (size_t)W * 4, cn.data(), cw, ch, 0, ch);
        });
        std::printf("[pair] crop_convert crop_copy_then_cvt_720p crop_inplace_cvt_720p\n");
    }

    // Keeping the converted picture: the old per-frame 3 MB copies versus handing the same buffer over.
    {
        std::vector<uint8_t> a((size_t)W * H * 3 / 2, 1), b(a.size());
        run("yuv_copy_3mb", [&] { std::memcpy(b.data(), a.data(), a.size()); b[0] ^= 1; });
        run("yuv_share_3mb", [&] { b[0] ^= 1; });      // a shared picture: no copy, only the bookkeeping
        std::printf("[pair] yuv_keep yuv_copy_3mb yuv_share_3mb\n");
    }
    return 0;
}
