// settings_dialog.cpp — tabbed rewrite.
//
// Was a single flat page. Split into a wxNotebook so each settings group
// gets its own tab (General / Video & Codec / Audio / Hotkeys / Advanced),
// and the fields that used to live in the separate raw-Win32 "Advanced
// Settings" dialog (advanced_settings_dialog.cpp) are folded in here as
// tabs instead — that dialog is retired, since duplicating a whole second
// themed-vs-unthemed settings surface was the bigger inconsistency to fix.
//
// Persistence note carried over from advanced_settings_dialog.h's own
// documented gap: hr_settings.cpp's on-disk format only has fields for
// output_folder/quality/fps/monitor/codec/audio-enabled/countdown/
// timestamp/cursor/show_summary/theme/language/minimize_tray/
// always_on_top/performance/dxgi. Everything on the Video & Codec (besides
// codec itself)/Audio/Hotkeys/Advanced tabs updates AppState in memory for
// the current run but is not yet written to homrec_settings.json — that
// needs hr_settings.cpp's struct + JSON reader/writer extended with the
// extra fields, which is a separate, mechanical change to a core file
// rather than something to silently paper over here. The Advanced tab
// says so directly.
#include "settings_dialog.h"
#include "themed_widgets.h"
#include <wx/notebook.h>
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
    void hr_settings_set_codec(void *h, const char *v);
    void hr_settings_set_flag(void *h, const char *name, int v);
}

