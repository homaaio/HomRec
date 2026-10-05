#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <d3d11.h>
  #include <dxgi1_2.h>
  #include <wrl/client.h>   /* ComPtr */
  #if defined(_MSC_VER)
    // MinGW/GCC ignores #pragma comment(lib, ...) with a warning; it links
    // via -ld3d11 -ldxgi on the command line instead (see the Makefile).
    #pragma comment(lib, "d3d11.lib")
    #pragma comment(lib, "dxgi.lib")
  #endif

  using Microsoft::WRL::ComPtr;
#endif

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <vector>

#ifdef _WIN32
  #define HR_EXPORT extern "C" __declspec(dllexport)
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

/* Return codes from hr_dx_capture */
static constexpr int HR_DX_OK      =  0;
static constexpr int HR_DX_TIMEOUT =  1;
static constexpr int HR_DX_LOST    =  2;
static constexpr int HR_DX_ERROR   = -1;

#ifdef _WIN32
static HRESULT g_last_dx_error = S_OK;
#endif

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */
#ifdef _WIN32
/* Axis-aligned rectangle [l,r) x [t,b) used for dirty-region bookkeeping.
 * An empty box means "nothing". Unions are always the bounding box of the
 * two inputs - deliberately coarse: one bounding box per frame is enough to
 * turn "copy the whole 8MB desktop" into "copy the few rows that changed"
 * for the typical idle-desktop cases (blinking text caret, a ticking clock,
 * typing) without any per-rect bookkeeping. */
struct DxBox {
    int l = 0, t = 0, r = 0, b = 0;

    bool empty() const { return r <= l || b <= t; }
    void clear() { l = t = r = b = 0; }
    void set_full(int w, int h) { l = 0; t = 0; r = w; b = h; }
    bool is_full(int w, int h) const { return l <= 0 && t <= 0 && r >= w && b >= h; }

    void add(int l2, int t2, int r2, int b2) {
        if (r2 <= l2 || b2 <= t2) return;
        if (empty()) { l = l2; t = t2; r = r2; b = b2; return; }
        l = std::min(l, l2); t = std::min(t, t2);
        r = std::max(r, r2); b = std::max(b, b2);
    }
    void add(const DxBox &o) { add(o.l, o.t, o.r, o.b); }

    void clamp(int w, int h) {
        l = std::max(l, 0); t = std::max(t, 0);
        r = std::min(r, w); b = std::min(b, h);
        if (empty()) clear();
    }
};

/* Every this-many *changed* frames one full-frame copy is forced. Dirty
 * rectangles come straight from the driver; a driver that under-reports
 * them would otherwise leave a stale patch on screen indefinitely. This
 * bounds any such artifact to ~2s at 60fps and costs one full copy per
 * ~120 changed frames. */
static constexpr int kFullRefreshEvery = 120;

/* GPU scheduler priority of the capture D3D11 device, -7..7 (0 = normal).
 * See hr_dx_create() for why this is no longer 7. */
static constexpr int kCaptureGpuPriority = 0;

struct DxCapCtx {
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;
    ComPtr<IDXGIOutputDuplication> duplication;

    /* ---- CPU readback ring (BGRA) ------------------------------------
     * Two staging textures used ping-pong: frame N is copied into one
     * while frame N-1 (already finished on the GPU) is read out of the
     * other, so Map() never has to wait for the copy it just queued. */
    ComPtr<ID3D11Texture2D> staging[2];
    /* Per staging texture: the region that may differ from the *latest*
     * desktop image (union of every changed-rect since that texture was
     * last brought up to date). A new frame only has to copy this region
     * into the texture instead of the whole desktop. */
    DxBox stale[2];
    /* Per staging texture: the region that changed between the frame
     * before it and the frame it currently holds. */
    DxBox slot_box[2];
    int  write_idx    = 0;
    int  pending_idx  = -1;
    bool have_pending = false;

    /* Region of the CALLER's BGRA buffer that is known to lag behind the
     * frame we're about to hand out (frames that were dropped, or consumed
     * by the GPU path instead). Together with slot_box it is exactly what
     * has to be memcpy'd into the caller's buffer. */
    DxBox out_carry;
    const uint8_t *last_out = nullptr;  /* buffer the last delivery went to      */
    bool out_valid   = false;           /* caller's buffer holds a coherent frame */
    bool last_partial = false;          /* last HR_DX_OK only touched some rows   */
    int  last_dirty_y0 = 0, last_dirty_y1 = 0;

    bool have_image  = false;           /* at least one desktop image seen        */
    bool rotated     = false;           /* rotated output: always copy everything */
    int  frames_since_full = 0;
    std::vector<uint8_t> meta;          /* scratch for dirty/move rect metadata   */

    uint64_t st_acquired = 0;           /* frames DXGI handed us                  */
    uint64_t st_skipped  = 0;           /* ...of which had no new desktop image   */
    uint64_t st_partial  = 0;           /* deliveries that copied a sub-region    */
    uint64_t st_full     = 0;           /* deliveries that copied everything      */

    /* ---- GPU path: D3D11 Video Processor, BGRA -> NV12 (+crop/scale) ----
     * desk_tex is a persistent GPU-side copy of the latest desktop image
     * (DXGI only hands out an image when something changes, so anything
     * that wants to re-render a static desktop - mode switches, a moved
     * crop rectangle - needs its own copy). Only maintained while
     * gpu_enabled. */
    bool gpu_enabled = false;
    bool vp_ready    = false;
    int  nv_w = 0, nv_h = 0;            /* NV12 output size (even)                */
    ComPtr<ID3D11Texture2D> desk_tex;
    bool desk_valid = false;
    ComPtr<ID3D11VideoDevice>                 vdev;
    ComPtr<ID3D11VideoContext>                vctx;
    ComPtr<ID3D11VideoProcessorEnumerator>    venum;
    ComPtr<ID3D11VideoProcessor>              vproc;
    ComPtr<ID3D11VideoProcessorInputView>     vin;
    ComPtr<ID3D11Texture2D>                   nv12_tex;
    ComPtr<ID3D11VideoProcessorOutputView>    vout;
    ComPtr<ID3D11Texture2D>                   nv_stage[2];
    int  nv_write = 0, nv_pending = -1;
    bool nv_have_pending = false;
    bool nv_out_valid    = false;       /* caller's NV12 buffer holds a coherent frame */
    bool nv_rect_valid   = false;
    RECT nv_last_rect{0, 0, 0, 0};
    uint64_t st_gpu_frames = 0;

    int  adapter_idx;
    int  output_idx;
    int  width;
    int  height;
    bool acquired;

    // Fixed output shape the CALLER's buffer (and, downstream, ffmpeg's
    // rawvideo pipe) was sized for -- set once via hr_dx_set_output_size()
    // right after the caller allocates its buffer, and deliberately never
    // touched by reset(). width/height above track whatever DXGI's output
    // duplication is ACTUALLY handing back right now, which can change
    // out from under us mid-recording (see hr_dx_capture()'s blit comment
    // below). 0/0 means "not set yet -- behave exactly as before and just
    // use width/height", so nothing changes for any caller that never
    // calls hr_dx_set_output_size().
    int  out_w = 0;
    int  out_h = 0;

    DxCapCtx() : adapter_idx(0), output_idx(0), width(0), height(0), acquired(false) {
        nv_last_rect = RECT{0, 0, 0, 0};
    }

