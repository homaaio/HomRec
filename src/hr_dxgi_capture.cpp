#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <d3d11.h>
  #include <dxgi1_2.h>
  #include <wrl/client.h>   /* ComPtr */
  #pragma comment(lib, "d3d11.lib")
  #pragma comment(lib, "dxgi.lib")

  using Microsoft::WRL::ComPtr;
#endif

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>

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

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */
#ifdef _WIN32
struct DxCapCtx {
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D>        staging;   /* CPU-readable shadow */

    int  adapter_idx;
    int  output_idx;
    int  width;
    int  height;
    bool acquired;

    DxCapCtx() : adapter_idx(0), output_idx(0), width(0), height(0), acquired(false) {}

    /* Re-acquire duplication interface (needed after HR_DX_LOST) */
    HRESULT reset() {
        if (acquired) {
            duplication->ReleaseFrame();
            acquired = false;
        }
        duplication.Reset();
        staging.Reset();

        ComPtr<IDXGIDevice> dxgi_dev;
        HRESULT hr = device.As(&dxgi_dev);
        if (FAILED(hr)) return hr;

        ComPtr<IDXGIAdapter> adapter;
        hr = dxgi_dev->GetAdapter(&adapter);
        if (FAILED(hr)) return hr;

        ComPtr<IDXGIOutput> output;
        hr = adapter->EnumOutputs(output_idx, &output);
        if (FAILED(hr)) return hr;

        ComPtr<IDXGIOutput1> output1;
        hr = output.As(&output1);
        if (FAILED(hr)) return hr;

        hr = output1->DuplicateOutput(device.Get(), &duplication);
        if (FAILED(hr)) return hr;

        /* Get fresh size from output desc */
        DXGI_OUTDUPL_DESC dd;
        duplication->GetDesc(&dd);
        width  = (int)dd.ModeDesc.Width;
        height = (int)dd.ModeDesc.Height;

        /* (Re)create staging texture */
        D3D11_TEXTURE2D_DESC td{};
        td.Width          = (UINT)width;
        td.Height         = (UINT)height;
        td.MipLevels      = 1;
        td.ArraySize      = 1;
        td.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc     = {1, 0};
        td.Usage          = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return device->CreateTexture2D(&td, nullptr, &staging);
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
    if (FAILED(hr)) { delete ctx; return nullptr; }

    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapters1((UINT)adapter_idx, &adapter);
    if (FAILED(hr)) { delete ctx; return nullptr; }

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
    if (FAILED(hr)) { delete ctx; return nullptr; }

    hr = ctx->reset();
    if (FAILED(hr)) { delete ctx; return nullptr; }

    return ctx;
#else
    return nullptr;
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
 * hr_dx_capture
 * Grabs one frame into caller-allocated BGRA buffer.
 * ---------------------------------------------------------------------- */
HR_EXPORT int hr_dx_capture(void *handle, uint8_t *out_bgra, int timeout_ms) {
#ifdef _WIN32
    if (!handle || !out_bgra) return HR_DX_ERROR;
    auto *ctx = static_cast<DxCapCtx *>(handle);

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

    /* Copy GPU texture → staging (CPU-accessible) texture */
    ComPtr<ID3D11Texture2D> gpu_tex;
    hr = res.As(&gpu_tex);
    if (FAILED(hr)) {
        ctx->duplication->ReleaseFrame(); ctx->acquired = false;
        return HR_DX_ERROR;
    }
    ctx->context->CopyResource(ctx->staging.Get(), gpu_tex.Get());

    /* Map staging texture → read pixels */
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = ctx->context->Map(ctx->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        ctx->duplication->ReleaseFrame(); ctx->acquired = false;
        return HR_DX_ERROR;
    }

    /* Copy row-by-row: D3D textures may have row padding (RowPitch != width*4) */
    const int row_bytes  = ctx->width * 4;
    const uint8_t *src   = reinterpret_cast<const uint8_t *>(mapped.pData);
    uint8_t       *dst   = out_bgra;
    for (int y = 0; y < ctx->height; ++y) {
        memcpy(dst, src, (size_t)row_bytes);
        src += mapped.RowPitch;
        dst += row_bytes;
    }

    ctx->context->Unmap(ctx->staging.Get(), 0);
    ctx->duplication->ReleaseFrame();
    ctx->acquired = false;
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