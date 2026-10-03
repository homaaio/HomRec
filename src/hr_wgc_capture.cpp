#include "hr_wgc_capture.h"
#include "hr_log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kMagic = 0x57474331u;   // 'WGC1'
constexpr int kOk = 0, kTimeout = 1, kLost = 2, kError = -1;
constexpr int kPixelFormatBgra8 = 87;      // DirectXPixelFormat::B8G8R8A8UIntNormalized (= DXGI_FORMAT_B8G8R8A8_UNORM)

// ---------------------------------------------------------------------------------------------
// Hand-declared WinRT ABI. Older MinGW has no <windows.graphics.capture.h>, so the handful of
// interfaces needed are declared here with the vtable order of the Windows SDK's ABI headers.
// (Names are prefixed so they can never clash with a toolchain header that does ship them.)
// ---------------------------------------------------------------------------------------------
struct SizeInt32 { INT32 Width; INT32 Height; };

struct HrInspectable : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetIids(ULONG *, IID **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeClassName(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(int *) = 0;
};
struct HrClosable : public HrInspectable {
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
struct HrCaptureItemInterop : public IUnknown {            // IGraphicsCaptureItemInterop (plain COM)
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID riid, void **result) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID riid, void **result) = 0;
};
struct HrCaptureItem : public HrInspectable {              // IGraphicsCaptureItem
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(SizeInt32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Closed(void *, INT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Closed(INT64) = 0;
};
struct HrFramePoolStatics2 : public HrInspectable {        // IDirect3D11CaptureFramePoolStatics2
    virtual HRESULT STDMETHODCALLTYPE CreateFreeThreaded(void *device, INT32 format, INT32 buffers,
                                                         SizeInt32 size, void **pool) = 0;
};
struct HrFramePool : public HrInspectable {                // IDirect3D11CaptureFramePool
    virtual HRESULT STDMETHODCALLTYPE Recreate(void *device, INT32 format, INT32 buffers, SizeInt32 size) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetNextFrame(void **frame) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_FrameArrived(void *, INT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_FrameArrived(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCaptureSession(void *item, void **session) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(void **) = 0;
};
struct HrFrame : public HrInspectable {                    // IDirect3D11CaptureFrame
    virtual HRESULT STDMETHODCALLTYPE get_Surface(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(INT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ContentSize(SizeInt32 *) = 0;
};
struct HrSession : public HrInspectable {                  // IGraphicsCaptureSession
    virtual HRESULT STDMETHODCALLTYPE StartCapture() = 0;
};
struct HrSession2 : public HrInspectable {                 // IGraphicsCaptureSession2
    virtual HRESULT STDMETHODCALLTYPE get_IsCursorCaptureEnabled(BOOLEAN *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsCursorCaptureEnabled(BOOLEAN) = 0;
};
struct HrSession3 : public HrInspectable {                 // IGraphicsCaptureSession3
    virtual HRESULT STDMETHODCALLTYPE get_IsBorderRequired(BOOLEAN *) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsBorderRequired(BOOLEAN) = 0;
};
struct HrMultithread : public IUnknown {                   // ID3D10Multithread / ID3D11Multithread
    virtual void  STDMETHODCALLTYPE Enter() = 0;
    virtual void  STDMETHODCALLTYPE Leave() = 0;
    virtual BOOL  STDMETHODCALLTYPE SetMultithreadProtected(BOOL) = 0;
    virtual BOOL  STDMETHODCALLTYPE GetMultithreadProtected() = 0;
};
const GUID kIidMultithread = {0x9B7E4E00, 0x342C, 0x4106, {0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0}};
struct HrDxgiAccess : public IUnknown {                    // IDirect3DDxgiInterfaceAccess
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void **object) = 0;
};

const GUID kIidInterop     = {0x3628E81B, 0x3CAC, 0x4C60, {0xB7, 0xF4, 0x23, 0xCE, 0x0E, 0x0C, 0x33, 0x56}};
const GUID kIidItem        = {0x79C3F95B, 0x31F7, 0x4EC2, {0xA4, 0x64, 0x63, 0x2E, 0xF5, 0xD3, 0x07, 0x60}};
const GUID kIidPoolStat2   = {0x589B103F, 0x6BBC, 0x5DF5, {0xA9, 0x91, 0x02, 0xE2, 0x8B, 0x3B, 0x66, 0xD5}};
const GUID kIidDxgiAccess  = {0xA9B3D012, 0x3DF2, 0x4EE3, {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}};
const GUID kIidSession2    = {0x2C39AE40, 0x7D2E, 0x5044, {0x80, 0x4E, 0x8B, 0x67, 0x99, 0xD4, 0xCF, 0x9E}};
const GUID kIidSession3    = {0xF2CDD966, 0x22AE, 0x5EA1, {0x95, 0x96, 0x3A, 0x28, 0x93, 0x44, 0xC3, 0xBE}};
const GUID kIidClosable    = {0x30D5A829, 0x7FA4, 0x4026, {0x83, 0xBB, 0xD7, 0x5B, 0xAE, 0x4E, 0xA9, 0x9E}};
const GUID kIidWinrtDevice = {0xA37624AB, 0x8D5F, 0x4650, {0x9D, 0x3E, 0x9E, 0xAE, 0x3D, 0x9B, 0xC6, 0x70}};

// ---------------------------------------------------------------------------------------------
// Late-bound API (nothing here links statically, so the app still starts on Windows 7/8/10<1903)
// ---------------------------------------------------------------------------------------------
using RoInitializeFn            = HRESULT(WINAPI *)(int);
using RoGetActivationFactoryFn  = HRESULT(WINAPI *)(void *hstring, REFIID iid, void **factory);
using WindowsCreateStringFn     = HRESULT(WINAPI *)(PCWSTR src, UINT32 len, void **hstring);
using WindowsDeleteStringFn     = HRESULT(WINAPI *)(void *hstring);
using CreateD3DDeviceFromDxgiFn = HRESULT(WINAPI *)(IDXGIDevice *, void **inspectable);

struct Api {
    RoInitializeFn            RoInitialize = nullptr;
    RoGetActivationFactoryFn  RoGetActivationFactory = nullptr;
    WindowsCreateStringFn     WindowsCreateString = nullptr;
    WindowsDeleteStringFn     WindowsDeleteString = nullptr;
    CreateD3DDeviceFromDxgiFn CreateD3D11DeviceFromDXGIDevice = nullptr;
    bool                      ok = false;
};

const Api &GetApi() {
    static Api api = [] {
        Api a;
        HMODULE combase = LoadLibraryW(L"combase.dll");
        HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
        if (!combase || !d3d11) return a;
        a.RoInitialize = reinterpret_cast<RoInitializeFn>(GetProcAddress(combase, "RoInitialize"));
        a.RoGetActivationFactory = reinterpret_cast<RoGetActivationFactoryFn>(GetProcAddress(combase, "RoGetActivationFactory"));
        a.WindowsCreateString = reinterpret_cast<WindowsCreateStringFn>(GetProcAddress(combase, "WindowsCreateString"));
        a.WindowsDeleteString = reinterpret_cast<WindowsDeleteStringFn>(GetProcAddress(combase, "WindowsDeleteString"));
        a.CreateD3D11DeviceFromDXGIDevice = reinterpret_cast<CreateD3DDeviceFromDxgiFn>(
            GetProcAddress(d3d11, "CreateDirect3D11DeviceFromDXGIDevice"));
        a.ok = a.RoInitialize && a.RoGetActivationFactory && a.WindowsCreateString &&
               a.WindowsDeleteString && a.CreateD3D11DeviceFromDXGIDevice;
        return a;
    }();
    return api;
}

bool OsBuildAtLeast(DWORD build) {
    using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return false;
    auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(nt, "RtlGetVersion"));
    if (!fn) return false;
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return false;
    return vi.dwMajorVersion > 10 || (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= build);
}

// Activation factory for a runtime class (class name given as a plain wide string).
HRESULT GetFactory(const wchar_t *class_name, REFIID iid, void **out) {
    const Api &api = GetApi();
    void *hs = nullptr;
    HRESULT hr = api.WindowsCreateString(class_name, (UINT32)wcslen(class_name), &hs);
    if (FAILED(hr)) return hr;
    hr = api.RoGetActivationFactory(hs, iid, out);
    api.WindowsDeleteString(hs);
    return hr;
}

template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

// IClosable::Close() on a WinRT object, then Release().
template <class T> void CloseAndRelease(T *&p) {
    if (!p) return;
    HrClosable *c = nullptr;
    if (SUCCEEDED(p->QueryInterface(kIidClosable, reinterpret_cast<void **>(&c))) && c) {
        c->Close();
        c->Release();
    }
    p->Release();
    p = nullptr;
}

// Window rectangles in screen pixels (the process is per-monitor-DPI aware).
bool WindowRects(HWND hwnd, RECT &frame, RECT &client_screen) {
    if (!IsWindow(hwnd)) return false;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) ||
        frame.right <= frame.left || frame.bottom <= frame.top) {
        if (!GetWindowRect(hwnd, &frame)) return false;
    }
    RECT cr{};
    if (!GetClientRect(hwnd, &cr)) return false;
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    client_screen = {origin.x, origin.y, origin.x + cr.right, origin.y + cr.bottom};
    return true;
}

std::mutex g_registry_mu;
std::vector<void *> g_registry;

struct Ctx {
    uint32_t magic = kMagic;
    HWND  hwnd = nullptr;
    int   canvas_w = 0, canvas_h = 0;
    bool  client_only = false;
    bool  cursor = false;
    bool  ok = false;

    ComPtr<ID3D11Device>        dev;
    ComPtr<ID3D11DeviceContext> dc;
    IUnknown      *winrt_dev = nullptr;
    HrCaptureItem *item = nullptr;
    HrFramePool   *pool = nullptr;
    HrSession     *session = nullptr;
    HrSession2    *session2 = nullptr;
    int pool_w = 0, pool_h = 0;

    ComPtr<ID3D11Texture2D> staging;
    int st_w = 0, st_h = 0;

    int  geom_w = -1, geom_h = -1;      // content size the canvas was last composed for
    std::vector<int> xmap;              // scratch for nearest-neighbour scaling
};

void Teardown(Ctx *c) {
    if (c->session) { CloseAndRelease(c->session); }
    SafeRelease(c->session2);
    if (c->pool) { CloseAndRelease(c->pool); }
    SafeRelease(c->item);
    SafeRelease(c->winrt_dev);
    c->staging.Reset();
    c->dc.Reset();
    c->dev.Reset();
    c->pool_w = c->pool_h = 0;
    c->st_w = c->st_h = 0;
    c->geom_w = c->geom_h = -1;
    c->ok = false;
}

bool Init(Ctx *c) {
    const Api &api = GetApi();
    if (!api.ok) { HrLog::Warn("Window capture: Windows.Graphics.Capture is not available on this system."); return false; }
    if (!IsWindow(c->hwnd)) return false;
    api.RoInitialize(1 /*RO_INIT_MULTITHREADED*/);   // harmless if the thread is already an STA (RPC_E_CHANGED_MODE)

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, (UINT)(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
                                   c->dev.GetAddressOf(), nullptr, c->dc.GetAddressOf());
    if (FAILED(hr)) {
        // 11_1 is rejected by pre-Platform-Update systems when listed; retry with the default list.
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               nullptr, 0, D3D11_SDK_VERSION, c->dev.GetAddressOf(), nullptr, c->dc.GetAddressOf());
    }
    if (FAILED(hr)) { HrLog::Warn("Window capture: D3D11CreateDevice failed (0x" + std::to_string((unsigned long)hr) + ")."); return false; }

    {   // WGC uses the device from its own threads too - serialise access to the immediate context.
        HrMultithread *mt = nullptr;
        if (SUCCEEDED(c->dev->QueryInterface(kIidMultithread, reinterpret_cast<void **>(&mt))) && mt) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
        }
    }
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(c->dev.As(&dxgi))) return false;
    void *insp = nullptr;
    hr = api.CreateD3D11DeviceFromDXGIDevice(dxgi.Get(), &insp);
    if (FAILED(hr) || !insp) { HrLog::Warn("Window capture: CreateDirect3D11DeviceFromDXGIDevice failed."); return false; }
    {
        IUnknown *unk = static_cast<IUnknown *>(insp);
        IUnknown *as_device = nullptr;
        if (SUCCEEDED(unk->QueryInterface(kIidWinrtDevice, reinterpret_cast<void **>(&as_device))) && as_device) {
            unk->Release();
            c->winrt_dev = as_device;
        } else {
            c->winrt_dev = unk;   // the object returned already is the device interface
        }
    }