    /* Forget everything we believed about "what the caller's buffers hold
     * and what the staging textures hold" - used by reset(). */
    void reset_tracking() {
        have_image = false;
        for (int i = 0; i < 2; ++i) { stale[i].set_full(width, height); slot_box[i].set_full(width, height); }
        out_carry.set_full(width, height);
        last_out = nullptr;
        out_valid = false;
        last_partial = false;
        frames_since_full = 0;
        nv_have_pending = false; nv_pending = -1; nv_write = 0;
        nv_out_valid = false;   nv_rect_valid = false;
        desk_valid = false;
    }

    /* Dirty + move rectangles of the currently acquired frame, folded into
     * one bounding box. Must be called while the frame is still acquired
     * (the metadata dies with ReleaseFrame). Anything unexpected returns
     * the full desktop, which is always safe. */
    DxBox changed_box(const DXGI_OUTDUPL_FRAME_INFO &fi) {
        DxBox full; full.set_full(width, height);
        if (rotated || !have_image || fi.TotalMetadataBufferSize == 0) return full;

        const UINT total = fi.TotalMetadataBufferSize;
        if (meta.size() < total) meta.resize(total);

        UINT move_bytes = 0;
        HRESULT hr = duplication->GetFrameMoveRects(
            total, reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT *>(meta.data()), &move_bytes);
        if (FAILED(hr) || move_bytes > total) return full;

        UINT dirty_bytes = 0;
        hr = duplication->GetFrameDirtyRects(
            total - move_bytes, reinterpret_cast<RECT *>(meta.data() + move_bytes), &dirty_bytes);
        if (FAILED(hr) || move_bytes + dirty_bytes > total) return full;

        DxBox box;
        const auto *mv = reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT *>(meta.data());
        for (UINT i = 0; i < move_bytes / sizeof(DXGI_OUTDUPL_MOVE_RECT); ++i) {
            /* Only the destination changed; where the pixels came FROM is
             * either untouched or reported again as a dirty rect. */
            const RECT &d = mv[i].DestinationRect;
            box.add((int)d.left, (int)d.top, (int)d.right, (int)d.bottom);
        }
        const auto *dr = reinterpret_cast<const RECT *>(meta.data() + move_bytes);
        for (UINT i = 0; i < dirty_bytes / sizeof(RECT); ++i)
            box.add((int)dr[i].left, (int)dr[i].top, (int)dr[i].right, (int)dr[i].bottom);

        box.clamp(width, height);
        /* An image WAS presented, so "no rectangles" cannot be right. */
        if (box.empty()) return full;
        return box;
    }

    /* ---------------- GPU path helpers ---------------- */

    void gpu_release() {
        vp_ready = false;
        vin.Reset(); vout.Reset(); vproc.Reset(); venum.Reset();
        vctx.Reset(); vdev.Reset();
        nv12_tex.Reset(); nv_stage[0].Reset(); nv_stage[1].Reset();
        desk_tex.Reset();
        desk_valid = false;
        nv_have_pending = false; nv_pending = -1; nv_write = 0;
        nv_out_valid = false;    nv_rect_valid = false;
    }

