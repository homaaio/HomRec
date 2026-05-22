#include "capture.hpp"
#include <chrono>
#include <stdexcept>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace homrec {

// -- Helpers ------------------------------------------------------------------

static std::string hr_msg(HRESULT hr) {
    char buf[128];
    snprintf(buf, sizeof(buf), "HRESULT 0x%08lX", (unsigned long)hr);
    return buf;
}

// -- Constructor / Destructor -------------------------------------------------

CaptureThread::CaptureThread(MainRingBuffer& ring_buf)
    : ring_(ring_buf) {}

CaptureThread::~CaptureThread() {
    stop();
    release_duplication();
    if (d3d_context_) { d3d_context_->Release(); d3d_context_ = nullptr; }
    if (d3d_device_)  { d3d_device_->Release();  d3d_device_  = nullptr; }
    if (output1_)     { output1_->Release();      output1_     = nullptr; }
    if (output_)      { output_->Release();       output_      = nullptr; }
    if (adapter_)     { adapter_->Release();      adapter_     = nullptr; }
}

// -- init() -------------------------------------------------------------------

bool CaptureThread::init(int monitor_index, HWND hwnd) {
    monitor_index_ = monitor_index;
    target_hwnd_   = hwnd;

    // -- Create D3D11 device -------------------------------------------------
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };
    D3D_FEATURE_LEVEL chosen_level{};

    HRESULT hr = D3D11CreateDevice(
        nullptr,                        // default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,                              // no debug flags in release
        feature_levels,
        ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION,
        &d3d_device_,
        &chosen_level,
        &d3d_context_
    );
    if (FAILED(hr)) {
        report(CaptureStatus::INIT_FAILED, "D3D11CreateDevice failed: " + hr_msg(hr));
        return false;
    }

    // -- Get DXGI adapter → output -------------------------------------------
    IDXGIDevice* dxgi_device = nullptr;
    hr = d3d_device_->QueryInterface(__uuidof(IDXGIDevice),
                                     reinterpret_cast<void**>(&dxgi_device));
    if (FAILED(hr)) {
        report(CaptureStatus::INIT_FAILED, "QI IDXGIDevice: " + hr_msg(hr));
        return false;
    }

    hr = dxgi_device->GetParent(__uuidof(IDXGIAdapter),
                                reinterpret_cast<void**>(&adapter_));
    dxgi_device->Release();
    if (FAILED(hr)) {
        report(CaptureStatus::INIT_FAILED, "GetParent IDXGIAdapter: " + hr_msg(hr));
        return false;
    }

    hr = adapter_->EnumOutputs(monitor_index_, &output_);
    if (hr == DXGI_ERROR_NOT_FOUND) {
        report(CaptureStatus::INIT_FAILED,
               "Monitor index " + std::to_string(monitor_index_) + " not found");
        return false;
    }
    if (FAILED(hr)) {
        report(CaptureStatus::INIT_FAILED, "EnumOutputs: " + hr_msg(hr));
        return false;
    }

    hr = output_->QueryInterface(__uuidof(IDXGIOutput1),
                                 reinterpret_cast<void**>(&output1_));
    if (FAILED(hr)) {
        report(CaptureStatus::INIT_FAILED, "QI IDXGIOutput1: " + hr_msg(hr));
        return false;
    }

    // Get monitor dimensions
    DXGI_OUTPUT_DESC out_desc{};
    output_->GetDesc(&out_desc);
    stat_width_.store(out_desc.DesktopCoordinates.right
                      - out_desc.DesktopCoordinates.left);
    stat_height_.store(out_desc.DesktopCoordinates.bottom
                       - out_desc.DesktopCoordinates.top);

    // If window capture, grab its rect
    if (target_hwnd_) {
        GetWindowRect(target_hwnd_, &target_rect_);
    }

    return acquire_duplication();
}

// -- acquire_duplication() ----------------------------------------------------

