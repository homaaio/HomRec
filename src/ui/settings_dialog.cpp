// settings_dialog.cpp - progressive-disclosure rewrite.
//
// Was a wxNotebook split into 5 tabs (General / Video & Codec / Audio /
// Hotkeys / Advanced) - functional, but it put codec/CRF/hw-accel/custom
// ffmpeg args (stuff an average user has no reason to touch, and can
// genuinely break a recording if set wrong) at the same visual weight as
// "where do my recordings get saved". Rebuilt as a single scrollable page:
// everyday fields are always visible, and everything a casual user
// wouldn't understand lives in a panel that's hidden by default and
// toggled by one button in the header ("Show advanced settings" /
// "Hide advanced settings") - clicking it adds/removes those fields to
// THIS window in place, it never opens a second dialog. Persistence is
// unchanged: still goes through hr_settings_create/load/save/get_*/set_*
// (see the note above BuildAdvancedSection() for which fields that
// covers).
#include "settings_dialog.h"
#include "themed_widgets.h"
#include "../hr_mic_enum.h"
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

enum { IDC_QUALITY = 3001, IDC_BROWSE = 3002, IDC_SAVE = 3003, IDC_CANCEL = 3004, IDC_TOGGLE_ADVANCED = 3005 };

// Old tab indices ShowSettingsDialogTab() callers still pass in (see
// settings_dialog.h) - 1 (old "Video/Codec") and 4 (old "Advanced") now
// map to "open with the advanced panel already expanded" instead of
// selecting a notebook page, since there are no separate pages anymore.
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

class SettingsDialog : public wxDialog {
public:
    SettingsDialog(wxWindow *parent, AppState &state, const ThemeColors &theme)
        : wxDialog(parent, wxID_ANY, "Settings", wxDefaultPosition, wxSize(560, 600),
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

        // Header: title on the left, the one button that reveals/hides
        // everything in BuildAdvancedSection() on the right. This is the
        // "corner button" from the spec - toggling it never opens a new
        // window, it just Show()/Hide()s advanced_panel_ inside this one.
        auto *headerRow = new wxBoxSizer(wxHORIZONTAL);
        auto *titleLbl = new wxStaticText(this, wxID_ANY, "Settings");
        wxFont titleFont = titleLbl->GetFont();
        titleFont.SetPointSize(titleFont.GetPointSize() + 3);
        titleFont.SetWeight(wxFONTWEIGHT_BOLD);
        titleLbl->SetFont(titleFont);
        titleLbl->SetForegroundColour(text);
        titleLbl->SetBackgroundColour(bg);
        headerRow->Add(titleLbl, 0, wxALIGN_CENTRE_VERTICAL);
        headerRow->AddStretchSpacer(1);
        advanced_toggle_btn_ = new ColorButton(this, IDC_TOGGLE_ADVANCED, wxString::FromUTF8("\u2699 Advanced"));
        advanced_toggle_btn_->SetMinSize(wxSize(150, 28));
        advanced_toggle_btn_->SetColours(surface, text);
        headerRow->Add(advanced_toggle_btn_, 0, wxALIGN_CENTRE_VERTICAL);
        root->Add(headerRow, 0, wxEXPAND | wxALL, 12);

        scroller_ = new wxScrolledWindow(this);
        scroller_->SetBackgroundColour(bg);
        auto *scrollRoot = new wxBoxSizer(wxVERTICAL);

        BuildBasicSection(scroller_, scrollRoot, bg, surface, accent, text, textDim);

        // Everything "for verified users" lives in this one panel so it
        // can be added/removed from the layout as a single unit. Built
        // hidden; ToggleAdvanced() is the only thing that shows it.
        advanced_panel_ = new wxPanel(scroller_);
        advanced_panel_->SetBackgroundColour(bg);
        auto *advRoot = new wxBoxSizer(wxVERTICAL);
        BuildAdvancedSection(advanced_panel_, advRoot, bg, accent, text, textDim);
        advanced_panel_->SetSizer(advRoot);
        advanced_panel_->Hide();
        scrollRoot->Add(advanced_panel_, 0, wxEXPAND);

        scroller_->SetSizer(scrollRoot);
        scroller_->SetScrollRate(0, 12);
        scroller_->FitInside();
        root->Add(scroller_, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

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
        Bind(wxEVT_BUTTON, &SettingsDialog::OnToggleAdvanced, this, IDC_TOGGLE_ADVANCED);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); }, IDC_CANCEL);
    }

    ~SettingsDialog() override { hr_settings_destroy(settings_); }

    // Old call sites (the "Advanced Settings..." menu item) used to pick a
    // notebook tab by index; now there's only one page, so this just makes
    // sure the advanced panel is open when the caller was asking for one
    // of the tabs that used to hold "pro" fields (Video/Codec, Advanced).
    void SelectTab(int index) {
        if (index == kOldTabVideoCodec || index == kOldTabAdvanced) ShowAdvanced(true);
    }