    /* (Re)creates every GPU-side object for the current desktop size and
     * NV12 output size. Returns false (leaving vp_ready false) if this
     * adapter/driver can't do BGRA->NV12 through the video processor; the
     * caller then simply keeps using the CPU path. */
    bool gpu_build() {
        gpu_release();
        if (!gpu_enabled || nv_w < 2 || nv_h < 2 || width <= 0 || height <= 0 || rotated) return false;
        if (FAILED(device.As(&vdev)) || FAILED(context.As(&vctx))) { gpu_release(); return false; }

        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)width; td.Height = (UINT)height;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc = {1, 0};
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &desk_tex))) { gpu_release(); return false; }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate   = {60, 1};
        cd.InputWidth       = (UINT)width;
        cd.InputHeight      = (UINT)height;
        cd.OutputFrameRate  = {60, 1};
        cd.OutputWidth      = (UINT)nv_w;
        cd.OutputHeight     = (UINT)nv_h;
        /* OPTIMAL_SPEED asks the driver for its cheapest video-processing mode
         * (no extra filtering/rate-conversion stages) - this blit runs on the
         * same GPU the game is rendering on, so every microsecond of it is a
         * microsecond the game doesn't get. PLAYBACK_NORMAL ("balanced") was
         * what this used before. If a driver rejects the speed hint the
         * enumerator is simply rebuilt with the old value. */
        cd.Usage            = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        if (FAILED(vdev->CreateVideoProcessorEnumerator(&cd, &venum))) {
            venum.Reset();
            cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            if (FAILED(vdev->CreateVideoProcessorEnumerator(&cd, &venum))) { gpu_release(); return false; }
        }

        UINT fmt_in = 0, fmt_out = 0;
        if (FAILED(venum->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &fmt_in)) ||
            !(fmt_in & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) { gpu_release(); return false; }
        if (FAILED(venum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &fmt_out)) ||
            !(fmt_out & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) { gpu_release(); return false; }
        if (FAILED(vdev->CreateVideoProcessor(venum.Get(), 0, &vproc))) { gpu_release(); return false; }

        D3D11_TEXTURE2D_DESC nd{};
        nd.Width = (UINT)nv_w; nd.Height = (UINT)nv_h;
        nd.MipLevels = 1; nd.ArraySize = 1;
        nd.Format = DXGI_FORMAT_NV12;
        nd.SampleDesc = {1, 0};
        nd.Usage = D3D11_USAGE_DEFAULT;
        nd.BindFlags = D3D11_BIND_RENDER_TARGET;   /* required for a VP output view */
        if (FAILED(device->CreateTexture2D(&nd, nullptr, &nv12_tex))) { gpu_release(); return false; }
        nd.Usage = D3D11_USAGE_STAGING;
        nd.BindFlags = 0;
        nd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&nd, nullptr, &nv_stage[0])) ||
            FAILED(device->CreateTexture2D(&nd, nullptr, &nv_stage[1]))) { gpu_release(); return false; }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
        iv.FourCC = 0;
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        iv.Texture2D.MipSlice = 0;
        iv.Texture2D.ArraySlice = 0;
        if (FAILED(vdev->CreateVideoProcessorInputView(desk_tex.Get(), venum.Get(), &iv, &vin))) { gpu_release(); return false; }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
        ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ov.Texture2D.MipSlice = 0;
        if (FAILED(vdev->CreateVideoProcessorOutputView(nv12_tex.Get(), venum.Get(), &ov, &vout))) { gpu_release(); return false; }

        /* No "enhancements" (auto denoise/sharpen/etc.) on a screen capture. */
        vctx->VideoProcessorSetStreamAutoProcessingMode(vproc.Get(), 0, FALSE);
        vctx->VideoProcessorSetStreamFrameFormat(vproc.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

        /* Match the CPU converter (hr_bgra_to_nv12_band): full-range RGB in,
         * BT.601 studio/limited-range (16..235 / 16..240) YCbCr out - which
         * is also what ffmpeg_runner.cpp tags the stream as. */
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
        in_cs.Usage = 0; in_cs.RGB_Range = 0; in_cs.YCbCr_Matrix = 0;
        in_cs.YCbCr_xvYCC = 0; in_cs.Nominal_Range = 2;   /* 0..255 */
        vctx->VideoProcessorSetStreamColorSpace(vproc.Get(), 0, &in_cs);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{};
        out_cs.Usage = 0; out_cs.RGB_Range = 0; out_cs.YCbCr_Matrix = 0;   /* 0 = BT.601 */
        out_cs.YCbCr_xvYCC = 0; out_cs.Nominal_Range = 1;                   /* 16..235   */
        vctx->VideoProcessorSetOutputColorSpace(vproc.Get(), &out_cs);

        vp_ready = true;
        return true;
    }

    /* Keep the persistent GPU-side desktop copy current. Called for every
     * frame that carried a new desktop image, whichever path consumes it. */
    void update_desktop_copy(ID3D11Texture2D *tex, const DxBox &B) {
        if (!gpu_enabled || !desk_tex) return;
        if (!desk_valid || B.empty() || B.is_full(width, height)) {
            context->CopyResource(desk_tex.Get(), tex);
            desk_valid = true;
        } else {
            D3D11_BOX bx{};
            bx.left = (UINT)B.l; bx.top = (UINT)B.t; bx.front = 0;
            bx.right = (UINT)B.r; bx.bottom = (UINT)B.b; bx.back = 1;
            context->CopySubresourceRegion(desk_tex.Get(), 0, (UINT)B.l, (UINT)B.t, 0, tex, 0, &bx);
        }
    }

    /* Runs the video processor: crop `src` out of desk_tex, scale it to
     * nv_w x nv_h, convert to NV12, and queue a copy into nv_stage[slot]. */
    bool nv_blit(const RECT &src_in, int slot) {
        if (!vp_ready || !desk_valid) return false;
        RECT src = src_in;
        if (src.left < 0) src.left = 0;
        if (src.top  < 0) src.top  = 0;
        if (src.right  > width)  src.right  = width;
        if (src.bottom > height) src.bottom = height;
        if (src.right - src.left < 2 || src.bottom - src.top < 2) {
            src.left = 0; src.top = 0; src.right = width; src.bottom = height;
        }
        RECT dst{0, 0, nv_w, nv_h};
        vctx->VideoProcessorSetStreamSourceRect(vproc.Get(), 0, TRUE, &src);
        vctx->VideoProcessorSetStreamDestRect(vproc.Get(), 0, TRUE, &dst);
        vctx->VideoProcessorSetOutputTargetRect(vproc.Get(), TRUE, &dst);

        D3D11_VIDEO_PROCESSOR_STREAM st{};
        st.Enable = TRUE;
        st.pInputSurface = vin.Get();
        HRESULT hr = vctx->VideoProcessorBlt(vproc.Get(), vout.Get(), 0, 1, &st);
        if (FAILED(hr)) return false;
        context->CopyResource(nv_stage[slot].Get(), nv12_tex.Get());
        return true;
    }

    /* Re-acquire duplication interface (needed after HR_DX_LOST) */
    HRESULT reset() {
        if (acquired) {
            if (duplication) duplication->ReleaseFrame();
            acquired = false;
        }
        duplication.Reset();
        staging[0].Reset();
        staging[1].Reset();
        write_idx = 0;
        pending_idx = -1;
        have_pending = false;
        gpu_release();

        ComPtr<IDXGIDevice> dxgi_dev;
        HRESULT hr = device.As(&dxgi_dev);
        if (FAILED(hr)) { g_last_dx_error = hr; return hr; }

        ComPtr<IDXGIAdapter> adapter;
        hr = dxgi_dev->GetAdapter(&adapter);
        if (FAILED(hr)) { g_last_dx_error = hr; return hr; }

        ComPtr<IDXGIOutput> output;
        hr = adapter->EnumOutputs(output_idx, &output);
        if (FAILED(hr)) { g_last_dx_error = hr; return hr; }

        ComPtr<IDXGIOutput1> output1;
        hr = output.As(&output1);
        if (FAILED(hr)) { g_last_dx_error = hr; return hr; }

        for (int attempt = 0; attempt < 5; ++attempt) {
            hr = output1->DuplicateOutput(device.Get(), &duplication);
            if (SUCCEEDED(hr)) break;
            if (hr != E_ACCESSDENIED &&
                hr != static_cast<HRESULT>(DXGI_ERROR_ACCESS_DENIED) &&
                hr != static_cast<HRESULT>(DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) &&
                hr != static_cast<HRESULT>(DXGI_ERROR_SESSION_DISCONNECTED) &&
                hr != E_INVALIDARG)
                break;
            Sleep(20);
        }
        if (FAILED(hr)) { g_last_dx_error = hr; return hr; }

        /* Get fresh size from output desc */
        DXGI_OUTDUPL_DESC dd;
        duplication->GetDesc(&dd);
        width  = (int)dd.ModeDesc.Width;
        height = (int)dd.ModeDesc.Height;
        rotated = (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
                   dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED);

        /* (Re)create both staging textures */
        D3D11_TEXTURE2D_DESC td{};
        td.Width          = (UINT)width;
        td.Height         = (UINT)height;
        td.MipLevels      = 1;
        td.ArraySize      = 1;
        td.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc     = {1, 0};
        td.Usage          = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device->CreateTexture2D(&td, nullptr, &staging[0]);
        if (FAILED(hr)) { g_last_dx_error = hr; return hr; }
        hr = device->CreateTexture2D(&td, nullptr, &staging[1]);
        if (FAILED(hr)) { g_last_dx_error = hr; return hr; }

        reset_tracking();
        /* A fresh duplication invalidates the GPU objects (desktop size may
         * have changed); rebuild them if the caller had asked for the GPU
         * path. A failure is not fatal - vp_ready simply stays false. */
        if (gpu_enabled) gpu_build();
        return S_OK;
    }
};
#endif

/* -------------------------------------------------------------------------
 * hr_dx_create
 * ---------------------------------------------------------------------- */
HR_EXPORT void *hr_dx_create(int adapter_idx, int output_idx) {
#ifdef _WIN32
    DxCapCtx *ctx = nullptr;
    try { ctx = new DxCapCtx(); } catch (...) { return nullptr; }
    ctx->adapter_idx = adapter_idx;
    ctx->output_idx  = output_idx;

    /* Enumerate adapters */
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                     reinterpret_cast<void **>(factory.GetAddressOf()));
    if (FAILED(hr)) { g_last_dx_error = hr; delete ctx; return nullptr; }

    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapters1((UINT)adapter_idx, &adapter);
    if (FAILED(hr)) { g_last_dx_error = hr; delete ctx; return nullptr; }

    /* Create D3D11 device on this adapter */
    D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
#ifndef D3D11_CREATE_DEVICE_VIDEO_SUPPORT
#define D3D11_CREATE_DEVICE_VIDEO_SUPPORT 0x800
#endif
    /* VIDEO_SUPPORT is what the GPU colour-conversion path (D3D11 video
     * processor) needs. Not every driver/OS accepts the flag, and capture
     * itself doesn't need it, so retry without it rather than lose capture. */
    hr = D3D11CreateDevice(
        adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0,
        D3D11_SDK_VERSION,
        &ctx->device, &fl, &ctx->context);
    if (FAILED(hr)) {
        ctx->device.Reset();
        ctx->context.Reset();
        hr = D3D11CreateDevice(
            adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,  /* BGRA needed for DXGI surface map */
            nullptr, 0,
            D3D11_SDK_VERSION,
            &ctx->device, &fl, &ctx->context);
    }
    if (FAILED(hr)) { g_last_dx_error = hr; delete ctx; return nullptr; }
    {
        ComPtr<IDXGIDevice> gpu_dev;
        if (SUCCEEDED(ctx->device.As(&gpu_dev))) gpu_dev->SetGPUThreadPriority(kCaptureGpuPriority);
    }

    hr = ctx->reset();
    if (FAILED(hr)) { delete ctx; return nullptr; } // reset() already set g_last_dx_error

    g_last_dx_error = S_OK;
    return ctx;