bool CaptureThread::acquire_duplication() {
    if (duplication_) {
        duplication_->Release();
        duplication_ = nullptr;
    }
    if (!output1_) return false;

    HRESULT hr = output1_->DuplicateOutput(d3d_device_, &duplication_);
    if (FAILED(hr)) {
        report(CaptureStatus::INIT_FAILED,
               "DuplicateOutput failed: " + hr_msg(hr)
               + (hr == E_ACCESSDENIED
                  ? " (run as admin or disable Game DVR)" : ""));
        return false;
    }
    return true;
}

void CaptureThread::release_duplication() {
    if (duplication_) {
        duplication_->ReleaseFrame();
        duplication_->Release();
        duplication_ = nullptr;
    }
}

// -- start / stop -------------------------------------------------------------

void CaptureThread::start() {
    if (running_.load()) return;
    stop_requested_.store(false);
    running_.store(true);
    thread_ = std::thread(&CaptureThread::thread_main, this);
}

void CaptureThread::stop() {
    stop_requested_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

// -- thread_main() ------------------------------------------------------------

void CaptureThread::thread_main() {
    using clock = std::chrono::steady_clock;
    auto fps_t0 = clock::now();
    int  fps_frames = 0;

    static constexpr UINT ACQUIRE_TIMEOUT_MS = 100;

    while (!stop_requested_.load()) {
        // -- Acquire next frame ----------------------------------------------
        DXGI_OUTDUPL_FRAME_INFO frame_info{};
        IDXGIResource*          resource = nullptr;

        HRESULT hr = duplication_->AcquireNextFrame(
            ACQUIRE_TIMEOUT_MS, &frame_info, &resource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // No new frame within timeout — keep looping
            continue;
        }

        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
            // Alt+Tab in exclusive fullscreen, or device removed
            report(CaptureStatus::DXGI_ACCESS_LOST,
                   "DXGI access lost — retrying…");
            release_duplication();

            // Retry loop: try to re-acquire every 200ms for up to 10 seconds
            bool recovered = false;
            for (int attempt = 0; attempt < 50 && !stop_requested_.load(); ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (acquire_duplication()) {
                    report(CaptureStatus::OK, "DXGI access recovered");
                    recovered = true;
                    break;
                }
            }
            if (!recovered) {
                report(CaptureStatus::DEVICE_REMOVED, "Could not recover DXGI access");
                break;
            }
            continue;
        }

        if (FAILED(hr)) {
            report(CaptureStatus::DEVICE_REMOVED,
                   "AcquireNextFrame fatal: " + hr_msg(hr));
            break;
        }

        // -- Get D3D11 texture from the resource -----------------------------
        ID3D11Texture2D* frame_tex = nullptr;
        hr = resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                      reinterpret_cast<void**>(&frame_tex));
        resource->Release();

        if (SUCCEEDED(hr) && frame_tex) {
            // Push a CPU-accessible copy into the ring buffer.
            // The original DXGI texture is released immediately after push().
            ring_.push(frame_tex, d3d_device_);
            frame_tex->Release();

            // -- FPS calculation ---------------------------------------------
            ++fps_frames;
            ++stat_frames_;
            auto now = clock::now();
            double elapsed = std::chrono::duration<double>(now - fps_t0).count();
            if (elapsed >= 1.0) {
                stat_fps_.store(fps_frames / elapsed);
                fps_frames = 0;
                fps_t0     = now;
            }
        }

        duplication_->ReleaseFrame();
    }

    report(CaptureStatus::STOPPED, "Capture thread exited");
}

// -- stats() ------------------------------------------------------------------

CaptureStats CaptureThread::stats() const noexcept {
    CaptureStats s;
    s.width          = stat_width_.load();
    s.height         = stat_height_.load();
    s.actual_fps     = stat_fps_.load();
    s.frames_total   = stat_frames_.load();
    s.frames_dropped = ring_.frames_dropped();
    return s;
}

void CaptureThread::report(CaptureStatus s, const std::string& msg) {
    if (status_cb_) status_cb_(s, msg);
}

} // namespace homrec
