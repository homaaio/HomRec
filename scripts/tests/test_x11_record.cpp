// scripts/tests/test_x11_record.cpp - src/platform/linux/x11/x11_record.cpp.
//
// Build: scripts/linux/test.sh. The last case starts its own Xvfb and records it with ffmpeg;
// it is skipped when Xvfb, ffmpeg or ffprobe is missing.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "platform/linux/x11/x11_record.h"
#include "platform/paths.h"
#include "platform/process_backend.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace hr::platform;

namespace {

bool Has(const std::vector<std::string> &v, const std::string &s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// True when `a` is directly followed by `b`.
bool Pair(const std::vector<std::string> &v, const std::string &a, const std::string &b) {
    for (size_t i = 0; i + 1 < v.size(); ++i)
        if (v[i] == a && v[i + 1] == b) return true;
    return false;
}

bool OnPath(const char *name) {
    const std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

}  // namespace

TEST_CASE("whole screen: no -video_size, no offset") {
    X11RecordOptions o;
    o.display = ":0";
    o.output  = "/tmp/x.mp4";
    const auto a = BuildX11GrabArgs(o);
    CHECK(Pair(a, "-f", "x11grab"));
    CHECK(Pair(a, "-framerate", "30"));
    CHECK(Pair(a, "-draw_mouse", "1"));
    CHECK(Pair(a, "-i", ":0"));
    CHECK_FALSE(Has(a, "-video_size"));
    CHECK(a.back() == "/tmp/x.mp4");
}

TEST_CASE("region, fps and cursor") {
    X11RecordOptions o;
    o.display = ":1.0";
    o.fps = 60;
    o.draw_cursor = false;
    o.region = true;
    o.x = 10; o.y = 20; o.w = 640; o.h = 480;
    o.output = "out file.mp4";   // spaces stay in one argument: nothing goes through a shell
    const auto a = BuildX11GrabArgs(o);
    CHECK(Pair(a, "-framerate", "60"));
    CHECK(Pair(a, "-draw_mouse", "0"));
    CHECK(Pair(a, "-video_size", "640x480"));
    CHECK(Pair(a, "-i", ":1.0+10,20"));
    CHECK(a.back() == "out file.mp4");
}

TEST_CASE("X11GrabAvailable needs $DISPLAY") {
    const char *old = std::getenv("DISPLAY");
    const std::string saved = old ? old : "";
    unsetenv("DISPLAY");
    std::string why;
    CHECK_FALSE(X11GrabAvailable(&why));
    CHECK(why.find("DISPLAY") != std::string::npos);
    if (old) setenv("DISPLAY", saved.c_str(), 1);
}

TEST_CASE("records a real (Xvfb) screen, odd size is cut to even") {
    if (!OnPath("Xvfb") || !OnPath("ffmpeg") || !OnPath("ffprobe")) { MESSAGE("Xvfb/ffmpeg/ffprobe missing - skipped"); return; }
    const std::string sock = "/tmp/.X11-unix/X97";
    struct stat st;
    if (stat(sock.c_str(), &st) == 0) { MESSAGE(":97 is in use - skipped"); return; }

    auto xvfb = CreateChildProcess();
    SpawnSpec xs;
    xs.exe = "Xvfb";
    xs.args = {":97", "-screen", "0", "641x481x24", "-nolisten", "tcp"};
    REQUIRE(xvfb->Start(xs, nullptr));
    for (int i = 0; i < 100 && stat(sock.c_str(), &st) != 0; ++i) usleep(50 * 1000);
    REQUIRE(stat(sock.c_str(), &st) == 0);

    const std::string out = "/tmp/hr_test_x11_" + std::to_string((long)getpid()) + ".mp4";
    std::remove(out.c_str());
    X11RecordOptions o;
    o.display = ":97";
    o.output  = out;
    auto ff = CreateChildProcess();
    SpawnSpec fs;
    fs.exe  = "ffmpeg";
    fs.args = BuildX11GrabArgs(o);
    REQUIRE(ff->Start(fs, nullptr));
    usleep(2000 * 1000);
    ff->RequestStop();
    REQUIRE(ff->Wait(10000));
    xvfb->Kill();

    const std::string cmd = "ffprobe -v error -show_entries stream=width,height -of csv=p=0 " + out + " > " + out + ".txt";
    CHECK(std::system(cmd.c_str()) == 0);
    FILE *f = std::fopen((out + ".txt").c_str(), "rb");
    REQUIRE(f);
    int w = 0, h = 0;
    CHECK(std::fscanf(f, "%d,%d", &w, &h) == 2);
    std::fclose(f);
    CHECK(w == 640);
    CHECK(h == 480);
    std::remove(out.c_str());
    std::remove((out + ".txt").c_str());
}
