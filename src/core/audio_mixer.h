// audio_mixer.h - real-time mixer for the in-process audio path.
//
// WHAT: the capture threads (mic, system loopback, the extra Audio Mixer channels) each hand over
// interleaved 16-bit stereo at THEIR device's rate. This turns them into ONE stereo stream at one
// output rate while the recording is running, so no per-source WAV files and no mix pass after Stop
// are needed. The result goes to an "audio sink": the Media Foundation sink writer (AAC, in process) or
// a named pipe that ffmpeg reads as a second input and muxes live.
//
// Contents
//   StereoResampler  streaming linear resampler (state is kept between calls, so the result does not
//                    depend on how the input was chopped into blocks).
//   LiveMixer        any number of "push" sources (a capture thread feeds them) plus "pull" sources
//                    (an audio file that is read on demand), summed with clipping.
//
// Alignment rule: every push source already keeps itself on the wall clock (WasapiStream::read() pads
// silence / trims surplus), so equal amounts of audio are equal amounts of time. mix() therefore emits as
// many frames as the SLOWEST push source has (the sources are consumed in lock-step). A source that falls
// more than `max_lag` behind the fastest one is not waited for: it is padded with silence and remembers
// the silence it owes ("debt"), so when its data finally arrives the stale part is dropped instead of
// staying shifted for the rest of the recording.
//
// Pure C++11, no platform headers: unit tests live in scripts/tests/test_audio_mixer.cpp.
// Not thread-safe: one thread owns a LiveMixer.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hrlive {

// ---------------------------------------------------------------------------------------------------
// Streaming linear resampler, interleaved stereo int16.
// ---------------------------------------------------------------------------------------------------
class StereoResampler {
public:
    void init(uint32_t in_rate, uint32_t out_rate) {
        in_rate_  = in_rate  ? in_rate  : 48000;
        out_rate_ = out_rate ? out_rate : 48000;
        passthrough_ = (in_rate_ == out_rate_);
        step_ = ((uint64_t)in_rate_ << 32) / out_rate_;   // input frames per output frame, Q32
        phase_ = 0;
        have_prev_ = false;
        prev_[0] = prev_[1] = 0;
    }
    bool passthrough() const { return passthrough_; }

    // Appends the converted frames to `out`. Every input frame is consumed.
    void process(const int16_t *in, size_t in_frames, std::vector<int16_t> &out) {
        if (!in || in_frames == 0) return;
        if (passthrough_) {
            out.insert(out.end(), in, in + in_frames * 2);
            return;
        }
        const uint64_t ONE = (uint64_t)1 << 32;
        size_t i = 0;
        if (!have_prev_) {                 // the very first frame is output frame 0 (position 0.0)
            prev_[0] = in[0];
            prev_[1] = in[1];
            have_prev_ = true;
            phase_ = 0;
            i = 1;
        }
        out.reserve(out.size() + (size_t)((uint64_t)in_frames * out_rate_ / in_rate_ + 4) * 2);
        for (; i < in_frames; ++i) {
            const int32_t c0 = in[2 * i], c1 = in[2 * i + 1];
            // `phase_` = position of the next output frame between prev (0.0) and cur (1.0).
            while (phase_ < ONE) {
                const int64_t f = (int64_t)(phase_ >> 16);        // 0..65535
                out.push_back((int16_t)(prev_[0] + (int32_t)(((int64_t)(c0 - prev_[0]) * f) >> 16)));
                out.push_back((int16_t)(prev_[1] + (int32_t)(((int64_t)(c1 - prev_[1]) * f) >> 16)));
                phase_ += step_;
            }
            phase_ -= ONE;
            prev_[0] = c0;
            prev_[1] = c1;
        }
    }

private:
    uint32_t in_rate_ = 48000, out_rate_ = 48000;
    bool     passthrough_ = true;
    uint64_t step_ = (uint64_t)1 << 32;
    uint64_t phase_ = 0;
    bool     have_prev_ = false;
    int32_t  prev_[2] = {0, 0};
};

// ---------------------------------------------------------------------------------------------------
// Mixer
// ---------------------------------------------------------------------------------------------------
class LiveMixer {
public:
    // Pull source: ADD `frames` interleaved stereo frames (at the mixer's output rate) to `acc`
    // (acc has frames * 2 int32 values, already holding the other sources). Called from mix() only.
    typedef void (*PullFn)(void *ctx, int32_t *acc, size_t frames);

    // max_lag_frames 0 = a quarter of a second.
    void init(uint32_t out_rate, size_t max_lag_frames = 0) {
        out_rate_ = out_rate ? out_rate : 48000;
        max_lag_ = max_lag_frames ? max_lag_frames : out_rate_ / 4;
        push_.clear();
        pull_.clear();
        head_ = 0;
        head_done_ = false;
        skip_ = 0;
        frames_out_ = 0;
        frames_padded_ = 0;
        frames_dropped_ = 0;
    }
    uint32_t out_rate() const { return out_rate_; }

    // Returns the source index to use with push().
    int add_push(uint32_t in_rate) {
        PushSrc s;
        s.rs.init(in_rate, out_rate_);
        push_.push_back(std::move(s));
        return (int)push_.size() - 1;
    }
    int add_pull(PullFn fn, void *ctx) {
        PullSrc p;
        p.fn = fn;
        p.ctx = ctx;
        pull_.push_back(p);
        return (int)pull_.size() - 1;
    }
    size_t push_sources() const { return push_.size(); }

