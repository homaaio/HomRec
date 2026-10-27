// replay_ring.h - Instant Replay buffer in RAM. Portable, header-only, no Windows code.
//
// ffmpeg writes the replay encoder's output as an MPEG-TS stream to its stdout (`-f mpegts pipe:1`); a reader
// thread feeds every byte it gets into ReplayRing::push(). The ring cuts the stream at video KEYFRAMES (the
// random-access flag ffmpeg's mpegts muxer sets on the first packet of every key picture) into GOP chunks and
// keeps just enough of the newest chunks to cover the requested number of seconds - this replaces the old
// %TEMP%\HomRec_replay\seg_NNN.mp4 files that were written, wrapped and deleted all the time.
//
// MPEG-TS can be cut and joined at packet boundaries, so "save the replay" is: concatenate the kept chunks
// (snapshot()), write them once, `ffmpeg -c copy` them into the output container.
//
// What it understands of the stream (and nothing more): 188-byte packets (re-syncs on garbage), PAT -> PMT ->
// the first video elementary stream, the random-access flag, the PES PTS of video packets (33-bit, wrap-safe).
// Thread-safe: push() (reader thread) and snapshot()/stats() (any thread) share one mutex; finished chunks are
// immutable shared buffers, so a snapshot does not copy them and never blocks the reader while a file is written.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

class ReplayRing {
public:
    using Chunk = std::shared_ptr<const std::vector<uint8_t>>;

    struct Stats {
        size_t       bytes = 0;          // bytes held (finished chunks + the open one)
        size_t       chunks = 0;         // finished chunks (the open one is not counted)
        double       seconds = 0.0;      // length of what a snapshot() would contain right now
        bool         have_video = false; // PAT/PMT seen and a keyframe found
        bool         capped = false;     // the memory cap already forced chunks out before the time window was full
        unsigned long long resyncs = 0;  // bytes skipped while looking for a packet boundary
    };

    // window_sec: how much the saved replay must cover at least (the newest chunks are kept until the second
    // oldest one alone covers it, so the kept length is window_sec .. window_sec + one GOP).
    // max_bytes : hard memory cap; the oldest chunks go first when it is hit (0 = no cap).
    explicit ReplayRing(double window_sec = 30.0, size_t max_bytes = 0) { set_limits(window_sec, max_bytes); }

    void set_limits(double window_sec, size_t max_bytes) {
        std::lock_guard<std::mutex> lk(mu_);
        window_ticks_ = (int64_t)((window_sec > 1.0 ? window_sec : 1.0) * 90000.0);
        max_bytes_ = max_bytes;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        chunks_.clear();
        cur_.clear();
        cur_start_pts_ = 0;
        cur_open_ = false;
        done_bytes_ = 0;
        carry_.clear();
        newest_pts_ = 0;
        last_pes_off_ = 0;
        last_frame_dur_ = 0;
        have_pts_ = false;
        wraps_ = 0;
        capped_ = false;
        // PAT / PMT / pids are kept: they are re-learned from the stream anyway, and a restarted encoder
        // calls clear() before its first byte arrives.
        pat_pkt_.clear();
        pmt_pkt_.clear();
        pmt_pid_ = -1;
        video_pid_ = -1;
    }

    // Feed any amount of stream bytes (need not be packet aligned).
    void push(const uint8_t *data, size_t n) {
        if (!data || !n) return;
        std::lock_guard<std::mutex> lk(mu_);
        size_t i = 0;
        if (!carry_.empty()) {                     // finish a packet split across two push() calls (carry_ starts with 0x47)
            const size_t need = kPkt - carry_.size();
            const size_t take = n < need ? n : need;
            carry_.insert(carry_.end(), data, data + take);
            i = take;
            if (carry_.size() < kPkt) return;
            packet(carry_.data());
            carry_.clear();
        }
        while (i < n) {
            if (data[i] != 0x47 || !looks_synced(data, n, i)) { ++resyncs_; ++i; continue; }
            if (n - i < kPkt) { carry_.assign(data + i, data + n); return; }
            packet(data + i);
            i += kPkt;
        }
    }