#else
    return nullptr;
#endif
}

/* -------------------------------------------------------------------------
 * hr_dx_last_error
 *
 * HRESULT (as an unsigned 32-bit value, since HR_EXPORT functions are
 * plain C and HRESULT isn't a portable type across the DLL boundary) from
 * the most recent hr_dx_create() failure - S_OK (0) if the most recent
 * attempt succeeded or none has run yet. Purely diagnostic: lets callers
 * log *why* DXGI init failed (access denied vs. no current session vs.
 * something else) instead of just "returned null", without having to
 * plumb HRESULT itself across the C boundary. See hr_pl_create()'s use of
 * this in pipeline.cpp.
 * ---------------------------------------------------------------------- */
HR_EXPORT unsigned long hr_dx_last_error(void) {
#ifdef _WIN32
    return (unsigned long)g_last_dx_error;
#else
    return 0;
#endif
}

/* -------------------------------------------------------------------------
 * hr_dx_destroy
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_dx_destroy(void *handle) {
#ifdef _WIN32
    if (!handle) return;
    auto *ctx = static_cast<DxCapCtx *>(handle);
    if (ctx->acquired) {
        ctx->duplication->ReleaseFrame();
        ctx->acquired = false;
    }
    delete ctx;
#endif
}

/* -------------------------------------------------------------------------
 * hr_dx_get_size
 * ---------------------------------------------------------------------- */
HR_EXPORT int hr_dx_get_size(void *handle, int *out_w, int *out_h) {
#ifdef _WIN32
    if (!handle) return 0;
    auto *ctx = static_cast<DxCapCtx *>(handle);
    if (out_w) *out_w = ctx->width;
    if (out_h) *out_h = ctx->height;
    return 1;
#else
    return 0;
#endif
}

/* -------------------------------------------------------------------------
 * hr_dx_set_output_size
 *
 * (diagonal line/pixel corruption + the bottom slice of the frame
 * missing, seen recording fullscreen games -- e.g. older Source titles --
 * running at a lower exclusive-fullscreen resolution than the desktop,
 * such as 1366x768 on a 1600x900 desktop):
 *
 * The caller (pipeline.cpp) allocates its bgra_buf once, at
 * hr_pl_start() time, sized to whatever dx_get_size() reports THEN, and
 * tells ffmpeg's rawvideo demuxer that exact width/height for the entire
 * recording. But a game switching into (or out of, or between two
 * different) exclusive-fullscreen resolutions after that point triggers
 * DXGI_ERROR_ACCESS_LOST -- handled by calling reset() -- and reset()
 * re-reads the OS's current output mode and resizes DxCapCtx::width/
 * height to match it. hr_dx_capture() used to always blit ctx->width
 * tightly-packed columns per row; once ctx->width no longer matched what
 * the caller's buffer/ffmpeg pipe were told, every row landed at the
 * wrong offset in the caller's buffer (a smaller ctx->width packed into a
 * wider expected stride), which reads back as the frame shearing into
 * diagonal lines, and left the buffer's tail -- the bottom rows, since
 * everything is one row-major array -- with whatever stale bytes were
 * there before, i.e. a chunk of the bottom of the frame effectively
 * missing.
 *
 * Call this once with the width/height the caller's buffer was actually
 * allocated for (immediately after that allocation). From then on,
 * hr_dx_capture() always fills exactly that many rows of exactly that
 * stride, regardless of what the desktop's actual mode does mid-
 * recording -- nearest-neighbour stretching the real frame to fit if it
 * doesn't match -- so a resolution change can, at worst, introduce mild
 * resampling, instead of ever writing a corrupted/misaligned frame into
 * the caller's buffer.
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_dx_set_output_size(void *handle, int w, int h) {
#ifdef _WIN32
    if (!handle) return;
    auto *ctx = static_cast<DxCapCtx *>(handle);
    ctx->out_w = (w > 0) ? w : 0;
    ctx->out_h = (h > 0) ? h : 0;
    // A (re)allocated caller buffer must never be patched incrementally:
    // it may sit at the same address as the previous one but hold nothing.
    ctx->out_valid = false;
    ctx->last_out  = nullptr;
    ctx->last_partial = false;
    ctx->out_carry.set_full(ctx->width, ctx->height);
#else
    (void)handle; (void)w; (void)h;
#endif
}

/* -------------------------------------------------------------------------
 * Frame acquisition shared by the CPU (BGRA) and GPU (NV12) paths.
 *
 * SKIP UNCHANGED FRAMES: AcquireNextFrame() also returns successfully when
 * only the mouse pointer moved or was redrawn. In that case
 * LastPresentTime and AccumulatedFrames are both 0 - the desktop *image*
 * has not changed at all (the pointer is delivered out-of-band). The old
 * code still CopyResource()'d the whole desktop into a staging texture,
 * mapped it, memcpy'd ~8MB (1080p) into the caller's buffer and made the
 * caller re-convert the whole picture - every tick, on an otherwise static
 * screen. Now such frames are released immediately and reported the same
 * way a real DXGI timeout is (HR_DX_TIMEOUT: "nothing new"), which
 * capture_loop() already turns into a cheap "repeat the last frame".
 * ---------------------------------------------------------------------- */
enum class DxAcq { Timeout, Lost, Error, Idle, New };

static DxAcq dx_acquire(DxCapCtx *c, int timeout_ms, ComPtr<ID3D11Texture2D> &tex, DxBox &box) {
    /* Release any previously acquired frame */
    if (c->acquired) {
        c->duplication->ReleaseFrame();
        c->acquired = false;
    }

    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource>   res;
    HRESULT hr = c->duplication->AcquireNextFrame((UINT)timeout_ms, &fi, &res);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)    return DxAcq::Timeout;
    if (hr == DXGI_ERROR_ACCESS_LOST ||
        hr == DXGI_ERROR_INVALID_CALL)    return DxAcq::Lost;
    if (FAILED(hr))                        return DxAcq::Error;

    c->acquired = true;
    ++c->st_acquired;

    /* Pointer-only update: LastPresentTime == 0 AND AccumulatedFrames == 0.
     * (Requiring both is deliberately conservative: if a driver reports one
     * without the other we copy - a wasted copy, never a lost update.) The
     * very first image is always taken. */
    if (c->have_image && fi.LastPresentTime.QuadPart == 0 && fi.AccumulatedFrames == 0) {
        c->duplication->ReleaseFrame();
        c->acquired = false;
        ++c->st_skipped;
        return DxAcq::Idle;
    }

    hr = res.As(&tex);
    if (FAILED(hr)) {
        c->duplication->ReleaseFrame(); c->acquired = false;
        return DxAcq::Error;
    }

    /* What part of the picture changed? (metadata dies at ReleaseFrame) */
    box = c->changed_box(fi);
    if (!c->have_image || c->rotated || ++c->frames_since_full >= kFullRefreshEvery) {
        box.set_full(c->width, c->height);
        c->frames_since_full = 0;
    }
    c->have_image = true;
    return DxAcq::New;
}

static inline void dx_release(DxCapCtx *c) {
    if (c->acquired) {
        c->duplication->ReleaseFrame();
        c->acquired = false;
    }
}

