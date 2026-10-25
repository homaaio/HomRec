// gpu_overlay.cpp - see gpu_overlay.h. NOT COMPILED OR RUN (written without a compiler / GPU).
#ifdef _WIN32
#define HR_GPU_OVERLAY_INTERNAL
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "gpu_overlay.h"
#include "../utils/log.h"
#include <algorithm>
#include <iterator>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

// Same layout / IID as ID3D10Multithread (declared again here so no d3d10.h is needed).
struct HrMtIface : public IUnknown {
    virtual void  STDMETHODCALLTYPE Enter() = 0;
    virtual void  STDMETHODCALLTYPE Leave() = 0;
    virtual BOOL  STDMETHODCALLTYPE SetMultithreadProtected(BOOL protect) = 0;
    virtual BOOL  STDMETHODCALLTYPE GetMultithreadProtected() = 0;
};

// Enter()/Leave() around a block of context calls (no-op without the interface).
struct MtLock {
    HrMtIface *m;
    explicit MtLock(IUnknown *u) : m(reinterpret_cast<HrMtIface *>(u)) { if (m) m->Enter(); }
    ~MtLock() { if (m) m->Leave(); }
    MtLock(const MtLock &) = delete;
    MtLock &operator=(const MtLock &) = delete;
};

// One quad per layer, generated from SV_VertexID (no vertex buffer / input layout). The vertex shader maps the
// CLIPPED destination rectangle (pixels) to clip space; the pixel shader derives the texture coordinate from
// the UNCLIPPED rectangle, so a layer that sticks out of the picture is cut, not squeezed. Point sampling: at
// 1:1 every output pixel reads exactly one texel.
const char kHlsl[] =
    "cbuffer CB : register(b0) {\n"
    "  float4 clip_rect;\n"   // x0, y0, x1, y1 of the clipped quad (pixels)
    "  float4 full_rect;\n"   // x, y, w, h of the unclipped destination (pixels)
    "  float4 misc;\n"        // target w, target h, opacity (0..1), unused
    "};\n"
    "Texture2D    tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct VSOut { float4 pos : SV_Position; };\n"
    "VSOut VS(uint id : SV_VertexID) {\n"
    "  float2 c = float2((float)(id & 1u), (float)(id >> 1));\n"
    "  float2 p = lerp(clip_rect.xy, clip_rect.zw, c);\n"
    "  VSOut o;\n"
    "  o.pos = float4(p.x / misc.x * 2.0 - 1.0, 1.0 - p.y / misc.y * 2.0, 0.0, 1.0);\n"
    "  return o;\n"
    "}\n"
    "float4 PS(VSOut i) : SV_Target {\n"
    "  float2 uv = (i.pos.xy - full_rect.xy) / full_rect.zw;\n"
    "  float4 c = tex.SampleLevel(smp, uv, 0);\n"
    "  return float4(c.rgb, c.a * misc.z);\n"
    "}\n";

typedef HRESULT(WINAPI *PFN_D3DCompileLocal)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR,
                                             UINT, UINT, ID3DBlob **, ID3DBlob **);

struct CBData { float clip[4]; float full[4]; float misc[4]; };

} // namespace

bool HrGpuOverlay::Setup(ID3D11Device *dev, ID3D11DeviceContext *ctx, IUnknown *mt) {
    Release();
    if (!dev || !ctx) { failed_ = true; return false; }
    dev_ = dev; ctx_ = ctx;
    if (mt) { mt->AddRef(); mt_.Attach(mt); }
    failed_ = false;
    return true;
}

void HrGpuOverlay::Release() {
    tex_.clear(); cur_.clear();
    vs_.Reset(); ps_.Reset(); blend_.Reset(); rast_.Reset(); samp_.Reset(); cb_.Reset();
    ctx_.Reset(); dev_.Reset(); mt_.Reset();
    built_ = false;
}

