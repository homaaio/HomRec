#include "overlay_render.h"
#include "../utils/log.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <wincodec.h>
  #include <propvarutil.h> // PropVariantInit/PropVariantClear, used by GIF frame-metadata reads below
  #include <wrl/client.h>
  using Microsoft::WRL::ComPtr;
#endif

#include <algorithm>
#include <cstring>
#include <ctime>
#include <exception>
#include "wgc_capture.h"   // 8th pass: window layers that capture the window itself (after NOMINMAX above)

namespace {
// Bounded strcpy into a fixed-size char array (always NUL-terminates).
// Used by the overlay-cache staleness snapshots below.
void _scopy_local(char *dst, size_t dstlen, const char *src) {
    if (!dst || dstlen == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < dstlen && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}
}

// Composite: alpha-blend an RGBA/BGRA overlay buffer onto a BGRA base buffer at (dst_x,
// dst_y), clipping to the base's bounds.
static void CompositeBgra(uint8_t *base, int base_w, int base_h, int base_stride,
                           const uint8_t *overlay, int ov_w, int ov_h,
                           int dst_x, int dst_y, uint8_t extra_opacity = 255)
{
    if (!base || !overlay || ov_w <= 0 || ov_h <= 0 || extra_opacity == 0) return;

    int src_x0 = 0, src_y0 = 0;
    int dx0 = dst_x, dy0 = dst_y;
    if (dx0 < 0) { src_x0 = -dx0; dx0 = 0; }
    if (dy0 < 0) { src_y0 = -dy0; dy0 = 0; }
    int copy_w = std::min(ov_w - src_x0, base_w - dx0);
    int copy_h = std::min(ov_h - src_y0, base_h - dy0);
    if (copy_w <= 0 || copy_h <= 0) return;

    for (int row = 0; row < copy_h; ++row) {
        uint8_t *dst_row = base + (size_t)(dy0 + row) * base_stride + (size_t)dx0 * 4;
        const uint8_t *src_row = overlay + (size_t)(src_y0 + row) * (size_t)ov_w * 4 + (size_t)src_x0 * 4;
        for (int col = 0; col < copy_w; ++col) {
            const uint8_t *o = src_row + (size_t)col * 4;
            uint8_t *d = dst_row + (size_t)col * 4;
            uint32_t a = o[3];
            if (extra_opacity != 255) a = (a * extra_opacity + 127u) / 255u;
            if (a == 0) continue;               // fully transparent
            if (a == 255) {                      // fully opaque
                d[0] = o[0]; d[1] = o[1]; d[2] = o[2];
                continue;
            }
            uint32_t a1 = 255u - a;
            // Rounded alpha blend (same rounding fix as hr_blend_rgba).
            d[0] = (uint8_t)((o[0] * a + d[0] * a1 + 128u) >> 8u);
            d[1] = (uint8_t)((o[1] * a + d[1] * a1 + 128u) >> 8u);
            d[2] = (uint8_t)((o[2] * a + d[2] * a1 + 128u) >> 8u);
        }
    }
}

// Composite: alpha-blend a BGRA overlay layer straight into an NV12 frame.
static void CompositeBgraNv12(uint8_t *nv12, int w, int h,
                              const uint8_t *ov, int ov_w, int ov_h,
                              int dst_x, int dst_y, uint8_t extra_opacity)
{
    if (!nv12 || !ov || w < 2 || h < 2 || ov_w <= 0 || ov_h <= 0 || extra_opacity == 0) return;

    const int x0 = std::max(dst_x, 0), y0 = std::max(dst_y, 0);
    const int x1 = std::min(dst_x + ov_w, w), y1 = std::min(dst_y + ov_h, h);
    if (x0 >= x1 || y0 >= y1) return;

    uint8_t *Yp  = nv12;
    uint8_t *UVp = nv12 + (size_t)w * (size_t)h;
    auto clamp8 = [](int v) -> int { return v < 0 ? 0 : (v > 255 ? 255 : v); };

    for (int by = y0 & ~1; by < y1; by += 2) {
        for (int bx = x0 & ~1; bx < x1; bx += 2) {
            int sum_a = 0, rs = 0, gs = 0, bs = 0;   // alpha-weighted colour of this 2x2 block
            int n_opaque = 0, rr = 0, gg = 0, bb = 0; // plain sums, for the all-opaque fast path
            for (int j = 0; j < 2; ++j) {
                const int py = by + j;
                if (py < y0 || py >= y1) continue;
                const uint8_t *srow = ov + (size_t)(py - dst_y) * (size_t)ov_w * 4;
                uint8_t *yrow = Yp + (size_t)py * w;
                for (int i = 0; i < 2; ++i) {
                    const int px = bx + i;
                    if (px < x0 || px >= x1) continue;
                    const uint8_t *sp = srow + (size_t)(px - dst_x) * 4;
                    int a = sp[3];
                    if (extra_opacity != 255) a = (a * extra_opacity + 127) / 255;
                    if (a == 0) continue;
                    const int b = sp[0], g = sp[1], r = sp[2];
                    const int yc = (16829 * r + 33039 * g + 6416 * b + 32768 + (16 << 16)) >> 16;
                    if (a == 255) {
                        yrow[px] = (uint8_t)yc;
                        ++n_opaque; rr += r; gg += g; bb += b;
                    } else {
                        yrow[px] = (uint8_t)((yc * a + (int)yrow[px] * (255 - a) + 127) / 255);
                    }
                    rs += r * a; gs += g * a; bs += b * a; sum_a += a;
                }
            }
            if (sum_a == 0) continue;

            int r4, g4, b4;                           // mean colour of the block scaled to a 4-pixel sum
            if (n_opaque == 4) { r4 = rr; g4 = gg; b4 = bb; }   // fully opaque block: exact, no division
            else               { r4 = rs * 4 / sum_a; g4 = gs * 4 / sum_a; b4 = bs * 4 / sum_a; }
            const int cb = clamp8(((-9714 * r4 - 19071 * g4 + 28785 * b4 + (1 << 17)) >> 18) + 128);
            const int cr = clamp8(((28785 * r4 - 24103 * g4 - 4682 * b4 + (1 << 17)) >> 18) + 128);
            const int cov = std::min(sum_a, 1020);   // 4 pixels * 255
            uint8_t *uv = UVp + (size_t)(by / 2) * w + bx;
            if (cov == 1020) { uv[0] = (uint8_t)cb; uv[1] = (uint8_t)cr; }
            else {
                uv[0] = (uint8_t)((uv[0] * (1020 - cov) + cb * cov + 510) / 1020);
                uv[1] = (uint8_t)((uv[1] * (1020 - cov) + cr * cov + 510) / 1020);
            }
        }
    }
}

#ifdef _WIN32

// Nearest-neighbor resample of a BGRA buffer, alpha included.
static void ScaleBgraNearest(const std::vector<uint8_t> &src, int sw, int sh,
                              std::vector<uint8_t> &dst, int dw, int dh)
{
    dst.assign((size_t)dw * dh * 4, 0);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    for (int y = 0; y < dh; ++y) {
        int sy = (int)((int64_t)y * sh / dh);
        if (sy >= sh) sy = sh - 1;
        const uint8_t *srow = src.data() + (size_t)sy * sw * 4;
        uint8_t *drow = dst.data() + (size_t)y * dw * 4;
        for (int x = 0; x < dw; ++x) {
            int sx = (int)((int64_t)x * sw / dw);
            if (sx >= sw) sx = sw - 1;
            std::memcpy(drow + (size_t)x * 4, srow + (size_t)sx * 4, 4);
        }
    }
}

static void ScaleRectBox(const uint8_t *src, size_t src_stride, int sx, int sy, int sw, int sh,
                         uint8_t *dst, int dw, int dh)
{
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    const bool avg_x = sw >= dw * 3 / 2, avg_y = sh >= dh * 3 / 2;
    for (int y = 0; y < dh; ++y) {
        const int y0 = sy + (int)((int64_t)y * sh / dh);
        int y1 = avg_y ? sy + (int)((int64_t)(y + 1) * sh / dh) - 1 : y0;
        if (y1 < y0) y1 = y0;
        const uint8_t *r0 = src + (size_t)y0 * src_stride;
        const uint8_t *r1 = src + (size_t)std::min(y1, y0 + 1 + (y1 - y0) / 2) * src_stride;
        uint8_t *drow = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; ++x) {
            const int x0 = sx + (int)((int64_t)x * sw / dw);
            int x1 = avg_x ? sx + (int)((int64_t)(x + 1) * sw / dw) - 1 : x0;
            if (x1 < x0) x1 = x0;
            const int xb = std::min(x1, x0 + 1 + (x1 - x0) / 2);
            const uint8_t *p00 = r0 + (size_t)x0 * 4, *p01 = r0 + (size_t)xb * 4;
            const uint8_t *p10 = r1 + (size_t)x0 * 4, *p11 = r1 + (size_t)xb * 4;
            uint8_t *d = drow + (size_t)x * 4;
            d[0] = (uint8_t)((p00[0] + p01[0] + p10[0] + p11[0] + 2) >> 2);
            d[1] = (uint8_t)((p00[1] + p01[1] + p10[1] + p11[1] + 2) >> 2);
            d[2] = (uint8_t)((p00[2] + p01[2] + p10[2] + p11[2] + 2) >> 2);
            d[3] = 255;
        }
    }
}

