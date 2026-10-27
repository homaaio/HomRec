// replay_ram.h - the RAM Instant Replay buffer of ONE replay encoder run: the ffmpeg process' captured
// stdout (an MPEG-TS stream, see hr_ff_set_capture_stdout() in ffmpeg_runner.cpp) is drained by a reader thread into a
// ReplayRing (replay_ring.h). Nothing here touches Windows; hr_ff_read_stdout() is the only dependency.
//
// Lifetime rules (the controller follows them in StopInstantReplayEncoder*):
//   * start() once, right after hr_ff_start() succeeded;
//   * the reader ends by itself when ffmpeg exits (end-of-file on the pipe); join() waits for that - call it only after
//     the ffmpeg process has ended or been killed, and BEFORE hr_ff_destroy() (the pipe handle belongs to that object);
//   * destroying a ReplayRam joins first.
#pragma once

#include "replay_ring.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

extern "C" int hr_ff_read_stdout(void *h, void *buf, int cap);   // ffmpeg_runner.cpp

class ReplayRam {
public:
    ReplayRam(void *ff, double window_sec, size_t max_bytes) : ff_(ff), ring_(window_sec, max_bytes) {}
    ~ReplayRam() { join(); }
    ReplayRam(const ReplayRam &) = delete;
    ReplayRam &operator=(const ReplayRam &) = delete;

    void start() {
        if (reader_.joinable()) return;
        reader_ = std::thread([this]() { run(); });
    }
    void join() {
        if (reader_.joinable()) reader_.join();
    }

    ReplayRing &ring() { return ring_; }
    const ReplayRing &ring() const { return ring_; }
    bool reader_finished() const { return done_.load(); }
    unsigned long long bytes_read() const { return total_.load(); }

private:
    void run() {
        std::vector<uint8_t> buf(64 * 1024);
        for (;;) {
            const int n = hr_ff_read_stdout(ff_, buf.data(), (int)buf.size());
            if (n <= 0) break;                       // 0 = ffmpeg ended, -1 = error / no pipe
            ring_.push(buf.data(), (size_t)n);
            total_ += (unsigned long long)n;
        }
        done_ = true;
    }

    void *ff_;
    ReplayRing ring_;
    std::thread reader_;
    std::atomic<bool> done_{false};
    std::atomic<unsigned long long> total_{0};
};
