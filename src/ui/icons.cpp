#include "icons.h"
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <map>
#include <memory>

namespace HrIcons {
namespace {

struct Entry {
    wxBitmap bmp;
    wxIcon   icon;
    bool     tried = false;
};

std::map<std::pair<int, int>, Entry> &Cache() {
    static std::map<std::pair<int, int>, Entry> c;
    return c;
}

// Alternative file names accepted per icon, in priority order.
const char *const *Names(Id id) {
    static const char *text[]    = {"text", "overlay_text", "text_overlay", "txt", nullptr};
    static const char *gif[]     = {"gif", "overlay_gif", "gif_overlay", nullptr};
    static const char *image[]   = {"image", "overlay_image", "image_overlay", "img", "picture", nullptr};
    static const char *webcam[]  = {"webcam", "overlay_webcam", "webcam_overlay", "camera", "cam", nullptr};
    static const char *mic[]     = {"microphone", "mic", "audio_microphone", "audio_mic", nullptr};
    static const char *desktop[] = {"desktop_audio", "desktop", "audio_desktop", "speaker", "system_audio", nullptr};
    static const char *browser[] = {"browser", "audio_browser", "web", nullptr};
    static const char *window[]  = {"window", "window_audio", "audio_window", nullptr};
    static const char *file[]    = {"file", "audio_file", "music", nullptr};
    switch (id) {
        case Id::OverlayText:    return text;
        case Id::OverlayGif:     return gif;
        case Id::OverlayImage:   return image;
        case Id::OverlayWebcam:  return webcam;
        case Id::AudioMic:       return mic;
        case Id::AudioDesktop:   return desktop;
        case Id::AudioBrowser:   return browser;
        case Id::AudioWindow:    return window;
        case Id::AudioFile:      return file;
    }
    return text;
}

bool IsOverlayIcon(Id id) {
    return id == Id::OverlayText || id == Id::OverlayGif || id == Id::OverlayImage || id == Id::OverlayWebcam;
}

wxString FindFile(Id id) {
    const wxString exeDir = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
    // Sub-folder first (icons/overlays, icons/audio), then the shared folders.
    const char *sub = IsOverlayIcon(id) ? "overlays" : "audio";
    const wxString dirs[] = {
        exeDir + "/icons/" + sub,
        exeDir + "/icons/ui",
        exeDir + "/icons",
        exeDir + "/Assets/icons/" + sub,
        exeDir + "/Assets/icons",
        wxGetCwd() + "/icons/" + sub,
        wxGetCwd() + "/icons",
    };
    for (const wxString &d : dirs) {
        for (const char *const *n = Names(id); *n; ++n) {
            wxString p = d + "/" + *n + ".png";
            if (wxFileName::FileExists(p)) return p;
        }
    }
    return wxString();
}

Entry &Load(Id id, int size) {
    Entry &e = Cache()[{(int)id, size}];
    if (e.tried) return e;
    e.tried = true;
    const wxString path = FindFile(id);
    if (path.empty()) return e;
    wxImage img;
    {
        wxLogNull quiet;   // a corrupt PNG must not pop a message box
        if (!img.LoadFile(path, wxBITMAP_TYPE_PNG) || !img.IsOk()) return e;
    }
    if (!img.HasAlpha()) img.InitAlpha();
    if (img.GetWidth() != size || img.GetHeight() != size)
        img.Rescale(size, size, wxIMAGE_QUALITY_HIGH);
    e.bmp = wxBitmap(img);
    if (e.bmp.IsOk()) e.icon.CopyFromBitmap(e.bmp);
    return e;
}

} // namespace

const wxBitmap &Get(Id id, int size) {
    if (size < 8) size = 8;
    if (size > 128) size = 128;
    return Load(id, size).bmp;
}

HICON GetHIcon(Id id, int size) {
    if (size < 8) size = 8;
    if (size > 128) size = 128;
    Entry &e = Load(id, size);
    return e.icon.IsOk() ? (HICON)e.icon.GetHandle() : nullptr;
}

bool IdForOverlayType(const std::string &type, Id &out) {
    if (type == "text")   { out = Id::OverlayText;   return true; }
    if (type == "gif")    { out = Id::OverlayGif;    return true; }
    if (type == "image")  { out = Id::OverlayImage;  return true; }
    if (type == "webcam") { out = Id::OverlayWebcam; return true; }
    return false;
}

void ClearCache() { Cache().clear(); }

} // namespace HrIcons
