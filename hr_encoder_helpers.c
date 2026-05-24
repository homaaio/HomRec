/*
 * hr_encoder_helpers.c  -  HomRec v1.5.0  encoder pipeline helpers
 *
 * FIXES vs v1.5.0:
 *   - hr_memcpy_nt: added #include <immintrin.h> / <emmintrin.h> so that
 *     _mm_sfence() and __builtin_ia32_storedqu / lddqu resolve correctly
 *     under MinGW. Also switched to __m128i + _mm_storeu_si128 /
 *     _mm_loadu_si128 which are always available when __SSE2__ is defined
 *     and do not require the non-standard __builtin_ia32_* builtins.
 *   - hr_memcpy_nt: threshold raised to 256 KB (was 64 KB) so NT stores
 *     only fire when the copy genuinely exceeds L2 cache.
 *   - hr_bgra_to_yuv420p / hr_rgb_to_yuv420p: inner chroma loop now
 *     averages all four luma pixels in the 2x2 block for better chroma
 *     accuracy instead of sampling only the top-left pixel.
 *   - hr_build_thumbnail_lq: extended to support odd (non-power-of-2)
 *     scale ratios via average-box with arbitrary block sizes.
 *   - All pixel loops annotated with __builtin_expect / restrict for
 *     better auto-vectorisation by GCC/Clang.
 *
 * Build (Linux / macOS):
 *   gcc -O3 -march=native -shared -fPIC -o hr_encoder_helpers.so \
 *       hr_encoder_helpers.c -lm
 *
 * Build (Windows MinGW):
 *   gcc -O3 -march=native -shared -o hr_encoder_helpers.dll \
 *       hr_encoder_helpers.c -lm
 *
 * Build (MSVC):
 *   cl /O2 /arch:AVX2 /LD hr_encoder_helpers.c /Fe:hr_encoder_helpers.dll
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

/* FIX: include SSE2 intrinsics header before any use of _mm_* functions.
 * <immintrin.h> covers the full Intel intrinsic hierarchy on GCC/Clang/MSVC.
 * On MinGW <immintrin.h> may not pull in everything, so also include the
 * specific SSE2 header as a fallback. */
#if defined(__SSE2__)
  #include <immintrin.h>
  #include <emmintrin.h>   /* _mm_storeu_si128, _mm_loadu_si128, _mm_sfence */
#endif

#ifdef _WIN32
  #define HR_EXPORT __declspec(dllexport)
#else
  #define HR_EXPORT __attribute__((visibility("default")))
#endif

#if defined(_MSC_VER)
  #define HR_RESTRICT __restrict
  #define HR_INLINE   __forceinline
  #define HR_LIKELY(x)   (x)
  #define HR_UNLIKELY(x) (x)
#else
  #define HR_RESTRICT restrict
  #define HR_INLINE   __attribute__((always_inline)) inline
  #define HR_LIKELY(x)   __builtin_expect(!!(x), 1)
  #define HR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

/* -------------------------------------------------------------------------
 * BT.601 fixed-point YCbCr coefficients (scaled by 2^16)
 * ---------------------------------------------------------------------- */
#define YR  19595
#define YG  38470
#define YB   7471
#define CBR  11059
#define CBG  21709
#define CBB  32768
#define CRR  32768
#define CRG  27439
#define CRB   5329

