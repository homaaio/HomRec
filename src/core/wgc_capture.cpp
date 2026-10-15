#include "wgc_capture.h"
#include "../utils/log.h"
#include "../utils/log_paths.h"
#include "../utils/wgc_log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

// Screen duplication helpers (dxgi_capture.cpp / ui_utils.cpp, linked into the same exe). Used by the smart
// window mode, see SmartWantsScreen() below.
extern "C" {
    void *hr_dx_create(int adapter_idx, int output_idx);
    void  hr_dx_destroy(void *handle);
    int   hr_dx_get_size(void *handle, int *out_w, int *out_h);
    int   hr_dx_capture(void *handle, uint8_t *out_bgra, int timeout_ms);
    int   hr_dx_reset(void *handle);
    void  hr_dx_set_output_size(void *handle, int w, int h);
    int   hr_dx_get_dirty_rows(void *handle, int *y0, int *y1);
    int   hr_dx_output_count(int adapter_idx);
    int   hr_dx_output_desc(int adapter_idx, int output_idx, int *out_x, int *out_y, int *out_w, int *out_h,
                            char *name_buf, int name_buf_len);
    void  hr_composite_cursor(uint8_t *bgra, int width, int height, int origin_x, int origin_y);
    int   hr_cursor_state(int *visible, int *x, int *y, unsigned long long *handle);
}

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

// NOTE: D3D11CreateDevice is deliberately NOT late-bound here. It is the plain import from
// -ld3d11, called exactly the way dxgi_capture.cpp's hr_dx_create() calls it (explicit adapter,
// D3D_DRIVER_TYPE_UNKNOWN, non-null feature-level out-pointer) - that call is proven to work in
// this very process, see CreateWgcDevice().
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

// ---- crash sentinel ("circuit breaker") -------------------------------------------------------
// Windows.Graphics.Capture is driven through hand-written COM/WinRT calls. If native code in here
// ever takes the whole process down (the 2.4.0 crash on "select a window, then click the preview /
// press Start"), every later launch would walk straight into the same crash again.
//
// So: while ANY native WGC work is in flight a small flag file (logs\wgc_guard.flag) exists. It is
// deleted again when that work finishes. If the process dies in the middle, the flag survives, and
// the next launch sees it, logs it once and refuses WGC (HrWgcSupported() == false) - the caller
// then uses the screen-crop path, which does not touch any of this code. The user can give WGC
// another chance from Settings (HrWgcResetCrashGuard()).
//
// Deliberately built from plain Win32 primitives (SRWLOCK, interlocked ops, CreateFileW) so the
// sentinel itself depends on nothing that could be involved in the fault it is watching for.
std::wstring GuardPath() { return HrLogPaths::LogFilePath(L"wgc_guard.flag"); }

volatile LONG g_guard_state = 0;      // 0 = not looked yet, 1 = clean, 2 = previous run crashed in WGC
SRWLOCK       g_guard_lock  = SRWLOCK_INIT;
int           g_guard_depth = 0;      // number of WGC operations currently in flight (any thread)

// Bump when a WGC crash cause has been fixed: a flag written by an OLDER build (no / smaller marker)
// belongs to a bug that no longer exists, so it is deleted instead of switching WGC off for good.
// (2 = first attempt: D3D11CreateDevice resolved from the export table - did not help.)
// (3 = D3D11CreateDevice(nullptr adapter, DRIVER_TYPE_HARDWARE) crashed on some setups; now created on an
//  explicit adapter exactly like dxgi_capture.cpp, see CreateWgcDevice().)
// (4 = the crash was not in D3D11CreateDevice but right after it, in the ID3D11Multithread block, which is
//  removed; device creation also runs contained on a helper thread now.)
// (5 = the crash moved on to the first WinRT call on the GUI thread (CreateForWindow): the WHOLE session setup
//  now runs on its own MTA thread, contained, see InitContained().)
#define HR_WGC_GUARD_GEN "WGC-GUARD-GEN-5"
constexpr char kGuardGen[] = HR_WGC_GUARD_GEN;

bool GuardTripped() {
    if (g_guard_state == 0) {
        bool present = false;
        const std::wstring path = GuardPath();
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char buf[512] = {};
            DWORD n = 0;
            ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
            CloseHandle(h);
            buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
            if (std::strstr(buf, kGuardGen)) {
                present = true;                       // this build's own flag: the last run really crashed in WGC
            } else {
                DeleteFileW(path.c_str());            // left by an older build, whose bug is fixed
                HrWgcLog::Note("Window capture: cleared a crash flag left by an older HomRec build - WGC gets another chance.");
            }
        }
        if (InterlockedCompareExchange(&g_guard_state, present ? 2 : 1, 0) == 0 && present) {
            HrWgcLog::Warn("Window capture: Windows.Graphics.Capture is switched OFF because the previous run "
                        "crashed inside it (logs\\wgc_guard.flag). Window recording uses the screen-crop "
                        "method. To try WGC again: Settings > General > Window capture > re-select it.");
        }
    }
    return g_guard_state == 2;
}

void GuardEnter() {
    (void)GuardTripped();   // latch the previous run's verdict BEFORE this process creates the flag itself
    AcquireSRWLockExclusive(&g_guard_lock);
    if (g_guard_depth++ == 0) {
        HANDLE h = CreateFileW(GuardPath().c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            static const char note[] = HR_WGC_GUARD_GEN "\r\n"
                                       "HomRec: native Windows.Graphics.Capture code was running when this file "
                                       "was written. If it still exists at the next start, that run crashed.\r\n";
            DWORD n = 0;
            WriteFile(h, note, (DWORD)(sizeof(note) - 1), &n, nullptr);
            CloseHandle(h);
        }
    }
    ReleaseSRWLockExclusive(&g_guard_lock);
}

void GuardLeave() {
    AcquireSRWLockExclusive(&g_guard_lock);
    if (g_guard_depth > 0 && --g_guard_depth == 0) DeleteFileW(GuardPath().c_str());
    ReleaseSRWLockExclusive(&g_guard_lock);
}

struct GuardScope {
    GuardScope()  { GuardEnter(); }
    ~GuardScope() { GuardLeave(); }
    GuardScope(const GuardScope &) = delete;
    GuardScope &operator=(const GuardScope &) = delete;
};

// Same, but only armed when `on` - used for the first few frames of a capture session.
struct OptionalGuard {
    bool on;
    explicit OptionalGuard(bool o) : on(o) { if (on) GuardEnter(); }
    ~OptionalGuard() { if (on) GuardLeave(); }
    OptionalGuard(const OptionalGuard &) = delete;
    OptionalGuard &operator=(const OptionalGuard &) = delete;
};

std::mutex g_registry_mu;

struct Ctx {
    uint32_t magic = kMagic;
    // Serialises everything that touches the COM objects below. The capture thread (HrWgcCapture /
    // HrWgcReset), the UI thread (HrWgcSetCursor, HrWgcDestroy) and the preview-teardown thread
    // (HrWgcDestroy) used to race on session/pool/dc: HrWgcSetCursor() called session2->put_...()
    // while the capture thread's HrWgcReset() -> Teardown() had just released it (calls through a
    // freed COM object - the kind of bug that ends as an "execute (DEP) violation at a non-code
    // address" like the 2026-10-07 15:48:53 dump; that dump could not be pinned to this line, so
    // treat this as hardening of a real race, not as a proven root cause).
    std::mutex mu;
    bool  dead = false;      // HrWgcDestroy() ran - every later call is a no-op / error
    HWND  hwnd = nullptr;
    int   canvas_w = 0, canvas_h = 0;
    bool  client_only = false;
    bool  cursor = false;
    bool  ok = false;
    int   frames_ok = 0;     // frames delivered so far - the first few run under the crash sentinel
    int   guard_calls = 0;   // HrWgcCapture() calls made under the sentinel (capped, so an idle window costs nothing)

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

    // ---- PrintWindow backend (no WinRT at all, see PwCapture()) ----
    bool     pw = false;                // this context captures with PrintWindow, not Windows.Graphics.Capture
    HDC      pw_dc = nullptr;
    HBITMAP  pw_bmp = nullptr, pw_old = nullptr;
    uint8_t *pw_bits = nullptr;         // top-down BGRA DIB section (pw_w x pw_h)
    int      pw_w = 0, pw_h = 0;
    bool     pw_fail_warned = false, pw_slow_warned = false;

