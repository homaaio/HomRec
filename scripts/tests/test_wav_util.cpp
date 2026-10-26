// scripts/tests/test_wav_util.cpp - Standalone unit tests for src/core/wav_util.h (WAV length handling
// for tracks over 4 GiB). Build: g++ -std=c++17 -Isrc -Iscripts/tests scripts/tests/test_wav_util.cpp

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/wav_util.h"

#include <cstdio>
#include <string>

using namespace hrwav;

TEST_CASE("size fields: normal length is stored exactly") {
    uint32_t riff = 0, data = 0;
    SizeFields(1000, riff, data);
    CHECK(data == 1000);
    CHECK(riff == 1036);
}

TEST_CASE("size fields: past the 4 GiB limit they become 'unknown' instead of wrapping") {
    uint32_t riff = 0, data = 0;
    SizeFields(kMaxClassicData, riff, data);          // exactly the largest classic file
    CHECK(data == (uint32_t)kMaxClassicData);
    CHECK(riff == 0xFFFFFFFFu);
    SizeFields(kMaxClassicData + 1, riff, data);
    CHECK(data == kUnknownSize);
    CHECK(riff == kUnknownSize);
    // 11.5 h of 48 kHz 16-bit stereo - the recording that was lost
    const uint64_t eleven_half_hours = 192000ull * 41600ull;
    SizeFields(eleven_half_hours, riff, data);
    CHECK(data == kUnknownSize);
}

TEST_CASE("effective length: trusted header, untrusted header") {
    CHECK(EffectiveDataBytes(1000, 1000) == 1000);               // normal
    CHECK(EffectiveDataBytes(1000, 1200) == 1000);               // trailing bytes are ignored
    CHECK(EffectiveDataBytes(0, 5000) == 5000);                  // header never patched (crash)
    CHECK(EffectiveDataBytes(kUnknownSize, 5000) == 5000);       // 'unknown'
    CHECK(EffectiveDataBytes(9000, 5000) == 5000);               // claims more than the file has
}

TEST_CASE("effective length: a wrapped 32-bit header on a file over 4 GiB is ignored") {
    const uint64_t real = 192000ull * 41600ull;                  // ~8.0e9 bytes
    const uint32_t wrapped = (uint32_t)real;                     // what the old writer stored
    REQUIRE(wrapped < real);
    CHECK(EffectiveDataBytes(wrapped, real) == real);
    CHECK(EffectiveDataBytes(kUnknownSize, real) == real);
    CHECK(EffectiveDataBytes(0, real) == real);
}

#ifndef _WIN32
// A sparse 5 GiB file: reading its size must not go through a 32-bit long.
TEST_CASE("FileSize64 handles files over 4 GiB") {
    const std::string path = "wav_util_sparse_test.bin";
    FILE *f = fopen(path.c_str(), "wb");
    REQUIRE(f);
    const long long target = 5LL * 1024 * 1024 * 1024 + 44;
    REQUIRE(fseeko(f, (off_t)(target - 1), SEEK_SET) == 0);
    fputc(0, f);
    fclose(f);

    f = fopen(path.c_str(), "rb");
    REQUIRE(f);
    fseek(f, 10, SEEK_SET);
    uint64_t sz = 0;
    CHECK(FileSize64(f, sz));
    CHECK(sz == (uint64_t)target);
    CHECK(ftell(f) == 10);                                        // position restored
    fclose(f);
    remove(path.c_str());
}
#endif
