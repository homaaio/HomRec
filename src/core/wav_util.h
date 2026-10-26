// wav_util.h - header-only helpers for the plain 44-byte-header WAV files HomRec writes itself
// (temp mic / system / mixed audio tracks). No Windows headers, so scripts/tests can build it.
//
// Why this exists: a classic RIFF/WAV stores its length in 32 bits, i.e. at most 4 GiB. 16-bit stereo
// at 48 kHz is 192 KB/s, so a track overflows after ~6.2 h (6.8 h at 44.1 kHz). The old writer stored
// (uint32_t)bytes, which silently wrapped, and the readers trusted that value (the file-length
// fallback used a 32-bit ftell() that returns -1 above 2 GB).
#pragma once

#include <cstdint>
#include <cstdio>
#ifndef _WIN32
#include <sys/types.h>
#endif

namespace hrwav {

constexpr uint64_t kHeaderBytes = 44;
constexpr uint32_t kUnknownSize = 0xFFFFFFFFu;   // "read to end of file" - what ffmpeg itself writes for streams

// Largest data length a classic RIFF header can describe (chunk_size = 36 + data must fit in 32 bits).
constexpr uint64_t kMaxClassicData = 0xFFFFFFFFull - 36;

// Values for the two size fields of the header. Above the classic limit both become 0xFFFFFFFF.
inline void SizeFields(uint64_t data_bytes, uint32_t &riff_chunk_size, uint32_t &data_chunk_size) {
    if (data_bytes > kMaxClassicData) {
        riff_chunk_size = kUnknownSize;
        data_chunk_size = kUnknownSize;
    } else {
        data_chunk_size = (uint32_t)data_bytes;
        riff_chunk_size = (uint32_t)(36 + data_bytes);
    }
}

// Real length of the audio data. `header_size` is the data-chunk size field, `bytes_after_header` is
// (file size - 44). HomRec's own files are header + data up to end of file, so the file length is the
// truth whenever the header can't be trusted: 0 (crash before the header was patched), 0xFFFFFFFF
// (unknown), larger than the file, or - for files >= 4 GiB - a value that wrapped around.
inline uint64_t EffectiveDataBytes(uint32_t header_size, uint64_t bytes_after_header) {
    if (bytes_after_header > kMaxClassicData) return bytes_after_header;
    if (header_size == 0 || header_size == kUnknownSize || header_size > bytes_after_header)
        return bytes_after_header;
    return header_size;
}

// 64-bit file length. The CRT's ftell()/fseek() take a 32-bit `long` on Windows and fail above 2 GB.
inline bool FileSize64(FILE *f, uint64_t &out) {
    if (!f) return false;
#ifdef _WIN32
    const __int64 here = _ftelli64(f);
    if (here < 0 || _fseeki64(f, 0, SEEK_END) != 0) return false;
    const __int64 end = _ftelli64(f);
    if (_fseeki64(f, here, SEEK_SET) != 0 || end < 0) return false;
#else
    const long long here = (long long)ftello(f);
    if (here < 0 || fseeko(f, 0, SEEK_END) != 0) return false;
    const long long end = (long long)ftello(f);
    if (fseeko(f, (off_t)here, SEEK_SET) != 0 || end < 0) return false;
#endif
    out = (uint64_t)end;
    return true;
}

} // namespace hrwav
