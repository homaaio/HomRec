// welcome.cpp - first-run wizard (AppState::first_launch) and Help > Show Welcome Screen.
// Same three pages as the Windows wizard: greeting, two basic settings, finish.
#include "dialogs.h"
#include "../themed_widgets.h"
#include "../hrc_config.h"
#include "../version.h"
#include "../../platform/paths.h"
#include "../../utils/log.h"

#include <cmath>
#include <wx/dirdlg.h>
#include <wx/hyperlink.h>

namespace {

constexpr int kResPct[] = { 100, 75, 50, 25 };
constexpr int kFpsOpt[] = { 15, 24, 30, 60 };

class WelcomeDialog : public wxDialog {
public:
    WelcomeDialog(wxWindow *parent, AppState &state, const ThemeColors &theme)
        : wxDialog(parent, wxID_ANY, "Welcome to HomRec", wxDefaultPosition, wxSize(560, 400),
                   wxDEFAULT_DIALOG_STYLE),
          state_(state), theme_(theme) {
        const wxColour accent = FromColorref(theme_.accent);
        const wxColour dim = FromColorref(theme_.text_secondary);
        const wxColour bg = FromColorref(theme_.bg);

        auto *root = new wxBoxSizer(wxVERTICAL);

        auto *title = new wxStaticText(this, wxID_ANY, "HomRec");
        wxFont tf = title->GetFont();
        tf.SetPointSize(tf.GetPointSize() + 10);
        tf.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(tf);
        title_ = title;
        root->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, 20);
        auto *sub = new wxStaticText(this, wxID_ANY, wxString("Screen Recorder  v") + HR_APP_VERSION + "  by homaaio");
        sub_ = sub;
        root->Add(sub, 0, wxLEFT | wxRIGHT | wxBOTTOM, 20);

        // ---- page 0: greeting
        page_[0] = new wxPanel(this);
        {
            auto *s = new wxBoxSizer(wxVERTICAL);
            auto *hello = new wxStaticText(page_[0], wxID_ANY, "Hello, and thanks for choosing HomRec!");
            wxFont hf = hello->GetFont();
            hf.SetWeight(wxFONTWEIGHT_BOLD);
            hello->SetFont(hf);
            s->Add(hello, 0, wxBOTTOM, 10);
            auto *body = new wxStaticText(page_[0], wxID_ANY,
                "This quick setup takes a few seconds and helps HomRec record at settings that "
                "actually fit your machine.\n\n"
                "Click Next to choose where recordings are saved and pick a resolution and fps - "
                "or check \"I understand\" to skip straight past and just use the defaults. "
                "Desktop shortcut and startup options are always available later in "
                "Settings > System.");
            body->Wrap(500);
            s->Add(body, 0);
            page_[0]->SetSizer(s);
        }
        root->Add(page_[0], 1, wxEXPAND | wxLEFT | wxRIGHT, 20);