    // ---- 7th pass smart mode: cheap screen duplication while the window is not covered ----------------
    bool     smart = false;             // option (HrWgcOptions::smart)
    int      fps_cap = 0;               // option (HrWgcOptions::fps_cap): exact captures per second, 0 = none
    int      mode = 0;                  // 0 = exact capture (PrintWindow), 1 = screen duplication
    bool     dx_broken = false;         // the duplication failed for good: smart mode is off for this context
    int      dx_fail = 0;               // consecutive duplication failures
    ULONGLONG dx_retry_at = 0;          // after a failure: not before this tick
    void    *dx = nullptr;              // hr_dx_* context of the monitor the window is on
    RECT     dx_mon{0, 0, 0, 0};        // that monitor, desktop coordinates
    int      dx_w = 0, dx_h = 0;        // size of dx_buf
    std::vector<uint8_t> dx_buf;        // BGRA copy of the whole monitor (the pointer must stay stable)
    bool     dx_have_data = false;      // dx_buf holds a real picture since the last (re)start of the duplication
    int      dx_wait = 0;               // ticks spent waiting for that first picture
    bool     need_compose = true;       // the canvas must be rewritten even if nothing changed (mode switch...)
    RECT     last_crop{0, 0, 0, 0};     // screen rect the canvas was last composed from (screen mode)
    ULONGLONG next_occl_check = 0;      // exact mode: when to look again whether the window is still covered
    ULONGLONG clear_since = 0;          // exact mode: since when nothing has covered it
    bool     cur_drawn = false;         // screen mode: a pointer is baked into the canvas ...
    int      cur_x = 0, cur_y = 0;      // ... at this position,
    unsigned long long cur_handle = 0;  // ... with this shape

    // ---- exact mode: change detection and pacing ----
    uint64_t last_hash = 0;
    bool     have_hash = false;
    int      static_streak = 0;         // consecutive exact captures that came out identical
    ULONGLONG next_pw_at = 0;           // earliest tick of the next PrintWindow call
    DWORD    last_pw_ms = 0;            // how long the last PrintWindow call took
    DWORD    last_input_tick = 0;       // GetLastInputInfo() at the previous frame
    bool     gone_logged = false;       // "the window is gone" has been logged

    // ---- statistics, written to wgc.log every few seconds ----
    ULONGLONG st_t0 = 0;
    uint32_t st_calls = 0, st_pw = 0, st_pw_ms = 0, st_pw_max = 0, st_dx_new = 0, st_dx_idle = 0, st_hold = 0, st_static = 0;
};

// Registry of live contexts. shared_ptr: a caller that looked a handle up keeps the context alive
// until it is done, even if HrWgcDestroy() removes it from the registry in the meantime.
std::vector<std::shared_ptr<Ctx>> g_registry;

std::shared_ptr<Ctx> FindCtx(void *handle) {
    if (!handle) return nullptr;
    std::lock_guard<std::mutex> lk(g_registry_mu);
    for (const auto &p : g_registry)
        if (p.get() == handle) return p;
    return nullptr;
}

void PwRelease(Ctx *c) {
    if (c->pw_dc && c->pw_old) SelectObject(c->pw_dc, c->pw_old);
    if (c->pw_bmp) DeleteObject(c->pw_bmp);
    if (c->pw_dc) DeleteDC(c->pw_dc);
    c->pw_dc = nullptr; c->pw_bmp = nullptr; c->pw_old = nullptr; c->pw_bits = nullptr;
    c->pw_w = c->pw_h = 0;
}

void DxRelease(Ctx *c) {
    if (c->dx) { hr_dx_destroy(c->dx); c->dx = nullptr; }
    c->dx_buf.clear();
    c->dx_buf.shrink_to_fit();
    c->dx_have_data = false;
    c->dx_wait = 0;
    c->dx_w = c->dx_h = 0;
    c->dx_mon = RECT{0, 0, 0, 0};
}

void Teardown(Ctx *c) {
    if (c->pw) { PwRelease(c); DxRelease(c); }
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

// ---------------------------------------------------------------------------------------------
// WGC session setup - runs on its own short-lived MTA thread, never on the GUI thread, and contained.
//
// Crash history (five dumps, 2026-10-09 .. 10-10): always the same signature - an EXECUTE access
// violation (the CPU jumped to a non-executable address) at "hr.exe base - 0x40000000" on the GUI
// thread, called directly from WGC Init(). Each earlier "fix" only moved the crash to the next call
// after the previous log breadcrumb (D3D11CreateDevice -> ID3D11Multithread -> CreateForWindow), and
// the pass that moved ONLY the device creation to a helper thread got through it. So the common
// factor is "native WinRT/D3D code driven from the GUI thread" (wx STA + OLE + text-services /
// compatibility-shim hooks - AcLayers.dll/apphelp.dll are loaded in every dump), not one API call.
//
// Therefore the WHOLE of Init() now runs here:
//   * on a fresh thread that joins the MTA (the WGC objects are free-threaded/agile; the GUI thread
//     is an OLE STA and is out of the picture);
//   * under a vectored exception handler that recognises exactly one signature: an EXECUTE access
//     violation on that thread. The handler parks the faulting thread forever (no unwinding through
//     foreign frames, no DLL_THREAD_DETACH), wakes the waiting caller, and the caller reports "WGC
//     unusable" so the recording falls back to the screen crop instead of killing the process. Every
//     other exception (incl. first-chance AVs that drivers handle internally) is left alone;
//   * with a step breadcrumb (g_init_step) so a contained fault / hang names the exact call;
//   * with a timeout (a hang no longer freezes the GUI forever).
// After a contained fault or a timeout the objects the thread was building are abandoned (leaked on
// purpose) - releasing half-built COM objects is exactly what could crash a second time.
// ---------------------------------------------------------------------------------------------
constexpr DWORD kInitTimeoutMs = 20000;

enum : LONG {
    kStepNone = 0, kStepRoInit, kStepDxgiFactory, kStepAdapter, kStepCreateDevice, kStepWinrtDevice,
    kStepItemFactory, kStepCreateForWindow, kStepItemSize, kStepPoolFactory, kStepCreatePool,
    kStepCreateSession, kStepSessionOptions, kStepStartCapture, kStepDone
};
const char *const kInitSteps[] = {
    "not started", "RoInitialize", "CreateDXGIFactory1", "adapter selection", "D3D11CreateDevice",
    "CreateDirect3D11DeviceFromDXGIDevice", "GraphicsCaptureItem factory", "CreateForWindow",
    "IGraphicsCaptureItem::get_Size", "frame pool factory", "CreateFreeThreaded", "CreateCaptureSession",
    "session options (cursor/border)", "StartCapture", "done"};

volatile LONG g_init_step = kStepNone;       // last step entered by the init thread (one init at a time)
volatile LONG g_session_disabled = 0;        // a contained fault / timeout happened: no more WGC this run
#define HR_STEP(s) (g_init_step = (s))

struct InitJob {
    Ctx          *c = nullptr;
    HANDLE        done = nullptr;            // manual-reset event: job finished OR faulted
    DWORD         tid = 0;                   // init thread (the VEH only reacts to this one)
    volatile LONG finished = 0;
    volatile LONG faulted = 0;
    LONG          fault_step = 0;
    ULONG_PTR     fault_target = 0;          // address the CPU tried to execute
    ULONG_PTR     fault_rsp = 0;
    ULONG_PTR     fault_ret = 0;             // first stack word = return address of the bad call
    bool          ok = false;
    ~InitJob() { if (done) CloseHandle(done); }
};

InitJob *volatile g_active_job = nullptr;    // the job the VEH watches (guarded by g_job_mu)
std::mutex        g_job_mu;                  // one init at a time

std::string HexStr(unsigned long long v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llX", v);
    return buf;
}

std::string NarrowForLog(const wchar_t *w) {
    if (!w || !*w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// "module.dll+0xOFFSET" or "?" - turns a return address from a contained fault into a lead
// (hr.exe+0x... resolves to a function with the hr.map link map).
std::string ModuleOffset(ULONG_PTR addr) {
    HMODULE mod = nullptr;
    if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(addr), &mod) && mod) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(mod, path, MAX_PATH);
        const wchar_t *base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        return NarrowForLog(base) + "+" + HexStr((unsigned long long)(addr - (ULONG_PTR)mod));
    }
    return "?";
}