namespace {
constexpr char kSettingsPath[] = "homrec_settings.json"; // relative to app root, matches constants.py's SETTINGS_PATH

enum { IDC_QUALITY = 3001, IDC_BROWSE = 3002, IDC_SAVE = 3003, IDC_CANCEL = 3004 };

// Shared two-column-grid helpers, reused across tab pages (the old single
// page used local lambdas for this; those can't easily be shared between
// several Build*Tab() methods, so these are free functions instead).
wxStaticText *AddLabel(wxWindow *page, wxFlexGridSizer *grid, wxColour text, wxColour bg, const wxString &s) {
    auto *lbl = new wxStaticText(page, wxID_ANY, s);
    lbl->SetForegroundColour(text);
    lbl->SetBackgroundColour(bg);
    grid->Add(lbl, 0, wxALIGN_CENTRE_VERTICAL);
    return lbl;
}

wxCheckBox *AddCheck(wxWindow *page, wxSizer *sizer, wxColour text, wxColour bg, const wxString &label, bool value) {
    auto *chk = new wxCheckBox(page, wxID_ANY, label);
    chk->SetValue(value);
    chk->SetForegroundColour(text);
    chk->SetBackgroundColour(bg);
    sizer->Add(chk, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);
    return chk;
}

class SettingsDialog : public wxDialog {
public:
    SettingsDialog(wxWindow *parent, AppState &state, const ThemeColors &theme)
        : wxDialog(parent, wxID_ANY, "Settings", wxDefaultPosition, wxSize(540, 560),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          state_(state), theme_(theme) {
        settings_ = hr_settings_create();
        hr_settings_load(settings_, kSettingsPath);

        wxColour bg = FromColorref(theme_.bg);
        wxColour surface = FromColorref(theme_.surface);
        wxColour text = FromColorref(theme_.text);
        wxColour textDim = FromColorref(theme_.text_secondary);
        wxColour accent = FromColorref(theme_.accent);
        SetBackgroundColour(bg);

        auto *root = new wxBoxSizer(wxVERTICAL);

        notebook_ = new wxNotebook(this, wxID_ANY);
        notebook_->SetBackgroundColour(bg);

        BuildGeneralTab(bg, surface, accent, text);
        BuildVideoTab(bg, text);
        BuildAudioTab(bg, text);
        BuildHotkeysTab(bg, text);
        BuildAdvancedTab(bg, text, textDim);

        root->Add(notebook_, 1, wxEXPAND | wxALL, 12);

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

    // Lets "Advanced Settings..." open this same dialog focused on a
    // specific tab instead of always landing on General.
    void SelectTab(int index) { if (notebook_ && index >= 0 && index < (int)notebook_->GetPageCount()) notebook_->SetSelection(index); }

private:
    void BuildGeneralTab(wxColour bg, wxColour surface, wxColour accent, wxColour text) {
        auto *page = new wxPanel(notebook_);
        page->SetBackgroundColour(bg);
        auto *pageRoot = new wxBoxSizer(wxVERTICAL);
        auto *grid = new wxFlexGridSizer(2, 10, 10);
        grid->AddGrowableCol(1, 1);

        AddLabel(page, grid, text, bg, "Quality:");
        quality_slider_ = new ColorSlider(page, IDC_QUALITY, state_.quality, 0, 100);
        quality_slider_->SetTheme(surface, accent, text);
        grid->Add(quality_slider_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Target FPS:");
        fps_spin_ = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                    wxSP_ARROW_KEYS, 1, 240, state_.target_fps);
        grid->Add(fps_spin_, 0, wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Monitor:");
        monitor_spin_ = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                        wxSP_ARROW_KEYS, 0, 15, state_.monitor_id);
        grid->Add(monitor_spin_, 0, wxALIGN_CENTRE_VERTICAL);

        pageRoot->Add(grid, 0, wxEXPAND | wxALL, 16);

        auto *folderLbl = new wxStaticText(page, wxID_ANY, "Output folder:");
        folderLbl->SetForegroundColour(text);
        folderLbl->SetBackgroundColour(bg);
        pageRoot->Add(folderLbl, 0, wxLEFT | wxRIGHT, 16);

        auto *folderRow = new wxBoxSizer(wxHORIZONTAL);
        folder_edit_ = new wxTextCtrl(page, wxID_ANY, wxString::FromUTF8(state_.output_folder));
        folderRow->Add(folder_edit_, 1, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
        auto *browseBtn = new ColorButton(page, IDC_BROWSE, "Browse");
        browseBtn->SetMinSize(wxSize(70, 26));
        browseBtn->SetColours(surface, text);
        folderRow->Add(browseBtn, 0);
        pageRoot->Add(folderRow, 0, wxEXPAND | wxALL, 16);

        countdown_chk_ = AddCheck(page, pageRoot, text, bg, "Countdown (3s)", state_.countdown_enabled);
        timestamp_chk_ = AddCheck(page, pageRoot, text, bg, "Timestamp", state_.timestamp_enabled);
        cursor_chk_    = AddCheck(page, pageRoot, text, bg, "Cursor", state_.cursor_enabled);
        notify_chk_    = AddCheck(page, pageRoot, text, bg, "Show summary", state_.show_summary);

        pageRoot->AddStretchSpacer(1);
        page->SetSizer(pageRoot);
        notebook_->AddPage(page, "General");
    }

    void BuildVideoTab(wxColour bg, wxColour text) {
        auto *page = new wxPanel(notebook_);
        page->SetBackgroundColour(bg);
        auto *pageRoot = new wxBoxSizer(wxVERTICAL);
        auto *grid = new wxFlexGridSizer(2, 10, 10);
        grid->AddGrowableCol(1, 1);

        AddLabel(page, grid, text, bg, "Video codec:");
        codec_edit_ = new wxTextCtrl(page, wxID_ANY, wxString::FromUTF8(state_.video_codec));
        grid->Add(codec_edit_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "HW accel:");
        hwaccel_edit_ = new wxTextCtrl(page, wxID_ANY, wxString::FromUTF8(state_.hw_accel));
        grid->Add(hwaccel_edit_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Encoder preset:");
        preset_edit_ = new wxTextCtrl(page, wxID_ANY, wxString::FromUTF8(state_.enc_preset));
        grid->Add(preset_edit_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "CRF (0-51, lower = better quality):");
        crf_spin_ = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                    wxSP_ARROW_KEYS, 0, 51, state_.enc_crf);
        grid->Add(crf_spin_, 0, wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Pixel format:");
        pixfmt_edit_ = new wxTextCtrl(page, wxID_ANY, wxString::FromUTF8(state_.pix_fmt));
        grid->Add(pixfmt_edit_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        pageRoot->Add(grid, 0, wxEXPAND | wxALL, 16);

        auto *argsLbl = new wxStaticText(page, wxID_ANY, "Custom FFmpeg args:");
        argsLbl->SetForegroundColour(text);
        argsLbl->SetBackgroundColour(bg);
        pageRoot->Add(argsLbl, 0, wxLEFT | wxRIGHT, 16);
        custom_args_edit_ = new wxTextCtrl(page, wxID_ANY, wxString::FromUTF8(state_.custom_ffmpeg_args));
        pageRoot->Add(custom_args_edit_, 0, wxEXPAND | wxALL, 16);

        pageRoot->AddStretchSpacer(1);
        page->SetSizer(pageRoot);
        notebook_->AddPage(page, "Video / Codec");
    }

    void BuildAudioTab(wxColour bg, wxColour text) {
        auto *page = new wxPanel(notebook_);
        page->SetBackgroundColour(bg);
        auto *pageRoot = new wxBoxSizer(wxVERTICAL);
        auto *grid = new wxFlexGridSizer(2, 10, 10);
        grid->AddGrowableCol(1, 1);

        AddLabel(page, grid, text, bg, "Sample rate (Hz):");
        sample_rate_spin_ = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                            wxSP_ARROW_KEYS, 8000, 192000, state_.audio_sample_rate);
        grid->Add(sample_rate_spin_, 0, wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "AAC bitrate:");
        aac_bitrate_edit_ = new wxTextCtrl(page, wxID_ANY, wxString::FromUTF8(state_.audio_aac_bitrate));
        grid->Add(aac_bitrate_edit_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Channels:");
        channels_spin_ = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                         wxSP_ARROW_KEYS, 1, 8, state_.audio_out_channels);
        grid->Add(channels_spin_, 0, wxALIGN_CENTRE_VERTICAL);

        pageRoot->Add(grid, 0, wxEXPAND | wxALL, 16);

        separate_mp3_chk_ = AddCheck(page, pageRoot, text, bg, "Also save audio as a separate MP3", state_.separate_audio_mp3);

        pageRoot->AddStretchSpacer(1);
        page->SetSizer(pageRoot);
        notebook_->AddPage(page, "Audio");
    }

    void BuildHotkeysTab(wxColour bg, wxColour text) {
        auto *page = new wxPanel(notebook_);
        page->SetBackgroundColour(bg);
        auto *pageRoot = new wxBoxSizer(wxVERTICAL);
        auto *grid = new wxFlexGridSizer(2, 10, 10);
        grid->AddGrowableCol(1, 1);

        AddLabel(page, grid, text, bg, "Start/Stop:");
        hk_startstop_edit_ = new wxTextCtrl(page, wxID_ANY, wxString::FromUTF8(state_.hotkey_start_stop));
        grid->Add(hk_startstop_edit_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Pause:");
        hk_pause_edit_ = new wxTextCtrl(page, wxID_ANY, wxString::FromUTF8(state_.hotkey_pause));
        grid->Add(hk_pause_edit_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Fullscreen:");
        hk_fullscreen_edit_ = new wxTextCtrl(page, wxID_ANY, wxString::FromUTF8(state_.hotkey_fullscreen));
        grid->Add(hk_fullscreen_edit_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        pageRoot->Add(grid, 0, wxEXPAND | wxALL, 16);
        pageRoot->AddStretchSpacer(1);
        page->SetSizer(pageRoot);
        notebook_->AddPage(page, "Hotkeys");
    }

    void BuildAdvancedTab(wxColour bg, wxColour text, wxColour textDim) {
        auto *page = new wxPanel(notebook_);
        page->SetBackgroundColour(bg);
        auto *pageRoot = new wxBoxSizer(wxVERTICAL);
        auto *grid = new wxFlexGridSizer(2, 10, 10);
        grid->AddGrowableCol(1, 1);

        AddLabel(page, grid, text, bg, "Filename template:");
        fname_template_edit_ = new wxTextCtrl(page, wxID_ANY, wxString::FromUTF8(state_.filename_template));
        grid->Add(fname_template_edit_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Auto-stop (min, 0 = off):");
        autostop_spin_ = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                         wxSP_ARROW_KEYS, 0, 1440, state_.auto_stop_min);
        grid->Add(autostop_spin_, 0, wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Replay buffer (sec, 0 = off):");
        replay_buf_spin_ = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                           wxSP_ARROW_KEYS, 0, 3600, state_.replay_buffer_sec);
        grid->Add(replay_buf_spin_, 0, wxALIGN_CENTRE_VERTICAL);

        pageRoot->Add(grid, 0, wxEXPAND | wxALL, 16);

        auto *note = new wxStaticText(page, wxID_ANY,
            "Note: this tab, Video/Codec (besides Video codec), Audio, and\n"
            "Hotkeys apply for this session but aren't written to disk yet —\n"
            "the settings file only persists output folder, quality, FPS,\n"
            "monitor, video codec, and the checkboxes on General.");
        note->SetForegroundColour(textDim);
        note->SetBackgroundColour(bg);
        pageRoot->Add(note, 0, wxALL, 16);

        pageRoot->AddStretchSpacer(1);
        page->SetSizer(pageRoot);
        notebook_->AddPage(page, "Advanced");
    }

    void OnBrowse(wxCommandEvent &) {
        wxDirDialog dlg(this, "Select output folder", folder_edit_->GetValue());
        if (dlg.ShowModal() == wxID_OK) folder_edit_->SetValue(dlg.GetPath());
    }

    void OnSave(wxCommandEvent &) {
        // -- General (persisted to disk via hr_settings_*) ------------------
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

        // -- Video / Codec (video_codec persists; the rest is in-memory
        // only for now — see the Advanced tab's note) -----------------------
        state_.video_codec = codec_edit_->GetValue().ToUTF8().data();
        hr_settings_set_codec(settings_, state_.video_codec.c_str());
        state_.hw_accel = hwaccel_edit_->GetValue().ToUTF8().data();
        state_.enc_preset = preset_edit_->GetValue().ToUTF8().data();
        state_.enc_crf = crf_spin_->GetValue();
        state_.pix_fmt = pixfmt_edit_->GetValue().ToUTF8().data();
        state_.custom_ffmpeg_args = custom_args_edit_->GetValue().ToUTF8().data();

        // -- Audio ------------------------------------------------------------
        state_.audio_sample_rate = sample_rate_spin_->GetValue();
        state_.audio_aac_bitrate = aac_bitrate_edit_->GetValue().ToUTF8().data();
        state_.audio_out_channels = channels_spin_->GetValue();
        state_.separate_audio_mp3 = separate_mp3_chk_->GetValue();

        // -- Hotkeys ------------------------------------------------------------
        state_.hotkey_start_stop = hk_startstop_edit_->GetValue().ToUTF8().data();
        state_.hotkey_pause = hk_pause_edit_->GetValue().ToUTF8().data();
        state_.hotkey_fullscreen = hk_fullscreen_edit_->GetValue().ToUTF8().data();

        // -- Advanced ------------------------------------------------------------
        state_.filename_template = fname_template_edit_->GetValue().ToUTF8().data();
        state_.auto_stop_min = autostop_spin_->GetValue();
        state_.replay_buffer_sec = replay_buf_spin_->GetValue();

        hr_settings_save(settings_, kSettingsPath);
        EndModal(wxID_OK);
    }

    AppState &state_;
    ThemeColors theme_;
    void *settings_ = nullptr;
    wxNotebook *notebook_ = nullptr;

    // General
    ColorSlider *quality_slider_ = nullptr;
    wxSpinCtrl *fps_spin_ = nullptr, *monitor_spin_ = nullptr;
    wxTextCtrl *folder_edit_ = nullptr;
    wxCheckBox *countdown_chk_ = nullptr, *timestamp_chk_ = nullptr, *cursor_chk_ = nullptr, *notify_chk_ = nullptr;

    // Video / Codec
    wxTextCtrl *codec_edit_ = nullptr, *hwaccel_edit_ = nullptr, *preset_edit_ = nullptr,
               *pixfmt_edit_ = nullptr, *custom_args_edit_ = nullptr;
    wxSpinCtrl *crf_spin_ = nullptr;

    // Audio
    wxSpinCtrl *sample_rate_spin_ = nullptr, *channels_spin_ = nullptr;
    wxTextCtrl *aac_bitrate_edit_ = nullptr;
    wxCheckBox *separate_mp3_chk_ = nullptr;

    // Hotkeys
    wxTextCtrl *hk_startstop_edit_ = nullptr, *hk_pause_edit_ = nullptr, *hk_fullscreen_edit_ = nullptr;

    // Advanced
    wxTextCtrl *fname_template_edit_ = nullptr;
    wxSpinCtrl *autostop_spin_ = nullptr, *replay_buf_spin_ = nullptr;
};
} // namespace

bool ShowSettingsDialog(wxWindow *parent, AppState &state, const ThemeColors &theme) {
    SettingsDialog dlg(parent, state, theme);
    return dlg.ShowModal() == wxID_OK;
}

bool ShowSettingsDialogTab(wxWindow *parent, AppState &state, const ThemeColors &theme, int tab_index) {
    SettingsDialog dlg(parent, state, theme);
    dlg.SelectTab(tab_index);
    return dlg.ShowModal() == wxID_OK;
}
