// scripts/tests/test_audio_mixer.cpp - standalone unit tests for src/core/audio_mixer.h
// (the real-time mixer + streaming resampler behind the in-process audio path, item 10).
//
// Build (any OS with a C++11 compiler):
//   g++ -std=c++17 -O1 -fsanitize=address,undefined -I src/core scripts/tests/test_audio_mixer.cpp -o t && ./t

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "audio_mixer.h"

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using hrlive::LiveMixer;
using hrlive::StereoResampler;

namespace {

std::vector<int16_t> RandomStereo(size_t frames, uint32_t seed, int amp = 30000) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> d(-amp, amp);
    std::vector<int16_t> v(frames * 2);
    for (auto &x : v) x = (int16_t)d(rng);
    return v;
}

// Feeds `in` to the resampler in blocks of random size (1..max_block frames).
std::vector<int16_t> ResampleChunked(const std::vector<int16_t> &in, uint32_t from, uint32_t to,
                                     size_t max_block, uint32_t seed) {
    StereoResampler rs;
    rs.init(from, to);
    std::vector<int16_t> out;
    std::mt19937 rng(seed);
    size_t pos = 0, frames = in.size() / 2;
    while (pos < frames) {
        size_t n = 1 + rng() % max_block;
        if (n > frames - pos) n = frames - pos;
        rs.process(in.data() + pos * 2, n, out);
        pos += n;
    }
    return out;
}

} // namespace

TEST_CASE("resampler: equal rates are a plain copy") {
    auto in = RandomStereo(1000, 1);
    StereoResampler rs;
    rs.init(48000, 48000);
    CHECK(rs.passthrough());
    std::vector<int16_t> out;
    rs.process(in.data(), 1000, out);
    CHECK(out == in);
}

TEST_CASE("resampler: result does not depend on how the input is chopped into blocks") {
    const uint32_t pairs[][2] = {{44100, 48000}, {48000, 44100}, {96000, 48000}, {22050, 48000}, {44100, 44100}};
    for (const auto &p : pairs) {
        auto in = RandomStereo(20000, 7);
        auto whole = ResampleChunked(in, p[0], p[1], 20000, 1);
        for (size_t blk : {1u, 7u, 480u, 4410u}) {
            auto chunked = ResampleChunked(in, p[0], p[1], blk, 99);
            CHECK(chunked == whole);
        }
    }
}

TEST_CASE("resampler: output length matches the rate ratio") {
    auto in = RandomStereo(44100, 3);                       // exactly 1 s at 44.1 kHz
    auto out = ResampleChunked(in, 44100, 48000, 441, 5);
    const double frames = (double)out.size() / 2;
    CHECK(std::fabs(frames - 48000.0) <= 2.0);
    auto in2 = RandomStereo(48000, 3);
    auto out2 = ResampleChunked(in2, 48000, 44100, 480, 5);
    CHECK(std::fabs((double)out2.size() / 2 - 44100.0) <= 2.0);
}

TEST_CASE("resampler: a 1 kHz sine stays a 1 kHz sine (44.1k -> 48k)") {
    const double kPi = 3.14159265358979323846;
    const size_t N = 44100;
    std::vector<int16_t> in(N * 2);
    for (size_t i = 0; i < N; ++i) {
        const int16_t s = (int16_t)(20000.0 * std::sin(2.0 * kPi * 1000.0 * (double)i / 44100.0));
        in[2 * i] = s;
        in[2 * i + 1] = (int16_t)-s;
    }
    auto out = ResampleChunked(in, 44100, 48000, 333, 11);
    double worst = 0;
    for (size_t m = 0; m + 1 < out.size() / 2 && m < 47000; ++m) {
        const double ideal = 20000.0 * std::sin(2.0 * kPi * 1000.0 * (double)m / 48000.0);
        worst = std::max(worst, std::fabs(ideal - (double)out[2 * m]));
        worst = std::max(worst, std::fabs(-ideal - (double)out[2 * m + 1]));
    }
    // Linear interpolation error of a 1 kHz tone at 44.1 kHz is ~0.25 % of the amplitude (~50 LSB here).
    CHECK(worst < 120.0);
}

TEST_CASE("mixer: two sources at one rate = clipped sum, whatever the delivery pattern") {
    const size_t frames = 48000 * 2;
    auto a = RandomStereo(frames, 21, 20000);
    auto b = RandomStereo(frames, 22, 20000);

    LiveMixer mx;
    mx.init(48000);
    const int ia = mx.add_push(48000);
    const int ib = mx.add_push(48000);

    std::vector<int16_t> out;
    std::mt19937 rng(5);
    // 10 ms time steps: A delivers its 480 frames every step, B (a "bursty" device) holds its audio back
    // for 1..5 steps and then delivers everything it owes at once - the way WASAPI packets arrive.
    size_t pa = 0, pb = 0, owed_b = 0;
    int hold = 1;
    while (pa < frames || pb < frames) {
        if (pa < frames) {
            size_t n = std::min<size_t>(480, frames - pa);
            mx.push(ia, a.data() + pa * 2, n * 2);
            pa += n;
        }
        if (pb + owed_b < frames) owed_b += std::min<size_t>(480, frames - pb - owed_b);
        if (--hold <= 0 || pb + owed_b >= frames) {
            mx.push(ib, b.data() + pb * 2, owed_b * 2);
            pb += owed_b;
            owed_b = 0;
            hold = 1 + (int)(rng() % 5);
        }
        mx.mix(out, false);
    }
    mx.mix(out, true);

    REQUIRE(out.size() == frames * 2);
    for (size_t i = 0; i < out.size(); ++i) {
        int v = (int)a[i] + (int)b[i];
        v = std::max(-32768, std::min(32767, v));
        REQUIRE((int)out[i] == v);
    }
    CHECK(mx.frames_padded() == 0);
    CHECK(mx.frames_dropped() == 0);
}

