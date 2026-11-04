// about.cpp - Help > About, plus the two tiny helpers every Linux dialog shares (ThemeWindow, ShowNotNow).
#include "dialogs.h"
#include "../themed_widgets.h"
#include "../version.h"
#include "../../platform/platform.h"
#include "../../platform/paths.h"

#include <wx/hyperlink.h>

void ThemeWindow(wxWindow *w, const ThemeColors &theme) {
    if (!w) return;
    const wxColour bg = FromColorref(theme.bg);
    const wxColour surface = FromColorref(theme.surface);
    const wxColour text = FromColorref(theme.text);
    w->SetBackgroundColour(bg);
    w->SetForegroundColour(text);
    for (wxWindow *child : w->GetChildren()) {
        if (dynamic_cast<ColorButton *>(child)) continue;       // paint themselves, coloured by their owner
        if (dynamic_cast<StatusDot *>(child)) continue;
        if (dynamic_cast<wxHyperlinkCtrl *>(child)) { child->SetBackgroundColour(bg); continue; }   // keeps its link colour
        if (dynamic_cast<wxTextCtrl *>(child) || dynamic_cast<wxChoice *>(child) ||
            dynamic_cast<wxListBox *>(child)) {
            child->SetBackgroundColour(surface);
            child->SetForegroundColour(text);
            continue;
        }
        ThemeWindow(child, theme);
    }
}

void ShowNotNow(wxWindow *parent, const wxString &what) {
    wxMessageBox(what + " is not available on Linux yet (not now).", "HomRec",
                 wxOK | wxICON_INFORMATION, parent);
}

namespace {

class AboutDialog : public wxDialog {
public:
    AboutDialog(wxWindow *parent, const ThemeColors &theme)
        : wxDialog(parent, wxID_ANY, "About HomRec", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE) {
        const wxColour accent = FromColorref(theme.accent);
        const wxColour dim = FromColorref(theme.text_secondary);

        auto *root = new wxBoxSizer(wxVERTICAL);

        auto *title = new wxStaticText(this, wxID_ANY, "HomRec");
        wxFont tf = title->GetFont();
        tf.SetPointSize(tf.GetPointSize() + 10);
        tf.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(tf);
        title->SetForegroundColour(accent);
        root->Add(title, 0, wxALIGN_CENTRE_HORIZONTAL | wxTOP, 20);

        auto *ver = new wxStaticText(this, wxID_ANY,
            wxString("Screen Recorder  v") + HR_APP_VERSION + "  (Linux, early build)");
        ver->SetForegroundColour(dim);
        root->Add(ver, 0, wxALIGN_CENTRE_HORIZONTAL | wxTOP, 4);

        auto *by = new wxStaticText(this, wxID_ANY, "by homaaio  -  GPLv3");
        by->SetForegroundColour(dim);
        root->Add(by, 0, wxALIGN_CENTRE_HORIZONTAL | wxTOP, 2);

        // What this machine looks like to HomRec - the first thing a bug report needs.
        const hr::platform::SessionInfo s = hr::platform::DetectSession();
        wxString env;
        env << "Display server: " << hr::platform::DisplayServerName(s.display_server)
            << (s.xwayland_available ? " (+XWayland)" : "") << "\n"
            << "Desktop: " << (s.desktop.empty() ? std::string("(unknown)") : s.desktop) << "\n"
            << "Sound server: " << hr::platform::AudioServerName(s.audio_server) << "\n"
            << "Config folder: " << wxString::FromUTF8(hr::platform::ConfigDir());
        auto *envLbl = new wxStaticText(this, wxID_ANY, env);
        root->Add(envLbl, 0, wxALL | wxALIGN_CENTRE_HORIZONTAL, 16);

        auto *note = new wxStaticText(this, wxID_ANY,
            "Recording, overlays and the audio mixer are not available on Linux yet (not now).");
        note->SetForegroundColour(dim);
        root->Add(note, 0, wxLEFT | wxRIGHT | wxALIGN_CENTRE_HORIZONTAL, 16);

        auto *links = new wxBoxSizer(wxHORIZONTAL);
        links->Add(new wxHyperlinkCtrl(this, wxID_ANY, "GitHub", "https://github.com/homaaio/HomRec"), 0, wxRIGHT, 16);
        links->Add(new wxHyperlinkCtrl(this, wxID_ANY, "Website", "https://homaaio.github.io/HomREC/"), 0, wxRIGHT, 16);
        links->Add(new wxHyperlinkCtrl(this, wxID_ANY, "Changelog", "https://github.com/homaaio/HomREC/blob/main/CHANGELOG.txt"));
        root->Add(links, 0, wxALIGN_CENTRE_HORIZONTAL | wxTOP, 14);

        auto *close = new ColorButton(this, wxID_OK, "Close");
        close->SetColours(accent, FromColorref(theme.bg));
        close->SetMinSize(wxSize(100, 32));
        root->Add(close, 0, wxALIGN_CENTRE_HORIZONTAL | wxALL, 16);

        SetSizerAndFit(root);
        ThemeWindow(this, theme);
        title->SetForegroundColour(accent);
        ver->SetForegroundColour(dim);
        by->SetForegroundColour(dim);
        note->SetForegroundColour(dim);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_OK); }, wxID_OK);
        CentreOnParent();
    }
};

}  // namespace

void ShowAboutDialog(wxWindow *parent, const ThemeColors &theme) {
    AboutDialog dlg(parent, theme);
    dlg.ShowModal();
}
