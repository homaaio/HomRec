#include "settings_dialog.h"
#include "themed_widgets.h"
#include <wx/spinctrl.h>
#include <wx/dirdlg.h>
#include <string>

extern "C" {
    void *hr_settings_create();
    void hr_settings_destroy(void *handle);
    int hr_settings_load(void *handle, const char *path);
    int hr_settings_save(const void *handle, const char *path);
    void hr_settings_set_output_folder(void *h, const char *v);
    void hr_settings_set_quality(void *h, int v);
    void hr_settings_set_fps(void *h, int v);
    void hr_settings_set_monitor(void *h, int v);
    void hr_settings_set_flag(void *h, const char *name, int v);
}

namespace {
constexpr char kSettingsPath[] = "homrec_settings.json"; // relative to app root, matches constants.py's SETTINGS_PATH

enum { IDC_QUALITY = 3001, IDC_BROWSE = 3002, IDC_SAVE = 3003, IDC_CANCEL = 3004 };

class SettingsDialog : public wxDialog {
public:
    SettingsDialog(wxWindow *parent, AppState &state, const ThemeColors &theme)
        : wxDialog(parent, wxID_ANY, "Settings", wxDefaultPosition, wxSize(460, 460)),
          state_(state), theme_(theme) {
        settings_ = hr_settings_create();
        hr_settings_load(settings_, kSettingsPath);

        wxColour bg = FromColorref(theme_.bg);
        wxColour surface = FromColorref(theme_.surface);
        wxColour text = FromColorref(theme_.text);
        wxColour accent = FromColorref(theme_.accent);
        SetBackgroundColour(bg);

        auto *root = new wxBoxSizer(wxVERTICAL);
        auto *grid = new wxFlexGridSizer(2, 10, 10);
        grid->AddGrowableCol(1, 1);

        auto addLabel = [&](const wxString &s) {
            auto *lbl = new wxStaticText(this, wxID_ANY, s);
            lbl->SetForegroundColour(text);
            lbl->SetBackgroundColour(bg);
            grid->Add(lbl, 0, wxALIGN_CENTRE_VERTICAL);
        };

        addLabel("Quality:");
        quality_slider_ = new ColorSlider(this, IDC_QUALITY, state_.quality, 0, 100);
        quality_slider_->SetTheme(surface, accent, text);
        grid->Add(quality_slider_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        addLabel("Target FPS:");
        fps_spin_ = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                    wxSP_ARROW_KEYS, 1, 240, state_.target_fps);
        grid->Add(fps_spin_, 0, wxALIGN_CENTRE_VERTICAL);

        addLabel("Monitor:");
        monitor_spin_ = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                        wxSP_ARROW_KEYS, 0, 15, state_.monitor_id);
        grid->Add(monitor_spin_, 0, wxALIGN_CENTRE_VERTICAL);

        root->Add(grid, 0, wxEXPAND | wxALL, 16);

        auto *folderLbl = new wxStaticText(this, wxID_ANY, "Output folder:");
        folderLbl->SetForegroundColour(text);
        folderLbl->SetBackgroundColour(bg);
        root->Add(folderLbl, 0, wxLEFT | wxRIGHT, 16);

