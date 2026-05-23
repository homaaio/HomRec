/*
 * hr_preview.cpp  -  HomRec v1.5.0  preview pipeline helpers
 *
 * Fast pixel-level operations used exclusively by the preview thread.
 * Kept separate from homrec_core.c so the preview can be compiled with
 * different optimisation flags (e.g. AVX2) without affecting the audio core.
 *
 * API:
 *   hr_pv_thumbnail(src, dst, sw, sh, dw, dh)
 *       Downscale an RGB image using a 2x2 box filter (better quality than
 *       nearest-neighbour for preview, cheaper than full bilinear).
 *
 *   hr_pv_draw_border(pixels, w, h, stride, r, g, b, thickness)
 *       Draw a solid-colour border rectangle in-place.
 *       Used for the red "recording" flash effect without Tkinter overhead.
 *
 *   hr_pv_gray_overlay(pixels, w, h, alpha)
 *       Blend a grey veil over the frame (paused state visual).
 *
 *   hr_pv_watermark_text is NOT here - text rendering needs a font library.
 *   Use Pillow / tkinter for that.
 *
 * BUG FIXES vs previous version:
 *   - restrict keyword is C99-only; replaced with HR_RESTRICT macro that
 *     expands to __restrict__ (GCC/Clang) or __restrict (MSVC).
 *     The previous code failed to compile on MinGW g++ because the compiler
 *     treated `restrict` as an unknown identifier, breaking all parameter
 *     lists that used it — causing cascading "not declared in this scope"
 *     errors for every subsequent parameter and local variable.
 *   - hr_pv_draw_border: thickness fallback now clamps to min(w,h)/2-1
 *     instead of hard-coding 1, which was wrong for small images.
 *
 * Compile (Linux):
 *   g++ -O3 -std=c++17 -shared -fPIC -o hr_preview.so hr_preview.cpp
 *
 * Compile (Windows MinGW):
 *   g++ -O3 -std=c++17 -shared -static-libgcc -static-libstdc++ \
 *       -o hr_preview.dll hr_preview.cpp
 */

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
  #define HR_EXPORT extern "C" __declspec(dllexport)
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

/* BUG FIX: `restrict` is a C99 keyword, not valid C++.
 * Use compiler-specific spellings instead. */
#if defined(__GNUC__) || defined(__clang__)
  #define HR_RESTRICT __restrict__
#elif defined(_MSC_VER)
  #define HR_RESTRICT __restrict
#else
  #define HR_RESTRICT
#endif