bool HrGpuOverlay::BuildPipeline() {
    if (built_) return true;
    if (!dev_) return false;

    // The shader compiler is loaded from System32 at run time: the exe does not link against it and still
    // starts where it is missing (then the CPU compositor is used).
    wchar_t sysdir[MAX_PATH] = {0};
    UINT n = GetSystemDirectoryW(sysdir, MAX_PATH);
    HMODULE mod = nullptr;
    if (n > 0 && n < MAX_PATH - 32) {
        static const wchar_t *names[] = { L"\\d3dcompiler_47.dll", L"\\d3dcompiler_46.dll", L"\\d3dcompiler_43.dll" };
        for (const wchar_t *nm : names) {
            std::wstring p = std::wstring(sysdir) + nm;
            mod = LoadLibraryW(p.c_str());
            if (mod) break;
        }
    }
    if (!mod) { HrLog::Warn("GPU overlays: d3dcompiler_47.dll not found - overlays stay on the CPU."); return false; }
    auto compile = reinterpret_cast<PFN_D3DCompileLocal>(GetProcAddress(mod, "D3DCompile"));
    if (!compile) { HrLog::Warn("GPU overlays: D3DCompile missing - overlays stay on the CPU."); return false; }

    ComPtr<ID3DBlob> vsb, psb, err;
    HRESULT hr = compile(kHlsl, sizeof(kHlsl) - 1, "hr_overlay.hlsl", nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr) || !vsb) {
        HrLog::Warn("GPU overlays: vertex shader did not compile" +
                    std::string(err ? std::string(" (") + (const char *)err->GetBufferPointer() + ")" : "") + ".");
        return false;
    }
    err.Reset();
    hr = compile(kHlsl, sizeof(kHlsl) - 1, "hr_overlay.hlsl", nullptr, nullptr, "PS", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr) || !psb) {
        HrLog::Warn("GPU overlays: pixel shader did not compile" +
                    std::string(err ? std::string(" (") + (const char *)err->GetBufferPointer() + ")" : "") + ".");
        return false;
    }
    if (FAILED(dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_))) {
        HrLog::Warn("GPU overlays: this GPU / driver rejected the shaders - overlays stay on the CPU.");
        return false;
    }

    // Straight-alpha "over": rgb = src * a + dst * (1 - a); the destination alpha is left alone.
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev_->CreateBlendState(&bd, &blend_))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev_->CreateRasterizerState(&rd, &rast_))) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev_->CreateSamplerState(&sd, &samp_))) return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth      = sizeof(CBData);           // 48 bytes, a multiple of 16
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev_->CreateBuffer(&cbd, nullptr, &cb_))) return false;

    built_ = true;
    return true;
}

int HrGpuOverlay::SetLayers(const HrGpuLayer *layers, int count) {
    if (failed_ || !dev_ || !ctx_) return -1;
    if (count < 0) count = 0;
    if (count > 0 && !layers) count = 0;
    if (count > 0 && !BuildPipeline()) { failed_ = true; return -1; }

    std::vector<Meta> next;
    next.reserve((size_t)count);
    for (int i = 0; i < count; ++i) {
        const HrGpuLayer &L = layers[i];
        if (!L.bgra || L.w <= 0 || L.h <= 0 || L.w > 8192 || L.h > 8192 || L.dst_w <= 0 || L.dst_h <= 0 ||
            L.opacity <= 0) continue;                                   // draws nothing: not part of the picture
        Tex &t = tex_[L.key];
        if (!t.tex || t.w != L.w || t.h != L.h) {
            t.tex.Reset(); t.srv.Reset(); t.gen = 0;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = (UINT)L.w; td.Height = (UINT)L.h;
            td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc = {1, 0};
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &t.tex)) ||
                FAILED(dev_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv))) {
                HrLog::Warn("GPU overlays: could not create a layer texture - overlays go back to the CPU.");
                failed_ = true;
                return -1;
            }
            t.w = L.w; t.h = L.h;
        }
        if (t.gen != L.gen) {
            MtLock lk(mt_.Get());
            ctx_->UpdateSubresource(t.tex.Get(), 0, nullptr, L.bgra, (UINT)L.w * 4u, 0);
            t.gen = L.gen;
        }
        Meta m;
        m.key = L.key; m.w = L.w; m.h = L.h; m.gen = L.gen;
        m.dx = L.dst_x; m.dy = L.dst_y; m.dw = L.dst_w; m.dh = L.dst_h;
        m.opacity = std::min(255, L.opacity);
        next.push_back(m);
    }

    // Forget the textures of layers that are gone.
    for (auto it = tex_.begin(); it != tex_.end(); ) {
        bool used = false;
        for (const Meta &m : next) if (m.key == it->first) { used = true; break; }
        it = used ? std::next(it) : tex_.erase(it);
    }

    bool changed = next.size() != cur_.size();
    for (size_t i = 0; !changed && i < next.size(); ++i)
        changed = std::memcmp(&next[i], &cur_[i], sizeof(Meta)) != 0;
    cur_.swap(next);
    return changed ? 1 : 0;
}

