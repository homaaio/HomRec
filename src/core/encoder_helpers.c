#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#if defined(__SSE2__)
  #include <immintrin.h>
  #include <emmintrin.h>
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #include <smmintrin.h>
  #define HR_HAVE_X86_SIMD 1
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

/* BT.601 fixed-point coefficients */
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

// GB24 -> YUV420p (I420)

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
        const uint8_t *row0 = rgb + (size_t)y * width * 3;
        const uint8_t *row1 = (y + 1 < height)
                            ? rgb + (size_t)(y + 1) * width * 3
                            : row0;
        int y1 = (y + 1 < height) ? y + 1 : y;

        // hoist the per-row output bases out of the pixel loop below.
        uint8_t *Yrow0 = Y + (size_t)y * width;
        uint8_t *Yrow1 = Y + (size_t)y1 * width;
        size_t crow = (size_t)(y / 2) * (size_t)(width / 2);
        uint8_t *Cbrow = Cb + crow;
        uint8_t *Crrow = Cr + crow;

        int x = 0;
        /* Unroll x2: process two chroma blocks per iteration */
        for (; x + 3 < width; x += 4) {
            /* Block 0: x, x+1 */
            uint8_t r00=row0[x*3+0],   g00=row0[x*3+1],   b00=row0[x*3+2];
            uint8_t r01=row0[(x+1)*3+0],g01=row0[(x+1)*3+1],b01=row0[(x+1)*3+2];
            uint8_t r10=row1[x*3+0],   g10=row1[x*3+1],   b10=row1[x*3+2];
            uint8_t r11=row1[(x+1)*3+0],g11=row1[(x+1)*3+1],b11=row1[(x+1)*3+2];

            Yrow0[x ]=(uint8_t)((YR*r00+YG*g00+YB*b00+32768)>>16);
            Yrow0[x+1]=(uint8_t)((YR*r01+YG*g01+YB*b01+32768)>>16);
            Yrow1[x ]=(uint8_t)((YR*r10+YG*g10+YB*b10+32768)>>16);
            Yrow1[x+1]=(uint8_t)((YR*r11+YG*g11+YB*b11+32768)>>16);
            {
                int ra=((int)r00+r01+r10+r11)>>2, ga=((int)g00+g01+g10+g11)>>2, ba=((int)b00+b01+b10+b11)>>2;
                size_t ci=(size_t)(x/2);
                Cbrow[ci]=_clamp8(((-CBR*ra-CBG*ga+CBB*ba+32768)>>16)+128);
                Crrow[ci]=_clamp8((( CRR*ra-CRG*ga-CRB*ba+32768)>>16)+128);
            }

            /* Block 1: x+2, x+3 */
            uint8_t r02=row0[(x+2)*3+0],g02=row0[(x+2)*3+1],b02=row0[(x+2)*3+2];
            uint8_t r03=row0[(x+3)*3+0],g03=row0[(x+3)*3+1],b03=row0[(x+3)*3+2];
            uint8_t r12=row1[(x+2)*3+0],g12=row1[(x+2)*3+1],b12=row1[(x+2)*3+2];
            uint8_t r13=row1[(x+3)*3+0],g13=row1[(x+3)*3+1],b13=row1[(x+3)*3+2];

            Yrow0[x+2]=(uint8_t)((YR*r02+YG*g02+YB*b02+32768)>>16);
            Yrow0[x+3]=(uint8_t)((YR*r03+YG*g03+YB*b03+32768)>>16);
            Yrow1[x+2]=(uint8_t)((YR*r12+YG*g12+YB*b12+32768)>>16);
            Yrow1[x+3]=(uint8_t)((YR*r13+YG*g13+YB*b13+32768)>>16);
            {
                int ra=((int)r02+r03+r12+r13)>>2, ga=((int)g02+g03+g12+g13)>>2, ba=((int)b02+b03+b12+b13)>>2;
                size_t ci=(size_t)((x+2)/2);
                Cbrow[ci]=_clamp8(((-CBR*ra-CBG*ga+CBB*ba+32768)>>16)+128);
                Crrow[ci]=_clamp8((( CRR*ra-CRG*ga-CRB*ba+32768)>>16)+128);
            }
        }
        /* Tail: remaining 0 or 2 pixels */
        for (; x < width; x += 2) {
            int x1 = (x+1 < width) ? x+1 : x;
            uint8_t r00=row0[x*3+0],g00=row0[x*3+1],b00=row0[x*3+2];
            uint8_t r01=row0[x1*3+0],g01=row0[x1*3+1],b01=row0[x1*3+2];
            uint8_t r10=row1[x*3+0],g10=row1[x*3+1],b10=row1[x*3+2];
            uint8_t r11=row1[x1*3+0],g11=row1[x1*3+1],b11=row1[x1*3+2];
            Yrow0[x ]=(uint8_t)((YR*r00+YG*g00+YB*b00+32768)>>16);
            Yrow0[x1]=(uint8_t)((YR*r01+YG*g01+YB*b01+32768)>>16);
            Yrow1[x ]=(uint8_t)((YR*r10+YG*g10+YB*b10+32768)>>16);
            Yrow1[x1]=(uint8_t)((YR*r11+YG*g11+YB*b11+32768)>>16);
            int ra=((int)r00+r01+r10+r11)>>2,ga=((int)g00+g01+g10+g11)>>2,ba=((int)b00+b01+b10+b11)>>2;
            size_t ci=(size_t)(x/2);
            Cbrow[ci]=_clamp8(((-CBR*ra-CBG*ga+CBB*ba+32768)>>16)+128);
            Crrow[ci]=_clamp8((( CRR*ra-CRG*ga-CRB*ba+32768)>>16)+128);
        }
    }
}

// BGRA -> YUV 4:2:0 (planar I420 or semi-planar NV12)
typedef struct {
    int yr, yg, yb;     /* luma coefficients, scaled by 65536            */
    int yoff;           /* luma offset added after the shift (0 or 16)   */
    int cbr, cbg, cbb;  /* Cb coefficients (2x2 sums), scaled by 65536   */
    int crr, crg, crb;  /* Cr coefficients                               */
} HrCsc;

__attribute__((unused)) static const HrCsc k_csc_full = {   /* full range; no converter uses it since I420 went to studio range */
    19595, 38470, 7471, 0,
    -11059, -21709, 32768,
     32768, -27439, -5329
};
static const HrCsc k_csc_tv = {
    16829, 33039, 6416, 16,
    -9714, -19071, 28785,
     28785, -24103, -4682
};

