// audio_pipe.cpp - item 10: the "audio sink" that feeds ffmpeg's SECOND input (the mixed recording audio) through
// a named pipe while the recording is running, so ffmpeg encodes AAC and muxes it into the output file live -
// no WAV files, no merge pass after Stop.

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>

  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <deque>
  #include <mutex>
  #include <string>
  #include <thread>
  #include <vector>

  #include "../utils/log.h"

namespace {

// 60 s of 48 kHz stereo. ffmpeg connects to the pipe only after it has probed the VIDEO input, which can take a
// while right after launch (the capture pipeline is still starting), and everything recorded meanwhile waits here.
constexpr size_t kMaxQueuedSamples = 48000u * 2u * 60u;

struct PipeSink {
    HANDLE                 pipe = INVALID_HANDLE_VALUE;
    int                    rate = 48000;

    std::thread            th;
    std::mutex             mu;
    std::condition_variable cv;
    std::deque<std::vector<int16_t>> q;
    size_t                 q_samples = 0;
    bool                   finishing = false;     // no more input: drain and close
    bool                   failed = false;        // the reader went away / an I/O error
    bool                   connected = false;
    bool                   overflow_logged = false;
    uint64_t               frames_in = 0, frames_dropped = 0;       // lock: mu
    std::atomic<uint64_t>  frames_out{0};                            // written by the writer thread only
    std::atomic<bool>      done{false};           // writer thread has left
    // A real Win32 handle of the writer thread (std::thread::native_handle() is a pthread_t with MinGW's posix
    // threads, not a HANDLE). Set by the thread itself first thing; used by CancelSynchronousIo().
    std::atomic<HANDLE>    thread_handle{nullptr};

    void run() {
        {
            HANDLE dup = nullptr;
            if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &dup, 0, FALSE,
                                DUPLICATE_SAME_ACCESS))
                thread_handle.store(dup);
        }
        // ---- wait for ffmpeg to open its end ------------------------------------------------------------
        BOOL ok = ConnectNamedPipe(pipe, nullptr);
        if (!ok && GetLastError() == ERROR_PIPE_CONNECTED) ok = TRUE; // ffmpeg was faster than us
        if (!ok) {
            std::lock_guard<std::mutex> lk(mu);
            failed = true; // cancelled (ffmpeg never came) or an error
            done.store(true);
            return;
        }
        { std::lock_guard<std::mutex> lk(mu); connected = true; }

        // ---- stream ---------------------------------------------------------------------------------
        std::vector<int16_t> chunk;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return !q.empty() || finishing; });
                if (q.empty()) break; // finishing and everything written
                chunk = std::move(q.front());
                q.pop_front();
                q_samples -= chunk.size();
                while (!q.empty() && chunk.size() < 16384) { // fewer, bigger writes
                    chunk.insert(chunk.end(), q.front().begin(), q.front().end());
                    q_samples -= q.front().size();
                    q.pop_front();
                }
            }
            const char *p = reinterpret_cast<const char *>(chunk.data());
            size_t left = chunk.size() * sizeof(int16_t);
            while (left > 0) {
                DWORD w = 0;
                const DWORD want = (DWORD)(left > 65536 ? 65536 : left);
                if (!WriteFile(pipe, p, want, &w, nullptr) || w == 0) {
                    std::lock_guard<std::mutex> lk(mu);
                    failed = true;
                    q.clear();
                    q_samples = 0;
                    done.store(true);
                    return;
                }
                p += w;
                left -= w;
                frames_out.fetch_add(w / 4, std::memory_order_relaxed);
            }
        }
        // Hand the tail to ffmpeg before the handle goes away: FlushFileBuffers waits until the reader has taken
        // everything (it is interrupted by hr_apipe_finish() if ffmpeg stopped reading).
        FlushFileBuffers(pipe);
        done.store(true);
    }

    // Interrupts a blocked ConnectNamedPipe / WriteFile / FlushFileBuffers of the writer thread.
    void cancel_io() {
        HANDLE t = thread_handle.load();
        if (t && !done.load()) CancelSynchronousIo(t);
    }
    ~PipeSink() {
        HANDLE t = thread_handle.exchange(nullptr);
        if (t) CloseHandle(t);
    }
};

} // namespace