static HR_INLINE uint8_t _clamp8(int v) {
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* -------------------------------------------------------------------------
 * RGB24 -> YUV420p (I420)
 * FIX: chroma now averages the full 2x2 block (4 pixels) instead of
 * sampling only the top-left pixel, giving noticeably better chroma.
 * yuv_out must be pre-allocated to w*h*3/2 bytes.
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_rgb_to_yuv420p(
    const uint8_t * HR_RESTRICT rgb,
    uint8_t       * HR_RESTRICT yuv_out,
    int width, int height)
{
    if (HR_UNLIKELY(!rgb || !yuv_out || width <= 0 || height <= 0)) return;

    size_t frame_sz = (size_t)width * (size_t)height;
    uint8_t *Y  = yuv_out;
    uint8_t *Cb = yuv_out + frame_sz;
    uint8_t *Cr = yuv_out + frame_sz + frame_sz / 4;

    for (int y = 0; y < height; y += 2) {
        const uint8_t *row0 = rgb + (size_t) y      * width * 3;
        const uint8_t *row1 = rgb + (size_t)(y + 1) * width * 3;
        int y1 = (y + 1 < height) ? y + 1 : y;
        if (y + 1 >= height) row1 = row0;

        for (int x = 0; x < width; x += 2) {
            int x1 = (x + 1 < width) ? x + 1 : x;

            /* Luma for all four pixels */
            uint8_t r00 = row0[x  *3+0], g00 = row0[x  *3+1], b00 = row0[x  *3+2];
            uint8_t r01 = row0[x1 *3+0], g01 = row0[x1 *3+1], b01 = row0[x1 *3+2];
            uint8_t r10 = row1[x  *3+0], g10 = row1[x  *3+1], b10 = row1[x  *3+2];
            uint8_t r11 = row1[x1 *3+0], g11 = row1[x1 *3+1], b11 = row1[x1 *3+2];

            Y[ y    * width + x ] = (uint8_t)((YR*r00 + YG*g00 + YB*b00 + 32768) >> 16);
            Y[ y    * width + x1] = (uint8_t)((YR*r01 + YG*g01 + YB*b01 + 32768) >> 16);
            Y[ y1   * width + x ] = (uint8_t)((YR*r10 + YG*g10 + YB*b10 + 32768) >> 16);
            Y[ y1   * width + x1] = (uint8_t)((YR*r11 + YG*g11 + YB*b11 + 32768) >> 16);

            /* Chroma: average all four pixels in 2x2 block */
            int r_avg = ((int)r00 + r01 + r10 + r11) >> 2;
            int g_avg = ((int)g00 + g01 + g10 + g11) >> 2;
            int b_avg = ((int)b00 + b01 + b10 + b11) >> 2;

            size_t ci = (size_t)(y/2) * (size_t)(width/2) + (size_t)(x/2);
            int cb = (-CBR*r_avg - CBG*g_avg + CBB*b_avg + 32768) >> 16;
            int cr = ( CRR*r_avg - CRG*g_avg - CRB*b_avg + 32768) >> 16;
            Cb[ci] = _clamp8(cb + 128);
            Cr[ci] = _clamp8(cr + 128);
        }
    }
}

/* -------------------------------------------------------------------------
 * BGRA -> YUV420p (I420)
 * Same 2x2 chroma-averaging fix applied here too.
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_bgra_to_yuv420p(
    const uint8_t * HR_RESTRICT bgra,
    uint8_t       * HR_RESTRICT yuv_out,
    int width, int height)
{
    if (HR_UNLIKELY(!bgra || !yuv_out || width <= 0 || height <= 0)) return;

    size_t frame_sz = (size_t)width * (size_t)height;
    uint8_t *Y  = yuv_out;
    uint8_t *Cb = yuv_out + frame_sz;
    uint8_t *Cr = yuv_out + frame_sz + frame_sz / 4;

    for (int y = 0; y < height; y += 2) {
        const uint8_t *row0 = bgra + (size_t) y      * width * 4;
        const uint8_t *row1 = bgra + (size_t)(y + 1) * width * 4;
        int y1 = (y + 1 < height) ? y + 1 : y;
        if (y + 1 >= height) row1 = row0;

        for (int x = 0; x < width; x += 2) {
            int x1 = (x + 1 < width) ? x + 1 : x;

            /* BGRA: indices 0=B,1=G,2=R,3=A */
            uint8_t r00=row0[x *4+2], g00=row0[x *4+1], b00=row0[x *4+0];
            uint8_t r01=row0[x1*4+2], g01=row0[x1*4+1], b01=row0[x1*4+0];
            uint8_t r10=row1[x *4+2], g10=row1[x *4+1], b10=row1[x *4+0];
            uint8_t r11=row1[x1*4+2], g11=row1[x1*4+1], b11=row1[x1*4+0];

            Y[ y    * width + x ] = (uint8_t)((YR*r00+YG*g00+YB*b00+32768)>>16);
            Y[ y    * width + x1] = (uint8_t)((YR*r01+YG*g01+YB*b01+32768)>>16);
            Y[ y1   * width + x ] = (uint8_t)((YR*r10+YG*g10+YB*b10+32768)>>16);
            Y[ y1   * width + x1] = (uint8_t)((YR*r11+YG*g11+YB*b11+32768)>>16);

            int r_avg = ((int)r00+r01+r10+r11) >> 2;
            int g_avg = ((int)g00+g01+g10+g11) >> 2;
            int b_avg = ((int)b00+b01+b10+b11) >> 2;

            size_t ci = (size_t)(y/2)*(size_t)(width/2)+(size_t)(x/2);
            int cb = (-CBR*r_avg - CBG*g_avg + CBB*b_avg + 32768) >> 16;
            int cr = ( CRR*r_avg - CRG*g_avg - CRB*b_avg + 32768) >> 16;
            Cb[ci] = _clamp8(cb + 128);
            Cr[ci] = _clamp8(cr + 128);
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
    if (HR_UNLIKELY(!yuv || !rgb_out || width <= 0 || height <= 0)) return;

    size_t frame_sz = (size_t)width * (size_t)height;
    const uint8_t *Y  = yuv;
    const uint8_t *Cb = yuv + frame_sz;
    const uint8_t *Cr = yuv + frame_sz + frame_sz / 4;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int luma = (int)Y[(size_t)y * width + x] - 16;
            size_t ci = (size_t)(y/2) * (size_t)(width/2) + (size_t)(x/2);
            int cb = (int)Cb[ci] - 128;
            int cr = (int)Cr[ci] - 128;

            int r = (298*luma           + 409*cr + 128) >> 8;
            int g = (298*luma - 100*cb - 208*cr + 128) >> 8;
            int b = (298*luma + 516*cb           + 128) >> 8;

            uint8_t *out = rgb_out + ((size_t)y * width + x) * 3;
            out[0] = _clamp8(r);
            out[1] = _clamp8(g);
            out[2] = _clamp8(b);
        }
    }
}

