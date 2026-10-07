// icons.h - small PNG icon loader for the UI (2.4).
//
// Looks for user-supplied PNGs next to the exe (and up to 3 folders above it, and in
// the current directory) and returns them scaled to the requested size (cached - each
// icon is read from disk and rescaled once per size). A missing file is NOT an error:
// IsOk() is false and every caller draws a text glyph instead, so the app looks right
// with or without the art. What was searched / what failed to decode is written to
// homrec.log (look for lines starting with "Icons:").
//
// Only .png files are read. The .svg files next to them are the editable source art and
// are ignored at run time.
//
// PNG decoding needs wxWidgets' PNG handler: win_main.cpp calls wxInitAllImageHandlers()
// at startup and icons.cpp registers the handler itself as a safety net.
//
// Expected files (any of the alternative names listed in icons.cpp also work):
//   icons/overlays/text.png      icons/overlays/gif.png
//   icons/overlays/image.png     icons/overlays/webcam.png
//   icons/audio/microphone.png   icons/audio/desktop_audio.png
//   (optional) icons/audio/browser.png, window.png, file.png
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
