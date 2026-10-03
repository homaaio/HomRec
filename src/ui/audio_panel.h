// audio_panel.h - the Audio Mixer (rebuilt for 2.4).
//
// What changed in 2.4:
//   * every channel is a self-contained "card": icon + name, mute, a level meter and a
//     fader. The fader is FlatFader (themed_widgets.h) which paints with its own colours -
//     the white frames the old LabeledSlider/wxSpinCtrl pair drew around each slider are
//     gone (the exact value is shown as text, e.g. "112%").
//   * two layouts, Settings > Audio > Mixer layout (AppState::audio_meter_style):
//       "horizontal" - cards stacked top to bottom, level bar above the fader (pre-2.4 feel)
//       "vertical"   - OBS-style channel strips side by side: vertical meter + vertical fader
//   * "+ Add source": another microphone, a browser, one program's/window's sound, or an
//     audio file. Those channels are stored in AppState::audio_sources and captured by the
//     hr_audio_extra_* backend (hr_audio_extra.inc).
//   * icons: icons/audio/microphone.png and desktop_audio.png (see hr_icons.h); a text
//     glyph is drawn when a file is missing.
//   * lives in a dockable pane (main_frame.cpp), so it has no title row / close button of its
//     own any more - the pane caption provides both.
//
// Cost: no timers of its own. PollLevels() is driven by the main frame's level-meter timer,
// repaints a meter only when its level or peak actually changed, and does nothing while
// hidden/minimized.
#pragma once

#include <wx/wx.h>
#include <wx/scrolwin.h>
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include "app_state.h"
#include "theme.h"
#include "themed_widgets.h"
#include "hr_icons.h"

class RecordingController;

// Live level meter - horizontal bar or vertical bar, green/yellow/red zones, white
// peak-hold tick. Same physics as before (hr_peak_decay).
class LevelMeterPanel : public wxPanel {
public:
    explicit LevelMeterPanel(wxWindow *parent, bool vertical = false);
    void SetVertical(bool v);
    void SetBgColour(wxColour c) { bg_ = c; Refresh(false); }
    void SetDimmed(bool d) { if (dimmed_ != d) { dimmed_ = d; Refresh(false); } }
    // Returns true when the picture changed (and a repaint was queued).
    bool SetLevel(int level_0_100);

protected:
    wxSize DoGetBestSize() const override;

private:
    void OnPaint(wxPaintEvent &evt);
    int level_ = 0, peak_ = 0, peak_decay_ = 0;
    bool vertical_;
    bool dimmed_ = false;
    wxColour bg_ = wxColour(17, 17, 27);
};

class AudioPanel : public wxPanel {
public:
    AudioPanel(wxWindow *parent, AppState &state, RecordingController &rec);
    ~AudioPanel() override;

    void ApplyTheme(const ThemeColors &theme);

    // Driven by the main frame's level-meter timer.
    void PollLevels();

    // Re-reads AppState::audio_meter_style and rebuilds the layout if it changed.
    void ApplyMeterStyle();

    float mic_volume() const { return slot_[0].vol; }
    float sys_volume() const { return slot_[1].vol; }
    bool mic_muted() const { return slot_[0].muted; }
    bool sys_muted() const { return slot_[1].muted; }

    // Kept for source compatibility with the pre-2.4 header (the pane caption closes it now).
    std::function<void()> on_close;
    // The mixer changed something that should be saved (a source was added/removed/renamed or
    // a fader moved on an extra source). main_frame persists the settings.
    std::function<void()> on_changed;

private:
    // One mixer channel. Slots 0/1 are the built-in Microphone and Desktop Audio; the extras
    // mirror AppState::audio_sources one to one.
    struct Channel {
        std::string  id;              // AudioSourceDef::id ("" for the built-ins)
        std::string  kind;            // "builtin_mic" | "builtin_sys" | "mic" | "app" | "file"
        wxString     name;
        float        vol   = 1.0f;
        bool         muted = false;
        int          handle = 0;      // hr_audio_extra_add() handle (extras)
        bool         online = true;   // false = the device / program isn't available right now
        unsigned long pid = 0;        // "app" channels: process currently captured
        std::string  temp_wav;        // "file" channels: decoded copy
        std::string  target;          // mic endpoint id / exe name / file path (mirrors AudioSourceDef)
        std::string  window_title;

        // widgets (rebuilt with the layout)
        wxPanel        *card  = nullptr;
        wxStaticText   *name_lbl = nullptr, *pct_lbl = nullptr;
        wxStaticBitmap *icon  = nullptr;
        wxStaticText   *glyph = nullptr;   // shown when there is no icon file
        FlatFader      *fader = nullptr;
        ColorButton    *mute_btn = nullptr, *menu_btn = nullptr;
        LevelMeterPanel *meter = nullptr;
    };

    void BuildLayout();                     // (re)creates every card for the current style
    void BuildCard(Channel &ch, int index, bool vertical);
    void UpdateCardState(Channel &ch);      // label/dim/colours after a mute / online change
    void ApplyChannelColours(Channel &ch);

    Channel &At(int index) { return index < 2 ? slot_[index] : extras_[(size_t)(index - 2)]; }
    int  ChannelCount() const { return 2 + (int)extras_.size(); }
    int  IndexOfWindow(wxObject *o) const;

    void OnFader(wxCommandEvent &evt);
    void OnMute(wxCommandEvent &evt);
    void OnMenu(wxCommandEvent &evt);
    void ShowAddMenu();
    void ShowChannelMenu(int index);
    void PushBuiltinVolumes();
    void PushExtra(Channel &ch);

    // sources
    void AddSource(const AudioSourceDef &def, bool persist);
    void RemoveSource(int index);
    bool ConnectSource(Channel &ch, const AudioSourceDef &def);
    void DisconnectSource(Channel &ch);
    void RetryOfflineSources();
    void BeginFileDecode(const AudioSourceDef &def);
    void OnFileDecoded(const std::string &id, bool ok, const std::string &wav);
    void InitFromState();
    wxBitmap IconBitmapFor(const Channel &ch, int size) const;
    wxString GlyphFor(const Channel &ch) const;
    std::string NextSourceId() const;
    bool AddingBlocked(const wxString &what);

    AppState &state_;
    RecordingController &rec_;
    ThemeColors theme_;

    Channel slot_[2];
    std::vector<Channel> extras_;

    wxScrolledWindow *scroller_ = nullptr;
    wxBoxSizer *outer_ = nullptr;
    ColorButton *add_btn_ = nullptr;
    std::string built_style_;               // style the current layout was built for

    // retry bookkeeping for offline "app"/"mic" sources
    unsigned long last_retry_tick_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);   // guards async file decodes
};

enum AudioPanelControlId {
    ID_AUDIO_FADER_BASE = 2100,   // + channel index
    ID_AUDIO_MUTE_BASE  = 2200,   // + channel index
    ID_AUDIO_MENU_BASE  = 2300,   // + channel index
    ID_AUDIO_ADD        = 2400,
    ID_AUDIO_CLOSE      = 2005,   // legacy id, unused
};
