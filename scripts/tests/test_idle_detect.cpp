// scripts/tests/test_idle_detect.cpp - unit tests for src/core/idle_detect.h
// (the decision logic of "auto-pause when the screen is idle").
//
// Build (any OS with a C++17 compiler):
//   g++ -std=c++17 -I src -I src/core -I scripts/tests scripts/tests/test_idle_detect.cpp -o t && ./t

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "idle_detect.h"

#include <cstdint>

using HrIdle::Action;
using HrIdle::Config;
using HrIdle::Detector;

namespace {

constexpr uint64_t kArea = 1920ull * 1080ull;   // 2 073 600 px
constexpr double   kTick = 0.5;                  // PollStats runs every 500 ms

// Drives a Detector the way RecordingController::PollIdle() does: one reading per tick,
// `px_per_sec` newly changed pixels per second, and the pause state fed back in.
struct Sim {
    Detector det;
    double   now = 0.0;
    uint64_t total = 0;
    bool     paused = false;      // paused by the detector
    int      pauses = 0, resumes = 0;
    double   first_pause_at = -1.0;

    explicit Sim(Config c = Config()) { det.SetConfig(c); det.Reset(0.0); }

    Action Step(double px_per_sec, bool probe_ok = true) {
        now   += kTick;
        total += (uint64_t)(px_per_sec * kTick);
        Action a = det.Update(now, total, kArea, probe_ok, paused);
        if (a == Action::Pause)  { paused = true;  ++pauses;  if (first_pause_at < 0) first_pause_at = now; }
        if (a == Action::Resume) { paused = false; ++resumes; det.Reset(now); }
        return a;
    }
    void Run(double seconds, double px_per_sec, bool probe_ok = true) {
        const int n = (int)(seconds / kTick + 0.5);
        for (int i = 0; i < n; ++i) Step(px_per_sec, probe_ok);
    }
};

} // namespace

TEST_CASE("idle: a frozen screen pauses after idle_sec, not before") {
    Config c; c.idle_sec = 5.0;
    Sim s(c);
    s.Run(4.0, 0);
    CHECK(s.pauses == 0);
    s.Run(2.0, 0);
    CHECK(s.pauses == 1);
    CHECK(s.first_pause_at >= 5.0);
    CHECK(s.first_pause_at <= 6.0);
}

TEST_CASE("idle: steady activity never pauses") {
    Sim s;
    s.Run(120.0, 200000.0);           // ~10 % of the screen per second
    CHECK(s.pauses == 0);
    CHECK(s.det.rate_known());
    CHECK(s.det.rate_pct() > 5.0);
}

TEST_CASE("idle: typing-sized activity keeps recording at the default threshold") {
    Sim s;
    s.Run(60.0, 1000.0);              // ~0.048 % per second, about 5 characters per second
    CHECK(s.pauses == 0);
}

TEST_CASE("idle: a blinking caret and a minute clock count as idle") {
    Sim s;
    s.Run(30.0, 40.0);                // a few dozen pixels per second
    CHECK(s.pauses == 1);
}

TEST_CASE("idle: sparse updates are averaged over the window, not judged one tick at a time") {
    // A clock that repaints 1200 px once per second: half of the 500 ms ticks see nothing at all.
    Config c; c.threshold_pct = 0.02;
    Sim s(c);
    for (int i = 0; i < 120; ++i) {
        s.Step((i % 2 == 0) ? 2400.0 : 0.0);
        CHECK(s.paused == false);
    }
    // The same clock is "idle" once the threshold is raised above its rate (0.058 % per second).
    Config c2; c2.threshold_pct = 0.1;
    Sim s2(c2);
    for (int i = 0; i < 40; ++i) s2.Step((i % 2 == 0) ? 2400.0 : 0.0);
    CHECK(s2.pauses == 1);
}

TEST_CASE("idle: long activity before a freeze does not delay the pause (the window slides)") {
    Config c; c.idle_sec = 5.0; c.window_sec = 1.0;
    Sim s(c);
    s.Run(300.0, 400000.0);           // five minutes of video playing...
    REQUIRE(s.pauses == 0);
    const double stopped_at = s.now;
    s.Run(15.0, 0);                   // ...then the picture freezes
    REQUIRE(s.pauses == 1);
    // idle_sec plus the window the old activity still lingers in plus one tick - nothing like minutes
    CHECK(s.first_pause_at - stopped_at <= 5.0 + 1.0 + kTick + 0.01);
}

TEST_CASE("idle: activity resumes the recording at once") {
    Sim s;
    s.Run(10.0, 0);
    REQUIRE(s.paused);
    const double t = s.now;
    s.Step(300000.0);                 // a window dragged across the screen
    CHECK_FALSE(s.paused);
    CHECK(s.resumes == 1);
    CHECK(s.now - t <= 1.0);
}