static bool RenderTextBgra(const std::wstring &text, int w, int h, COLORREF color,
                            const std::wstring &font_name, std::vector<uint8_t> &out)
{
    if (w <= 0 || h <= 0 || text.empty()) return false;
    out.assign((size_t)w * h * 4, 0);

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP dib = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return false;
    }
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, dib);
    memset(bits, 0, (size_t)w * h * 4); // black background -> 0 coverage everywhere

    // Font size derived from the overlay box height; comfortably fits with
    // a little padding, matching how the drag-resize handle on the preview
    // implies "this box is roughly how big the text will be".
    int pointSize = std::max(8, (int)(h * 0.65));
    const wchar_t *face = font_name.empty() ? L"Segoe UI" : font_name.c_str();
    HFONT font = CreateFontW(-pointSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
    HFONT oldFont = (HFONT)SelectObject(memDC, font);
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255));

    RECT rc{0, 0, w, h};
    DrawTextW(memDC, text.c_str(), (int)text.size(), &rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    SelectObject(memDC, oldFont);
    DeleteObject(font);

    uint8_t cr = GetRValue(color), cg = GetGValue(color), cb = GetBValue(color);
    const uint8_t *src = (const uint8_t *)bits;
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        const uint8_t *p = src + i * 4;   // GDI DIB bytes: B,G,R,X
        uint8_t coverage = p[0];           // R==G==B here (drawn white on black)
        uint8_t *o = out.data() + i * 4;
        o[0] = cb; o[1] = cg; o[2] = cr; o[3] = coverage;
    }

    SelectObject(memDC, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    return true;
}

// Image: decode via WIC (built into Windows -- handles PNG/JPG/BMP/GIF/etc.
// without pulling in a third-party image library) straight to a
// premultiplied-alpha-free 32bpp BGRA buffer, scaled to the overlay's w x h.
static bool RenderImageBgraNative(const std::wstring &path, std::vector<uint8_t> &out, int &out_w, int &out_h)
{
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(IWICImagingFactory),
                                   reinterpret_cast<void **>(factory.GetAddressOf()));
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                             WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;

    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return false;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return false;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                      WICBitmapDitherTypeNone, nullptr, 0.0,
                                      WICBitmapPaletteTypeCustom))) {
        return false;
    }

    out.assign((size_t)w * h * 4, 0);
    hr = converter->CopyPixels(nullptr, (UINT)(w * 4), (UINT)out.size(), out.data());
    if (FAILED(hr)) return false;
    out_w = (int)w; out_h = (int)h;
    return true;
}

struct GifFrameNative {
    std::vector<uint8_t> bgra;
    int delay_ms = 100;
};

// Defined further below; forward-declared here since DecodeGifFramesNative
// needs it to composite each decoded frame onto the running canvas.
static void BlitBgraRect(uint8_t *dst, int dst_w, int dst_h,
                          const uint8_t *src, int src_stride_w, int src_h,
                          int src_x, int src_y, int rect_w, int rect_h,
                          int dst_x, int dst_y);