/* One 2x2 block (2 columns of row0/row1) at columns x, x1 -> Y + chroma. */
static HR_INLINE void hr_csc_block_scalar(
    const uint8_t *row0, const uint8_t *row1, int x, int x1,
    uint8_t *Yrow0, uint8_t *Yrow1, const HrCsc *k,
    uint8_t *cb_out, uint8_t *cr_out)
{
    const int yround = 32768 + (k->yoff << 16);
    int b00=row0[x*4+0],  g00=row0[x*4+1],  r00=row0[x*4+2];
    int b01=row0[x1*4+0], g01=row0[x1*4+1], r01=row0[x1*4+2];
    int b10=row1[x*4+0],  g10=row1[x*4+1],  r10=row1[x*4+2];
    int b11=row1[x1*4+0], g11=row1[x1*4+1], r11=row1[x1*4+2];

    Yrow0[x ] = (uint8_t)((k->yr*r00 + k->yg*g00 + k->yb*b00 + yround) >> 16);
    Yrow0[x1] = (uint8_t)((k->yr*r01 + k->yg*g01 + k->yb*b01 + yround) >> 16);
    Yrow1[x ] = (uint8_t)((k->yr*r10 + k->yg*g10 + k->yb*b10 + yround) >> 16);
    Yrow1[x1] = (uint8_t)((k->yr*r11 + k->yg*g11 + k->yb*b11 + yround) >> 16);

    int rs = r00 + r01 + r10 + r11;
    int gs = g00 + g01 + g10 + g11;
    int bs = b00 + b01 + b10 + b11;
    *cb_out = _clamp8(((k->cbr*rs + k->cbg*gs + k->cbb*bs + (1 << 17)) >> 18) + 128);
    *cr_out = _clamp8(((k->crr*rs + k->crg*gs + k->crb*bs + (1 << 17)) >> 18) + 128);
}

/* Scalar band converter (also the tail handler for the SIMD one). Converts
 * rows [y0,y1) x columns [xs,width). y0 must be even. */
static void hr_csc_band_scalar(
    const uint8_t * HR_RESTRICT bgra, size_t stride,
    uint8_t       * HR_RESTRICT out,
    int width, int height, int y0, int y1, int xs, int nv12, const HrCsc *k)
{
    size_t frame_sz = (size_t)width * (size_t)height;
    uint8_t *Y  = out;
    uint8_t *C0 = out + frame_sz;                               /* Cb plane, or the UV plane */
    uint8_t *C1 = out + frame_sz + frame_sz / 4;                /* Cr plane (I420 only)      */

    for (int y = y0; y < y1; y += 2) {
        const uint8_t *row0 = bgra + (size_t)y * stride;
        const uint8_t *row1 = (y + 1 < height) ? bgra + (size_t)(y + 1) * stride : row0;
        int y1r = (y + 1 < height) ? y + 1 : y;
        uint8_t *Yrow0 = Y + (size_t)y  * width;
        uint8_t *Yrow1 = Y + (size_t)y1r * width;
        size_t cbase = (size_t)(y / 2) * (size_t)(nv12 ? width : width / 2);

        for (int x = xs; x < width; x += 2) {
            int x1 = (x + 1 < width) ? x + 1 : x;
            uint8_t cb, cr;
            hr_csc_block_scalar(row0, row1, x, x1, Yrow0, Yrow1, k, &cb, &cr);
            if (nv12) {
                C0[cbase + (size_t)x]     = cb;      /* (x/2)*2 == x for even x */
                C0[cbase + (size_t)x + 1] = cr;
            } else {
                C0[cbase + (size_t)(x / 2)] = cb;
                C1[cbase + (size_t)(x / 2)] = cr;
            }
        }
    }
}

#if defined(HR_HAVE_X86_SIMD)
/* SSE4.1 band converter: 8 pixels x 2 rows per iteration; luma AND chroma are
 * vectorised (chroma used to be a scalar loop even in the "SIMD" path). */