/* -------------------------------------------------------------------------
 * Box-filter 2x2 thumbnail (better than nearest for UI preview)
 *
 * src    : source RGB pixels  (sw * sh * 3 bytes)
 * dst    : dest   RGB pixels  (dw * dh * 3 bytes, caller-allocated)
 * sw, sh : source dimensions
 * dw, dh : destination dimensions
 *
 * For arbitrary scale ratios we use a single-sample bilinear; the 2x2 box
 * special case only fires when scale is exactly 0.5x (common for 4K->1080p).
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_pv_thumbnail(const uint8_t * HR_RESTRICT src,
                                uint8_t       * HR_RESTRICT dst,
                                int sw, int sh,
                                int dw, int dh)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    /* Fast 2x2 box-filter path for exact 2:1 downscale */
    if (sw == dw * 2 && sh == dh * 2) {
        for (int y = 0; y < dh; ++y) {
            const uint8_t *row0 = src + (size_t)(y * 2    ) * sw * 3;
            const uint8_t *row1 = src + (size_t)(y * 2 + 1) * sw * 3;
            uint8_t *out = dst + (size_t)y * dw * 3;
            for (int x = 0; x < dw; ++x) {
                int x2 = x * 2;
                for (int c = 0; c < 3; ++c) {
                    out[x * 3 + c] = (uint8_t)(
                        ((uint32_t)row0[x2 * 3 + c]
                       + (uint32_t)row0[(x2 + 1) * 3 + c]
                       + (uint32_t)row1[x2 * 3 + c]
                       + (uint32_t)row1[(x2 + 1) * 3 + c] + 2u) >> 2u);
                }
            }
        }
        return;
    }

    /* General bilinear path */
    float xr = (dw > 1) ? (float)(sw - 1) / (float)(dw - 1) : 0.f;
    float yr = (dh > 1) ? (float)(sh - 1) / (float)(dh - 1) : 0.f;

    for (int y = 0; y < dh; ++y) {
        float fy  = y * yr;
        int   y0  = (int)fy;
        int   y1  = (y0 + 1 < sh) ? y0 + 1 : sh - 1;
        float dy  = fy - (float)y0;
        float dy1 = 1.f - dy;

        for (int x = 0; x < dw; ++x) {
            float fx  = x * xr;
            int   x0  = (int)fx;
            int   x1  = (x0 + 1 < sw) ? x0 + 1 : sw - 1;
            float dx  = fx - (float)x0;
            float dx1 = 1.f - dx;

            const uint8_t *p00 = src + ((size_t)y0 * sw + x0) * 3;
            const uint8_t *p01 = src + ((size_t)y0 * sw + x1) * 3;
            const uint8_t *p10 = src + ((size_t)y1 * sw + x0) * 3;
            const uint8_t *p11 = src + ((size_t)y1 * sw + x1) * 3;

            uint8_t *out = dst + ((size_t)y * dw + x) * 3;
            for (int c = 0; c < 3; ++c) {
                float v = dy1 * (dx1 * p00[c] + dx * p01[c])
                        + dy  * (dx1 * p10[c] + dx * p11[c]);
                out[c] = (uint8_t)(v + 0.5f);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Draw solid border rectangle in-place on an RGB frame.
 * Used for the red "recording active" flash.
 *
 * pixels    : RGB buffer (w * h * 3 bytes)
 * w, h      : image dimensions
 * stride    : row stride in bytes (usually w * 3)
 * r, g, b   : border colour
 * thickness : border width in pixels
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_pv_draw_border(uint8_t *pixels,
                                  int w, int h, int stride,
                                  uint8_t r, uint8_t g, uint8_t b,
                                  int thickness)
{
    if (!pixels || w <= 0 || h <= 0 || thickness <= 0) return;

    /* BUG FIX: previous code clamped to hard-coded 1 whenever thickness
     * exceeded w/2 or h/2, which was wrong for small images.
     * Now clamp to the actual maximum sensible thickness. */
    int max_t = std::min(w, h) / 2 - 1;
    int t = (max_t > 0) ? std::min(thickness, max_t) : 1;

    auto fill_row = [&](int y, int x0, int x1) {
        if (y < 0 || y >= h) return;
        uint8_t *row = pixels + (size_t)y * stride;
        for (int x = x0; x < x1 && x < w; ++x) {
            row[x * 3 + 0] = r;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = b;
        }
    };

    /* Top and bottom bands */
    for (int i = 0; i < t; ++i) {
        fill_row(i,         0, w);
        fill_row(h - 1 - i, 0, w);
    }

    /* Left and right bands (excluding corners already filled) */
    for (int y = t; y < h - t; ++y) {
        uint8_t *row = pixels + (size_t)y * stride;
        for (int i = 0; i < t; ++i) {
            /* Left */
            row[i * 3 + 0] = r; row[i * 3 + 1] = g; row[i * 3 + 2] = b;
            /* Right */
            int rx = (w - 1 - i) * 3;
            row[rx + 0] = r; row[rx + 1] = g; row[rx + 2] = b;
        }
    }
}

/* -------------------------------------------------------------------------
 * Grey veil overlay (paused state)
 * Blends a neutral grey over every pixel at given alpha (0-255).
 * pixels : RGB buffer modified in-place
 * n_pix  : total pixel count (w * h)
 * alpha  : 0 = no effect, 255 = full grey
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_pv_gray_overlay(uint8_t *pixels, size_t n_pix,
                                   uint8_t alpha)
{
    if (!pixels || alpha == 0) return;

    uint32_t a  = (uint32_t)alpha;
    uint32_t a1 = 255u - a;
    /* 128 grey target */
    const uint32_t grey = 128u;

    for (size_t i = 0; i < n_pix * 3u; ++i) {
        pixels[i] = (uint8_t)((grey * a + (uint32_t)pixels[i] * a1 + 128u) >> 8u);
    }
}

/* -------------------------------------------------------------------------
 * Horizontal flip (mirror) - used for webcam overlay if needed
 * pixels : RGB buffer modified in-place
 * w, h   : dimensions
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_pv_flip_horizontal(uint8_t *pixels, int w, int h)
{
    if (!pixels || w <= 1) return;
    for (int y = 0; y < h; ++y) {
        uint8_t *row = pixels + (size_t)y * w * 3;
        int lo = 0, hi = w - 1;
        while (lo < hi) {
            /* Swap pixels at lo and hi */
            uint8_t tmp[3];
            memcpy(tmp,           row + lo * 3, 3);
            memcpy(row + lo * 3,  row + hi * 3, 3);
            memcpy(row + hi * 3,  tmp,          3);
            ++lo; --hi;
        }
    }
}
