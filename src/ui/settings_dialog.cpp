// settings_dialog.cpp - tabbed rewrite.
//
// Was a single scrollable page: everyday fields always visible, and
// everything a casual user wouldn't need (codec/CRF/hw-accel/custom
// ffmpeg args/audio format/filename template/auto-stop/replay buffer)
// hidden behind one "Advanced" toggle button. That worked for 5 tabs'
// worth of fields squeezed into one show/hide panel, but stopped
// scaling once Security (logging toggles, live-preview resolution/fps,
// "reset to defaults") and System (desktop shortcut, autostart, tray
// icon) were added on top - cramming 7 tabs' worth of fields into one
// collapsible panel would have made "Advanced" itself need sub-sections
// again. Back to a wxNotebook, but with a cleaner split than the old
// 5-tab version had: General / Video & Codec / Audio / Hotkeys /
// Advanced / Security / System.
//
// Persistence note (Phase 1 settings-storage migration, see commands.md):
// this dialog used to save through hr_settings.cpp's on-disk
// homrec_settings.json, whose fixed field whitelist didn't cover
// hw_accel/enc_preset/enc_crf/pix_fmt/custom_ffmpeg_args/audio_*/
// filename_template/auto_stop_min/replay_buffer_sec/hotkeys - those
// applied for the current run and round-tripped through .hrc profiles
// (hrc_config.cpp) but reverted on next launch if you hadn't also
// exported an .hrc. OnSave() now persists through HrcConfig::Save()
// instead, which writes the *entire* AppState - every field on every
// tab is saved for real now, and the on-page notes below reflect that.
#include "settings_dialog.h"
#include "themed_widgets.h"
#include "language.h"
#include "../hr_mic_enum.h"
#include "../hr_system_integration.h"
#include "../hr_pc_log.h"
#include "../hr_plugin_log.h"
#include "hrc_config.h"
#include <wx/spinctrl.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/combobox.h>
#include <wx/choice.h>
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

extern "C" {
    // Only the "read pure defaults" accessors remain in use here
    // (OnResetDefaults() below) - actual persistence now goes through
    // HrcConfig::Save()/Load() (hrc_config.h), see this file's header
    // comment and OnSave().
    void *hr_settings_create();
    void hr_settings_destroy(void *handle);
    int hr_settings_get_flag(const void *h, const char *name);
    int hr_settings_get_quality(const void *h);
    int hr_settings_get_fps(const void *h);
    int hr_settings_get_resolution_pct(const void *h);
    int hr_settings_get_resolution_mode(const void *h);
    const char *hr_settings_get_codec(const void *h);

    // hr_display_info.cpp - used to list real connected monitors (with
    // their actual resolution) instead of making the user guess a plain
    // index number in a spin control.
    void *hr_di_create();
    void hr_di_destroy(void *handle);
    void hr_di_refresh(void *handle);
    int hr_di_count(void *handle);
    int hr_di_get(void *handle, int index, int *out_x, int *out_y,
                   int *out_w, int *out_h, float *out_dpi);
}

