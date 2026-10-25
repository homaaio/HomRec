// mf_encoder.cpp - in-process hardware H.264 encoder via the Media Foundation Sink Writer.
// See mf_encoder.h for the contract.
//
// NOT COMPILED OR RUN where it was written (no Windows SDK / MinGW in that environment): every API
// call below is from the documented Media Foundation interfaces, but treat the first build as the
// first test.

#ifdef _WIN32
  // IMFDXGIDeviceManager / MFCreateDXGISurfaceBuffer are declared only for WINVER >= 0x0602 in some
  // header sets, while the project builds with _WIN32_WINNT=0x0601 (Windows 7). Only THIS file raises
  // the version so the declarations are visible; the two Windows-8-era functions are still resolved
  // with GetProcAddress at run time, so the exe keeps loading on Windows 7 (where the hardware
  // encoder path simply reports "unavailable" and the caller falls back to ffmpeg).
  #ifdef _WIN32_WINNT
    #undef _WIN32_WINNT
  #endif
  #define _WIN32_WINNT 0x0A00
  #ifdef WINVER
    #undef WINVER
  #endif
  #define WINVER 0x0A00
  #ifdef NTDDI_VERSION
    #undef NTDDI_VERSION
  #endif
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <objbase.h>
  #include <mfapi.h>
  #include <mfidl.h>
  #include <mfreadwrite.h>
  #include <mferror.h>
  #include <mftransform.h>
  #include <d3d11.h>
  #include <wrl/client.h>
  using Microsoft::WRL::ComPtr;
#endif

#include "mf_encoder.h"
#include "../utils/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32