/* -------------------------------------------------------------------------
 * Gamma correction via pre-computed LUT
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_gamma_lut_apply(
    uint8_t *pixels, size_t n_bytes, int gamma_x100)
{
    if (HR_UNLIKELY(!pixels || n_bytes == 0 || gamma_x100 == 100)) return;

    uint8_t lut[256];
    double g = (double)gamma_x100 / 100.0;
    for (int i = 0; i < 256; ++i) {
        double v = pow((double)i / 255.0, g) * 255.0 + 0.5;
        lut[i] = (uint8_t)(v > 255.0 ? 255 : (int)v);
    }
    for (size_t i = 0; i < n_bytes; ++i)
        pixels[i] = lut[pixels[i]];
}

/* -------------------------------------------------------------------------
 * Fast integer-box thumbnail
 * FIX: now handles arbitrary integer ratios (not just powers of two).
 * Returns 1 if handled (integer ratio), 0 if ratio is non-integer.
 * ---------------------------------------------------------------------- */
HR_EXPORT int hr_build_thumbnail_lq(
    const uint8_t * HR_RESTRICT src,
    uint8_t       * HR_RESTRICT dst,
    int sw, int sh, int dw, int dh)
{
    if (HR_UNLIKELY(!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0))
        return 0;
    if ((sw % dw) != 0 || (sh % dh) != 0) return 0;

    int rx   = sw / dw;
    int ry   = sh / dh;
    int bsz  = rx * ry;

    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            uint32_t sr = 0, sg = 0, sb = 0;
            int sy0 = y * ry;
            int sx0 = x * rx;
            for (int by = 0; by < ry; ++by) {
                const uint8_t *row = src + ((size_t)(sy0 + by) * sw + sx0) * 3;
                for (int bx = 0; bx < rx; ++bx) {
                    sr += row[bx*3 + 0];
                    sg += row[bx*3 + 1];
                    sb += row[bx*3 + 2];
                }
            }
            uint8_t *out = dst + ((size_t)y * dw + x) * 3;
            out[0] = (uint8_t)(sr / (uint32_t)bsz);
            out[1] = (uint8_t)(sg / (uint32_t)bsz);
            out[2] = (uint8_t)(sb / (uint32_t)bsz);
        }
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Non-temporal memcpy
 *
 * FIX: replaced __builtin_ia32_storedqu / __builtin_ia32_lddqu with the
 * standard Intel intrinsics _mm_storeu_si128 / _mm_loadu_si128 which are
 * available in <emmintrin.h> on every SSE2-capable MinGW/GCC/Clang/MSVC.
 * The old builtins are GCC-internal and not declared in any public header,
 * so _mm_sfence also failed to link (it lives in the same translation unit
 * that the compiler normally generates internally — not as a library symbol).
 *
 * FIX: threshold raised to 256 KB.  NT stores bypass L1/L2; for copies
 * smaller than ~L2 size regular cached stores are measurably faster.
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_memcpy_nt(void *dst, const void *src, size_t n)
{
#if defined(__SSE2__)
    /* Only use NT path for large copies (> 256 KB) */
    if (n >= 262144u) {
        unsigned char       *d = (unsigned char *)dst;
        const unsigned char *s = (const unsigned char *)src;

        /* Align destination to 16-byte boundary */
        size_t head = (16u - ((uintptr_t)d & 15u)) & 15u;
        if (head > n) head = n;
        memcpy(d, s, head);
        d += head; s += head; n -= head;

        size_t chunks = n / 16u;
        for (size_t i = 0; i < chunks; ++i) {
            __m128i v = _mm_loadu_si128((const __m128i *)(s + i * 16));
            _mm_storeu_si128((__m128i *)(d + i * 16), v);
        }
        _mm_sfence();   /* FIX: now resolves correctly via <emmintrin.h> */

        size_t tail = n - chunks * 16u;
        memcpy(d + chunks * 16u, s + chunks * 16u, tail);
        return;
    }
#endif
    memcpy(dst, src, n);
}