TEST_CASE("mixer: a mixed-rate pair comes out at the output rate with the right length") {
    LiveMixer mx;
    mx.init(48000);
    const int mic = mx.add_push(44100);
    const int sys = mx.add_push(48000);
    auto m = RandomStereo(44100 * 3, 31);
    auto s = RandomStereo(48000 * 3, 32);
    std::vector<int16_t> out;
    for (size_t pos = 0; pos < 3 * 100; ++pos) {              // 10 ms slices
        mx.push(mic, m.data() + pos * 441 * 2, 441 * 2);
        mx.push(sys, s.data() + pos * 480 * 2, 480 * 2);
        mx.mix(out, false);
    }
    mx.mix(out, true);
    const double frames = (double)out.size() / 2;
    CHECK(frames >= 3 * 48000 - 4);
    CHECK(frames <= 3 * 48000 + 4);
}

TEST_CASE("mixer: head silence delays the audio, a negative head cuts its start") {
    auto a = RandomStereo(4800, 41, 10000);
    {
        LiveMixer mx;
        mx.init(48000);
        const int ia = mx.add_push(48000);
        mx.set_head(480);
        mx.push(ia, a.data(), a.size());
        std::vector<int16_t> out;
        mx.mix(out, true);
        REQUIRE(out.size() == (4800 + 480) * 2);
        for (size_t i = 0; i < 480 * 2; ++i) REQUIRE(out[i] == 0);
        for (size_t i = 0; i < a.size(); ++i) REQUIRE(out[480 * 2 + i] == a[i]);
    }
    {
        LiveMixer mx;
        mx.init(48000);
        const int ia = mx.add_push(48000);
        mx.set_head(-480);
        mx.push(ia, a.data(), a.size());
        std::vector<int16_t> out;
        mx.mix(out, true);
        REQUIRE(out.size() == (4800 - 480) * 2);
        for (size_t i = 0; i < out.size(); ++i) REQUIRE(out[i] == a[480 * 2 + i]);
    }
}

TEST_CASE("mixer: a stalled source is not waited for, and its late data lands in the right place") {
    // A delivers 1 s up front. B delivers nothing for that second, then its first second arrives late.
    const size_t kRate = 48000;
    std::vector<int16_t> a(kRate * 2), b(kRate * 2);
    for (size_t i = 0; i < kRate; ++i) {
        a[2 * i] = a[2 * i + 1] = 100;
        b[2 * i] = b[2 * i + 1] = (int16_t)((i % 997) + 1); // never 0, identifies its true position
    }
    LiveMixer mx;
    mx.init(kRate); // max lag = 12000 frames
    const int ia = mx.add_push(kRate);
    const int ib = mx.add_push(kRate);

    std::vector<int16_t> out;
    mx.push(ia, a.data(), a.size());
    mx.mix(out, false);
    const size_t emitted_stalled = out.size() / 2;
    CHECK(emitted_stalled == kRate - kRate / 4); // everything but the allowed lag

    mx.push(ib, b.data(), b.size()); // B wakes up with its first second
    mx.mix(out, true);
    REQUIRE(out.size() / 2 == kRate);

    for (size_t p = 0; p < emitted_stalled; ++p) REQUIRE(out[2 * p] == 100);                       // B was silent
    for (size_t p = emitted_stalled; p < kRate; ++p) REQUIRE(out[2 * p] == 100 + b[2 * p]);      // B aligned again
    CHECK(mx.frames_padded() == emitted_stalled);
    CHECK(mx.frames_dropped() == emitted_stalled);
}

namespace {
void AddConstant(void *ctx, int32_t *acc, size_t frames) {
    const int v = *static_cast<int *>(ctx);
    for (size_t i = 0; i < frames * 2; ++i) acc[i] += v;
    }
}

TEST_CASE("mixer: a pull source is asked for exactly as many frames as are emitted") {
    LiveMixer mx;
    mx.init(48000);
    const int ia = mx.add_push(48000);
    int level = 500;
    mx.add_pull(AddConstant, &level);
    auto a = RandomStereo(1000, 51, 1000);
    mx.push(ia, a.data(), a.size());
    std::vector<int16_t> out;
    mx.mix(out, false);
    REQUIRE(out.size() == a.size());
    for (size_t i = 0; i < a.size(); ++i) REQUIRE(out[i] == a[i] + 500);
}

TEST_CASE("mixer: nothing to emit without push sources") {
    LiveMixer mx;
    mx.init(48000);
    int level = 500;
    mx.add_pull(AddConstant, &level);
    std::vector<int16_t> out;
    CHECK(mx.mix(out, true) == 0);
    CHECK(out.empty());
}

TEST_CASE("mixer: clipping instead of wrap-around") {
    LiveMixer mx;
    mx.init(48000);
    const int ia = mx.add_push(48000);
    const int ib = mx.add_push(48000);
    std::vector<int16_t> hi(200, 30000), lo(200, -30000);
    mx.push(ia, hi.data(), hi.size());
    mx.push(ib, hi.data(), hi.size());
    std::vector<int16_t> out;
    mx.mix(out, true);
    for (auto v : out) REQUIRE(v == 32767);
    LiveMixer my;
    my.init(48000);
    const int ja = my.add_push(48000);
    const int jb = my.add_push(48000);
    my.push(ja, lo.data(), lo.size());
    my.push(jb, lo.data(), lo.size());
    out.clear();
    my.mix(out, true);
    for (auto v : out) REQUIRE(v == -32768);
}
