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
  #define HR_GPU_OVERLAY_INTERNAL
#endif

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <memory>
#include "gpu_overlay.h"   // item 9: overlays blended on the GPU (HrGpuLayer / HrGpuOverlay)

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
// Axis-aligned rectangle [l,r) x [t,b) used for dirty-region bookkeeping.
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

// Every this-many *changed* frames one full-frame copy is forced.
static constexpr int kFullRefreshEvery = 120;

/* GPU scheduler priority of the capture D3D11 device, -7..7 (0 = normal).
 * See hr_dx_create() for why this is no longer 7. */
static constexpr int kCaptureGpuPriority = 0;

struct DxCapCtx {
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;
    ComPtr<IDXGIOutputDuplication> duplication;

    ComPtr<ID3D11Texture2D> staging[2];
    // Per staging texture: the region that may differ from the *latest* desktop image (union
    // of every changed-rect since that texture was last brought up to date).
    DxBox stale[2];
    /* Per staging texture: the region that changed between the frame
     * before it and the frame it currently holds. */
    DxBox slot_box[2];
    int  write_idx    = 0;
    int  pending_idx  = -1;
    bool have_pending = false;

    // Region of the CALLER's BGRA buffer that is known to lag behind the frame we're about to
    // hand out (frames that were dropped, or consumed by the GPU path instead).
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

    /* ---- in-process encoder hand-off (Media Foundation, see hr_dx_capture_nv12_tex) ----------
     * Instead of reading the NV12 picture back to system memory, the finished picture is copied
     * (GPU -> GPU) into one of these textures and the texture itself goes to the encoder. A texture
     * is free again once nobody but this pool holds a reference to it (the encoder / sample / the
     * pipeline's queue each hold one while they still need the picture). */
    static constexpr int kMfPool = 10;
    ComPtr<ID3D11Texture2D> mf_pool[kMfPool];
    int  mf_pool_count = 0;
    int  mf_pool_next  = 0;
    bool mf_picture_pending = false;    /* nv12_tex holds a picture that could not be handed out yet (pool busy) */
    bool mf_have_picture    = false;    /* nv12_tex holds at least one finished picture */
    /* Preview thumbnail while the texture path is active: a 2-slot staging ring read back at preview rate. */
    int  pv_slot = 0, pv_pending = -1;
    bool pv_have_pending = false;
    bool mt_protected = false;

    /* ---- item 9: overlays blended on the GPU --------------------------------------------------
     * With layers set (hr_dx_gpu_overlay_set) vp_blt() first copies the part of desk_tex it is going to read
     * into comp_tex, blends the layers onto that copy (pixel shader, gpu_overlay.cpp) and feeds comp_tex to the
     * video processor instead of desk_tex. desk_tex itself is never drawn on: it is the persistent desktop
     * image that partial (dirty-rect) updates keep patching. */
    std::unique_ptr<HrGpuOverlay>             gov;
    bool                                      gov_layers = false;   /* the current list is non-empty            */
    bool                                      ov_dirty   = false;   /* picture owed to the caller differs now   */
    ComPtr<ID3D11Texture2D>                   comp_tex;
    ComPtr<ID3D11RenderTargetView>            comp_rtv;
    ComPtr<ID3D11VideoProcessorInputView>     vin_comp;

    int  adapter_idx;
    int  output_idx;
    int  width;
    int  height;
    bool acquired;

    // Fixed output shape the CALLER's buffer (and, downstream, ffmpeg's
    // rawvideo pipe) was sized for -- set once via hr_dx_set_output_size()
    int  out_w = 0;
    int  out_h = 0;

    /* Column lookup table for the CPU stretch in cpu_deliver() (rebuilt only
     * when the source/destination widths change). */
    std::vector<int> xmap;
    int  xmap_src_w = 0, xmap_dst_w = 0;

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