/* Copy `cb` from src into dst on the GPU (whole resource if cb is empty/full). */
static void dx_copy_region(DxCapCtx *c, ID3D11Texture2D *dst, ID3D11Texture2D *src, const DxBox &cb) {
    if (cb.empty() || cb.is_full(c->width, c->height)) {
        c->context->CopyResource(dst, src);
    } else {
        D3D11_BOX bx{};
        bx.left = (UINT)cb.l; bx.top = (UINT)cb.t; bx.front = 0;
        bx.right = (UINT)cb.r; bx.bottom = (UINT)cb.b; bx.back = 1;
        c->context->CopySubresourceRegion(dst, 0, (UINT)cb.l, (UINT)cb.t, 0, src, 0, &bx);
    }
}

/* Map a staging texture. wait=false polls with DO_NOT_WAIT for up to
 * max(timeout_ms, 8) ms and returns HR_DX_TIMEOUT if the GPU isn't done. */
static int dx_map(DxCapCtx *c, ID3D11Texture2D *tex, bool wait, int timeout_ms,
                   D3D11_MAPPED_SUBRESOURCE &mapped) {
    HRESULT hr;
    if (wait) {
        hr = c->context->Map(tex, 0, D3D11_MAP_READ, 0, &mapped);
    } else {
        const ULONGLONG map_deadline = GetTickCount64() + (ULONGLONG)std::max(timeout_ms, 8);
        for (;;) {
            hr = c->context->Map(tex, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (hr != DXGI_ERROR_WAS_STILL_DRAWING) break;
            if (GetTickCount64() >= map_deadline) return HR_DX_TIMEOUT;
            Sleep(1);
        }
    }
    return FAILED(hr) ? HR_DX_ERROR : HR_DX_OK;
}

/* -------------------------------------------------------------------------
 * CPU path: hand staging[slot] to the caller's BGRA buffer.
 *
 * When the caller's buffer is known to hold the previously delivered frame
 * (same pointer, same shape), only the bounding box of what changed since
 * then is copied - a blinking caret costs a few KB instead of 8MB. The
 * changed row range is remembered for hr_dx_get_dirty_rows() so the
 * pipeline can also skip re-converting the unchanged rows.
 * ---------------------------------------------------------------------- */
static int cpu_deliver(DxCapCtx *c, int slot, uint8_t *out, bool wait, int timeout_ms) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    int mr = dx_map(c, c->staging[slot].Get(), wait, timeout_ms, mapped);
    if (mr != HR_DX_OK) return mr;

    // Target shape the caller's buffer actually has room for -- see the
    // hr_dx_set_output_size() comment above. Falls back to the real
    // captured size (old behaviour) when nobody's called it.
    const int W = c->width, H = c->height;
    const int tgt_w = (c->out_w  > 0) ? c->out_w  : W;
    const int tgt_h = (c->out_h  > 0) ? c->out_h  : H;
    const int dst_row_bytes = tgt_w * 4;
    const uint8_t *src = reinterpret_cast<const uint8_t *>(mapped.pData);

    if (tgt_w == W && tgt_h == H) {
        DxBox R = c->out_carry;
        R.add(c->slot_box[slot]);
        R.clamp(W, H);
        const bool partial = c->out_valid && c->last_out == out && !R.empty() && !R.is_full(W, H);
        if (partial) {
            const size_t bytes = (size_t)(R.r - R.l) * 4;
            for (int y = R.t; y < R.b; ++y)
                memcpy(out + ((size_t)y * W + R.l) * 4,
                       src + (size_t)y * mapped.RowPitch + (size_t)R.l * 4, bytes);
            c->last_partial = true;
            c->last_dirty_y0 = R.t;
            c->last_dirty_y1 = R.b;
            ++c->st_partial;
        } else {
            // Common case: nothing's mismatched, straight row copy.
            uint8_t *dst = out;
            for (int y = 0; y < tgt_h; ++y) {
                memcpy(dst, src + (size_t)y * mapped.RowPitch, (size_t)dst_row_bytes);
                dst += dst_row_bytes;
            }
            c->last_partial = false;
            ++c->st_full;
        }
        c->out_valid = true;
        c->last_out  = out;
    } else {
        uint8_t *dst = out;
        const float rx = (float)W / (float)tgt_w;
        const float ry = (float)H / (float)tgt_h;
        for (int y = 0; y < tgt_h; ++y) {
            int sy = (int)(y * ry);
            if (sy >= H) sy = H - 1;
            const uint8_t *srow = src + (size_t)sy * mapped.RowPitch;
            uint32_t *drow = reinterpret_cast<uint32_t *>(dst);
            for (int x = 0; x < tgt_w; ++x) {
                int sx = (int)(x * rx);
                if (sx >= W) sx = W - 1;
                drow[x] = reinterpret_cast<const uint32_t *>(srow)[sx];
            }
            dst += dst_row_bytes;
        }
        // A stretched picture is not incrementally updatable.
        c->out_valid = false;
        c->last_out  = nullptr;
        c->last_partial = false;
        ++c->st_full;
    }
    c->out_carry.clear();

    c->context->Unmap(c->staging[slot].Get(), 0);
    return HR_DX_OK;
}

/* Nothing new from DXGI (timeout, or a pointer-only update). */
static int cpu_idle(DxCapCtx *c, uint8_t *out, int timeout_ms) {
    if (c->have_pending) {
        /* FLUSH: the newest real frame is still sitting in the second
         * staging texture because delivery is deliberately one call behind
         * (so Map() never stalls on the copy it just queued). On a screen
         * that then goes quiet nothing would ever push it out: the last
         * change before a pause (a dialog opening, the final typed
         * character) stayed invisible in the recording until the *next*
         * change happened. Hand it over now. If the GPU still isn't done,
         * keep it pending and retry on the next call. */
        int r = cpu_deliver(c, c->pending_idx, out, false, timeout_ms);
        if (r == HR_DX_OK) { c->have_pending = false; return HR_DX_OK; }
        return r;
    }
    /* Nothing pending, but the caller's buffer may still be out of date
     * (frames were consumed by the GPU/NV12 path meanwhile). Re-render it
     * from the persistent GPU copy of the desktop, synchronously - this only
     * happens on a path switch, not per frame. */
    if (!c->out_carry.empty() && c->gpu_enabled && c->desk_valid && c->staging[0]) {
        const int w = c->write_idx;
        DxBox all; all.set_full(c->width, c->height);
        c->context->CopyResource(c->staging[w].Get(), c->desk_tex.Get());
        c->stale[w].clear();
        c->slot_box[w] = all;
        return cpu_deliver(c, w, out, /*wait=*/true, timeout_ms);
    }
    return HR_DX_TIMEOUT;
}

