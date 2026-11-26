// main.cpp - `homrec`, the entry point of the Linux build.
//
// Right now it only knows how to look at the machine (`doctor`) and to say honestly that recording
// is not there yet. It exists so that (a) the Linux build system, CI and packaging have a real
// binary to build and smoke-test from day one, and (b) every platform interface gets exercised by a
// caller. When the backends land, `start` grows into the Linux counterpart of tools/homrec/homrec.cpp.
#include "../platform.h"
#include "../capture_backend.h"
#include "../audio_backend.h"
#include "../window_backend.h"
#include "../hotkey_backend.h"
#include "../paths.h"
#include "../process_backend.h"
#include "x11/x11_record.h"
#include "../../ui/version.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <sys/stat.h>

using namespace hr::platform;

namespace {

void PrintUsage() {
    std::puts(
        "homrec " HR_APP_VERSION " (Linux build - work in progress)\n"
        "\n"
        "Usage:\n"
        "  homrec --version     Show the HomRec version\n"
        "  homrec doctor        Check this machine: session, sound server, ffmpeg, backends\n"
        "  homrec paths         Show where config, logs and recordings would live\n"
        "  homrec start [opts]  Record the screen (X11 only for now), Ctrl+C stops and saves\n"
        "      --output FILE      .mp4 to write (default: <recordings dir>/HomRec_<date>_<time>.mp4)\n"
        "      --fps N            frames per second (default 30)\n"
        "      --seconds N        stop by itself after N seconds\n"
        "      --region X,Y,WxH   record only this rectangle of the screen\n"
        "      --no-cursor        leave the mouse pointer out\n"
        "  homrec help          This text\n"
        "\n"
        "Plan and status: plan-linux.md");
}

const char *YesNo(bool b) { return b ? "yes" : "NO"; }

int CmdDoctor() {
    const SessionInfo s = DetectSession();
    std::printf("HomRec %s on %s\n\n", HR_APP_VERSION, PlatformName());

    std::puts("Session");
    std::printf("  display server : %s%s\n", DisplayServerName(s.display_server),
                (s.display_server == DisplayServer::Wayland && s.xwayland_available) ? " (+XWayland)" : "");
    std::printf("  desktop        : %s\n", s.desktop.empty() ? "(unknown)" : s.desktop.c_str());
    std::printf("  sound server   : %s\n", AudioServerName(s.audio_server));

    std::puts("\nTools");
    const std::string ff = FindFfmpeg();
    std::printf("  ffmpeg         : %s\n", ff.empty() ? "NOT FOUND (install ffmpeg or set HOMREC_FFMPEG)" : ff.c_str());
    if (!ff.empty()) {
        const std::string fv = FfmpegVersion(ff);
        std::printf("  ffmpeg version : %s\n", fv.empty() ? "(could not run it)" : fv.c_str());
    }

    std::puts("\nBackends (what this build can do in this session)");
    const CaptureCaps cc = QueryCaptureCaps(s.display_server);
    std::printf("  screen capture : %s%s%s\n", YesNo(cc.available),
                cc.available ? " - " : "  ", cc.available ? cc.backend_name.c_str() : cc.unavailable_reason.c_str());
    const AudioCaps ac = QueryAudioCaps();
    std::printf("  audio capture  : %s%s%s\n", YesNo(ac.available),
                ac.available ? " - " : "  ", ac.available ? ac.backend_name.c_str() : ac.unavailable_reason.c_str());
    std::string why;
    auto ws = CreateWindowService(s.display_server, &why);
    std::printf("  window list    : %s%s%s\n", YesNo(ws != nullptr), ws ? "" : "  ", ws ? "" : why.c_str());
    why.clear();
    const bool hk = HotkeysAvailable(s.display_server, &why);
    std::printf("  global hotkeys : %s%s%s\n", YesNo(hk), hk ? "" : "  ", hk ? "" : why.c_str());
    return 0;
}

int CmdPaths() {
    std::printf("portable mode : %s\n", IsPortableMode() ? "yes" : "no");
    std::printf("executable dir: %s\n", ExecutableDir().c_str());
    std::printf("config dir    : %s\n", ConfigDir().c_str());
    std::printf("data dir      : %s\n", DataDir().c_str());
    std::printf("log dir       : %s\n", LogDir().c_str());
    std::printf("recordings    : %s\n", VideosDir().c_str());
    return 0;
}

volatile std::sig_atomic_t g_stop = 0;
void OnSignal(int) { g_stop = 1; }

struct StartArgs {
    std::string output;
    int  fps     = 30;
    int  seconds = 0;     // 0 = until Ctrl+C
    bool cursor  = true;
    bool region  = false;
    int  x = 0, y = 0, w = 0, h = 0;
};

bool ParseStartArgs(int argc, char **argv, StartArgs *a, std::string *err) {
    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        auto value = [&]() -> const char * {
            if (i + 1 >= argc) { *err = k + " needs a value"; return nullptr; }
            return argv[++i];
        };
        if (k == "--output" || k == "-o") {
            const char *v = value(); if (!v) return false;
            a->output = v;
        } else if (k == "--fps") {
            const char *v = value(); if (!v) return false;
            a->fps = std::atoi(v);
            if (a->fps < 1 || a->fps > 240) { *err = "--fps must be 1..240"; return false; }
        } else if (k == "--seconds") {
            const char *v = value(); if (!v) return false;
            a->seconds = std::atoi(v);
            if (a->seconds < 1) { *err = "--seconds must be 1 or more"; return false; }
        } else if (k == "--region") {
            const char *v = value(); if (!v) return false;
            if (std::sscanf(v, "%d,%d,%dx%d", &a->x, &a->y, &a->w, &a->h) != 4 ||
                a->x < 0 || a->y < 0 || a->w < 2 || a->h < 2) {
                *err = "--region must look like X,Y,WxH (for example 0,0,1280x720)";
                return false;
            }
            a->region = true;
        } else if (k == "--no-cursor") {
            a->cursor = false;
        } else {
            *err = "unknown option '" + k + "'";
            return false;
        }
    }
    return true;
}

