#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#if defined(__SSE2__)
  #include <immintrin.h>
  #include <emmintrin.h>
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

        int x = 0;
        /* Unroll x2: process two chroma blocks per iteration */
        for (; x + 3 < width; x += 4) {
            /* Block 0: x, x+1 */
            uint8_t r00=row0[x*3+0],   g00=row0[x*3+1],   b00=row0[x*3+2];
            uint8_t r01=row0[(x+1)*3+0],g01=row0[(x+1)*3+1],b01=row0[(x+1)*3+2];
            uint8_t r10=row1[x*3+0],   g10=row1[x*3+1],   b10=row1[x*3+2];
            uint8_t r11=row1[(x+1)*3+0],g11=row1[(x+1)*3+1],b11=row1[(x+1)*3+2];

            Y[y *width+x ]=(uint8_t)((YR*r00+YG*g00+YB*b00+32768)>>16);
            Y[y *width+x+1]=(uint8_t)((YR*r01+YG*g01+YB*b01+32768)>>16);
            Y[y1*width+x ]=(uint8_t)((YR*r10+YG*g10+YB*b10+32768)>>16);
            Y[y1*width+x+1]=(uint8_t)((YR*r11+YG*g11+YB*b11+32768)>>16);
            {
                int ra=((int)r00+r01+r10+r11)>>2, ga=((int)g00+g01+g10+g11)>>2, ba=((int)b00+b01+b10+b11)>>2;
                size_t ci=(size_t)(y/2)*(size_t)(width/2)+(size_t)(x/2);
                Cb[ci]=_clamp8(((-CBR*ra-CBG*ga+CBB*ba+32768)>>16)+128);
                Cr[ci]=_clamp8((( CRR*ra-CRG*ga-CRB*ba+32768)>>16)+128);
            }

            /* Block 1: x+2, x+3 */
            uint8_t r02=row0[(x+2)*3+0],g02=row0[(x+2)*3+1],b02=row0[(x+2)*3+2];
            uint8_t r03=row0[(x+3)*3+0],g03=row0[(x+3)*3+1],b03=row0[(x+3)*3+2];
            uint8_t r12=row1[(x+2)*3+0],g12=row1[(x+2)*3+1],b12=row1[(x+2)*3+2];
            uint8_t r13=row1[(x+3)*3+0],g13=row1[(x+3)*3+1],b13=row1[(x+3)*3+2];

            Y[y *width+x+2]=(uint8_t)((YR*r02+YG*g02+YB*b02+32768)>>16);
            Y[y *width+x+3]=(uint8_t)((YR*r03+YG*g03+YB*b03+32768)>>16);
            Y[y1*width+x+2]=(uint8_t)((YR*r12+YG*g12+YB*b12+32768)>>16);
            Y[y1*width+x+3]=(uint8_t)((YR*r13+YG*g13+YB*b13+32768)>>16);
            {
                int ra=((int)r02+r03+r12+r13)>>2, ga=((int)g02+g03+g12+g13)>>2, ba=((int)b02+b03+b12+b13)>>2;
                size_t ci=(size_t)(y/2)*(size_t)(width/2)+(size_t)((x+2)/2);
                Cb[ci]=_clamp8(((-CBR*ra-CBG*ga+CBB*ba+32768)>>16)+128);
                Cr[ci]=_clamp8((( CRR*ra-CRG*ga-CRB*ba+32768)>>16)+128);
            }
        }
        /* Tail: remaining 0 or 2 pixels */
        for (; x < width; x += 2) {
            int x1 = (x+1 < width) ? x+1 : x;
            uint8_t r00=row0[x*3+0],g00=row0[x*3+1],b00=row0[x*3+2];
            uint8_t r01=row0[x1*3+0],g01=row0[x1*3+1],b01=row0[x1*3+2];
            uint8_t r10=row1[x*3+0],g10=row1[x*3+1],b10=row1[x*3+2];
            uint8_t r11=row1[x1*3+0],g11=row1[x1*3+1],b11=row1[x1*3+2];
            Y[y*width+x ]=(uint8_t)((YR*r00+YG*g00+YB*b00+32768)>>16);
            Y[y*width+x1]=(uint8_t)((YR*r01+YG*g01+YB*b01+32768)>>16);
            Y[y1*width+x ]=(uint8_t)((YR*r10+YG*g10+YB*b10+32768)>>16);
            Y[y1*width+x1]=(uint8_t)((YR*r11+YG*g11+YB*b11+32768)>>16);
            int ra=((int)r00+r01+r10+r11)>>2,ga=((int)g00+g01+g10+g11)>>2,ba=((int)b00+b01+b10+b11)>>2;
            size_t ci=(size_t)(y/2)*(size_t)(width/2)+(size_t)(x/2);
            Cb[ci]=_clamp8(((-CBR*ra-CBG*ga+CBB*ba+32768)>>16)+128);
            Cr[ci]=_clamp8((( CRR*ra-CRG*ga-CRB*ba+32768)>>16)+128);
        }
    }
}

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
        const uint8_t *row0 = bgra + (size_t)y * width * 4;
        const uint8_t *row1 = (y + 1 < height)
                            ? bgra + (size_t)(y+1) * width * 4
                            : row0;
        int y1 = (y+1 < height) ? y+1 : y;

        int x = 0;
        for (; x + 3 < width; x += 4) {
            /* BGRA: 0=B,1=G,2=R,3=A */
            uint8_t r00=row0[x*4+2],g00=row0[x*4+1],b00=row0[x*4+0];
            uint8_t r01=row0[(x+1)*4+2],g01=row0[(x+1)*4+1],b01=row0[(x+1)*4+0];
            uint8_t r10=row1[x*4+2],g10=row1[x*4+1],b10=row1[x*4+0];
            uint8_t r11=row1[(x+1)*4+2],g11=row1[(x+1)*4+1],b11=row1[(x+1)*4+0];
            Y[y*width+x]  =(uint8_t)((YR*r00+YG*g00+YB*b00+32768)>>16);
            Y[y*width+x+1]=(uint8_t)((YR*r01+YG*g01+YB*b01+32768)>>16);
            Y[y1*width+x] =(uint8_t)((YR*r10+YG*g10+YB*b10+32768)>>16);
            Y[y1*width+x+1]=(uint8_t)((YR*r11+YG*g11+YB*b11+32768)>>16);
            {
                int ra=((int)r00+r01+r10+r11)>>2,ga=((int)g00+g01+g10+g11)>>2,ba=((int)b00+b01+b10+b11)>>2;
                size_t ci=(size_t)(y/2)*(size_t)(width/2)+(size_t)(x/2);
                Cb[ci]=_clamp8(((-CBR*ra-CBG*ga+CBB*ba+32768)>>16)+128);
                Cr[ci]=_clamp8((( CRR*ra-CRG*ga-CRB*ba+32768)>>16)+128);
            }

            uint8_t r02=row0[(x+2)*4+2],g02=row0[(x+2)*4+1],b02=row0[(x+2)*4+0];
            uint8_t r03=row0[(x+3)*4+2],g03=row0[(x+3)*4+1],b03=row0[(x+3)*4+0];
            uint8_t r12=row1[(x+2)*4+2],g12=row1[(x+2)*4+1],b12=row1[(x+2)*4+0];
            uint8_t r13=row1[(x+3)*4+2],g13=row1[(x+3)*4+1],b13=row1[(x+3)*4+0];
            Y[y*width+x+2]=(uint8_t)((YR*r02+YG*g02+YB*b02+32768)>>16);
            Y[y*width+x+3]=(uint8_t)((YR*r03+YG*g03+YB*b03+32768)>>16);
            Y[y1*width+x+2]=(uint8_t)((YR*r12+YG*g12+YB*b12+32768)>>16);
            Y[y1*width+x+3]=(uint8_t)((YR*r13+YG*g13+YB*b13+32768)>>16);
            {
                int ra=((int)r02+r03+r12+r13)>>2,ga=((int)g02+g03+g12+g13)>>2,ba=((int)b02+b03+b12+b13)>>2;
                size_t ci=(size_t)(y/2)*(size_t)(width/2)+(size_t)((x+2)/2);
                Cb[ci]=_clamp8(((-CBR*ra-CBG*ga+CBB*ba+32768)>>16)+128);
                Cr[ci]=_clamp8((( CRR*ra-CRG*ga-CRB*ba+32768)>>16)+128);
            }
        }
        for (; x < width; x += 2) {
            int x1=(x+1<width)?x+1:x;
            uint8_t r00=row0[x*4+2],g00=row0[x*4+1],b00=row0[x*4+0];
            uint8_t r01=row0[x1*4+2],g01=row0[x1*4+1],b01=row0[x1*4+0];
            uint8_t r10=row1[x*4+2],g10=row1[x*4+1],b10=row1[x*4+0];
            uint8_t r11=row1[x1*4+2],g11=row1[x1*4+1],b11=row1[x1*4+0];
            Y[y*width+x] =(uint8_t)((YR*r00+YG*g00+YB*b00+32768)>>16);
            Y[y*width+x1]=(uint8_t)((YR*r01+YG*g01+YB*b01+32768)>>16);
            Y[y1*width+x] =(uint8_t)((YR*r10+YG*g10+YB*b10+32768)>>16);
            Y[y1*width+x1]=(uint8_t)((YR*r11+YG*g11+YB*b11+32768)>>16);
            int ra=((int)r00+r01+r10+r11)>>2,ga=((int)g00+g01+g10+g11)>>2,ba=((int)b00+b01+b10+b11)>>2;
            size_t ci=(size_t)(y/2)*(size_t)(width/2)+(size_t)(x/2);
            Cb[ci]=_clamp8(((-CBR*ra-CBG*ga+CBB*ba+32768)>>16)+128);
            Cr[ci]=_clamp8((( CRR*ra-CRG*ga-CRB*ba+32768)>>16)+128);
        }
    }
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
        for (int x = 0; x < width; ++x) {
            int luma = (int)Y[(size_t)y * width + x] - 16;
            size_t ci = (size_t)(y/2) * (size_t)(width/2) + (size_t)(x/2);
            int cb = (int)Cb[ci] - 128;
            int cr = (int)Cr[ci] - 128;
            int r = (298*luma           + 409*cr + 128) >> 8;
            int g = (298*luma - 100*cb - 208*cr + 128) >> 8;
            int b = (298*luma + 516*cb           + 128) >> 8;
            uint8_t *out = rgb_out + ((size_t)y * width + x) * 3;
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
            /* OPT: _mm_stream_si128 — реальный NT store, обходит кэш */
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