        // ---- page 1: basic settings
        page_[1] = new wxPanel(this);
        {
            auto *s = new wxBoxSizer(wxVERTICAL);
            auto *h = new wxStaticText(page_[1], wxID_ANY, "A couple of basic settings");
            wxFont hf = h->GetFont();
            hf.SetWeight(wxFONTWEIGHT_BOLD);
            h->SetFont(hf);
            s->Add(h, 0, wxBOTTOM, 10);

            auto *grid = new wxFlexGridSizer(2, 8, 10);
            grid->AddGrowableCol(1, 1);
            grid->Add(new wxStaticText(page_[1], wxID_ANY, "Save recordings to:"), 0, wxALIGN_CENTRE_VERTICAL);
            auto *folderRow = new wxBoxSizer(wxHORIZONTAL);
            std::string folder = state_.output_folder.empty() ? hr::platform::VideosDir() : state_.output_folder;
            folder_edit_ = new wxTextCtrl(page_[1], wxID_ANY, wxString::FromUTF8(folder));
            folderRow->Add(folder_edit_, 1, wxEXPAND | wxRIGHT, 6);
            browse_btn_ = new ColorButton(page_[1], kBrowse, "Browse...");
            browse_btn_->SetMinSize(wxSize(90, 28));
            folderRow->Add(browse_btn_, 0);
            grid->Add(folderRow, 1, wxEXPAND);

            grid->Add(new wxStaticText(page_[1], wxID_ANY, "Resolution:"), 0, wxALIGN_CENTRE_VERTICAL);
            res_choice_ = new wxChoice(page_[1], wxID_ANY);
            res_choice_->Append("100% (Native)");
            res_choice_->Append("75%");
            res_choice_->Append("50%");
            res_choice_->Append("25%");
            int rsel = 1;
            for (int i = 0; i < 4; ++i)
                if (std::abs(state_.scale_factor * 100.0 - kResPct[i]) < 1.0) rsel = i;
            res_choice_->SetSelection(rsel);
            grid->Add(res_choice_, 0);

            grid->Add(new wxStaticText(page_[1], wxID_ANY, "FPS:"), 0, wxALIGN_CENTRE_VERTICAL);
            fps_choice_ = new wxChoice(page_[1], wxID_ANY);
            int fsel = 0;
            for (int i = 0; i < 4; ++i) {
                fps_choice_->Append(wxString::Format("%d", kFpsOpt[i]));
                if (state_.target_fps == kFpsOpt[i]) fsel = i;
            }
            fps_choice_->SetSelection(fsel);
            grid->Add(fps_choice_, 0);
            s->Add(grid, 0, wxEXPAND | wxBOTTOM, 14);

            skip_chk_ = new wxCheckBox(page_[1], wxID_ANY, "I understand - skip this and just use the defaults");
            s->Add(skip_chk_, 0);
            page_[1]->SetSizer(s);
        }
        root->Add(page_[1], 1, wxEXPAND | wxLEFT | wxRIGHT, 20);

        // ---- page 2: finish
        page_[2] = new wxPanel(this);
        {
            auto *s = new wxBoxSizer(wxVERTICAL);
            auto *h = new wxStaticText(page_[2], wxID_ANY, "You're all set");
            wxFont hf = h->GetFont();
            hf.SetWeight(wxFONTWEIGHT_BOLD);
            h->SetFont(hf);
            s->Add(h, 0, wxBOTTOM, 10);
            auto *body = new wxStaticText(page_[2], wxID_ANY,
                "Good luck recording! If you'd like a tour of what HomRec can do, the documentation "
                "and changelog are one click away below - and you can always re-open this wizard "
                "later from Help > Show Welcome Screen.\n\n"
                "Note: on Linux the window and its settings are ready, but recording, overlays and "
                "the audio mixer are not available yet (not now).");
            body->Wrap(500);
            s->Add(body, 0, wxBOTTOM, 14);
            auto *links = new wxBoxSizer(wxHORIZONTAL);
            links->Add(new wxHyperlinkCtrl(page_[2], wxID_ANY, "Changelog", "https://github.com/homaaio/HomREC/blob/main/CHANGELOG.txt"), 0, wxRIGHT, 16);
            links->Add(new wxHyperlinkCtrl(page_[2], wxID_ANY, "GitHub", "https://github.com/homaaio/HomREC"), 0, wxRIGHT, 16);
            links->Add(new wxHyperlinkCtrl(page_[2], wxID_ANY, "Website", "https://homaaio.github.io/HomREC/"));
            s->Add(links, 0);
            page_[2]->SetSizer(s);
        }
        root->Add(page_[2], 1, wxEXPAND | wxLEFT | wxRIGHT, 20);

        // ---- navigation
        auto *nav = new wxBoxSizer(wxHORIZONTAL);
        back_btn_ = new ColorButton(this, kBack, "Back");
        back_btn_->SetMinSize(wxSize(90, 32));
        next_btn_ = new ColorButton(this, kNext, "Next");
        next_btn_->SetMinSize(wxSize(110, 32));
        nav->Add(back_btn_, 0);
        nav->AddStretchSpacer(1);
        nav->Add(next_btn_, 0);
        root->Add(nav, 0, wxEXPAND | wxALL, 20);

