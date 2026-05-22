#pragma once
/*
 * gpu_scaler.hpp — HomRec 2.0
 * GPU-based frame downscaler using D3D11 compute shaders.
 *
 * Why:
 *   CPU bilinear downscale (color_convert.c) costs ~2 ms at 1080p→720p.
 *   A D3D11 CS dispatched as a 16×16 tile takes < 0.2 ms on any modern GPU,
 *   and the result stays on the GPU — no PCIe round-trip until the encoder
 *   staging copy.
 *
 * Usage:
 *   GpuScaler scaler;
 *   scaler.init(device, context, 1920, 1080, 1280, 720);
 *   // each frame:
 *   ID3D11Texture2D* small = scaler.scale(src_texture);
 *   // small is valid until the next call to scale() or destroy().
 *   scaler.destroy();
 *
 * Thread safety:
 *   Must be used from a single thread (the capture thread).
 */

#include <d3d11.h>
#include <cstdint>

namespace homrec {

class GpuScaler {
public:
    GpuScaler() = default;
    ~GpuScaler() { destroy(); }

    // Non-copyable, non-movable (owns D3D11 resources)
    GpuScaler(const GpuScaler&)            = delete;
    GpuScaler& operator=(const GpuScaler&) = delete;

    // -- Lifecycle ---------------------------------------------------------

    // Compile the compute shader and allocate the output texture.
    // src_fmt: expected DXGI_FORMAT of the input texture (typically
    //          DXGI_FORMAT_B8G8R8A8_UNORM from DXGI duplication).
    bool init(ID3D11Device*        device,
              ID3D11DeviceContext* context,
              UINT src_w, UINT src_h,
              UINT dst_w, UINT dst_h,
              DXGI_FORMAT src_fmt = DXGI_FORMAT_B8G8R8A8_UNORM);

    void destroy() noexcept;

    // -- Per-frame ---------------------------------------------------------

    // Run the compute shader to scale src → internal output texture.
    // Returns a non-AddRef'd pointer to the output (valid until next call).
    // Returns nullptr if not initialised or on D3D11 error.
    ID3D11Texture2D* scale(ID3D11Texture2D* src) noexcept;

    // -- Info -------------------------------------------------------------

    bool     is_ready()  const noexcept { return ready_; }
    UINT     dst_width() const noexcept { return dst_w_; }
    UINT     dst_height()const noexcept { return dst_h_; }

private:
    // Compile a minimal HLSL compute shader at runtime.
    // We embed the HLSL source as a string to avoid needing fxc.exe at
    // build time; D3DCompile is always available on Windows 8.1+.
    bool compile_shader(ID3D11Device* device);

    ID3D11Device*              device_  = nullptr;
    ID3D11DeviceContext*       context_ = nullptr;
    ID3D11ComputeShader*       cs_      = nullptr;
    ID3D11ShaderResourceView*  srv_     = nullptr;  // view of src (rebuilt per frame)
    ID3D11UnorderedAccessView* uav_     = nullptr;  // view of dst
    ID3D11Texture2D*           dst_tex_ = nullptr;  // output texture (GPU-resident)
    ID3D11Buffer*              cb_      = nullptr;  // constant buffer: src/dst dims

    UINT src_w_ = 0, src_h_ = 0;
    UINT dst_w_ = 0, dst_h_ = 0;
    bool ready_ = false;

    // Inline HLSL for bilinear downscale CS.
    // Exposed here so tests can inspect it without linking the whole engine.
    static constexpr const char* kHLSL = R"hlsl(
        Texture2D<float4>   SrcTex : register(t0);
        RWTexture2D<float4> DstTex : register(u0);

        cbuffer ScaleParams : register(b0) {
            float2 inv_dst;   // 1/dst_w, 1/dst_h
            float2 scale;     // dst_w/src_w, dst_h/src_h  (unused, kept for alignment)
        };

        SamplerState LinearClamp : register(s0);

        [numthreads(16, 16, 1)]
        void CSMain(uint3 tid : SV_DispatchThreadID) {
            if (tid.x >= (uint)(1.0f / inv_dst.x) ||
                tid.y >= (uint)(1.0f / inv_dst.y)) return;

            // UV in [0,1] addressing the source texture
            float2 uv = (float2(tid.xy) + 0.5f) * inv_dst;
            DstTex[tid.xy] = SrcTex.SampleLevel(LinearClamp, uv, 0);
        }
    )hlsl";
};

} // namespace homrec