namespace {

// ---- CODECAPI property keys (values from codecapi.h; defined here so the build does not depend on
// which of the MinGW / MSVC header sets exports them). A key an encoder does not know is simply
// ignored by the sink writer - the bitrate in the output media type still applies.
const GUID kCapRateControlMode = {0x1c0608e9, 0x370c, 0x4710, {0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23}};
const GUID kCapMeanBitRate     = {0xf7222374, 0x2144, 0x4815, {0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52}};
const GUID kCapMaxBitRate      = {0x9651eae4, 0x39b9, 0x4ebf, {0x85, 0xef, 0xd7, 0xf4, 0x44, 0xec, 0x74, 0x65}};
const GUID kCapGopSize         = {0x95f31b26, 0x95a4, 0x41aa, {0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1}};
const GUID kCapBPictureCount   = {0x8d390aac, 0xdc5c, 0x4200, {0xb5, 0x7f, 0x81, 0x4d, 0x04, 0xba, 0xba, 0xb2}};
const GUID kCapLowLatencyMode  = {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};

constexpr UINT32 kRcPeakConstrainedVbr = 1;   // eAVEncCommonRateControlMode_PeakConstrainedVBR
constexpr UINT32 kProfileHigh          = 100; // eAVEncH264VProfile_High
// Colour tags, identical to what ffmpeg_runner.cpp writes for the pipe path: limited range, BT.601.
constexpr UINT32 kNominalRange_16_235  = 2;   // MFNominalRange_16_235
constexpr UINT32 kMatrixBT601          = 2;   // MFVideoTransferMatrix_BT601
constexpr UINT32 kPrimariesSMPTE170M   = 6;   // MFVideoPrimaries_SMPTE170M
constexpr UINT32 kTransferFunc709      = 5;   // MFVideoTransFunc_709 (same curve as smpte170m)

typedef HRESULT (WINAPI *PFN_MFCreateDXGIDeviceManager)(UINT *, IMFDXGIDeviceManager **);
typedef HRESULT (WINAPI *PFN_MFCreateDXGISurfaceBuffer)(REFIID, IUnknown *, UINT, BOOL, IMFMediaBuffer **);

struct MfApi {
    PFN_MFCreateDXGIDeviceManager create_mgr = nullptr;
    PFN_MFCreateDXGISurfaceBuffer create_surface_buf = nullptr;
    bool loaded = false;
};
MfApi &mf_api() {
    static MfApi api;
    static std::once_flag once;
    std::call_once(once, []() {
        HMODULE m = LoadLibraryW(L"mfplat.dll");
        if (m) {
            api.create_mgr = reinterpret_cast<PFN_MFCreateDXGIDeviceManager>(
                reinterpret_cast<void *>(GetProcAddress(m, "MFCreateDXGIDeviceManager")));
            api.create_surface_buf = reinterpret_cast<PFN_MFCreateDXGISurfaceBuffer>(
                reinterpret_cast<void *>(GetProcAddress(m, "MFCreateDXGISurfaceBuffer")));
        }
        api.loaded = true;
    });
    return api;
}

std::wstring widen(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::string narrow(const wchar_t *w) {
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    s.resize((size_t)n - 1);
    return s;
}
std::string hex(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08lX", (unsigned long)hr);
    return b;
}

struct MfEnc {
    // configuration
    std::string out_path_utf8;
    int w = 0, h = 0, fps = 30, kbps = 0;

    // MF / device objects
    bool mf_started = false;
    ComPtr<IMFSinkWriter>        writer;
    ComPtr<IMFDXGIDeviceManager> mgr;
    UINT                         mgr_token = 0;
    ComPtr<IUnknown>             device;        // the capture ID3D11Device (kept alive while the writer uses it)
    bool textures_ok = false;                   // writer was built with a D3D manager

    // state
    std::mutex wmu;                             // serialises writes / finalize / rebuild
    std::atomic<int>  state{0};                 // 0 idle, 1 running, 2 finalizing, 3 done, -1 failed
    std::atomic<bool> fin_done{false};
    std::atomic<long long> samples{0};
    std::atomic<bool> tex_broken{false};
    int  consecutive_fail = 0;
    bool began = false;
    bool size_warned = false;

    mutable std::mutex err_mu;
    std::string last_error;

    // timeline
    bool    have_last = false;
    int64_t base_slot = 0;                      // slot of the first picture -> sample time 0
    int64_t last_slot = -1;
    ComPtr<IMFMediaBuffer> last_buf;            // the picture repeats re-submit
    long long real_pictures = 0;                // pictures (not repeats) accepted by the writer

    // reusable system-memory buffers (a buffer is free again once nobody but this pool references it)
    std::vector<ComPtr<IMFMediaBuffer>> pool;

    std::thread fin_thread;

    void set_error(const std::string &e) {
        std::lock_guard<std::mutex> lk(err_mu);
        last_error = e;
    }
    std::string get_error() const {
        std::lock_guard<std::mutex> lk(err_mu);
        return last_error;
    }
};

std::mutex g_reg_mu;
std::map<const void *, std::shared_ptr<MfEnc>> g_reg;

std::shared_ptr<MfEnc> lookup(const void *h) {
    if (!h) return nullptr;
    std::lock_guard<std::mutex> lk(g_reg_mu);
    auto it = g_reg.find(h);
    return it == g_reg.end() ? nullptr : it->second;
}

// ---- media types -------------------------------------------------------------------------------
HRESULT make_type(const MfEnc &e, const GUID &subtype, bool output, UINT32 profile, IMFMediaType **out) {
    ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, subtype);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, (UINT32)e.w, (UINT32)e.h);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, (UINT32)e.fps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, kNominalRange_16_235);
    t->SetUINT32(MF_MT_YUV_MATRIX, kMatrixBT601);
    t->SetUINT32(MF_MT_VIDEO_PRIMARIES, kPrimariesSMPTE170M);
    t->SetUINT32(MF_MT_TRANSFER_FUNCTION, kTransferFunc709);
    if (output) {
        t->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)std::max(100, e.kbps) * 1000u);
        if (profile) t->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
    } else {
        t->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)e.w);   // NV12, tightly packed, top-down
        t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    }
    *out = t.Detach();
    return S_OK;
}