        auto *folderRow = new wxBoxSizer(wxHORIZONTAL);
        folder_edit_ = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(state_.output_folder));
        folderRow->Add(folder_edit_, 1, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
        auto *browseBtn = new ColorButton(this, IDC_BROWSE, "Browse");
        browseBtn->SetMinSize(wxSize(70, 26));
        browseBtn->SetColours(surface, text);
        folderRow->Add(browseBtn, 0);
        root->Add(folderRow, 0, wxEXPAND | wxALL, 16);

        auto addCheck = [&](const wxString &label, bool value) {
            auto *chk = new wxCheckBox(this, wxID_ANY, label);
            chk->SetValue(value);
            chk->SetForegroundColour(text);
            chk->SetBackgroundColour(bg);
            root->Add(chk, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);
            return chk;
        };
        countdown_chk_ = addCheck("Countdown (3s)", state_.countdown_enabled);
        timestamp_chk_ = addCheck("Timestamp", state_.timestamp_enabled);
        cursor_chk_    = addCheck("Cursor", state_.cursor_enabled);
        notify_chk_    = addCheck("Show summary", state_.show_summary);

        root->AddStretchSpacer(1);

        auto *btnRow = new wxBoxSizer(wxHORIZONTAL);
        btnRow->AddStretchSpacer(1);
        auto *saveBtn = new ColorButton(this, IDC_SAVE, "Save");
        saveBtn->SetMinSize(wxSize(80, 28));
        saveBtn->SetColours(FromColorref(theme_.success), FromColorref(theme_.bg));
        btnRow->Add(saveBtn, 0, wxRIGHT, 8);
        auto *cancelBtn = new ColorButton(this, IDC_CANCEL, "Cancel");
        cancelBtn->SetMinSize(wxSize(80, 28));
        cancelBtn->SetColours(surface, text);
        btnRow->Add(cancelBtn, 0);
        root->Add(btnRow, 0, wxEXPAND | wxALL, 16);

        SetSizer(root);

        Bind(wxEVT_BUTTON, &SettingsDialog::OnBrowse, this, IDC_BROWSE);
        Bind(wxEVT_BUTTON, &SettingsDialog::OnSave, this, IDC_SAVE);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); }, IDC_CANCEL);
    }

    ~SettingsDialog() override { hr_settings_destroy(settings_); }

private:
    void OnBrowse(wxCommandEvent &) {
        wxDirDialog dlg(this, "Select output folder", folder_edit_->GetValue());
        if (dlg.ShowModal() == wxID_OK) folder_edit_->SetValue(dlg.GetPath());
    }

    void OnSave(wxCommandEvent &) {
        state_.output_folder = folder_edit_->GetValue().ToUTF8().data();
        hr_settings_set_output_folder(settings_, state_.output_folder.c_str());

        state_.quality = quality_slider_->GetValue();
        hr_settings_set_quality(settings_, state_.quality);

        state_.target_fps = fps_spin_->GetValue();
        hr_settings_set_fps(settings_, state_.target_fps);

        state_.monitor_id = monitor_spin_->GetValue();
        hr_settings_set_monitor(settings_, state_.monitor_id);

        state_.countdown_enabled = countdown_chk_->GetValue();
        state_.timestamp_enabled = timestamp_chk_->GetValue();
        state_.cursor_enabled = cursor_chk_->GetValue();
        state_.show_summary = notify_chk_->GetValue();
        hr_settings_set_flag(settings_, "countdown", state_.countdown_enabled ? 1 : 0);
        hr_settings_set_flag(settings_, "timestamp", state_.timestamp_enabled ? 1 : 0);
        hr_settings_set_flag(settings_, "cursor", state_.cursor_enabled ? 1 : 0);
        hr_settings_set_flag(settings_, "show_summary", state_.show_summary ? 1 : 0);

        hr_settings_save(settings_, kSettingsPath);
        EndModal(wxID_OK);
    }

    AppState &state_;
    ThemeColors theme_;
    void *settings_ = nullptr;

    ColorSlider *quality_slider_ = nullptr;
    wxSpinCtrl *fps_spin_ = nullptr, *monitor_spin_ = nullptr;
    wxTextCtrl *folder_edit_ = nullptr;
    wxCheckBox *countdown_chk_ = nullptr, *timestamp_chk_ = nullptr, *cursor_chk_ = nullptr, *notify_chk_ = nullptr;
};
} // namespace

bool ShowSettingsDialog(wxWindow *parent, AppState &state, const ThemeColors &theme) {
    SettingsDialog dlg(parent, state, theme);
    return dlg.ShowModal() == wxID_OK;
}