extern "C" {

// Takes ownership of the pipe's server HANDLE. Returns nullptr on failure (the handle is then NOT consumed).
void *hr_apipe_open(intptr_t pipe_handle, int rate_hz) {
    if (pipe_handle == 0 || pipe_handle == -1) return nullptr;
    try {
        PipeSink *s = new PipeSink();
        s->pipe = reinterpret_cast<HANDLE>(pipe_handle);
        s->rate = rate_hz > 0 ? rate_hz : 48000;
        s->th = std::thread([s]() {
            try { s->run(); }
            catch (...) { std::lock_guard<std::mutex> lk(s->mu); s->failed = true; s->done.store(true); }
        });
        return s;
    } catch (...) {
        return nullptr;
    }
}

// Audio sink callback (hr_audio_live_start): `user` is the PipeSink. Never blocks.
void hr_apipe_write(void *user, const int16_t *stereo, size_t frames) {
    PipeSink *s = static_cast<PipeSink *>(user);
    if (!s || !stereo || frames == 0) return;
    try {
        std::vector<int16_t> c(stereo, stereo + frames * 2);
        std::lock_guard<std::mutex> lk(s->mu);
        if (s->failed || s->finishing) return;
        s->frames_in += frames;
        s->q_samples += c.size();
        s->q.push_back(std::move(c));
        while (s->q_samples > kMaxQueuedSamples && !s->q.empty()) {         // the reader is stuck: drop the oldest
            s->q_samples -= s->q.front().size();
            s->frames_dropped += s->q.front().size() / 2;
            s->q.pop_front();
            if (!s->overflow_logged) {
                s->overflow_logged = true;
                HrLog::Warn("Audio pipe: ffmpeg is not reading the audio fast enough - dropping the oldest audio "
                            "(the recording's sound will be cut / out of sync from here).");
            }
        }
        s->cv.notify_one();
    } catch (...) {
    }
}

// No more input: the writer thread sends what is queued, flushes and closes the pipe on its own (ffmpeg then sees EOF
// on its audio input). Never blocks - Stop must call this BEFORE the video side ends, because ffmpeg only opens its
// audio input after it is done with the video input.
void hr_apipe_end_input(void *h) {
    PipeSink *s = static_cast<PipeSink *>(h);
    if (!s) return;
    std::lock_guard<std::mutex> lk(s->mu);
    s->finishing = true;
    s->cv.notify_all();
}

// 1 = everything was delivered; 0 = it failed (ffmpeg gone) or did not finish within timeout_ms.
// No more hr_apipe_write() calls may follow. A reader that is connected but takes nothing for 2 s is treated as
// gone; one that has not connected yet (ffmpeg may still be probing the video input) gets the whole timeout.
int hr_apipe_finish(void *h, int timeout_ms) {
    PipeSink *s = static_cast<PipeSink *>(h);
    if (!s) return 0;
    { std::lock_guard<std::mutex> lk(s->mu); s->finishing = true; s->cv.notify_all(); }
    const auto t0 = std::chrono::steady_clock::now();
    auto t_progress = t0;
    uint64_t last_out = s->frames_out.load(std::memory_order_relaxed);
    bool give_up = false;
    while (!s->done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const auto now = std::chrono::steady_clock::now();
        const long long waited = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
        const uint64_t cur = s->frames_out.load(std::memory_order_relaxed);
        if (cur != last_out) { last_out = cur; t_progress = now; }
        bool conn;
        { std::lock_guard<std::mutex> lk(s->mu); conn = s->connected; }
        const long long stalled = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - t_progress).count();
        if (waited >= timeout_ms || (conn && stalled >= 2000)) {
            HrLog::Warn(std::string("Audio pipe: ffmpeg did not take the last audio (") +
                        (conn ? "no progress for 2 s" : "it never opened the audio input") + ", waited " +
                        std::to_string(waited) + " ms) - cutting it off.");
            give_up = true;
            break;
        }
    }
    if (give_up) {
        { std::lock_guard<std::mutex> lk(s->mu); s->failed = true; }
        for (int i = 0; i < 200 && !s->done.load(); ++i) {      // interrupt the blocked call (repeated: it can race)
            s->cancel_io();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    std::lock_guard<std::mutex> lk(s->mu);
    return (!s->failed && s->done.load()) ? 1 : 0;
}

int hr_apipe_failed(const void *h) {
    PipeSink *s = const_cast<PipeSink *>(static_cast<const PipeSink *>(h));
    if (!s) return 0;
    std::lock_guard<std::mutex> lk(s->mu);
    return s->failed ? 1 : 0;
}

int hr_apipe_connected(const void *h) {
    PipeSink *s = const_cast<PipeSink *>(static_cast<const PipeSink *>(h));
    if (!s) return 0;
    std::lock_guard<std::mutex> lk(s->mu);
    return s->connected ? 1 : 0;
}

long long hr_apipe_frames_in(const void *h) {
    PipeSink *s = const_cast<PipeSink *>(static_cast<const PipeSink *>(h));
    if (!s) return 0;
    std::lock_guard<std::mutex> lk(s->mu);
    return (long long)s->frames_in;
}

// Joins the writer thread (interrupting it when needed), closes the pipe and frees the sink.
void hr_apipe_destroy(void *h) {
    PipeSink *s = static_cast<PipeSink *>(h);
    if (!s) return;
    { std::lock_guard<std::mutex> lk(s->mu); s->finishing = true; s->failed = s->failed || !s->done.load(); s->cv.notify_all(); }
    for (int i = 0; i < 400 && !s->done.load(); ++i) {                      // up to ~2 s
        s->cancel_io();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (s->th.joinable()) {
        if (s->done.load()) s->th.join();
        else s->th.detach();                  // stuck in a driver call: leak the sink rather than hang the app
    }
    if (s->done.load()) {
        if (s->pipe != INVALID_HANDLE_VALUE) CloseHandle(s->pipe);
        delete s;
    }
}

} // extern "C"

#else  // ------------------------------------------------------------------------------------------ non-Windows

extern "C" {
void     *hr_apipe_open(intptr_t, int) { return nullptr; }
void      hr_apipe_write(void *, const int16_t *, size_t) {}
void      hr_apipe_end_input(void *) {}
int       hr_apipe_finish(void *, int) { return 0; }
int       hr_apipe_failed(const void *) { return 1; }
int       hr_apipe_connected(const void *) { return 0; }
long long hr_apipe_frames_in(const void *) { return 0; }
void      hr_apipe_destroy(void *) {}
}

#endif