// Creates the sink writer; with_manager = hand the capture device to the encoder (GPU textures).
HRESULT build_writer(MfEnc &e, bool with_manager) {
    e.writer.Reset();
    e.mgr.Reset();
    e.textures_ok = false;
    e.began = false;

    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 4);
    if (FAILED(hr)) return hr;
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);

    if (with_manager && e.device) {
        MfApi &api = mf_api();
        if (api.create_mgr) {
            ComPtr<IMFDXGIDeviceManager> m;
            UINT token = 0;
            if (SUCCEEDED(api.create_mgr(&token, &m)) && m && SUCCEEDED(m->ResetDevice(e.device.Get(), token))) {
                e.mgr = m;
                e.mgr_token = token;
                attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, e.mgr.Get());
                e.textures_ok = true;
            }
        }
    }

    const std::wstring wpath = widen(e.out_path_utf8);
    hr = MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, attrs.Get(), &e.writer);
    if (FAILED(hr)) { e.textures_ok = false; return hr; }

    // Output (H.264) + encoder parameters. Profile High first, then without a profile.
    ComPtr<IMFMediaType> out_t;
    DWORD stream = 0;
    hr = E_FAIL;
    for (UINT32 profile : {kProfileHigh, (UINT32)0}) {
        out_t.Reset();
        if (FAILED(make_type(e, MFVideoFormat_H264, true, profile, &out_t))) continue;
        hr = e.writer->AddStream(out_t.Get(), &stream);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) return hr;

    ComPtr<IMFSinkWriterEncoderConfig> cfg;
    if (SUCCEEDED(e.writer.As(&cfg)) && cfg) {
        ComPtr<IMFAttributes> p;
        if (SUCCEEDED(MFCreateAttributes(&p, 8))) {
            const UINT32 mean = (UINT32)std::max(100, e.kbps) * 1000u;
            p->SetUINT32(kCapRateControlMode, kRcPeakConstrainedVbr);
            p->SetUINT32(kCapMeanBitRate, mean);
            p->SetUINT32(kCapMaxBitRate, mean * 2u);
            p->SetUINT32(kCapGopSize, (UINT32)std::max(1, e.fps * 2));
            p->SetUINT32(kCapBPictureCount, 0);
            p->SetUINT32(kCapLowLatencyMode, TRUE);
            HRESULT hc = cfg->SetTargetMediaType(stream, out_t.Get(), p.Get());
            if (FAILED(hc)) HrLog::Warn("MF encoder: encoder parameters were rejected (" + hex(hc) + ") - using the encoder's defaults.");
        }
    }

    ComPtr<IMFMediaType> in_t;
    hr = make_type(e, MFVideoFormat_NV12, false, 0, &in_t);
    if (FAILED(hr)) return hr;
    hr = e.writer->SetInputMediaType(stream, in_t.Get(), nullptr);
    if (FAILED(hr)) return hr;

    hr = e.writer->BeginWriting();
    if (FAILED(hr)) return hr;
    e.began = true;
    return S_OK;
}

// 1 = hardware MFT, 0 = software MFT, -1 = could not tell.
int probe_hardware(MfEnc &e, std::string &name) {
    ComPtr<IMFTransform> t;
    if (FAILED(e.writer->GetServiceForStream(0, GUID_NULL, IID_PPV_ARGS(t.GetAddressOf()))) || !t) return -1;
    ComPtr<IMFAttributes> a;
    if (FAILED(t->GetAttributes(&a)) || !a) return -1;
    UINT32 async = 0;
    a->GetUINT32(MF_TRANSFORM_ASYNC, &async);
    UINT32 len = 0;
    const bool hw_url = SUCCEEDED(a->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &len)) && len > 0;
    WCHAR *nm = nullptr;
    UINT32 nl = 0;
    if (SUCCEEDED(a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &nl)) && nm) {
        name = narrow(nm);
        CoTaskMemFree(nm);
    }
    return (async || hw_url) ? 1 : 0;
}

// Hands one picture (a buffer) to the writer at `slot`. Caller holds wmu.
bool submit(MfEnc &e, IMFMediaBuffer *buf, int64_t slot) {
    if (!e.writer || e.state.load() != 1) return false;
    ComPtr<IMFSample> s;
    HRESULT hr = MFCreateSample(&s);
    if (SUCCEEDED(hr)) hr = s->AddBuffer(buf);
    if (SUCCEEDED(hr)) {
        const int64_t rel = slot - e.base_slot;
        const LONGLONG t0 = rel * 10000000LL / e.fps;
        const LONGLONG t1 = (rel + 1) * 10000000LL / e.fps;
        s->SetSampleTime(t0);
        s->SetSampleDuration(t1 - t0);
        hr = e.writer->WriteSample(0, s.Get());
    }
    if (FAILED(hr)) {
        if (++e.consecutive_fail >= 5) {
            e.set_error("WriteSample failed " + hex(hr) + " repeatedly - the encoder stopped accepting frames");
            e.state.store(-1);
            HrLog::Error("MF encoder: " + e.get_error());
        }
        return false;
    }
    e.consecutive_fail = 0;
    e.samples.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Repeats the last picture for the slots between last_slot and `upto` (inclusive). Caller holds wmu.
void fill_to(MfEnc &e, int64_t upto, int max_repeats) {
    int n = 0;
    while (e.have_last && e.last_slot < upto && n < max_repeats && e.state.load() == 1) {
        if (!submit(e, e.last_buf.Get(), e.last_slot + 1)) break;
        ++e.last_slot;
        ++n;
    }
    // A hole this loop refused to fill (huge gap) is skipped: the next picture simply starts later.
}

// Free system-memory buffer of at least `size` bytes (reused when no sample references it any more).
ComPtr<IMFMediaBuffer> take_sysmem_buffer(MfEnc &e, DWORD size) {
    for (auto &b : e.pool) {
        if (!b) continue;
        b->AddRef();
        const ULONG rc = b->Release();   // reference count WITHOUT our probe
        if (rc != 1) continue;           // somebody (a sample / the encoder / last_buf) still holds it
        DWORD maxlen = 0;
        if (SUCCEEDED(b->GetMaxLength(&maxlen)) && maxlen >= size) return b;
    }
    ComPtr<IMFMediaBuffer> nb;
    if (FAILED(MFCreateMemoryBuffer(size, &nb)) || !nb) return nullptr;
    if (e.pool.size() < 12) e.pool.push_back(nb);
    return nb;
}

void finalize_now(MfEnc &e) {
    // Caller must not hold wmu while waiting for a long time; Finalize() itself is serialised by it.
    std::lock_guard<std::mutex> lk(e.wmu);
    HRESULT hr = S_OK;
    if (e.writer && e.began) hr = e.writer->Finalize();
    e.writer.Reset();
    e.last_buf.Reset();
    e.pool.clear();
    e.mgr.Reset();
    if (FAILED(hr)) {
        e.set_error("Finalize failed " + hex(hr) + " - the .mp4 may be unplayable");
        HrLog::Error("MF encoder: " + e.get_error());
        e.state.store(-1);
    } else if (e.state.load() != -1) {
        e.state.store(3);
    }
}

} // namespace