    // Everything a saved replay consists of: the kept chunks plus the open one up to (not including) the
    // picture that is still being written. `final_flush` = the encoder has ended, the open chunk is complete.
    // Returns the pieces (write them in order) and the length in seconds; empty when nothing usable is held.
    std::vector<Chunk> snapshot(double *seconds_out = nullptr, bool final_flush = false) const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<Chunk> out;
        out.reserve(chunks_.size() + 1);
        for (const auto &c : chunks_) out.push_back(c.data);
        if (cur_open_) {
            size_t upto = final_flush ? cur_.size() : last_pes_off_;
            if (upto > cur_.size()) upto = cur_.size();
            if (upto > 0) out.push_back(std::make_shared<const std::vector<uint8_t>>(cur_.begin(), cur_.begin() + upto));
        }
        if (seconds_out) *seconds_out = seconds_locked();
        return out;
    }

    Stats stats() const {
        std::lock_guard<std::mutex> lk(mu_);
        Stats s;
        s.bytes = done_bytes_ + cur_.size();
        s.chunks = chunks_.size();
        s.seconds = seconds_locked();
        s.have_video = cur_open_ || !chunks_.empty();
        s.capped = capped_;
        s.resyncs = resyncs_;
        return s;
    }

    static constexpr size_t kPkt = 188;