// Windows compatibility shims (apphelp/AcLayers/AcGenral...) hook API calls inside the process and are
// loaded in every crash dump of this bug. Say so in the log, with the registry flag that usually
// causes it (hr.exe > Properties > Compatibility), so it is visible without opening a dump.
std::string CompatShimInfo() {
    std::string out;
    static const wchar_t *const kShimDlls[] = {L"AcLayers.dll", L"AcGenral.dll", L"AcSpecfc.dll", L"AcXtrnal.dll"};
    for (const wchar_t *dll : kShimDlls)
        if (GetModuleHandleW(dll)) { if (!out.empty()) out += ", "; out += NarrowForLog(dll); }
    if (!out.empty()) out = "compatibility shim DLLs loaded in this process: " + out;

    wchar_t env[256] = {};
    if (GetEnvironmentVariableW(L"__COMPAT_LAYER", env, 256) > 0 && env[0]) {
        if (!out.empty()) out += "; ";
        out += "__COMPAT_LAYER=" + NarrowForLog(env);
    }
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        static const HKEY kRoots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
        for (HKEY root : kRoots) {
            wchar_t val[256] = {};
            DWORD cb = sizeof(val) - sizeof(wchar_t);
            if (RegGetValueW(root, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
                             exe, RRF_RT_REG_SZ, nullptr, val, &cb) == ERROR_SUCCESS && val[0]) {
                if (!out.empty()) out += "; ";
                out += std::string(root == HKEY_CURRENT_USER ? "HKCU" : "HKLM") + " AppCompatFlags\\Layers = '" +
                       NarrowForLog(val) + "'";
            }
        }
    }
    return out;
}

// Where a faulted init thread ends up: it sleeps forever. Deliberately NOT ExitThread/TerminateThread -
// ExitThread would run DLL_THREAD_DETACH of d3d11 / the GPU driver on top of whatever state the fault
// left behind, TerminateThread could leave a lock held. A parked thread costs a few KB.
[[noreturn]] void ParkFaultedThread() {
    for (;;) Sleep(INFINITE);
}

LONG CALLBACK InitJobVeh(PEXCEPTION_POINTERS ep) {
    InitJob *j = g_active_job;
    if (!j || GetCurrentThreadId() != j->tid) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    // ExceptionInformation[0]: 0 = read, 1 = write, 8 = execute (DEP). Only "execute" is the bad-jump signature.
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 ||
        er->ExceptionInformation[0] != 8)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&j->faulted, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;

    j->fault_step = g_init_step;
    j->fault_target = er->ExceptionInformation[1];
    j->fault_rsp = (ULONG_PTR)ep->ContextRecord->Rsp;
    ULONG_PTR ret = 0;   // for a call through a bad pointer this is the caller's return address
    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(j->fault_rsp), &ret, sizeof(ret), nullptr);
    j->fault_ret = ret;

    // Resume the thread in ParkFaultedThread() on a fresh, 16-byte-aligned spot below the fault
    // (rsp % 16 == 8 as at any function entry), then wake the caller.
    ep->ContextRecord->Rip = reinterpret_cast<DWORD64>(&ParkFaultedThread);
    ep->ContextRecord->Rsp = ((ep->ContextRecord->Rsp - 0x400) & ~static_cast<DWORD64>(0xF)) - 8;
    SetEvent(j->done);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Called with g_job_mu held, so there is no race between "installed" and "usable".
void EnsureInitVeh() {
    static volatile LONG installed = 0;
    if (InterlockedCompareExchange(&installed, 1, 0) == 0) AddVectoredExceptionHandler(1, InitJobVeh);
}

// The init thread is the only thread that joins the MTA for WGC. Without this the MTA would be torn
// down the moment that thread exits, which is not what objects created inside it expect.
void KeepMtaAlive() {
    static volatile LONG done = 0;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    HMODULE ole = GetModuleHandleW(L"ole32.dll");
    if (!ole) ole = LoadLibraryW(L"ole32.dll");
    using CoIncrementMTAUsageFn = HRESULT(WINAPI *)(void **cookie);
    auto fn = ole ? reinterpret_cast<CoIncrementMTAUsageFn>(GetProcAddress(ole, "CoIncrementMTAUsage")) : nullptr;
    void *cookie = nullptr;
    if (fn) fn(&cookie);
}

// Creates c->dev / c->dc on the adapter that shows the window. Same call sequence as hr_dx_create()
// in dxgi_capture.cpp (explicit adapter, D3D_DRIVER_TYPE_UNKNOWN, non-null feature-level out-pointer).
HRESULT CreateWgcDevice(Ctx *c) {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> dc;
    D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
    std::string adapter_name;
    {
        ComPtr<IDXGIFactory1> factory;
        HR_STEP(kStepDxgiFactory);
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(factory.GetAddressOf()));
        if (FAILED(hr) || !factory) return FAILED(hr) ? hr : E_FAIL;

        // Adapter whose output shows the window (monitor of the window); adapter 0 otherwise.
        HR_STEP(kStepAdapter);
        ComPtr<IDXGIAdapter1> adapter;
        const HMONITOR mon = MonitorFromWindow(c->hwnd, MONITOR_DEFAULTTOPRIMARY);
        for (UINT ai = 0; mon; ++ai) {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapters1(ai, a.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
            bool match = false;
            for (UINT oi = 0; !match; ++oi) {
                ComPtr<IDXGIOutput> out;
                if (a->EnumOutputs(oi, out.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC od{};
                if (SUCCEEDED(out->GetDesc(&od)) && od.Monitor == mon) match = true;
            }
            if (match) { adapter = a; break; }
        }
        if (!adapter) {
            hr = factory->EnumAdapters1(0, adapter.GetAddressOf());
            if (FAILED(hr) || !adapter) return FAILED(hr) ? hr : E_FAIL;
        }
        {
            DXGI_ADAPTER_DESC1 ad{};
            if (SUCCEEDED(adapter->GetDesc1(&ad))) adapter_name = NarrowForLog(ad.Description);
        }

        HR_STEP(kStepCreateDevice);
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               nullptr, 0, D3D11_SDK_VERSION, dev.GetAddressOf(), &fl, dc.GetAddressOf());
        if (FAILED(hr) || !dev || !dc) return FAILED(hr) ? hr : E_FAIL;
    }   // adapter + factory are released here
    c->dev = std::move(dev);
    c->dc = std::move(dc);
    HrWgcLog::Note("WGC init: D3D11 device created on adapter '" + adapter_name + "', feature level " +
                HexStr((unsigned long long)fl));
    return S_OK;
}

// The actual session setup. Runs ONLY on the init thread (see InitContained()).
bool InitImpl(Ctx *c) {
    const Api &api = GetApi();
    if (!api.ok) { HrWgcLog::Warn("Window capture: Windows.Graphics.Capture is not available on this system."); return false; }
    if (!IsWindow(c->hwnd)) return false;
    HR_STEP(kStepRoInit);
    HrWgcLog::Note("WGC init: RoInitialize");
    api.RoInitialize(1 /*RO_INIT_MULTITHREADED*/);   // fresh thread: joins the MTA

    HrWgcLog::Note("WGC init: D3D11CreateDevice");
    HRESULT hr = CreateWgcDevice(c);
    if (FAILED(hr) || !c->dev || !c->dc) {
        HrWgcLog::Warn("Window capture: D3D11 device creation failed (" + HexStr((unsigned long)hr) + ") in step '" +
                    kInitSteps[g_init_step] + "'.");
        return false;
    }

    // No ID3D11Multithread::SetMultithreadProtected() here: the immediate context is only touched by
    // HrWgcCapture()/Teardown(), always under c->mu and on one thread at a time, and the frame pool is
    // polled with TryGetNextFrame() (no FrameArrived handler, so WGC never runs code on our context
    // from a thread of its own).
    HR_STEP(kStepWinrtDevice);
    HrWgcLog::Note("WGC init: D3D11 device ready, creating the WinRT device");
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(c->dev.As(&dxgi))) return false;
    void *insp = nullptr;
    hr = api.CreateD3D11DeviceFromDXGIDevice(dxgi.Get(), &insp);
    if (FAILED(hr) || !insp) { HrWgcLog::Warn("Window capture: CreateDirect3D11DeviceFromDXGIDevice failed."); return false; }
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
    HR_STEP(kStepItemFactory);
    HrWgcLog::Note("WGC init: activating GraphicsCaptureItem factory");
    hr = GetFactory(L"Windows.Graphics.Capture.GraphicsCaptureItem", kIidInterop, reinterpret_cast<void **>(&interop));
    if (FAILED(hr) || !interop) { HrWgcLog::Warn("Window capture: GraphicsCaptureItem factory unavailable."); return false; }
    HR_STEP(kStepCreateForWindow);
    HrWgcLog::Note("WGC init: CreateForWindow");
    hr = interop->CreateForWindow(c->hwnd, kIidItem, reinterpret_cast<void **>(&c->item));
    interop->Release();
    if (FAILED(hr) || !c->item) {
        HrWgcLog::Warn("Window capture: this window can't be captured (CreateForWindow " + HexStr((unsigned long)hr) +
                    ") - falling back to screen crop.");
        return false;
    }

    HR_STEP(kStepItemSize);
    SizeInt32 sz{0, 0};
    if (FAILED(c->item->get_Size(&sz)) || sz.Width <= 0 || sz.Height <= 0) { sz.Width = c->canvas_w; sz.Height = c->canvas_h; }
    sz.Width  = std::min<INT32>(std::max<INT32>(sz.Width, 16), 16384);
    sz.Height = std::min<INT32>(std::max<INT32>(sz.Height, 16), 16384);

    HrFramePoolStatics2 *stat2 = nullptr;
    HR_STEP(kStepPoolFactory);
    hr = GetFactory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", kIidPoolStat2, reinterpret_cast<void **>(&stat2));
    if (FAILED(hr) || !stat2) { HrWgcLog::Warn("Window capture: frame pool factory unavailable."); return false; }
    HR_STEP(kStepCreatePool);
    HrWgcLog::Note("WGC init: CreateFreeThreaded frame pool");
    hr = stat2->CreateFreeThreaded(c->winrt_dev, kPixelFormatBgra8, 2, sz, reinterpret_cast<void **>(&c->pool));
    stat2->Release();
    if (FAILED(hr) || !c->pool) { HrWgcLog::Warn("Window capture: CreateFreeThreaded failed."); return false; }
    c->pool_w = sz.Width; c->pool_h = sz.Height;

    HR_STEP(kStepCreateSession);
    hr = c->pool->CreateCaptureSession(c->item, reinterpret_cast<void **>(&c->session));
    if (FAILED(hr) || !c->session) { HrWgcLog::Warn("Window capture: CreateCaptureSession failed."); return false; }

    // Optional niceties - every failure here is ignored (older builds simply lack the interfaces).
    HR_STEP(kStepSessionOptions);
    if (SUCCEEDED(c->session->QueryInterface(kIidSession2, reinterpret_cast<void **>(&c->session2))) && c->session2)
        c->session2->put_IsCursorCaptureEnabled(c->cursor ? TRUE : FALSE);
    {
        HrSession3 *s3 = nullptr;
        if (SUCCEEDED(c->session->QueryInterface(kIidSession3, reinterpret_cast<void **>(&s3))) && s3) {
            s3->put_IsBorderRequired(FALSE);   // no yellow frame around the window (Windows 11 / recent 10)
            s3->Release();
        }
    }

    HR_STEP(kStepStartCapture);
    HrWgcLog::Note("WGC init: StartCapture");
    hr = c->session->StartCapture();
    if (FAILED(hr)) { HrWgcLog::Warn("Window capture: StartCapture failed (" + HexStr((unsigned long)hr) + ")."); return false; }

    c->geom_w = c->geom_h = -1;
    c->ok = true;
    return true;
}