// =================================================================================================
extern "C" {

void *hr_mf_create(void) {
    auto e = std::make_shared<MfEnc>();
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) return nullptr;
    e->mf_started = true;
    void *h = e.get();
    std::lock_guard<std::mutex> lk(g_reg_mu);
    g_reg[h] = e;
    return h;
}

void hr_mf_destroy(void *h) {
    std::shared_ptr<MfEnc> e;
    {
        std::lock_guard<std::mutex> lk(g_reg_mu);
        auto it = g_reg.find(h);
        if (it == g_reg.end()) return;
        e = it->second;
        g_reg.erase(it);        // from now on every API call with this handle is a no-op
    }
    if (e->fin_thread.joinable()) e->fin_thread.join();
    if (e->state.load() == 1 || e->state.load() == 2) {
        // Never ended (crash path / abandoned recording): close the file so it stays playable.
        e->state.store(2);
        finalize_now(*e);
    }
    e->writer.Reset();
    e->mgr.Reset();
    e->device.Reset();
    e->last_buf.Reset();
    e->pool.clear();
    if (e->mf_started) { MFShutdown(); e->mf_started = false; }
}

int hr_mf_configure(void *h, const char *out_path_utf8, int w, int h_px, int fps, int kbps) {
    auto e = lookup(h);
    if (!e || !out_path_utf8 || w < 2 || h_px < 2 || fps < 1) return 0;
    e->out_path_utf8 = out_path_utf8;
    e->w = w & ~1;
    e->h = h_px & ~1;
    e->fps = fps;
    e->kbps = kbps > 0 ? kbps : 8000;
    return 1;
}

int hr_mf_start(void *h, void *d3d11_device, int want_textures) {
    auto e = lookup(h);
    if (!e) return 0;
    if (e->out_path_utf8.empty() || e->w < 2 || e->h < 2) { e->set_error("not configured"); return 0; }
    std::lock_guard<std::mutex> lk(e->wmu);

    if (d3d11_device) {
        IUnknown *u = static_cast<IUnknown *>(d3d11_device);
        e->device = u;     // ComPtr::operator= AddRefs
    }
    HRESULT hr = build_writer(*e, want_textures != 0 && d3d11_device != nullptr);
    if (FAILED(hr) && e->textures_ok == false && want_textures && d3d11_device) {
        // The D3D-aware writer could not be built - retry as a plain system-memory writer.
        HrLog::Warn("MF encoder: could not build the writer with a D3D11 device manager (" + hex(hr) +
                    ") - retrying with system-memory input only.");
        hr = build_writer(*e, false);
    }
    if (FAILED(hr)) {
        e->set_error("could not start the Media Foundation writer / hardware H.264 encoder (" + hex(hr) + ")");
        e->writer.Reset();
        return 0;
    }

    std::string name;
    const int hw = probe_hardware(*e, name);
    if (hw == 0) {
        // Only the (slow, CPU) Microsoft software encoder is available: ffmpeg's NVENC/QSV/AMF is better.
        e->set_error("no hardware H.264 encoder MFT available (only a software encoder was offered)");
        HrLog::Warn("MF encoder: " + e->get_error() + " - using ffmpeg instead.");
        e->writer.Reset();
        e->mgr.Reset();
        return 0;
    }
    HrLog::Info("MF encoder: started (" + std::to_string(e->w) + "x" + std::to_string(e->h) + " @ " +
                std::to_string(e->fps) + " fps, " + std::to_string(e->kbps) + " kbps, encoder: " +
                (name.empty() ? std::string(hw == 1 ? "hardware MFT" : "unknown MFT") : name) +
                ", GPU textures " + (e->textures_ok ? "on" : "off") + ").");
    e->state.store(1);
    return 1;
}

