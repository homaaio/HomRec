#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#if defined(__SSE2__)
  #include <immintrin.h>
  #include <emmintrin.h>
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #include <smmintrin.h>  /* SSE4.1 - _mm_mullo_epi32/_mm_packus_epi32, for the Y-channel fast path below */
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

        /* hoist the per-row output bases out of the pixel loop below.
         * y*width/y1*width/(y/2)*(width/2) used to be recomputed (as a
         * fresh multiply) for every single pixel; each is now the same
         * value for the whole row, so it's computed once here and the
         * inner loop just indexes off these row pointers instead. */
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

/* -------------------------------------------------------------------------
 * BGRA -> YUV 4:2:0 (planar I420 or semi-planar NV12)
 *
 * One shared implementation for both output layouts; only the way the
 * chroma samples are stored differs.  Two colour matrices are supported
 * (both BT.601, matching the "-colorspace smpte170m" tag the ffmpeg runner
 * writes):
 *
 *   k_csc_full - full/PC range   (Y 0..255, C 0..255)  -> planar I420,
 *                fed to libx264/libx265 and tagged "-color_range pc".
 *   k_csc_tv   - studio/TV range (Y 16..235, C 16..240) -> NV12, fed to
 *                NVENC/QSV/AMF, tagged "-color_range tv".
 *
 * BUGFIX: hr_bgra_to_nv12_band() used to reuse the full-range maths while
 * hr_ffmpeg_runner.cpp already tags hardware-encoder output as limited
 * range ("-color_range tv", see CHANGELOG "hardware-encoded recordings came
 * out dark/dull").  Full-range samples decoded as limited range crush every
 * shadow below Y=16 to pure black and clip highlights above 235 - exactly the
 * "dark/dull picture" that entry describes.  The NV12 path now really emits
 * 16..235 / 16..240 data.
 *
 * Chroma is the average of each 2x2 block.  The four samples are summed
 * (0..1020) and scaled once (>>18) instead of truncating an average first,
 * which is both a little more accurate and cheaper.
 * ---------------------------------------------------------------------- */
typedef struct {
    int yr, yg, yb;     /* luma coefficients, scaled by 65536            */
    int yoff;           /* luma offset added after the shift (0 or 16)   */
    int cbr, cbg, cbb;  /* Cb coefficients (2x2 sums), scaled by 65536   */
    int crr, crg, crb;  /* Cr coefficients                               */
} HrCsc;

static const HrCsc k_csc_full = {
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
    const uint8_t * HR_RESTRICT bgra,
    uint8_t       * HR_RESTRICT out,
    int width, int height, int y0, int y1, int xs, int nv12, const HrCsc *k)
{
    size_t frame_sz = (size_t)width * (size_t)height;
    uint8_t *Y  = out;
    uint8_t *C0 = out + frame_sz;                               /* Cb plane, or the UV plane */
    uint8_t *C1 = out + frame_sz + frame_sz / 4;                /* Cr plane (I420 only)      */

    for (int y = y0; y < y1; y += 2) {
        const uint8_t *row0 = bgra + (size_t)y * width * 4;
        const uint8_t *row1 = (y + 1 < height) ? bgra + (size_t)(y + 1) * width * 4 : row0;
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
    const uint8_t * HR_RESTRICT bgra,
    uint8_t       * HR_RESTRICT out,
    int width, int height, int y0, int y1, int nv12, const HrCsc *k)
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
        const uint8_t *row0 = bgra + (size_t)y * width * 4;
        const uint8_t *row1 = (y + 1 < height) ? bgra + (size_t)(y + 1) * width * 4 : row0;
        int y1r = (y + 1 < height) ? y + 1 : y;
        uint8_t *Yrow0 = Y + (size_t)y   * width;
        uint8_t *Yrow1 = Y + (size_t)y1r * width;
        size_t cbase = (size_t)(y / 2) * (size_t)(nv12 ? width : width / 2);

        for (int x = 0; x < vec_w; x += 8) {
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
        hr_csc_band_scalar(bgra, out, width, height, y0, y1, vec_w, nv12, k);
}
#endif /* HR_HAVE_X86_SIMD */

static void hr_csc_band(
    const uint8_t *bgra, uint8_t *out,
    int width, int height, int y0, int y1, int nv12, const HrCsc *k)
{
    if (HR_UNLIKELY(!bgra || !out || width <= 0 || height <= 0)) return;
    if (y0 < 0) y0 = 0;
    if (y1 > height) y1 = height;
    y0 &= ~1;                       /* chroma rows come in pairs */
    if (y0 >= y1) return;
#if defined(HR_HAVE_X86_SIMD)
    static int has_sse41 = -1;
    if (HR_UNLIKELY(has_sse41 < 0)) {
        __builtin_cpu_init();
        has_sse41 = __builtin_cpu_supports("sse4.1") ? 1 : 0;
    }
    if (has_sse41) {
        hr_csc_band_sse41(bgra, out, width, height, y0, y1, nv12, k);
        return;
    }
#endif
    hr_csc_band_scalar(bgra, out, width, height, y0, y1, 0, nv12, k);
}

/* Planar I420, full/PC range - software encoders (libx264/libx265). */
HR_EXPORT void hr_bgra_to_yuv420p_band(
    const uint8_t * HR_RESTRICT bgra,
    uint8_t       * HR_RESTRICT yuv_out,
    int width, int height, int y0, int y1)
{
    hr_csc_band(bgra, yuv_out, width, height, y0, y1, 0, &k_csc_full);
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
 * swscale pass).  See hr_ffmpeg_runner.cpp's matching "-pix_fmt nv12 -color_range tv". */
HR_EXPORT void hr_bgra_to_nv12_band(
    const uint8_t * HR_RESTRICT bgra,
    uint8_t       * HR_RESTRICT nv12_out,
    int width, int height, int y0, int y1)
{
    hr_csc_band(bgra, nv12_out, width, height, y0, y1, 1, &k_csc_tv);
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
        /* row bases hoisted out of the x loop (same idea as the
         * encode-direction functions above) - one multiply per row
         * instead of one per pixel for both the Y/chroma reads and the
         * RGB write. */
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
