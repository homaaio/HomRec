#pragma once
// gpu_overlay.h - overlay layers alpha-blended on the GPU (D3D11 pixel shader) instead of the CPU
// ApplyNv12() pass. The layers are the same cached BGRA bitmaps OverlayCompositor already renders (text,
// image, gif, webcam, input overlay); they are uploaded to small GPU textures only when their content changes
// and are blended onto a GPU copy of the desktop BEFORE the video processor converts / scales it to NV12.
#include <cstdint>

// One layer, in the pixel space of the DESKTOP texture. `bgra` is only read during hr_dx_gpu_overlay_set().
struct HrGpuLayer {
    int            key;                 // stable id of the layer (the overlay's index in the list)
    const uint8_t *bgra;                // w * h * 4, straight (non-premultiplied) alpha
    int            w, h;                // size of the bitmap
    unsigned long long gen;             // changes whenever the bitmap content changes (never reused)
    int            dst_x, dst_y;        // top-left on the desktop texture (may be partly / fully outside)
    int            dst_w, dst_h;        // drawn size (== w / h unless the display mode changed)
    int            opacity;             // 0..255, multiplied with the bitmap's own alpha
};

#if defined(_WIN32) && defined(HR_GPU_OVERLAY_INTERNAL)
#include <d3d11.h>
#include <wrl/client.h>
#include <unordered_map>
#include <vector>

class HrGpuOverlay {
public:
    // `mt` = the device's multithread-protection interface (or null). Enter()/Leave() are called around the
    // pipeline-state changes so a second thread using the same immediate context cannot slip in between.
    bool Setup(ID3D11Device *dev, ID3D11DeviceContext *ctx, IUnknown *mt);
    void Release();
    // False after any permanent failure (no shader compiler, no device support, a draw error ...).
    bool Usable() const { return !failed_; }
    bool HasLayers() const { return !cur_.empty(); }

    // Replaces the layer list. Uploads the bitmaps whose gen changed. Returns -1 on failure (the caller falls
    // back to the CPU compositor), 0 when the resulting picture is identical to the previous call's, 1 when it
    // differs (a layer changed, moved, appeared or disappeared).
    int SetLayers(const HrGpuLayer *layers, int count);

    // Copies `src` (clipped to the texture) from `base` into `target` and blends every layer onto it.
    bool Render(ID3D11Texture2D *base, ID3D11Texture2D *target, ID3D11RenderTargetView *rtv,
                int tw, int th, const RECT &src);

private:
    bool BuildPipeline();
    struct Tex {
        Microsoft::WRL::ComPtr<ID3D11Texture2D>          tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        int w = 0, h = 0;
        unsigned long long gen = 0;
    };
    struct Meta {
        int key = 0, w = 0, h = 0, dx = 0, dy = 0, dw = 0, dh = 0, opacity = 0;
        unsigned long long gen = 0;
    };
    Microsoft::WRL::ComPtr<ID3D11Device>              dev_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>       ctx_;
    Microsoft::WRL::ComPtr<IUnknown>                  mt_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>        vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>         ps_;
    Microsoft::WRL::ComPtr<ID3D11BlendState>          blend_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>     rast_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>        samp_;
    Microsoft::WRL::ComPtr<ID3D11Buffer>              cb_;
    std::unordered_map<int, Tex> tex_;
    std::vector<Meta>            cur_;
    bool built_  = false;
    bool failed_ = false;
};
#endif
