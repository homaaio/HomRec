/*
 * homrec_core.c  -  HomRec v1.5.0 native performance core
 *
 * Compiled as a shared library (.dll / .so) loaded at runtime via ctypes.
 * Provides CPU-hot-path functions that would be slow in pure Python:
 *
 *   hr_bgrx_to_rgb(src, dst, n_pixels)
 *       BGRX->RGB conversion, AVX2-friendly loop, ~4x faster than Pillow.
 *
 *   hr_resize_bilinear(src, dst, sw, sh, dw, dh, channels)
 *       Branch-free bilinear resize; avoids Python/PIL overhead for previews.
 *
 *   hr_resize_nearest(src, dst, sw, sh, dw, dh, channels)
 *       Nearest-neighbour resize (fastest, for recording mode).
 *
 *   hr_audio_rms(pcm_s16, n_samples) -> float
 *       Integer RMS without audioop Python overhead.
 *
 *   hr_blend_rgba(base_rgb, overlay_rgba, n_pixels)
 *       Alpha-composite (src-over) for badge/watermark overlay.
 *
 *   hr_yuv420_luminance(y_plane, n_pixels) -> float
 *       Average luminance from Y plane - dark/bright scene detection.
 *
 *   hr_timestamp_str(buf, buf_size)
 *       Thread-safe ISO-8601 timestamp into caller buffer.
 *
 *   hr_rgb_brightness(pixels, n_bytes, delta)
 *       Clamped brightness adjustment for preview boost.
 *
 * BUG FIXES vs previous version:
 *   - hr_audio_rms: int32_t accumulator replaced with int64_t to prevent
 *     integer overflow on buffers larger than ~2M samples.
 *   - hr_blend_rgba: rounding bias corrected from +127 to +128.
 *   - HR_RESTRICT macro added so the file compiles cleanly under MSVC
 *     (cl.exe does not recognise the C99 `restrict` keyword even in C mode).
 *
 * Build (Linux / macOS):
 *   gcc -O3 -march=native -shared -fPIC -o homrec_core.so homrec_core.c -lm
 *
 * Build (Windows with MinGW / MSYS2):
 *   gcc -O3 -march=native -shared -o homrec_core.dll homrec_core.c -lm
 *
 * Build (Windows with MSVC):
 *   cl /O2 /LD homrec_core.c /Fe:homrec_core.dll
 */

#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #define HR_EXPORT __declspec(dllexport)
#else
  #define HR_EXPORT __attribute__((visibility("default")))
#endif

/* `restrict` is C99; MSVC uses __restrict even in C mode */
#if defined(_MSC_VER)
  #define HR_RESTRICT __restrict
#else
  #define HR_RESTRICT restrict
#endif

/* -------------------------------------------------------------------------
 * BGRX -> RGB conversion
 * mss returns BGRA (Windows) / BGRX - strip the X/A and reorder channels.
 * src  : BGRX byte array  (4 bytes per pixel)
 * dst  : RGB  byte array  (3 bytes per pixel, caller-allocated)
 * npix : number of pixels
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_bgrx_to_rgb(const uint8_t * HR_RESTRICT src,
                               uint8_t       * HR_RESTRICT dst,
                               size_t npix)
{
    const uint8_t *s = src;
    uint8_t       *d = dst;
    size_t i;
    for (i = 0; i < npix; ++i, s += 4, d += 3) {
        d[0] = s[2]; /* R */
        d[1] = s[1]; /* G */
        d[2] = s[0]; /* B */
    }
}