static bool DecodeGifFramesNative(const std::wstring &path, std::vector<GifFrameNative> &frames,
                                   int &canvas_w, int &canvas_h)
{
    frames.clear();
    canvas_w = canvas_h = 0;

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(IWICImagingFactory),
                                   reinterpret_cast<void **>(factory.GetAddressOf()));
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                             WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return false;

    UINT frame_count = 0;
    decoder->GetFrameCount(&frame_count);
    if (frame_count == 0) return false;

    ComPtr<IWICBitmapFrameDecode> frame0;
    if (FAILED(decoder->GetFrame(0, &frame0))) return false;

    // The overall canvas size lives in the GIF's logical screen descriptor, which WIC only
    // exposes through frame 0's metadata reader (not a decoder-level one).
    {
        ComPtr<IWICMetadataQueryReader> qr;
        if (SUCCEEDED(frame0->GetMetadataQueryReader(&qr))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(qr->GetMetadataByName(L"/logscrdesc/Width", &pv)) && pv.vt == VT_UI2) canvas_w = pv.uiVal;
            PropVariantClear(&pv);
            PropVariantInit(&pv);
            if (SUCCEEDED(qr->GetMetadataByName(L"/logscrdesc/Height", &pv)) && pv.vt == VT_UI2) canvas_h = pv.uiVal;
            PropVariantClear(&pv);
        }
    }
    if (canvas_w <= 0 || canvas_h <= 0) {
        UINT w = 0, h = 0;
        if (FAILED(frame0->GetSize(&w, &h)) || w == 0 || h == 0) return false;
        canvas_w = (int)w; canvas_h = (int)h;
    }

    std::vector<uint8_t> canvas((size_t)canvas_w * canvas_h * 4, 0); // starts fully transparent
    std::vector<uint8_t> prev_snapshot; // only populated when a frame needs "restore to previous"

    for (UINT i = 0; i < frame_count; ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(i, &frame))) break;

        UINT fw = 0, fh = 0;
        if (FAILED(frame->GetSize(&fw, &fh)) || fw == 0 || fh == 0) break;

        int left = 0, top = 0, delay_cs = 10, disposal = 0;
        ComPtr<IWICMetadataQueryReader> qr;
        if (SUCCEEDED(frame->GetMetadataQueryReader(&qr))) {
            PROPVARIANT pv;
            auto readU2 = [&](const wchar_t *name, int &out) {
                PropVariantInit(&pv);
                if (SUCCEEDED(qr->GetMetadataByName(name, &pv)) && pv.vt == VT_UI2) out = pv.uiVal;
                PropVariantClear(&pv);
            };
            auto readU1 = [&](const wchar_t *name, int &out) {
                PropVariantInit(&pv);
                if (SUCCEEDED(qr->GetMetadataByName(name, &pv)) && pv.vt == VT_UI1) out = pv.bVal;
                PropVariantClear(&pv);
            };
            readU2(L"/imgdesc/Left", left);
            readU2(L"/imgdesc/Top", top);
            readU2(L"/grctlext/Delay", delay_cs);
            readU1(L"/grctlext/Disposal", disposal);
        }
        // GIF spec allows a 0 delay ("as fast as possible"); real-world
        // encoders use that to mean "let the viewer pick a sane default"
        // rather than literally 0, so clamp it the same way browsers do.
        if (delay_cs <= 0) delay_cs = 10;

        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                          WICBitmapDitherTypeNone, nullptr, 0.0,
                                          WICBitmapPaletteTypeCustom))) {
            break;
        }
        std::vector<uint8_t> frame_bgra((size_t)fw * fh * 4, 0);
        if (FAILED(converter->CopyPixels(nullptr, (UINT)(fw * 4), (UINT)frame_bgra.size(), frame_bgra.data()))) {
            break;
        }

        if (disposal == 3) prev_snapshot = canvas; // "restore to previous" needs a pre-draw copy

        BlitBgraRect(canvas.data(), canvas_w, canvas_h,
                     frame_bgra.data(), (int)fw, (int)fh,
                     0, 0, (int)fw, (int)fh, left, top);

        GifFrameNative gf;
        gf.bgra = canvas; // this frame's fully-composed appearance
        gf.delay_ms = delay_cs * 10;
        frames.push_back(std::move(gf));

        if (disposal == 2) {
            for (int row = 0; row < (int)fh; ++row) {
                int cy = top + row;
                if (cy < 0 || cy >= canvas_h) continue;
                uint8_t *r = canvas.data() + (size_t)cy * canvas_w * 4;
                for (int col = 0; col < (int)fw; ++col) {
                    int cx = left + col;
                    if (cx < 0 || cx >= canvas_w) continue;
                    uint8_t *px = r + (size_t)cx * 4;
                    px[0] = px[1] = px[2] = px[3] = 0;
                }
            }
        } else if (disposal == 3 && !prev_snapshot.empty()) {
            canvas = prev_snapshot;
        }
    }

    return !frames.empty();
}

static void BlitBgraRect(uint8_t *dst, int dst_w, int dst_h,
                          const uint8_t *src, int src_stride_w, int src_h,
                          int src_x, int src_y, int rect_w, int rect_h,
                          int dst_x, int dst_y)
{
    if (rect_w <= 0 || rect_h <= 0) return;
    for (int row = 0; row < rect_h; ++row) {
        int sy = src_y + row;
        int dy = dst_y + row;
        if (sy < 0 || sy >= src_h || dy < 0 || dy >= dst_h) continue;
        const uint8_t *srow = src + (size_t)sy * src_stride_w * 4;
        uint8_t *drow = dst + (size_t)dy * dst_w * 4;
        for (int col = 0; col < rect_w; ++col) {
            int sx = src_x + col;
            int dx = dst_x + col;
            if (sx < 0 || sx >= src_stride_w || dx < 0 || dx >= dst_w) continue;
            const uint8_t *s = srow + (size_t)sx * 4;
            uint8_t *d = drow + (size_t)dx * 4;
            uint32_t a = s[3];
            if (a == 0) continue;
            if (a == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; continue; }
            uint32_t a1 = 255u - a;
            d[0] = (uint8_t)((s[0] * a + d[0] * a1 + 128u) >> 8u);
            d[1] = (uint8_t)((s[1] * a + d[1] * a1 + 128u) >> 8u);
            d[2] = (uint8_t)((s[2] * a + d[2] * a1 + 128u) >> 8u);
            d[3] = (uint8_t)std::max<uint32_t>(d[3], a);
        }
    }
}

#endif // _WIN32