    HrCaptureItemInterop *interop = nullptr;
    hr = GetFactory(L"Windows.Graphics.Capture.GraphicsCaptureItem", kIidInterop, reinterpret_cast<void **>(&interop));
    if (FAILED(hr) || !interop) { HrLog::Warn("Window capture: GraphicsCaptureItem factory unavailable."); return false; }
    hr = interop->CreateForWindow(c->hwnd, kIidItem, reinterpret_cast<void **>(&c->item));
    interop->Release();
    if (FAILED(hr) || !c->item) {
        HrLog::Warn("Window capture: this window can't be captured (CreateForWindow 0x" +
                    std::to_string((unsigned long)hr) + ") - falling back to screen crop.");
        return false;
    }

    SizeInt32 sz{0, 0};
    if (FAILED(c->item->get_Size(&sz)) || sz.Width <= 0 || sz.Height <= 0) { sz.Width = c->canvas_w; sz.Height = c->canvas_h; }
    sz.Width  = std::min<INT32>(std::max<INT32>(sz.Width, 16), 16384);
    sz.Height = std::min<INT32>(std::max<INT32>(sz.Height, 16), 16384);

    HrFramePoolStatics2 *stat2 = nullptr;
    hr = GetFactory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", kIidPoolStat2, reinterpret_cast<void **>(&stat2));
    if (FAILED(hr) || !stat2) { HrLog::Warn("Window capture: frame pool factory unavailable."); return false; }
    hr = stat2->CreateFreeThreaded(c->winrt_dev, kPixelFormatBgra8, 2, sz, reinterpret_cast<void **>(&c->pool));
    stat2->Release();
    if (FAILED(hr) || !c->pool) { HrLog::Warn("Window capture: CreateFreeThreaded failed."); return false; }
    c->pool_w = sz.Width; c->pool_h = sz.Height;