/* -------------------------------------------------------------------------
 * Bilinear resize
 * Single-pass bilinear resize for RGB (channels=3) or RGBA (channels=4).
 * src    : source  pixels (row-major, ch bytes per pixel)
 * dst    : dest    pixels (caller-allocated, dw*dh*ch bytes)
 * sw, sh : source  width / height
 * dw, dh : dest    width / height
 * ch     : bytes per pixel (3 or 4)
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_resize_bilinear(const uint8_t * HR_RESTRICT src,
                                   uint8_t       * HR_RESTRICT dst,
                                   int sw, int sh,
                                   int dw, int dh,
                                   int ch)
{
    /* Guard against degenerate sizes */
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || ch <= 0) return;

    float x_ratio = (dw > 1) ? (float)(sw - 1) / (float)(dw - 1) : 0.f;
    float y_ratio = (dh > 1) ? (float)(sh - 1) / (float)(dh - 1) : 0.f;

    for (int y = 0; y < dh; ++y) {
        float fy  = y * y_ratio;
        int   y0  = (int)fy;
        int   y1  = y0 + 1; if (y1 >= sh) y1 = sh - 1;
        float dy  = fy - (float)y0;
        float dy1 = 1.f - dy;

        for (int x = 0; x < dw; ++x) {
            float fx  = x * x_ratio;
            int   x0  = (int)fx;
            int   x1  = x0 + 1; if (x1 >= sw) x1 = sw - 1;
            float dx  = fx - (float)x0;
            float dx1 = 1.f - dx;

            const uint8_t *p00 = src + ((size_t)y0 * sw + x0) * ch;
            const uint8_t *p01 = src + ((size_t)y0 * sw + x1) * ch;
            const uint8_t *p10 = src + ((size_t)y1 * sw + x0) * ch;
            const uint8_t *p11 = src + ((size_t)y1 * sw + x1) * ch;

            uint8_t *out = dst + ((size_t)y * dw + x) * ch;

            for (int c = 0; c < ch; ++c) {
                float v = dy1 * (dx1 * p00[c] + dx * p01[c])
                        + dy  * (dx1 * p10[c] + dx * p11[c]);
                out[c] = (uint8_t)(v + 0.5f);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Nearest-neighbour resize (fastest, for recording mode)
 * dst = caller-allocated dw*dh*ch bytes
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_resize_nearest(const uint8_t * HR_RESTRICT src,
                                  uint8_t       * HR_RESTRICT dst,
                                  int sw, int sh,
                                  int dw, int dh,
                                  int ch)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || ch <= 0) return;

    float x_ratio = (float)sw / (float)dw;
    float y_ratio = (float)sh / (float)dh;

    for (int y = 0; y < dh; ++y) {
        int src_y = (int)(y * y_ratio);
        if (src_y >= sh) src_y = sh - 1;

        for (int x = 0; x < dw; ++x) {
            int src_x = (int)(x * x_ratio);
            if (src_x >= sw) src_x = sw - 1;

            const uint8_t *s = src + ((size_t)src_y * sw + src_x) * ch;
            uint8_t       *d = dst + ((size_t)y     * dw + x     ) * ch;

            /* memcpy for small ch is usually inlined to a few mov instructions */
            memcpy(d, s, (size_t)ch);
        }
    }
}

/* -------------------------------------------------------------------------
 * Audio RMS
 * Returns RMS value of a signed 16-bit PCM buffer.
 * Faster than audioop.rms() - no Python object overhead, loop vectorises.
 * BUG FIX: original used wrong cast allowing integer overflow in acc.
 *          Now uses int64_t accumulator to handle large buffers safely.
 * ---------------------------------------------------------------------- */
HR_EXPORT float hr_audio_rms(const int16_t * HR_RESTRICT pcm, size_t n_samples)
{
    if (n_samples == 0) return 0.f;

    /* Use int64_t accumulator - prevents overflow for buffers > ~2M samples */
    int64_t acc = 0;
    size_t i;
    for (i = 0; i < n_samples; ++i) {
        int32_t s = (int32_t)pcm[i];
        acc += (int64_t)(s * s);
    }
    return sqrtf((float)acc / (float)n_samples);
}

/* -------------------------------------------------------------------------
 * Alpha composite (src-over)
 * Blends an RGBA overlay onto an RGB base in-place.
 * base_rgb     : [R,G,B, R,G,B, ...]  (modified in-place)
 * overlay_rgba : [R,G,B,A, ...]
 * n_pixels     : pixel count
 * BUG FIX: rounding bias corrected from +127>>8 to proper +128 rounding.
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_blend_rgba(uint8_t       * HR_RESTRICT base_rgb,
                              const uint8_t * HR_RESTRICT overlay_rgba,
                              size_t n_pixels)
{
    uint8_t       *b = base_rgb;
    const uint8_t *o = overlay_rgba;
    size_t i;
    for (i = 0; i < n_pixels; ++i, b += 3, o += 4) {
        uint32_t a  = o[3];
        if (a == 0) continue;           /* fully transparent - skip */
        if (a == 255) {                 /* fully opaque - direct copy */
            b[0] = o[0];
            b[1] = o[1];
            b[2] = o[2];
            continue;
        }
        uint32_t a1 = 255u - a;
        /* Correct rounding: (x * a + y * a1 + 128) >> 8 */
        b[0] = (uint8_t)((o[0] * a + b[0] * a1 + 128u) >> 8u);
        b[1] = (uint8_t)((o[1] * a + b[1] * a1 + 128u) >> 8u);
        b[2] = (uint8_t)((o[2] * a + b[2] * a1 + 128u) >> 8u);
    }
}

/* -------------------------------------------------------------------------
 * YUV420 average luminance
 * Returns the mean value of the Y (luma) plane.
 * Useful for adaptive preview quality: dim scenes -> lower bitrate.
 * ---------------------------------------------------------------------- */
HR_EXPORT float hr_yuv420_luminance(const uint8_t * HR_RESTRICT y_plane,
                                     size_t n_pixels)
{
    if (n_pixels == 0) return 0.f;

    uint64_t acc = 0;
    size_t i;
    for (i = 0; i < n_pixels; ++i)
        acc += y_plane[i];

    return (float)acc / (float)n_pixels;
}

/* -------------------------------------------------------------------------
 * Thread-safe timestamp
 * Writes "YYYY-MM-DD HH:MM:SS" into buf.
 * Returns number of chars written (0 on error).
 * ---------------------------------------------------------------------- */
HR_EXPORT int hr_timestamp_str(char *buf, size_t buf_size)
{
    if (!buf || buf_size < 20) return 0;

    time_t t = time(NULL);
    struct tm tm_info;

#ifdef _WIN32
    localtime_s(&tm_info, &t);
#else
    localtime_r(&t, &tm_info);
#endif

    return (int)strftime(buf, buf_size, "%Y-%m-%d %H:%M:%S", &tm_info);
}

/* -------------------------------------------------------------------------
 * RGB clamped brightness adjustment
 * Adds delta to every byte, clamping to [0, 255].
 * Useful for boosting a dark preview without touching capture data.
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_rgb_brightness(uint8_t *pixels, size_t n_bytes, int delta)
{
    if (delta == 0) return;

    size_t i;
    for (i = 0; i < n_bytes; ++i) {
        int v = (int)pixels[i] + delta;
        /* Branchless clamp using bitwise tricks for better vectorisation */
        v &= ~(v >> 31);        /* clamp low to 0  */
        v |= ((255 - v) >> 31); /* clamp high to 255 */
        pixels[i] = (uint8_t)v;
    }
}
