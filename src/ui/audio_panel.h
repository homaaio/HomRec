// audio_panel.h — Phase 4
//
// Port of homrec_app/dialogs/audio_panel.py + audio_level_meter.py (the
// live copies — see the corrected audit note in the migration plan; the
// top-level files of the same name are dead code, not these). Level meter
// coloring/peak-decay physics reuse hr_lerp_color / hr_peak_decay verbatim
// instead of re-deriving them, since those exports already mirror
// AudioLevelMeter._lerp_color / set_level exactly.
#pragma once

#include <windows.h>
#include "app_state.h"

class RecordingController; // for hr_audio_get_levels access via the controller

class AudioLevelMeterCtl {
public:
    // Creates a child control (custom-drawn) at the given rect. `id` is the
    // control ID used in WM_DRAWITEM/timer dispatch.
    HWND Create(HWND parent, HINSTANCE hInst, int id, int x, int y, int w, int h);
    void SetLevel(int level_0_100); // drives peak/decay via hr_peak_decay, then repaints
    void Draw(HDC hdc, const RECT &rect) const;
    HWND hwnd() const { return hwnd_; }

private:
    HWND hwnd_ = nullptr;
    int level_ = 0;
    int peak_ = 0;
    int peak_decay_ = 0;
};

class AudioPanel {
public:
    AudioPanel(AppState &state, RecordingController &rec);

    HWND Create(HWND parent, HINSTANCE hInst, int x, int y, int w, int h);

    // Called on the same ~50ms timer tick the meters use (mirrors
    // `_poll_audio_levels`'s `after(50, ...)` loop).
    void PollLevels();

    void OnCommand(int id);
    void OnHScroll(HWND ctrlHwnd, int pos);

    // Routes a WM_DRAWITEM (forwarded from main_window's WndProc, since
    // owner-draw notifications go to the PARENT window, not the control
    // itself) to whichever meter it's actually for. Returns true if this
    // panel handled it.
    bool HandleDrawItem(DRAWITEMSTRUCT *dis) {
        if (dis->hwndItem == mic_meter_.hwnd()) { mic_meter_.Draw(dis->hDC, dis->rcItem); return true; }
        if (dis->hwndItem == sys_meter_.hwnd()) { sys_meter_.Draw(dis->hDC, dis->rcItem); return true; }
        return false;
    }

    float mic_volume() const { return mic_vol_; }
    float sys_volume() const { return sys_vol_; }
    bool mic_muted() const { return mic_muted_; }
    bool sys_muted() const { return sys_muted_; }

private:
    AppState &state_;
    RecordingController &rec_;

    HWND hwnd_ = nullptr;
    HWND mic_slider_ = nullptr, sys_slider_ = nullptr;
    HWND mic_mute_btn_ = nullptr, sys_mute_btn_ = nullptr;
    AudioLevelMeterCtl mic_meter_, sys_meter_;

    float mic_vol_ = 1.0f, sys_vol_ = 1.0f;
    bool mic_muted_ = false, sys_muted_ = false;
};

enum AudioPanelControlId {
    ID_AUDIO_MIC_SLIDER = 2001,
    ID_AUDIO_SYS_SLIDER = 2002,
    ID_AUDIO_MIC_MUTE   = 2003,
    ID_AUDIO_SYS_MUTE   = 2004,
};