namespace {
bool HasDynamicToken(const char *text) {
    return std::strstr(text, "{fps}") || std::strstr(text, "{time}") || std::strstr(text, "{date}");
}

std::string ExpandOverlayTextTokens(const char *tmpl, double fps) {
    std::string s(tmpl);
    if (s.find("{fps}") != std::string::npos) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", fps);
        size_t pos = 0;
        while ((pos = s.find("{fps}", pos)) != std::string::npos) {
            s.replace(pos, 5, buf);
            pos += strlen(buf);
        }
    }
    if (s.find("{time}") != std::string::npos || s.find("{date}") != std::string::npos) {
        time_t now = time(nullptr);
        struct tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        char time_buf[16], date_buf[16];
        strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &lt);
        strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &lt);
        size_t pos = 0;
        while ((pos = s.find("{time}", pos)) != std::string::npos) {
            s.replace(pos, 6, time_buf);
            pos += strlen(time_buf);
        }
        pos = 0;
        while ((pos = s.find("{date}", pos)) != std::string::npos) {
            s.replace(pos, 6, date_buf);
            pos += strlen(date_buf);
        }
    }
    return s;
}
} // namespace

const OverlayCompositor::CachedLayer *OverlayCompositor::GetOrRenderText(size_t idx, const HrOverlayDesc &ov) {
#ifdef _WIN32
    bool dynamic = HasDynamicToken(ov.text);
    std::string expanded_text = dynamic ? ExpandOverlayTextTokens(ov.text, current_fps_) : std::string(ov.text);
    const char *text_for_key = expanded_text.c_str();
    auto &snap = snapshots_[idx];
    bool unchanged = snap.valid && std::strcmp(snap.type, "text") == 0 &&
                      snap.w == ov.w && snap.h == ov.h &&
                      snap.text_r == ov.text_r && snap.text_g == ov.text_g &&
                      snap.text_b == ov.text_b &&
                      std::strncmp(snap.text, text_for_key, sizeof(snap.text)) == 0 &&
                      std::strncmp(snap.font_name, ov.font_name, sizeof(snap.font_name)) == 0;
    auto it0 = cache_.find(idx);
    if (unchanged && it0 != cache_.end()) return &it0->second;

    std::string key = std::string("t|") + text_for_key + "|" + std::to_string(ov.w) + "x" + std::to_string(ov.h)
                     + "|" + std::to_string(ov.text_r) + "," + std::to_string(ov.text_g) + "," + std::to_string(ov.text_b)
                     + "|" + ov.font_name;
    auto it = cache_.find(idx);
    if (it != cache_.end() && it->second.key == key) {
        snap.valid = true; _scopy_local(snap.type, sizeof(snap.type), "text");
        snap.w = ov.w; snap.h = ov.h;
        snap.text_r = ov.text_r; snap.text_g = ov.text_g; snap.text_b = ov.text_b;
        _scopy_local(snap.text, sizeof(snap.text), text_for_key);
        _scopy_local(snap.font_name, sizeof(snap.font_name), ov.font_name);
        return &it->second;
    }

    // Widen the UTF-8-ish text buffer (best-effort; overlay text is normally
    // plain ASCII entered through the UI).
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text_for_key, -1, nullptr, 0);
    std::wstring wtext(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 1) MultiByteToWideChar(CP_UTF8, 0, text_for_key, -1, wtext.data(), wlen);

    int flen = MultiByteToWideChar(CP_UTF8, 0, ov.font_name, -1, nullptr, 0);
    std::wstring wfont(flen > 0 ? flen - 1 : 0, L'\0');
    if (flen > 1) MultiByteToWideChar(CP_UTF8, 0, ov.font_name, -1, wfont.data(), flen);

    CachedLayer layer;
    layer.w = ov.w; layer.h = ov.h; layer.key = key;
    if (!RenderTextBgra(wtext, ov.w, ov.h, RGB(ov.text_r, ov.text_g, ov.text_b), wfont, layer.bgra)) return nullptr;
    layer.gen = ++layer_gen_;
    cache_[idx] = std::move(layer);
    snap.valid = true; _scopy_local(snap.type, sizeof(snap.type), "text");
    snap.w = ov.w; snap.h = ov.h;
    snap.text_r = ov.text_r; snap.text_g = ov.text_g; snap.text_b = ov.text_b;
    _scopy_local(snap.text, sizeof(snap.text), text_for_key);
    _scopy_local(snap.font_name, sizeof(snap.font_name), ov.font_name);
    return &cache_[idx];
#else
    (void)idx; (void)ov;
    return nullptr;
#endif
}

const OverlayCompositor::CachedLayer *OverlayCompositor::GetOrRenderImage(size_t idx, const HrOverlayDesc &ov) {
#ifdef _WIN32
    // OPT: same cheap-comparison-first fast path as GetOrRenderText() above.
    auto &snap = snapshots_[idx];
    bool unchanged = snap.valid && std::strcmp(snap.type, "image") == 0 &&
                      snap.w == ov.w && snap.h == ov.h &&
                      std::strncmp(snap.image_path, ov.image_path, sizeof(snap.image_path)) == 0;
    auto it0 = cache_.find(idx);
    if (unchanged && it0 != cache_.end()) return &it0->second;
    ImageSourceCache &src = image_source_cache_[idx];
    bool path_changed = src.path != ov.image_path;
    if (path_changed || (!src.valid && !src.attempted)) {
        src.path = ov.image_path;
        src.valid = false;
        src.attempted = true;

        int wlen = MultiByteToWideChar(CP_UTF8, 0, ov.image_path, -1, nullptr, 0);
        if (wlen > 1) {
            std::wstring wpath(wlen - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, ov.image_path, -1, wpath.data(), wlen);
            if (RenderImageBgraNative(wpath, src.native_bgra, src.native_w, src.native_h)) {
                src.valid = true;
            } else {
                HrLog::Warn(std::string("Overlay: couldn't decode image '") + ov.image_path + "'");
            }
        }
    }
    if (!src.valid) return nullptr;

    std::string key = std::string("i|") + ov.image_path + "|" + std::to_string(ov.w) + "x" + std::to_string(ov.h);
    auto it = cache_.find(idx);
    if (it != cache_.end() && it->second.key == key) {
        snap.valid = true; _scopy_local(snap.type, sizeof(snap.type), "image");
        snap.w = ov.w; snap.h = ov.h;
        _scopy_local(snap.image_path, sizeof(snap.image_path), ov.image_path);
        return &it->second;
    }

    CachedLayer layer;
    layer.w = ov.w; layer.h = ov.h; layer.key = key;
    if (src.native_w == ov.w && src.native_h == ov.h) {
        layer.bgra = src.native_bgra; // already the right size -- no rescale needed
    } else {
        ScaleBgraNearest(src.native_bgra, src.native_w, src.native_h, layer.bgra, ov.w, ov.h);
    }
    layer.gen = ++layer_gen_;
    cache_[idx] = std::move(layer);
    snap.valid = true; _scopy_local(snap.type, sizeof(snap.type), "image");
    snap.w = ov.w; snap.h = ov.h;
    _scopy_local(snap.image_path, sizeof(snap.image_path), ov.image_path);
    return &cache_[idx];
#else
    (void)idx; (void)ov;
    return nullptr;
#endif
}

