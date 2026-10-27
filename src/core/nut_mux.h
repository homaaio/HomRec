// nut_mux.h - the smallest possible NUT (https://ffmpeg.org/~michael/nut.txt) writer: ONE raw
// video stream, every frame a key frame, an explicit presentation time stamp on every frame.
//
// WHY: a plain `-f rawvideo` pipe has no timestamps, so ffmpeg numbers the frames 0,1,2,... at the
// input frame rate. A static screen therefore had to be sent frame after frame (3 MB each at 1080p
// NV12, ~90 MB/s through the pipe at 30 fps) only so the timeline kept its length. With NUT the
// recorder sends a frame only when the picture changed, stamped with the slot on the constant-fps
// timeline it belongs to, and ffmpeg's `fps` filter repeats the previous frame across the gap.
// The output stays constant-frame-rate, so everything downstream (audio mux, retime check, Instant
// Replay segments) is unchanged.
//
// Pure C++11, no platform headers, so it can be unit-tested anywhere (see the report in CHANGELOG).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hrnut {

// CRC-32, polynomial 0x04C11DB7, MSB first, init 0, no final xor (NUT's checksum).
inline uint32_t crc32_nut(const uint8_t *p, size_t n, uint32_t crc = 0)
{
    static uint32_t table[256];
    static bool     built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
            table[i] = c;
        }
        built = true;
    }
    for (size_t i = 0; i < n; ++i) crc = (crc << 8) ^ table[((crc >> 24) ^ p[i]) & 0xFF];
    return crc;
}

// Variable-length unsigned ("v"): big-endian 7-bit groups, high bit set on all but the last.
inline void put_v(std::vector<uint8_t> &o, uint64_t v)
{
    int groups = 1;
    for (uint64_t t = v >> 7; t; t >>= 7) ++groups;
    for (int i = groups - 1; i > 0; --i) o.push_back((uint8_t)(0x80 | ((v >> (7 * i)) & 0x7F)));
    o.push_back((uint8_t)(v & 0x7F));
}
// Signed ("s"): 0, 1, -1, 2, -2 ... mapped onto v.
inline void put_s(std::vector<uint8_t> &o, int64_t v)
{
    put_v(o, v <= 0 ? (uint64_t)(-2 * v) : (uint64_t)(2 * v - 1));
}
inline void put_be64(std::vector<uint8_t> &o, uint64_t v)
{
    for (int i = 7; i >= 0; --i) o.push_back((uint8_t)(v >> (8 * i)));
}
inline void put_be32(std::vector<uint8_t> &o, uint32_t v)
{
    for (int i = 3; i >= 0; --i) o.push_back((uint8_t)(v >> (8 * i)));
}

constexpr uint64_t kMainStartcode   = 0x4E4D7A561F5F04ADull;
constexpr uint64_t kStreamStartcode = 0x4E5311405BF2F9DBull;
constexpr uint64_t kSyncStartcode   = 0x4E4BE4ADEECA4569ull;

constexpr uint32_t kFlagKey      = 1;
constexpr uint32_t kFlagCodedPts = 8;
constexpr uint32_t kFlagSizeMsb  = 32;
constexpr uint32_t kFlagChecksum = 64;
constexpr uint32_t kFlagCoded    = 4096;
constexpr uint32_t kFlagInvalid  = 8192;

constexpr int kMsbPtsShift = 14;     // coded_pts is always sent in its full form: pts + (1 << shift)

// startcode + forward_ptr [+ header checksum] + payload + checksum
inline void put_packet(std::vector<uint8_t> &o, uint64_t startcode, const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> head;
    put_be64(head, startcode);
    put_v(head, payload.size() + 4);
    o.insert(o.end(), head.begin(), head.end());
    if (payload.size() + 4 > 4096) put_be32(o, crc32_nut(head.data(), head.size()));
    o.insert(o.end(), payload.begin(), payload.end());
    put_be32(o, crc32_nut(payload.data(), payload.size()));
}