    hr = c->pool->CreateCaptureSession(c->item, reinterpret_cast<void **>(&c->session));
    if (FAILED(hr) || !c->session) { HrLog::Warn("Window capture: CreateCaptureSession failed."); return false; }

    // Optional niceties - every failure here is ignored (older builds simply lack the interfaces).
    if (SUCCEEDED(c->session->QueryInterface(kIidSession2, reinterpret_cast<void **>(&c->session2))) && c->session2)
        c->session2->put_IsCursorCaptureEnabled(c->cursor ? TRUE : FALSE);
    {
        HrSession3 *s3 = nullptr;
        if (SUCCEEDED(c->session->QueryInterface(kIidSession3, reinterpret_cast<void **>(&s3))) && s3) {
            s3->put_IsBorderRequired(FALSE);   // no yellow frame around the window (Windows 11 / recent 10)
            s3->Release();
        }
    }

    hr = c->session->StartCapture();
    if (FAILED(hr)) { HrLog::Warn("Window capture: StartCapture failed (0x" + std::to_string((unsigned long)hr) + ")."); return false; }

    c->geom_w = c->geom_h = -1;
    c->ok = true;
    return true;
}

// Copies the mapped window pixels into the fixed-size canvas (see the header comment).
void Compose(Ctx *c, const uint8_t *src, int pitch, int sw, int sh, uint8_t *dst) {
    const int cw = c->canvas_w, ch = c->canvas_h;
    const bool geom_changed = (c->geom_w != sw || c->geom_h != sh);
    c->geom_w = sw; c->geom_h = sh;

    if (sw == cw && sh == ch) {
        for (int y = 0; y < sh; ++y)
            std::memcpy(dst + (size_t)y * cw * 4, src + (size_t)y * pitch, (size_t)cw * 4);
        return;
    }

    if (geom_changed) std::memset(dst, 0, (size_t)cw * ch * 4);   // black bars

    int dw = sw, dh = sh;
    if (sw > cw || sh > ch) {                       // too big: scale down, keep the aspect ratio
        const double s = std::min((double)cw / sw, (double)ch / sh);
        dw = std::max(2, (int)(sw * s));
        dh = std::max(2, (int)(sh * s));
    }
    dw = std::min(dw, cw); dh = std::min(dh, ch);
    const int ox = (cw - dw) / 2, oy = (ch - dh) / 2;

    if (dw == sw && dh == sh) {                     // smaller: centred, 1:1
        for (int y = 0; y < sh; ++y)
            std::memcpy(dst + ((size_t)(oy + y) * cw + ox) * 4, src + (size_t)y * pitch, (size_t)sw * 4);
        return;
    }

    c->xmap.resize((size_t)dw);
    for (int x = 0; x < dw; ++x) c->xmap[(size_t)x] = (int)(((int64_t)x * sw) / dw);
    for (int y = 0; y < dh; ++y) {
        const int sy = (int)(((int64_t)y * sh) / dh);
        const uint32_t *srow = reinterpret_cast<const uint32_t *>(src + (size_t)sy * pitch);
        uint32_t *drow = reinterpret_cast<uint32_t *>(dst + ((size_t)(oy + y) * cw + ox) * 4);
        for (int x = 0; x < dw; ++x) drow[x] = srow[c->xmap[(size_t)x]];
    }
}