const OverlayCompositor::CachedLayer *OverlayCompositor::GetOrRenderGif(size_t idx, const HrOverlayDesc &ov) {
#ifdef _WIN32
    GifSourceCache &src = gif_cache_[idx];
    bool path_changed = src.path != ov.image_path;
    if (path_changed || (!src.valid && !src.attempted)) {
        src.path = ov.image_path;
        src.valid = false;
        src.attempted = true;
        src.frames.clear();
        src.total_duration_ms = 0;

        int wlen = MultiByteToWideChar(CP_UTF8, 0, ov.image_path, -1, nullptr, 0);
        if (wlen > 1) {
            std::wstring wpath(wlen - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, ov.image_path, -1, wpath.data(), wlen);

            std::vector<GifFrameNative> decoded;
            int cw = 0, ch = 0;
            if (DecodeGifFramesNative(wpath, decoded, cw, ch)) {
                src.canvas_w = cw; src.canvas_h = ch;
                src.frames.reserve(decoded.size());
                for (auto &f : decoded) {
                    GifFrameCache gc;
                    gc.bgra = std::move(f.bgra);
                    gc.delay_ms = f.delay_ms;
                    src.total_duration_ms += gc.delay_ms;
                    src.frames.push_back(std::move(gc));
                }
                src.valid = !src.frames.empty() && src.total_duration_ms > 0;
                // Anchor playback to "now" so the loop always starts from frame 0 the moment this GIF is
                // (re)loaded, rather than some arbitrary offset if start_time were left at its default-
                // constructed epoch value.
                src.start_time = std::chrono::steady_clock::now();
            }
            if (!src.valid) {
                HrLog::Warn(std::string("Overlay: couldn't decode GIF '") + ov.image_path + "'");
            }
        }
    }
    if (!src.valid || src.frames.empty() || src.total_duration_ms <= 0) return nullptr;

    auto now = std::chrono::steady_clock::now();
    long long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - src.start_time).count();
    long long pos = elapsed_ms % src.total_duration_ms;
    size_t frame_idx = src.frames.size() - 1;
    long long acc = 0;
    for (size_t f = 0; f < src.frames.size(); ++f) {
        acc += src.frames[f].delay_ms;
        if (pos < acc) { frame_idx = f; break; }
    }
    const GifFrameCache &native_frame = src.frames[frame_idx];

    std::string key = std::string("g|") + ov.image_path + "|" + std::to_string(ov.w) + "x" +
                       std::to_string(ov.h) + "|" + std::to_string(frame_idx);
    auto it = cache_.find(idx);
    if (it != cache_.end() && it->second.key == key) return &it->second;

    CachedLayer layer;
    layer.w = ov.w; layer.h = ov.h; layer.key = key;
    if (src.canvas_w == ov.w && src.canvas_h == ov.h) {
        layer.bgra = native_frame.bgra;
    } else {
        ScaleBgraNearest(native_frame.bgra, src.canvas_w, src.canvas_h, layer.bgra, ov.w, ov.h);
    }
    layer.gen = ++layer_gen_;
    cache_[idx] = std::move(layer);
    return &cache_[idx];
#else
    (void)idx; (void)ov;
    return nullptr;
#endif
}

const OverlayCompositor::CachedLayer *OverlayCompositor::GetOrRenderWebcam(size_t idx, const HrOverlayDesc &ov) {
#ifdef _WIN32
    WebcamSourceCache &src = webcam_cache_[idx];
    bool device_changed = src.device_name != ov.webcam_name || src.device_index != ov.webcam_index;
    if (device_changed) {
        delete src.capture;
        src.capture = nullptr;
        src.device_name = ov.webcam_name;
        src.device_index = ov.webcam_index;
        src.have_layer = false;
        src.warned_dead = false;
    }
    if (!src.capture) {
        src.capture = HrWebcamCapture::Open(ov.webcam_name, ov.webcam_index);
    }

    std::vector<uint8_t> frame_bgra;
    int fw = 0, fh = 0;
    bool got_new = src.capture && src.capture->GetLatestFrame(frame_bgra, fw, fh);

    if (!got_new) {
        // Nothing new this tick (usual case -- camera frame rate is normally lower than capture
        // frame rate) or the device never came up at all.
        if (!src.have_layer && src.capture && !src.capture->IsAlive() && !src.warned_dead) {
            src.warned_dead = true;
            HrLog::Warn(std::string("Overlay: couldn't open webcam '") + ov.webcam_name +
                        "' (index " + std::to_string(ov.webcam_index) + ")");
        }
        return src.have_layer ? &src.layer : nullptr;
    }

    src.layer.w = ov.w; src.layer.h = ov.h;
    src.layer.key = std::string("w|") + ov.webcam_name + "|" + std::to_string(ov.webcam_index) +
                     "|" + std::to_string(ov.w) + "x" + std::to_string(ov.h);
    if (fw == ov.w && fh == ov.h) {
        src.layer.bgra = std::move(frame_bgra);
    } else {
        ScaleBgraNearest(frame_bgra, fw, fh, src.layer.bgra, ov.w, ov.h);
    }
    src.layer.gen = ++layer_gen_;
    src.have_layer = true;
    return &src.layer;
#else
    (void)idx; (void)ov;
    return nullptr;
#endif
}