DWORD WINAPI InitThreadProc(LPVOID param) {
    auto *holder = static_cast<std::shared_ptr<InitJob> *>(param);
    std::shared_ptr<InitJob> j = *holder;         // keeps the job alive even if the caller gave up waiting
    delete holder;
    KeepMtaAlive();
    j->ok = InitImpl(j->c);
    HR_STEP(kStepDone);
    InterlockedExchange(&j->finished, 1);
    SetEvent(j->done);
    return 0;
}

enum class InitResult { kOk, kFailed, kFaulted, kTimeout };

// Everything the init thread may have half-built is dropped WITHOUT Release()/Close(): after a bad
// jump or a hang those objects can't be trusted, and Teardown() on them could crash a second time.
void AbandonCtx(Ctx *c) {
    c->session = nullptr; c->session2 = nullptr; c->pool = nullptr; c->item = nullptr; c->winrt_dev = nullptr;
    c->dev.Detach(); c->dc.Detach(); c->staging.Detach();
    c->pool_w = c->pool_h = c->st_w = c->st_h = 0;
    c->ok = false;
    c->dead = true;
}

InitResult InitContained(Ctx *c) {
    std::lock_guard<std::mutex> lk(g_job_mu);
    if (g_session_disabled) return InitResult::kFailed;
    EnsureInitVeh();

    const std::string shims = CompatShimInfo();
    if (!shims.empty()) HrWgcLog::Warn("Window capture: " + shims + ". Windows hooks API calls of such processes; if WGC "
                                    "misbehaves, clear hr.exe > Properties > Compatibility (see homrec.log).");

    auto job = std::make_shared<InitJob>();
    job->c = c;
    job->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job->done) return InitResult::kFailed;
    g_init_step = kStepNone;

    auto *holder = new std::shared_ptr<InitJob>(job);
    DWORD tid = 0;
    HANDLE th = CreateThread(nullptr, 0, InitThreadProc, holder, CREATE_SUSPENDED, &tid);
    if (!th) { delete holder; return InitResult::kFailed; }
    job->tid = tid;                 // set while the thread is still suspended: the VEH can never miss it
    g_active_job = job.get();
    ResumeThread(th);
    const DWORD w = WaitForSingleObject(job->done, kInitTimeoutMs);
    g_active_job = nullptr;
    CloseHandle(th);

    if (job->faulted) {
        InterlockedExchange(&g_session_disabled, 1);
        const LONG st = job->fault_step;
        HrWgcLog::Warn("Window capture: Windows.Graphics.Capture jumped to an invalid address (" + HexStr(job->fault_target) +
                    "; return address " + ModuleOffset(job->fault_ret) + ", stack " + HexStr(job->fault_rsp) +
                    ") in step '" + kInitSteps[(st >= 0 && st <= kStepDone) ? st : 0] +
                    "'. The fault was contained (helper thread parked), WGC is switched off until HomRec is "
                    "restarted - falling back to the screen crop.");
        AbandonCtx(c);
        return InitResult::kFaulted;
    }
    if (w != WAIT_OBJECT_0 || !job->finished) {
        InterlockedExchange(&g_session_disabled, 1);
        const LONG st = g_init_step;
        HrWgcLog::Warn("Window capture: Windows.Graphics.Capture did not finish within " +
                    std::to_string(kInitTimeoutMs / 1000) + " s (stuck in step '" +
                    kInitSteps[(st >= 0 && st <= kStepDone) ? st : 0] + "') - falling back to the screen crop.");
        AbandonCtx(c);
        return InitResult::kTimeout;
    }
    return job->ok ? InitResult::kOk : InitResult::kFailed;
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

// ---------------------------------------------------------------------------------------------
// PrintWindow backend (2.4.1) - "record ONE window" WITHOUT any hand-written WinRT call.
//
// Why it exists: on some PCs the native Windows.Graphics.Capture setup above jumps to an invalid
// address inside CreateForWindow (six crash dumps / log lines, identical every time, see
// CHANGELOG 2.4.0 crash passes 1-5). The fault is contained, but then window recording used to fall
// back to the SCREEN CROP - which for a maximized window is practically the whole screen plus
// everything that lies on top of it ("I select Chrome and the entire screen is recorded").
//
// PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT) (Windows 8.1+) asks DWM for the window's own
// composited picture, so the result is that ONE window - not the desktop under its rectangle - even
// when other windows cover it. It is plain Win32/GDI: nothing in here can take the process down the
// way the WinRT path did. The price is CPU: the picture is read back and converted on the CPU (the
// same path WGC frames already took), so it is the fallback, not the first choice.
// ---------------------------------------------------------------------------------------------
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

bool PwAvailable() {
    using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return false;
    auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(nt, "RtlGetVersion"));
    if (!fn) return false;
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return false;
    return vi.dwMajorVersion > 6 || (vi.dwMajorVersion == 6 && vi.dwMinorVersion >= 3);   // 8.1+
}