__attribute__((target("sse4.1")))
static void hr_csc_band_sse41(
    const uint8_t * HR_RESTRICT bgra, size_t stride,
    uint8_t       * HR_RESTRICT out,
    int width, int height, int y0, int y1, int xs, int nv12, const HrCsc *k)
{
    size_t frame_sz = (size_t)width * (size_t)height;
    uint8_t *Y  = out;
    uint8_t *C0 = out + frame_sz;
    uint8_t *C1 = out + frame_sz + frame_sz / 4;

    const __m128i mask  = _mm_set1_epi32(0xFF);
    const __m128i c_yr  = _mm_set1_epi32(k->yr),  c_yg  = _mm_set1_epi32(k->yg),  c_yb  = _mm_set1_epi32(k->yb);
    const __m128i c_yrd = _mm_set1_epi32(32768 + (k->yoff << 16));
    const __m128i c_cbr = _mm_set1_epi32(k->cbr), c_cbg = _mm_set1_epi32(k->cbg), c_cbb = _mm_set1_epi32(k->cbb);
    const __m128i c_crr = _mm_set1_epi32(k->crr), c_crg = _mm_set1_epi32(k->crg), c_crb = _mm_set1_epi32(k->crb);
    const __m128i c_crd = _mm_set1_epi32(1 << 17);
    const __m128i c_128 = _mm_set1_epi32(128);
    const __m128i c_0   = _mm_setzero_si128();
    const __m128i c_255 = _mm_set1_epi32(255);

    const int vec_w = width & ~7;

    for (int y = y0; y < y1; y += 2) {
        const uint8_t *row0 = bgra + (size_t)y * stride;
        const uint8_t *row1 = (y + 1 < height) ? bgra + (size_t)(y + 1) * stride : row0;
        int y1r = (y + 1 < height) ? y + 1 : y;
        uint8_t *Yrow0 = Y + (size_t)y   * width;
        uint8_t *Yrow1 = Y + (size_t)y1r * width;
        size_t cbase = (size_t)(y / 2) * (size_t)(nv12 ? width : width / 2);

        for (int x = xs; x < vec_w; x += 8) {
            __m128i a0 = _mm_loadu_si128((const __m128i *)(row0 + (size_t)x * 4));
            __m128i a1 = _mm_loadu_si128((const __m128i *)(row0 + (size_t)x * 4 + 16));
            __m128i b0 = _mm_loadu_si128((const __m128i *)(row1 + (size_t)x * 4));
            __m128i b1 = _mm_loadu_si128((const __m128i *)(row1 + (size_t)x * 4 + 16));

            __m128i a0b = _mm_and_si128(a0, mask), a0g = _mm_and_si128(_mm_srli_epi32(a0, 8), mask), a0r = _mm_and_si128(_mm_srli_epi32(a0, 16), mask);
            __m128i a1b = _mm_and_si128(a1, mask), a1g = _mm_and_si128(_mm_srli_epi32(a1, 8), mask), a1r = _mm_and_si128(_mm_srli_epi32(a1, 16), mask);
            __m128i b0b = _mm_and_si128(b0, mask), b0g = _mm_and_si128(_mm_srli_epi32(b0, 8), mask), b0r = _mm_and_si128(_mm_srli_epi32(b0, 16), mask);
            __m128i b1b = _mm_and_si128(b1, mask), b1g = _mm_and_si128(_mm_srli_epi32(b1, 8), mask), b1r = _mm_and_si128(_mm_srli_epi32(b1, 16), mask);

            /* ---- luma: 8 px of row0 and 8 px of row1 ---- */
            __m128i ya0 = _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(_mm_mullo_epi32(a0r, c_yr), _mm_mullo_epi32(a0g, c_yg)),
                                                       _mm_add_epi32(_mm_mullo_epi32(a0b, c_yb), c_yrd)), 16);
            __m128i ya1 = _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(_mm_mullo_epi32(a1r, c_yr), _mm_mullo_epi32(a1g, c_yg)),
                                                       _mm_add_epi32(_mm_mullo_epi32(a1b, c_yb), c_yrd)), 16);
            __m128i yb0 = _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(_mm_mullo_epi32(b0r, c_yr), _mm_mullo_epi32(b0g, c_yg)),
                                                       _mm_add_epi32(_mm_mullo_epi32(b0b, c_yb), c_yrd)), 16);
            __m128i yb1 = _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(_mm_mullo_epi32(b1r, c_yr), _mm_mullo_epi32(b1g, c_yg)),
                                                       _mm_add_epi32(_mm_mullo_epi32(b1b, c_yb), c_yrd)), 16);
            __m128i ypa = _mm_packus_epi32(ya0, ya1);
            __m128i ypb = _mm_packus_epi32(yb0, yb1);
            _mm_storel_epi64((__m128i *)(Yrow0 + x), _mm_packus_epi16(ypa, ypa));
            _mm_storel_epi64((__m128i *)(Yrow1 + x), _mm_packus_epi16(ypb, ypb));

            /* ---- chroma: vertical sum, then horizontal pair sum -> 4 samples ---- */
            __m128i rs = _mm_hadd_epi32(_mm_add_epi32(a0r, b0r), _mm_add_epi32(a1r, b1r));
            __m128i gs = _mm_hadd_epi32(_mm_add_epi32(a0g, b0g), _mm_add_epi32(a1g, b1g));
            __m128i bs = _mm_hadd_epi32(_mm_add_epi32(a0b, b0b), _mm_add_epi32(a1b, b1b));

            __m128i cb = _mm_add_epi32(_mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(_mm_mullo_epi32(rs, c_cbr), _mm_mullo_epi32(gs, c_cbg)),
                                                                     _mm_add_epi32(_mm_mullo_epi32(bs, c_cbb), c_crd)), 18), c_128);
            __m128i cr = _mm_add_epi32(_mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(_mm_mullo_epi32(rs, c_crr), _mm_mullo_epi32(gs, c_crg)),
                                                                     _mm_add_epi32(_mm_mullo_epi32(bs, c_crb), c_crd)), 18), c_128);
            cb = _mm_min_epi32(_mm_max_epi32(cb, c_0), c_255);
            cr = _mm_min_epi32(_mm_max_epi32(cr, c_0), c_255);

            if (nv12) {
                /* U0 V0 U1 V1 U2 V2 U3 V3 */
                __m128i uv  = _mm_or_si128(cb, _mm_slli_epi32(cr, 8));
                __m128i uv16 = _mm_packus_epi32(uv, uv);
                _mm_storel_epi64((__m128i *)(C0 + cbase + (size_t)x), uv16);
            } else {
                __m128i cb16 = _mm_packus_epi32(cb, cb);
                __m128i cr16 = _mm_packus_epi32(cr, cr);
                int cb4 = _mm_cvtsi128_si32(_mm_packus_epi16(cb16, cb16));
                int cr4 = _mm_cvtsi128_si32(_mm_packus_epi16(cr16, cr16));
                memcpy(C0 + cbase + (size_t)(x / 2), &cb4, 4);
                memcpy(C1 + cbase + (size_t)(x / 2), &cr4, 4);
            }
        }
    }

    /* Columns beyond the last full group of 8 (only when width % 8 != 0). */
    if (vec_w < width)
        hr_csc_band_scalar(bgra, stride, out, width, height, y0, y1, vec_w > xs ? vec_w : xs, nv12, k);
}

/* AVX2 band converter: 16 pixels x 2 rows per iteration, same arithmetic as the SSE4.1 one (so the
 * output is bit-identical). 256-bit pack / hadd work per 128-bit lane, hence the 0xD8 qword permutes
 * that put the results back in pixel order. Columns after the last full group of 16 go to the
 * SSE4.1 / scalar code. */
__attribute__((target("avx2")))
static void hr_csc_band_avx2(
    const uint8_t * HR_RESTRICT bgra, size_t stride,
    uint8_t       * HR_RESTRICT out,
    int width, int height, int y0, int y1, int nv12, const HrCsc *k)
{
    size_t frame_sz = (size_t)width * (size_t)height;
    uint8_t *Y  = out;
    uint8_t *C0 = out + frame_sz;
    uint8_t *C1 = out + frame_sz + frame_sz / 4;

    const __m256i mask  = _mm256_set1_epi32(0xFF);
    const __m256i c_yr  = _mm256_set1_epi32(k->yr),  c_yg  = _mm256_set1_epi32(k->yg),  c_yb  = _mm256_set1_epi32(k->yb);
    const __m256i c_yrd = _mm256_set1_epi32(32768 + (k->yoff << 16));
    const __m256i c_cbr = _mm256_set1_epi32(k->cbr), c_cbg = _mm256_set1_epi32(k->cbg), c_cbb = _mm256_set1_epi32(k->cbb);
    const __m256i c_crr = _mm256_set1_epi32(k->crr), c_crg = _mm256_set1_epi32(k->crg), c_crb = _mm256_set1_epi32(k->crb);
    const __m256i c_crd = _mm256_set1_epi32(1 << 17);
    const __m256i c_128 = _mm256_set1_epi32(128);
    const __m256i c_0   = _mm256_setzero_si256();
    const __m256i c_255 = _mm256_set1_epi32(255);

#define HR_AVX2_LUMA(r, g, b) \
    _mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32((r), c_yr), _mm256_mullo_epi32((g), c_yg)), \
                                       _mm256_add_epi32(_mm256_mullo_epi32((b), c_yb), c_yrd)), 16)