int CmdStart(int argc, char **argv) {
    StartArgs a;
    std::string err;
    if (!ParseStartArgs(argc, argv, &a, &err)) {
        std::fprintf(stderr, "homrec start: %s\n", err.c_str());
        return 1;
    }

    const CaptureCaps cc = QueryCaptureCaps(DetectDisplayServer());
    if (!cc.available) {
        std::fprintf(stderr, "homrec: cannot record: %s\n", cc.unavailable_reason.c_str());
        return 2;       // 2 = "not available in this build / session", distinct from 1 = runtime error
    }

    X11RecordOptions o;
    o.display     = std::getenv("DISPLAY");
    o.fps         = a.fps;
    o.draw_cursor = a.cursor;
    o.region      = a.region;
    o.x = a.x; o.y = a.y; o.w = a.w; o.h = a.h;
    o.output      = a.output;
    if (o.output.empty()) {
        EnsureDir(VideosDir());
        char stamp[32];
        const time_t now = time(nullptr);
        struct tm lt;
        localtime_r(&now, &lt);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &lt);
        o.output = VideosDir() + "/HomRec_" + stamp + ".mp4";
    }

    EnsureDir(LogDir());
    const std::string log = LogDir() + "/ffmpeg_x11grab.log";

    auto proc = CreateChildProcess();
    SpawnSpec spec;
    spec.exe         = FindFfmpeg();
    spec.args        = BuildX11GrabArgs(o);
    spec.stderr_path = log;
    if (!proc->Start(spec, &err)) {
        std::fprintf(stderr, "homrec: cannot start ffmpeg: %s\n", err.c_str());
        return 1;
    }

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = OnSignal;          // no SA_RESTART: a signal ends the Wait() sleep early
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::printf("Recording %s at %d fps to %s\n", a.region ? "a region" : "the screen", a.fps, o.output.c_str());
    std::printf(a.seconds > 0 ? "Stops by itself after %d s (Ctrl+C stops earlier)\n" : "Press Ctrl+C to stop and save\n",
                a.seconds);
    std::fflush(stdout);

    const auto t0 = std::chrono::steady_clock::now();
    bool asked_to_stop = false, killed = false;
    while (!proc->Wait(200)) {
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (g_stop || (a.seconds > 0 && el >= a.seconds)) {
            asked_to_stop = true;
            proc->RequestStop();           // ffmpeg finishes the MP4 (it has no index before that)
            break;
        }
    }
    if (asked_to_stop && !proc->Wait(10000)) {
        std::fprintf(stderr, "homrec: ffmpeg did not finish in 10 s, killing it - the file may be unplayable\n");
        proc->Kill();
        killed = true;
    }

    const int code = proc->ExitCode();
    // ffmpeg answers a stop signal with exit code 255 even though the file is complete.
    const bool ok = !killed && (code == 0 || (asked_to_stop && code == 255));
    if (!ok) {
        std::fprintf(stderr, "homrec: ffmpeg stopped with code %d, see %s\n", code, log.c_str());
        return 1;
    }
    struct stat st;
    if (stat(o.output.c_str(), &st) != 0 || st.st_size == 0) {
        std::fprintf(stderr, "homrec: ffmpeg finished but %s was not written, see %s\n", o.output.c_str(), log.c_str());
        return 1;
    }
    std::printf("Saved %s (%lld bytes)\n", o.output.c_str(), (long long)st.st_size);
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) { PrintUsage(); return 0; }
    const char *cmd = argv[1];
    if (!std::strcmp(cmd, "--version") || !std::strcmp(cmd, "version")) { std::puts(HR_APP_VERSION); return 0; }
    if (!std::strcmp(cmd, "doctor")) return CmdDoctor();
    if (!std::strcmp(cmd, "paths"))  return CmdPaths();
    if (!std::strcmp(cmd, "start"))  return CmdStart(argc, argv);
    if (!std::strcmp(cmd, "help") || !std::strcmp(cmd, "--help") || !std::strcmp(cmd, "-h")) { PrintUsage(); return 0; }
    std::fprintf(stderr, "homrec: unknown command '%s' (try 'help')\n", cmd);
    return 1;
}