bool HrGpuOverlay::Render(ID3D11Texture2D *base, ID3D11Texture2D *target, ID3D11RenderTargetView *rtv,
                          int tw, int th, const RECT &src) {
    if (failed_ || !built_ || !base || !target || !rtv || tw <= 0 || th <= 0) return false;
    if (cur_.empty()) return false;

    MtLock lk(mt_.Get());

    // 1) the part of the desktop the video processor is going to read -> the work texture
    RECT s = src;
    s.left = std::max<LONG>(0, s.left);   s.top = std::max<LONG>(0, s.top);
    s.right = std::min<LONG>(tw, s.right); s.bottom = std::min<LONG>(th, s.bottom);
    if (s.right <= s.left || s.bottom <= s.top) { s = RECT{0, 0, tw, th}; }
    D3D11_BOX bx{};
    bx.left = (UINT)s.left; bx.top = (UINT)s.top; bx.front = 0;
    bx.right = (UINT)s.right; bx.bottom = (UINT)s.bottom; bx.back = 1;
    ctx_->CopySubresourceRegion(target, 0, (UINT)s.left, (UINT)s.top, 0, base, 0, &bx);

    // 2) fixed pipeline state
    D3D11_VIEWPORT vp{};
    vp.Width = (FLOAT)tw; vp.Height = (FLOAT)th; vp.MinDepth = 0.f; vp.MaxDepth = 1.f;
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11Buffer *cb = cb_.Get();
    ctx_->VSSetConstantBuffers(0, 1, &cb);
    ctx_->PSSetConstantBuffers(0, 1, &cb);
    ID3D11SamplerState *sm = samp_.Get();
    ctx_->PSSetSamplers(0, 1, &sm);
    ctx_->RSSetState(rast_.Get());
    ctx_->RSSetViewports(1, &vp);
    const FLOAT bf[4] = {0, 0, 0, 0};
    ctx_->OMSetBlendState(blend_.Get(), bf, 0xFFFFFFFFu);
    ctx_->OMSetDepthStencilState(nullptr, 0);
    ID3D11RenderTargetView *rt = rtv;
    ctx_->OMSetRenderTargets(1, &rt, nullptr);

    // 3) one quad per layer, bottom to top (same order as the CPU compositor)
    bool ok = true;
    for (const Meta &m : cur_) {
        auto it = tex_.find(m.key);
        if (it == tex_.end() || !it->second.srv) continue;
        const int x0 = std::max(m.dx, (int)s.left),  y0 = std::max(m.dy, (int)s.top);
        const int x1 = std::min(m.dx + m.dw, (int)s.right), y1 = std::min(m.dy + m.dh, (int)s.bottom);
        if (x0 >= x1 || y0 >= y1) continue;                              // completely outside the picture

        D3D11_MAPPED_SUBRESOURCE ms{};
        if (FAILED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) { ok = false; break; }
        CBData d;
        d.clip[0] = (float)x0; d.clip[1] = (float)y0; d.clip[2] = (float)x1; d.clip[3] = (float)y1;
        d.full[0] = (float)m.dx; d.full[1] = (float)m.dy; d.full[2] = (float)m.dw; d.full[3] = (float)m.dh;
        d.misc[0] = (float)tw; d.misc[1] = (float)th; d.misc[2] = (float)m.opacity / 255.0f; d.misc[3] = 0.f;
        std::memcpy(ms.pData, &d, sizeof(d));
        ctx_->Unmap(cb_.Get(), 0);

        ID3D11ShaderResourceView *srv = it->second.srv.Get();
        ctx_->PSSetShaderResources(0, 1, &srv);
        ctx_->Draw(4, 0);
    }

    // 4) leave nothing bound: the work texture is read by the video processor next
    ID3D11ShaderResourceView *nullsrv = nullptr;
    ctx_->PSSetShaderResources(0, 1, &nullsrv);
    ID3D11RenderTargetView *nullrt = nullptr;
    ctx_->OMSetRenderTargets(1, &nullrt, nullptr);

    if (!ok) failed_ = true;
    return ok;
}
#endif // _WIN32
