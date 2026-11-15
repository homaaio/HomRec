// idle_detect.h - decides when a recording should be auto-paused because nothing is happening on the
// screen, and when it should continue. Header-only and free of any OS call, so scripts/tests/
// test_idle_detect.cpp can run it on any machine.
//
// Input: a running total of "changed pixels" that the capture side counts (see hr_dx_activity_* in
// dxgi_capture.cpp: the area of the dirty rectangles DXGI reports; pointer movement is not counted), plus
// the size of the watched area. The detector looks at how much of that area changed per second over a short
// sliding window.
//
// Fail-safe rule: whenever the signal is missing or doubtful the detector says "active". A wrong
// "idle" loses footage for good; a wrong "active" only records a little more than needed.
#pragma once

#include <cstdint>
#include <deque>

namespace HrIdle {

struct Config {
    double threshold_pct = 0.02;
    double idle_sec = 5.0;
    double window_sec = 1.0;
};

enum class Action { None, Pause, Resume };

inline double ClampThresholdPct(double v) { return v < 0.001 ? 0.001 : (v > 100.0 ? 100.0 : v); }
inline double ClampIdleSec(double v)      { return v < 1.0 ? 1.0 : (v > 600.0 ? 600.0 : v); }

class Detector {
public:
    void SetConfig(const Config &c) {
        cfg_ = c;
        cfg_.threshold_pct = ClampThresholdPct(cfg_.threshold_pct);
        cfg_.idle_sec      = ClampIdleSec(cfg_.idle_sec);
        if (cfg_.window_sec < 0.25) cfg_.window_sec = 0.25;
        if (cfg_.window_sec > 5.0)  cfg_.window_sec = 5.0;
    }
    const Config &config() const { return cfg_; }

    // Start counting from "now": the screen is considered active at this moment, so an idle screen has
    // to stay idle for the whole idle_sec before the first pause. Call at the start of a recording and
    // whenever the recording is resumed (by us or by hand).
    void Reset(double now_sec) {
        samples_.clear();
        last_active_sec_ = now_sec;
        have_time_ = true;
        rate_pct_ = 0.0;
        rate_known_ = false;
    }

    Action Update(double now_sec, uint64_t changed_px_total, uint64_t area_px, bool probe_ok, bool paused_by_us) {
        if (!have_time_) Reset(now_sec);
        if (now_sec < last_sample_sec_) { // the clock went backwards: start over from here
            samples_.clear();
            last_active_sec_ = now_sec;
        }
        last_sample_sec_ = now_sec;

        // No usable signal: never pause, and give a recording we paused back.
        if (!probe_ok || area_px == 0) {
            samples_.clear();
            rate_known_ = false;
            last_active_sec_ = now_sec;
            return paused_by_us ? Action::Resume : Action::None;
        }

        // A total that went down means the probe was restarted: its history is worthless.
        if (!samples_.empty() && changed_px_total < samples_.back().total) samples_.clear();
        samples_.push_back({now_sec, changed_px_total});

        // Keep exactly one sample at or before the start of the window as its anchor.
        while (samples_.size() >= 2 && samples_[1].t <= now_sec - cfg_.window_sec) samples_.pop_front();

        const Sample &a = samples_.front();
        const double span = now_sec - a.t;
        if (span < 0.2) { rate_known_ = false; return Action::None; }   // not enough history yet

        const double px_per_sec = double(changed_px_total - a.total) / span;
        rate_pct_ = 100.0 * px_per_sec / double(area_px);
        rate_known_ = true;
        const bool active = rate_pct_ >= cfg_.threshold_pct;

        if (paused_by_us) {
            if (active) { last_active_sec_ = now_sec; return Action::Resume; }
            return Action::None;
        }
        if (active) { last_active_sec_ = now_sec; return Action::None; }
        if (now_sec - last_active_sec_ >= cfg_.idle_sec) return Action::Pause;
        return Action::None;
    }

    // The last measured change rate in percent of the area per second (for the log). Only
    // meaningful when rate_known() is true.
    double rate_pct() const { return rate_pct_; }
    bool   rate_known() const { return rate_known_; }

private:
    struct Sample { double t; uint64_t total; };
    Config cfg_;
    std::deque<Sample> samples_;
    double last_active_sec_ = 0.0;
    double last_sample_sec_ = 0.0;
    bool   have_time_ = false;
    double rate_pct_ = 0.0;
    bool   rate_known_ = false;
};

} // namespace HrIdle