#define HR_AVX2_CHROMA(s_r, s_g, s_b, kr, kg, kb) \
    _mm256_min_epi32(_mm256_max_epi32(_mm256_add_epi32(_mm256_srai_epi32(_mm256_add_epi32( \
        _mm256_add_epi32(_mm256_mullo_epi32((s_r), (kr)), _mm256_mullo_epi32((s_g), (kg))), \
        _mm256_add_epi32(_mm256_mullo_epi32((s_b), (kb)), c_crd)), 18), c_128), c_0), c_255)

    const int vec_w = width & ~15;

    for (int y = y0; y < y1; y += 2) {
        const uint8_t *row0 = bgra + (size_t)y * stride;
        const uint8_t *row1 = (y + 1 < height) ? bgra + (size_t)(y + 1) * stride : row0;
        int y1r = (y + 1 < height) ? y + 1 : y;
        uint8_t *Yrow0 = Y + (size_t)y   * width;
        uint8_t *Yrow1 = Y + (size_t)y1r * width;
        size_t cbase = (size_t)(y / 2) * (size_t)(nv12 ? width : width / 2);

        for (int x = 0; x < vec_w; x += 16) {
            __m256i a0 = _mm256_loadu_si256((const __m256i *)(row0 + (size_t)x * 4));
            __m256i a1 = _mm256_loadu_si256((const __m256i *)(row0 + (size_t)x * 4 + 32));
            __m256i b0 = _mm256_loadu_si256((const __m256i *)(row1 + (size_t)x * 4));
            __m256i b1 = _mm256_loadu_si256((const __m256i *)(row1 + (size_t)x * 4 + 32));

            __m256i a0b = _mm256_and_si256(a0, mask), a0g = _mm256_and_si256(_mm256_srli_epi32(a0, 8), mask), a0r = _mm256_and_si256(_mm256_srli_epi32(a0, 16), mask);
            __m256i a1b = _mm256_and_si256(a1, mask), a1g = _mm256_and_si256(_mm256_srli_epi32(a1, 8), mask), a1r = _mm256_and_si256(_mm256_srli_epi32(a1, 16), mask);
            __m256i b0b = _mm256_and_si256(b0, mask), b0g = _mm256_and_si256(_mm256_srli_epi32(b0, 8), mask), b0r = _mm256_and_si256(_mm256_srli_epi32(b0, 16), mask);
            __m256i b1b = _mm256_and_si256(b1, mask), b1g = _mm256_and_si256(_mm256_srli_epi32(b1, 8), mask), b1r = _mm256_and_si256(_mm256_srli_epi32(b1, 16), mask);

            /* ---- luma: 16 px of row0 and 16 px of row1 ---- */
            __m256i ya = _mm256_permute4x64_epi64(_mm256_packus_epi32(HR_AVX2_LUMA(a0r, a0g, a0b), HR_AVX2_LUMA(a1r, a1g, a1b)), 0xD8);
            __m256i yb = _mm256_permute4x64_epi64(_mm256_packus_epi32(HR_AVX2_LUMA(b0r, b0g, b0b), HR_AVX2_LUMA(b1r, b1g, b1b)), 0xD8);
            _mm_storeu_si128((__m128i *)(Yrow0 + x), _mm_packus_epi16(_mm256_castsi256_si128(ya), _mm256_extracti128_si256(ya, 1)));
            _mm_storeu_si128((__m128i *)(Yrow1 + x), _mm_packus_epi16(_mm256_castsi256_si128(yb), _mm256_extracti128_si256(yb, 1)));

            /* ---- chroma: vertical sum, horizontal pair sum -> 8 samples ---- */
            __m256i rs = _mm256_permute4x64_epi64(_mm256_hadd_epi32(_mm256_add_epi32(a0r, b0r), _mm256_add_epi32(a1r, b1r)), 0xD8);
            __m256i gs = _mm256_permute4x64_epi64(_mm256_hadd_epi32(_mm256_add_epi32(a0g, b0g), _mm256_add_epi32(a1g, b1g)), 0xD8);
            __m256i bs = _mm256_permute4x64_epi64(_mm256_hadd_epi32(_mm256_add_epi32(a0b, b0b), _mm256_add_epi32(a1b, b1b)), 0xD8);

            __m256i cb = HR_AVX2_CHROMA(rs, gs, bs, c_cbr, c_cbg, c_cbb);
            __m256i cr = HR_AVX2_CHROMA(rs, gs, bs, c_crr, c_crg, c_crb);

            if (nv12) {
                /* U0 V0 ... U7 V7 */
                __m256i uv = _mm256_or_si256(cb, _mm256_slli_epi32(cr, 8));
                __m256i p  = _mm256_permute4x64_epi64(_mm256_packus_epi32(uv, uv), 0x08);
                _mm_storeu_si128((__m128i *)(C0 + cbase + (size_t)x), _mm256_castsi256_si128(p));
            } else {
                __m128i cb16 = _mm256_castsi256_si128(_mm256_permute4x64_epi64(_mm256_packus_epi32(cb, cb), 0x08));
                __m128i cr16 = _mm256_castsi256_si128(_mm256_permute4x64_epi64(_mm256_packus_epi32(cr, cr), 0x08));
                _mm_storel_epi64((__m128i *)(C0 + cbase + (size_t)(x / 2)), _mm_packus_epi16(cb16, cb16));
                _mm_storel_epi64((__m128i *)(C1 + cbase + (size_t)(x / 2)), _mm_packus_epi16(cr16, cr16));
            }
        }
    }
#undef HR_AVX2_LUMA
#undef HR_AVX2_CHROMA

    if (vec_w < width) {
        /* up to 15 leftover columns: SSE4.1 for the next group(s) of 8, scalar for the rest.
         * (_mm256_zeroupper() is emitted by the compiler on return from an AVX2 function.) */
        hr_csc_band_sse41(bgra, stride, out, width, height, y0, y1, vec_w, nv12, k);
    }
}
#endif /* HR_HAVE_X86_SIMD */

#if defined(HR_HAVE_X86_SIMD)
/* 0 = scalar, 1 = SSE4.1, 2 = AVX2 (CPU and OS support, detected once). */
static int g_csc_simd_cap = -1;   /* -1 = no cap; set by hr_csc_set_max_simd() (tests, troubleshooting) */
static int hr_csc_simd_level(void)
{
    static int detected = -1;
    if (HR_UNLIKELY(detected < 0)) {
        __builtin_cpu_init();
        detected = __builtin_cpu_supports("avx2") ? 2 : (__builtin_cpu_supports("sse4.1") ? 1 : 0);
    }
    return (g_csc_simd_cap >= 0 && g_csc_simd_cap < detected) ? g_csc_simd_cap : detected;
}
#endif

/* Limits the BGRA->YUV converters to scalar (0), SSE4.1 (1) or AVX2 (2); -1 removes the limit. Used by
 * the unit test to compare every implementation against the scalar one. Returns the level now in use. */
HR_EXPORT int hr_csc_set_max_simd(int cap)
{
#if defined(HR_HAVE_X86_SIMD)
    g_csc_simd_cap = cap;
    return hr_csc_simd_level();
#else
    (void)cap;
    return 0;
#endif
}

static void hr_csc_band(
    const uint8_t *bgra, size_t stride, uint8_t *out,
    int width, int height, int y0, int y1, int nv12, const HrCsc *k)
{
    if (HR_UNLIKELY(!bgra || !out || width <= 0 || height <= 0)) return;
    if (stride == 0) stride = (size_t)width * 4;
    if (y0 < 0) y0 = 0;
    if (y1 > height) y1 = height;
    y0 &= ~1;                       /* chroma rows come in pairs */
    if (y0 >= y1) return;
#if defined(HR_HAVE_X86_SIMD)
    const int level = hr_csc_simd_level();
    if (level >= 2) {
        hr_csc_band_avx2(bgra, stride, out, width, height, y0, y1, nv12, k);
        return;
    }
    if (level == 1) {
        hr_csc_band_sse41(bgra, stride, out, width, height, y0, y1, 0, nv12, k);
        return;
    }
#endif
    hr_csc_band_scalar(bgra, stride, out, width, height, y0, y1, 0, nv12, k);
}