const OverlayCompositor::CachedLayer *OverlayCompositor::CropFromSource(size_t idx, const HrOverlayDesc &ov) {
#ifdef _WIN32
    if (!src_ptr_ || src_w_ <= 0 || src_h_ <= 0 || ov.src_w <= 0 || ov.src_h <= 0 || ov.w <= 0 || ov.h <= 0)
        return nullptr;
    const int cx0 = std::max(ov.src_x, 0), cy0 = std::max(ov.src_y, 0);
    const int cx1 = std::min(ov.src_x + ov.src_w, src_w_), cy1 = std::min(ov.src_y + ov.src_h, src_h_);
    if (cx1 - cx0 <= 0 || cy1 - cy0 <= 0) return nullptr;      // completely off this monitor

    CachedLayer &layer = cache_[idx];
    const size_t need = (size_t)ov.w * ov.h * 4;
    if (layer.w != ov.w || layer.h != ov.h || layer.bgra.size() != need) {
        layer.w = ov.w; layer.h = ov.h;
        layer.bgra.assign(need, 0);
    } else if (cx0 != ov.src_x || cy0 != ov.src_y || cx1 != ov.src_x + ov.src_w || cy1 != ov.src_y + ov.src_h) {
        std::fill(layer.bgra.begin(), layer.bgra.end(), (uint8_t)0);    // partly outside: keep the rest clear
    }
    layer.key = "src";
    layer.gen = ++layer_gen_;

    const double fx = (double)ov.w / ov.src_w, fy = (double)ov.h / ov.src_h;
    int dx0 = (int)((cx0 - ov.src_x) * fx + 0.5), dy0 = (int)((cy0 - ov.src_y) * fy + 0.5);
    int dx1 = (int)((cx1 - ov.src_x) * fx + 0.5), dy1 = (int)((cy1 - ov.src_y) * fy + 0.5);
    dx1 = std::min(dx1, ov.w); dy1 = std::min(dy1, ov.h);
    const int dw = dx1 - dx0, dh = dy1 - dy0;
    if (dw <= 0 || dh <= 0) return nullptr;
    // ScaleRectBox writes a tightly packed dw x dh block: build it in scratch, then place it.
    static thread_local std::vector<uint8_t> blk;
    if (dw == ov.w && dh == ov.h) {
        ScaleRectBox(src_ptr_, (size_t)src_w_ * 4, cx0, cy0, cx1 - cx0, cy1 - cy0, layer.bgra.data(), dw, dh);
    } else {
        blk.resize((size_t)dw * dh * 4);
        ScaleRectBox(src_ptr_, (size_t)src_w_ * 4, cx0, cy0, cx1 - cx0, cy1 - cy0, blk.data(), dw, dh);
        for (int y = 0; y < dh; ++y)
            std::memcpy(layer.bgra.data() + ((size_t)(dy0 + y) * ov.w + dx0) * 4, blk.data() + (size_t)y * dw * 4,
                        (size_t)dw * 4);
    }
    return &layer;
#else
    (void)idx; (void)ov;
    return nullptr;
#endif
}

const OverlayCompositor::CachedLayer *OverlayCompositor::GetOrRenderRegion(size_t idx, const HrOverlayDesc &ov) {
    return CropFromSource(idx, ov);
}

OverlayCompositor::WindowSourceCache::~WindowSourceCache() {
#ifdef _WIN32
    if (handle) HrWgcDestroy(handle);
#endif
}

const OverlayCompositor::CachedLayer *OverlayCompositor::GetOrRenderWindow(size_t idx, const HrOverlayDesc &ov) {
#ifdef _WIN32
    if (ov.win_method == 1 && ov.hwnd != 0) {
        WindowSourceCache &src = window_cache_[idx];
        const auto now = std::chrono::steady_clock::now();
        if (src.handle && src.hwnd != ov.hwnd) {
            // The program opened a new window: re-aim the source (PrintWindow backend can), else rebuild.
            if (HrWgcSetTarget(src.handle, (HWND)(intptr_t)ov.hwnd)) {
                src.hwnd = ov.hwnd;
            } else {
                HrWgcDestroy(src.handle);
                src.handle = nullptr;
                src.have_layer = false;
            }
        }
        if (!src.handle && now >= src.retry_at) {
            int ww = 0, wh = 0;
            if (HrWgcQueryWindowSize((HWND)(intptr_t)ov.hwnd, ov.win_client_only != 0, &ww, &wh) && ww > 1 && wh > 1) {
                // Smart mode is off on purpose: it would open a second screen duplication next to the
                // recording's own one. The cheap cut-out of the monitor picture is the "screen" method.
                HrWgcOptions opt;
                opt.smart = false;
                opt.fps_cap = ov.win_fps_cap;
                src.canvas_w = ww & ~1; src.canvas_h = wh & ~1;
                src.handle = HrWgcCreate((HWND)(intptr_t)ov.hwnd, src.canvas_w, src.canvas_h,
                                         ov.win_client_only != 0, /*draw_cursor=*/false, &opt);
                if (src.handle) { src.hwnd = ov.hwnd; src.warned = false; }
            }
            if (!src.handle) {
                src.retry_at = now + std::chrono::seconds(2);
                if (!src.warned) {
                    src.warned = true;
                    HrLog::Warn("Window layer: the window itself can't be captured right now - cutting it out of "
                                "the screen picture instead.");
                }
            }
        }
        if (src.handle) {
            src.frame.resize((size_t)src.canvas_w * src.canvas_h * 4);
            const int rc = HrWgcCapture(src.handle, src.frame.data(), 0);
            if (rc == 0) {
                src.layer.w = ov.w; src.layer.h = ov.h;
                src.layer.key = "win";
                src.layer.bgra.assign((size_t)ov.w * ov.h * 4, 0);
                ScaleRectBox(src.frame.data(), (size_t)src.canvas_w * 4, 0, 0, src.canvas_w, src.canvas_h,
                             src.layer.bgra.data(), ov.w, ov.h);
                src.layer.gen = ++layer_gen_;
                src.have_layer = true;
            } else if (rc == 2) {
                HrWgcReset(src.handle);
            } else if (rc < 0) {
                HrWgcDestroy(src.handle);
                src.handle = nullptr;
                src.retry_at = now + std::chrono::seconds(2);
            }
            if (src.have_layer && (src.layer.w != ov.w || src.layer.h != ov.h)) {
                // the layer was resized without a new window frame: rescale what is held
                src.layer.bgra.assign((size_t)ov.w * ov.h * 4, 0);
                ScaleRectBox(src.frame.data(), (size_t)src.canvas_w * 4, 0, 0, src.canvas_w, src.canvas_h,
                             src.layer.bgra.data(), ov.w, ov.h);
                src.layer.w = ov.w; src.layer.h = ov.h;
                src.layer.gen = ++layer_gen_;
            }
            if (src.have_layer) return &src.layer;
        }
    } else {
        window_cache_.erase(idx);     // switched back to the screen method: free the window source
    }
    return CropFromSource(idx, ov);   // method "screen" (or the exact capture is unavailable)
#else
    (void)idx; (void)ov;
    return nullptr;
#endif
}