// Draws the live mouse cursor into the DIB - PrintWindow never includes it. Only when the cursor is
// really over THIS window (not over something that happens to lie on top of it).
void PwDrawCursor(Ctx *c, const RECT &wr) {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
    if (!PtInRect(&wr, ci.ptScreenPos)) return;
    HWND under = WindowFromPoint(ci.ptScreenPos);
    if (!under || GetAncestor(under, GA_ROOT) != GetAncestor(c->hwnd, GA_ROOT)) return;
    int hx = 0, hy = 0;
    ICONINFO ii{};
    if (GetIconInfo(ci.hCursor, &ii)) {
        hx = (int)ii.xHotspot; hy = (int)ii.yHotspot;
        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
    }
    DrawIconEx(c->pw_dc, ci.ptScreenPos.x - wr.left - hx, ci.ptScreenPos.y - wr.top - hy,
               ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
}

// ---------------------------------------------------------------------------------------------
// 7th pass: smart mode + a much cheaper exact mode
//
// Measured on a real PC (Chrome, 1080p): the PrintWindow backend cost ~30% CPU and made every window
// dragged over the recorded one stutter. Two reasons, both fixed here:
//   * PrintWindow makes the target (and DWM) redraw the whole window for every single frame, even when
//     nothing changed, and every frame went through a full colour conversion in the pipeline.
//   * a window that is simply in front (the normal case) needs none of that: its picture is just a part
//     of the screen.
// So, per call:
//   1. smart mode on and the window uncovered (and on one monitor)  ->  cut it out of the screen duplication
//      (cheap; DXGI says "nothing changed" for free, and only the changed rows are copied);
//   2. otherwise PrintWindow, but (a) no per-frame buffer clear, (b) paced: never more than 50% duty
//      cycle, optional fps cap, slower polling while the picture stays identical and the user is idle,
//      and (c) an identical picture is reported as "no new frame" so the pipeline re-sends the last
//      converted frame (that path costs almost nothing) instead of converting the same pixels again.
// ---------------------------------------------------------------------------------------------
constexpr int kFallback = -100;       // internal: "leave the screen reading, use the exact capture"
constexpr int kFallbackKeep = -101;   // internal: "exact capture for THIS frame, but stay in screen mode (it is starting up)"

// True when another program's window lies over `want` (the window we record). Walks the z-order ABOVE the
// target. Windows that cannot change what the screen duplication shows are skipped: invisible /
// minimized / cloaked ones, click-through overlays, windows hidden from capture, and the target
// program's own popups (menus, tooltips - they belong to the picture).
bool CoveredByOthers(HWND target, const RECT &want) {
    DWORD tpid = 0;
    GetWindowThreadProcessId(target, &tpid);
    int guard = 0;
    for (HWND w = GetWindow(target, GW_HWNDPREV); w && guard < 4096; w = GetWindow(w, GW_HWNDPREV), ++guard) {
        if (!IsWindowVisible(w)) continue;
        RECT r{}, t{};
        if (!GetWindowRect(w, &r) || !IntersectRect(&t, &r, &want)) continue;
        if (IsIconic(w)) continue;
        const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        if ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT)) continue;          // click-through overlay
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == tpid) continue;                                              // the program's own popups
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) continue;
        DWORD aff = 0;
        if (GetWindowDisplayAffinity(w, &aff) && aff == 0x11 /*WDA_EXCLUDEFROMCAPTURE*/) continue;
        RECT fr{};   // visible frame, without the invisible resize border Windows adds around most windows
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr))) &&
            !IntersectRect(&t, &fr, &want)) continue;
        return true;
    }
    return false;
}

// Makes sure c->dx duplicates the monitor `mr`. False = not possible right now (the caller uses the exact
// capture); permanent problems set c->dx_broken.
bool DxEnsure(Ctx *c, const RECT &mr, ULONGLONG now) {
    if (c->dx && EqualRect(&c->dx_mon, &mr)) return true;
    if (now < c->dx_retry_at) return false;

    const int mw = (int)(mr.right - mr.left), mh = (int)(mr.bottom - mr.top);
    int out_idx = -1;
    const int n = hr_dx_output_count(0);
    for (int i = 0; i < n && out_idx < 0; ++i) {
        int ox = 0, oy = 0, ow = 0, oh = 0;
        if (hr_dx_output_desc(0, i, &ox, &oy, &ow, &oh, nullptr, 0) && ox == mr.left && oy == mr.top &&
            ow == mw && oh == mh) out_idx = i;
    }
    if (out_idx < 0) {
        c->dx_retry_at = now + 2000;     // e.g. the monitor sits on another GPU: exact capture, try again later
        HrWgcLog::Info("Smart window capture: monitor " + std::to_string(mw) + "x" + std::to_string(mh) + " at " +
                       std::to_string(mr.left) + "," + std::to_string(mr.top) +
                       " is not an output of the first GPU - using the exact capture for this window.");
        return false;
    }
    DxRelease(c);
    void *dx = hr_dx_create(0, out_idx);
    if (!dx) {
        if (++c->dx_fail >= 3) {
            c->dx_broken = true;
            HrWgcLog::Warn("Smart window capture: the screen duplication can't be created (3 tries) - smart mode is "
                           "off for this recording, the exact window capture is used all the time.");
        } else {
            c->dx_retry_at = now + 1500;
            HrWgcLog::Info("Smart window capture: screen duplication of output " + std::to_string(out_idx) +
                           " failed (try " + std::to_string(c->dx_fail) + ") - using the exact capture meanwhile.");
        }
        return false;
    }
    int dw = 0, dh = 0;
    hr_dx_get_size(dx, &dw, &dh);
    if (dw != mw || dh != mh || dw <= 0 || dh <= 0) {
        hr_dx_destroy(dx);
        c->dx_broken = true;
        HrWgcLog::Warn("Smart window capture: the duplication reports " + std::to_string(dw) + "x" + std::to_string(dh) +
                       " for a " + std::to_string(mw) + "x" + std::to_string(mh) +
                       " monitor (scaling / rotation?) - smart mode is off, the exact window capture is used.");
        return false;
    }
    c->dx = dx;
    c->dx_mon = mr;
    c->dx_w = dw; c->dx_h = dh;
    c->dx_buf.assign((size_t)dw * dh * 4, 0);
    hr_dx_set_output_size(dx, dw, dh);   // tell the duplication the real size of dx_buf: it never stretches into it
    c->dx_have_data = false;
    c->dx_wait = 0;
    c->mode = 0;                         // freshly created: the caller switches over (and primes it) explicitly
    c->need_compose = true;
    HrWgcLog::Info("Smart window capture: duplicating output " + std::to_string(out_idx) + " (" + std::to_string(dw) +
                   "x" + std::to_string(dh) + ") for the window.");
    return true;
}

// Decides, for this call, between the screen duplication (true) and the exact capture (false).
bool SmartWantsScreen(Ctx *c, const RECT &want, RECT &mon_out, ULONGLONG now) {
    if (!c->smart || c->dx_broken) return false;
    const int ww = (int)(want.right - want.left), wh = (int)(want.bottom - want.top);
    if ((ww & ~1) > c->canvas_w || (wh & ~1) > c->canvas_h) return false;   // would have to be scaled: the exact path does that
    const HMONITOR mon = MonitorFromRect(&want, MONITOR_DEFAULTTONULL);
    if (!mon) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    const RECT &m = mi.rcMonitor;
    if (want.left < m.left || want.top < m.top || want.right > m.right || want.bottom > m.bottom)
        return false;                                                // spans monitors / partly off-screen
    mon_out = m;

    if (c->mode == 1) return !CoveredByOthers(c->hwnd, want);        // on the screen: check every frame (cheap)

    // On the exact capture: look again every 250 ms, and go back to the screen only after the window has
    // been uncovered for 400 ms (dragging a window around over it must not make the source flap).
    if (now < c->next_occl_check) return false;
    c->next_occl_check = now + 250;
    if (CoveredByOthers(c->hwnd, want)) { c->clear_since = 0; return false; }
    if (!c->clear_since) c->clear_since = now;
    return now - c->clear_since >= 400;
}

// Draws the pointer into the canvas (screen mode), only when it really is over the recorded window.
bool CursorOverWindow(Ctx *c, const RECT &want, int *x, int *y, unsigned long long *h) {
    int vis = 0, px = 0, py = 0;
    unsigned long long hh = 0;
    if (!hr_cursor_state(&vis, &px, &py, &hh) || !vis) return false;
    POINT pt{px, py};
    if (!PtInRect(&want, pt)) return false;
    HWND under = WindowFromPoint(pt);
    if (!under || GetAncestor(under, GA_ROOT) != GetAncestor(c->hwnd, GA_ROOT)) return false;
    *x = px; *y = py; *h = hh;
    return true;
}

