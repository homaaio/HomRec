// settings_dialog.cpp - tabbed rewrite.
//
// Was a single flat page. Split into a wxNotebook so each settings group
// gets its own tab (General / Video & Codec / Audio / Hotkeys / Advanced),
// and the fields that used to live in the separate raw-Win32 "Advanced
// Settings" dialog (advanced_settings_dialog.cpp) are folded in here as
// tabs instead - that dialog is retired, since duplicating a whole second
// themed-vs-unthemed settings surface was the bigger inconsistency to fix.
//
// Persistence note carried over from advanced_settings_dialog.h's own
// documented gap: hr_settings.cpp's on-disk format only has fields for
// output_folder/quality/fps/monitor/codec/audio-enabled/countdown/
// timestamp/cursor/show_summary/theme/language/minimize_tray/
// always_on_top/performance/dxgi. Everything on the Video & Codec (besides
// codec itself)/Audio/Hotkeys/Advanced tabs updates AppState in memory for
// the current run but is not yet written to homrec_settings.json - that
// needs hr_settings.cpp's struct + JSON reader/writer extended with the
// extra fields, which is a separate, mechanical change to a core file
// rather than something to silently paper over here. The Advanced tab
// says so directly.
#include "settings_dialog.h"
#include "themed_widgets.h"
#include "../hr_mic_enum.h"
#include <wx/notebook.h>
#include <wx/spinctrl.h>
#include <wx/dirdlg.h>
#include <wx/combobox.h>
#include <wx/choice.h>
#include <wx/scrolwin.h>
#include <string>
#include <vector>
#include <cmath>

