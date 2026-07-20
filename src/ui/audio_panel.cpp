#include "audio_panel.h"
#include <wx/dcbuffer.h>
#include <cstdint>
#include <algorithm>

extern "C" {
    void hr_audio_set_volumes(float mic_vol, float sys_vol, int mic_mute, int sys_mute);
    void hr_audio_get_levels(int *out_mic, int *out_sys);
    uint32_t hr_lerp_color(float t);
    void hr_peak_decay(int level, int *peak, int *peak_decay);
}

namespace {
wxColour U32ToColour(uint32_t rgb) {
    // hr_lerp_color returns 0x00RRGGBB.
    return wxColour((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}
} // namespace

// ---------------------------------------------------------------------------
// LevelMeterPanel
// ---------------------------------------------------------------------------
LevelMeterPanel::LevelMeterPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 18), wxBORDER_NONE) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &LevelMeterPanel::OnPaint, this);
}

void LevelMeterPanel::SetLevel(int level_0_100) {
    level_ = level_0_100;
    hr_peak_decay(level_, &peak_, &peak_decay_);
    Refresh(false);
}

void LevelMeterPanel::OnPaint(wxPaintEvent &) {
    wxAutoBufferedPaintDC dc(this);
    wxSize cs = GetClientSize();
    dc.SetBrush(wxBrush(bg_));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, cs.GetWidth(), cs.GetHeight());

    int fillW = (int)((level_ / 100.0) * cs.GetWidth());
    if (fillW > 0) {
        dc.SetBrush(wxBrush(U32ToColour(hr_lerp_color(level_ / 100.0f))));
        dc.DrawRectangle(0, 0, fillW, cs.GetHeight());
    }

    int peakX = (int)((peak_ / 100.0) * cs.GetWidth());
    if (peakX > 0 && peakX < cs.GetWidth()) {
        dc.SetPen(wxPen(*wxWHITE, 2));
        dc.DrawLine(peakX, 0, peakX, cs.GetHeight());
    }
}

// ---------------------------------------------------------------------------
// AudioPanel
// ---------------------------------------------------------------------------
AudioPanel::AudioPanel(wxWindow *parent, AppState &state, RecordingController &rec)
    : wxPanel(parent), state_(state), rec_(rec) {
    auto *grid = new wxFlexGridSizer(2, 4, 8, 10);
    grid->AddGrowableCol(1, 1); // slider column stretches
    grid->SetFlexibleDirection(wxHORIZONTAL);

    auto addRow = [&](const wxString &labelText, int sliderId, int muteId,
                       wxStaticText *&label, ColorSlider *&slider, ColorButton *&mute, LevelMeterPanel *&meter) {
        label = new wxStaticText(this, wxID_ANY, labelText, wxDefaultPosition, wxSize(90, -1));
        grid->Add(label, 0, wxALIGN_CENTRE_VERTICAL);

        slider = new ColorSlider(this, sliderId, 100, 0, 150);
        grid->Add(slider, 1, wxEXPAND | wxALIGN_CENTRE_VERTICAL);

        mute = new ColorButton(this, muteId, "Mute");
        mute->SetMinSize(wxSize(56, 24));
        grid->Add(mute, 0, wxALIGN_CENTRE_VERTICAL);

        meter = new LevelMeterPanel(this);
        meter->SetMinSize(wxSize(140, 18));
        grid->Add(meter, 0, wxALIGN_CENTRE_VERTICAL);
    };

    addRow("Microphone", ID_AUDIO_MIC_SLIDER, ID_AUDIO_MIC_MUTE, mic_label_, mic_slider_, mic_mute_btn_, mic_meter_);
    addRow("Desktop Audio", ID_AUDIO_SYS_SLIDER, ID_AUDIO_SYS_MUTE, sys_label_, sys_slider_, sys_mute_btn_, sys_meter_);

    auto *outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(grid, 1, wxEXPAND | wxALL, 12);
    SetSizer(outer);

    Bind(wxEVT_SLIDER, &AudioPanel::OnMicSlider, this, ID_AUDIO_MIC_SLIDER);
    Bind(wxEVT_SLIDER, &AudioPanel::OnSysSlider, this, ID_AUDIO_SYS_SLIDER);
    Bind(wxEVT_BUTTON, &AudioPanel::OnMicMute, this, ID_AUDIO_MIC_MUTE);
    Bind(wxEVT_BUTTON, &AudioPanel::OnSysMute, this, ID_AUDIO_SYS_MUTE);

    ApplyTheme(GetBuiltinTheme("dark"));
}

