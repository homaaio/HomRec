#pragma once
#include "capture.hpp"
#include "encoder.hpp"
#include "ring_buffer.hpp"
#include <mutex>
#include <string>

namespace homrec {

struct EngineStats {
    // Capture
    UINT32 capture_width    = 0;
    UINT32 capture_height   = 0;
    double capture_fps      = 0.0;
    UINT64 frames_captured  = 0;
    UINT64 frames_dropped   = 0;
    // Encoder
    double encode_fps       = 0.0;
    UINT64 frames_encoded   = 0;
    bool   is_recording     = false;
    std::string encoder_status;
};

class Engine {
public:
    Engine();
    ~Engine();

    // -- Lifecycle ------------------------------------------------------------

    // init_capture: call once at startup or when monitor changes.
    // monitor_index: 0 = primary; hwnd: 0 = full desktop
    bool init_capture(int monitor_index = 0, HWND hwnd = nullptr);

    // start_recording: begin writing to file
    bool start_recording(const EncoderConfig& cfg);

    // stop_recording: finish file gracefully (blocks until encoder exits)
    void stop_recording();

    // stop_capture: full shutdown
    void stop_capture();

    // -- Info -----------------------------------------------------------------
    EngineStats stats() const;
    bool is_capture_running() const noexcept;
    bool is_recording()       const noexcept;

    // -- Callbacks (called from C++ threads — must be thread-safe) ------------
    using StatusFn = std::function<void(const std::string&)>;
    void set_capture_callback(StatusFn fn);
    void set_encoder_callback(StatusFn fn);

private:
    MainRingBuffer   ring_;
    CaptureThread    capture_;
    EncoderThread    encoder_;

    mutable std::mutex mtx_;
    StatusFn           cap_cb_;
    StatusFn           enc_cb_;
};

} // namespace homrec