        SetSizer(root);
        ThemeWindow(this, theme_);
        title->SetForegroundColour(accent);
        sub->SetForegroundColour(dim);
        back_btn_->SetColours(FromColorref(theme_.surface_light), FromColorref(theme_.text));
        next_btn_->SetColours(accent, bg);
        browse_btn_->SetColours(FromColorref(theme_.surface_light), FromColorref(theme_.text));

        Bind(wxEVT_BUTTON, &WelcomeDialog::OnBack, this, kBack);
        Bind(wxEVT_BUTTON, &WelcomeDialog::OnNext, this, kNext);
        Bind(wxEVT_BUTTON, &WelcomeDialog::OnBrowse, this, kBrowse);
        skip_chk_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
            const bool on = !skip_chk_->GetValue();
            folder_edit_->Enable(on);
            res_choice_->Enable(on);
            fps_choice_->Enable(on);
            browse_btn_->Enable2(on);
        });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &) { EndModal(wxID_CANCEL); });

        ShowPage(0);
        CentreOnParent();
    }

private:
    enum { kBack = wxID_HIGHEST + 1, kNext, kBrowse };

    void ShowPage(int p) {
        page_no_ = p;
        for (int i = 0; i < 3; ++i) page_[i]->Show(i == p);
        back_btn_->Show(p > 0);
        next_btn_->SetLabelText2(p == 2 ? "Get started" : "Next");
        Layout();
        Refresh();
    }

    void OnBack(wxCommandEvent &) { if (page_no_ > 0) ShowPage(page_no_ - 1); }

    void OnNext(wxCommandEvent &) {
        if (page_no_ == 1) Apply();
        if (page_no_ == 2) { EndModal(wxID_OK); return; }
        ShowPage(page_no_ + 1);
    }

    void OnBrowse(wxCommandEvent &) {
        wxDirDialog dlg(this, "Choose where recordings are saved", folder_edit_->GetValue(),
                        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) folder_edit_->SetValue(dlg.GetPath());
    }

    // Writes the choices into the state and the .hrc file (the wizard only runs once per install).
    void Apply() {
        if (!skip_chk_->GetValue()) {
            const std::string folder = folder_edit_->GetValue().ToUTF8().data();
            if (!folder.empty()) state_.output_folder = folder;
            const int r = res_choice_->GetSelection();
            if (r >= 0 && r < 4) {
                state_.scale_factor = kResPct[r] / 100.0;
                state_.resolution_mode = ResolutionMode::Percent;
            }
            const int f = fps_choice_->GetSelection();
            if (f >= 0 && f < 4) state_.target_fps = kFpsOpt[f];
        }
        state_.first_launch = false;
        const std::wstring target = HrcConfig::ResolveSettingsPath(state_);
        if (!HrcConfig::Save(state_, target)) HrLog::Error("Welcome: couldn't save the settings file.");
        if (target != HrcConfig::kDefaultSettingsPath) HrcConfig::Save(state_, HrcConfig::kDefaultSettingsPath);
    }

    AppState &state_;
    ThemeColors theme_;
    int page_no_ = 0;
    wxPanel *page_[3] = { nullptr, nullptr, nullptr };
    wxStaticText *title_ = nullptr, *sub_ = nullptr;
    wxTextCtrl *folder_edit_ = nullptr;
    wxChoice *res_choice_ = nullptr, *fps_choice_ = nullptr;
    wxCheckBox *skip_chk_ = nullptr;
    ColorButton *back_btn_ = nullptr, *next_btn_ = nullptr, *browse_btn_ = nullptr;
};

}  // namespace

void ShowWelcomeDialog(wxWindow *parent, AppState &state, const ThemeColors &theme) {
    WelcomeDialog dlg(parent, state, theme);
    dlg.ShowModal();
    // Closed with the X on the first page: don't show the wizard again at every start.
    if (state.first_launch) {
        state.first_launch = false;
        HrcConfig::Save(state, HrcConfig::ResolveSettingsPath(state));
    }
}