/* A real new desktop image `tex` (changed region B) for the CPU path. */
static int cpu_new_frame(DxCapCtx *c, ID3D11Texture2D *tex, const DxBox &B,
                          uint8_t *out, int timeout_ms) {
    /* Bring the ring's write slot up to date: it lags the desktop by every
     * change since it was last written, so copy exactly that region. */
    c->stale[0].add(B);
    c->stale[1].add(B);
    const int w = c->write_idx;
    const DxBox cb = c->stale[w];
    c->stale[w].clear();
    dx_copy_region(c, c->staging[w].Get(), tex, cb);
    c->slot_box[w] = B;

    /* Release the desktop-duplication frame as soon as the copy is queued
     * rather than after Map()/Unmap() -- lets the next AcquireNextFrame()
     * proceed sooner, and CopyResource()'s destination keeps the data
     * alive regardless of when the source frame is released. */
    dx_release(c);

    /* Output the buffer copied on the *previous* call, not this one -- see
     * the staging[] comment on DxCapCtx for why. */
    const int read_idx = c->pending_idx;
    const bool have_output = c->have_pending;
    c->pending_idx  = w;
    c->have_pending = true;
    c->write_idx   ^= 1;

    if (!have_output) {
        /* Nothing older to hand out. Normally that just means "first frame
         * after an idle period": report it like a real DXGI timeout (the
         * capture loop carries the last frame forward) and deliver this one
         * on the next call. But when the caller's buffer holds no usable
         * picture at all (very first frame, after a reset, or after the GPU
         * path consumed frames) deliver right now instead, so the first
         * thing that gets encoded isn't stale/black. */
        if (c->out_valid && c->out_carry.empty()) return HR_DX_TIMEOUT;
        int r = cpu_deliver(c, w, out, /*wait=*/true, timeout_ms);
        if (r == HR_DX_OK) c->have_pending = false;
        return r == HR_DX_OK ? HR_DX_OK : HR_DX_TIMEOUT;
    }

    int r = cpu_deliver(c, read_idx, out, /*wait=*/false, timeout_ms);
    if (r == HR_DX_TIMEOUT) {
        /* The older frame is dropped (the newer one stays pending) - the
         * caller's buffer now lags by whatever that frame changed. */
        c->out_carry.add(c->slot_box[read_idx]);
    }
    return r;
}

/* -------------------------------------------------------------------------
 * hr_dx_capture
 * Grabs one frame into caller-allocated BGRA buffer.
 * ---------------------------------------------------------------------- */
HR_EXPORT int hr_dx_capture(void *handle, uint8_t *out_bgra, int timeout_ms) {
#ifdef _WIN32
    if (!handle || !out_bgra) return HR_DX_ERROR;
    auto *ctx = static_cast<DxCapCtx *>(handle);

    // CRASHFIX (0xC0000005 null-pointer read a few ms after "DXGI capture
    // lost -- resetting", nothing else logged in between - matches the
    // supplied crash dump exactly: exception in hr.exe itself, read access
    // violation at address 0x0). reset() (called from hr_dx_reset() after
    // HR_DX_LOST, see capture_loop()'s HR_DX_LOST branch) unconditionally
    // does `duplication.Reset()` up front before trying to re-acquire a
    // new IDXGIOutputDuplication - if that re-acquire then fails (e.g. a
    // game is holding exclusive fullscreen right after the display mode
    // change that caused the loss in the first place - a completely normal,
    // expected condition DuplicateOutput() itself already retries for, see
    // reset()'s own loop), duplication is left null and reset() returns a
    // failure HRESULT. capture_loop() logs the "resetting" message and
    // kicks reset() off but was never checking that return value, so on
    // the very next tick it called back in here anyway - and every call
    // below assumes ctx->duplication is a live object. Calling a virtual
    // method (AcquireNextFrame) through a null ComPtr reads the vtable
    // pointer from address 0, which is exactly this crash. Reporting
    // HR_DX_LOST here (instead of dereferencing) makes this call site
    // behave the same way it already does for every OTHER "capture isn't
    // currently possible" case: capture_loop() falls back to re-encoding
    // the last good frame and retries dx_reset() again in a second,
    // instead of taking the whole app down.
    if (!ctx->duplication) return HR_DX_LOST;

    ComPtr<ID3D11Texture2D> tex;
    DxBox box;
    switch (dx_acquire(ctx, timeout_ms, tex, box)) {
        case DxAcq::Timeout:
        case DxAcq::Idle:  return cpu_idle(ctx, out_bgra, timeout_ms);
        case DxAcq::Lost:  return HR_DX_LOST;
        case DxAcq::Error: return HR_DX_ERROR;
        case DxAcq::New:   break;
    }

    /* Keep the GPU-side desktop copy current (no-op unless the GPU path is
     * enabled) and tell the NV12 ring that the frame went elsewhere. */
    ctx->update_desktop_copy(tex.Get(), box);
    if (ctx->gpu_enabled) { ctx->nv_have_pending = false; ctx->nv_out_valid = false; }

    return cpu_new_frame(ctx, tex.Get(), box, out_bgra, timeout_ms);
#else
    (void)handle; (void)out_bgra; (void)timeout_ms;
    return HR_DX_ERROR;
#endif
}

/* -------------------------------------------------------------------------
 * hr_dx_get_dirty_rows
 *
 * After a hr_dx_capture() that returned HR_DX_OK: 1 and [*y0,*y1) if only
 * those rows of the caller's buffer differ from what the PREVIOUS
 * HR_DX_OK delivery left there (so a caller that still has that previous
 * picture converted can re-convert just those rows); 0 if the whole frame
 * must be treated as changed (first frame, stretched output, periodic full
 * refresh, path switch...).
 * ---------------------------------------------------------------------- */
HR_EXPORT int hr_dx_get_dirty_rows(void *handle, int *y0, int *y1) {
#ifdef _WIN32
    if (!handle) return 0;
    auto *ctx = static_cast<DxCapCtx *>(handle);
    if (!ctx->last_partial) return 0;
    if (y0) *y0 = ctx->last_dirty_y0;
    if (y1) *y1 = ctx->last_dirty_y1;
    return 1;
#else
    (void)handle; (void)y0; (void)y1;
    return 0;
#endif
}

/* -------------------------------------------------------------------------
 * hr_dx_get_stats  - diagnostics only.
 * ---------------------------------------------------------------------- */
HR_EXPORT void hr_dx_get_stats(void *handle, unsigned long long *acquired,
                                unsigned long long *skipped,
                                unsigned long long *partial,
                                unsigned long long *full,
                                unsigned long long *gpu_frames) {
#ifdef _WIN32
    if (!handle) return;
    auto *ctx = static_cast<DxCapCtx *>(handle);
    if (acquired)   *acquired   = ctx->st_acquired;
    if (skipped)    *skipped    = ctx->st_skipped;
    if (partial)    *partial    = ctx->st_partial;
    if (full)       *full       = ctx->st_full;
    if (gpu_frames) *gpu_frames = ctx->st_gpu_frames;
#else
    (void)handle; (void)acquired; (void)skipped; (void)partial; (void)full; (void)gpu_frames;
#endif
}

/* =========================================================================
 * GPU PATH: D3D11 Video Processor, BGRA -> NV12 (+ crop + scale)
 *
 * The desktop image never leaves the GPU until it is already NV12: the
 * readback is 1.5 bytes/pixel instead of 4 and the CPU does no colour
 * conversion or scaling at all (works on integrated GPUs too - the video
 * processor is part of every D3D11 driver that supports video).
 *
 * Semantics of hr_dx_capture_nv12() mirror hr_dx_capture(): HR_DX_OK = a
 * (possibly new) complete NV12 frame is in out_nv12; HR_DX_TIMEOUT =
 * nothing changed, the caller's previous NV12 buffer is still current.
 * ====================================================================== */

/* Enable (out_w/out_h > 0) or disable (0/0) the GPU path. out_w/out_h are
 * the NV12 frame size; they are rounded down to even. cur_bgra, if not
 * null, is the caller's current full-desktop BGRA buffer, used to seed the
 * persistent GPU copy of the desktop when DXGI has no new image to give us
 * (static screen). Returns 1 if the video processor is usable. */