/* The *_s variants read the BGRA picture with a row pitch of `stride` bytes (0 = width*4), so a
 * rectangle INSIDE a bigger frame can be converted in place: pass the address of its top-left pixel and
 * the pitch of the big frame. This is what lets the capture loop crop without first copying the
 * rectangle into a packed buffer. The output is always tightly packed (width x height). */

HR_EXPORT void hr_bgra_to_yuv420p_band_s(
    const uint8_t * HR_RESTRICT bgra, size_t stride,
    uint8_t       * HR_RESTRICT yuv_out,
    int width, int height, int y0, int y1)
{
    hr_csc_band(bgra, stride, yuv_out, width, height, y0, y1, 0, &k_csc_tv);
}

HR_EXPORT void hr_bgra_to_yuv420p_band(
    const uint8_t * HR_RESTRICT bgra,
    uint8_t       * HR_RESTRICT yuv_out,
    int width, int height, int y0, int y1)
{
    hr_csc_band(bgra, 0, yuv_out, width, height, y0, y1, 0, &k_csc_tv);
}

HR_EXPORT void hr_bgra_to_yuv420p(
    const uint8_t * HR_RESTRICT bgra,
    uint8_t       * HR_RESTRICT yuv_out,
    int width, int height)
{
    hr_bgra_to_yuv420p_band(bgra, yuv_out, width, height, 0, height);
}

/* Semi-planar NV12, studio/TV range - hardware encoders (nvenc/qsv/amf), which
 * consume NV12 natively (feeding them yuv420p would make ffmpeg insert its own
 * swscale pass).  See ffmpeg_runner.cpp's matching "-pix_fmt nv12 -color_range tv". */
HR_EXPORT void hr_bgra_to_nv12_band_s(
    const uint8_t * HR_RESTRICT bgra, size_t stride,
    uint8_t       * HR_RESTRICT nv12_out,
    int width, int height, int y0, int y1)
{
    hr_csc_band(bgra, stride, nv12_out, width, height, y0, y1, 1, &k_csc_tv);
}

HR_EXPORT void hr_bgra_to_nv12_band(
    const uint8_t * HR_RESTRICT bgra,
    uint8_t       * HR_RESTRICT nv12_out,
    int width, int height, int y0, int y1)
{
    hr_csc_band(bgra, 0, nv12_out, width, height, y0, y1, 1, &k_csc_tv);
}

HR_EXPORT void hr_bgra_to_nv12(
    const uint8_t * HR_RESTRICT bgra,
    uint8_t       * HR_RESTRICT nv12_out,
    int width, int height)
{
    hr_bgra_to_nv12_band(bgra, nv12_out, width, height, 0, height);
}

/* -------------------------------------------------------------------------
 * YUV420p -> RGB24
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
        // row bases hoisted out of the x loop (same idea as the encode-direction functions above)
        // - one multiply per row instead of one per pixel for both the Y/chroma reads and the RGB
        // write.
        const uint8_t *Yrow  = Y  + (size_t)y * width;
        const uint8_t *Cbrow = Cb + (size_t)(y/2) * (size_t)(width/2);
        const uint8_t *Crrow = Cr + (size_t)(y/2) * (size_t)(width/2);
        uint8_t *outrow = rgb_out + (size_t)y * width * 3;
        for (int x = 0; x < width; ++x) {
            int luma = (int)Yrow[x] - 16;
            int cb = (int)Cbrow[x/2] - 128;
            int cr = (int)Crrow[x/2] - 128;
            int r = (298*luma           + 409*cr + 128) >> 8;
            int g = (298*luma - 100*cb - 208*cr + 128) >> 8;
            int b = (298*luma + 516*cb           + 128) >> 8;
            uint8_t *out = outrow + (size_t)x * 3;
            out[0] = _clamp8(r); out[1] = _clamp8(g); out[2] = _clamp8(b);
        }
    }
}

HR_EXPORT void hr_gamma_lut_apply(uint8_t *pixels, size_t n_bytes, int gamma_x100)
{
    if (HR_UNLIKELY(!pixels || n_bytes == 0 || gamma_x100 == 100)) return;
    uint8_t lut[256];
    double g = (double)gamma_x100 / 100.0;
    for (int i = 0; i < 256; ++i) {
        double v = pow((double)i / 255.0, g) * 255.0 + 0.5;
        lut[i] = (uint8_t)(v > 255.0 ? 255 : (int)v);
    }
    for (size_t i = 0; i < n_bytes; ++i) pixels[i] = lut[pixels[i]];
}

HR_EXPORT int hr_build_thumbnail_lq(
    const uint8_t * HR_RESTRICT src,
    uint8_t       * HR_RESTRICT dst,
    int sw, int sh, int dw, int dh)
{
    if (HR_UNLIKELY(!src||!dst||sw<=0||sh<=0||dw<=0||dh<=0)) return 0;
    if ((sw % dw) != 0 || (sh % dh) != 0) return 0;

    int rx  = sw / dw;
    int ry  = sh / dh;
    int bsz = rx * ry;

    /* Detect power-of-2 block for shift optimisation */
    int shift = 0;
    if ((bsz & (bsz - 1)) == 0) {
        int tmp = bsz; while (tmp > 1) { tmp >>= 1; shift++; }
    }

    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            uint32_t r = 0, g = 0, b = 0;
            int sy0 = y * ry, sx0 = x * rx;
            for (int by = 0; by < ry; ++by) {
                const uint8_t *row = src + ((size_t)(sy0+by)*sw + sx0)*3;
                for (int bx = 0; bx < rx; ++bx) {
                    r += row[bx*3+0]; g += row[bx*3+1]; b += row[bx*3+2];
                }
            }
            uint8_t *o = dst + ((size_t)y*dw+x)*3;
            if (shift) {
                o[0] = (uint8_t)(r >> shift);
                o[1] = (uint8_t)(g >> shift);
                o[2] = (uint8_t)(b >> shift);
            } else {
                o[0] = (uint8_t)(r / (uint32_t)bsz);
                o[1] = (uint8_t)(g / (uint32_t)bsz);
                o[2] = (uint8_t)(b / (uint32_t)bsz);
            }
        }
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Overlay blend: straight-alpha BGRA layer over a BGRA picture (CPU compositor)
 * ---------------------------------------------------------------------- */

/* One pixel. Rules (kept exactly as the old C++ loop in overlay_render.cpp, the SIMD paths match them bit for bit):
 *   a = layer alpha, scaled by `opacity` (0..255) -> (a*opacity + 127) / 255
 *   a == 0   : picture pixel untouched
 *   a == 255 : B,G,R copied from the layer
 *   else     : (layer*a + picture*(255-a) + 128) >> 8
 * The picture's own alpha byte is never written. */
