// webcam_enum.h - lists the video capture devices (webcams) attached to the system (DirectShow).
#pragma once

#include <string>
#include <vector>

struct HrWebcamDevice {
    int index = 0;
    std::string name; // UTF-8 friendly name, e.g. "HD Webcam C920", never empty
};

// Enumerates currently-attached video capture devices.
std::vector<HrWebcamDevice> HrEnumerateWebcams();
