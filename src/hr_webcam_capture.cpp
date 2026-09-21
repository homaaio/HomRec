#include "hr_webcam_capture.h"
#include "hr_log.h"

#include <cstring>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <objbase.h>
  #include <mfapi.h>
  #include <mfidl.h>
  #include <mfreadwrite.h>
  #include <wrl/client.h>
  #include <thread>
  #include <mutex>
  #include <condition_variable>
  #include <atomic>
  using Microsoft::WRL::ComPtr;
#endif

#ifdef _WIN32

namespace {

std::string NarrowFromWide(const wchar_t *w) {
    if (!w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

} // namespace

struct HrWebcamCapture::Impl {
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> alive{true}; // cleared once the read loop exits for good

    std::mutex frame_mtx;
    std::vector<uint8_t> latest_bgra;
    int latest_w = 0, latest_h = 0;
    bool has_frame = false;

    std::string device_name;
    int device_index = -1;

    std::mutex finish_mtx;
    std::condition_variable finish_cv;
    bool finished = false;
    std::atomic<bool> handed_off{false};
};

namespace {

// Reads the reader's currently-negotiated frame size + row stride. Called
// once right after format negotiation and again whenever ReadSample
// reports MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED (some devices/drivers
// renegotiate mid-stream, e.g. on an exposure/resolution auto-switch).
void ReadCurrentDims(IMFSourceReader *reader, int &w, int &h, LONG &stride) {
    w = h = 0; stride = 0;
    ComPtr<IMFMediaType> cur;
    if (FAILED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) || !cur) {
        return;
    }
    UINT32 uw = 0, uh = 0;
    if (SUCCEEDED(MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &uw, &uh))) {
        w = (int)uw; h = (int)uh;
    }
    UINT32 raw_stride = 0;
    if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride))) {
        // The attribute stores a signed LONG in this UINT32 slot (negative
        // means the device hands back bottom-up rows) -- reinterpret, not
        // convert.
        stride = (LONG)raw_stride;
    } else if (w > 0) {
        stride = w * 4; // RGB32 == 4 bytes/pixel; no reported padding means tightly packed
    }
}

} // namespace

// Declared as a friend in hr_webcam_capture.h (needs access to the private
// Impl type) -- kept at global scope rather than inside the anonymous
// namespace above so that friend declaration can actually name it.
void HrWebcamCaptureThreadMain(HrWebcamCapture::Impl *impl) {
    // A brand-new thread, independent of the capture pipeline's own
    // CoInitializeEx (hr_pipeline.cpp) -- needs its own COM apartment. MTA
    // to match how the rest of this codebase initializes background
    // threads (hr_pipeline.cpp, hr_webcam_enum.cpp).
    HRESULT hr_co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool should_uninit_co = (hr_co == S_OK || hr_co == S_FALSE);
    bool mf_started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));

    auto give_up = [&]() {
        impl->alive = false;
        if (mf_started) MFShutdown();
        if (should_uninit_co) CoUninitialize();
        // Signal the destructor (if it's still waiting on us) that we're
        // done, then check whether it already gave up and handed cleanup
        // off to us instead (see Impl's own comment) - if so, it's not
        // coming back, so we delete ourselves here rather than leaking.
        bool was_handed_off = impl->handed_off.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lk(impl->finish_mtx);
            impl->finished = true;
        }
        impl->finish_cv.notify_all();
        if (was_handed_off) delete impl;
    };

    if (!mf_started) { give_up(); return; }

    ComPtr<IMFAttributes> attr;
    if (FAILED(MFCreateAttributes(&attr, 1)) ||
        FAILED(attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
        give_up(); return;
    }

    IMFActivate **devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attr.Get(), &devices, &count)) || count == 0) {
        give_up(); return;
    }

    // Primary match: exact friendly-name match (first one found). Fallback:
    // the device_index-th enumerated device -- see the reasoning in the
    // header. Last resort: whatever camera is first, rather than nothing,
    // matching how a missing image/gif path still shows *something* is
    // better than silently compositing nothing wherever reasonable.
    int chosen = -1;
    if (!impl->device_name.empty()) {
        for (UINT32 i = 0; i < count; ++i) {
            wchar_t *name_w = nullptr; UINT32 name_len = 0;
            if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name_w, &name_len))) {
                std::string name = NarrowFromWide(name_w);
                CoTaskMemFree(name_w);
                if (name == impl->device_name) { chosen = (int)i; break; }
            }
        }
    }
    if (chosen < 0 && impl->device_index >= 0 && (UINT32)impl->device_index < count) {
        chosen = impl->device_index;
    }
    if (chosen < 0) chosen = 0;

    ComPtr<IMFMediaSource> media_source;
    HRESULT hr = devices[chosen]->ActivateObject(IID_PPV_ARGS(&media_source));
    for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
    CoTaskMemFree(devices);

    if (FAILED(hr) || !media_source) { give_up(); return; }

    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFAttributes> reader_attr;
    if (SUCCEEDED(MFCreateAttributes(&reader_attr, 1))) {
        reader_attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    if (FAILED(MFCreateSourceReaderFromMediaSource(media_source.Get(), reader_attr.Get(), &reader)) || !reader) {
        give_up(); return;
    }

    ComPtr<IMFMediaType> want_type;
    if (FAILED(MFCreateMediaType(&want_type)) ||
        FAILED(want_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(want_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
        FAILED(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want_type.Get()))) {
        give_up(); return;
    }
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    int w = 0, h = 0; LONG stride = 0;
    ReadCurrentDims(reader.Get(), w, h, stride);
    if (w <= 0 || h <= 0) { give_up(); return; }

    std::vector<uint8_t> scratch;
    while (!impl->stop.load()) {
        DWORD stream_index = 0, flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT rr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                         &stream_index, &flags, &timestamp, &sample);
        if (FAILED(rr) || (flags & MF_SOURCE_READERF_ERROR)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            ReadCurrentDims(reader.Get(), w, h, stride);
            if (w <= 0 || h <= 0) break;
        }
        if (!sample) continue; // gap/dropped frame -- not fatal, nothing new to composite this round

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) continue;
        BYTE *data = nullptr; DWORD max_len = 0, cur_len = 0;
        if (FAILED(buffer->Lock(&data, &max_len, &cur_len))) continue;

        LONG row_bytes = w * 4;
        LONG abs_stride = stride >= 0 ? stride : -stride;
        if (abs_stride >= row_bytes && (DWORD)(abs_stride) * (DWORD)h <= cur_len) {
            scratch.resize((size_t)w * h * 4);
            for (int row = 0; row < h; ++row) {
                const uint8_t *src_row = stride >= 0
                    ? data + (size_t)row * abs_stride
                    : data + (size_t)(h - 1 - row) * abs_stride; // bottom-up: read back to front
                std::memcpy(scratch.data() + (size_t)row * row_bytes, src_row, (size_t)row_bytes);
            }
            for (size_t px = 3; px < scratch.size(); px += 4) scratch[px] = 0xFF;
            std::lock_guard<std::mutex> lk(impl->frame_mtx);
            impl->latest_bgra.swap(scratch);
            impl->latest_w = w;
            impl->latest_h = h;
            impl->has_frame = true;
        }
        buffer->Unlock();
    }

    give_up();
}

