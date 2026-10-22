// mic_enum.h - lists the available audio capture devices (microphones) for the Settings picker.
#pragma once

#include <string>
#include <vector>

struct HrMicDevice {
    // WASAPI endpoint ID (IMMDevice::GetId()) -- stable across reboots/ reconnects for the
    // same physical device, unlike an array index.
    std::string id;
    std::string name; // UTF-8 friendly name, e.g. "Microphone (USB Audio Device)"
};

// Enumerates active capture (recording) endpoints. Safe to call from the UI
// thread regardless of whether COM is already initialized there. Returns an
// empty vector if no microphone is attached or enumeration fails.
std::vector<HrMicDevice> HrEnumerateMics();
