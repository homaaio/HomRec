// cursor_blend.h - draws the rasterized mouse pointer straight into an already-converted frame.
//
// Why: the classic (CPU) capture path used to draw the pointer into the BGRA capture buffer. That
// forced an 8 MB copy of the whole 1080p frame every tick (the capture buffer must stay pristine
// for DXGI's dirty-rectangle logic) and made the colour conversion a full-frame job even when
// nothing but the pointer moved. Blending into the 3 MB YUV frame instead (like the GPU path
// already did) removes the copy and keeps the cached clean picture reusable.
//
// No platform headers: the Windows-only part (GetCursorInfo + the GDI cursor cache) stays in
// ui_utils.cpp and just hands the bitmap over.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace hrcursor {

struct Bitmap {
    const uint8_t *bgra = nullptr;   // straight (non-premultiplied) BGRA, cw*ch*4
    const uint8_t *inv  = nullptr;   // cw*ch, 1 = "invert what is underneath" (XOR cursors); may be null
    int cw = 0, ch = 0;
};

namespace detail {
struct Csc {                       // same constants as encoder_helpers.c (scaled by 65536)
    int yr, yg, yb, yoff;
    int cbr, cbg, cbb;
    int crr, crg, crb;
    int ymin, ymax;                // legal luma range, used by the XOR "invert" mirror
    int ymirror;                   // ymin + ymax
};
inline const Csc &csc(bool full_range)
{
    static const Csc kTv   = {16829, 33039, 6416, 16, -9714, -19071, 28785, 28785, -24103, -4682, 16, 235, 251};
    static const Csc kFull = {19595, 38470, 7471,  0, -11059, -21709, 32768, 32768, -27439, -5329,  0, 255, 255};
    return full_range ? kFull : kTv;
}
inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
} // namespace detail

// Blend `bm` (stretched to dw x dh, top-left at lx,ly in frame pixels) into a 4:2:0 frame.
//   nv12 = true : Y plane followed by one interleaved UV plane (stride = width)
//   nv12 = false: Y plane, then the U plane, then the V plane (stride = width / 2)
//   full_range  : false = studio swing (what the NV12 / hardware-encoder path writes),
//                 true  = full swing (what the I420 / software-encoder path writes)
// The chroma of every 2x2 block is the coverage-weighted mean of the pointer's colour, laid over
// the existing chroma - the same maths hr_bgra_to_nv12_band / hr_bgra_to_yuv420p use.
inline void blend_yuv420(uint8_t *yuv, int width, int height, bool nv12, bool full_range,
                         const Bitmap &bm, int lx, int ly, int dw, int dh)
{
    if (!yuv || !bm.bgra || bm.cw <= 0 || bm.ch <= 0 || width < 2 || height < 2 || dw < 1 || dh < 1) return;
    if (lx + dw <= 0 || ly + dh <= 0 || lx >= width || ly >= height) return;   // fully off-frame

    const detail::Csc &k = detail::csc(full_range);
    const int cw = bm.cw, ch = bm.ch;
    uint8_t *Yp = yuv;
    uint8_t *Up = yuv + (size_t)width * (size_t)height;
    uint8_t *Vp = nv12 ? nullptr : Up + (size_t)(width / 2) * (size_t)(height / 2);
    const size_t cstride = nv12 ? (size_t)width : (size_t)(width / 2);

    const int bx0 = std::max(lx, 0) & ~1;
    const int by0 = std::max(ly, 0) & ~1;
    const int bx1 = std::min(lx + dw, width);
    const int by1 = std::min(ly + dh, height);
    const int yround = 32768 + (k.yoff << 16);

    for (int by = by0; by < by1; by += 2) {
        for (int bx = bx0; bx < bx1; bx += 2) {
            int sum_a = 0, rs = 0, gs = 0, bs = 0;   // alpha-weighted colour of this 2x2 block
            int inv_cnt = 0;                          // XOR "invert" pixels in this block
            for (int j = 0; j < 2; ++j) {
                const int py = by + j;
                if (py < ly || py >= ly + dh || py >= height) continue;
                const int sy = (int)((int64_t)(py - ly) * ch / dh);
                for (int i = 0; i < 2; ++i) {
                    const int px = bx + i;
                    if (px < lx || px >= lx + dw || px >= width) continue;
                    const int sx = (int)((int64_t)(px - lx) * cw / dw);
                    const size_t sidx = (size_t)sy * cw + sx;
                    const uint8_t *sp = bm.bgra + sidx * 4;
                    const int a = sp[3];
                    uint8_t &yd = Yp[(size_t)py * width + px];
                    if (a == 0) {                         // fully transparent... or an invert pixel
                        if (bm.inv && bm.inv[sidx]) {
                            int yi = k.ymirror - (int)yd;
                            yd = (uint8_t)std::max(k.ymin, std::min(k.ymax, yi));
                            ++inv_cnt;
                        }
                        continue;
                    }
                    const int b = sp[0], g = sp[1], r = sp[2];
                    const int yc = (k.yr * r + k.yg * g + k.yb * b + yround) >> 16;
                    yd = (uint8_t)((yc * a + (int)yd * (255 - a) + 127) / 255);   // straight-alpha blend
                    rs += r * a; gs += g * a; bs += b * a; sum_a += a;
                }
            }
            const size_t cidx = (size_t)(by / 2) * cstride + (size_t)(nv12 ? bx : bx / 2);
            uint8_t *pu = Up + cidx;
            uint8_t *pv = nv12 ? Up + cidx + 1 : Vp + cidx;
            if (inv_cnt >= 2) {                       // chroma of an inverted area mirrors around 128
                *pu = (uint8_t)detail::clamp8(256 - (int)*pu);
                *pv = (uint8_t)detail::clamp8(256 - (int)*pv);
            }
            if (sum_a == 0) continue;
            // Mean cursor colour of the block, scaled to a 4-pixel sum exactly like the 2x2 sums
            // the converters feed their chroma coefficients.
            const int r4 = rs * 4 / sum_a, g4 = gs * 4 / sum_a, b4 = bs * 4 / sum_a;
            const int cb = detail::clamp8(((k.cbr * r4 + k.cbg * g4 + k.cbb * b4 + (1 << 17)) >> 18) + 128);
            const int cr = detail::clamp8(((k.crr * r4 + k.crg * g4 + k.crb * b4 + (1 << 17)) >> 18) + 128);
            // Coverage of the block (0..1020 = 4 pixels * 255): keep the underlying chroma where the
            // pointer is transparent or only partly covers.
            const int cov = std::min(sum_a, 1020);
            *pu = (uint8_t)((*pu * (1020 - cov) + cb * cov + 510) / 1020);
            *pv = (uint8_t)((*pv * (1020 - cov) + cr * cov + 510) / 1020);
        }
    }
}