static HR_INLINE void hr_blend_px(uint8_t *d, const uint8_t *o, int opacity)
{
    uint32_t a = o[3];
    if (opacity != 255) a = (a * (uint32_t)opacity + 127u) / 255u;
    if (a == 0) return;
    if (a == 255) { d[0] = o[0]; d[1] = o[1]; d[2] = o[2]; return; }
    uint32_t a1 = 255u - a;
    d[0] = (uint8_t)((o[0] * a + d[0] * a1 + 128u) >> 8u);
    d[1] = (uint8_t)((o[1] * a + d[1] * a1 + 128u) >> 8u);
    d[2] = (uint8_t)((o[2] * a + d[2] * a1 + 128u) >> 8u);
}

static void hr_blend_row_scalar(uint8_t *d, const uint8_t *s, int n, int opacity)
{
    for (int i = 0; i < n; ++i) hr_blend_px(d + (size_t)i * 4, s + (size_t)i * 4, opacity);
}

#if defined(HR_HAVE_X86_SIMD)
/* 4 pixels per step. Whole groups that are fully transparent are skipped without touching the picture, fully
 * opaque groups (opacity 255) are a masked copy - text and sprites are mostly one of the two. */
__attribute__((target("sse4.1")))
static void hr_blend_row_sse41(uint8_t *d, const uint8_t *s, int n, int opacity)
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i c255 = _mm_set1_epi16(255), c127 = _mm_set1_epi16(127), c128 = _mm_set1_epi16(128), c1 = _mm_set1_epi16(1);
    const __m128i cop  = _mm_set1_epi16((short)opacity);
    const __m128i amask = _mm_set1_epi32((int)0xFF000000u);
    const __m128i a255  = _mm_set1_epi32(255);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m128i o = _mm_loadu_si128((const __m128i *)(s + (size_t)i * 4));
        const __m128i ao = _mm_srli_epi32(o, 24);
        if (_mm_testz_si128(ao, ao)) continue;                                   /* 4 transparent pixels */
        const __m128i dv = _mm_loadu_si128((const __m128i *)(d + (size_t)i * 4));
        if (opacity == 255 && _mm_movemask_epi8(_mm_cmpeq_epi32(ao, a255)) == 0xFFFF) {   /* 4 opaque pixels */
            _mm_storeu_si128((__m128i *)(d + (size_t)i * 4), _mm_blendv_epi8(o, dv, amask));
            continue;
        }
        __m128i res[2];
        for (int half = 0; half < 2; ++half) {
            const __m128i ol = half ? _mm_unpackhi_epi8(o, zero)  : _mm_unpacklo_epi8(o, zero);
            const __m128i dl = half ? _mm_unpackhi_epi8(dv, zero) : _mm_unpacklo_epi8(dv, zero);
            __m128i a = _mm_shufflehi_epi16(_mm_shufflelo_epi16(ol, 0xFF), 0xFF);   /* alpha of each pixel in all 4 lanes */
            if (opacity != 255) {
                __m128i t = _mm_add_epi16(_mm_mullo_epi16(a, cop), c127);
                a = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(t, c1), _mm_srli_epi16(t, 8)), 8);   /* exact t / 255 */
            }
            const __m128i a1  = _mm_sub_epi16(c255, a);
            __m128i mix = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(ol, a), _mm_mullo_epi16(dl, a1)), c128), 8);
            mix = _mm_blendv_epi8(mix, dl, _mm_cmpeq_epi16(a, zero));    /* a == 0   : keep the picture */
            mix = _mm_blendv_epi8(mix, ol, _mm_cmpeq_epi16(a, c255));    /* a == 255 : take the layer   */
            res[half] = _mm_blend_epi16(mix, dl, 0x88);                  /* alpha byte stays the picture's */
        }
        _mm_storeu_si128((__m128i *)(d + (size_t)i * 4), _mm_packus_epi16(res[0], res[1]));
    }
    if (i < n) hr_blend_row_scalar(d + (size_t)i * 4, s + (size_t)i * 4, n - i, opacity);
}

/* Same as above, 8 pixels per step (256-bit unpack/pack work per 128-bit lane and cancel each other out). */
__attribute__((target("avx2")))
static void hr_blend_row_avx2(uint8_t *d, const uint8_t *s, int n, int opacity)
{
    const __m256i zero = _mm256_setzero_si256();
    const __m256i c255 = _mm256_set1_epi16(255), c127 = _mm256_set1_epi16(127), c128 = _mm256_set1_epi16(128), c1 = _mm256_set1_epi16(1);
    const __m256i cop  = _mm256_set1_epi16((short)opacity);
    const __m256i amask = _mm256_set1_epi32((int)0xFF000000u);
    const __m256i a255  = _mm256_set1_epi32(255);
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256i o = _mm256_loadu_si256((const __m256i *)(s + (size_t)i * 4));
        const __m256i ao = _mm256_srli_epi32(o, 24);
        if (_mm256_testz_si256(ao, ao)) continue;                                 /* 8 transparent pixels */
        const __m256i dv = _mm256_loadu_si256((const __m256i *)(d + (size_t)i * 4));
        if (opacity == 255 && _mm256_movemask_epi8(_mm256_cmpeq_epi32(ao, a255)) == -1) {   /* 8 opaque pixels */
            _mm256_storeu_si256((__m256i *)(d + (size_t)i * 4), _mm256_blendv_epi8(o, dv, amask));
            continue;
        }
        __m256i res[2];
        for (int half = 0; half < 2; ++half) {
            const __m256i ol = half ? _mm256_unpackhi_epi8(o, zero)  : _mm256_unpacklo_epi8(o, zero);
            const __m256i dl = half ? _mm256_unpackhi_epi8(dv, zero) : _mm256_unpacklo_epi8(dv, zero);
            __m256i a = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(ol, 0xFF), 0xFF);
            if (opacity != 255) {
                __m256i t = _mm256_add_epi16(_mm256_mullo_epi16(a, cop), c127);
                a = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(t, c1), _mm256_srli_epi16(t, 8)), 8);
            }
            const __m256i a1  = _mm256_sub_epi16(c255, a);
            __m256i mix = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(ol, a), _mm256_mullo_epi16(dl, a1)), c128), 8);
            mix = _mm256_blendv_epi8(mix, dl, _mm256_cmpeq_epi16(a, zero));
            mix = _mm256_blendv_epi8(mix, ol, _mm256_cmpeq_epi16(a, c255));
            res[half] = _mm256_blend_epi16(mix, dl, 0x88);
        }
        _mm256_storeu_si256((__m256i *)(d + (size_t)i * 4), _mm256_packus_epi16(res[0], res[1]));
    }
    if (i < n) hr_blend_row_sse41(d + (size_t)i * 4, s + (size_t)i * 4, n - i, opacity);
}
#endif /* HR_HAVE_X86_SIMD */

/* Blends a w x h BGRA layer (row pitch src_stride) onto a BGRA picture (row pitch dst_stride) at `dst`, clipped
 * by the caller. opacity 0..255 is applied on top of the layer's own alpha. SSE4.1 / AVX2 are picked at run time
 * like the colour converters (hr_csc_set_max_simd() limits both). */