    // Dirty + move rectangles of the currently acquired frame, folded into one bounding box.
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
        vin_comp.Reset(); comp_rtv.Reset(); comp_tex.Reset();
        ov_dirty = true;
        vctx.Reset(); vdev.Reset();
        nv12_tex.Reset(); nv_stage[0].Reset(); nv_stage[1].Reset();
        desk_tex.Reset();
        desk_valid = false;
        nv_have_pending = false; nv_pending = -1; nv_write = 0;
        nv_out_valid = false;    nv_rect_valid = false;
        for (auto &t : mf_pool) t.Reset();
        mf_pool_count = 0; mf_pool_next = 0;
        mf_picture_pending = false; mf_have_picture = false;
        pv_slot = 0; pv_pending = -1; pv_have_pending = false;
    }

    // (Re)creates every GPU-side object for the current desktop size and NV12 output size.
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
        // OPTIMAL_SPEED asks the driver for its cheapest video-processing mode (no extra
        // filtering/rate-conversion stages) - this blit runs on the same GPU the game is rendering
        // on, so every microsecond of it is a microsecond the game doesn't get.
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

    /* The work texture the overlays are blended into (same size / format as desk_tex) + its video-processor
     * input view. Created on first use. */
    bool make_comp() {
        if (comp_tex && comp_rtv && vin_comp) return true;
        comp_tex.Reset(); comp_rtv.Reset(); vin_comp.Reset();
        if (!device || !vdev || !venum || width <= 0 || height <= 0) return false;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)width; td.Height = (UINT)height;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc = {1, 0};
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &comp_tex))) return false;
        if (FAILED(device->CreateRenderTargetView(comp_tex.Get(), nullptr, &comp_rtv))) { comp_tex.Reset(); return false; }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
        iv.FourCC = 0;
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (FAILED(vdev->CreateVideoProcessorInputView(comp_tex.Get(), venum.Get(), &iv, &vin_comp))) {
            comp_rtv.Reset(); comp_tex.Reset(); return false;
        }
        return true;
    }

    /* desk_tex (+ layers) -> comp_tex for the rectangle `src`. False = nothing was drawn (use desk_tex). */
    bool ov_render(const RECT &src) {
        if (!gov_layers || !gov || !gov->Usable() || !desk_tex) return false;
        if (!make_comp()) return false;
        return gov->Render(desk_tex.Get(), comp_tex.Get(), comp_rtv.Get(), width, height, src);
    }

    /* Runs the video processor: crop `src` out of desk_tex, scale it to
     * nv_w x nv_h and convert to NV12 into nv12_tex. */
    bool vp_blt(const RECT &src_in) {
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
        if (ov_render(src)) st.pInputSurface = vin_comp.Get();   /* item 9: desktop + overlays */
        ov_dirty = false;                                        /* this picture shows the current layers */
        HRESULT hr = vctx->VideoProcessorBlt(vproc.Get(), vout.Get(), 0, 1, &st);
        return SUCCEEDED(hr);
    }

    /* vp_blt() + queue a copy into nv_stage[slot] (CPU read-back path). */
    bool nv_blit(const RECT &src_in, int slot) {
        if (!vp_blt(src_in)) return false;
        context->CopyResource(nv_stage[slot].Get(), nv12_tex.Get());
        return true;
    }

    /* Creates the texture pool (lazily, on first use). */
    bool mf_pool_make() {
        if (mf_pool_count > 0) return true;
        if (!device || nv_w < 2 || nv_h < 2) return false;
        D3D11_TEXTURE2D_DESC nd{};
        nd.Width = (UINT)nv_w; nd.Height = (UINT)nv_h;
        nd.MipLevels = 1; nd.ArraySize = 1;
        nd.Format = DXGI_FORMAT_NV12;
        nd.SampleDesc = {1, 0};
        nd.Usage = D3D11_USAGE_DEFAULT;
        /* The flags an encoder MFT accepts differ between drivers: try the richest set first. */
        const UINT binds[3] = { D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_BIND_RENDER_TARGET, 0 };
        for (UINT b : binds) {
            nd.BindFlags = b;
            int made = 0;
            for (int i = 0; i < kMfPool; ++i) {
                if (FAILED(device->CreateTexture2D(&nd, nullptr, &mf_pool[i]))) break;
                ++made;
            }
            if (made >= 3) { mf_pool_count = made; mf_pool_next = 0; return true; }
            for (auto &t : mf_pool) t.Reset();
        }
        return false;
    }
    /* A pool texture nobody else references (see kMfPool), or null when all are still in use. */
    ID3D11Texture2D *mf_pool_take() {
        if (!mf_pool_make()) return nullptr;
        for (int k = 0; k < mf_pool_count; ++k) {
            const int i = (mf_pool_next + k) % mf_pool_count;
            ID3D11Texture2D *t = mf_pool[i].Get();
            t->AddRef();
            const ULONG rc = t->Release();      /* references held by everybody else + this pool's one */
            if (rc == 1) { mf_pool_next = (i + 1) % mf_pool_count; return t; }
        }
        return nullptr;
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
 * Multithread protection (needed once the in-process encoder shares this device)
 * ---------------------------------------------------------------------- */
#ifdef _WIN32
/* ID3D10Multithread, declared here so this file does not depend on d3d10.h being present. Layout and
 * IID are the documented ones; the encoder MFT calls into the same device from its own threads, so
 * the device must be multithread-protected. */
struct HrD3D10Multithread : public IUnknown {
    virtual void  STDMETHODCALLTYPE Enter() = 0;
    virtual void  STDMETHODCALLTYPE Leave() = 0;
    virtual BOOL  STDMETHODCALLTYPE SetMultithreadProtected(BOOL protect) = 0;
    virtual BOOL  STDMETHODCALLTYPE GetMultithreadProtected() = 0;
};
static const GUID kIID_HrD3D10Multithread =
    {0x9b7e4e00, 0x342c, 0x4106, {0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0}};


/* Set (hr_dx_set_mt_protect) BEFORE a capture context is created: every device created afterwards is
 * switched to multithread-protected mode. It is deliberately NOT toggled on a device that is already
 * in use - the capture thread may be inside a D3D call at that moment. */
static volatile LONG g_mt_protect_req = 0;
#endif
HR_EXPORT void hr_dx_set_mt_protect(int enabled) {
#ifdef _WIN32
    InterlockedExchange(&g_mt_protect_req, enabled ? 1 : 0);
#else
    (void)enabled;
#endif
}

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
    if (g_mt_protect_req) {
        HrD3D10Multithread *mt = nullptr;
        if (SUCCEEDED(ctx->device->QueryInterface(kIID_HrD3D10Multithread, reinterpret_cast<void **>(&mt))) && mt) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
            ctx->mt_protected = true;
        }
    }

    hr = ctx->reset();
    if (FAILED(hr)) { delete ctx; return nullptr; } // reset() already set g_last_dx_error

    g_last_dx_error = S_OK;
    return ctx;
