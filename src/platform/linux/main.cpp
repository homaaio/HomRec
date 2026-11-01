// main.cpp - `homrec`, the entry point of the raw Linux build.
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
#include "../../ui/version.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace hr::platform;

namespace {

void PrintUsage() {
    std::puts(
        "homrec " HR_APP_VERSION " (raw Linux build - work in progress)\n"
        "\n"
        "Usage:\n"
        "  homrec --version     Show the HomRec version\n"
        "  homrec doctor        Check this machine: session, sound server, ffmpeg, backends\n"
        "  homrec paths         Show where config, logs and recordings would live\n"
        "  homrec start         Record the screen (not implemented yet)\n"
        "  homrec help          This text\n"
        "\n"
        "Plan and status: docs/LINUX_PORT_PLAN.md");
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

int CmdStart() {
    std::string why;
    auto cap = CreateScreenCapture(DetectDisplayServer(), &why);
    if (!cap) {
        std::fprintf(stderr, "homrec: cannot record: %s\n", why.c_str());
        return 2;       // 2 = "not available in this build", distinct from 1 = runtime error
    }
    std::fprintf(stderr, "homrec: recording is not wired up yet\n");
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) { PrintUsage(); return 0; }
    const char *cmd = argv[1];
    if (!std::strcmp(cmd, "--version") || !std::strcmp(cmd, "version")) { std::puts(HR_APP_VERSION); return 0; }
    if (!std::strcmp(cmd, "doctor")) return CmdDoctor();
    if (!std::strcmp(cmd, "paths"))  return CmdPaths();
    if (!std::strcmp(cmd, "start"))  return CmdStart();
    if (!std::strcmp(cmd, "help") || !std::strcmp(cmd, "--help") || !std::strcmp(cmd, "-h")) { PrintUsage(); return 0; }
    std::fprintf(stderr, "homrec: unknown command '%s' (try 'help')\n", cmd);
    return 1;
}