namespace {
// Phase 1 settings-storage migration (see commands.md): this dialog now
// persists via HrcConfig::Save() (hrc_config.h) instead of the old JSON
// engine writing to a fixed "homrec_settings.json" path - see OnSave().

enum {
    IDC_QUALITY = 3001, IDC_BROWSE = 3002, IDC_SAVE = 3003, IDC_CANCEL = 3004,
    IDC_SEC_RESET = 3006, IDC_SYS_SHORTCUT_BROWSE = 3007,
    IDC_SETTINGS_PATH_BROWSE = 3008, IDC_LANG_ADD = 3009,
};

// Same folder LanguageTable::Load()/ScanCustomLanguages() read from
// (main_frame.cpp's ctor uses this literal too) - kept as one constant
// here since this dialog is now also the thing that writes into it.
const char *const kLangsDir = "Assets\\L";

// Old tab indices ShowSettingsDialogTab() callers still pass in (see
// settings_dialog.h) mapped onto this version's actual notebook page
// indices.
constexpr int kOldTabVideoCodec = 1;
constexpr int kOldTabAdvanced = 4;

// Shared two-column-grid helpers.
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

wxStaticText *AddSectionHeading(wxWindow *page, wxSizer *sizer, wxColour accent, wxColour bg, const wxString &s) {
    auto *lbl = new wxStaticText(page, wxID_ANY, s);
    wxFont f = lbl->GetFont();
    f.SetWeight(wxFONTWEIGHT_BOLD);
    lbl->SetFont(f);
    lbl->SetForegroundColour(accent);
    lbl->SetBackgroundColour(bg);
    sizer->Add(lbl, 0, wxLEFT | wxRIGHT | wxTOP, 16);
    return lbl;
}

// Every tab is a plain scrolled page inside the notebook - some (Video &
// Codec, Security) have enough fields to need scrolling on a small
// screen, and using the same widget for every tab keeps them visually
// consistent.
wxScrolledWindow *NewTabPage(wxNotebook *nb, wxColour bg) {
    auto *page = new wxScrolledWindow(nb);
    page->SetBackgroundColour(bg);
    page->SetScrollRate(0, 12);
    return page;
}

class SettingsDialog : public wxDialog {
public:
    SettingsDialog(wxWindow *parent, AppState &state, const ThemeColors &theme)
        : wxDialog(parent, wxID_ANY, "Settings", wxDefaultPosition, wxSize(600, 640),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          state_(state), theme_(theme) {
        wxColour bg = FromColorref(theme_.bg);
        wxColour surface = FromColorref(theme_.surface);
        wxColour text = FromColorref(theme_.text);
        wxColour textDim = FromColorref(theme_.text_secondary);
        wxColour accent = FromColorref(theme_.accent);
        SetBackgroundColour(bg);

        auto *root = new wxBoxSizer(wxVERTICAL);

        auto *titleLbl = new wxStaticText(this, wxID_ANY, "Settings");
        wxFont titleFont = titleLbl->GetFont();
        titleFont.SetPointSize(titleFont.GetPointSize() + 3);
        titleFont.SetWeight(wxFONTWEIGHT_BOLD);
        titleLbl->SetFont(titleFont);
        titleLbl->SetForegroundColour(text);
        titleLbl->SetBackgroundColour(bg);
        root->Add(titleLbl, 0, wxALL, 12);

        notebook_ = new wxNotebook(this, wxID_ANY);
        notebook_->SetBackgroundColour(bg);

        auto *generalPage = NewTabPage(notebook_, bg);
        { auto *r = new wxBoxSizer(wxVERTICAL); BuildGeneralTab(generalPage, r, bg, surface, accent, text, textDim); generalPage->SetSizer(r); }
        notebook_->AddPage(generalPage, "General");
        kTabGeneral = notebook_->GetPageCount() - 1;

        auto *videoPage = NewTabPage(notebook_, bg);
        { auto *r = new wxBoxSizer(wxVERTICAL); BuildVideoCodecTab(videoPage, r, bg, accent, text, textDim); videoPage->SetSizer(r); }
        notebook_->AddPage(videoPage, "Video && Codec");
        kTabVideo = notebook_->GetPageCount() - 1;

        auto *audioPage = NewTabPage(notebook_, bg);
        { auto *r = new wxBoxSizer(wxVERTICAL); BuildAudioTab(audioPage, r, bg, accent, text, textDim); audioPage->SetSizer(r); }
        notebook_->AddPage(audioPage, "Audio");

        auto *hotkeysPage = NewTabPage(notebook_, bg);
        { auto *r = new wxBoxSizer(wxVERTICAL); BuildHotkeysTab(hotkeysPage, r, bg, text); hotkeysPage->SetSizer(r); }
        notebook_->AddPage(hotkeysPage, "Hotkeys");

        auto *advancedPage = NewTabPage(notebook_, bg);
        { auto *r = new wxBoxSizer(wxVERTICAL); BuildAdvancedTab(advancedPage, r, bg, text, textDim); advancedPage->SetSizer(r); }
        notebook_->AddPage(advancedPage, "Advanced");
        kTabAdvanced = notebook_->GetPageCount() - 1;

        auto *securityPage = NewTabPage(notebook_, bg);
        { auto *r = new wxBoxSizer(wxVERTICAL); BuildSecurityTab(securityPage, r, bg, surface, accent, text, textDim); securityPage->SetSizer(r); }
        notebook_->AddPage(securityPage, "Security");

        auto *systemPage = NewTabPage(notebook_, bg);
        { auto *r = new wxBoxSizer(wxVERTICAL); BuildSystemTab(systemPage, r, bg, surface, accent, text, textDim); systemPage->SetSizer(r); }
        notebook_->AddPage(systemPage, "System");

        root->Add(notebook_, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

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
        Bind(wxEVT_BUTTON, &SettingsDialog::OnResetDefaults, this, IDC_SEC_RESET);
        Bind(wxEVT_BUTTON, &SettingsDialog::OnBrowseShortcutFolder, this, IDC_SYS_SHORTCUT_BROWSE);
        Bind(wxEVT_BUTTON, &SettingsDialog::OnBrowseSettingsPath, this, IDC_SETTINGS_PATH_BROWSE);
        Bind(wxEVT_BUTTON, &SettingsDialog::OnAddLanguage, this, IDC_LANG_ADD);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); }, IDC_CANCEL);
    }

    ~SettingsDialog() override {}

    // Old call sites (the "Advanced Settings..." menu item) used to pick
    // a page by index in the old 5-tab layout; map those onto this
    // version's actual page indices.
    void SelectTab(int index) {
        if (index == kOldTabVideoCodec) notebook_->SetSelection((int)kTabVideo);
        else if (index == kOldTabAdvanced) notebook_->SetSelection((int)kTabAdvanced);
    }

private:
    // Enumerates actually-connected monitors (via hr_display_info.cpp) so
    // the dropdown shows "Monitor 1 - 1920x1080 (Primary)" instead of
    // making the user guess an opaque index 0-15 in a spin control.
    // Falls back to a plain numbered list if enumeration fails for any
    // reason (e.g. running headless), so the control is never empty.
    void PopulateMonitorChoice() {
        monitor_choice_->Clear();
        void *di = hr_di_create();
        int count = 0;
        if (di) {
            hr_di_refresh(di);
            count = hr_di_count(di);
        }
        if (count > 0) {
            for (int i = 0; i < count; ++i) {
                int x = 0, y = 0, w = 0, h = 0; float dpi = 96.0f;
                hr_di_get(di, i, &x, &y, &w, &h, &dpi);
                wxString label = wxString::Format("Monitor %d \u2014 %dx%d", i + 1, w, h);
                if (i == 0) label += " (Primary)";
                monitor_choice_->Append(label);
            }
        } else {
            for (int i = 0; i < 4; ++i)
                monitor_choice_->Append(wxString::Format("Monitor %d", i + 1));
        }
        if (di) hr_di_destroy(di);

        int sel = state_.monitor_id > 0 ? state_.monitor_id - 1 : 0;
        if (sel < 0 || sel >= (int)monitor_choice_->GetCount()) sel = 0;
        monitor_choice_->SetSelection(sel);
    }

