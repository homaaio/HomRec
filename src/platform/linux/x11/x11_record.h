// x11_record.h - screen recording on X11 by letting ffmpeg's x11grab device do the capture.
// This is the "first useful release" path of plan-linux.md (phase 2): no frames pass through
// HomRec, ffmpeg grabs, encodes (libx264) and writes the MP4 itself.
#pragma once

#include <string>
#include <vector>

namespace hr {
namespace platform {

struct X11RecordOptions {
    std::string display;            // $DISPLAY, e.g. ":0" or ":0.0"
    int         fps         = 30;
    bool        draw_cursor = true;
    bool        region      = false;   // false = the whole X screen
    int         x = 0, y = 0, w = 0, h = 0;
    std::string output;             // path of the .mp4
};

// ffmpeg arguments (without the executable name) for the options above. Pure function.
std::vector<std::string> BuildX11GrabArgs(const X11RecordOptions &o);

// True when this session can record with x11grab: $DISPLAY is set and ffmpeg is found.
// Otherwise false and *why_not says what is missing.
bool X11GrabAvailable(std::string *why_not);

}  // namespace platform
}  // namespace hr
