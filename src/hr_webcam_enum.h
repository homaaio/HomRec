// hr_webcam_enum.h
//
// Lists the video capture devices ("webcams") currently attached to the
// system, so the UI can offer a picker instead of asking the user to type
// a raw camera index from memory (see overlays_dock_panel.cpp's
// AddWebcamOverlay / overlay_add_dialogs.cpp's HrPromptForWebcamDevice).
//
// Implemented via DirectShow's device-enumeration category (still the
// standard way to list capture devices without pulling in a full Media
// Foundation dependency) -- this only *lists* devices and reads their
// friendly names, it doesn't open/capture from any of them.
#pragma once

#include <string>
#include <vector>

struct HrWebcamDevice {
    // Index matching this device's position in DirectShow's own enumeration
    // order -- this is the same number AppState::OverlayDef::webcam_index
    // has always stored (previously typed in by hand), so existing/older
    // profiles that already have a webcam_index keep meaning the same
    // physical device as before.
    int index = 0;
    std::string name; // UTF-8 friendly name, e.g. "HD Webcam C920", never empty
};

// Enumerates currently-attached video capture devices. Safe to call from the
// UI thread regardless of whether COM has already been initialized there
// (wx/other code may have done so) -- it only initializes/uninitializes its
// own reference if it actually owns one. Returns an empty vector if no
// camera is attached or enumeration fails for any reason (never throws).
std::vector<HrWebcamDevice> HrEnumerateWebcams();
