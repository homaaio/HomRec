// scripts/tests/test_replay_ring.cpp - standalone unit tests for src/core/replay_ring.h
// (the RAM buffer behind Instant Replay, item 7).
//
// The stream under test is REAL ffmpeg output (libx264 -> `-f mpegts`, what the replay encoder's stdout carries).
// If `ffmpeg` is not on PATH the ffmpeg-based cases are skipped, the synthetic ones still run.
//
// Build (any OS with a C++17 compiler):
//   g++ -std=c++17 -O1 -fsanitize=address,undefined -I src/core scripts/tests/test_replay_ring.cpp -o t && ./t

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "replay_ram.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

// Stand-in for ffmpeg_runner.cpp's hr_ff_read_stdout(): the "handle" is the FILE* of a popen()ed ffmpeg.
extern "C" int hr_ff_read_stdout(void *h, void *buf, int cap) {
    if (!h) return -1;
    const size_t n = std::fread(buf, 1, (size_t)cap, static_cast<FILE *>(h));
    return n > 0 ? (int)n : 0;      // 0 = EOF, exactly like the Windows version at ERROR_BROKEN_PIPE
}

namespace {

bool HaveFfmpeg() {
    static int have = -1;
    if (have < 0) have = (std::system("ffmpeg -version > /dev/null 2>&1") == 0) ? 1 : 0;
    return have == 1;
}

// Makes `sec` seconds of 320x180 test video with a keyframe every `gop` frames at 30 fps, as MPEG-TS bytes.
// ts_offset_sec shifts the timestamps (used to cross the 33-bit PTS wrap).
std::vector<uint8_t> MakeTs(int sec, int gop, double ts_offset_sec = 0.0) {
    const char *path = "/tmp/hr_ring_test.ts";
    char cmd[512];
    std::snprintf(cmd, sizeof(cmd),
                  "ffmpeg -v error -f lavfi -i testsrc2=size=320x180:rate=30 -t %d -c:v libx264 -preset ultrafast "
                  "-bf 0 -g %d -pix_fmt yuv420p -f mpegts -muxdelay 0 -muxpreload 0 -mpegts_flags +pat_pmt_at_frames "
                  "-output_ts_offset %f -y %s",
                  sec, gop, ts_offset_sec, path);
    if (std::system(cmd) != 0) return {};
    std::vector<uint8_t> v;
    if (FILE *f = std::fopen(path, "rb")) {
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) v.insert(v.end(), buf, buf + n);
        std::fclose(f);
    }
    return v;
}

// Pushes `data` in blocks of random size - the stream reader gets arbitrary pipe reads.
void PushRandom(ReplayRing &r, const std::vector<uint8_t> &data, uint32_t seed, size_t max_block = 5000) {
    std::mt19937 rng(seed);
    size_t i = 0;
    while (i < data.size()) {
        size_t n = 1 + rng() % max_block;
        if (n > data.size() - i) n = data.size() - i;
        r.push(data.data() + i, n);
        i += n;
    }
}

std::vector<uint8_t> Join(const std::vector<ReplayRing::Chunk> &parts) {
    std::vector<uint8_t> out;
    for (const auto &p : parts) out.insert(out.end(), p->begin(), p->end());
    return out;
}

// Counts the packets with the random-access flag on a video start (key pictures) in an aligned TS buffer.
int CountKeys(const std::vector<uint8_t> &v) {
    int k = 0;
    for (size_t i = 0; i + 188 <= v.size(); i += 188) {
        if (v[i] != 0x47) return -1;
        const int pid = ((v[i + 1] & 0x1f) << 8) | v[i + 2];
        const int afc = (v[i + 3] >> 4) & 3;
        if (pid == 0x100 && (v[i + 1] & 0x40) && (afc & 2) && v[i + 4] > 0 && (v[i + 5] & 0x40)) ++k;
    }
    return k;
}

} // namespace

TEST_CASE("keeps every GOP of a short stream, in order, packet aligned") {
    if (!HaveFfmpeg()) return;
    const auto ts = MakeTs(6, 30);
    REQUIRE(ts.size() > 188 * 100);
    ReplayRing r(60.0, 0);                       // window longer than the stream: nothing may be dropped
    PushRandom(r, ts, 1);
    double sec = 0;
    const auto out = Join(r.snapshot(&sec, /*final_flush=*/true));
    CHECK(out.size() % 188 == 0);
    CHECK(CountKeys(out) == 6);
    CHECK(sec == doctest::Approx(6.0).epsilon(0.05));
    // The kept stream is the input, only with PAT/PMT copies added in front of every chunk.
    CHECK(out.size() >= ts.size());
    CHECK(out.size() <= ts.size() + 6 * 2 * 188);
}

TEST_CASE("window trimming keeps at least the window and at most window + one GOP") {
    if (!HaveFfmpeg()) return;
    const auto ts = MakeTs(20, 30);              // 1 s GOPs
    ReplayRing r(5.0, 0);
    PushRandom(r, ts, 2);
    double sec = 0;
    const auto out = Join(r.snapshot(&sec, true));
    CHECK(sec >= 5.0 - 0.1);
    CHECK(sec <= 6.0 + 0.1);
    const int keys = CountKeys(out);
    CHECK(keys >= 5);
    CHECK(keys <= 6);
    CHECK(out.size() < ts.size());
}

