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
struct DxCapCtx {
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D>        staging[2];
    int  write_idx    = 0;
    int  pending_idx  = -1;
    bool have_pending  = false;

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

    DxCapCtx() : adapter_idx(0), output_idx(0), width(0), height(0), acquired(false) {}

    /* Re-acquire duplication interface (needed after HR_DX_LOST) */
    HRESULT reset() {
        if (acquired) {
            duplication->ReleaseFrame();
            acquired = false;
        }
        duplication.Reset();
        staging[0].Reset();
        staging[1].Reset();
        write_idx = 0;
        pending_idx = -1;
        have_pending = false;

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
    hr = D3D11CreateDevice(
        adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,  /* BGRA needed for DXGI surface map */
        nullptr, 0,
        D3D11_SDK_VERSION,
        &ctx->device, &fl, &ctx->context);
    if (FAILED(hr)) { g_last_dx_error = hr; delete ctx; return nullptr; }

    // Ask the GPU scheduler to run our desktop-copy ahead of the game's own
    // queued work (OBS does the same).  Without this, CopyResource()/Map()
    // wait behind the game's frames, capture misses its deadline and frames
    // are dropped exactly when the GPU is busiest.  Priorities above 0 need
    // an elevated process; when refused this is a harmless no-op.
    {
        ComPtr<IDXGIDevice> gpu_dev;
        if (SUCCEEDED(ctx->device.As(&gpu_dev))) gpu_dev->SetGPUThreadPriority(7);
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
 * this in hr_pipeline.cpp.
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
 * BUGFIX (diagonal line/pixel corruption + the bottom slice of the frame
 * missing, seen recording fullscreen games -- e.g. older Source titles --
 * running at a lower exclusive-fullscreen resolution than the desktop,
 * such as 1366x768 on a 1600x900 desktop):
 *
 * The caller (hr_pipeline.cpp) allocates its bgra_buf once, at
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
#else
    (void)handle; (void)w; (void)h;
#endif
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

    /* Release any previously acquired frame */
    if (ctx->acquired) {
        ctx->duplication->ReleaseFrame();
        ctx->acquired = false;
    }

    /* Acquire next frame */
    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource>   res;
    HRESULT hr = ctx->duplication->AcquireNextFrame(
        (UINT)timeout_ms, &fi, &res);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)    return HR_DX_TIMEOUT;
    if (hr == DXGI_ERROR_ACCESS_LOST ||
        hr == DXGI_ERROR_INVALID_CALL)    return HR_DX_LOST;
    if (FAILED(hr))                        return HR_DX_ERROR;

    ctx->acquired = true;

    /* Copy GPU texture → this call's staging (CPU-accessible) texture */
    ComPtr<ID3D11Texture2D> gpu_tex;
    hr = res.As(&gpu_tex);
    if (FAILED(hr)) {
        ctx->duplication->ReleaseFrame(); ctx->acquired = false;
        return HR_DX_ERROR;
    }
    ctx->context->CopyResource(ctx->staging[ctx->write_idx].Get(), gpu_tex.Get());

    /* Release the desktop-duplication frame as soon as the copy is queued
     * rather than after Map()/Unmap() -- lets the next AcquireNextFrame()
     * proceed sooner, and CopyResource()'s destination keeps the data
     * alive regardless of when the source frame is released. */
    ctx->duplication->ReleaseFrame();
    ctx->acquired = false;

    /* Output the buffer copied on the *previous* call, not this one -- see
     * the staging[] comment on DxCapCtx for why. On the very first call
     * there's nothing previous to output yet, so report it the same way a
     * real DXGI timeout is reported; the capture loop already knows how to
     * carry the last frame forward in that case. */
    const int read_idx = ctx->pending_idx;
    const bool have_output = ctx->have_pending;
    ctx->pending_idx  = ctx->write_idx;
    ctx->have_pending = true;
    ctx->write_idx   ^= 1;

    if (!have_output) return HR_DX_TIMEOUT;

    const ULONGLONG kMapWaitBudgetMs = (ULONGLONG)std::max(timeout_ms, 8);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const ULONGLONG map_deadline = GetTickCount64() + kMapWaitBudgetMs;
    for (;;) {
        hr = ctx->context->Map(ctx->staging[read_idx].Get(), 0, D3D11_MAP_READ,
                                D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr != DXGI_ERROR_WAS_STILL_DRAWING) break;
        if (GetTickCount64() >= map_deadline) return HR_DX_TIMEOUT;
        Sleep(1);
    }
    if (FAILED(hr)) return HR_DX_ERROR;

    // Target shape the caller's buffer actually has room for -- see the
    // hr_dx_set_output_size() comment above. Falls back to the real
    // captured size (old behaviour) when nobody's called it.
    const int tgt_w = (ctx->out_w  > 0) ? ctx->out_w  : ctx->width;
    const int tgt_h = (ctx->out_h  > 0) ? ctx->out_h  : ctx->height;
    const int dst_row_bytes = tgt_w * 4;
    const uint8_t *src = reinterpret_cast<const uint8_t *>(mapped.pData);
    uint8_t       *dst = out_bgra;

    if (tgt_w == ctx->width && tgt_h == ctx->height) {
        // Common case: nothing's mismatched, straight row copy.
        for (int y = 0; y < tgt_h; ++y) {
            memcpy(dst, src + (size_t)y * mapped.RowPitch, (size_t)dst_row_bytes);
            dst += dst_row_bytes;
        }
    } else {
        const float rx = (float)ctx->width  / (float)tgt_w;
        const float ry = (float)ctx->height / (float)tgt_h;
        for (int y = 0; y < tgt_h; ++y) {
            int sy = (int)(y * ry);
            if (sy >= ctx->height) sy = ctx->height - 1;
            const uint8_t *srow = src + (size_t)sy * mapped.RowPitch;
            uint32_t *drow = reinterpret_cast<uint32_t *>(dst);
            for (int x = 0; x < tgt_w; ++x) {
                int sx = (int)(x * rx);
                if (sx >= ctx->width) sx = ctx->width - 1;
                drow[x] = reinterpret_cast<const uint32_t *>(srow)[sx];
            }
            dst += dst_row_bytes;
        }
    }

    ctx->context->Unmap(ctx->staging[read_idx].Get(), 0);
    return HR_DX_OK;
#else
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