int hr_mf_textures_enabled(const void *h) {
    auto e = lookup(h);
    return (e && e->textures_ok && !e->tex_broken.load()) ? 1 : 0;
}

int hr_mf_write_sysmem(void *h, const uint8_t *nv12, int w, int h_px, int64_t slot) {
    auto e = lookup(h);
    if (!e || !nv12 || e->state.load() != 1) return 0;
    if (w != e->w || h_px != e->h) {
        if (!e->size_warned) {
            e->size_warned = true;   // log once
            HrLog::Warn("MF encoder: a " + std::to_string(w) + "x" + std::to_string(h_px) + " picture does not match the " +
                        std::to_string(e->w) + "x" + std::to_string(e->h) + " encoder - skipped.");
        }
        return 0;
    }
    std::lock_guard<std::mutex> lk(e->wmu);
    if (!e->writer) return 0;
    const DWORD size = (DWORD)((size_t)e->w * e->h * 3 / 2);
    ComPtr<IMFMediaBuffer> buf = take_sysmem_buffer(*e, size);
    if (!buf) return 0;
    BYTE *dst = nullptr;
    DWORD maxlen = 0;
    if (FAILED(buf->Lock(&dst, &maxlen, nullptr)) || !dst) return 0;
    std::memcpy(dst, nv12, size);
    buf->Unlock();
    buf->SetCurrentLength(size);

    if (!e->have_last) { e->base_slot = slot; e->last_slot = slot - 1; }
    if (slot > e->last_slot + 1) fill_to(*e, slot - 1, 1200);
    if (slot <= e->last_slot) return 0;   // out of order / duplicate slot
    if (!submit(*e, buf.Get(), slot)) return 0;
    e->last_buf = buf;
    e->have_last = true;
    e->last_slot = slot;
    ++e->real_pictures;
    return 1;
}

int hr_mf_write_texture(void *h, void *tex, int64_t slot) {
    auto e = lookup(h);
    if (!e || !tex || e->state.load() != 1 || !e->textures_ok || e->tex_broken.load()) return 0;
    MfApi &api = mf_api();
    if (!api.create_surface_buf) { e->tex_broken.store(true); return 0; }
    std::lock_guard<std::mutex> lk(e->wmu);
    if (!e->writer) return 0;

    ComPtr<IMFMediaBuffer> buf;
    ID3D11Texture2D *t2d = static_cast<ID3D11Texture2D *>(tex);
    HRESULT hr = api.create_surface_buf(__uuidof(ID3D11Texture2D), t2d, 0, FALSE, &buf);
    if (SUCCEEDED(hr) && buf) {
        DWORD len = 0;
        ComPtr<IMF2DBuffer> b2;
        if (SUCCEEDED(buf.As(&b2)) && b2) b2->GetContiguousLength(&len);
        if (len == 0) len = (DWORD)((size_t)e->w * e->h * 3 / 2);
        buf->SetCurrentLength(len);

        if (!e->have_last) { e->base_slot = slot; e->last_slot = slot - 1; }
        if (slot > e->last_slot + 1) fill_to(*e, slot - 1, 1200);
        if (slot <= e->last_slot) return 0;
        if (submit(*e, buf.Get(), slot)) {
            e->last_buf = buf;
            e->have_last = true;
            e->last_slot = slot;
            ++e->real_pictures;
            return 1;
        }
        hr = E_FAIL;
    }

    // The texture path does not work with this encoder / driver.
    e->tex_broken.store(true);
    HrLog::Warn("MF encoder: handing the GPU texture to the encoder failed (" + hex(hr) +
                ") - switching to system-memory input for the rest of this recording.");
    if (e->real_pictures == 0) {
        // Nothing was written yet, so the writer can simply be rebuilt without the D3D manager.
        e->state.store(0);
        const HRESULT hb = build_writer(*e, false);
        if (SUCCEEDED(hb)) {
            e->consecutive_fail = 0;
            e->state.store(1);
        } else {
            e->set_error("could not rebuild the writer after the texture path failed (" + hex(hb) + ")");
            e->state.store(-1);
        }
    }
    return 0;
}

