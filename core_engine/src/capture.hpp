#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <dxgi1_2.h>
#include <d3d11.h>
#include "ring_buffer.hpp"

namespace homrec {

// Status codes the capture thread reports to Python via callback
enum class CaptureStatus {
    OK,
    DXGI_ACCESS_LOST,   // Alt+Tab in exclusive fullscreen
    DXGI_TIMEOUT,       // No new frame within timeout
    DEVICE_REMOVED,
    INIT_FAILED,
    STOPPED,
};

struct CaptureStats {
    UINT32 width         = 0;
    UINT32 height        = 0;
    double actual_fps    = 0.0;
    UINT64 frames_total  = 0;
    UINT64 frames_dropped = 0;
};

// Callback: Python-side sets this via bindings to react to status changes
using StatusCallback = std::function<void(CaptureStatus, const std::string&)>;

// ---------------------------------------------------------------------------
class CaptureThread {
public:
    explicit CaptureThread(MainRingBuffer& ring_buf);
    ~CaptureThread();

    // init_capture() must be called before start().
    // monitor_index: 0 = primary monitor, 1 = second, etc.
    // hwnd: optional — if non-null, captures only that window's region.
    bool init(int monitor_index = 0, HWND hwnd = nullptr);

    void start();
    void stop();

    void set_status_callback(StatusCallback cb) { status_cb_ = std::move(cb); }

    CaptureStats stats() const noexcept;
    bool is_running()    const noexcept { return running_.load(); }
    ID3D11Device*        device()  const noexcept { return d3d_device_;  }
    ID3D11DeviceContext* context() const noexcept { return d3d_context_; }

private:
    void thread_main();

    // Attempt to (re)acquire IDXGIOutputDuplication.
    // Returns true on success.  Must be called from the capture thread.
    bool acquire_duplication();
    void release_duplication();

    // Report status to Python callback (thread-safe string copy)
    void report(CaptureStatus s, const std::string& msg = "");

    // -- D3D11 objects ----------------------------------------------------
    ID3D11Device*             d3d_device_   = nullptr;
    ID3D11DeviceContext*      d3d_context_  = nullptr;
    IDXGIOutputDuplication*   duplication_  = nullptr;
    IDXGIAdapter*             adapter_      = nullptr;
    IDXGIOutput*              output_       = nullptr;
    IDXGIOutput1*             output1_      = nullptr;

    // -- Config ----------------------------------------------------------
    int  monitor_index_ = 0;
    HWND target_hwnd_   = nullptr;
    RECT target_rect_   = {0, 0, 0, 0};   // set when hwnd != nullptr

    // -- Ring buffer reference -------------------------------------------
    MainRingBuffer& ring_;

    // -- Thread control --------------------------------------------------
    std::thread          thread_;
    std::atomic<bool>    running_{false};
    std::atomic<bool>    stop_requested_{false};

    // -- Stats -----------------------------------------------------------
    mutable std::atomic<UINT32>  stat_width_{0};
    mutable std::atomic<UINT32>  stat_height_{0};
    mutable std::atomic<double>  stat_fps_{0.0};
    mutable std::atomic<UINT64>  stat_frames_{0};

    // -- Callback -------------------------------------------------------
    StatusCallback status_cb_;
};

} // namespace homrec
