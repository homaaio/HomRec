// icons.h - small PNG icon loader for the UI (2.4).
#pragma once

#include <wx/wx.h>
#include <string>

namespace HrIcons {

enum class Id {
    OverlayText, OverlayGif, OverlayImage, OverlayWebcam,
    AudioMic, AudioDesktop, AudioBrowser, AudioWindow, AudioFile,
};

// Scaled bitmap (square, `size` px) - invalid wxBitmap when the file isn't there.
const wxBitmap &Get(Id id, int size);

// HICON built from the same file (for the raw-Win32 overlay list); nullptr when missing.
// Owned by the cache - do NOT DestroyIcon() it.
HICON GetHIcon(Id id, int size);

// Maps an OverlayDef::type ("text"/"image"/"gif"/"webcam"/...) to its icon id.
bool IdForOverlayType(const std::string &type, Id &out);

// Drops every cached bitmap (e.g. after the user replaced the PNGs).
void ClearCache();

} // namespace HrIcons