HR_EXPORT int hr_dx_gpu_enable(void *handle, int out_w, int out_h, const uint8_t *cur_bgra) {
#ifdef _WIN32
    if (!handle) return 0;
    auto *ctx = static_cast<DxCapCtx *>(handle);
    out_w &= ~1; out_h &= ~1;
    if (out_w < 2 || out_h < 2) {
        ctx->gpu_enabled = false;
        ctx->gpu_release();
        return 0;
    }
    const bool same = ctx->gpu_enabled && ctx->vp_ready && ctx->nv_w == out_w && ctx->nv_h == out_h;
    if (!same) {
        ctx->gpu_enabled = true;
        ctx->nv_w = out_w;
        ctx->nv_h = out_h;
        if (!ctx->gpu_build()) return 0;
    }

    /* Seed the desktop copy from the caller's buffer - only if that buffer
     * is provably the latest picture (fully synced, nothing pending, and
     * exactly the desktop's size). Retried on every call until it works,
     * because on a static screen DXGI will never deliver an image that
     * could seed it the normal way. */
    if (!ctx->desk_valid && cur_bgra && ctx->have_image && ctx->out_valid &&
        ctx->last_out == cur_bgra && ctx->out_carry.empty() && !ctx->have_pending &&
        (ctx->out_w <= 0 || ctx->out_w == ctx->width) &&
        (ctx->out_h <= 0 || ctx->out_h == ctx->height)) {
        ctx->context->UpdateSubresource(ctx->desk_tex.Get(), 0, nullptr, cur_bgra,
                                         (UINT)ctx->width * 4, 0);
        ctx->desk_valid = true;
    }
    return 1;
#else
    (void)handle; (void)out_w; (void)out_h; (void)cur_bgra;
    return 0;
#endif
}

HR_EXPORT int hr_dx_gpu_ready(void *handle) {
#ifdef _WIN32
    if (!handle) return 0;
    auto *ctx = static_cast<DxCapCtx *>(handle);
    return (ctx->gpu_enabled && ctx->vp_ready && ctx->desk_valid) ? 1 : 0;
#else
    (void)handle;
    return 0;
#endif
}

/* Hand staging NV12 texture `slot` to the caller (tightly packed: Y plane
 * nv_w*nv_h, then interleaved UV plane nv_w*nv_h/2). */
static int nv_deliver(DxCapCtx *c, int slot, uint8_t *out, bool wait, int timeout_ms) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    int mr = dx_map(c, c->nv_stage[slot].Get(), wait, timeout_ms, mapped);
    if (mr != HR_DX_OK) return mr;

    const uint8_t *src = reinterpret_cast<const uint8_t *>(mapped.pData);
    const size_t   pitch = mapped.RowPitch;
    /* The chroma plane follows the luma plane at RowPitch * texture height
     * (same convention ffmpeg's d3d11va hwdownload relies on). */
    const uint8_t *uv = src + pitch * (size_t)c->nv_h;
    uint8_t *uv_out = out + (size_t)c->nv_w * c->nv_h;
    if (pitch == (size_t)c->nv_w) {
        /* Typical for widths that are a multiple of the driver's alignment (1920, 1280...):
         * the mapped planes are already tightly packed -> two big copies instead of
         * ~1620 row copies per frame. */
        memcpy(out, src, (size_t)c->nv_w * c->nv_h);
        memcpy(uv_out, uv, (size_t)c->nv_w * (c->nv_h / 2));
    } else {
        for (int y = 0; y < c->nv_h; ++y)
            memcpy(out + (size_t)y * c->nv_w, src + (size_t)y * pitch, (size_t)c->nv_w);
        for (int y = 0; y < c->nv_h / 2; ++y)
            memcpy(uv_out + (size_t)y * c->nv_w, uv + (size_t)y * pitch, (size_t)c->nv_w);
    }

    c->context->Unmap(c->nv_stage[slot].Get(), 0);
    c->nv_out_valid = true;
    ++c->st_gpu_frames;
    return HR_DX_OK;
}

static int nv_idle(DxCapCtx *c, const RECT &rect, uint8_t *out, int timeout_ms) {
    if (c->nv_have_pending) {
        int r = nv_deliver(c, c->nv_pending, out, false, timeout_ms);
        if (r == HR_DX_OK) { c->nv_have_pending = false; return HR_DX_OK; }
        return r;
    }
    const bool rect_changed = !c->nv_rect_valid ||
        memcmp(&rect, &c->nv_last_rect, sizeof(RECT)) != 0;
    if ((rect_changed || !c->nv_out_valid) && c->desk_valid) {
        /* The desktop is static but the picture we owe the caller changed
         * (its crop rectangle moved, or its NV12 buffer is stale after a
         * stretch on the CPU path): re-render from the persistent copy. */
        const int w = c->nv_write;
        if (!c->nv_blit(rect, w)) return HR_DX_ERROR;
        c->nv_last_rect = rect; c->nv_rect_valid = true;
        return nv_deliver(c, w, out, /*wait=*/true, timeout_ms);
    }
    return HR_DX_TIMEOUT;
}

static int nv_new_frame(DxCapCtx *c, const RECT &rect, uint8_t *out, int timeout_ms) {
    const int w = c->nv_write;
    if (!c->nv_blit(rect, w)) return HR_DX_ERROR;
    c->nv_last_rect = rect; c->nv_rect_valid = true;

    const int  read_idx    = c->nv_pending;
    const bool have_output = c->nv_have_pending;
    c->nv_pending      = w;
    c->nv_have_pending = true;
    c->nv_write       ^= 1;

    if (!have_output) {
        if (c->nv_out_valid) return HR_DX_TIMEOUT;   /* delivered next call */
        int r = nv_deliver(c, w, out, /*wait=*/true, timeout_ms);
        if (r == HR_DX_OK) c->nv_have_pending = false;
        return r;
    }
    return nv_deliver(c, read_idx, out, /*wait=*/false, timeout_ms);
}

HR_EXPORT int hr_dx_capture_nv12(void *handle, uint8_t *out_nv12, int timeout_ms,
                                  int src_l, int src_t, int src_r, int src_b) {
#ifdef _WIN32
    if (!handle || !out_nv12) return HR_DX_ERROR;
    auto *ctx = static_cast<DxCapCtx *>(handle);
    if (!ctx->duplication) return HR_DX_LOST;
    /* Refuse BEFORE acquiring so a frame is never taken and then thrown away. */
    if (!ctx->gpu_enabled || !ctx->vp_ready) return HR_DX_ERROR;

    const RECT rect{src_l, src_t, src_r, src_b};
    ComPtr<ID3D11Texture2D> tex;
    DxBox box;
    switch (dx_acquire(ctx, timeout_ms, tex, box)) {
        case DxAcq::Timeout:
        case DxAcq::Idle:  return nv_idle(ctx, rect, out_nv12, timeout_ms);
        case DxAcq::Lost:  return HR_DX_LOST;
        case DxAcq::Error: return HR_DX_ERROR;
        case DxAcq::New:   break;
    }

    ctx->update_desktop_copy(tex.Get(), box);
    dx_release(ctx);

    /* The CPU ring did not see this frame: its staging textures are now
     * behind by `box`, its pending frame is obsolete, and the caller's BGRA
     * buffer no longer matches the desktop. The next CPU-path call will
     * therefore do one full, synchronous refresh (see cpu_idle()). */
    ctx->stale[0].add(box);
    ctx->stale[1].add(box);
    ctx->have_pending = false;
    ctx->out_carry.set_full(ctx->width, ctx->height);

    return nv_new_frame(ctx, rect, out_nv12, timeout_ms);
#else
    (void)handle; (void)out_nv12; (void)timeout_ms;
    (void)src_l; (void)src_t; (void)src_r; (void)src_b;
    return HR_DX_ERROR;
#endif
}