private:
    void ShowAdvanced(bool show) {
        if (show == advanced_shown_) return;
        advanced_shown_ = show;
        // Go through the sizer's Show(), not just the window's - the
        // sizer item that reserves advanced_panel_'s space needs to know
        // too, or the space it occupies (or doesn't) can stay stale even
        // though the panel's own visibility flag flipped.
        scroller_->GetSizer()->Show(advanced_panel_, advanced_shown_, true);
        advanced_toggle_btn_->SetLabelText2(wxString::FromUTF8(
            advanced_shown_ ? "\u2699 Basic" : "\u2699 Advanced"));
        // BUGFIX: advanced_panel_ itself was never told to re-Layout() when
        // shown - its own wxFlexGridSizer (advRoot, built once back in the
        // ctor while the panel was still Hidden()) never got a chance to
        // compute real widths/heights for its children, since a hidden
        // window's Layout() calls are skipped by wx. The result: the sizer
        // Show() above correctly flips the "is this item visible" flag and
        // the button label updated (proving the click handler *did* run),
        // but scroller_->FitInside()/Layout() below were reflowing a child
        // whose own internal layout was still the all-zero one from
        // construction, so the reserved space for it stayed effectively
        // empty - the fields never had a size to actually be visible at.
        // Laying out advanced_panel_ itself first (now that it's shown, so
        // its own Layout() call actually takes effect) before asking the
        // parent scroller to size around it fixes that.
        advanced_panel_->Layout();
        scroller_->FitInside();
        scroller_->Layout();
        Layout();
    }

    void OnToggleAdvanced(wxCommandEvent &) { ShowAdvanced(!advanced_shown_); }

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

    // -- Basic: everyday fields, always visible ---------------------------
    void BuildBasicSection(wxWindow *page, wxSizer *pageRoot, wxColour bg, wxColour surface,
                            wxColour accent, wxColour text, wxColour textDim) {
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
        separate_mp3_chk_ = AddCheck(page, pageRoot, text, bg, "Also save audio as a separate MP3", state_.separate_audio_mp3);
        disable_preview_chk_ = AddCheck(page, pageRoot, text, bg,
                                         "Disable live preview (for performance)",
                                         state_.disable_preview);

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

        // Hotkeys - simple click-to-bind buttons, not "pro" enough to
        // hide, but visually grouped under their own heading so they read
        // as a distinct block rather than bleeding into the checkboxes
        // above.
        AddSectionHeading(page, pageRoot, accent, bg, "Hotkeys");
        auto *hkGrid = new wxFlexGridSizer(2, 8, 10);
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

    // -- Advanced ("for verified users"): hidden until the header button
    // is clicked. Persistence note carried over from the old tabbed
    // layout's Advanced tab: hr_settings.cpp's on-disk format only has
    // fields for output_folder/quality/fps/monitor/codec/audio-enabled/
    // countdown/timestamp/cursor/show_summary/theme/language/
    // minimize_tray/always_on_top/performance/dxgi. Everything below
    // besides the video codec updates AppState in memory for the current
    // run but isn't written to homrec_settings.json yet - that needs
    // hr_settings.cpp's struct + JSON reader/writer extended with the
    // extra fields, a separate mechanical change to a core file rather
    // than something to silently paper over here.
    void BuildAdvancedSection(wxWindow *page, wxSizer *pageRoot, wxColour bg, wxColour accent,
                               wxColour text, wxColour textDim) {
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

        auto *argsLbl = new wxStaticText(page, wxID_ANY, "Custom FFmpeg args:");
        argsLbl->SetForegroundColour(text);
        argsLbl->SetBackgroundColour(bg);
        pageRoot->Add(argsLbl, 0, wxLEFT | wxRIGHT, 16);
        custom_args_edit_ = new wxTextCtrl(page, wxID_ANY, wxString::FromUTF8(state_.custom_ffmpeg_args));
        pageRoot->Add(custom_args_edit_, 0, wxEXPAND | wxALL, 16);

        AddSectionHeading(page, pageRoot, accent, bg, "Advanced");
        auto *grid2 = new wxFlexGridSizer(2, 10, 10);
        grid2->AddGrowableCol(1, 1);

        AddLabel(page, grid2, text, bg, "Filename template:");
        fname_template_edit_ = new wxTextCtrl(page, wxID_ANY, wxString::FromUTF8(state_.filename_template));
        grid2->Add(fname_template_edit_, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid2, text, bg, "Auto-stop (min, 0 = off):");
        autostop_spin_ = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                         wxSP_ARROW_KEYS, 0, 1440, state_.auto_stop_min);
        grid2->Add(autostop_spin_, 0, wxALIGN_CENTRE_VERTICAL);

        AddLabel(page, grid2, text, bg, "Replay buffer (sec, 0 = off):");
        replay_buf_spin_ = new wxSpinCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                           wxSP_ARROW_KEYS, 0, 3600, state_.replay_buffer_sec);
        grid2->Add(replay_buf_spin_, 0, wxALIGN_CENTRE_VERTICAL);

        pageRoot->Add(grid2, 0, wxEXPAND | wxALL, 16);

        auto *note = new wxStaticText(page, wxID_ANY,
            "Note: everything on this panel besides Video codec applies for\n"
            "this session but isn't written to disk yet - the settings file\n"
            "only persists output folder, quality, FPS, monitor, video codec,\n"
            "and the checkboxes above.");
        note->SetForegroundColour(textDim);
        note->SetBackgroundColour(bg);
        pageRoot->Add(note, 0, wxALL, 16);
    }

    void OnBrowse(wxCommandEvent &) {
        wxDirDialog dlg(this, "Select output folder", folder_edit_->GetValue());
        if (dlg.ShowModal() == wxID_OK) folder_edit_->SetValue(dlg.GetPath());
    }

    void OnSave(wxCommandEvent &) {
        // -- Basic (persisted to disk via hr_settings_*) --------------------
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
        state_.separate_audio_mp3 = separate_mp3_chk_->GetValue();

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

        {
            int sel = mic_choice_->GetSelection();
            state_.mic_device_id = (sel >= 0 && (size_t)sel < mic_ids_.size()) ? mic_ids_[(size_t)sel] : "";
        }

        state_.hotkey_start_stop = hk_startstop_btn_->GetValue().ToUTF8().data();
        state_.hotkey_pause = hk_pause_btn_->GetValue().ToUTF8().data();
        state_.hotkey_fullscreen = hk_fullscreen_btn_->GetValue().ToUTF8().data();

        // -- Advanced (video_codec persists; the rest is in-memory only for
        // now - see BuildAdvancedSection()'s note) --------------------------
        state_.video_codec = codec_combo_->GetValue().ToUTF8().data();
        hr_settings_set_codec(settings_, state_.video_codec.c_str());
        state_.hw_accel = hwaccel_combo_->GetValue().ToUTF8().data();
        state_.enc_preset = preset_combo_->GetValue().ToUTF8().data();
        state_.enc_crf = crf_spin_->GetValue();
        state_.pix_fmt = pixfmt_combo_->GetValue().ToUTF8().data();
        state_.custom_ffmpeg_args = custom_args_edit_->GetValue().ToUTF8().data();

        state_.audio_sample_rate = sample_rate_spin_->GetValue();
        state_.audio_aac_bitrate = aac_bitrate_edit_->GetValue().ToUTF8().data();
        state_.audio_out_channels = channels_spin_->GetValue();

        state_.filename_template = fname_template_edit_->GetValue().ToUTF8().data();
        state_.auto_stop_min = autostop_spin_->GetValue();
        state_.replay_buffer_sec = replay_buf_spin_->GetValue();

        hr_settings_save(settings_, kSettingsPath);
        EndModal(wxID_OK);
    }

    AppState &state_;
    ThemeColors theme_;
    void *settings_ = nullptr;
    wxScrolledWindow *scroller_ = nullptr;
    wxPanel *advanced_panel_ = nullptr;
    ColorButton *advanced_toggle_btn_ = nullptr;
    bool advanced_shown_ = false;

    // Basic
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
    wxChoice *mic_choice_ = nullptr;
    std::vector<std::string> mic_ids_; // parallel to mic_choice_'s items
    wxCheckBox *separate_mp3_chk_ = nullptr;
    HotkeyButton *hk_startstop_btn_ = nullptr, *hk_pause_btn_ = nullptr, *hk_fullscreen_btn_ = nullptr;

    // Advanced ("verified users")
    wxComboBox *codec_combo_ = nullptr, *hwaccel_combo_ = nullptr, *preset_combo_ = nullptr,
               *pixfmt_combo_ = nullptr;
    wxTextCtrl *custom_args_edit_ = nullptr;
    wxSpinCtrl *crf_spin_ = nullptr;
    wxSpinCtrl *sample_rate_spin_ = nullptr, *channels_spin_ = nullptr;
    wxTextCtrl *aac_bitrate_edit_ = nullptr;
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
