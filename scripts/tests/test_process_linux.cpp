// scripts/tests/test_process_linux.cpp - IChildProcess on Linux (src/platform/linux/process_linux.cpp).
//
// Build: scripts/linux/test.sh, or by hand:
//   g++ -std=c++17 -I src -I scripts/tests scripts/tests/test_process_linux.cpp src/platform/linux/process_linux.cpp -o t && ./t
//
// The ffmpeg case is skipped when ffmpeg / ffprobe are not on PATH.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "platform/process_backend.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace hr::platform;

namespace {

std::string ReadAll(IChildProcess &p) {
    std::string out;
    char buf[4096];
    long n;
    while ((n = p.ReadStdout(buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    return out;
}

bool OnPath(const char *name) {
    const std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::string TmpPath(const char *name) {
    return std::string("/tmp/hr_test_process_") + std::to_string((long)getpid()) + "_" + name;
}

}  // namespace

TEST_CASE("stdin -> cat -> stdout round trip") {
    auto p = CreateChildProcess();
    SpawnSpec s;
    s.exe = "cat";
    s.pipe_stdin = s.pipe_stdout = true;
    std::string err;
    REQUIRE(p->Start(s, &err));
    CHECK(p->IsRunning());

    const std::string msg(200000, 'x');   // bigger than a pipe buffer: reader must drain while we write
    std::string got;
    // cat echoes as it reads, so write in chunks and drain in between.
    for (size_t off = 0; off < msg.size(); off += 30000) {
        const size_t n = std::min<size_t>(30000, msg.size() - off);
        REQUIRE(p->WriteStdin(msg.data() + off, n) == (long)n);
        char buf[30000];
        size_t want = n;
        while (want > 0) {
            const long r = p->ReadStdout(buf, std::min(want, sizeof(buf)));
            REQUIRE(r > 0);
            got.append(buf, (size_t)r);
            want -= (size_t)r;
        }
    }
    p->CloseStdin();
    CHECK(ReadAll(*p).empty());   // end of stream
    CHECK(p->Wait(2000));
    CHECK(p->ExitCode() == 0);
    CHECK(got == msg);
    CHECK_FALSE(p->IsRunning());
}

TEST_CASE("exit code and arguments are passed as-is") {
    auto p = CreateChildProcess();
    SpawnSpec s;
    s.exe = "sh";
    s.args = {"-c", "printf '%s|%s' \"$0\" \"$1\"; exit 7", "a b", "c;d"};   // no shell quoting on our side
    s.pipe_stdout = true;
    REQUIRE(p->Start(s, nullptr));
    CHECK(ReadAll(*p) == "a b|c;d");
    CHECK(p->Wait(2000));
    CHECK(p->ExitCode() == 7);
}

TEST_CASE("a missing executable fails in Start with a reason") {
    auto p = CreateChildProcess();
    SpawnSpec s;
    s.exe = "/nonexistent/hr-no-such-binary";
    std::string err;
    CHECK_FALSE(p->Start(s, &err));
    CHECK(err.find("No such file") != std::string::npos);
    CHECK_FALSE(p->IsRunning());
}

TEST_CASE("working_dir and stderr_path (appended)") {
    const std::string log = TmpPath("stderr.txt");
    std::remove(log.c_str());
    for (int i = 0; i < 2; ++i) {
        auto p = CreateChildProcess();
        SpawnSpec s;
        s.exe = "sh";
        s.args = {"-c", "pwd >&2"};
        s.working_dir = "/tmp";
        s.stderr_path = log;
        REQUIRE(p->Start(s, nullptr));
        REQUIRE(p->Wait(2000));
    }
    FILE *f = std::fopen(log.c_str(), "rb");
    REQUIRE(f);
    char buf[64] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::remove(log.c_str());
    CHECK(std::string(buf, n) == "/tmp\n/tmp\n");
}

TEST_CASE("Wait times out, RequestStop ends the child, Kill is the last resort") {
    {
        auto p = CreateChildProcess();
        SpawnSpec s;
        s.exe = "sleep";
        s.args = {"30"};
        REQUIRE(p->Start(s, nullptr));
        CHECK_FALSE(p->Wait(100));
        CHECK(p->IsRunning());
        p->RequestStop();
        CHECK(p->Wait(2000));
        CHECK(p->ExitCode() == 128 + 2);   // SIGINT
    }
    {
        auto p = CreateChildProcess();
        SpawnSpec s;
        s.exe = "sh";
        s.args = {"-c", "trap '' INT; echo ready; exec sleep 30"};   // ignores the polite request
        s.pipe_stdout = true;
        REQUIRE(p->Start(s, nullptr));
        char ready[8];
        REQUIRE(p->ReadStdout(ready, sizeof(ready)) > 0);   // the trap is in place from here on
        p->RequestStop();
        CHECK_FALSE(p->Wait(300));
        p->Kill();
        CHECK(p->Wait(0));
        CHECK(p->ExitCode() == 128 + 9);   // SIGKILL
    }
}

TEST_CASE("writing to a child that has exited returns -1 and does not kill us with SIGPIPE") {
    auto p = CreateChildProcess();
    SpawnSpec s;
    s.exe = "true";
    s.pipe_stdin = true;
    REQUIRE(p->Start(s, nullptr));
    REQUIRE(p->Wait(2000));
    const char data[1024] = {};
    CHECK(p->WriteStdin(data, sizeof(data)) == -1);
    CHECK(p->WriteStdin(data, sizeof(data)) == -1);
}

TEST_CASE("RequestStop lets ffmpeg finish an MP4 (the file gets its index)") {
    if (!OnPath("ffmpeg") || !OnPath("ffprobe")) { MESSAGE("ffmpeg/ffprobe not on PATH - skipped"); return; }
    const std::string out = TmpPath("stop.mp4");
    std::remove(out.c_str());

    auto p = CreateChildProcess();
    SpawnSpec s;
    s.exe = "ffmpeg";
    s.args = {"-nostdin", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc=size=320x240:rate=25",
              "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-y", out};
    REQUIRE(p->Start(s, nullptr));
    usleep(2000 * 1000);
    p->RequestStop();
    REQUIRE(p->Wait(10000));
    CHECK(p->ExitCode() == 255);   // ffmpeg's own code for "stopped by a signal", the file is still finished

    // ffprobe fails on an MP4 without its moov atom, and prints a duration when the file is complete.
    const std::string cmd = "ffprobe -v error -show_entries format=duration -of csv=p=0 " + out + " > " + out + ".txt";
    CHECK(std::system(cmd.c_str()) == 0);
    FILE *f = std::fopen((out + ".txt").c_str(), "rb");
    REQUIRE(f);
    double dur = 0;
    CHECK(std::fscanf(f, "%lf", &dur) == 1);
    std::fclose(f);
    CHECK(dur > 1.0);
    std::remove(out.c_str());
    std::remove((out + ".txt").c_str());
}