int ProcessFrame(Ctx *c, HrFrame *frame, uint8_t *out) {
    SizeInt32 cs{0, 0};
    if (FAILED(frame->get_ContentSize(&cs)) || cs.Width <= 0 || cs.Height <= 0) return kTimeout;

    // The window was resized: later frames must be delivered at the new size.
    if (cs.Width != c->pool_w || cs.Height != c->pool_h) {
        if (SUCCEEDED(c->pool->Recreate(c->winrt_dev, kPixelFormatBgra8, 2, cs))) {
            c->pool_w = cs.Width; c->pool_h = cs.Height;
        }
    }

    void *surf_raw = nullptr;
    if (FAILED(frame->get_Surface(&surf_raw)) || !surf_raw) return kError;
    IUnknown *surf = static_cast<IUnknown *>(surf_raw);
    HrDxgiAccess *access = nullptr;
    HRESULT hr = surf->QueryInterface(kIidDxgiAccess, reinterpret_cast<void **>(&access));
    ID3D11Texture2D *tex = nullptr;
    if (SUCCEEDED(hr) && access) hr = access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex));
    SafeRelease(access);
    surf->Release();
    if (FAILED(hr) || !tex) return kError;

    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    int rx = 0, ry = 0;
    int rw = std::min<int>(cs.Width, (int)td.Width);
    int rh = std::min<int>(cs.Height, (int)td.Height);

    if (c->client_only) {
        RECT frame_rc{}, client_rc{};
        if (WindowRects(c->hwnd, frame_rc, client_rc)) {
            int ax = std::max(0, (int)(client_rc.left - frame_rc.left));
            int ay = std::max(0, (int)(client_rc.top - frame_rc.top));
            int aw = std::min<int>(client_rc.right - client_rc.left, rw - ax);
            int ah = std::min<int>(client_rc.bottom - client_rc.top, rh - ay);
            if (aw >= 2 && ah >= 2) { rx = ax; ry = ay; rw = aw; rh = ah; }
        }
    }
    rw &= ~1; rh &= ~1;                              // even sizes keep later YUV math simple
    if (rw < 2 || rh < 2) { tex->Release(); return kTimeout; }

    if (!c->staging || c->st_w != rw || c->st_h != rh) {
        c->staging.Reset();
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = (UINT)rw; sd.Height = (UINT)rh;
        sd.MipLevels = 1; sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(c->dev->CreateTexture2D(&sd, nullptr, c->staging.GetAddressOf()))) { tex->Release(); return kError; }
        c->st_w = rw; c->st_h = rh;
    }

    D3D11_BOX box{(UINT)rx, (UINT)ry, 0, (UINT)(rx + rw), (UINT)(ry + rh), 1};
    c->dc->CopySubresourceRegion(c->staging.Get(), 0, 0, 0, 0, tex, 0, &box);
    tex->Release();

    D3D11_MAPPED_SUBRESOURCE m{};
    hr = c->dc->Map(c->staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        return (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) ? kLost : kError;
    }
    Compose(c, static_cast<const uint8_t *>(m.pData), (int)m.RowPitch, rw, rh, out);
    c->dc->Unmap(c->staging.Get(), 0);
    return kOk;
}

} // namespace