// Screen-duplication frame. kOk = canvas rewritten, kTimeout = nothing changed (canvas untouched),
// kFallback = use the exact capture for this frame.
int DxFrame(Ctx *c, const RECT &want, const RECT &mon, uint8_t *out, int timeout_ms, ULONGLONG now) {
    if (!DxEnsure(c, mon, now)) return kFallback;
    if (c->mode != 1) {
        // Coming back from the exact capture: restart the duplication so the next delivery is a complete,
        // current picture (it would otherwise only carry what changed since it was last read).
        hr_dx_reset(c->dx);
        c->dx_have_data = false;
        c->dx_wait = 0;
        c->need_compose = true;
        c->mode = 1;
        HrWgcLog::Info("Smart window capture: window is uncovered -> reading the screen (cheap mode).");
    }

    const int W = c->dx_w, H = c->dx_h;
    const int rw = (int)(want.right - want.left) & ~1, rh = (int)(want.bottom - want.top) & ~1;
    const int rx = (int)(want.left - mon.left), ry = (int)(want.top - mon.top);
    if (rw < 2 || rh < 2 || rx < 0 || ry < 0 || rx + rw > W || ry + rh > H) return kFallback;

    const int r = hr_dx_capture(c->dx, c->dx_buf.data(), std::max(0, std::min(timeout_ms, 33)));
    bool new_pixels = false;
    if (r == kOk) {
        int y0 = 0, y1 = 0;
        const bool partial = hr_dx_get_dirty_rows(c->dx, &y0, &y1) != 0;
        // Only some rows of the screen changed: if none of them is a row of our window the picture is the same.
        new_pixels = !partial || (y1 > ry && y0 < ry + rh);
        c->dx_have_data = true;
        c->dx_fail = 0;
        ++c->st_dx_new;
    } else if (r == kTimeout) {
        ++c->st_dx_idle;
        if (!c->dx_have_data) {
            // Nothing delivered since the (re)start: the buffer is empty, don't build a picture from it.
            if (++c->dx_wait > 40) {
                c->dx_wait = 0;
                if (++c->dx_fail >= 3) {
                    c->dx_broken = true;
                    HrWgcLog::Warn("Smart window capture: the screen duplication delivers nothing - smart mode is "
                                   "off, the exact window capture is used all the time.");
                } else {
                    c->dx_retry_at = now + 1500;
                    HrWgcLog::Info("Smart window capture: the screen duplication delivered no first picture - trying again later.");
                }
                return kFallback;
            }
            return kFallbackKeep;
        }
    } else if (r == kLost) {
        hr_dx_reset(c->dx);
        c->dx_have_data = false;
        c->dx_wait = 0;
        if (++c->dx_fail >= 5) {
            c->dx_broken = true;
            HrWgcLog::Warn("Smart window capture: the screen duplication keeps getting lost - smart mode is off.");
            return kFallback;
        }
        return kFallbackKeep;
    } else {
        c->dx_broken = true;
        HrWgcLog::Warn("Smart window capture: the screen duplication reported an error (" + std::to_string(r) +
                       ") - smart mode is off, the exact window capture is used all the time.");
        return kFallback;
    }

    const RECT crop{want.left, want.top, want.left + rw, want.top + rh};
    int cx = 0, cy = 0;
    unsigned long long ch = 0;
    const bool cur_here = c->cursor && CursorOverWindow(c, want, &cx, &cy, &ch);
    const bool cur_changed = (cur_here != c->cur_drawn) || (cur_here && (cx != c->cur_x || cy != c->cur_y || ch != c->cur_handle));

    if (!new_pixels && !c->need_compose && EqualRect(&crop, &c->last_crop) && !cur_changed) return kTimeout;

    Compose(c, c->dx_buf.data() + ((size_t)ry * W + rx) * 4, W * 4, rw, rh, out);
    if (cur_here) {
        const int ox = (c->canvas_w - std::min(rw, c->canvas_w)) / 2, oy = (c->canvas_h - std::min(rh, c->canvas_h)) / 2;
        hr_composite_cursor(out, c->canvas_w, c->canvas_h, (int)want.left - ox, (int)want.top - oy);
    }
    c->cur_drawn = cur_here; c->cur_x = cx; c->cur_y = cy; c->cur_handle = ch;
    c->last_crop = crop;
    c->need_compose = false;
    return kOk;
}

// 64-bit hash of a w x h BGRA region (4 independent lanes keep the multiplier chains busy: ~0.5 ms for 1080p).
uint64_t HashRegion(const uint8_t *base, int pitch, int w, int h) {
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ (uint64_t)w, h1 = 0xC2B2AE3D27D4EB4Full ^ (uint64_t)h,
             h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    const size_t words = (size_t)w * 4 / 8;
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = base + (size_t)y * pitch;
        size_t i = 0;
        for (; i + 4 <= words; i += 4) {
            uint64_t a, b, c2, d;
            std::memcpy(&a, row + (i + 0) * 8, 8);
            std::memcpy(&b, row + (i + 1) * 8, 8);
            std::memcpy(&c2, row + (i + 2) * 8, 8);
            std::memcpy(&d, row + (i + 3) * 8, 8);
            h0 = (h0 ^ a) * 0x100000001B3ull + (h0 >> 29);
            h1 = (h1 ^ b) * 0x100000001B3ull + (h1 >> 29);
            h2 = (h2 ^ c2) * 0x100000001B3ull + (h2 >> 29);
            h3 = (h3 ^ d) * 0x100000001B3ull + (h3 >> 29);
        }
        for (; i < words; ++i) {
            uint64_t a;
            std::memcpy(&a, row + i * 8, 8);
            h0 = (h0 ^ a) * 0x100000001B3ull + (h0 >> 29);
        }
    }
    return h0 ^ (h1 * 31) ^ (h2 * 131) ^ (h3 * 1031);
}

void StatsTick(Ctx *c, ULONGLONG now) {
    if (!c->st_t0) { c->st_t0 = now; return; }
    if (now - c->st_t0 < 5000) return;
    const double secs = (double)(now - c->st_t0) / 1000.0;
    HrWgcLog::Debug(std::string("window capture stats (") + (c->mode == 1 ? "screen" : "exact") + " mode, last " +
                    std::to_string((int)(secs + 0.5)) + " s): calls " + std::to_string(c->st_calls) +
                    ", exact captures " + std::to_string(c->st_pw) + " (avg " +
                    std::to_string(c->st_pw ? c->st_pw_ms / c->st_pw : 0) + " ms, max " + std::to_string(c->st_pw_max) +
                    " ms), identical pictures skipped " + std::to_string(c->st_static) + ", screen frames " +
                    std::to_string(c->st_dx_new) + " (idle " + std::to_string(c->st_dx_idle) + "), held " +
                    std::to_string(c->st_hold) + ", static streak " + std::to_string(c->static_streak));
    c->st_t0 = now;
    c->st_calls = c->st_pw = c->st_pw_ms = c->st_pw_max = c->st_dx_new = c->st_dx_idle = c->st_hold = c->st_static = 0;
}

// Exact frame via PrintWindow. `wr` = GetWindowRect() of the window. kOk = canvas rewritten; kTimeout = the
// picture is the same as before / not due yet (canvas untouched).
int PwFrame(Ctx *c, const RECT &wr, uint8_t *out, ULONGLONG now) {
    const int ww = (int)(wr.right - wr.left), wh = (int)(wr.bottom - wr.top);
    if (ww < 2 || wh < 2 || ww > 16384 || wh > 16384) return kTimeout;

    // --- pacing -----------------------------------------------------------------------------------
    // The user is active -> back to the full rate; idle and the picture stays identical -> poll slower.
    // Input only counts while the recorded window is the one in front (typing / clicking elsewhere does not
    // change it; its own animations are caught by the identical-picture test).
    LASTINPUTINFO li{};
    li.cbSize = sizeof(li);
    if (GetLastInputInfo(&li) && li.dwTime != c->last_input_tick) {
        c->last_input_tick = li.dwTime;
        const HWND fg = GetForegroundWindow();
        if (fg && GetAncestor(fg, GA_ROOT) == GetAncestor(c->hwnd, GA_ROOT)) {
            c->static_streak = 0;
            c->next_pw_at = 0;           // look right away, don't wait out a slow-poll interval
        }
    }
    DWORD min_gap = 0;
    if (c->fps_cap > 0) min_gap = (DWORD)(1000 / c->fps_cap);
    if (c->static_streak >= 30)      min_gap = std::max<DWORD>(min_gap, 250);
    else if (c->static_streak >= 4)  min_gap = std::max<DWORD>(min_gap, 100);
    // Never spend more than half the time inside PrintWindow: a window that needs 40 ms per picture is
    // asked at most every 80 ms (this is what keeps other windows from stuttering while the target redraws).
    min_gap = std::max<DWORD>(min_gap, c->last_pw_ms);
    if (c->have_hash && now < c->next_pw_at) return kTimeout;

    if (!c->pw_dc) {
        HDC screen = GetDC(nullptr);
        c->pw_dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!c->pw_dc) return kError;
    }
    if (!c->pw_bmp || c->pw_w != ww || c->pw_h != wh) {     // first frame, or the window was resized
        if (c->pw_bmp) {
            SelectObject(c->pw_dc, c->pw_old);
            DeleteObject(c->pw_bmp);
            c->pw_bmp = nullptr; c->pw_bits = nullptr; c->pw_old = nullptr;
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = ww;
        bi.bmiHeader.biHeight = -wh;                        // negative = top-down rows
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        HBITMAP bmp = CreateDIBSection(c->pw_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) { if (bmp) DeleteObject(bmp); return kError; }
        c->pw_bmp = bmp;
        c->pw_old = static_cast<HBITMAP>(SelectObject(c->pw_dc, bmp));
        c->pw_bits = static_cast<uint8_t *>(bits);          // CreateDIBSection memory starts zeroed (black)
        c->pw_w = ww; c->pw_h = wh;
        c->have_hash = false;
    }
    // (No per-frame clear any more: the invisible resize border is never painted and is cropped away below,
    //  so it stays black from the allocation - the old 8 MB memset per frame was pure cost.)

    const DWORD t0 = GetTickCount();
    const BOOL ok = PrintWindow(c->hwnd, c->pw_dc, PW_RENDERFULLCONTENT);
    const DWORD took = GetTickCount() - t0;
    ++c->st_pw; c->st_pw_ms += took; c->st_pw_max = std::max<uint32_t>(c->st_pw_max, took);
    c->last_pw_ms = took;
    c->next_pw_at = now + std::max<DWORD>(min_gap, took);
    if (!ok) {
        if (!c->pw_fail_warned) {
            c->pw_fail_warned = true;
            HrWgcLog::Warn("Window capture (PrintWindow): Windows refused to draw this window - the last picture is repeated until it works.");
        }
        return kTimeout;
    }
    if (took > 250 && !c->pw_slow_warned) {
        c->pw_slow_warned = true;
        HrWgcLog::Warn("Window capture (PrintWindow): the window needs " + std::to_string(took) +
                       " ms to draw itself - the recording will drop frames while it is that slow.");
    }
    if (c->cursor) PwDrawCursor(c, wr);

    // The part of the window that gets recorded (whole window = DWM frame bounds, or just the client
    // area), expressed relative to the GetWindowRect() bitmap above.
    int rx = 0, ry = 0, rw = ww, rh = wh;
    RECT frame{}, client{};
    if (WindowRects(c->hwnd, frame, client)) {
        const RECT &want = c->client_only ? client : frame;
        rx = (int)(want.left - wr.left); ry = (int)(want.top - wr.top);
        rw = (int)(want.right - want.left); rh = (int)(want.bottom - want.top);
    }
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > ww) rw = ww - rx;
    if (ry + rh > wh) rh = wh - ry;
    rw &= ~1; rh &= ~1;
    if (rw < 2 || rh < 2) return kTimeout;

    // Same picture as last time (and the canvas already holds it)? Then there is nothing new to hand over:
    // reporting "no new frame" lets the pipeline re-send the frame it already converted instead of
    // converting identical pixels again (the pipeline does not count this as a dropped frame in the
    // overload warning - only real stalls are).
    const uint8_t *region = c->pw_bits + ((size_t)ry * ww + rx) * 4;
    const uint64_t hsh = HashRegion(region, ww * 4, rw, rh) ^ ((uint64_t)rw << 20) ^ ((uint64_t)rh << 40);
    if (c->have_hash && !c->need_compose && hsh == c->last_hash) {
        ++c->static_streak;
        ++c->st_static;
        return kTimeout;
    }
    c->last_hash = hsh;
    c->have_hash = true;
    c->static_streak = 0;
    c->need_compose = false;
    Compose(c, region, ww * 4, rw, rh, out);
    c->cur_drawn = false;
    return kOk;
}

