/*
 * hr_encoder_helpers.c  -  HomRec v1.6.0  encoder pipeline helpers
 *
 * Fast pixel-format conversion and frame-preparation helpers used between
 * screen capture and the FFmpeg stdin pipe. Keeping these in C lets the
 * compiler auto-vectorise inner loops without Python overhead.
 *
 * API:
 *   hr_rgb_to_yuv420p(rgb, yuv_out, width, height)
 *       Convert packed RGB24 to planar YUV420p (I420) in-place.
 *       This is what FFmpeg rawvideo expects via the stdin pipe, saving
 *       FFmpeg from doing the conversion itself on the first filter stage.
 *
 *   hr_bgra_to_yuv420p(bgra, yuv_out, width, height)
 *       BGRA (mss output) direct to YUV420p – skips the intermediate
 *       RGB step that homrec_core.c needed before.
 *
 *   hr_yuv420p_to_rgb(yuv, rgb_out, width, height)
 *       YUV420p → RGB24 (for preview reconstruction from encoded frames).
 *
 *   hr_gamma_lut_apply(pixels, n_bytes, gamma_x100)
 *       Apply a gamma correction look-up table. gamma_x100 = 100 means
 *       gamma 1.0 (no-op); 80 = gamma 0.80 (brighten), 120 = 1.20 (darken).
 *
 *   hr_build_thumbnail_lq(src, dst, sw, sh, dw, dh)
 *       Fast integer-only thumbnail using average-box decimation (2x2 or 4x4
 *       blocks). Faster than the bilinear path in homrec_core for the common
 *       4K→720p preview case. Falls back to nearest-neighbour for odd ratios.
 *
 *   hr_memcpy_nt(dst, src, n)
 *       Non-temporal (streaming) memcpy on x86 – avoids cache pollution when
 *       copying large frame buffers. Falls back to regular memcpy on ARM.
 *
 * Build (Linux / macOS):
 *   gcc -O3 -march=native -shared -fPIC -o hr_encoder_helpers.so hr_encoder_helpers.c -lm
 *
 * Build (Windows MinGW):
 *   gcc -O3 -march=native -shared -o hr_encoder_helpers.dll hr_encoder_helpers.c -lm
 *
 * Build (MSVC):
 *   cl /O2 /LD hr_encoder_helpers.c /Fe:hr_encoder_helpers.dll
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
  #define HR_EXPORT __declspec(dllexport)
#else
  #define HR_EXPORT __attribute__((visibility("default")))
#endif

#if defined(_MSC_VER)
  #define HR_RESTRICT __restrict
  #define HR_INLINE   __forceinline
#else
  #define HR_RESTRICT restrict
  #define HR_INLINE   __attribute__((always_inline)) inline
#endif

/* -------------------------------------------------------------------------
 * BT.601 integer YCbCr coefficients (scaled by 2^16 for fixed-point maths)
 * Y  =  0.2990 R + 0.5870 G + 0.1140 B
 * Cb = -0.1687 R - 0.3313 G + 0.5000 B + 128
 * Cr =  0.5000 R - 0.4187 G - 0.0813 B + 128
 * ---------------------------------------------------------------------- */
#define YR  19595   /* 0.2990 * 65536 */
#define YG  38470   /* 0.5870 * 65536 */
#define YB   7471   /* 0.1140 * 65536 */
#define CBR  11059  /* 0.1687 * 65536 */
#define CBG  21709  /* 0.3313 * 65536 */
#define CBB  32768  /* 0.5000 * 65536 */
#define CRR  32768  /* 0.5000 * 65536 */
#define CRG  27439  /* 0.4187 * 65536 */
#define CRB   5329  /* 0.0813 * 65536 */

static HR_INLINE uint8_t _clamp8(int v) {
    return (v < 0) ? 0 : (v > 255) ? 255 : (uint8_t)v;
}

