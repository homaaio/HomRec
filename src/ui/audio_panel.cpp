#include "audio_panel.h"
#include <wx/dcbuffer.h>
#include <cstdint>
#include <algorithm>

extern "C" {
    void hr_audio_set_volumes(float mic_vol, float sys_vol, int mic_mute, int sys_mute);
    void hr_audio_get_levels(int *out_mic, int *out_sys);
    void hr_peak_decay(int level, int *peak, int *peak_decay);
}

namespace {
constexpr int kZoneYellowStart = 22;  // ~ -20 dBFS
constexpr int kZoneRedStart    = 78;  // ~ -9 dBFS (clipping zone starts here)

wxColour ZoneColour(int level_0_100) {
    if (level_0_100 >= kZoneRedStart)    return wxColour(232, 68, 62);    // red   - clipping zone
    if (level_0_100 >= kZoneYellowStart) return wxColour(230, 190, 60);   // yellow
    return wxColour(110, 200, 130);                                      // green
}
} // namespace

// ---------------------------------------------------------------------------
// LevelMeterPanel
// ---------------------------------------------------------------------------
LevelMeterPanel::LevelMeterPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 18), wxBORDER_NONE) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &LevelMeterPanel::OnPaint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) { Refresh(true); evt.Skip(); });
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

    int w = cs.GetWidth();
    int fillW = (int)((level_ / 100.0) * w);
    int yellowPx = (int)((kZoneYellowStart / 100.0) * w);
    int redPx    = (int)((kZoneRedStart    / 100.0) * w);

    // Draw the lit portion of the bar in up to three segments so the
    // color actually changes where the level crosses each zone boundary,
    // instead of the whole filled bar being one single (blended) color.
    if (fillW > 0) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        int greenEnd  = std::min(fillW, yellowPx);
        int yellowEnd = std::min(fillW, redPx);
        if (greenEnd > 0) {
            dc.SetBrush(wxBrush(ZoneColour(0)));
            dc.DrawRectangle(0, 0, greenEnd, cs.GetHeight());
        }
        if (yellowEnd > yellowPx) {
            dc.SetBrush(wxBrush(ZoneColour(kZoneYellowStart)));
            dc.DrawRectangle(yellowPx, 0, yellowEnd - yellowPx, cs.GetHeight());
        }
        if (fillW > redPx) {
            dc.SetBrush(wxBrush(ZoneColour(kZoneRedStart)));
            dc.DrawRectangle(redPx, 0, fillW - redPx, cs.GetHeight());
        }
    }

    // Static marker at the clipping-zone boundary, drawn regardless of
    // the current level - so where clipping starts is visible even while
    // quiet, not just after the fact once it's already happened.
    if (redPx > 0 && redPx < w) {
        dc.SetPen(wxPen(wxColour(232, 68, 62), 1));
        dc.DrawLine(redPx, 0, redPx, cs.GetHeight());
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
                       wxStaticText *&label, LabeledSlider *&slider, ColorButton *&mute, LevelMeterPanel *&meter) {
        label = new wxStaticText(this, wxID_ANY, labelText, wxDefaultPosition, wxSize(90, -1));
        grid->Add(label, 0, wxALIGN_CENTRE_VERTICAL);

        slider = new LabeledSlider(this, sliderId, 100, 0, 150);
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

    auto *header = new wxBoxSizer(wxHORIZONTAL);
    auto *title = new wxStaticText(this, wxID_ANY, "Audio Mixer");
    header->Add(title, 1, wxALIGN_CENTRE_VERTICAL);
    // A bare narrow-string literal here ("\u2715") gets encoded by
    // the compiler using the *execution* character set, then handed to
    // wxString's implicit const char* constructor, which decodes it back
    // using the current *locale's* codepage (wxConvLibc) - not UTF-8. On
    // a non-Latin Windows locale (e.g. Cyrillic/CP1251) those two don't
    // agree, so the multiplication-sign glyph came out as a couple of
    // garbled Cyrillic-looking characters instead of a clean "X"/cross.
    // wxString::FromUTF8(...) (see every other icon literal in
    // main_frame.cpp) decodes explicitly as UTF-8 regardless of locale,
    // which is what this one was missing.
    close_btn_ = new ColorButton(this, ID_AUDIO_CLOSE, wxString::FromUTF8("\u2715"));
    close_btn_->SetMinSize(wxSize(24, 24));
    header->Add(close_btn_, 0);

    auto *outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    outer->Add(grid, 1, wxEXPAND | wxALL, 12);
    SetSizer(outer);

    Bind(wxEVT_SLIDER, &AudioPanel::OnMicSlider, this, ID_AUDIO_MIC_SLIDER);
    Bind(wxEVT_SLIDER, &AudioPanel::OnSysSlider, this, ID_AUDIO_SYS_SLIDER);
    Bind(wxEVT_BUTTON, &AudioPanel::OnMicMute, this, ID_AUDIO_MIC_MUTE);
    Bind(wxEVT_BUTTON, &AudioPanel::OnSysMute, this, ID_AUDIO_SYS_MUTE);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { if (on_close) on_close(); }, ID_AUDIO_CLOSE);

    title_ = title;
    ApplyTheme(GetBuiltinTheme("dark"));
}

void AudioPanel::ApplyTheme(const ThemeColors &theme) {
    theme_ = theme;
    wxColour surface = FromColorref(theme.surface_light);
    wxColour text = FromColorref(theme.text);
    wxColour trackCol = FromColorref(theme.surface);
    wxColour meterBg = FromColorref(theme.preview_bg);

    SetBackgroundColour(surface);
    for (wxStaticText *lbl : {mic_label_, sys_label_, title_}) {
        if (!lbl) continue;
        lbl->SetForegroundColour(text);
        lbl->SetBackgroundColour(surface);
    }
    if (title_) {
        wxFont f = title_->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        title_->SetFont(f);
    }
    if (close_btn_) close_btn_->SetColours(surface, text);
    for (LabeledSlider *s : {mic_slider_, sys_slider_}) {
        if (s) s->SetTheme(trackCol, FromColorref(theme.accent), FromColorref(theme.text), surface, text);
    }
    UpdateMuteButtonColours();
    for (LevelMeterPanel *m : {mic_meter_, sys_meter_}) {
        if (m) m->SetBgColour(meterBg);
    }
    Refresh(true);
}

// Mute is a toggle, not a permanent warning, so it shouldn't sit in the
// error/red color all the time regardless of state - that reads as "this is
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