// File header: id string + main header + stream header. `fourcc` is "NV12" or "I420".
// Time base is 1/fps, so a frame's pts is simply its slot number on the constant-fps timeline.
inline std::vector<uint8_t> make_header(int width, int height, const char fourcc[4], int fps)
{
    std::vector<uint8_t> out;
    static const char id[] = "nut/multimedia container";   // + the terminating NUL
    out.insert(out.end(), id, id + sizeof(id));

    // ---- main header ----
    std::vector<uint8_t> m;
    put_v(m, 3);                        // version
    put_v(m, 1);                        // stream_count
    put_v(m, 65536);                    // max_distance
    put_v(m, 1);                        // time_base_count
    put_v(m, 1);  put_v(m, (uint64_t)(fps > 0 ? fps : 30));   // time_base = 1/fps
    // frame_code table: code 0 is "the" frame code (coded flags + explicit size), the rest invalid.
    put_v(m, kFlagCoded);  put_v(m, 6);
    put_s(m, 0);                        // pts_delta
    put_v(m, 1);                        // size_mul   (size = size_lsb(0) + 1 * data_size_msb)
    put_v(m, 0);                        // stream_id
    put_v(m, 0);                        // size_lsb
    put_v(m, 0);                        // reserved_count
    put_v(m, 1);                        // count: just code 0
    put_v(m, kFlagInvalid); put_v(m, 6);
    put_s(m, 0); put_v(m, 1); put_v(m, 0); put_v(m, 0); put_v(m, 0);
    put_v(m, 254);                      // codes 1..255 except 'N' (which is invalid by definition)
    put_v(m, 0);                        // header_count - 1
    put_packet(out, kMainStartcode, m);

    // ---- stream header ----
    std::vector<uint8_t> s;
    put_v(s, 0);                        // stream_id
    put_v(s, 0);                        // stream_class: video
    put_v(s, 4); s.insert(s.end(), fourcc, fourcc + 4);
    put_v(s, 0);                        // time_base_id
    put_v(s, kMsbPtsShift);
    put_v(s, 1u << 20);                 // max_pts_distance (only used by checksum-less frames; we always checksum)
    put_v(s, 0);                        // decode_delay
    put_v(s, 0);                        // stream_flags
    put_v(s, 0);                        // codec_specific_data length
    put_v(s, (uint64_t)width);
    put_v(s, (uint64_t)height);
    put_v(s, 1);                        // sample_width  (square pixels)
    put_v(s, 1);                        // sample_height
    put_v(s, 0);                        // colorspace_type: unspecified
    put_packet(out, kStreamStartcode, s);
    return out;
}

// Everything that goes on the wire BEFORE a frame's payload bytes: a sync point plus the frame
// header (payload_size bytes of raw pixels follow it, nothing after them). `pts` in 1/fps slots.
inline void make_frame_prefix(std::vector<uint8_t> &out, int64_t pts, size_t payload_size)
{
    out.clear();
    std::vector<uint8_t> sp;
    put_v(sp, (uint64_t)pts);           // global_key_pts = pts * time_base_count + time_base_id (count = 1)
    put_v(sp, 0);                       // back_ptr_div16: not used when reading from a pipe
    put_packet(out, kSyncStartcode, sp);

    const size_t start = out.size();
    out.push_back(0);                   // frame_code 0
    const uint32_t final_flags = kFlagKey | kFlagCodedPts | kFlagSizeMsb | kFlagChecksum | kFlagCoded;
    put_v(out, final_flags ^ kFlagCoded);                       // coded_flags (table flags = kFlagCoded)
    put_v(out, (uint64_t)pts + ((uint64_t)1 << kMsbPtsShift));  // coded_pts, full form
    put_v(out, (uint64_t)payload_size);                         // data_size_msb
    put_be32(out, crc32_nut(out.data() + start, out.size() - start));
}

} // namespace hrnut