#else
    return nullptr;
#endif
}

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

// hr_dx_set_output_size - (diagonal line/pixel corruption + the bottom slice of the frame
// missing, seen recording fullscreen games -- e.g.
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

// Frame acquisition shared by the CPU (BGRA) and GPU (NV12) paths.
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

    // Pointer-only update: LastPresentTime == 0 AND AccumulatedFrames == 0.
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

// CPU path: hand staging[slot] to the caller's BGRA buffer.
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

        if (c->xmap_src_w != W || c->xmap_dst_w != tgt_w) {
            c->xmap.resize((size_t)tgt_w);
            const float rx = (float)W / (float)tgt_w;
            for (int x = 0; x < tgt_w; ++x) {
                int sx = (int)(x * rx);
                c->xmap[(size_t)x] = sx >= W ? W - 1 : sx;
            }
            c->xmap_src_w = W;
            c->xmap_dst_w = tgt_w;
        }
        const int *xm = c->xmap.data();
        const float ry = (float)H / (float)tgt_h;
        uint8_t *dst = out;
        int prev_sy = -1;
        for (int y = 0; y < tgt_h; ++y) {
            int sy = (int)(y * ry);
            if (sy >= H) sy = H - 1;
            if (sy == prev_sy) {
                memcpy(dst, dst - dst_row_bytes, (size_t)dst_row_bytes);
            } else {
                const uint32_t *srow = reinterpret_cast<const uint32_t *>(src + (size_t)sy * mapped.RowPitch);
                uint32_t *drow = reinterpret_cast<uint32_t *>(dst);
                for (int x = 0; x < tgt_w; ++x) drow[x] = srow[xm[x]];
                prev_sy = sy;
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
        // FLUSH: the newest real frame is still sitting in the second staging texture because
        // delivery is deliberately one call behind (so Map() never stalls on the copy it just
        // queued).
        int r = cpu_deliver(c, c->pending_idx, out, false, timeout_ms);
        if (r == HR_DX_OK) { c->have_pending = false; return HR_DX_OK; }
        return r;
    }
    // Nothing pending, but the caller's buffer may still be out of date (frames were consumed
    // by the GPU/NV12 path meanwhile).
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

    // Release the desktop-duplication frame as soon as the copy is queued
    // rather than after Map()/Unmap() -- lets the next AcquireNextFrame()
    dx_release(c);

    /* Output the buffer copied on the *previous* call, not this one -- see
     * the staging[] comment on DxCapCtx for why. */
    const int read_idx = c->pending_idx;
    const bool have_output = c->have_pending;
    c->pending_idx  = w;
    c->have_pending = true;
    c->write_idx   ^= 1;

    if (!have_output) {
        // Nothing older to hand out.
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

// hr_dx_capture
// Grabs one frame into caller-allocated BGRA buffer.
HR_EXPORT int hr_dx_capture(void *handle, uint8_t *out_bgra, int timeout_ms) {
#ifdef _WIN32
    if (!handle || !out_bgra) return HR_DX_ERROR;
    auto *ctx = static_cast<DxCapCtx *>(handle);

    // CRASHFIX (0xC0000005 null-pointer read a few ms after "DXGI capture lost -- resetting",
    // nothing else logged in between - matches the supplied crash dump exactly: exception in
    // hr.exe itself, read access violation at address 0x0).
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
    if (ctx->gpu_enabled) {
        ctx->nv_have_pending = false; ctx->nv_out_valid = false;
        /* ...and nv12_tex is no longer the latest picture (texture path: re-render it from the desktop copy). */
        ctx->mf_have_picture = false; ctx->mf_picture_pending = false; ctx->pv_have_pending = false;
    }

    return cpu_new_frame(ctx, tex.Get(), box, out_bgra, timeout_ms);
#else
    (void)handle; (void)out_bgra; (void)timeout_ms;
    return HR_DX_ERROR;
#endif
}

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

// GPU PATH: D3D11 Video Processor, BGRA -> NV12 (+ crop + scale)

// Enable (out_w/out_h > 0) or disable (0/0) the GPU path.
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

    // Seed the desktop copy from the caller's buffer - only if that buffer is provably the
    // latest picture (fully synced, nothing pending, and exactly the desktop's size).
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

/* Item 9 - overlays blended on the GPU.
 * Sets the layer list for the NEXT captures made with hr_dx_capture_nv12() / hr_dx_capture_nv12_tex(): until the
 * list is replaced every picture they produce has the layers blended onto the desktop BEFORE the video
 * processor scales / converts it (so coordinates are desktop pixels, see HrGpuLayer). Call it once per capture
 * tick, before the capture; count 0 clears the list. Bitmaps are uploaded to the GPU only when their `gen`
 * changed; a changed list marks the picture "dirty", so a static desktop with an animated overlay still delivers
 * a new picture (see nv_idle / mf_idle).
 * Returns 1 = ok (also for count 0), 0 = not available / failed - the caller must draw the overlays itself. */
HR_EXPORT int hr_dx_gpu_overlay_set(void *handle, const HrGpuLayer *layers, int count) {
#ifdef _WIN32
    if (!handle) return 0;
    auto *ctx = static_cast<DxCapCtx *>(handle);
    if (count <= 0 || !layers) {
        if (ctx->gov && ctx->gov_layers) {
            ctx->gov->SetLayers(nullptr, 0);
            ctx->gov_layers = false;
            ctx->ov_dirty = true;
        }
        return 1;
    }
    if (!ctx->gpu_enabled || !ctx->vp_ready || !ctx->device || !ctx->context) return 0;
    if (!ctx->gov) {
        try { ctx->gov.reset(new HrGpuOverlay()); } catch (...) { return 0; }
        HrD3D10Multithread *mt = nullptr;
        if (ctx->mt_protected)
            ctx->device->QueryInterface(kIID_HrD3D10Multithread, reinterpret_cast<void **>(&mt));
        ctx->gov->Setup(ctx->device.Get(), ctx->context.Get(), mt);   /* takes its own reference */
        if (mt) mt->Release();
    }
    const int r = ctx->gov->SetLayers(layers, count);
    if (r < 0) {
        ctx->gov_layers = false;
        ctx->ov_dirty = true;       /* whatever was drawn before must disappear from the next picture */
        return 0;
    }
    ctx->gov_layers = ctx->gov->HasLayers();
    if (r == 1) ctx->ov_dirty = true;
    return 1;
#else
    (void)handle; (void)layers; (void)count;
    return 0;
#endif
}

/* Copy an NV12 staging texture into the caller's tightly packed buffer (Y plane nv_w*nv_h, then the
 * interleaved UV plane nv_w*nv_h/2). single_try: one non-blocking Map, HR_DX_TIMEOUT if the GPU is
 * still working on it (used by the preview read-back, which must never stall the capture loop). */
static int nv_read_stage(DxCapCtx *c, ID3D11Texture2D *stage, uint8_t *out, bool wait, int timeout_ms,
                          bool single_try) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (single_try) {
        HRESULT hr = c->context->Map(stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return HR_DX_TIMEOUT;
        if (FAILED(hr)) return HR_DX_ERROR;
    } else {
        int mr = dx_map(c, stage, wait, timeout_ms, mapped);
        if (mr != HR_DX_OK) return mr;
    }

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

    c->context->Unmap(stage, 0);
    return HR_DX_OK;
}

/* Hand staging NV12 texture `slot` to the caller. */
static int nv_deliver(DxCapCtx *c, int slot, uint8_t *out, bool wait, int timeout_ms) {
    int r = nv_read_stage(c, c->nv_stage[slot].Get(), out, wait, timeout_ms, false);
    if (r != HR_DX_OK) return r;
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
    if ((rect_changed || !c->nv_out_valid || c->ov_dirty) && c->desk_valid) {
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
    ctx->pv_have_pending = false;   /* the texture path's preview ring shares nv_stage: forget it */
    ctx->mf_picture_pending = false;

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

    // The CPU ring did not see this frame: its staging textures are now behind by `box`, its
    // pending frame is obsolete, and the caller's BGRA buffer no longer matches the desktop.
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
 * In-process encoder hand-off (Media Foundation) - GPU texture instead of a CPU read-back
 * ---------------------------------------------------------------------- */
#ifdef _WIN32
/* The picture in nv12_tex -> a free pool texture (GPU copy). HR_DX_TIMEOUT when every pool texture is
 * still referenced; the picture then stays in nv12_tex and is handed out by a later call. */
static int mf_hand_out(DxCapCtx *c, void **out_tex) {
    ID3D11Texture2D *t = c->mf_pool_take();
    if (!t) { c->mf_picture_pending = true; return HR_DX_TIMEOUT; }
    c->context->CopyResource(t, c->nv12_tex.Get());
    t->AddRef();                       /* the reference the caller owns (released by whoever consumes it) */
    *out_tex = t;
    c->mf_picture_pending = false;
    ++c->st_gpu_frames;
    return HR_DX_OK;
}

static int mf_idle(DxCapCtx *c, const RECT &rect, void **out_tex) {
    if (c->mf_picture_pending && c->mf_have_picture) return mf_hand_out(c, out_tex);
    const bool rect_changed = !c->nv_rect_valid || memcmp(&rect, &c->nv_last_rect, sizeof(RECT)) != 0;
    if ((rect_changed || !c->mf_have_picture || c->ov_dirty) && c->desk_valid) {
        /* Static desktop, but the picture we owe the caller changed (its crop rectangle moved) or none
         * was produced yet: render it again from the persistent desktop copy. */
        if (!c->vp_blt(rect)) return HR_DX_ERROR;
        c->nv_last_rect = rect; c->nv_rect_valid = true;
        c->mf_have_picture = true;
        return mf_hand_out(c, out_tex);
    }
    return HR_DX_TIMEOUT;
}
#endif

/* The D3D11 device the capture runs on (no reference added; valid until hr_dx_destroy). The in-process
 * encoder is built on the SAME device so textures can be passed without a copy between devices.
 * Returns null when the device was not created multithread-protected (hr_dx_set_mt_protect). */
HR_EXPORT void *hr_dx_get_d3d_device(void *handle) {
#ifdef _WIN32
    if (!handle) return nullptr;
    auto *ctx = static_cast<DxCapCtx *>(handle);
    /* Only a device that was created multithread-protected may be shared (see g_mt_protect_req). */
    if (!ctx->device || !ctx->mt_protected) return nullptr;
    return ctx->device.Get();
#else
    (void)handle;
    return nullptr;
#endif
}

/* Like hr_dx_capture_nv12(), but the finished NV12 picture is NOT read back: on HR_DX_OK *out_tex is an
 * ID3D11Texture2D* (AddRef'd - the caller must Release() it) that holds the picture. HR_DX_TIMEOUT = no
 * new picture (static desktop, or all pool textures busy), HR_DX_LOST / HR_DX_ERROR as usual. */
HR_EXPORT int hr_dx_capture_nv12_tex(void *handle, int timeout_ms,
                                      int src_l, int src_t, int src_r, int src_b, void **out_tex) {
#ifdef _WIN32
    if (!handle || !out_tex) return HR_DX_ERROR;
    *out_tex = nullptr;
    auto *ctx = static_cast<DxCapCtx *>(handle);
    if (!ctx->duplication) return HR_DX_LOST;
    if (!ctx->gpu_enabled || !ctx->vp_ready) return HR_DX_ERROR;
    /* The CPU read-back state (caller's NV12 buffer, pending staging slot) is stale after any
     * texture-path call; it is rebuilt from the desktop copy if the caller switches back. */
    ctx->nv_out_valid = false;
    ctx->nv_have_pending = false;
    ctx->nv_pending = -1;

    const RECT rect{src_l, src_t, src_r, src_b};
    ComPtr<ID3D11Texture2D> tex;
    DxBox box;
    switch (dx_acquire(ctx, timeout_ms, tex, box)) {
        case DxAcq::Timeout:
        case DxAcq::Idle:  return mf_idle(ctx, rect, out_tex);
        case DxAcq::Lost:  return HR_DX_LOST;
        case DxAcq::Error: return HR_DX_ERROR;
        case DxAcq::New:   break;
    }

    ctx->update_desktop_copy(tex.Get(), box);
    dx_release(ctx);

    /* Same bookkeeping as hr_dx_capture_nv12(): the CPU BGRA ring did not see this frame. */
    ctx->stale[0].add(box);
    ctx->stale[1].add(box);
    ctx->have_pending = false;
    ctx->out_carry.set_full(ctx->width, ctx->height);

    if (!ctx->vp_blt(rect)) return HR_DX_ERROR;
    ctx->nv_last_rect = rect; ctx->nv_rect_valid = true;
    ctx->mf_have_picture = true;
    return mf_hand_out(ctx, out_tex);
#else
    (void)handle; (void)timeout_ms; (void)src_l; (void)src_t; (void)src_r; (void)src_b;
    if (out_tex) *out_tex = nullptr;
    return HR_DX_ERROR;
#endif
}

/* Live-preview thumbnail source while the texture path is active (nothing is read back otherwise).
 * Call at preview rate. Delivers the picture queued by the PREVIOUS call into out_nv12 (tightly packed,
 * nv_w x nv_h) when the GPU has finished with it - never blocks - and, if queue_new, queues the latest
 * finished picture for the next call. Returns 1 when out_nv12 was refreshed. */
HR_EXPORT int hr_dx_nv12_preview(void *handle, uint8_t *out_nv12, int queue_new) {
#ifdef _WIN32
    if (!handle || !out_nv12) return 0;
    auto *ctx = static_cast<DxCapCtx *>(handle);
    if (!ctx->gpu_enabled || !ctx->vp_ready || !ctx->nv_stage[0] || !ctx->nv_stage[1]) return 0;
    int got = 0;
    if (ctx->pv_have_pending && ctx->pv_pending >= 0) {
        if (nv_read_stage(ctx, ctx->nv_stage[ctx->pv_pending].Get(), out_nv12, false, 0, /*single_try=*/true) == HR_DX_OK) {
            ctx->pv_have_pending = false;
            got = 1;
        }
    }
    if (queue_new && ctx->mf_have_picture && !ctx->pv_have_pending) {
        const int w = ctx->pv_slot;
        ctx->context->CopyResource(ctx->nv_stage[w].Get(), ctx->nv12_tex.Get());
        ctx->pv_pending = w;
        ctx->pv_have_pending = true;
        ctx->pv_slot ^= 1;
    }
    return got;
#else
    (void)handle; (void)out_nv12; (void)queue_new;
    return 0;
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

#ifdef _WIN32
static volatile LONG g_gpu_sched_last_status = 0;
#endif

HR_EXPORT long hr_gpu_sched_last_status() {
#ifdef _WIN32
    return (long)g_gpu_sched_last_status;
#else
    return 0;
#endif
}

HR_EXPORT int hr_gpu_sched_set_process(void *hProcess, int level) {
#ifdef _WIN32
    typedef LONG (WINAPI *PFN_SetProcPrioClass)(HANDLE, int);
    static PFN_SetProcPrioClass fn = []() -> PFN_SetProcPrioClass {
        HMODULE g = GetModuleHandleW(L"gdi32.dll");
        if (!g) g = LoadLibraryW(L"gdi32.dll");
        return g ? reinterpret_cast<PFN_SetProcPrioClass>(
                       reinterpret_cast<void *>(GetProcAddress(g, "D3DKMTSetProcessSchedulingPriorityClass")))
                 : nullptr;
    }();
    if (!fn) { InterlockedExchange(&g_gpu_sched_last_status, (LONG)0xC0000002L /* STATUS_NOT_IMPLEMENTED */); return 0; }
    if (level < 0) level = 0;
    if (level > 2) level = 2;   /* never RAISE above normal from here (needs admin, and not the point) */
    HANDLE h = hProcess ? static_cast<HANDLE>(hProcess) : GetCurrentProcess();
    const LONG st = fn(h, level);
    InterlockedExchange(&g_gpu_sched_last_status, st);
    return st >= 0 ? 1 : 0;   /* NTSTATUS: >= 0 is success */
#else
    (void)hProcess; (void)level;
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

// hr_dx_recommend_codec - First-launch-only helper (see main_frame.cpp's `first_launch` branch):
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
