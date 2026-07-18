// audio_panel.h — wxWidgets rewrite of the raw-Win32 mixer strip.
//
// Was CreateWindowExW'd Trackbar/BUTTON/owner-drawn-STATIC controls parented
// onto a plain STATIC container — never themed at all, which is why it sat
// on a plain white background below a now-themed preview. Rebuilt as real
// wx widgets (ColorSlider/ColorButton from themed_widgets.h, a wx-painted
// level meter) so it can actually pick up the app's theme colors.
#pragma once

#include <wx/wx.h>
#include "app_state.h"
#include "theme.h"
#include "themed_widgets.h"

class RecordingController; // kept for interface parity; not otherwise used here

// Live mic/system level meter — flat filled bar + white peak-hold line,
// same physics as the original (hr_lerp_color/hr_peak_decay), just
// wx-painted instead of drawn from a WM_DRAWITEM handler.
class LevelMeterPanel : public wxPanel {
public:
    explicit LevelMeterPanel(wxWindow *parent);
    void SetLevel(int level_0_100);
    void SetBgColour(wxColour c) { bg_ = c; Refresh(); }

private:
    void OnPaint(wxPaintEvent &evt);
    int level_ = 0, peak_ = 0, peak_decay_ = 0;
    wxColour bg_ = wxColour(17, 17, 27);
};

class AudioPanel : public wxPanel {
public:
    AudioPanel(wxWindow *parent, AppState &state, RecordingController &rec);

    void ApplyTheme(const ThemeColors &theme);

    // Called on the same ~50ms timer tick the meters use (mirrors
    // `_poll_audio_levels`'s `after(50, ...)` loop).
    void PollLevels();

    float mic_volume() const { return mic_vol_; }
    float sys_volume() const { return sys_vol_; }
    bool mic_muted() const { return mic_muted_; }
    bool sys_muted() const { return sys_muted_; }

private:
    void OnMicSlider(wxCommandEvent &evt);
    void OnSysSlider(wxCommandEvent &evt);
    void OnMicMute(wxCommandEvent &evt);
    void OnSysMute(wxCommandEvent &evt);
    void PushVolumes();

    AppState &state_;
    RecordingController &rec_;

    wxStaticText *mic_label_ = nullptr, *sys_label_ = nullptr;
    ColorSlider *mic_slider_ = nullptr, *sys_slider_ = nullptr;
    ColorButton *mic_mute_btn_ = nullptr, *sys_mute_btn_ = nullptr;
    LevelMeterPanel *mic_meter_ = nullptr, *sys_meter_ = nullptr;

    ThemeColors theme_;
    float mic_vol_ = 1.0f, sys_vol_ = 1.0f;
    bool mic_muted_ = false, sys_muted_ = false;
};

enum AudioPanelControlId {
    ID_AUDIO_MIC_SLIDER = 2001,
    ID_AUDIO_SYS_SLIDER = 2002,
    ID_AUDIO_MIC_MUTE   = 2003,
    ID_AUDIO_SYS_MUTE   = 2004,
};