// ---------------------------------------------------------------------------------------------
bool HrWgcSupported() {
    return OsBuildAtLeast(18362) && GetApi().ok;
}

bool HrWgcQueryWindowSize(HWND hwnd, bool client_only, int *w, int *h) {
    RECT frame{}, client{};
    if (!WindowRects(hwnd, frame, client)) return false;
    const RECT &r = client_only ? client : frame;
    if (w) *w = (int)(r.right - r.left);
    if (h) *h = (int)(r.bottom - r.top);
    return (r.right - r.left) > 0 && (r.bottom - r.top) > 0;
}

bool HrWgcProbeWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !HrWgcSupported()) return false;
    GetApi().RoInitialize(1);
    HrCaptureItemInterop *interop = nullptr;
    if (FAILED(GetFactory(L"Windows.Graphics.Capture.GraphicsCaptureItem", kIidInterop,
                          reinterpret_cast<void **>(&interop))) || !interop)
        return false;
    HrCaptureItem *item = nullptr;
    const HRESULT hr = interop->CreateForWindow(hwnd, kIidItem, reinterpret_cast<void **>(&item));
    interop->Release();
    if (FAILED(hr) || !item) return false;
    item->Release();
    return true;
}

void *HrWgcCreate(HWND hwnd, int canvas_w, int canvas_h, bool client_only, bool draw_cursor) {
    if (!hwnd || canvas_w < 2 || canvas_h < 2 || !HrWgcSupported()) return nullptr;
    auto *c = new Ctx();
    c->hwnd = hwnd;
    c->canvas_w = canvas_w & ~1;
    c->canvas_h = canvas_h & ~1;
    c->client_only = client_only;
    c->cursor = draw_cursor;
    if (!Init(c)) {
        Teardown(c);
        delete c;
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(g_registry_mu);
        g_registry.push_back(c);
    }
    HrLog::Info("Window capture: using Windows.Graphics.Capture (" + std::to_string(c->canvas_w) + "x" +
                std::to_string(c->canvas_h) + (client_only ? ", content area only" : ", whole window") + ").");
    return c;
}