// One frame. Called with c->mu held. Same return codes as HrWgcCapture().
int PwCapture(Ctx *c, uint8_t *out, int timeout_ms) {
    const ULONGLONG now = GetTickCount64();
    ++c->st_calls;
    StatsTick(c, now);

    if (!IsWindow(c->hwnd)) {
        // The window is gone (closed / re-created). Hold the last picture: the recording keeps its timeline
        // and RecordingController re-targets us (HrWgcSetTarget) as soon as the window is found again.
        if (!c->gone_logged) {
            c->gone_logged = true;
            HrWgcLog::Note("Window capture: the recorded window is gone - holding the last picture until it is found again.");
        }
        ++c->st_hold;
        return kTimeout;
    }
    if (c->gone_logged) { c->gone_logged = false; HrWgcLog::Info("Window capture: the recorded window is back."); }
    if (IsIconic(c->hwnd)) { ++c->st_hold; return kTimeout; }       // a minimised window has nothing to draw

    RECT wr{};
    if (!GetWindowRect(c->hwnd, &wr)) return kTimeout;
    RECT frame{}, client{};
    if (!WindowRects(c->hwnd, frame, client)) return kTimeout;
    const RECT &want = c->client_only ? client : frame;
    if (want.right - want.left < 2 || want.bottom - want.top < 2) { ++c->st_hold; return kTimeout; }

    RECT mon{};
    if (SmartWantsScreen(c, want, mon, now)) {
        const int r = DxFrame(c, want, mon, out, timeout_ms, now);
        if (r == kFallbackKeep) return PwFrame(c, wr, out, now);   // starting up: exact frame, stay in screen mode
        if (r != kFallback) return r;
        // not possible this frame (duplication not ready / failed): exact capture below
    }
    if (c->mode == 1) {
        c->mode = 0;                         // the window got covered (or the duplication gave up)
        c->need_compose = true;
        c->have_hash = false;
        c->static_streak = 0;
        c->clear_since = 0;
        c->next_occl_check = now + 250;
        c->next_pw_at = 0;
        HrWgcLog::Info("Smart window capture: window is covered / duplication unavailable -> exact window capture (PrintWindow).");
    }
    return PwFrame(c, wr, out, now);
}

// ---- "native" (WinRT) Windows.Graphics.Capture: is it allowed to run in this process? ----------
// Separate from PrintWindow: after a crash / contained fault the native path stays off, but window
// recording itself stays available through PrintWindow.
std::wstring NativeBadPath() { return HrLogPaths::LogFilePath(L"wgc_native_bad.flag"); }
volatile LONG g_native_bad = -1;      // -1 = not looked yet, 0 = fine, 1 = a contained fault happened on this PC

bool NativeMarkedBad() {
    if (g_native_bad < 0) {
        bool bad = false;
        const std::wstring path = NativeBadPath();
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char buf[256] = {};
            DWORD n = 0;
            ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
            CloseHandle(h);
            buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
            if (std::strstr(buf, kGuardGen)) bad = true;      // written by this generation of the code
            else DeleteFileW(path.c_str());                   // an older build's verdict - give WGC another go
        }
        InterlockedCompareExchange(&g_native_bad, bad ? 1 : 0, -1);
    }
    return g_native_bad == 1;
}

void MarkNativeBad() {
    InterlockedExchange(&g_native_bad, 1);
    HANDLE h = CreateFileW(NativeBadPath().c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        static const char note[] = HR_WGC_GUARD_GEN "\r\n"
            "HomRec: native Windows.Graphics.Capture faulted on this PC (see homrec.log). Window recording uses the "
            "PrintWindow backend instead. Delete this file (or re-select the window capture method in Settings) to retry.\r\n";
        DWORD n = 0;
        WriteFile(h, note, (DWORD)(sizeof(note) - 1), &n, nullptr);
        CloseHandle(h);
    }
}

bool NativeWgcAllowed() {
    if (GuardTripped()) return false;           // the previous run died inside native WGC
    if (g_session_disabled) return false;       // a fault/hang was contained earlier in THIS run
    if (NativeMarkedBad()) return false;        // ... or in an earlier run: don't walk into it again
    // The very first call loads combase.dll / d3d11.dll and resolves the WinRT entry points.
    static volatile LONG api_ready = 0;
    if (!api_ready) {
        GuardScope g;
        const bool ok = OsBuildAtLeast(18362) && GetApi().ok;
        InterlockedExchange(&api_ready, 1);
        return ok;
    }
    return OsBuildAtLeast(18362) && GetApi().ok;
}

std::shared_ptr<Ctx> MakeCtx(HWND hwnd, int canvas_w, int canvas_h, bool client_only, bool draw_cursor) {
    auto sp = std::make_shared<Ctx>();
    sp->hwnd = hwnd;
    sp->canvas_w = canvas_w & ~1;
    sp->canvas_h = canvas_h & ~1;
    sp->client_only = client_only;
    sp->cursor = draw_cursor;
    return sp;
}

void *CreatePrintWindowCtx(HWND hwnd, int canvas_w, int canvas_h, bool client_only, bool draw_cursor,
                           const HrWgcOptions &opt) {
    auto sp = MakeCtx(hwnd, canvas_w, canvas_h, client_only, draw_cursor);
    Ctx *c = sp.get();
    c->pw = true;
    c->ok = true;
    c->smart = opt.smart;
    c->fps_cap = std::max(0, std::min(opt.fps_cap, 240));
    {
        std::lock_guard<std::mutex> lk(g_registry_mu);
        g_registry.push_back(sp);
    }
    HrWgcLog::Note("Window capture: using the PrintWindow backend (" + std::to_string(c->canvas_w) + "x" +
                   std::to_string(c->canvas_h) + (client_only ? ", content area only" : ", whole window") +
                   (c->smart ? ", SMART mode: cheap screen reading while the window is uncovered, exact capture when covered"
                             : ", exact capture only") +
                   (c->fps_cap > 0 ? ", exact captures capped at " + std::to_string(c->fps_cap) + "/s" : std::string()) +
                   ") - records this ONE window, even when other windows cover it.");
    return c;
}

} // namespace

