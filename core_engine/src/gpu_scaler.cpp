/*
 * gpu_scaler.cpp — HomRec 2.0
 * D3D11 compute-shader bilinear downscaler.
 */

#include "gpu_scaler.hpp"
#include <d3dcompiler.h>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")

namespace homrec {

// -- init() -------------------------------------------------------------------

bool GpuScaler::init(ID3D11Device*        device,
                     ID3D11DeviceContext* context,
                     UINT src_w, UINT src_h,
                     UINT dst_w, UINT dst_h,
                     DXGI_FORMAT /*src_fmt*/)
{
    destroy();

    device_  = device;
    context_ = context;
    src_w_ = src_w; src_h_ = src_h;
    dst_w_ = dst_w; dst_h_ = dst_h;

    // -- Compile compute shader --------------------------------------------
    if (!compile_shader(device)) return false;

    // -- Output texture (GPU-resident, UAV-capable) ------------------------
    D3D11_TEXTURE2D_DESC td{};
    td.Width          = dst_w;
    td.Height         = dst_h;
    td.MipLevels      = 1;
    td.ArraySize      = 1;
    td.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc     = { 1, 0 };
    td.Usage          = D3D11_USAGE_DEFAULT;
    td.BindFlags      = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &dst_tex_))) return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.Format        = td.Format;
    uavd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateUnorderedAccessView(dst_tex_, &uavd, &uav_))) return false;

    // -- Constant buffer: scale params -------------------------------------
    struct CB { float inv_dst[2]; float scale[2]; };
    CB cbdata = {
        { 1.0f / (float)dst_w, 1.0f / (float)dst_h },
        { (float)dst_w / (float)src_w, (float)dst_h / (float)src_h }
    };
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(CB);
    bd.Usage          = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{ &cbdata, 0, 0 };
    if (FAILED(device->CreateBuffer(&bd, &sd, &cb_))) return false;

    ready_ = true;
    return true;
}

// -- destroy() ----------------------------------------------------------------

void GpuScaler::destroy() noexcept {
    if (srv_)     { srv_->Release();     srv_     = nullptr; }
    if (uav_)     { uav_->Release();     uav_     = nullptr; }
    if (dst_tex_) { dst_tex_->Release(); dst_tex_ = nullptr; }
    if (cs_)      { cs_->Release();      cs_      = nullptr; }
    if (cb_)      { cb_->Release();      cb_      = nullptr; }
    ready_ = false;
}

// -- scale() ------------------------------------------------------------------

ID3D11Texture2D* GpuScaler::scale(ID3D11Texture2D* src) noexcept {
    if (!ready_ || !src) return nullptr;

    // Build a fresh SRV pointing at this frame's texture
    if (srv_) { srv_->Release(); srv_ = nullptr; }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format              = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(device_->CreateShaderResourceView(src, &srvd, &srv_)))
        return nullptr;

    // Bind and dispatch
    context_->CSSetShader(cs_, nullptr, 0);
    context_->CSSetShaderResources(0, 1, &srv_);
    context_->CSSetUnorderedAccessViews(0, 1, &uav_, nullptr);
    context_->CSSetConstantBuffers(0, 1, &cb_);

    // Dispatch enough 16×16 groups to cover the output
    UINT gx = (dst_w_ + 15) / 16;
    UINT gy = (dst_h_ + 15) / 16;
    context_->Dispatch(gx, gy, 1);

    // Unbind UAV (required before using dst_tex_ as SRV elsewhere)
    ID3D11UnorderedAccessView* null_uav = nullptr;
    context_->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);

    return dst_tex_;
}

// -- compile_shader() ---------------------------------------------------------

bool GpuScaler::compile_shader(ID3D11Device* device) {
    ID3DBlob* blob  = nullptr;
    ID3DBlob* errs  = nullptr;

    HRESULT hr = D3DCompile(
        kHLSL, strlen(kHLSL),
        "GpuScaler",            // source name (for error messages)
        nullptr,                 // no macros
        nullptr,                 // no includes
        "CSMain",               // entry point
        "cs_5_0",               // shader model
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &blob, &errs);

    if (errs) errs->Release();
    if (FAILED(hr) || !blob) return false;

    hr = device->CreateComputeShader(
        blob->GetBufferPointer(),
        blob->GetBufferSize(),
        nullptr, &cs_);
    blob->Release();
    return SUCCEEDED(hr);
}

} // namespace homrec