// Same for a packed RGB24 buffer (the live-preview thumbnail: R,G,B byte order, no row padding).
inline void blend_rgb24(uint8_t *rgb, int width, int height,
                        const Bitmap &bm, int lx, int ly, int dw, int dh)
{
    if (!rgb || !bm.bgra || bm.cw <= 0 || bm.ch <= 0 || width < 1 || height < 1 || dw < 1 || dh < 1) return;
    if (lx + dw <= 0 || ly + dh <= 0 || lx >= width || ly >= height) return;
    const int cw = bm.cw, ch = bm.ch;
    const int x0 = std::max(lx, 0), x1 = std::min(lx + dw, width);
    const int y0 = std::max(ly, 0), y1 = std::min(ly + dh, height);
    for (int py = y0; py < y1; ++py) {
        const int sy = (int)((int64_t)(py - ly) * ch / dh);
        uint8_t *drow = rgb + (size_t)py * width * 3;
        for (int px = x0; px < x1; ++px) {
            const int sx = (int)((int64_t)(px - lx) * cw / dw);
            const size_t sidx = (size_t)sy * cw + sx;
            const uint8_t *sp = bm.bgra + sidx * 4;
            const int a = sp[3];
            uint8_t *dp = drow + (size_t)px * 3;
            if (a == 0) {
                if (bm.inv && bm.inv[sidx]) { dp[0] = (uint8_t)~dp[0]; dp[1] = (uint8_t)~dp[1]; dp[2] = (uint8_t)~dp[2]; }
                continue;
            }
            if (a == 255) { dp[0] = sp[2]; dp[1] = sp[1]; dp[2] = sp[0]; continue; }
            const int ia = 255 - a;
            dp[0] = (uint8_t)((sp[2] * a + dp[0] * ia + 127) / 255);
            dp[1] = (uint8_t)((sp[1] * a + dp[1] * ia + 127) / 255);
            dp[2] = (uint8_t)((sp[0] * a + dp[2] * ia + 127) / 255);
        }
    }
}

} // namespace hrcursor