bool HrWgcIsHandle(void *handle) {
    if (!handle) return false;
    std::lock_guard<std::mutex> lk(g_registry_mu);
    return std::find(g_registry.begin(), g_registry.end(), handle) != g_registry.end();
}

void HrWgcDestroy(void *handle) {
    if (!handle) return;
    {
        std::lock_guard<std::mutex> lk(g_registry_mu);
        auto it = std::find(g_registry.begin(), g_registry.end(), handle);
        if (it == g_registry.end()) return;
        g_registry.erase(it);
    }
    auto *c = static_cast<Ctx *>(handle);
    Teardown(c);
    delete c;
}

int HrWgcCapture(void *handle, uint8_t *out_bgra, int timeout_ms) {
    auto *c = static_cast<Ctx *>(handle);
    if (!c || !out_bgra || !c->ok) return kError;

    const DWORD t0 = GetTickCount();
    for (;;) {
        HrFrame *frame = nullptr;
        HRESULT hr = c->pool->TryGetNextFrame(reinterpret_cast<void **>(&frame));
        if (SUCCEEDED(hr) && frame) {
            // Always present the newest picture: drop anything older that queued up.
            for (;;) {
                HrFrame *newer = nullptr;
                if (FAILED(c->pool->TryGetNextFrame(reinterpret_cast<void **>(&newer))) || !newer) break;
                CloseAndRelease(frame);
                frame = newer;
            }
            const int r = ProcessFrame(c, frame, out_bgra);
            CloseAndRelease(frame);
            return r;
        }
        if (FAILED(hr)) return kLost;
        if (timeout_ms <= 0 || (int)(GetTickCount() - t0) >= timeout_ms) return kTimeout;
        Sleep(2);   // frames arrive at most every ~8 ms; 2 ms keeps latency low without busy-spinning
    }
}

int HrWgcGetSize(void *handle, int *w, int *h) {
    auto *c = static_cast<Ctx *>(handle);
    if (!c) return 0;
    if (w) *w = c->canvas_w;
    if (h) *h = c->canvas_h;
    return 1;
}

int HrWgcReset(void *handle) {
    auto *c = static_cast<Ctx *>(handle);
    if (!c) return 0;
    Teardown(c);
    return Init(c) ? 1 : 0;
}

void HrWgcSetCursor(void *handle, bool on) {
    auto *c = static_cast<Ctx *>(handle);
    if (!c) return;
    c->cursor = on;
    if (c->session2) c->session2->put_IsCursorCaptureEnabled(on ? TRUE : FALSE);
}
