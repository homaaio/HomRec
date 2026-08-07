// hr_mic_enum.h
//
// Lists the audio *capture* endpoints (microphones) currently available, so
// Settings can offer a picker instead of always silently using whatever
// Windows currently considers the default recording device (see
// hr_audio.cpp's hr_audio_start(), which previously had no way to ask for
// anything else).
#pragma once

#include <string>
#include <vector>

struct HrMicDevice {
    // WASAPI endpoint ID (IMMDevice::GetId()) -- stable across reboots/
    // reconnects for the same physical device, unlike an array index. This
    // is what gets stored in AppState::mic_device_id and passed back into
    // hr_audio_start() to actually open the right device.
    std::string id;
    std::string name; // UTF-8 friendly name, e.g. "Microphone (USB Audio Device)"
};

// Enumerates active capture (recording) endpoints. Safe to call from the UI
// thread regardless of whether COM is already initialized there. Returns an
// empty vector if no microphone is attached or enumeration fails.
std::vector<HrMicDevice> HrEnumerateMics();