/* -------------------------------------------------------------------------
 * RGB24 -> YUV420p (I420)
 * Output layout: Y plane (w*h), Cb plane (w*h/4), Cr plane (w*h/4)
 * yuv_out must be pre-allocated to w*h*3/2 bytes.
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_rgb_to_yuv420p(
    const uint8_t * HR_RESTRICT rgb,
    uint8_t       * HR_RESTRICT yuv_out,
    int width, int height)
{
    if (!rgb || !yuv_out || width <= 0 || height <= 0) return;

    size_t frame_sz = (size_t)width * (size_t)height;
    uint8_t *Y  = yuv_out;
    uint8_t *Cb = yuv_out + frame_sz;
    uint8_t *Cr = yuv_out + frame_sz + frame_sz / 4;

    for (int y = 0; y < height; ++y) {
        const uint8_t *row = rgb + (size_t)y * width * 3;
        for (int x = 0; x < width; ++x) {
            uint8_t r = row[x * 3 + 0];
            uint8_t g = row[x * 3 + 1];
            uint8_t b = row[x * 3 + 2];

            /* Luma - every pixel */
            Y[y * width + x] = (uint8_t)((YR*r + YG*g + YB*b + 32768) >> 16);

            /* Chroma - once per 2x2 block (top-left pixel of each block) */
            if ((y & 1) == 0 && (x & 1) == 0) {
                int cb = (int)(-CBR*r - CBG*g + CBB*b + 32768) >> 16;
                int cr = (int)( CRR*r - CRG*g - CRB*b + 32768) >> 16;
                size_t ci = (size_t)(y/2) * (size_t)(width/2) + (size_t)(x/2);
                Cb[ci] = _clamp8(cb + 128);
                Cr[ci] = _clamp8(cr + 128);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * BGRA -> YUV420p (I420)
 * Direct from mss grab buffer, saves one copy compared to BGRX->RGB->YUV.
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_bgra_to_yuv420p(
    const uint8_t * HR_RESTRICT bgra,
    uint8_t       * HR_RESTRICT yuv_out,
    int width, int height)
{
    if (!bgra || !yuv_out || width <= 0 || height <= 0) return;

    size_t frame_sz = (size_t)width * (size_t)height;
    uint8_t *Y  = yuv_out;
    uint8_t *Cb = yuv_out + frame_sz;
    uint8_t *Cr = yuv_out + frame_sz + frame_sz / 4;

    for (int y = 0; y < height; ++y) {
        const uint8_t *row = bgra + (size_t)y * width * 4;
        for (int x = 0; x < width; ++x) {
            /* BGRA: B=0, G=1, R=2, A=3 */
            uint8_t b = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t r = row[x * 4 + 2];

            Y[y * width + x] = (uint8_t)((YR*r + YG*g + YB*b + 32768) >> 16);

            if ((y & 1) == 0 && (x & 1) == 0) {
                int cb = (int)(-CBR*r - CBG*g + CBB*b + 32768) >> 16;
                int cr = (int)( CRR*r - CRG*g - CRB*b + 32768) >> 16;
                size_t ci = (size_t)(y/2) * (size_t)(width/2) + (size_t)(x/2);
                Cb[ci] = _clamp8(cb + 128);
                Cr[ci] = _clamp8(cr + 128);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * YUV420p -> RGB24 (for preview reconstruction)
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_yuv420p_to_rgb(
    const uint8_t * HR_RESTRICT yuv,
    uint8_t       * HR_RESTRICT rgb_out,
    int width, int height)
{
    if (!yuv || !rgb_out || width <= 0 || height <= 0) return;

    size_t frame_sz = (size_t)width * (size_t)height;
    const uint8_t *Y  = yuv;
    const uint8_t *Cb = yuv + frame_sz;
    const uint8_t *Cr = yuv + frame_sz + frame_sz / 4;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int luma = (int)Y[y * width + x] - 16;
            size_t ci = (size_t)(y/2) * (size_t)(width/2) + (size_t)(x/2);
            int cb = (int)Cb[ci] - 128;
            int cr = (int)Cr[ci] - 128;

            int r = (298 * luma               + 409 * cr + 128) >> 8;
            int g = (298 * luma - 100 * cb - 208 * cr + 128) >> 8;
            int b = (298 * luma + 516 * cb               + 128) >> 8;

            uint8_t *out = rgb_out + ((size_t)y * width + x) * 3;
            out[0] = _clamp8(r);
            out[1] = _clamp8(g);
            out[2] = _clamp8(b);
        }
    }
}

/* -------------------------------------------------------------------------
 * Gamma correction via pre-computed LUT
 * gamma_x100: 100 = 1.0 (no-op), 80 = 0.80 (brighten), 120 = 1.20 (darken)
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_gamma_lut_apply(
    uint8_t *pixels, size_t n_bytes, int gamma_x100)
{
    if (!pixels || n_bytes == 0 || gamma_x100 == 100) return;

    /* Build 256-entry LUT once per call (tiny cost vs loop over megapixels) */
    uint8_t lut[256];
    double g = (double)gamma_x100 / 100.0;
    for (int i = 0; i < 256; ++i) {
        double v = pow((double)i / 255.0, g) * 255.0 + 0.5;
        lut[i] = (v > 255.0) ? 255 : (uint8_t)v;
    }

    for (size_t i = 0; i < n_bytes; ++i)
        pixels[i] = lut[pixels[i]];
}

/* -------------------------------------------------------------------------
 * Fast integer-box thumbnail
 * Only handles even downscale ratios (2x, 4x, 8x).
 * For 4K→1080p (2x): each output pixel averages a 2×2 source block.
 * For 4K→540p  (4x): each output pixel averages a 4×4 source block.
 * Returns 1 if handled, 0 if ratio is non-integer (caller should fall back).
 * ---------------------------------------------------------------------- */
HR_EXPORT int hr_build_thumbnail_lq(
    const uint8_t * HR_RESTRICT src,
    uint8_t       * HR_RESTRICT dst,
    int sw, int sh, int dw, int dh)
{
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return 0;
    if ((sw % dw) != 0 || (sh % dh) != 0) return 0;

    int rx = sw / dw;   /* x block size */
    int ry = sh / dh;   /* y block size */
    int bsz = rx * ry;  /* pixels per block */

    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            uint32_t sum_r = 0, sum_g = 0, sum_b = 0;
            for (int by = 0; by < ry; ++by) {
                const uint8_t *row = src + ((size_t)(y*ry + by) * sw + x*rx) * 3;
                for (int bx = 0; bx < rx; ++bx) {
                    sum_r += row[bx*3 + 0];
                    sum_g += row[bx*3 + 1];
                    sum_b += row[bx*3 + 2];
                }
            }
            uint8_t *out = dst + ((size_t)y * dw + x) * 3;
            out[0] = (uint8_t)(sum_r / (uint32_t)bsz);
            out[1] = (uint8_t)(sum_g / (uint32_t)bsz);
            out[2] = (uint8_t)(sum_b / (uint32_t)bsz);
        }
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Non-temporal memcpy (avoids cache pollution for large frame copies)
 * Falls back to regular memcpy on non-x86 or small copies.
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_memcpy_nt(void *dst, const void *src, size_t n)
{
    /* For small copies regular memcpy is faster due to SSE2 setup cost */
    if (n < 65536) {
        memcpy(dst, src, n);
        return;
    }

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    /* Use 128-bit SSE2 non-temporal stores if available */
    #if defined(__SSE2__)
    {
        unsigned char       *d  = (unsigned char *)dst;
        const unsigned char *s  = (const unsigned char *)src;
        size_t head = (16 - ((uintptr_t)d & 15u)) & 15u;
        if (head > n) head = n;
        memcpy(d, s, head);
        d += head; s += head; n -= head;

        size_t chunks = n / 16;
        for (size_t i = 0; i < chunks; ++i) {
            __builtin_ia32_storedqu(
                (char *)(d + i*16),
                __builtin_ia32_lddqu((const char *)(s + i*16))
            );
        }
        _mm_sfence();
        size_t tail = n - chunks * 16;
        memcpy(d + chunks*16, s + chunks*16, tail);
    }
    #else
    memcpy(dst, src, n);
    #endif
#else
    memcpy(dst, src, n);
#endif
}