TEST_CASE("idle: after a resume the idle timer starts again") {
    Config c; c.idle_sec = 5.0;
    Sim s(c);
    s.Run(10.0, 0);
    REQUIRE(s.paused);
    s.Step(300000.0);
    REQUIRE_FALSE(s.paused);
    const int before = s.pauses;
    s.Run(3.0, 0);
    CHECK(s.pauses == before);        // 3 s of calm is not enough
    s.Run(4.0, 0);
    CHECK(s.pauses == before + 1);
}

TEST_CASE("idle: a start on an already idle screen waits the full idle time") {
    Config c; c.idle_sec = 8.0;
    Sim s(c);
    s.Run(7.0, 0);
    CHECK(s.pauses == 0);
    s.Run(2.0, 0);
    CHECK(s.pauses == 1);
}

TEST_CASE("idle: a manual pause is never resumed by the detector") {
    Detector d;
    d.Reset(0.0);
    uint64_t total = 0;
    // paused_by_us = false the whole time (the user paused by hand): activity must not give Resume.
    for (int i = 1; i <= 40; ++i) {
        total += 100000;
        Action a = d.Update(i * 0.5, total, kArea, true, /*paused_by_us=*/false);
        CHECK(a != Action::Resume);
    }
}

TEST_CASE("idle: a missing signal never pauses and gives a paused recording back") {
    Sim s;
    s.Run(60.0, 0, /*probe_ok=*/false);
    CHECK(s.pauses == 0);
    // Pause normally, then lose the signal: the recording must continue.
    Sim s2;
    s2.Run(10.0, 0);
    REQUIRE(s2.paused);
    s2.Step(0, /*probe_ok=*/false);
    CHECK_FALSE(s2.paused);
    CHECK(s2.resumes == 1);
}

TEST_CASE("idle: an empty watch area counts as no signal") {
    Detector d;
    d.Reset(0.0);
    for (int i = 1; i <= 40; ++i)
        CHECK(d.Update(i * 0.5, 0, /*area_px=*/0, true, false) == Action::None);
}

TEST_CASE("idle: a probe restart (total goes down) does not fake a huge rate or a pause") {
    Sim s;
    s.Run(4.0, 50000.0);              // some activity, so the running total is well above zero
    REQUIRE(s.total > 0);
    s.total = 0;                      // the probe was recreated, its counter starts over
    s.Run(1.0, 0);
    CHECK(s.det.rate_pct() < 0.01);   // not a wrapped-around 18-quintillion-pixel burst
    CHECK(s.pauses == 0);             // and no pause yet either: only 1 s of calm
    s.Run(6.0, 0);
    CHECK(s.pauses == 1);             // the idle screen is still noticed afterwards
}

TEST_CASE("idle: the first reading after a reset has no rate and no division by zero") {
    Detector d;
    d.Reset(10.0);
    CHECK(d.Update(10.0, 0, kArea, true, false) == Action::None);
    CHECK_FALSE(d.rate_known());
    CHECK(d.Update(10.0, 500000, kArea, true, false) == Action::None);   // same instant again
    CHECK_FALSE(d.rate_known());
}

TEST_CASE("idle: a clock that goes backwards does not freeze the detector") {
    Config c; c.idle_sec = 5.0;
    Detector d; d.SetConfig(c);
    d.Reset(100.0);
    d.Update(100.5, 0, kArea, true, false);
    CHECK(d.Update(50.0, 0, kArea, true, false) == Action::None);
    // From the new time line on, an idle screen is still noticed after idle_sec - not after 50 more seconds.
    Action last = Action::None;
    double t = 50.0;
    for (int i = 0; i < 16 && last != Action::Pause; ++i) { t += 0.5; last = d.Update(t, 0, kArea, true, false); }
    CHECK(last == Action::Pause);
    CHECK(t - 50.0 <= 8.0);
}

TEST_CASE("idle: time without a signal does not count as idle time") {
    Sim s;
    s.Run(60.0, 0, /*probe_ok=*/false);
    s.Run(3.0, 0);                    // the signal is back and the screen is quiet
    CHECK(s.pauses == 0);             // 3 s of calm is not 5 s
    s.Run(4.0, 0);
    CHECK(s.pauses == 1);
}

TEST_CASE("idle: config values are clamped") {
    CHECK(HrIdle::ClampThresholdPct(0.0)   == doctest::Approx(0.001));
    CHECK(HrIdle::ClampThresholdPct(-5.0)  == doctest::Approx(0.001));
    CHECK(HrIdle::ClampThresholdPct(250.0) == doctest::Approx(100.0));
    CHECK(HrIdle::ClampThresholdPct(0.5)   == doctest::Approx(0.5));
    CHECK(HrIdle::ClampIdleSec(0.0)        == doctest::Approx(1.0));
    CHECK(HrIdle::ClampIdleSec(99999.0)    == doctest::Approx(600.0));
    Detector d;
    Config c; c.threshold_pct = 0.0; c.idle_sec = -3.0; c.window_sec = 0.0;
    d.SetConfig(c);
    CHECK(d.config().threshold_pct >= 0.001);
    CHECK(d.config().idle_sec >= 1.0);
    CHECK(d.config().window_sec >= 0.25);
}