TEST_CASE("the result is the same however the stream is cut into reads") {
    if (!HaveFfmpeg()) return;
    const auto ts = MakeTs(8, 30);
    ReplayRing a(3.0, 0), b(3.0, 0), c(3.0, 0);
    PushRandom(a, ts, 10, 7);                    // tiny, odd-sized reads
    PushRandom(b, ts, 11, 100000);               // big reads
    for (size_t i = 0; i < ts.size(); ++i) c.push(&ts[i], 1);   // byte by byte
    const auto oa = Join(a.snapshot(nullptr, true));
    const auto ob = Join(b.snapshot(nullptr, true));
    const auto oc = Join(c.snapshot(nullptr, true));
    CHECK(oa == ob);
    CHECK(oa == oc);
    CHECK(!oa.empty());
}

TEST_CASE("garbage bytes between packets are skipped and counted") {
    if (!HaveFfmpeg()) return;
    const auto ts = MakeTs(4, 30);
    ReplayRing clean(60.0, 0), dirty(60.0, 0);
    PushRandom(clean, ts, 3);
    // 13 junk bytes in front of every 500th packet.
    std::vector<uint8_t> bad;
    const uint8_t junk[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    for (size_t i = 0, p = 0; i + 188 <= ts.size(); i += 188, ++p) {
        if (p % 500 == 250) bad.insert(bad.end(), junk, junk + 13);
        bad.insert(bad.end(), ts.begin() + i, ts.begin() + i + 188);
    }
    PushRandom(dirty, bad, 4);
    CHECK(dirty.stats().resyncs > 0);
    CHECK(CountKeys(Join(dirty.snapshot(nullptr, true))) == CountKeys(Join(clean.snapshot(nullptr, true))));
}

TEST_CASE("the memory cap drops the oldest chunks first and says so") {
    if (!HaveFfmpeg()) return;
    const auto ts = MakeTs(12, 30);
    ReplayRing r(1000.0, ts.size() / 3);         // window never fills; the cap does
    PushRandom(r, ts, 5);
    const auto st = r.stats();
    CHECK(st.capped);
    CHECK(st.bytes <= ts.size() / 3 + 200 * 1024);   // cap + the chunk that was open when it was hit
    CHECK(st.bytes < ts.size());
    CHECK(CountKeys(Join(r.snapshot(nullptr, true))) >= 1);
}

TEST_CASE("a snapshot of a running stream stops before the picture being written") {
    if (!HaveFfmpeg()) return;
    const auto ts = MakeTs(3, 30);
    ReplayRing r(60.0, 0);
    const size_t cut = (ts.size() / 188) * 188 / 2 + 77;   // stop in the middle of a packet
    r.push(ts.data(), cut);
    const auto live = Join(r.snapshot(nullptr, false));
    const auto full = Join(r.snapshot(nullptr, true));
    CHECK(live.size() % 188 == 0);
    CHECK(live.size() <= full.size());
}

TEST_CASE("33-bit PTS wrap does not break the length") {
    if (!HaveFfmpeg()) return;
    // 2^33 ticks / 90 kHz = 95443.7 s: start 3 s before the wrap, run 10 s.
    const auto ts = MakeTs(10, 30, 95443.0 - 3.0);
    REQUIRE(!ts.empty());
    ReplayRing r(60.0, 0);
    PushRandom(r, ts, 6);
    double sec = 0;
    r.snapshot(&sec, true);
    CHECK(sec == doctest::Approx(10.0).epsilon(0.05));
}

TEST_CASE("nothing usable before the first key picture / on empty input") {
    ReplayRing r(10.0, 0);
    double sec = -1;
    CHECK(r.snapshot(&sec, true).empty());
    CHECK(sec == 0.0);
    std::vector<uint8_t> junk(5000, 0x47);          // sync bytes but no valid structure
    r.push(junk.data(), junk.size());
    const auto st = r.stats();
    CHECK(!st.have_video);
    r.clear();
    CHECK(r.stats().bytes == 0);
}

TEST_CASE("ReplayRam: the reader thread drains a live ffmpeg pipe until it ends") {
    if (!HaveFfmpeg()) return;
    // Real encoder process writing the stream to ITS stdout, like the replay encoder does on Windows.
    FILE *ff = popen("ffmpeg -v error -f lavfi -i testsrc2=size=320x180:rate=30 -t 12 -c:v libx264 -preset ultrafast "
                     "-bf 0 -g 30 -pix_fmt yuv420p -f mpegts -muxdelay 0 -muxpreload 0 -mpegts_flags +pat_pmt_at_frames pipe:1",
                     "r");
    REQUIRE(ff != nullptr);
    {
        ReplayRam ram(ff, 4.0, 0);
        ram.start();
        ram.join();                                // returns once ffmpeg closed its stdout
        CHECK(ram.reader_finished());
        CHECK(ram.bytes_read() > 188 * 100);
        double sec = 0;
        const auto out = Join(ram.ring().snapshot(&sec, true));
        CHECK(sec >= 4.0 - 0.1);
        CHECK(sec <= 5.0 + 0.1);
        CHECK(CountKeys(out) >= 4);
        CHECK(out.size() % 188 == 0);
    }
    pclose(ff);
}

TEST_CASE("ReplayRam: reader on a dead / missing pipe ends at once") {
    ReplayRam ram(nullptr, 4.0, 0);
    ram.start();
    ram.join();
    CHECK(ram.reader_finished());
    CHECK(ram.bytes_read() == 0);
}