// ---------------------------------------------------------------------------------------------
bool HrWgcCrashGuardTripped() { return GuardTripped(); }

void HrWgcResetCrashGuard() {
    AcquireSRWLockExclusive(&g_guard_lock);
    if (g_guard_depth == 0) DeleteFileW(GuardPath().c_str());
    ReleaseSRWLockExclusive(&g_guard_lock);
    InterlockedExchange(&g_guard_state, 1);
    // The user asked for another go at native WGC: forget the contained-fault verdicts as well.
    DeleteFileW(NativeBadPath().c_str());
    InterlockedExchange(&g_native_bad, 0);
    InterlockedExchange(&g_session_disabled, 0);
}

bool HrWgcSupported() {
    // True when a window can be recorded as ITSELF: native Windows.Graphics.Capture if it is allowed to
    // run, otherwise the PrintWindow backend (Windows 8.1+). Only when neither works does the caller
    // fall back to the screen crop.
    return NativeWgcAllowed() || PwAvailable();
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
    // Used to activate the GraphicsCaptureItem factory and call CreateForWindow() right here - on the
    // UI thread, in the middle of "select a window" (RefreshPreviewSettings -> ResolveCaptureSize).
    // That is native WinRT code that can take the whole process down (2.4.0 crash; the
    // 2026-10-07 dumps), and it only duplicated what HrWgcCreate() does a moment later anyway.
    // HrWgcCreate() already reports a window Windows refuses (CreateForWindow failing) and the caller
    // falls back to the screen crop, so the probe now only does the cheap Win32 checks and touches
    // no WinRT object at all.
    if (!hwnd || !IsWindow(hwnd) || !HrWgcSupported()) return false;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;   // never WGC-capture HomRec's own windows
    return true;
}

void *HrWgcCreate(HWND hwnd, int canvas_w, int canvas_h, bool client_only, bool draw_cursor,
                  const HrWgcOptions *opt) {
    HrWgcLog::WriteSessionHeaderOnce();
    const HrWgcOptions options = opt ? *opt : HrWgcOptions();
    HrWgcLog::Info("HrWgcCreate: hwnd " + std::to_string((unsigned long long)(uintptr_t)hwnd) + ", canvas " +
                   std::to_string(canvas_w) + "x" + std::to_string(canvas_h) + ", " +
                   (client_only ? "client area" : "whole window") + ", cursor " + (draw_cursor ? "on" : "off") +
                   ", smart " + (options.smart ? "on" : "off") + ", fps cap " + std::to_string(options.fps_cap));
    if (!hwnd || canvas_w < 2 || canvas_h < 2 || !HrWgcSupported()) {
        HrWgcLog::Warn("HrWgcCreate: refused (no window / canvas too small / window capture unsupported on this PC).");
        return nullptr;
    }

    if (NativeWgcAllowed()) {
        GuardScope guard;
        auto sp = MakeCtx(hwnd, canvas_w, canvas_h, client_only, draw_cursor);
        Ctx *c = sp.get();
        const InitResult ir = InitContained(c);
        if (ir == InitResult::kOk) {
            {
                std::lock_guard<std::mutex> lk(g_registry_mu);
                g_registry.push_back(sp);
            }
            HrWgcLog::Note("Window capture: using Windows.Graphics.Capture (" + std::to_string(c->canvas_w) + "x" +
                        std::to_string(c->canvas_h) + (client_only ? ", content area only" : ", whole window") + ").");
            return c;
        }
        if (ir == InitResult::kFaulted || ir == InitResult::kTimeout) {
            // The init thread is parked / still running and may still write into this context:
            // keep it alive for the rest of the process (deliberate leak) and never release its objects.
            new std::shared_ptr<Ctx>(sp);
            MarkNativeBad();
            HrWgcLog::Warn("Window capture: native Windows.Graphics.Capture is unusable on this PC (remembered in "
                        "logs\\wgc_native_bad.flag) - recording the window with the PrintWindow backend instead of "
                        "the screen crop.");
        } else {
            Teardown(c);         // a plain failure (e.g. Windows refused this window): sp frees the context
        }
    }
    if (!PwAvailable()) return nullptr;
    return CreatePrintWindowCtx(hwnd, canvas_w, canvas_h, client_only, draw_cursor, options);
}

bool HrWgcIsHandle(void *handle) {
    return FindCtx(handle) != nullptr;
}

void HrWgcDestroy(void *handle) {
    if (!handle) return;
    std::shared_ptr<Ctx> sp;
    {
        std::lock_guard<std::mutex> lk(g_registry_mu);
        auto it = std::find_if(g_registry.begin(), g_registry.end(),
                               [handle](const std::shared_ptr<Ctx> &p) { return p.get() == handle; });
        if (it == g_registry.end()) return;
        sp = *it;
        g_registry.erase(it);
    }
    GuardScope guard;
    std::lock_guard<std::mutex> lk(sp->mu);     // waits for a capture / reset that is still running
    sp->dead = true;
    Teardown(sp.get());
}                                               // the context itself is freed with the last shared_ptr

int HrWgcCapture(void *handle, uint8_t *out_bgra, int timeout_ms) {
    std::shared_ptr<Ctx> sp = FindCtx(handle);
    if (!sp || !out_bgra) return kError;
    Ctx *c = sp.get();
    const DWORD t0 = GetTickCount();
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(c->mu);
            if (c->dead || !c->ok) return kError;
            if (c->pw) return PwCapture(c, out_bgra, timeout_ms);   // PrintWindow / smart: synchronous, the pipeline paces the calls
            if (!c->pool) return kError;

            // Only the first frames are watched - once capture is proven to work there is no file churn.
            OptionalGuard guard(c->frames_ok < 3 && c->guard_calls++ < 20);
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
                if (r == kOk && c->frames_ok < 3) ++c->frames_ok;
                return r;
            }
            if (FAILED(hr)) return kLost;
        }   // lock released while waiting, so Destroy / SetCursor / Reset never starve behind this loop
        if (timeout_ms <= 0 || (int)(GetTickCount() - t0) >= timeout_ms) return kTimeout;
        Sleep(2);   // frames arrive at most every ~8 ms; 2 ms keeps latency low without busy-spinning
    }
}

int HrWgcGetSize(void *handle, int *w, int *h) {
    std::shared_ptr<Ctx> sp = FindCtx(handle);
    if (!sp) return 0;
    if (w) *w = sp->canvas_w;
    if (h) *h = sp->canvas_h;
    return 1;
}

int HrWgcReset(void *handle) {
    std::shared_ptr<Ctx> sp = FindCtx(handle);
    if (!sp) return 0;
    GuardScope guard;
    std::lock_guard<std::mutex> lk(sp->mu);
    if (sp->dead) return 0;
    if (sp->pw) return 1;                 // PrintWindow has no device that can be lost
    Teardown(sp.get());
    const InitResult ir = InitContained(sp.get());
    if (ir == InitResult::kFaulted || ir == InitResult::kTimeout) {
        new std::shared_ptr<Ctx>(sp);     // see HrWgcCreate(): the init thread may still touch it
        return 0;
    }
    return ir == InitResult::kOk ? 1 : 0;
}

void HrWgcSetCursor(void *handle, bool on) {
    std::shared_ptr<Ctx> sp = FindCtx(handle);
    if (!sp) return;
    std::lock_guard<std::mutex> lk(sp->mu);
    if (sp->dead) return;
    sp->cursor = on;
    if (sp->session2) sp->session2->put_IsCursorCaptureEnabled(on ? TRUE : FALSE);
}

bool HrWgcSetTarget(void *handle, HWND hwnd) {
    std::shared_ptr<Ctx> sp = FindCtx(handle);
    if (!sp || !hwnd) return false;
    std::lock_guard<std::mutex> lk(sp->mu);
    if (sp->dead || !sp->pw) return false;               // native WGC is bound to its window
    if (sp->hwnd == hwnd) return true;
    HrWgcLog::Note("Window capture: re-targeting to window " + std::to_string((unsigned long long)(uintptr_t)hwnd) +
                   " (was " + std::to_string((unsigned long long)(uintptr_t)sp->hwnd) + ").");
    sp->hwnd = hwnd;
    sp->gone_logged = false;
    sp->have_hash = false;
    sp->static_streak = 0;
    sp->need_compose = true;
    sp->next_pw_at = 0;
    sp->clear_since = 0;
    sp->next_occl_check = 0;
    return true;
}

int HrWgcMode(void *handle) {
    std::shared_ptr<Ctx> sp = FindCtx(handle);
    if (!sp) return 0;
    if (!sp->pw) return 1;
    return sp->mode == 1 ? 3 : 2;
}