HrWebcamCapture::HrWebcamCapture() : impl_(nullptr) {}

HrWebcamCapture *HrWebcamCapture::Open(const std::string &device_name, int device_index) {
    auto *cap = new HrWebcamCapture();
    cap->impl_ = new Impl();
    cap->impl_->device_name = device_name;
    cap->impl_->device_index = device_index;
    cap->impl_->thread = std::thread(HrWebcamCaptureThreadMain, cap->impl_);
    return cap;
}

HrWebcamCapture::~HrWebcamCapture() {
    if (impl_) {
        impl_->stop = true;
        if (impl_->thread.joinable()) {
            // Bounded wait (see Impl's comment for why an unconditional
            // join() here isn't safe) - 2s is generous for a thread that's
            // just idling between camera frames (checks `finished` the
            // instant it actually exits), but short enough that a genuinely
            // stuck driver doesn't freeze whoever's destroying this overlay.
            bool finished;
            {
                std::unique_lock<std::mutex> lk(impl_->finish_mtx);
                finished = impl_->finish_cv.wait_for(lk, std::chrono::milliseconds(2000),
                    [this] { return impl_->finished; });
            }
            if (finished) {
                impl_->thread.join();
            } else {
                HrLog::Error("Webcam overlay: capture thread did not stop in time "
                             "(camera driver stuck, or the device was removed mid-capture) "
                             "- handing off cleanup to it instead of freezing here.");
                impl_->handed_off.store(true, std::memory_order_release);
                impl_->thread.detach();
                return; // impl_ is now owned by the detached thread - see give_up()
            }
        }
        delete impl_;
    }
}

bool HrWebcamCapture::GetLatestFrame(std::vector<uint8_t> &out_bgra, int &out_w, int &out_h) {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lk(impl_->frame_mtx);
    if (!impl_->has_frame) return false;
    out_bgra = impl_->latest_bgra;
    out_w = impl_->latest_w;
    out_h = impl_->latest_h;
    return true;
}

bool HrWebcamCapture::IsAlive() const {
    return impl_ && impl_->alive.load();
}

#else // !_WIN32

struct HrWebcamCapture::Impl {};
HrWebcamCapture::HrWebcamCapture() : impl_(nullptr) {}
HrWebcamCapture *HrWebcamCapture::Open(const std::string &, int) { return nullptr; }
HrWebcamCapture::~HrWebcamCapture() {}
bool HrWebcamCapture::GetLatestFrame(std::vector<uint8_t> &, int &, int &) { return false; }
bool HrWebcamCapture::IsAlive() const { return false; }

#endif