HR_EXPORT void hr_blend_bgra(uint8_t *dst, size_t dst_stride, const uint8_t *src, size_t src_stride,
                             int w, int h, int opacity)
{
    if (HR_UNLIKELY(!dst || !src || w <= 0 || h <= 0 || opacity <= 0)) return;
    if (opacity > 255) opacity = 255;
#if defined(HR_HAVE_X86_SIMD)
    const int level = hr_csc_simd_level();
    if (level >= 2) { for (int y = 0; y < h; ++y) hr_blend_row_avx2 (dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, w, opacity); return; }
    if (level == 1) { for (int y = 0; y < h; ++y) hr_blend_row_sse41(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, w, opacity); return; }
#endif
    for (int y = 0; y < h; ++y) hr_blend_row_scalar(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, w, opacity);
}

/* -------------------------------------------------------------------------
 * Live-preview thumbnail: BGRA -> RGB24, every output pixel = mean of nx*ny source samples
 * ---------------------------------------------------------------------- */

/* Sample columns/rows spread evenly over each destination pixel's footprint. */
static void hr_thumb_idx_spread(int *out, int dst, int src, int n)
{
    for (int d = 0; d < dst; ++d)
        for (int k = 0; k < n; ++k) {
            int64_t v = ((int64_t)(2 * n * d + 2 * k + 1) * src) / ((int64_t)2 * n * dst);
            out[(size_t)d * n + k] = (int)(v >= src ? src - 1 : v);
        }
}

/* Mean of the nx*ny samples of one destination pixel, exact floor division (reference for the SIMD code). */
static HR_INLINE void hr_thumb_px_scalar(const uint8_t *bgra, size_t stride, const int *xi, int nx,
                                         const int *yi, int ny, uint32_t cnt, uint8_t *o)
{
    uint32_t r = 0, g = 0, b = 0;
    for (int j = 0; j < ny; ++j) {
        const uint8_t *row = bgra + (size_t)yi[j] * stride;
        for (int i = 0; i < nx; ++i) {
            const uint8_t *px = row + (size_t)xi[i] * 4;
            b += px[0]; g += px[1]; r += px[2];
        }
    }
    o[0] = (uint8_t)(r / cnt); o[1] = (uint8_t)(g / cnt); o[2] = (uint8_t)(b / cnt);
}

#if defined(HR_HAVE_X86_SIMD)
/* Packs 4 BGRA-in-u32 pixels (values 0..255) to 12 bytes R,G,B,R,G,B... */
__attribute__((target("sse4.1")))
static HR_INLINE __m128i hr_thumb_pack4(__m128i p0, __m128i p1, __m128i p2, __m128i p3)
{
    const __m128i bytes = _mm_packus_epi16(_mm_packus_epi32(p0, p1), _mm_packus_epi32(p2, p3));
    return _mm_shuffle_epi8(bytes, _mm_setr_epi8(2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1));
}

/* One output row of the table-driven sampler for a fixed nx x ny: the compiler unrolls the sample loops, row
 * pointers are hoisted out of the pixel loop. 4 output pixels per step, packed with one shuffle. */
#define HR_THUMB_ROW_FN(NX, NY)                                                                                      \
__attribute__((target("sse4.1")))                                                                                    \
static void hr_thumb_row_##NX##x##NY(const uint8_t *const *rows, const int *xs, uint8_t *orow, int dw, int last_row) \
{                                                                                                                    \
    const __m128i M = _mm_set1_epi32((int)(((1u << 22) / (uint32_t)((NX) * (NY))) + 1u));                            \
    const __m128i zero = _mm_setzero_si128();                                                                        \
    int x = 0;                                                                                                       \
    for (; x + 4 <= dw; x += 4) {                                                                                    \
        __m128i acc[4];                                                                                              \
        for (int q = 0; q < 4; ++q) {                                                                                \
            const int *xi = xs + (size_t)(x + q) * (NX);                                                             \
            __m128i a = zero;                                                                                        \
            for (int j = 0; j < (NY); ++j)                                                                           \
                for (int i = 0; i < (NX); ++i) {                                                                     \
                    int32_t v; memcpy(&v, rows[j] + (size_t)xi[i] * 4, 4);                                           \
                    a = _mm_add_epi32(a, _mm_cvtepu8_epi32(_mm_cvtsi32_si128(v)));                                   \
                }                                                                                                    \
            acc[q] = _mm_srli_epi32(_mm_mullo_epi32(a, M), 22);                                                      \
        }                                                                                                            \
        const __m128i out = hr_thumb_pack4(acc[0], acc[1], acc[2], acc[3]);                                          \
        if (!last_row || x * 3 + 16 <= dw * 3) _mm_storeu_si128((__m128i *)(orow + (size_t)x * 3), out);             \
        else { uint8_t tmp[16]; _mm_storeu_si128((__m128i *)tmp, out); memcpy(orow + (size_t)x * 3, tmp, 12); }      \
    }                                                                                                                \
    for (; x < dw; ++x) {                                                                                            \
        uint32_t r = 0, g = 0, b = 0;                                                                                \
        for (int j = 0; j < (NY); ++j)                                                                               \
            for (int i = 0; i < (NX); ++i) {                                                                         \
                const uint8_t *px = rows[j] + (size_t)xs[(size_t)x * (NX) + i] * 4;                                  \
                b += px[0]; g += px[1]; r += px[2];                                                                  \
            }                                                                                                        \
        orow[(size_t)x * 3 + 0] = (uint8_t)(r / (uint32_t)((NX) * (NY)));                                            \
        orow[(size_t)x * 3 + 1] = (uint8_t)(g / (uint32_t)((NX) * (NY)));                                            \
        orow[(size_t)x * 3 + 2] = (uint8_t)(b / (uint32_t)((NX) * (NY)));                                            \
    }                                                                                                                \
}
#define HR_THUMB_ROW_FAMILY(NX) HR_THUMB_ROW_FN(NX, 1) HR_THUMB_ROW_FN(NX, 2) HR_THUMB_ROW_FN(NX, 3) HR_THUMB_ROW_FN(NX, 4)
HR_THUMB_ROW_FAMILY(1) HR_THUMB_ROW_FAMILY(2) HR_THUMB_ROW_FAMILY(3) HR_THUMB_ROW_FAMILY(4)

typedef void (*HrThumbRowFn)(const uint8_t *const *, const int *, uint8_t *, int, int);
#define HR_THUMB_PICK(NX) { hr_thumb_row_##NX##x1, hr_thumb_row_##NX##x2, hr_thumb_row_##NX##x3, hr_thumb_row_##NX##x4 }
static const HrThumbRowFn k_thumb_rows[4][4] = { HR_THUMB_PICK(1), HR_THUMB_PICK(2), HR_THUMB_PICK(3), HR_THUMB_PICK(4) };

