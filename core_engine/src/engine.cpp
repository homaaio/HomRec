#include "engine.hpp"

namespace homrec {

Engine::Engine()
    : ring_()
    , capture_(ring_)
    , encoder_(ring_)
{}

Engine::~Engine() {
    stop_recording();
    stop_capture();
}

// -- init_capture -------------------------------------------------------------

bool Engine::init_capture(int monitor_index, HWND hwnd) {
    capture_.set_status_callback([this](CaptureStatus s, const std::string& msg) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (cap_cb_) cap_cb_(msg);
    });

    if (!capture_.init(monitor_index, hwnd)) return false;
    capture_.start();
    return true;
}

// -- start_recording ----------------------------------------------------------

bool Engine::start_recording(const EncoderConfig& cfg) {
    if (!capture_.is_running()) return false;

    encoder_.set_callback([this](EncoderStatus s, const std::string& msg) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (enc_cb_) enc_cb_(msg);
    });

    return encoder_.start(cfg, capture_.device(), capture_.context());
}

// -- stop_recording -----------------------------------------------------------

void Engine::stop_recording() {
    encoder_.stop();
}

// -- stop_capture -------------------------------------------------------------

void Engine::stop_capture() {
    encoder_.stop();
    capture_.stop();
    ring_.clear();
}

// -- stats ---------------------------------------------------------------------

EngineStats Engine::stats() const {
    EngineStats s;
    auto cs = capture_.stats();
    s.capture_width   = cs.width;
    s.capture_height  = cs.height;
    s.capture_fps     = cs.actual_fps;
    s.frames_captured = cs.frames_total;
    s.frames_dropped  = cs.frames_dropped;
    s.encode_fps      = encoder_.encoding_fps();
    s.frames_encoded  = encoder_.frames_encoded();
    s.is_recording    = (encoder_.status() == EncoderStatus::RECORDING);
    return s;
}

bool Engine::is_capture_running() const noexcept { return capture_.is_running(); }
bool Engine::is_recording()       const noexcept {
    return encoder_.status() == EncoderStatus::RECORDING;
}

void Engine::set_capture_callback(StatusFn fn) {
    std::lock_guard<std::mutex> lk(mtx_);
    cap_cb_ = std::move(fn);
}
void Engine::set_encoder_callback(StatusFn fn) {
    std::lock_guard<std::mutex> lk(mtx_);
    enc_cb_ = std::move(fn);
}

} // namespace homrec