private:
    struct Entry { Chunk data; int64_t start_pts; };

    // Is data[i] really a packet start? When the next sync bytes are visible they must be 0x47 too.
    static bool looks_synced(const uint8_t *d, size_t n, size_t i) {
        if (i + kPkt < n && d[i + kPkt] != 0x47) return false;
        if (i + 2 * kPkt < n && d[i + 2 * kPkt] != 0x47) return false;
        return true;
    }

    double seconds_locked() const {
        if (!have_pts_) return 0.0;
        int64_t first = chunks_.empty() ? cur_start_pts_ : chunks_.front().start_pts;
        if (chunks_.empty() && !cur_open_) return 0.0;
        const int64_t span = newest_pts_ - first + last_frame_dur_;   // the newest picture lasts one frame
        return span > 0 ? (double)span / 90000.0 : 0.0;
    }

    void packet(const uint8_t *p) {
        const int pid = ((p[1] & 0x1f) << 8) | p[2];
        const bool pusi = (p[1] & 0x40) != 0;
        const int afc = (p[3] >> 4) & 3;
        const uint8_t *pay = nullptr;
        size_t pay_len = 0;
        {
            size_t off = 4;
            if (afc & 2) off += 1 + p[4];
            if ((afc & 1) && off < kPkt) { pay = p + off; pay_len = kPkt - off; }
        }

        if (pid == 0 && pusi && pay && pay_len > 8) {
            // PAT: pointer_field, table_id 0, then program entries (program_number, pid) after the 8-byte header.
            const size_t ptr = pay[0];
            if (1 + ptr + 8 <= pay_len && pay[1 + ptr] == 0x00) {
                const uint8_t *t = pay + 1 + ptr;
                const size_t sec_len = (size_t)(((t[1] & 0x0f) << 8) | t[2]);
                size_t end = 3 + sec_len;                     // up to and including the CRC
                if (1 + ptr + end > pay_len) end = pay_len - 1 - ptr;
                for (size_t q = 8; end >= 4 && q + 4 <= end - 4; q += 4) {   // entries run up to the 4-byte CRC
                    const int prog = (t[q] << 8) | t[q + 1];
                    if (prog != 0) {
                        pmt_pid_ = ((t[q + 2] & 0x1f) << 8) | t[q + 3];
                        pat_pkt_.assign(p, p + kPkt);
                        break;
                    }
                }
            }
        } else if (pmt_pid_ >= 0 && pid == pmt_pid_ && pusi && pay && pay_len > 12) {
            const size_t ptr = pay[0];
            if (1 + ptr + 12 <= pay_len && pay[1 + ptr] == 0x02) {
                const uint8_t *t = pay + 1 + ptr;
                const size_t sec_len = (size_t)(((t[1] & 0x0f) << 8) | t[2]);
                size_t end = 3 + sec_len;
                if (1 + ptr + end > pay_len) end = pay_len - 1 - ptr;
                const size_t pinfo = (size_t)(((t[10] & 0x0f) << 8) | t[11]);
                size_t q = 12 + pinfo;
                while (q + 5 <= end - 4) {
                    const int st = t[q];
                    const int epid = ((t[q + 1] & 0x1f) << 8) | t[q + 2];
                    const size_t es_info = (size_t)(((t[q + 3] & 0x0f) << 8) | t[q + 4]);
                    if (video_pid_ < 0 && (st == 0x1b || st == 0x24 || st == 0x02 || st == 0x10 || st == 0x01)) video_pid_ = epid;
                    q += 5 + es_info;
                }
                pmt_pkt_.assign(p, p + kPkt);
            }
        }

        const bool is_video = video_pid_ >= 0 && pid == video_pid_;
        bool new_key = false;
        if (is_video && pusi) {
            // A new picture starts here: remember where it begins (a snapshot of the open chunk stops before it),
            // its PTS, and whether it is a random-access (key) picture.
            const bool key = (afc & 2) && p[4] > 0 && (p[5] & 0x40);
            int64_t pts = 0;
            bool has_pts = false;
            if (pay && pay_len >= 14 && pay[0] == 0 && pay[1] == 0 && pay[2] == 1 && (pay[7] & 0x80)) {
                const uint8_t *b = pay + 9;
                const uint64_t v = ((uint64_t)((b[0] >> 1) & 0x07) << 30) | ((uint64_t)b[1] << 22) |
                                   ((uint64_t)(b[2] >> 1) << 15) | ((uint64_t)b[3] << 7) | (uint64_t)(b[4] >> 1);
                pts = unwrap(v);
                has_pts = true;
            }
            if (has_pts) {
                if (have_pts_ && pts > newest_pts_ && pts - newest_pts_ < 90000) last_frame_dur_ = pts - newest_pts_;
                if (!have_pts_ || pts > newest_pts_) newest_pts_ = pts;   // B-frames: PTS is not monotone, keep the max
                have_pts_ = true;
            }
            if (key && has_pts && !pat_pkt_.empty() && !pmt_pkt_.empty()) new_key = true;
            if (new_key) {
                close_chunk();
                cur_.reserve(256 * 1024);
                cur_.insert(cur_.end(), pat_pkt_.begin(), pat_pkt_.end());   // every chunk starts decodable
                cur_.insert(cur_.end(), pmt_pkt_.begin(), pmt_pkt_.end());
                cur_start_pts_ = pts;
                cur_open_ = true;
            }
            if (cur_open_) last_pes_off_ = cur_.size();
        }
        if (!cur_open_) return;                    // before the first keyframe nothing is worth keeping
        cur_.insert(cur_.end(), p, p + kPkt);
        if (is_video && pusi) trim();
    }

    void close_chunk() {
        if (!cur_open_) return;
        if (!cur_.empty()) {
            done_bytes_ += cur_.size();
            Entry e;
            e.data = std::make_shared<const std::vector<uint8_t>>(std::move(cur_));
            e.start_pts = cur_start_pts_;
            chunks_.push_back(std::move(e));
        }
        cur_.clear();
        cur_open_ = false;
        last_pes_off_ = 0;
    }

    // Drop the oldest chunk while the one after it already covers the window, and while the memory cap is hit.
    void trim() {
        for (;;) {
            if (chunks_.empty()) return;
            const int64_t next_start = chunks_.size() >= 2 ? chunks_[1].start_pts : cur_start_pts_;
            const bool have_next = chunks_.size() >= 2 || cur_open_;
            const bool time_ok = have_next && (newest_pts_ - next_start) >= window_ticks_;
            const bool over_cap = max_bytes_ > 0 && (done_bytes_ + cur_.size()) > max_bytes_ && have_next;
            if (!time_ok && !over_cap) return;
            if (over_cap && !time_ok) capped_ = true;
            done_bytes_ -= chunks_.front().data->size();
            chunks_.pop_front();
        }
    }

    int64_t unwrap(uint64_t v33) {
        constexpr int64_t kWrap = (int64_t)1 << 33;
        int64_t v = (int64_t)v33 + wraps_ * kWrap;
        if (have_pts_ && v < newest_pts_ - kWrap / 2) { ++wraps_; v += kWrap; }
        return v;
    }

    mutable std::mutex mu_;
    std::deque<Entry>  chunks_;
    std::vector<uint8_t> cur_;
    int64_t   cur_start_pts_ = 0;
    bool      cur_open_ = false;
    size_t    last_pes_off_ = 0;         // offset in cur_ where the newest video picture starts
    size_t    done_bytes_ = 0;
    std::vector<uint8_t> carry_;         // incomplete packet from the previous push()
    std::vector<uint8_t> pat_pkt_, pmt_pkt_;
    int       pmt_pid_ = -1, video_pid_ = -1;
    int64_t   newest_pts_ = 0, last_frame_dur_ = 0, wraps_ = 0;
    bool      have_pts_ = false;
    int64_t   window_ticks_ = 30 * 90000;
    size_t    max_bytes_ = 0;
    bool      capped_ = false;
    unsigned long long resyncs_ = 0;
};