    // >0: that many frames of silence come first (the audio started LATER than the video).
    // <0: the first -head frames of audio are thrown away (the audio started EARLIER).
    // Clamped to +-5 s. Set before the first mix().
    void set_head(int64_t frames) {
        const int64_t lim = (int64_t)out_rate_ * 5;
        if (frames > lim) frames = lim;
        if (frames < -lim) frames = -lim;
        if (frames >= 0) { head_ = (uint64_t)frames; skip_ = 0; }
        else             { head_ = 0; skip_ = (uint64_t)(-frames); }
    }

    // Feeds `samples` interleaved int16 (an even number) recorded at the source's own rate.
    void push(int src, const int16_t *interleaved, size_t samples) {
        if (src < 0 || (size_t)src >= push_.size() || !interleaved || samples < 2) return;
        PushSrc &s = push_[(size_t)src];
        s.tmp.clear();
        s.rs.process(interleaved, samples / 2, s.tmp);
        size_t n = s.tmp.size() / 2;
        const int16_t *p = s.tmp.data();
        if (s.debt > 0) {                                   // silence was already emitted for this stretch
            const size_t d = (size_t)std::min<uint64_t>(s.debt, n);
            p += d * 2;
            n -= d;
            s.debt -= d;
            frames_dropped_ += d;
        }
        if (n) s.fifo.insert(s.fifo.end(), p, p + n * 2);
    }

    // Appends the next mixed frames to `out`; returns how many were appended.
    // final_flush: emit everything that is buffered (padding the slower sources with silence).
    size_t mix(std::vector<int16_t> &out, bool final_flush) {
        if (push_.empty()) return 0;
        const size_t before = out.size();

        if (!head_done_) {
            head_done_ = true;
            if (head_) {
                out.resize(out.size() + (size_t)head_ * 2, 0);
                frames_out_ += head_;
            }
        }

        size_t nmin = (size_t)-1, nmax = 0;
        for (const PushSrc &s : push_) {
            const size_t a = s.avail();
            nmin = std::min(nmin, a);
            nmax = std::max(nmax, a);
        }
        size_t n = nmin;
        if (final_flush) n = nmax;
        else if (nmax > max_lag_ && nmax - max_lag_ > n) n = nmax - max_lag_;   // do not wait for a stalled source
        if (n == 0) return (out.size() - before) / 2;

        acc_.assign(n * 2, 0);
        for (PushSrc &s : push_) {
            const size_t a = s.avail();
            const size_t take = std::min(a, n);
            const int16_t *p = s.fifo.data() + s.rd;
            for (size_t i = 0; i < take * 2; ++i) acc_[i] += p[i];
            s.rd += take * 2;
            if (take < n) {                                  // owed silence
                frames_padded_ += (n - take);
                s.debt = std::min<uint64_t>(s.debt + (n - take), (uint64_t)out_rate_ * 10);   // a dead source must not eat 10+ s of a revived one
            }
            s.compact();
        }
        for (PullSrc &p : pull_)
            if (p.fn) p.fn(p.ctx, acc_.data(), n);

        size_t first = 0;
        if (skip_) {                                         // audio that started before the video
            first = (size_t)std::min<uint64_t>(skip_, n);
            skip_ -= first;
        }
        const size_t emit = n - first;
        const size_t base = out.size();
        out.resize(base + emit * 2);
        for (size_t i = 0; i < emit * 2; ++i) {
            int32_t v = acc_[first * 2 + i];
            if (v > 32767) v = 32767;
            else if (v < -32768) v = -32768;
            out[base + i] = (int16_t)v;
        }
        frames_out_ += emit;
        return (out.size() - before) / 2;
    }

    uint64_t frames_out() const { return frames_out_; }
    uint64_t frames_padded() const { return frames_padded_; }    // silence inserted for a stalled source
    uint64_t frames_dropped() const { return frames_dropped_; }  // late data dropped to pay that back

    // Frames the fastest push source is holding (diagnostics / tests).
    size_t backlog_frames() const {
        size_t m = 0;
        for (const PushSrc &s : push_) m = std::max(m, s.avail());
        return m;
    }

private:
    struct PushSrc {
        StereoResampler rs;
        std::vector<int16_t> fifo;     // interleaved stereo at the output rate; read position `rd`
        size_t rd = 0;                 // int16 index
        std::vector<int16_t> tmp;
        uint64_t debt = 0;             // frames of silence already emitted on this source's behalf
        size_t avail() const { return (fifo.size() - rd) / 2; }
        void compact() {
            if (rd == fifo.size()) { fifo.clear(); rd = 0; }
            else if (rd >= 16384 && rd * 2 >= fifo.size()) {
                fifo.erase(fifo.begin(), fifo.begin() + (ptrdiff_t)rd);
                rd = 0;
            }
        }
    };
    struct PullSrc {
        PullFn fn = nullptr;
        void *ctx = nullptr;
    };

    uint32_t out_rate_ = 48000;
    size_t   max_lag_ = 12000;
    std::vector<PushSrc> push_;
    std::vector<PullSrc> pull_;
    std::vector<int32_t> acc_;
    uint64_t head_ = 0, skip_ = 0;
    bool     head_done_ = false;
    uint64_t frames_out_ = 0, frames_padded_ = 0, frames_dropped_ = 0;
};

} // namespace hrlive