    // -- General: output location, capture quality/resolution, on-screen
    // capture options ------------------------------------------------------
    void BuildGeneralTab(wxWindow *page, wxSizer *pageRoot, wxColour bg, wxColour surface,
                          wxColour accent, wxColour text, wxColour /*textDim*/) {
        auto *grid = new wxFlexGridSizer(2, 10, 10);
        grid->AddGrowableCol(1, 1);

        AddLabel(page, grid, text, bg, "Encoding quality:");
        quality_slider_ = new LabeledSlider(page, IDC_QUALITY, state_.quality, 0, 100);
        quality_slider_->SetTheme(surface, accent, text, surface, text);
        grid->Add(quality_slider_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Target FPS (limit):");
        fps_spin_ = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                    wxSP_ARROW_KEYS, 1, 240, state_.target_fps);
        grid->Add(fps_spin_, 0, wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Monitor:");
        monitor_choice_ = new wxChoice(page, wxID_ANY);
        PopulateMonitorChoice();
        grid->Add(monitor_choice_, 0, wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Resolution mode:");
        resolution_mode_choice_ = new wxChoice(page, wxID_ANY);
        resolution_mode_choice_->Append("Percent of native");
        resolution_mode_choice_->Append("Custom (width x height)");
        resolution_mode_choice_->SetSelection(
            state_.resolution_mode == ResolutionMode::Absolute ? 1 : 0);
        grid->Add(resolution_mode_choice_, 0, wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Resolution:");
        auto *resRow = new wxBoxSizer(wxHORIZONTAL);
        resolution_choice_ = new wxChoice(page, wxID_ANY);
        resolution_choice_->Append("Full screen (Native, 100%)");
        resolution_choice_->Append("75%");
        resolution_choice_->Append("50%");
        resolution_choice_->Append("25%");
        {
            int pct = (int)std::lround(state_.scale_factor * 100.0);
            int sel = pct >= 100 ? 0 : pct >= 75 ? 1 : pct >= 50 ? 2 : 3;
            resolution_choice_->SetSelection(sel);
        }
        resRow->Add(resolution_choice_, 0, wxALIGN_CENTRE_VERTICAL);

        resolution_w_spin_ = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                             wxSize(80, -1), wxSP_ARROW_KEYS, 2, 7680,
                                             state_.resolution_w);
        resRow->Add(resolution_w_spin_, 0, wxALIGN_CENTRE_VERTICAL | wxLEFT, 6);
        auto *xLbl = new wxStaticText(page, wxID_ANY, "x");
        xLbl->SetForegroundColour(text);
        xLbl->SetBackgroundColour(bg);
        resRow->Add(xLbl, 0, wxALIGN_CENTRE_VERTICAL | wxLEFT | wxRIGHT, 4);
        resolution_h_spin_ = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                             wxSize(80, -1), wxSP_ARROW_KEYS, 2, 4320,
                                             state_.resolution_h);
        resRow->Add(resolution_h_spin_, 0, wxALIGN_CENTRE_VERTICAL);
        grid->Add(resRow, 0, wxALIGN_CENTRE_VERTICAL);

        auto updateResEnabled = [this]() {
            bool custom = resolution_mode_choice_->GetSelection() == 1;
            resolution_choice_->Enable(!custom);
            resolution_w_spin_->Enable(custom);
            resolution_h_spin_->Enable(custom);
        };
        updateResEnabled();
        resolution_mode_choice_->Bind(wxEVT_CHOICE, [updateResEnabled](wxCommandEvent &) {
            updateResEnabled();
        });

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

        auto *langLbl = new wxStaticText(page, wxID_ANY, "Language:");
        langLbl->SetForegroundColour(text);
        langLbl->SetBackgroundColour(bg);
        pageRoot->Add(langLbl, 0, wxLEFT | wxRIGHT | wxTOP, 16);

        auto *langRow = new wxBoxSizer(wxHORIZONTAL);
        lang_choice_ = new wxChoice(page, wxID_ANY);
        RefreshLanguageChoices();
        langRow->Add(lang_choice_, 1, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
        auto *langAddBtn = new ColorButton(page, IDC_LANG_ADD, "Add Language...");
        langAddBtn->SetMinSize(wxSize(120, 26));
        langAddBtn->SetColours(surface, text);
        langRow->Add(langAddBtn, 0);
        pageRoot->Add(langRow, 0, wxEXPAND | wxALL, 16);

        countdown_chk_ = AddCheck(page, pageRoot, text, bg, "Countdown (3s)", state_.countdown_enabled);
        timestamp_chk_ = AddCheck(page, pageRoot, text, bg, "Timestamp", state_.timestamp_enabled);
        cursor_chk_    = AddCheck(page, pageRoot, text, bg, "Cursor", state_.cursor_enabled);
        notify_chk_    = AddCheck(page, pageRoot, text, bg, "Show summary", state_.show_summary);
    }

    // Repopulates lang_choice_/lang_codes_ from the built-in "English"
    // entry plus whatever *.hrl files ScanCustomLanguages() finds in
    // kLangsDir right now - called on dialog build, and again after
    // OnAddLanguage() successfully imports a new one so it shows up
    // without having to close and reopen Settings. Tries to keep
    // whichever code was selected before (falling back to
    // state_.current_language the first time, then to "en" if that
    // code isn't in the list at all, e.g. it was deleted from disk).
    void RefreshLanguageChoices() {
        std::string keep = lang_codes_.empty() ? state_.current_language
                            : (lang_choice_->GetSelection() >= 0 &&
                               (size_t)lang_choice_->GetSelection() < lang_codes_.size())
                                  ? lang_codes_[(size_t)lang_choice_->GetSelection()]
                                  : state_.current_language;

        lang_choice_->Clear();
        lang_codes_.clear();

        lang_codes_.push_back("en");
        lang_choice_->Append("English");

        for (const auto &entry : LanguageTable::ScanCustomLanguages(kLangsDir)) {
            lang_codes_.push_back(entry.first);
            lang_choice_->Append(wxString::FromUTF8(entry.second));
        }

        auto it = std::find(lang_codes_.begin(), lang_codes_.end(), keep);
        int sel = (it != lang_codes_.end()) ? (int)std::distance(lang_codes_.begin(), it) : 0;
        lang_choice_->SetSelection(sel);
    }

    void OnAddLanguage(wxCommandEvent &) {
        wxFileDialog dlg(this, "Select a HomRec language file", "", "",
                          "HomRec Language (*.hrl)|*.hrl|All files (*.*)|*.*",
                          wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK) return;

        std::string code, displayName, error;
        if (!LanguageTable::ImportLanguageFile(dlg.GetPath().ToUTF8().data(), kLangsDir,
                                                code, displayName, error)) {
            wxMessageBox(wxString::FromUTF8(error), "Add Language", wxOK | wxICON_ERROR, this);
            return;
        }

        RefreshLanguageChoices();
        auto it = std::find(lang_codes_.begin(), lang_codes_.end(), code);
        if (it != lang_codes_.end())
            lang_choice_->SetSelection((int)std::distance(lang_codes_.begin(), it));

        wxMessageBox("Added \"" + wxString::FromUTF8(displayName) + "\" - select Save to apply it.",
                     "Add Language", wxOK | wxICON_INFORMATION, this);
    }

    // -- Video & Codec ------------------------------------------------------
    void BuildVideoCodecTab(wxWindow *page, wxSizer *pageRoot, wxColour bg,
                             wxColour accent, wxColour text, wxColour textDim) {
        AddSectionHeading(page, pageRoot, accent, bg, "Video & Codec");
        auto *grid = new wxFlexGridSizer(2, 10, 10);
        grid->AddGrowableCol(1, 1);

        AddLabel(page, grid, text, bg, "Video codec:");
        codec_combo_ = new wxComboBox(page, wxID_ANY, wxString::FromUTF8(state_.video_codec),
                                       wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_DROPDOWN);
        for (const auto &c : {"libx264", "libx265", "h264_nvenc", "hevc_nvenc",
                               "h264_qsv", "hevc_qsv", "h264_amf", "hevc_amf", "mpeg4"})
            codec_combo_->Append(c);
        grid->Add(codec_combo_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "HW accel:");
        hwaccel_combo_ = new wxComboBox(page, wxID_ANY, wxString::FromUTF8(state_.hw_accel),
                                         wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_DROPDOWN);
        for (const auto &c : {"none", "nvenc", "qsv", "amf", "dxva2", "d3d11va", "cuda"})
            hwaccel_combo_->Append(c);
        grid->Add(hwaccel_combo_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Encoder preset:");
        preset_combo_ = new wxComboBox(page, wxID_ANY, wxString::FromUTF8(state_.enc_preset),
                                        wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_DROPDOWN);
        for (const auto &c : {"ultrafast", "superfast", "veryfast", "faster", "fast",
                               "medium", "slow", "slower", "veryslow"})
            preset_combo_->Append(c);
        grid->Add(preset_combo_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "CRF (0-51, lower = better quality):");
        crf_spin_ = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                    wxSP_ARROW_KEYS, 0, 51, state_.enc_crf);
        grid->Add(crf_spin_, 0, wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Pixel format:");
        pixfmt_combo_ = new wxComboBox(page, wxID_ANY, wxString::FromUTF8(state_.pix_fmt),
                                        wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_DROPDOWN);
        for (const auto &c : {"yuv420p", "yuv444p", "yuv420p10le", "nv12", "p010le"})
            pixfmt_combo_->Append(c);
        grid->Add(pixfmt_combo_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        pageRoot->Add(grid, 0, wxEXPAND | wxALL, 16);

        auto *argsLbl = new wxStaticText(page, wxID_ANY, "Custom FFmpeg args:");
        argsLbl->SetForegroundColour(text);
        argsLbl->SetBackgroundColour(bg);
        pageRoot->Add(argsLbl, 0, wxLEFT | wxRIGHT, 16);
        custom_args_edit_ = new wxTextCtrl(page, wxID_ANY, wxString::FromUTF8(state_.custom_ffmpeg_args));
        pageRoot->Add(custom_args_edit_, 0, wxEXPAND | wxALL, 16);

        auto *note = new wxStaticText(page, wxID_ANY,
            "Note: only Video codec (above) is written to the settings file -\n"
            "the rest of this tab applies for this session, and round-trips\n"
            "through .hrc profiles, but resets to defaults on the next launch.");
        note->SetForegroundColour(textDim);
        note->SetBackgroundColour(bg);
        pageRoot->Add(note, 0, wxALL, 16);
    }

    // -- Audio ----------------------------------------------------------
    void BuildAudioTab(wxWindow *page, wxSizer *pageRoot, wxColour bg,
                        wxColour accent, wxColour text, wxColour textDim) {
        auto *grid = new wxFlexGridSizer(2, 10, 10);
        grid->AddGrowableCol(1, 1);

        AddLabel(page, grid, text, bg, "Microphone:");
        mic_choice_ = new wxChoice(page, wxID_ANY);
        mic_ids_.clear();
        mic_ids_.push_back(""); // "System Default" -- empty id means "keep using Windows' default"
        mic_choice_->Append("System Default");
        int mic_sel = 0;
        for (const auto &mic : HrEnumerateMics()) {
            mic_ids_.push_back(mic.id);
            mic_choice_->Append(wxString::FromUTF8(mic.name));
            if (mic.id == state_.mic_device_id) mic_sel = (int)mic_ids_.size() - 1;
        }
        mic_choice_->SetSelection(mic_sel);
        grid->Add(mic_choice_, 0, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

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

        separate_mp3_chk_ = AddCheck(page, pageRoot, text, bg,
                                      "Also save audio as a separate MP3", state_.separate_audio_mp3);

        auto *note = new wxStaticText(page, wxID_ANY,
            "Note: sample rate/bitrate/channels apply for this session and\n"
            "round-trip through .hrc profiles, but aren't written to the\n"
            "settings file yet.");
        note->SetForegroundColour(textDim);
        note->SetBackgroundColour(bg);
        pageRoot->Add(note, 0, wxLEFT | wxRIGHT | wxTOP, 16);
        (void)accent;
    }

    // -- Hotkeys ----------------------------------------------------------
    void BuildHotkeysTab(wxWindow *page, wxSizer *pageRoot, wxColour bg, wxColour text) {
        auto *hkGrid = new wxFlexGridSizer(2, 10, 10);
        hkGrid->AddGrowableCol(1, 1);
        AddLabel(page, hkGrid, text, bg, "Start/Stop:");
        hk_startstop_btn_ = new HotkeyButton(page, wxID_ANY, wxString::FromUTF8(state_.hotkey_start_stop));
        hkGrid->Add(hk_startstop_btn_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);
        AddLabel(page, hkGrid, text, bg, "Pause:");
        hk_pause_btn_ = new HotkeyButton(page, wxID_ANY, wxString::FromUTF8(state_.hotkey_pause));
        hkGrid->Add(hk_pause_btn_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);
        AddLabel(page, hkGrid, text, bg, "Fullscreen:");
        hk_fullscreen_btn_ = new HotkeyButton(page, wxID_ANY, wxString::FromUTF8(state_.hotkey_fullscreen));
        hkGrid->Add(hk_fullscreen_btn_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);
        for (HotkeyButton *hk : {hk_startstop_btn_, hk_pause_btn_, hk_fullscreen_btn_})
            hk->SetColours(bg, text, wxColour(120, 170, 250));
        pageRoot->Add(hkGrid, 0, wxEXPAND | wxALL, 16);
    }

    // -- Advanced: filename template, auto-stop, replay buffer ------------
    void BuildAdvancedTab(wxWindow *page, wxSizer *pageRoot, wxColour bg, wxColour text, wxColour textDim) {
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

        // -- Settings file path (Phase 1 storage migration, see
        // commands.md) - the app's own auto-managed settings file, an
        // .hrc-format file (hrc_config.cpp) that replaced the old
        // homrec_settings.json. Empty = HrcConfig::kDefaultSettingsPath
        // ("homrec.hrc" next to the exe). A custom path here is picked up
        // immediately on the next Save and read back from on the next
        // launch - it does NOT move an already-existing file for you.
        AddSectionHeading(page, pageRoot, FromColorref(theme_.accent), bg, "Settings Storage");
        auto *settingsPathLbl = new wxStaticText(page, wxID_ANY, "Settings file (.hrc):");
        settingsPathLbl->SetForegroundColour(text);
        settingsPathLbl->SetBackgroundColour(bg);
        pageRoot->Add(settingsPathLbl, 0, wxLEFT | wxRIGHT, 16);

        auto *settingsPathRow = new wxBoxSizer(wxHORIZONTAL);
        wxString curPath = state_.settings_path.empty()
            ? wxString(HrcConfig::kDefaultSettingsPath) : wxString::FromUTF8(state_.settings_path);
        settings_path_edit_ = new wxTextCtrl(page, wxID_ANY, curPath);
        settingsPathRow->Add(settings_path_edit_, 1, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
        auto *settingsPathBrowseBtn = new ColorButton(page, IDC_SETTINGS_PATH_BROWSE, "Browse");
        settingsPathBrowseBtn->SetMinSize(wxSize(70, 26));
        settingsPathBrowseBtn->SetColours(bg, text);
        settingsPathRow->Add(settingsPathBrowseBtn, 0);
        pageRoot->Add(settingsPathRow, 0, wxEXPAND | wxALL, 16);

        auto *note = new wxStaticText(page, wxID_ANY,
            "Filename template / auto-stop / replay buffer above, and every\n"
            "other tab's fields, are saved to the settings file above on Save.\n"
            "cfg/config.cfg (if present) can layer its own overrides on top of\n"
            "this file at every launch - see commands.md's \"sethrc\" command.");
        note->SetForegroundColour(textDim);
        note->SetBackgroundColour(bg);
        pageRoot->Add(note, 0, wxALL, 16);
    }

    // -- Security: logging toggles, live-preview resolution/fps, reset ----
    void BuildSecurityTab(wxWindow *page, wxSizer *pageRoot, wxColour bg, wxColour surface,
                           wxColour accent, wxColour text, wxColour textDim) {
        AddSectionHeading(page, pageRoot, accent, bg, "Logging");
        sys_log_chk_ = AddCheck(page, pageRoot, text, bg,
            "System logging (logs/pc.log)", state_.system_logging_enabled);
        plugin_log_chk_ = AddCheck(page, pageRoot, text, bg,
            "Plugin logging (logs/plugins.log)", state_.plugin_logging_enabled);

        AddSectionHeading(page, pageRoot, accent, bg, "Live Preview");
        disable_preview_chk_ = AddCheck(page, pageRoot, text, bg,
            "Disable live preview (for performance)", state_.disable_preview);

        auto *previewRow = new wxBoxSizer(wxHORIZONTAL);
        auto *pqLbl = new wxStaticText(page, wxID_ANY, "Preview resolution:");
        pqLbl->SetForegroundColour(text);
        pqLbl->SetBackgroundColour(bg);
        previewRow->Add(pqLbl, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 6);

        preview_quality_choice_ = new wxChoice(page, wxID_ANY);
        preview_quality_choice_->Append("Low (50%)");
        preview_quality_choice_->Append("Medium (75%)");
        preview_quality_choice_->Append("High (100%)");
        {
            int pct = state_.preview_quality_pct;
            int sel = pct <= 50 ? 0 : pct <= 75 ? 1 : 2;
            preview_quality_choice_->SetSelection(sel);
        }
        previewRow->Add(preview_quality_choice_, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 16);

        auto *pfLbl = new wxStaticText(page, wxID_ANY, "Preview FPS:");
        pfLbl->SetForegroundColour(text);
        pfLbl->SetBackgroundColour(bg);
        previewRow->Add(pfLbl, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 6);
        preview_fps_spin_ = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                            wxSize(70, -1), wxSP_ARROW_KEYS, 1, 60,
                                            state_.preview_fps);
        previewRow->Add(preview_fps_spin_, 0, wxALIGN_CENTRE_VERTICAL);
        pageRoot->Add(previewRow, 0, wxALL, 16);

        auto updatePreviewControlsEnabled = [this]() {
            bool enabled = !disable_preview_chk_->GetValue();
            preview_quality_choice_->Enable(enabled);
            preview_fps_spin_->Enable(enabled);
        };
        updatePreviewControlsEnabled();
        disable_preview_chk_->Bind(wxEVT_CHECKBOX, [updatePreviewControlsEnabled](wxCommandEvent &) {
            updatePreviewControlsEnabled();
        });

        AddSectionHeading(page, pageRoot, accent, bg, "Reset");
        auto *resetRow = new wxBoxSizer(wxHORIZONTAL);
        auto *resetBtn = new ColorButton(page, IDC_SEC_RESET, "Reset all settings to default");
        resetBtn->SetMinSize(wxSize(220, 28));
        resetBtn->SetColours(surface, FromColorref(theme_.error));
        resetRow->Add(resetBtn, 0);
        pageRoot->Add(resetRow, 0, wxALL, 16);

        auto *note = new wxStaticText(page, wxID_ANY,
            "Resets every field in this dialog (all tabs) back to defaults.\n"
            "Nothing is written to disk until you click Save afterward.");
        note->SetForegroundColour(textDim);
        note->SetBackgroundColour(bg);
        pageRoot->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);
    }

    // -- System: desktop shortcut, autostart, tray icon --------------------
    void BuildSystemTab(wxWindow *page, wxSizer *pageRoot, wxColour bg, wxColour surface,
                         wxColour accent, wxColour text, wxColour textDim) {
        AddSectionHeading(page, pageRoot, accent, bg, "Desktop Shortcut");
        shortcut_chk_ = AddCheck(page, pageRoot, text, bg,
            "Create a shortcut", state_.desktop_shortcut_enabled);

        auto *pathLbl = new wxStaticText(page, wxID_ANY, "Location:");
        pathLbl->SetForegroundColour(text);
        pathLbl->SetBackgroundColour(bg);
        pageRoot->Add(pathLbl, 0, wxLEFT | wxRIGHT, 16);

        auto *pathRow = new wxBoxSizer(wxHORIZONTAL);
        std::string defaultPath = state_.desktop_shortcut_path.empty()
            ? HrSystemIntegration::GetDefaultDesktopPath()
            : state_.desktop_shortcut_path;
        shortcut_path_edit_ = new wxTextCtrl(page, wxID_ANY, wxString::FromUTF8(defaultPath));
        pathRow->Add(shortcut_path_edit_, 1, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
        auto *shortcutBrowseBtn = new ColorButton(page, IDC_SYS_SHORTCUT_BROWSE, "Browse");
        shortcutBrowseBtn->SetMinSize(wxSize(70, 26));
        shortcutBrowseBtn->SetColours(surface, text);
        pathRow->Add(shortcutBrowseBtn, 0);
        pageRoot->Add(pathRow, 0, wxEXPAND | wxALL, 16);

        auto updateShortcutEnabled = [this]() {
            bool en = shortcut_chk_->GetValue();
            shortcut_path_edit_->Enable(en);
        };
        updateShortcutEnabled();
        shortcut_chk_->Bind(wxEVT_CHECKBOX, [updateShortcutEnabled](wxCommandEvent &) { updateShortcutEnabled(); });

        AddSectionHeading(page, pageRoot, accent, bg, "Startup");
        autostart_chk_ = AddCheck(page, pageRoot, text, bg,
            "Launch HomRec when Windows starts", state_.autostart_enabled);
        tray_chk_ = AddCheck(page, pageRoot, text, bg,
            "Enable tray icon (minimize to tray)", state_.minimize_to_tray);

        auto *note = new wxStaticText(page, wxID_ANY,
            "Shortcut creation/removal and the startup entry are applied as\n"
            "soon as you click Save on this dialog.");
        note->SetForegroundColour(textDim);
        note->SetBackgroundColour(bg);
        pageRoot->Add(note, 0, wxALL, 16);
    }

    void OnBrowse(wxCommandEvent &) {
        wxDirDialog dlg(this, "Select output folder", folder_edit_->GetValue());
        if (dlg.ShowModal() == wxID_OK) folder_edit_->SetValue(dlg.GetPath());
    }

    void OnBrowseShortcutFolder(wxCommandEvent &) {
        wxDirDialog dlg(this, "Select shortcut location", shortcut_path_edit_->GetValue());
        if (dlg.ShowModal() == wxID_OK) shortcut_path_edit_->SetValue(dlg.GetPath());
    }

    void OnBrowseSettingsPath(wxCommandEvent &) {
        wxFileDialog dlg(this, "Select settings file", "", settings_path_edit_->GetValue(),
                          "HomRec Config (*.hrc)|*.hrc|All files (*.*)|*.*",
                          wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() == wxID_OK) settings_path_edit_->SetValue(dlg.GetPath());
    }

    // Repopulates every control across every tab with hard-coded
    // defaults - doesn't touch state_/settings_/disk itself, so Cancel
    // (or just not clicking Save afterward) leaves everything exactly as
    // it was before Reset was clicked. Values that hr_settings.cpp
    // tracks are read from a fresh (never-loaded) HrSettings blob so
    // this can't drift out of sync with hr_settings.cpp's own
    // _defaults(); the handful of AppState-only fields hr_settings
    // doesn't cover yet (hw_accel/enc_preset/enc_crf/pix_fmt/
    // custom_ffmpeg_args/audio_*/filename_template/auto_stop_min/
    // replay_buffer_sec/hotkeys) are hard-coded here, matching their
    // app_state.h member-initializer defaults.
    void OnResetDefaults(wxCommandEvent &) {
        if (wxMessageBox(
                "Reset every setting in this dialog to its default? "
                "Nothing is written to disk until you click Save afterward.",
                "Reset Settings", wxYES_NO | wxICON_WARNING, this) != wxYES)
            return;

        void *def = hr_settings_create(); // never loaded from disk -> pure defaults

        quality_slider_->SetValue(hr_settings_get_quality(def));
        fps_spin_->SetValue(hr_settings_get_fps(def));
        monitor_choice_->SetSelection(0);
        resolution_mode_choice_->SetSelection(hr_settings_get_resolution_mode(def));
        {
            int pct = hr_settings_get_resolution_pct(def);
            int sel = pct >= 100 ? 0 : pct >= 75 ? 1 : pct >= 50 ? 2 : 3;
            resolution_choice_->SetSelection(sel);
        }
        resolution_w_spin_->SetValue(1280);
        resolution_h_spin_->SetValue(720);
        folder_edit_->SetValue("recordings");
        countdown_chk_->SetValue(hr_settings_get_flag(def, "countdown") != 0);
        timestamp_chk_->SetValue(hr_settings_get_flag(def, "timestamp") != 0);
        cursor_chk_->SetValue(hr_settings_get_flag(def, "cursor") != 0);
        notify_chk_->SetValue(hr_settings_get_flag(def, "show_summary") != 0);

        codec_combo_->SetValue(hr_settings_get_codec(def));
        hwaccel_combo_->SetValue("auto");
        preset_combo_->SetValue("ultrafast");
        crf_spin_->SetValue(18);
        pixfmt_combo_->SetValue("yuv420p");
        custom_args_edit_->SetValue("");

        mic_choice_->SetSelection(0);
        sample_rate_spin_->SetValue(44100);
        aac_bitrate_edit_->SetValue("192k");
        channels_spin_->SetValue(2);
        separate_mp3_chk_->SetValue(false);

        hk_startstop_btn_->SetValue("F9");
        hk_pause_btn_->SetValue("F10");
        hk_fullscreen_btn_->SetValue("F11");

        fname_template_edit_->SetValue("HomRec_{date}_{time}");
        autostop_spin_->SetValue(0);
        replay_buf_spin_->SetValue(0);

        sys_log_chk_->SetValue(hr_settings_get_flag(def, "system_logging_enabled") != 0);
        plugin_log_chk_->SetValue(hr_settings_get_flag(def, "plugin_logging_enabled") != 0);
        disable_preview_chk_->SetValue(hr_settings_get_flag(def, "disable_preview") != 0);
        preview_quality_choice_->SetSelection(2); // 100%
        preview_fps_spin_->SetValue(15);
        preview_quality_choice_->Enable(true);
        preview_fps_spin_->Enable(true);

        shortcut_chk_->SetValue(hr_settings_get_flag(def, "desktop_shortcut_enabled") != 0);
        shortcut_path_edit_->SetValue(wxString::FromUTF8(HrSystemIntegration::GetDefaultDesktopPath()));
        shortcut_path_edit_->Enable(false);
        autostart_chk_->SetValue(hr_settings_get_flag(def, "autostart_enabled") != 0);
        tray_chk_->SetValue(hr_settings_get_flag(def, "minimize_tray") != 0);

        hr_settings_destroy(def);
    }

    void OnSave(wxCommandEvent &) {
        // -- General ---------------------------------------------------
        state_.output_folder = folder_edit_->GetValue().ToUTF8().data();
        state_.quality = quality_slider_->GetValue();
        state_.target_fps = fps_spin_->GetValue();
        state_.monitor_id = monitor_choice_->GetSelection() + 1;

        {
            static const int kPct[] = {100, 75, 50, 25};
            int sel = resolution_choice_->GetSelection();
            if (sel < 0 || sel >= 4) sel = 1; // fall back to 75% if somehow unselected
            state_.scale_factor = kPct[sel] / 100.0;

            bool custom = resolution_mode_choice_->GetSelection() == 1;
            state_.resolution_mode = custom ? ResolutionMode::Absolute : ResolutionMode::Percent;

            state_.resolution_w = resolution_w_spin_->GetValue();
            state_.resolution_h = resolution_h_spin_->GetValue();
        }

        state_.countdown_enabled = countdown_chk_->GetValue();
        state_.timestamp_enabled = timestamp_chk_->GetValue();
        state_.cursor_enabled = cursor_chk_->GetValue();
        state_.show_summary = notify_chk_->GetValue();

        {
            int sel = lang_choice_->GetSelection();
            state_.current_language = (sel >= 0 && (size_t)sel < lang_codes_.size())
                                           ? lang_codes_[(size_t)sel] : "en";
        }

        // -- Video & Codec -------------------------------------------------
        // These used to be "in-memory only for now" per
        // this comment's own former text - hr_settings.cpp's JSON whitelist
        // never had fields for hw_accel/enc_preset/enc_crf/pix_fmt/
        // custom_ffmpeg_args, so anything typed here reverted on restart.
        // Now that HrcConfig::Save() below persists the *entire* AppState
        // in one shot (see hrc_config.cpp's Save()), every field set here
        // actually round-trips - nothing further to do per-field.
        state_.video_codec = codec_combo_->GetValue().ToUTF8().data();
        state_.hw_accel = hwaccel_combo_->GetValue().ToUTF8().data();
        state_.enc_preset = preset_combo_->GetValue().ToUTF8().data();
        state_.enc_crf = crf_spin_->GetValue();
        state_.pix_fmt = pixfmt_combo_->GetValue().ToUTF8().data();
        state_.custom_ffmpeg_args = custom_args_edit_->GetValue().ToUTF8().data();

        // -- Audio -------------------------------------------------------
        {
            int sel = mic_choice_->GetSelection();
            state_.mic_device_id = (sel >= 0 && (size_t)sel < mic_ids_.size()) ? mic_ids_[(size_t)sel] : "";
        }
        state_.audio_sample_rate = sample_rate_spin_->GetValue();
        state_.audio_aac_bitrate = aac_bitrate_edit_->GetValue().ToUTF8().data();
        state_.audio_out_channels = channels_spin_->GetValue();
        state_.separate_audio_mp3 = separate_mp3_chk_->GetValue();

        // -- Hotkeys -------------------------------------------------------
        state_.hotkey_start_stop = hk_startstop_btn_->GetValue().ToUTF8().data();
        state_.hotkey_pause = hk_pause_btn_->GetValue().ToUTF8().data();
        state_.hotkey_fullscreen = hk_fullscreen_btn_->GetValue().ToUTF8().data();

        // -- Advanced -------------------------------------------------------
        state_.filename_template = fname_template_edit_->GetValue().ToUTF8().data();
        state_.auto_stop_min = autostop_spin_->GetValue();
        state_.replay_buffer_sec = replay_buf_spin_->GetValue();

        // -- Security -------------------------------------------------------
        state_.system_logging_enabled = sys_log_chk_->GetValue();
        state_.plugin_logging_enabled = plugin_log_chk_->GetValue();
        // Applied immediately (not just at next launch) - see
        // hr_pc_log.h/hr_plugin_log.h's SetEnabled().
        HrPcLog::SetEnabled(state_.system_logging_enabled);
        HrPluginLog::SetEnabled(state_.plugin_logging_enabled);

        state_.disable_preview = disable_preview_chk_->GetValue();
        {
            static const int kPreviewQualityPct[] = {50, 75, 100};
            int sel = preview_quality_choice_->GetSelection();
            if (sel < 0 || sel >= 3) sel = 2;
            state_.preview_quality_pct = kPreviewQualityPct[sel];

            state_.preview_fps = preview_fps_spin_->GetValue();
        }

        // -- System -------------------------------------------------------
        {
            bool wantShortcut = shortcut_chk_->GetValue();
            std::string shortcutDir = shortcut_path_edit_->GetValue().ToUTF8().data();
            if (shortcutDir.empty()) shortcutDir = HrSystemIntegration::GetDefaultDesktopPath();

            if (wantShortcut) {
                HrSystemIntegration::CreateDesktopShortcut(shortcutDir);
            } else {
                // Remove from wherever it was last created (falls back to
                // the field's current folder if we never had a stored
                // path - e.g. first time this checkbox is touched).
                std::string prevDir = state_.desktop_shortcut_path.empty() ? shortcutDir : state_.desktop_shortcut_path;
                HrSystemIntegration::RemoveDesktopShortcut(prevDir);
            }
            state_.desktop_shortcut_enabled = wantShortcut;
            state_.desktop_shortcut_path = shortcutDir;

            bool wantAutostart = autostart_chk_->GetValue();
            HrSystemIntegration::SetAutostart(wantAutostart);
            state_.autostart_enabled = wantAutostart;

            state_.minimize_to_tray = tray_chk_->GetValue();
        }

        // -- Settings file path (Advanced tab) -------------------------
        // Resolve the path this session's settings actually live at
        // *before* overwriting settings_path with the field's new value -
        // that's the file on disk that needs to become the new name, not
        // just get a fresh copy written next to it. See
        // HrcConfig::RenameSettingsFile()'s comment for why this used to
        // look like the setting "didn't do anything": only the save
        // *destination* changed, the old file itself was never touched.
        std::wstring old_target = HrcConfig::ResolveSettingsPath(state_);
        state_.settings_path = settings_path_edit_->GetValue().ToUTF8().data();

        // Phase 1 settings-storage migration (see commands.md): one save
        // call persists the *entire* AppState, replacing the old JSON
        // engine's fixed field whitelist (hr_settings.cpp) - see that
        // file's own header comment for the exact bug this whitelist
        // caused previously (show_summary/show_overlays_panel silently
        // not persisting because someone forgot to add them to it).
        std::wstring target = HrcConfig::ResolveSettingsPath(state_);
        HrcConfig::RenameSettingsFile(old_target, target);
        HrcConfig::Save(state_, target);
        if (target != HrcConfig::kDefaultSettingsPath) {
            // Keep a live mirror at the default location too, so
            // main_frame.cpp's startup bootstrap - which always checks
            // kDefaultSettingsPath first - can find this custom path (via
            // settings_path, saved as part of the mirror) and follow it,
            // without needing a second file format just for that pointer.
            HrcConfig::Save(state_, HrcConfig::kDefaultSettingsPath);
        }
        EndModal(wxID_OK);
    }

    AppState &state_;
    ThemeColors theme_;
    wxNotebook *notebook_ = nullptr;
    size_t kTabGeneral = 0, kTabVideo = 0, kTabAdvanced = 0;

    // General
    LabeledSlider *quality_slider_ = nullptr;
    wxSpinCtrl *fps_spin_ = nullptr;
    wxChoice *monitor_choice_ = nullptr;
    wxChoice *resolution_choice_ = nullptr;
    wxChoice *resolution_mode_choice_ = nullptr;
    wxSpinCtrl *resolution_w_spin_ = nullptr, *resolution_h_spin_ = nullptr;
    wxTextCtrl *folder_edit_ = nullptr;
    wxCheckBox *countdown_chk_ = nullptr, *timestamp_chk_ = nullptr, *cursor_chk_ = nullptr, *notify_chk_ = nullptr;
    wxChoice *lang_choice_ = nullptr;
    std::vector<std::string> lang_codes_; // parallel to lang_choice_'s items

    // Video & Codec
    wxComboBox *codec_combo_ = nullptr, *hwaccel_combo_ = nullptr, *preset_combo_ = nullptr,
               *pixfmt_combo_ = nullptr;
    wxTextCtrl *custom_args_edit_ = nullptr;
    wxSpinCtrl *crf_spin_ = nullptr;

    // Audio
    wxChoice *mic_choice_ = nullptr;
    std::vector<std::string> mic_ids_; // parallel to mic_choice_'s items
    wxSpinCtrl *sample_rate_spin_ = nullptr, *channels_spin_ = nullptr;
    wxTextCtrl *aac_bitrate_edit_ = nullptr;
    wxCheckBox *separate_mp3_chk_ = nullptr;

    // Hotkeys
    HotkeyButton *hk_startstop_btn_ = nullptr, *hk_pause_btn_ = nullptr, *hk_fullscreen_btn_ = nullptr;

    // Advanced
    wxTextCtrl *fname_template_edit_ = nullptr;
    wxSpinCtrl *autostop_spin_ = nullptr, *replay_buf_spin_ = nullptr;

    // Security
    wxCheckBox *sys_log_chk_ = nullptr, *plugin_log_chk_ = nullptr;
    wxCheckBox *disable_preview_chk_ = nullptr;
    wxChoice *preview_quality_choice_ = nullptr;
    wxSpinCtrl *preview_fps_spin_ = nullptr;

    // System
    wxCheckBox *shortcut_chk_ = nullptr;
    wxTextCtrl *shortcut_path_edit_ = nullptr;
    wxTextCtrl *settings_path_edit_ = nullptr;
    wxCheckBox *autostart_chk_ = nullptr;
    wxCheckBox *tray_chk_ = nullptr;
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