__attribute__((target("sse4.1")))
static void hr_thumb_sse41(const uint8_t *bgra, size_t stride, uint8_t *dst, int dw, int dh,
                           const int *xs, int nx, const int *ys, int ny, int box2x2)
{
    const __m128i zero = _mm_setzero_si128();
    const HrThumbRowFn row_fn = (nx >= 1 && nx <= 4 && ny >= 1 && ny <= 4) ? k_thumb_rows[nx - 1][ny - 1] : NULL;
    const uint32_t cnt = (uint32_t)(nx * ny);

    for (int y = 0; y < dh; ++y) {
        uint8_t *orow = dst + (size_t)y * dw * 3;
        if (box2x2) {
            /* contiguous 2x2 blocks: 8 source pixels of two rows -> 4 output pixels */
            const uint8_t *r0 = bgra + (size_t)ys[y * 2] * stride;
            const uint8_t *r1 = r0 + stride;
            int x = 0;
            for (; x + 4 <= dw; x += 4) {
                __m128i pp[2];
                for (int k = 0; k < 2; ++k) {
                    const size_t off = (size_t)(x * 2 + k * 4) * 4;
                    const __m128i a = _mm_loadu_si128((const __m128i *)(r0 + off));
                    const __m128i b = _mm_loadu_si128((const __m128i *)(r1 + off));
                    const __m128i vlo = _mm_add_epi16(_mm_unpacklo_epi8(a, zero), _mm_unpacklo_epi8(b, zero));
                    const __m128i vhi = _mm_add_epi16(_mm_unpackhi_epi8(a, zero), _mm_unpackhi_epi8(b, zero));
                    const __m128i hlo = _mm_add_epi16(vlo, _mm_srli_si128(vlo, 8));
                    const __m128i hhi = _mm_add_epi16(vhi, _mm_srli_si128(vhi, 8));
                    pp[k] = _mm_srli_epi16(_mm_unpacklo_epi64(hlo, hhi), 2);       /* two output pixels, u16 lanes */
                }
                const __m128i out = _mm_shuffle_epi8(_mm_packus_epi16(pp[0], pp[1]),
                                    _mm_setr_epi8(2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1));
                if (y < dh - 1 || x * 3 + 16 <= dw * 3)
                    _mm_storeu_si128((__m128i *)(orow + (size_t)x * 3), out);       /* 4 spare bytes are overwritten next */
                else { uint8_t tmp[16]; _mm_storeu_si128((__m128i *)tmp, out); memcpy(orow + (size_t)x * 3, tmp, 12); }
            }
            for (; x < dw; ++x)
                hr_thumb_px_scalar(bgra, stride, xs + (size_t)x * nx, nx, ys + (size_t)y * ny, ny, cnt, orow + (size_t)x * 3);
        } else {
            const uint8_t *rows[4];
            for (int j = 0; j < ny && j < 4; ++j) rows[j] = bgra + (size_t)ys[(size_t)y * ny + j] * stride;
            row_fn(rows, xs, orow, dw, y == dh - 1);
        }
    }
}
#endif /* HR_HAVE_X86_SIMD */

/* Live-preview thumbnail: BGRA (row pitch `stride` bytes, 0 = sw*4) -> tightly packed RGB24, dw x dh.
 * Sampling (same as the former C++ code in pipeline.cpp):
 *   integer ratio, > 4 pixels per block : 2x2 samples spread over the block (touches ~1/4 of the memory or less)
 *   integer ratio, <= 4 pixels          : every pixel of the block
 *   other ratios                        : up to 4x4 samples spread over the footprint
 * Output = floor(mean) per channel. */
HR_EXPORT void hr_bgra_to_thumb(const uint8_t *bgra, size_t stride, uint8_t *rgb, int sw, int sh, int dw, int dh)
{
    if (HR_UNLIKELY(!bgra || !rgb || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)) return;
    if (stride == 0) stride = (size_t)sw * 4;

    int nx, ny, rx = 0, ry = 0, integer = ((sw % dw) == 0 && (sh % dh) == 0);
    if (integer) {
        rx = sw / dw; ry = sh / dh;
        if (rx * ry > 4) { nx = rx < 2 ? rx : 2; ny = ry < 2 ? ry : 2; }
        else             { nx = rx; ny = ry; }
    } else {
        nx = (sw + dw - 1) / dw; if (nx < 1) nx = 1; if (nx > 4) nx = 4;
        ny = (sh + dh - 1) / dh; if (ny < 1) ny = 1; if (ny > 4) ny = 4;
    }
    int *xs = (int *)malloc(((size_t)dw * nx + (size_t)dh * ny) * sizeof(int));
    if (!xs) return;
    int *ys = xs + (size_t)dw * nx;
    if (integer) {
        int ox[2] = {0, 0}, oy[2] = {0, 0};
        if (rx * ry > 4) { ox[0] = rx / 4; ox[1] = (rx * 3) / 4; oy[0] = ry / 4; oy[1] = (ry * 3) / 4; }
        const int sparse = rx * ry > 4;
        for (int x = 0; x < dw; ++x) for (int i = 0; i < nx; ++i) xs[(size_t)x * nx + i] = x * rx + (sparse ? ox[i] : i);
        for (int y = 0; y < dh; ++y) for (int j = 0; j < ny; ++j) ys[(size_t)y * ny + j] = y * ry + (sparse ? oy[j] : j);
    } else {
        hr_thumb_idx_spread(xs, dw, sw, nx);
        hr_thumb_idx_spread(ys, dh, sh, ny);
    }

#if defined(HR_HAVE_X86_SIMD)
    if (hr_csc_simd_level() >= 1) {
        const int box2x2 = integer && rx == 2 && ry == 2;
        hr_thumb_sse41(bgra, stride, rgb, dw, dh, xs, nx, ys, ny, box2x2);
        free(xs);
        return;
    }
#endif
    {
        const uint32_t cnt = (uint32_t)(nx * ny);
        for (int y = 0; y < dh; ++y) {
            uint8_t *orow = rgb + (size_t)y * dw * 3;
            for (int x = 0; x < dw; ++x)
                hr_thumb_px_scalar(bgra, stride, xs + (size_t)x * nx, nx, ys + (size_t)y * ny, ny, cnt, orow + (size_t)x * 3);
        }
    }
    free(xs);
}


HR_EXPORT void hr_memcpy_nt(void *dst, const void *src, size_t n)
{
#if defined(__SSE2__)
    if (n >= 262144u) {
        unsigned char       *d = (unsigned char *)dst;
        const unsigned char *s = (const unsigned char *)src;

        /* Align destination to 16 bytes */
        size_t head = (16u - ((uintptr_t)d & 15u)) & 15u;
        if (head > n) head = n;
        memcpy(d, s, head);
        d += head; s += head; n -= head;

        size_t chunks = n / 16u;
        for (size_t i = 0; i < chunks; ++i) {
            __m128i v = _mm_loadu_si128((const __m128i *)(s + i * 16));
            /* OPT: _mm_stream_si128 - реальный NT store, обходит кэш */
            _mm_stream_si128((__m128i *)(d + i * 16), v);
        }
        _mm_sfence();

        size_t tail = n - chunks * 16u;
        memcpy(d + chunks * 16u, s + chunks * 16u, tail);
        return;
    }
#endif
    memcpy(dst, src, n);
}