const OverlayCompositor::CachedLayer *OverlayCompositor::GetOrRenderInputOverlay(size_t idx, const HrOverlayDesc &ov) {
#ifdef _WIN32
    InputOverlayCache &c = input_cache_[idx];
    bool paths_changed = c.json_path != ov.input_json_path || c.png_path != ov.input_png_path;
    if (paths_changed || (!c.valid && !c.attempted)) {
        c.json_path = ov.input_json_path;
        c.png_path  = ov.input_png_path;
        c.valid = false;
        c.attempted = true;

        int wlen = MultiByteToWideChar(CP_UTF8, 0, ov.input_png_path, -1, nullptr, 0);
        if (wlen <= 1) return nullptr;
        std::wstring wpng(wlen - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, ov.input_png_path, -1, wpng.data(), wlen);

        if (!RenderImageBgraNative(wpng, c.sheet_bgra, c.sheet_w, c.sheet_h)) {
            HrLog::Warn(std::string("Input overlay: couldn't decode spritesheet '") + ov.input_png_path + "'");
            return nullptr;
        }
        if (!c.layout.Load(ov.input_json_path)) {
            HrLog::Warn(std::string("Input overlay: couldn't parse layout '") + ov.input_json_path + "'");
            return nullptr;
        }
        // Sanity-clamp the parsed canvas size before it's ever used to size an allocation.
        constexpr int kMaxOverlayCanvasDim = 8192;
        if (c.layout.width <= 0 || c.layout.height <= 0 ||
            c.layout.width > kMaxOverlayCanvasDim || c.layout.height > kMaxOverlayCanvasDim) {
            HrLog::Warn(std::string("Input overlay: layout '") + ov.input_json_path +
                        "' has an invalid or unreasonable canvas size -- ignoring it.");
            c.valid = false;
            return nullptr;
        }
        c.valid = true;

        // Resolve every element's virtual-key once per (re)load instead of
        // once per element per frame.
        c.vks.assign(c.layout.elements.size(), 0u);
        for (size_t k = 0; k < c.layout.elements.size(); ++k) {
            const auto &el = c.layout.elements[k];
            if (el.scan_code < 0) continue;
            if (el.type == 1) {
                UINT raw = (UINT)el.scan_code;
                UINT vsc = (raw & 0x80) ? (0xE000u | (raw & 0x7Fu)) : (raw & 0x7Fu);
                c.vks[k] = MapVirtualKeyW(vsc, MAPVK_VSC_TO_VK_EX);
            } else if (el.type == 3 || el.type == 4) {
                c.vks[k] = (UINT)el.scan_code;
            }
        }
        c.last_pressed.assign(c.layout.elements.size(), 0);
        c.layer_valid = false;
    }
    if (!c.valid || c.layout.width <= 0 || c.layout.height <= 0) return nullptr;

    // The layout's element coordinates (map_x/map_y/pos_x/pos_y/etc.) are only meaningful at
    // the layout's own native width/height, so the buttons are always composited at that
    // native size first.
    const int native_w = c.layout.width, native_h = c.layout.height;

    // Poll the live key state (one GetAsyncKeyState per key-driven element).
    const size_t n_el = c.layout.elements.size();
    if (c.vks.size() != n_el) c.vks.assign(n_el, 0u);
    if (c.last_pressed.size() != n_el) { c.last_pressed.assign(n_el, 0); c.layer_valid = false; }
    std::vector<uint8_t> &pressed_now = c.pressed_scratch;
    pressed_now.assign(n_el, 0);
    bool changed = !c.layer_valid;
    for (size_t k = 0; k < n_el; ++k) {
        const unsigned vk = c.vks[k];
        const bool pressed = vk != 0 && (GetAsyncKeyState((int)vk) & 0x8000) != 0;
        pressed_now[k] = pressed ? 1 : 0;
        if (pressed_now[k] != c.last_pressed[k]) changed = true;
    }
    if (c.last_ov_w != ov.w || c.last_ov_h != ov.h) changed = true;

    // Unchanged since the last frame (nobody pressed or released anything, box not resized)?
    if (!changed) {
        auto it = cache_.find(idx);
        if (it != cache_.end() && it->second.key == c.layer_key) return &it->second;
    }

    // Rebuilt only when something actually changed (see above).
    std::vector<uint8_t> &native_bgra = c.native_scratch;
    native_bgra.resize((size_t)native_w * native_h * 4);
    std::fill(native_bgra.begin(), native_bgra.end(), 0);

    for (size_t k = 0; k < n_el; ++k) {
        const auto &el = c.layout.elements[k];
        int sx = el.map_x, sy = el.map_y;
        if (pressed_now[k]) {
            // Convention (matches the bundled univrsal/input-overlay presets): the "pressed" frame for
            // a button sits directly below its "released" frame in the spritesheet, offset by the
            // element's own height plus the layout's space_v gap.
            int pressed_y = el.map_y + el.map_h + c.layout.space_v;
            if (pressed_y + el.map_h <= c.sheet_h) sy = pressed_y;
        }
        BlitBgraRect(native_bgra.data(), native_w, native_h,
                     c.sheet_bgra.data(), c.sheet_w, c.sheet_h,
                     sx, sy, el.map_w, el.map_h,
                     el.pos_x, el.pos_y);
    }

    CachedLayer &layer = cache_[idx];
    c.layer_key = std::string("io|") + ov.input_json_path;
    layer.key = c.layer_key;
    if (ov.w == native_w && ov.h == native_h) {
        // Copy (not move) -- native_bgra aliases c.native_scratch, which
        // has to survive intact to be reused next time.
        layer.bgra = native_bgra;
        layer.w = native_w; layer.h = native_h;
    } else {
        ScaleBgraNearest(native_bgra, native_w, native_h, layer.bgra, ov.w, ov.h);
        layer.w = ov.w; layer.h = ov.h;
    }
    c.last_pressed = pressed_now;
    c.last_ov_w = ov.w; c.last_ov_h = ov.h;
    c.layer_valid = true;
    layer.gen = ++layer_gen_;

    return &cache_[idx];
#else
    (void)idx; (void)ov;
    return nullptr;
#endif
}

OverlayCompositor::WebcamSourceCache::~WebcamSourceCache() {
    delete capture;
}

