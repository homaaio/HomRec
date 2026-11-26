// x11_record.cpp - see x11_record.h.
#include "x11_record.h"
#include "../../paths.h"

#include <cstdlib>

namespace hr {
namespace platform {

std::vector<std::string> BuildX11GrabArgs(const X11RecordOptions &o) {
    std::vector<std::string> a = {"-hide_banner", "-nostdin", "-loglevel", "info", "-f", "x11grab",
                                  "-framerate", std::to_string(o.fps),
                                  "-draw_mouse", o.draw_cursor ? "1" : "0"};
    std::string input = o.display;
    if (o.region) {
        a.push_back("-video_size");
        a.push_back(std::to_string(o.w) + "x" + std::to_string(o.h));
        input += "+" + std::to_string(o.x) + "," + std::to_string(o.y);
    }
    a.push_back("-i");
    a.push_back(input);
    // yuv420p needs even dimensions; cut an odd last row / column instead of failing to start.
    a.insert(a.end(), {"-vf", "crop=trunc(iw/2)*2:trunc(ih/2)*2",
                       "-c:v", "libx264", "-preset", "veryfast", "-crf", "23", "-pix_fmt", "yuv420p",
                       "-y", o.output});
    return a;
}

bool X11GrabAvailable(std::string *why_not) {
    const char *d = std::getenv("DISPLAY");
    if (!d || !*d) {
        if (why_not) *why_not = "$DISPLAY is not set (no X11 or XWayland display to grab)";
        return false;
    }
    if (FindFfmpeg().empty()) {
        if (why_not) *why_not = "ffmpeg not found (install ffmpeg or set HOMREC_FFMPEG)";
        return false;
    }
    return true;
}

}  // namespace platform
}  // namespace hr