void AudioPanel::ApplyTheme(const ThemeColors &theme) {
    theme_ = theme;
    wxColour surface = FromColorref(theme.surface_light);
    wxColour text = FromColorref(theme.text);
    wxColour trackCol = FromColorref(theme.surface);
    wxColour meterBg = FromColorref(theme.preview_bg);

    SetBackgroundColour(surface);
    for (wxStaticText *lbl : {mic_label_, sys_label_}) {
        if (!lbl) continue;
        lbl->SetForegroundColour(text);
        lbl->SetBackgroundColour(surface);
    }
    for (ColorSlider *s : {mic_slider_, sys_slider_}) {
        if (s) s->SetTheme(trackCol, FromColorref(theme.accent), FromColorref(theme.text));
    }
    UpdateMuteButtonColours();
    for (LevelMeterPanel *m : {mic_meter_, sys_meter_}) {
        if (m) m->SetBgColour(meterBg);
    }
    Refresh(true);
}

// Mute is a toggle, not a permanent warning, so it shouldn't sit in the
// error/red color all the time regardless of state — that reads as "this is
// broken" rather than "this is off". Neutral surface color when live audio
// is flowing, warning color only once the channel is actually muted.
void AudioPanel::UpdateMuteButtonColours() {
    wxColour neutralBg = FromColorref(theme_.surface_light);
    wxColour neutralFg = FromColorref(theme_.text);
    wxColour mutedBg = FromColorref(theme_.warning);
    wxColour mutedFg = FromColorref(theme_.bg);
    if (mic_mute_btn_) mic_mute_btn_->SetColours(mic_muted_ ? mutedBg : neutralBg, mic_muted_ ? mutedFg : neutralFg);
    if (sys_mute_btn_) sys_mute_btn_->SetColours(sys_muted_ ? mutedBg : neutralBg, sys_muted_ ? mutedFg : neutralFg);
}

void AudioPanel::PollLevels() {
    int mic = 0, sys = 0;
    hr_audio_get_levels(&mic, &sys);
    if (mic_meter_) mic_meter_->SetLevel(mic);
    if (sys_meter_) sys_meter_->SetLevel(sys);
}

void AudioPanel::PushVolumes() {
    hr_audio_set_volumes(mic_vol_, sys_vol_, mic_muted_ ? 1 : 0, sys_muted_ ? 1 : 0);
}

void AudioPanel::OnMicSlider(wxCommandEvent &evt) {
    mic_vol_ = evt.GetInt() / 100.0f;
    PushVolumes();
}

void AudioPanel::OnSysSlider(wxCommandEvent &evt) {
    sys_vol_ = evt.GetInt() / 100.0f;
    PushVolumes();
}

void AudioPanel::OnMicMute(wxCommandEvent &) {
    mic_muted_ = !mic_muted_;
    mic_mute_btn_->SetLabelText2(mic_muted_ ? "Unmute" : "Mute");
    UpdateMuteButtonColours();
    PushVolumes();
}

void AudioPanel::OnSysMute(wxCommandEvent &) {
    sys_muted_ = !sys_muted_;
    sys_mute_btn_->SetLabelText2(sys_muted_ ? "Unmute" : "Mute");
    UpdateMuteButtonColours();
    PushVolumes();
}
