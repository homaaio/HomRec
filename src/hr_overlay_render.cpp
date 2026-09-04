#include "hr_overlay_render.h"
#include "hr_log.h"

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
#include <exception>

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

// ---------------------------------------------------------------------------
// Composite: alpha-blend an RGBA/BGRA overlay buffer onto a BGRA base buffer
// at (dst_x, dst_y), clipping to the base's bounds. Row-pitch aware (unlike
// homrec_core.c's hr_blend_rgba, which assumes a 3-byte-per-pixel base with
// no stride/position -- fine for a full-frame effect, not for a positioned
// rectangular overlay inside a larger capture frame, which is what this is).
// ---------------------------------------------------------------------------
static void CompositeBgra(uint8_t *base, int base_w, int base_h, int base_stride,
                           const uint8_t *overlay, int ov_w, int ov_h,
                           int dst_x, int dst_y)
{
    if (!base || !overlay || ov_w <= 0 || ov_h <= 0) return;

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

#ifdef _WIN32

// ---------------------------------------------------------------------------
// Nearest-neighbor resample of a BGRA buffer, alpha included. Used to scale
// an input-overlay's composited spritesheet (drawn at the layout JSON's
// native size) to whatever size the user dragged the overlay's box to on
// the preview -- without this, resizing an input overlay has no visible
// effect since the layout's element coordinates are only meaningful at
// their native resolution.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Text: GDI can't draw anti-aliased text with a real alpha channel directly,
// so use the standard trick -- draw white text on a black 32bpp DIB, then
// treat the resulting (R==G==B) luminance as coverage/alpha and re-tint to
// the actual desired colour. Produces properly anti-aliased text that blends
// cleanly over whatever's under it.
// ---------------------------------------------------------------------------
static bool RenderTextBgra(const std::wstring &text, int w, int h, COLORREF color,
                            std::vector<uint8_t> &out)
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
    HFONT font = CreateFontW(-pointSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
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

// ---------------------------------------------------------------------------
// Image: decode via WIC (built into Windows -- handles PNG/JPG/BMP/GIF/etc.
// without pulling in a third-party image library) straight to a
// premultiplied-alpha-free 32bpp BGRA buffer, scaled to the overlay's w x h.
// ---------------------------------------------------------------------------
// Image, native resolution -- decodes straight to a 32bpp BGRA buffer with
// no resize step. Used both for input-overlay spritesheets (whose source
// pixel rects in the layout JSON are only meaningful against the image's
// actual size) and for regular image overlays (GetOrRenderImage() rescales
// from this cached native decode to whatever size the overlay is currently
// configured at -- see ImageSourceCache's comment in hr_overlay_render.h
// for why decode and resize are kept separate/independently cached).
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// GIF: decodes every frame via WIC, compositing each one onto a persistent
// canvas the size of the GIF's logical screen (its "/logscrdesc/..." global
// metadata), resolving each frame's disposal method (leave in place / clear
// to background / restore the previous frame) as it goes, so what comes out
// is a plain list of already-fully-composed canvas-sized BGRA frames, each
// tagged with its own delay. That keeps playback (GetOrRenderGif() below)
// as simple as "pick a frame index and scale it", the same as a static
// image overlay, with no disposal-method bookkeeping needed at render time.
// ---------------------------------------------------------------------------
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

    // The overall canvas size lives in the GIF's logical screen descriptor,
    // which WIC only exposes through frame 0's metadata reader (not a
    // decoder-level one). Fall back to frame 0's own pixel size if that
    // read fails for any reason, so a plain single-frame/malformed-header
    // GIF still decodes instead of being rejected outright.
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

        // Resolve this frame's disposal *after* capturing its composed
        // image above, so the canvas is ready for the next frame to draw
        // onto (disposal 0/"unspecified" and 1/"do not dispose" both just
        // leave the canvas as-is, so there's no branch needed for them).
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

// Alpha-blends a single sub-rect of one BGRA buffer onto another at
// (dst_x, dst_y), clipping to the destination's bounds -- the input-overlay
// equivalent of CompositeBgra() above, but copying FROM an arbitrary
// (src_x, src_y, src_w, src_h) window instead of the whole source image.
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

const OverlayCompositor::CachedLayer *OverlayCompositor::GetOrRenderText(size_t idx, const HrOverlayDesc &ov) {
#ifdef _WIN32
    // OPT: cheap field comparison first -- avoids building/allocating the
    // key string (several std::to_string() calls + concatenation) on every
    // frame for the common case where a static text overlay hasn't
    // changed since last frame. See TextImageSnapshot's comment in the
    // header for why this exists.
    auto &snap = snapshots_[idx];
    bool unchanged = snap.valid && std::strcmp(snap.type, "text") == 0 &&
                      snap.w == ov.w && snap.h == ov.h &&
                      snap.text_r == ov.text_r && snap.text_g == ov.text_g &&
                      snap.text_b == ov.text_b &&
                      std::strncmp(snap.text, ov.text, sizeof(snap.text)) == 0;
    auto it0 = cache_.find(idx);
    if (unchanged && it0 != cache_.end()) return &it0->second;

    std::string key = std::string("t|") + ov.text + "|" + std::to_string(ov.w) + "x" + std::to_string(ov.h)
                     + "|" + std::to_string(ov.text_r) + "," + std::to_string(ov.text_g) + "," + std::to_string(ov.text_b);
    auto it = cache_.find(idx);
    if (it != cache_.end() && it->second.key == key) {
        snap.valid = true; _scopy_local(snap.type, sizeof(snap.type), "text");
        snap.w = ov.w; snap.h = ov.h;
        snap.text_r = ov.text_r; snap.text_g = ov.text_g; snap.text_b = ov.text_b;
        _scopy_local(snap.text, sizeof(snap.text), ov.text);
        return &it->second;
    }

    // Widen the UTF-8-ish text buffer (best-effort; overlay text is normally
    // plain ASCII entered through the UI).
    int wlen = MultiByteToWideChar(CP_UTF8, 0, ov.text, -1, nullptr, 0);
    std::wstring wtext(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 1) MultiByteToWideChar(CP_UTF8, 0, ov.text, -1, wtext.data(), wlen);

    CachedLayer layer;
    layer.w = ov.w; layer.h = ov.h; layer.key = key;
    if (!RenderTextBgra(wtext, ov.w, ov.h, RGB(ov.text_r, ov.text_g, ov.text_b), layer.bgra)) return nullptr;
    cache_[idx] = std::move(layer);
    snap.valid = true; _scopy_local(snap.type, sizeof(snap.type), "text");
    snap.w = ov.w; snap.h = ov.h;
    snap.text_r = ov.text_r; snap.text_g = ov.text_g; snap.text_b = ov.text_b;
    _scopy_local(snap.text, sizeof(snap.text), ov.text);
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
                // Anchor playback to "now" so the loop always starts from
                // frame 0 the moment this GIF is (re)loaded, rather than
                // some arbitrary offset if start_time were left at its
                // default-constructed epoch value.
                src.start_time = std::chrono::steady_clock::now();
            }
            if (!src.valid) {
                HrLog::Warn(std::string("Overlay: couldn't decode GIF '") + ov.image_path + "'");
            }
        }
    }
    if (!src.valid || src.frames.empty() || src.total_duration_ms <= 0) return nullptr;

    // Pick whichever frame is due right now, based on wall-clock time since
    // this overlay's GIF was (re)loaded -- one shared clock per overlay
    // instance, not per Apply() call, so playback speed matches the file's
    // authored frame delays regardless of how often frames get composited.
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
        // Nothing new this tick (usual case -- camera frame rate is
        // normally lower than capture frame rate) or the device never
        // came up at all. Either way, keep showing the last good frame if
        // there is one instead of vanishing between camera frames.
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
    src.have_layer = true;
    return &src.layer;
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
        // Sanity-clamp the parsed canvas size before it's ever used to size
        // an allocation. A hand-edited or corrupt layout file (this preset
        // format is plain user-editable JSON, not something HomRec
        // generates itself) could otherwise hand us a huge or negative
        // width/height, which would either throw (std::bad_alloc, on a
        // std::thread with no exception handler around it -- see the
        // try/catch note in Apply() below for why that's worth avoiding on
        // its own) or, if it didn't throw, ask for a multi-gigabyte
        // allocation for something that's only ever a few hundred pixels
        // across in every real preset.
        constexpr int kMaxOverlayCanvasDim = 8192;
        if (c.layout.width <= 0 || c.layout.height <= 0 ||
            c.layout.width > kMaxOverlayCanvasDim || c.layout.height > kMaxOverlayCanvasDim) {
            HrLog::Warn(std::string("Input overlay: layout '") + ov.input_json_path +
                        "' has an invalid or unreasonable canvas size -- ignoring it.");
            c.valid = false;
            return nullptr;
        }
        c.valid = true;
    }
    if (!c.valid || c.layout.width <= 0 || c.layout.height <= 0) return nullptr;

    // Rebuilt every call (not staleness-checked like the other overlay
    // types' `cache_` entries) -- an input overlay's whole purpose is
    // reflecting live key state, so "unchanged since last frame" isn't a
    // condition that's meaningfully true here.
    //
    // The layout's element coordinates (map_x/map_y/pos_x/pos_y/etc.) are
    // only meaningful at the layout's own native width/height, so the
    // buttons are always composited at that native size first. The result
    // is then scaled to the overlay's configured w/h -- ov.w/ov.h are what
    // the user actually dragged the overlay box to on the preview, and
    // without this scaling step the overlay would render at a fixed size
    // no matter how it's resized.
    int native_w = c.layout.width, native_h = c.layout.height;

    std::vector<uint8_t> &native_bgra = c.native_scratch;
    native_bgra.resize((size_t)native_w * native_h * 4);
    std::fill(native_bgra.begin(), native_bgra.end(), 0);

    for (const auto &el : c.layout.elements) {
        int sx = el.map_x, sy = el.map_y;
        bool pressed = false;
        if (el.type == 1 && el.scan_code >= 0) {
            UINT raw = (UINT)el.scan_code;
            UINT vsc = (raw & 0x80) ? (0xE000u | (raw & 0x7Fu)) : (raw & 0x7Fu);
            UINT vk = MapVirtualKeyW(vsc, MAPVK_VSC_TO_VK_EX);
            pressed = vk != 0 && (GetAsyncKeyState((int)vk) & 0x8000) != 0;
        } else if ((el.type == 3 || el.type == 4) && el.scan_code >= 0) {
            pressed = (GetAsyncKeyState(el.scan_code) & 0x8000) != 0;
        }
        if (pressed) {
            // Convention (matches the bundled univrsal/input-overlay
            // presets): the "pressed" frame for a button sits directly
            // below its "released" frame in the spritesheet, offset by
            // the element's own height plus the layout's space_v gap.
            int pressed_y = el.map_y + el.map_h + c.layout.space_v;
            if (pressed_y + el.map_h <= c.sheet_h) sy = pressed_y;
        }
        BlitBgraRect(native_bgra.data(), native_w, native_h,
                     c.sheet_bgra.data(), c.sheet_w, c.sheet_h,
                     sx, sy, el.map_w, el.map_h,
                     el.pos_x, el.pos_y);
    }

    CachedLayer &layer = cache_[idx];
    layer.key = std::string("io|") + ov.input_json_path;
    if (ov.w == native_w && ov.h == native_h) {
        // Copy (not move) -- native_bgra aliases c.native_scratch, which
        // has to survive intact to be reused next frame.
        layer.bgra = native_bgra;
        layer.w = native_w; layer.h = native_h;
    } else {
        ScaleBgraNearest(native_bgra, native_w, native_h, layer.bgra, ov.w, ov.h);
        layer.w = ov.w; layer.h = ov.h;
    }

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
}

void OverlayCompositor::Apply(uint8_t *base_bgra, int base_w, int base_h, int base_stride,
                               const std::vector<HrOverlayDesc> &overlays)
{
    if (!base_bgra || base_w <= 0 || base_h <= 0) return;

    PruneStaleCaches(overlays.size());

    for (size_t i = 0; i < overlays.size(); ++i) {
        const HrOverlayDesc &ov = overlays[i];
        if (!ov.visible || ov.w <= 0 || ov.h <= 0) continue;
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
            } else {
                continue;
            }

            if (layer) {
                CompositeBgra(base_bgra, base_w, base_h, base_stride,
                              layer->bgra.data(), layer->w, layer->h, ov.x, ov.y);
            }
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