extern "C" {
    void *hr_settings_create();
    void hr_settings_destroy(void *handle);
    int hr_settings_load(void *handle, const char *path);
    int hr_settings_save(const void *handle, const char *path);
    void hr_settings_set_output_folder(void *h, const char *v);
    void hr_settings_set_quality(void *h, int v);
    void hr_settings_set_fps(void *h, int v);
    void hr_settings_set_monitor(void *h, int v);
    void hr_settings_set_resolution_pct(void *h, int v);
    void hr_settings_set_resolution_mode(void *h, int v);
    void hr_settings_set_resolution_w(void *h, int v);
    void hr_settings_set_resolution_h(void *h, int v);
    void hr_settings_set_preview_quality_pct(void *h, int v);
    void hr_settings_set_preview_fps(void *h, int v);
    void hr_settings_set_codec(void *h, const char *v);
    void hr_settings_set_flag(void *h, const char *name, int v);

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
            // hr_display_info unavailable - still let the user pick
            // *something* rather than showing an empty/disabled control.
            for (int i = 0; i < 4; ++i)
                monitor_choice_->Append(wxString::Format("Monitor %d", i + 1));
        }
        if (di) hr_di_destroy(di);

        // state_.monitor_id is 1-based elsewhere in the app (see
        // RecordingController::Start()'s "idx = monitor_id - 1" - 0 means
        // "unset, use primary"). Choice selection is 0-based, so convert
        // at this boundary rather than storing the raw selection index.
        int sel = state_.monitor_id > 0 ? state_.monitor_id - 1 : 0;
        if (sel < 0 || sel >= (int)monitor_choice_->GetCount()) sel = 0;
        monitor_choice_->SetSelection(sel);
    }

    void BuildGeneralTab(wxColour bg, wxColour surface, wxColour accent, wxColour text) {
        auto *page = new wxScrolledWindow(notebook_);
        page->SetBackgroundColour(bg);
        auto *pageRoot = new wxBoxSizer(wxVERTICAL);
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

        // Was silently stuck at a hardcoded 75% with no UI control at all
        // (AppState::scale_factor) - this is the actual capture-resolution
        // knob; "Encoding quality" above only affects the encoder's CRF and
        // was never a resolution setting despite being the only slider in
        // this tab, which is what made it look like the wrong/missing control.
        // Resolution mode: "Percent" keeps the original 25/50/75/100%-of-
        // native dropdown; "Custom" lets the user type an exact target
        // width/height (e.g. 1280x720) instead - useful when the recording
        // needs to match a specific size (an old/low-power target device,
        // a video platform's preferred resolution, etc.) rather than
        // whatever percentage happens to land near it.
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
            int sel = 0;
            if (pct >= 100) sel = 0;
            else if (pct >= 75) sel = 1;
            else if (pct >= 50) sel = 2;
            else sel = 3;
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

        // Only one of the two resolution controls is meaningful at a time -
        // enable/disable (rather than hide) so the layout doesn't jump
        // around when switching modes.
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

        countdown_chk_ = AddCheck(page, pageRoot, text, bg, "Countdown (3s)", state_.countdown_enabled);
        timestamp_chk_ = AddCheck(page, pageRoot, text, bg, "Timestamp", state_.timestamp_enabled);
        cursor_chk_    = AddCheck(page, pageRoot, text, bg, "Cursor", state_.cursor_enabled);
        notify_chk_    = AddCheck(page, pageRoot, text, bg, "Show summary", state_.show_summary);
        disable_preview_chk_ = AddCheck(page, pageRoot, text, bg,
                                         "Disable live preview (for performance)",
                                         state_.disable_preview);

        // Preview quality/FPS: the live preview thumbnail shown in the app
        // costs real CPU/GPU time (capture + overlay/cursor compositing +
        // downscale) on top of whatever's actually being recorded - these
        // two knobs let that cost be turned down independently, which
        // matters most on older/weaker machines. Grouped under the
        // "Disable live preview" checkbox since they're meaningless once
        // it's off.
        auto *previewRow = new wxBoxSizer(wxHORIZONTAL);
        auto *pqLbl = new wxStaticText(page, wxID_ANY, "Preview quality:");
        pqLbl->SetForegroundColour(text);
        pqLbl->SetBackgroundColour(bg);
        previewRow->Add(pqLbl, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 6);

        preview_quality_choice_ = new wxChoice(page, wxID_ANY);
        preview_quality_choice_->Append("Low (50%)");
        preview_quality_choice_->Append("Medium (75%)");
        preview_quality_choice_->Append("High (100%)");
        {
            int pct = state_.preview_quality_pct;
            int sel = 2;
            if (pct <= 50) sel = 0;
            else if (pct <= 75) sel = 1;
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

        pageRoot->AddStretchSpacer(1);
        page->SetSizer(pageRoot);
        page->SetScrollRate(0, 12);
        page->FitInside();
        notebook_->AddPage(page, "General");
    }

    void BuildVideoTab(wxColour bg, wxColour text) {
        auto *page = new wxPanel(notebook_);
        page->SetBackgroundColour(bg);
        auto *pageRoot = new wxBoxSizer(wxVERTICAL);
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

        // BUGFIX: recording always silently used whichever device Windows
        // currently considers the default microphone, with no way to pick
        // a different one (see hr_audio.cpp's hr_audio_start()). List what's
        // actually attached (hr_mic_enum.h) instead.
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
        hk_startstop_btn_ = new HotkeyButton(page, wxID_ANY, wxString::FromUTF8(state_.hotkey_start_stop));
        grid->Add(hk_startstop_btn_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Pause:");
        hk_pause_btn_ = new HotkeyButton(page, wxID_ANY, wxString::FromUTF8(state_.hotkey_pause));
        grid->Add(hk_pause_btn_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid, text, bg, "Fullscreen:");
        hk_fullscreen_btn_ = new HotkeyButton(page, wxID_ANY, wxString::FromUTF8(state_.hotkey_fullscreen));
        grid->Add(hk_fullscreen_btn_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        for (HotkeyButton *hk : {hk_startstop_btn_, hk_pause_btn_, hk_fullscreen_btn_})
            hk->SetColours(bg, text, wxColour(120, 170, 250));

        pageRoot->Add(grid, 0, wxEXPAND | wxALL, 16);
        auto *note = new wxStaticText(page, wxID_ANY, "Click a binding, then press the key combo you want to use.\nEsc cancels without changing it.");
        note->SetForegroundColour(text);
        note->SetBackgroundColour(bg);
        pageRoot->Add(note, 0, wxALL, 16);
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
            "Hotkeys apply for this session but aren't written to disk yet -\n"
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

        state_.monitor_id = monitor_choice_->GetSelection() + 1;
        hr_settings_set_monitor(settings_, state_.monitor_id);

        {
            static const int kPct[] = {100, 75, 50, 25};
            int sel = resolution_choice_->GetSelection();
            if (sel < 0 || sel >= 4) sel = 1; // fall back to 75% if somehow unselected
            state_.scale_factor = kPct[sel] / 100.0;
            hr_settings_set_resolution_pct(settings_, kPct[sel]);

            bool custom = resolution_mode_choice_->GetSelection() == 1;
            state_.resolution_mode = custom ? ResolutionMode::Absolute : ResolutionMode::Percent;
            hr_settings_set_resolution_mode(settings_, custom ? 1 : 0);

            state_.resolution_w = resolution_w_spin_->GetValue();
            state_.resolution_h = resolution_h_spin_->GetValue();
            hr_settings_set_resolution_w(settings_, state_.resolution_w);
            hr_settings_set_resolution_h(settings_, state_.resolution_h);
        }

        state_.countdown_enabled = countdown_chk_->GetValue();
        state_.timestamp_enabled = timestamp_chk_->GetValue();
        state_.cursor_enabled = cursor_chk_->GetValue();
        state_.show_summary = notify_chk_->GetValue();
        state_.disable_preview = disable_preview_chk_->GetValue();

        {
            static const int kPreviewQualityPct[] = {50, 75, 100};
            int sel = preview_quality_choice_->GetSelection();
            if (sel < 0 || sel >= 3) sel = 2;
            state_.preview_quality_pct = kPreviewQualityPct[sel];
            hr_settings_set_preview_quality_pct(settings_, state_.preview_quality_pct);

            state_.preview_fps = preview_fps_spin_->GetValue();
            hr_settings_set_preview_fps(settings_, state_.preview_fps);
        }

        hr_settings_set_flag(settings_, "countdown", state_.countdown_enabled ? 1 : 0);
        hr_settings_set_flag(settings_, "timestamp", state_.timestamp_enabled ? 1 : 0);
        hr_settings_set_flag(settings_, "cursor", state_.cursor_enabled ? 1 : 0);
        hr_settings_set_flag(settings_, "show_summary", state_.show_summary ? 1 : 0);
        hr_settings_set_flag(settings_, "disable_preview", state_.disable_preview ? 1 : 0);

        // -- Video / Codec (video_codec persists; the rest is in-memory
        // only for now - see the Advanced tab's note) -----------------------
        state_.video_codec = codec_combo_->GetValue().ToUTF8().data();
        hr_settings_set_codec(settings_, state_.video_codec.c_str());
        state_.hw_accel = hwaccel_combo_->GetValue().ToUTF8().data();
        state_.enc_preset = preset_combo_->GetValue().ToUTF8().data();
        state_.enc_crf = crf_spin_->GetValue();
        state_.pix_fmt = pixfmt_combo_->GetValue().ToUTF8().data();
        state_.custom_ffmpeg_args = custom_args_edit_->GetValue().ToUTF8().data();

        // -- Audio ------------------------------------------------------------
        state_.audio_sample_rate = sample_rate_spin_->GetValue();
        state_.audio_aac_bitrate = aac_bitrate_edit_->GetValue().ToUTF8().data();
        state_.audio_out_channels = channels_spin_->GetValue();
        state_.separate_audio_mp3 = separate_mp3_chk_->GetValue();
        {
            int sel = mic_choice_->GetSelection();
            state_.mic_device_id = (sel >= 0 && (size_t)sel < mic_ids_.size()) ? mic_ids_[(size_t)sel] : "";
        }

        // -- Hotkeys ------------------------------------------------------------
        state_.hotkey_start_stop = hk_startstop_btn_->GetValue().ToUTF8().data();
        state_.hotkey_pause = hk_pause_btn_->GetValue().ToUTF8().data();
        state_.hotkey_fullscreen = hk_fullscreen_btn_->GetValue().ToUTF8().data();

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
    LabeledSlider *quality_slider_ = nullptr;
    wxSpinCtrl *fps_spin_ = nullptr;
    wxChoice *monitor_choice_ = nullptr;
    wxChoice *resolution_choice_ = nullptr;
    wxChoice *resolution_mode_choice_ = nullptr;
    wxSpinCtrl *resolution_w_spin_ = nullptr, *resolution_h_spin_ = nullptr;
    wxTextCtrl *folder_edit_ = nullptr;
    wxCheckBox *countdown_chk_ = nullptr, *timestamp_chk_ = nullptr, *cursor_chk_ = nullptr, *notify_chk_ = nullptr;
    wxCheckBox *disable_preview_chk_ = nullptr;
    wxChoice *preview_quality_choice_ = nullptr;
    wxSpinCtrl *preview_fps_spin_ = nullptr;

    // Video / Codec
    wxComboBox *codec_combo_ = nullptr, *hwaccel_combo_ = nullptr, *preset_combo_ = nullptr,
               *pixfmt_combo_ = nullptr;
    wxTextCtrl *custom_args_edit_ = nullptr;
    wxSpinCtrl *crf_spin_ = nullptr;

    // Audio
    wxSpinCtrl *sample_rate_spin_ = nullptr, *channels_spin_ = nullptr;
    wxTextCtrl *aac_bitrate_edit_ = nullptr;
    wxCheckBox *separate_mp3_chk_ = nullptr;
    wxChoice *mic_choice_ = nullptr;
    std::vector<std::string> mic_ids_; // parallel to mic_choice_'s items

    // Hotkeys
    HotkeyButton *hk_startstop_btn_ = nullptr, *hk_pause_btn_ = nullptr, *hk_fullscreen_btn_ = nullptr;

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