int hr_mf_hold_until(void *h, int64_t slot) {
    auto e = lookup(h);
    if (!e || e->state.load() != 1) return 0;
    std::lock_guard<std::mutex> lk(e->wmu);
    fill_to(*e, slot, 600);
    return 1;
}

int hr_mf_end(void *h, int64_t end_slot) {
    auto e = lookup(h);
    if (!e) return 0;
    {
        std::lock_guard<std::mutex> lk(e->wmu);
        const int st = e->state.load();
        if (st == 0 || st == 2 || st == 3) return 0;         // not started / already ending / done
        if (st == 1) {
            fill_to(*e, end_slot - 1, 100000);               // hold the last picture to the real end of the timeline
            if (!e->have_last) {
                e->set_error("no picture was recorded");
                HrLog::Warn("MF encoder: the recording ended before the first picture - the file will be empty.");
            }
            e->state.store(2);
        }
        // st == -1 (failed earlier): still try to close the file below so the handle is released.
    }
    MfEnc *raw = e.get();
    // The registry keeps the object alive until hr_mf_destroy(); that joins this thread first.
    try {
        e->fin_thread = std::thread([raw]() {
            const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            finalize_now(*raw);
            if (com) CoUninitialize();
            raw->fin_done.store(true, std::memory_order_release);
        });
    } catch (...) {
        finalize_now(*raw);
        raw->fin_done.store(true, std::memory_order_release);
    }
    return 1;
}

int hr_mf_wait(void *h, int timeout_ms) {
    auto e = lookup(h);
    if (!e) return 0;
    if (e->state.load() == 0 || e->state.load() == 1) return 0;   // never ended: nothing to wait for
    const auto t0 = std::chrono::steady_clock::now();
    while (!e->fin_done.load(std::memory_order_acquire)) {
        if (timeout_ms > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() >= timeout_ms)
            return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return e->state.load() == 3 ? 0 : -2;
}

int hr_mf_failed(const void *h) {
    auto e = lookup(h);
    return (e && e->state.load() == -1) ? 1 : 0;
}

int hr_mf_last_error(const void *h, char *buf, int buf_len) {
    if (!buf || buf_len <= 0) return 0;
    buf[0] = '\0';
    auto e = lookup(h);
    if (!e) return 0;
    const std::string s = e->get_error();
    const int n = (int)std::min<size_t>(s.size(), (size_t)buf_len - 1);
    std::memcpy(buf, s.data(), (size_t)n);
    buf[n] = '\0';
    return n;
}

double hr_mf_output_size_mb(const void *h) {
    auto e = lookup(h);
    if (!e || e->out_path_utf8.empty()) return 0.0;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(widen(e->out_path_utf8).c_str(), GetFileExInfoStandard, &info)) return 0.0;
    const long long sz = ((long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    return (double)sz / (1024.0 * 1024.0);
}

long long hr_mf_frames_written(const void *h) {
    auto e = lookup(h);
    return e ? e->samples.load(std::memory_order_relaxed) : 0;
}

} // extern "C"

#else  // ---------------------------------------------------------------------------------- non-Windows
extern "C" {
void *hr_mf_create(void) { return nullptr; }
void  hr_mf_destroy(void *) {}
int   hr_mf_configure(void *, const char *, int, int, int, int) { return 0; }
int   hr_mf_start(void *, void *, int) { return 0; }
int   hr_mf_textures_enabled(const void *) { return 0; }
int   hr_mf_write_sysmem(void *, const uint8_t *, int, int, int64_t) { return 0; }
int   hr_mf_write_texture(void *, void *, int64_t) { return 0; }
int   hr_mf_hold_until(void *, int64_t) { return 0; }
int   hr_mf_end(void *, int64_t) { return 0; }
int   hr_mf_wait(void *, int) { return 0; }
int   hr_mf_failed(const void *) { return 0; }
int   hr_mf_last_error(const void *, char *buf, int n) { if (buf && n > 0) buf[0] = 0; return 0; }
double hr_mf_output_size_mb(const void *) { return 0.0; }
long long hr_mf_frames_written(const void *) { return 0; }
}
#endif