void OverlayCompositor::PruneStaleCaches(size_t overlay_count) {    auto prune = [overlay_count](auto &map) {
        for (auto it = map.begin(); it != map.end(); ) {
            if (it->first >= overlay_count) it = map.erase(it);
            else ++it;
        }
    };
    prune(snapshots_);
    prune(cache_);
    prune(input_cache_);
    prune(image_source_cache_);
    prune(gif_cache_);
    prune(webcam_cache_);
    prune(window_cache_);
}

void OverlayCompositor::ForEachLayer(const std::vector<HrOverlayDesc> &overlays, double current_fps,
        const std::function<void(const CachedLayer &, const HrOverlayDesc &)> &draw)
{
    current_fps_ = current_fps;

    PruneStaleCaches(overlays.size());

    for (size_t i = 0; i < overlays.size(); ++i) {
        const HrOverlayDesc &ov = overlays[i];
        // Hidden, zero-sized or fully faded-out (opacity 0) overlays draw nothing - don't render
        // them either.
        if (!HrOverlayDrawsAnything(ov)) {
            if (std::strcmp(ov.type, "webcam") == 0) {
                webcam_cache_.erase(i);   // closes the camera while the overlay is invisible
            }
            if (std::strcmp(ov.type, "window") == 0) {
                window_cache_.erase(i);   // stops capturing the window while its layer is hidden
            }
            continue;
        }
        try {
            const CachedLayer *layer = nullptr;
            if (std::strcmp(ov.type, "text") == 0) {
                layer = GetOrRenderText(i, ov);
            } else if (std::strcmp(ov.type, "image") == 0) {
                layer = GetOrRenderImage(i, ov);
            } else if (std::strcmp(ov.type, "gif") == 0) {
                layer = GetOrRenderGif(i, ov);
            } else if (std::strcmp(ov.type, "input_overlay") == 0) {
                layer = GetOrRenderInputOverlay(i, ov);
            } else if (std::strcmp(ov.type, "webcam") == 0) {
                layer = GetOrRenderWebcam(i, ov);
            } else if (std::strcmp(ov.type, "region") == 0) {
                layer = GetOrRenderRegion(i, ov);
            } else if (std::strcmp(ov.type, "window") == 0) {
                layer = GetOrRenderWindow(i, ov);
            } else {
                continue;
            }

            if (layer) draw(*layer, ov);
        } catch (const std::exception &e) {
            static bool warned_exc = false;
            if (!warned_exc) {
                HrLog::Error(std::string("Overlay: rendering overlay #") + std::to_string(i) +
                             " threw (" + e.what() + ") -- skipping it for this frame instead of "
                             "crashing the recording.");
                warned_exc = true;
            }
        } catch (...) {
            static bool warned_exc2 = false;
            if (!warned_exc2) {
                HrLog::Error(std::string("Overlay: rendering overlay #") + std::to_string(i) +
                             " threw an unknown exception -- skipping it for this frame instead of "
                             "crashing the recording.");
                warned_exc2 = true;
            }
        }
    }
}

void OverlayCompositor::Apply(uint8_t *base_bgra, int base_w, int base_h, int base_stride,
                               const std::vector<HrOverlayDesc> &overlays, double current_fps,
                               const uint8_t *source_bgra)
{
    if (!base_bgra || base_w <= 0 || base_h <= 0) return;

    bool need_source = false, hide_desktop = false;
    for (const auto &ov : overlays) {
        if (!HrOverlayDrawsAnything(ov)) continue;
        if (std::strcmp(ov.type, "region") == 0 || std::strcmp(ov.type, "window") == 0) need_source = true;
        else if (std::strcmp(ov.type, "desktop_hidden") == 0) hide_desktop = true;
    }
    src_ptr_ = nullptr; src_w_ = src_h_ = 0;
    if (source_bgra == base_bgra) source_bgra = nullptr;      // same buffer: it is about to be changed
    if (need_source) {
        if (source_bgra && base_stride == base_w * 4) {
            src_ptr_ = source_bgra;
        } else {
            src_copy_.resize((size_t)base_w * base_h * 4);
            for (int y = 0; y < base_h; ++y)
                std::memcpy(src_copy_.data() + (size_t)y * base_w * 4, base_bgra + (size_t)y * base_stride,
                            (size_t)base_w * 4);
            src_ptr_ = src_copy_.data();
        }
        src_w_ = base_w; src_h_ = base_h;
    }
    if (hide_desktop) {
        for (int y = 0; y < base_h; ++y) {
            uint32_t *row = reinterpret_cast<uint32_t *>(base_bgra + (size_t)y * base_stride);
            std::fill(row, row + base_w, 0xFF000000u);            // opaque black (B,G,R = 0)
        }
    }
    ForEachLayer(overlays, current_fps, [&](const CachedLayer &layer, const HrOverlayDesc &ov) {
        CompositeBgra(base_bgra, base_w, base_h, base_stride,
                      layer.bgra.data(), layer.w, layer.h, ov.x, ov.y, ov.opacity);
    });
}

void OverlayCompositor::ApplyNv12(uint8_t *nv12, int w, int h,
                                   const std::vector<HrOverlayDesc> &overlays, double current_fps)
{
    if (!nv12 || w < 2 || h < 2) return;
    ForEachLayer(overlays, current_fps, [&](const CachedLayer &layer, const HrOverlayDesc &ov) {
        CompositeBgraNv12(nv12, w, h, layer.bgra.data(), layer.w, layer.h, ov.x, ov.y, ov.opacity);
    });
}

void OverlayCompositor::CollectGpuLayers(const std::vector<HrOverlayDesc> &overlays, double current_fps,
                                         std::vector<HrGpuLayer> &out)
{
    out.clear();
    ForEachLayer(overlays, current_fps, [&](const CachedLayer &layer, const HrOverlayDesc &ov) {
        if (layer.bgra.empty() || layer.w <= 0 || layer.h <= 0 ||
            layer.bgra.size() < (size_t)layer.w * (size_t)layer.h * 4) return;
        HrGpuLayer g;
        g.key     = (int)(&ov - overlays.data());   // ov is an element of `overlays`
        g.bgra    = layer.bgra.data();
        g.w       = layer.w;  g.h = layer.h;
        g.gen     = layer.gen;
        g.dst_x   = ov.x;     g.dst_y = ov.y;
        g.dst_w   = layer.w;  g.dst_h = layer.h;
        g.opacity = ov.opacity;
        out.push_back(g);
    });
}