/* -------------------------------------------------------------------------
 * hr_dx_reset  (call after HR_DX_LOST)
 * ---------------------------------------------------------------------- */
HR_EXPORT int hr_dx_reset(void *handle) {
#ifdef _WIN32
    if (!handle) return 0;
    auto *ctx = static_cast<DxCapCtx *>(handle);
    HRESULT hr = ctx->reset();
    return SUCCEEDED(hr) ? 1 : 0;
#else
    return 0;
#endif
}

/* -------------------------------------------------------------------------
 * hr_dx_adapter_count
 * ---------------------------------------------------------------------- */
HR_EXPORT int hr_dx_adapter_count() {
#ifdef _WIN32
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
               reinterpret_cast<void **>(factory.GetAddressOf()))))
        return 0;
    int cnt = 0;
    ComPtr<IDXGIAdapter1> a;
    while (factory->EnumAdapters1((UINT)cnt, &a) != DXGI_ERROR_NOT_FOUND)
        ++cnt;
    return cnt;
#else
    return 0;
#endif
}

/* -------------------------------------------------------------------------
 * hr_dx_output_count
 * ---------------------------------------------------------------------- */
HR_EXPORT int hr_dx_output_count(int adapter_idx) {
#ifdef _WIN32
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
               reinterpret_cast<void **>(factory.GetAddressOf()))))
        return 0;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1((UINT)adapter_idx, &adapter)))
        return 0;
    int cnt = 0;
    ComPtr<IDXGIOutput> out;
    while (adapter->EnumOutputs((UINT)cnt, &out) != DXGI_ERROR_NOT_FOUND)
        ++cnt;
    return cnt;
#else
    return 0;
#endif
}

/* -------------------------------------------------------------------------
 * hr_dx_output_desc
 * ---------------------------------------------------------------------- */
HR_EXPORT int hr_dx_output_desc(int adapter_idx, int output_idx,
                                  int *out_x, int *out_y,
                                  int *out_w, int *out_h,
                                  char *name_buf, int name_buf_len) {
#ifdef _WIN32
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
               reinterpret_cast<void **>(factory.GetAddressOf()))))
        return 0;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1((UINT)adapter_idx, &adapter))) return 0;
    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs((UINT)output_idx, &output))) return 0;

    DXGI_OUTPUT_DESC desc{};
    if (FAILED(output->GetDesc(&desc))) return 0;

    if (out_x) *out_x = (int)desc.DesktopCoordinates.left;
    if (out_y) *out_y = (int)desc.DesktopCoordinates.top;
    if (out_w) *out_w = (int)(desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left);
    if (out_h) *out_h = (int)(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);

    if (name_buf && name_buf_len > 0) {
        /* DeviceName is wchar_t; convert to UTF-8 */
        int n = WideCharToMultiByte(CP_UTF8, 0,
                                     desc.DeviceName, -1,
                                     name_buf, name_buf_len,
                                     nullptr, nullptr);
        if (n <= 0) name_buf[0] = '\0';
    }
    return 1;
#else
    return 0;
#endif
}

/* -------------------------------------------------------------------------
 * hr_dx_recommend_codec
 *
 * First-launch-only helper (see main_frame.cpp's `first_launch` branch):
 * HomRec's compiled-in default encoder used to be a flat "libx264" for
 * every install (AppState::video_codec's in-class initializer), regardless
 * of what's actually in the machine. OBS auto-picks a GPU encoder
 * (NVENC/QSV/AMF) when one's available and only falls back to software
 * x264 when it isn't - HomRec never did, so identical recording settings
 * on a machine with a capable GPU meant HomRec was doing real-time video
 * encoding on the CPU while OBS was doing it on the GPU, a large and
 * entirely avoidable CPU gap that had nothing to do with how efficient
 * either app's *own* code was.
 *
 * Walks the same IDXGIAdapter1 list hr_dx_adapter_count()/hr_dx_create()
 * already enumerate (adapter 0 is what capture actually binds to), skips
 * the software/WARP adapter DXGI always reports, and scores every
 * recognized vendor's adapter so a real discrete GPU (NVIDIA/AMD) outranks
 * an Intel iGPU on a laptop that has both, and a bigger card outranks a
 * smaller one of the same vendor. Writes the matching ffmpeg encoder name
 * into out_buf ("h264_nvenc" / "h264_amf" / "h264_qsv") and returns 1, or
 * writes "libx264" and returns 0 if nothing recognized was found (old GPU,
 * remote/RDP session, or a vendor this function doesn't know about yet -
 * software encode is always a safe, working fallback).
 *
 * Deliberately just a static vendor-ID check, not a real "does ffmpeg's
 * h264_nvenc actually initialize on this driver" probe (that would mean
 * spawning ffmpeg and doing a throwaway encode before the user has even
 * seen the main window) - Settings > Video already lets anyone whose
 * hardware encoder turns out not to work switch back to libx264 by hand.
 * ---------------------------------------------------------------------- */
HR_EXPORT int hr_dx_recommend_codec(char *out_buf, int buf_chars) {
    const char *codec = "libx264";
    int found_gpu = 0;
#ifdef _WIN32
    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                  reinterpret_cast<void **>(factory.GetAddressOf())))) {
        UINT best_vendor = 0;
        unsigned long long best_score = 0;
        int cnt = 0;
        ComPtr<IDXGIAdapter1> a;
        while (factory->EnumAdapters1((UINT)cnt, &a) != DXGI_ERROR_NOT_FOUND) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(a->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                bool known = desc.VendorId == 0x10DE /* NVIDIA */ ||
                             desc.VendorId == 0x1002 /* AMD/ATI */ ||
                             desc.VendorId == 0x1022 /* AMD (older ID) */ ||
                             desc.VendorId == 0x8086 /* Intel */;
                if (known) {
                    // Intel here is (almost) always an iGPU; treat NVIDIA/AMD
                    // with meaningful dedicated VRAM as "discrete" and always
                    // prefer it over Intel, then break ties by VRAM size.
                    bool discrete = desc.VendorId != 0x8086 &&
                                     desc.DedicatedVideoMemory > (SIZE_T)(256ull * 1024 * 1024);
                    unsigned long long score = (unsigned long long)desc.DedicatedVideoMemory +
                                                (discrete ? (1ull << 40) : 0ull);
                    if (best_vendor == 0 || score > best_score) {
                        best_score = score;
                        best_vendor = desc.VendorId;
                    }
                }
            }
            a.Reset();
            ++cnt;
        }
        if (best_vendor == 0x10DE) { codec = "h264_nvenc"; found_gpu = 1; }
        else if (best_vendor == 0x1002 || best_vendor == 0x1022) { codec = "h264_amf"; found_gpu = 1; }
        else if (best_vendor == 0x8086) { codec = "h264_qsv"; found_gpu = 1; }
    }
#endif
    if (out_buf && buf_chars > 0) {
        strncpy(out_buf, codec, (size_t)buf_chars - 1);
        out_buf[buf_chars - 1] = '\0';
    }
    return found_gpu;
}