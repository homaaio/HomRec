#include "audio_panel.h"
#include "recording_controller.h"
#include "../hr_mic_enum.h"
#include "../hr_app_audio_enum.h"
#include "../hr_log.h"
#include <wx/dcbuffer.h>
#include <wx/menu.h>
#include <wx/filedlg.h>
#include <wx/choicdlg.h>
#include <wx/textdlg.h>
#include <wx/filename.h>
#include <cmath>
#include <cstdio>
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <thread>

extern "C" {
    void hr_audio_set_volumes(float mic_vol, float sys_vol, int mic_mute, int sys_mute);
    void hr_audio_get_levels(int *out_mic, int *out_sys);
    void hr_peak_decay(int level, int *peak, int *peak_decay);
    int  hr_audio_extra_add(int kind, const wchar_t *target, unsigned long pid, float vol, int mute);
    void hr_audio_extra_remove(int id);
    void hr_audio_extra_set_volume(int id, float vol, int mute);
    int  hr_audio_extra_level(int id);
    int  hr_decode_audio_to_wav(const wchar_t *ffpath, const wchar_t *in_path, const wchar_t *wav_path);
}

namespace {
constexpr int kZoneYellowStart = 22;  // ~ -20 dBFS
constexpr int kZoneRedStart    = 78;  // ~ -9 dBFS (clipping zone starts here)

std::wstring WideOf(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)std::max(n, 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
std::string NarrowOf(const std::wstring &w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)std::max(n, 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool IsBrowserName(std::string exe) {
    for (auto &c : exe) c = (char)tolower((unsigned char)c);
    static const char *k[] = {"chrome.exe", "msedge.exe", "firefox.exe", "opera.exe", "opera_gx.exe",
                              "brave.exe", "vivaldi.exe", "yandex.exe", "browser.exe", "waterfox.exe",
                              "librewolf.exe", "chromium.exe", "arc.exe", "zen.exe"};
    for (const char *b : k) if (exe == b) return true;
    return false;
}

std::string Truncate(const std::string &s, size_t n) {
    // UTF-8 safe: never cut inside a multi-byte sequence.
    if (s.size() <= n) return s;
    size_t cut = n;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

std::string WithoutExe(std::string s) {
    if (s.size() > 4) {
        std::string tail = s.substr(s.size() - 4);
        for (auto &c : tail) c = (char)tolower((unsigned char)c);
        if (tail == ".exe") s.resize(s.size() - 4);
    }
    return s;
}

wxColour ZoneColour(int zone, bool dimmed) {
    if (dimmed) return wxColour(90, 90, 100);
    if (zone == 2) return wxColour(232, 68, 62);    // red    - clipping zone
    if (zone == 1) return wxColour(230, 190, 60);   // yellow
    return wxColour(110, 200, 130);                 // green
}
} // namespace

// ---------------------------------------------------------------------------
// LevelMeterPanel
// ---------------------------------------------------------------------------
LevelMeterPanel::LevelMeterPanel(wxWindow *parent, bool vertical)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), vertical_(vertical) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &LevelMeterPanel::OnPaint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) { Refresh(false); evt.Skip(); });
}

wxSize LevelMeterPanel::DoGetBestSize() const { return vertical_ ? wxSize(12, 90) : wxSize(90, 10); }

void LevelMeterPanel::SetVertical(bool v) {
    if (vertical_ == v) return;
    vertical_ = v;
    InvalidateBestSize();
    Refresh(false);
}

bool LevelMeterPanel::SetLevel(int level_0_100) {
    const int old_level = level_, old_peak = peak_;
    level_ = std::max(0, std::min(100, level_0_100));
    hr_peak_decay(level_, &peak_, &peak_decay_);
    if (level_ == old_level && peak_ == old_peak) return false;   // nothing visible changed - no repaint
    Refresh(false);
    return true;
}

void LevelMeterPanel::OnPaint(wxPaintEvent &) {
    wxAutoBufferedPaintDC dc(this);
    const wxSize cs = GetClientSize();
    const int w = cs.GetWidth(), h = cs.GetHeight();
    dc.SetBackground(wxBrush(GetParent() ? GetParent()->GetBackgroundColour() : bg_));
    dc.Clear();
    if (w < 2 || h < 2) return;

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(bg_));
    dc.DrawRoundedRectangle(0, 0, w, h, 3);

    const int len = vertical_ ? h : w;           // length along the meter axis
    const int fill = (int)((level_ / 100.0) * len);
    const int yellowPx = (int)((kZoneYellowStart / 100.0) * len);
    const int redPx    = (int)((kZoneRedStart    / 100.0) * len);

    // Draws [from,to) along the meter axis (0 = empty end).
    auto seg = [&](int from, int to, const wxColour &c) {
        if (to <= from) return;
        dc.SetBrush(wxBrush(c));
        if (vertical_) dc.DrawRectangle(1, h - to, w - 2, to - from);
        else           dc.DrawRectangle(from, 1, to - from, h - 2);
    };
    if (fill > 0) {
        seg(0, std::min(fill, yellowPx), ZoneColour(0, dimmed_));
        if (fill > yellowPx) seg(yellowPx, std::min(fill, redPx), ZoneColour(1, dimmed_));
        if (fill > redPx)    seg(redPx, fill, ZoneColour(2, dimmed_));
    }

    // Faint marker where the clipping zone starts - visible even while quiet.
    if (redPx > 0 && redPx < len) {
        dc.SetPen(wxPen(wxColour(232, 68, 62, 160), 1));
        if (vertical_) dc.DrawLine(0, h - redPx, w, h - redPx);
        else           dc.DrawLine(redPx, 0, redPx, h);
    }

    const int peakPx = (int)((peak_ / 100.0) * len);
    if (peakPx > 0 && peakPx < len) {
        dc.SetPen(wxPen(*wxWHITE, 2));
        if (vertical_) dc.DrawLine(0, h - peakPx, w, h - peakPx);
        else           dc.DrawLine(peakPx, 0, peakPx, h);
    }
}

// ---------------------------------------------------------------------------
// AudioPanel
// ---------------------------------------------------------------------------
AudioPanel::AudioPanel(wxWindow *parent, AppState &state, RecordingController &rec)
    : wxPanel(parent), state_(state), rec_(rec), theme_(GetBuiltinTheme("dark")) {
    slot_[0].kind = "builtin_mic"; slot_[0].name = "Microphone";
    slot_[1].kind = "builtin_sys"; slot_[1].name = "Desktop Audio";

    outer_ = new wxBoxSizer(wxVERTICAL);
    SetSizer(outer_);

    // Compact, content-sized button pinned under the cards. It used to be a full-width
    // 26 px bar with 6 px margins, which ate a third of a short dock pane.
    add_btn_ = new ColorButton(this, ID_AUDIO_ADD, wxString::FromUTF8("\uFF0B Add source"));
    add_btn_->SetMinSize(wxSize(104, 22));
    add_btn_->SetMaxSize(wxSize(104, 22));
    add_btn_->SetToolTip("Add another microphone, a browser, a program's sound or an audio file");
    outer_->Add(add_btn_, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT | wxBOTTOM, 4);

    Bind(wxEVT_SLIDER, &AudioPanel::OnFader, this, ID_AUDIO_FADER_BASE, ID_AUDIO_FADER_BASE + 99);
    Bind(wxEVT_BUTTON, &AudioPanel::OnMute, this, ID_AUDIO_MUTE_BASE, ID_AUDIO_MUTE_BASE + 99);
    Bind(wxEVT_BUTTON, &AudioPanel::OnMenu, this, ID_AUDIO_MENU_BASE, ID_AUDIO_MENU_BASE + 99);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { ShowAddMenu(); }, ID_AUDIO_ADD);

    InitFromState();
    ApplyTheme(GetBuiltinTheme("dark"));
}

AudioPanel::~AudioPanel() {
    *alive_ = false;   // late file-decode callbacks must not touch a dead panel
}

void AudioPanel::InitFromState() {
    extras_.clear();
    for (const AudioSourceDef &d : state_.audio_sources) {
        Channel ch;
        ch.id = d.id; ch.kind = d.kind; ch.name = wxString::FromUTF8(d.name);
        ch.target = d.target; ch.window_title = d.window_title;
        ch.vol = std::max(0, std::min(150, d.volume)) / 100.0f;
        ch.muted = d.muted;
        ch.online = false;                       // until ConnectSource() says otherwise
        extras_.push_back(std::move(ch));
        if (extras_.size() >= 32) break;         // sanity bound
    }
    BuildLayout();
    for (size_t i = 0; i < extras_.size() && i < state_.audio_sources.size(); ++i) {
        ConnectSource(extras_[i], state_.audio_sources[i]);
        UpdateCardState(extras_[i]);
    }
}

wxBitmap AudioPanel::IconBitmapFor(const Channel &ch, int size) const {
    HrIcons::Id id = HrIcons::Id::AudioMic;
    if (ch.kind == "builtin_sys") id = HrIcons::Id::AudioDesktop;
    else if (ch.kind == "file")   id = HrIcons::Id::AudioFile;
    else if (ch.kind == "app")    id = IsBrowserName(ch.target) ? HrIcons::Id::AudioBrowser : HrIcons::Id::AudioWindow;
    const wxBitmap &bmp = HrIcons::Get(id, size);
    if (bmp.IsOk()) return bmp;
    // Only the two built-in icons are expected to exist; the extras fall back to the
    // microphone/desktop art (when present) before resorting to a text glyph.
    if (ch.kind == "mic") return wxNullBitmap;
    if (ch.kind != "builtin_mic" && ch.kind != "builtin_sys") {
        const wxBitmap &d = HrIcons::Get(HrIcons::Id::AudioDesktop, size);
        if (d.IsOk()) return d;
    }
    return wxNullBitmap;
}

wxString AudioPanel::GlyphFor(const Channel &ch) const {
    if (ch.kind == "builtin_mic" || ch.kind == "mic") return wxString::FromUTF8("\U0001F3A4");
    if (ch.kind == "builtin_sys") return wxString::FromUTF8("\U0001F50A");
    if (ch.kind == "file")        return wxString::FromUTF8("\U0001F3B5");
    return IsBrowserName(ch.target) ? wxString::FromUTF8("\U0001F310") : wxString::FromUTF8("\U0001F5A5");
}

void AudioPanel::BuildLayout() {
    const bool vertical = state_.audio_meter_style == "vertical";
    built_style_ = vertical ? "vertical" : "horizontal";

    Freeze();
    if (scroller_) {
        outer_->Detach(scroller_);
        scroller_->Destroy();
        scroller_ = nullptr;
    }
    for (int i = 0; i < ChannelCount(); ++i) {
        Channel &c = At(i);
        c.card = nullptr; c.name_lbl = nullptr; c.pct_lbl = nullptr; c.icon = nullptr; c.glyph = nullptr;
        c.fader = nullptr; c.mute_btn = nullptr; c.menu_btn = nullptr; c.meter = nullptr;
    }

    scroller_ = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                     vertical ? wxHSCROLL : wxVSCROLL);
    scroller_->SetScrollRate(vertical ? 10 : 0, vertical ? 0 : 10);
    auto *sz = new wxBoxSizer(vertical ? wxHORIZONTAL : wxVERTICAL);
    for (int i = 0; i < ChannelCount(); ++i) {
        BuildCard(At(i), i, vertical);
        if (vertical) sz->Add(At(i).card, 0, wxEXPAND | wxRIGHT, 6);
        else          sz->Add(At(i).card, 0, wxEXPAND | wxBOTTOM, 4);
    }
    scroller_->SetSizer(sz);
    outer_->Insert(0, scroller_, 1, wxEXPAND | wxALL, 4);

    SetMinSize(wxSize(vertical ? 190 : 170, vertical ? 200 : 90));
    for (int i = 0; i < ChannelCount(); ++i) UpdateCardState(At(i));
    ApplyTheme(theme_);          // also recolours the freshly built widgets
    scroller_->FitInside();
    Layout();
    Thaw();
}

void AudioPanel::BuildCard(Channel &ch, int index, bool vertical) {
    ch.card = new wxPanel(scroller_);
    auto *root = new wxBoxSizer(wxVERTICAL);
    const bool is_extra = index >= 2;
    const int icon_px = 20;

    auto makeIcon = [&]() -> wxWindow * {
        wxBitmap bmp = IconBitmapFor(ch, icon_px);
        if (bmp.IsOk()) {
            ch.icon = new wxStaticBitmap(ch.card, wxID_ANY, bmp);
            return ch.icon;
        }
        ch.glyph = new wxStaticText(ch.card, wxID_ANY, GlyphFor(ch));
        return ch.glyph;
    };

    ch.name_lbl = new wxStaticText(ch.card, wxID_ANY, ch.name, wxDefaultPosition, wxDefaultSize,
                                   wxST_ELLIPSIZE_END | (vertical ? wxALIGN_CENTRE_HORIZONTAL : 0));
    {
        wxFont f = ch.name_lbl->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        ch.name_lbl->SetFont(f);
    }
    ch.pct_lbl = new wxStaticText(ch.card, wxID_ANY, wxString::Format("%d%%", (int)std::lround(ch.vol * 100)),
                                  wxDefaultPosition, wxSize(44, -1),
                                  vertical ? wxALIGN_CENTRE_HORIZONTAL : wxALIGN_RIGHT);
    ch.mute_btn = new ColorButton(ch.card, ID_AUDIO_MUTE_BASE + index, "Mute");
    ch.mute_btn->SetMinSize(wxSize(vertical ? -1 : 64, 24));
    ch.fader = new FlatFader(ch.card, ID_AUDIO_FADER_BASE + index, (int)std::lround(ch.vol * 100), 0, 150, vertical);
    ch.fader->SetToolTip("Drag to set the volume. Mouse wheel: +-5%. Double-click: back to 100%.");
    ch.meter = new LevelMeterPanel(ch.card, vertical);
    if (is_extra) {
        ch.menu_btn = new ColorButton(ch.card, ID_AUDIO_MENU_BASE + index, wxString::FromUTF8("\u2026"));
        ch.menu_btn->SetMinSize(wxSize(vertical ? -1 : 28, 24));
        ch.menu_btn->SetToolTip("Rename / reconnect / remove");
    }

    if (!vertical) {
        // Compact card (~70 px):
        // [icon] Name ............ 100% [Mute] [...]
        // [================ meter ===============]
        // [---------------- fader ----------------]
        ch.pct_lbl->SetMinSize(wxSize(40, -1));
        auto *top = new wxBoxSizer(wxHORIZONTAL);
        top->Add(makeIcon(), 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 6);
        top->Add(ch.name_lbl, 1, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 6);
        top->Add(ch.pct_lbl, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 6);
        top->Add(ch.mute_btn, 0, wxALIGN_CENTRE_VERTICAL);
        if (ch.menu_btn) top->Add(ch.menu_btn, 0, wxALIGN_CENTRE_VERTICAL | wxLEFT, 4);
        root->Add(top, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        ch.meter->SetMinSize(wxSize(60, 8));
        root->Add(ch.meter, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        ch.fader->SetMinSize(wxSize(60, 20));
        // NB: wxEXPAND already overrides every alignment flag in a box sizer; combining it with
        // wxALIGN_* trips a wxWidgets debug assertion ("wxEXPAND overrides alignment flags").
        root->Add(ch.fader, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, 6);
    } else {
        //   [icon]
        //   Name
        //   [meter][fader]
        //   100%
        //   [Mute]  [...]
        ch.card->SetMinSize(wxSize(96, -1));
        root->Add(makeIcon(), 0, wxALIGN_CENTRE_HORIZONTAL | wxTOP, 10);
        ch.name_lbl->SetMinSize(wxSize(84, -1));
        root->Add(ch.name_lbl, 0, wxALIGN_CENTRE_HORIZONTAL | wxLEFT | wxRIGHT | wxTOP, 6);

        auto *mid = new wxBoxSizer(wxHORIZONTAL);
        ch.meter->SetMinSize(wxSize(12, 70));
        ch.fader->SetMinSize(wxSize(26, 70));
        mid->AddStretchSpacer(1);          // centres the meter+fader pair (no wxALIGN_* with wxEXPAND!)
        mid->Add(ch.meter, 0, wxEXPAND | wxRIGHT, 6);
        mid->Add(ch.fader, 0, wxEXPAND);
        mid->AddStretchSpacer(1);
        root->Add(mid, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        root->Add(ch.pct_lbl, 0, wxALIGN_CENTRE_HORIZONTAL | wxTOP, 4);
        root->Add(ch.mute_btn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
        if (ch.menu_btn) root->Add(ch.menu_btn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
        root->AddSpacer(10);
    }
    ch.card->SetSizer(root);
}

void AudioPanel::ApplyChannelColours(Channel &ch) {
    if (!ch.card) return;
    const wxColour cardBg = FromColorref(theme_.surface_light);
    const wxColour text = FromColorref(theme_.text);
    const wxColour dim = FromColorref(theme_.text_secondary);
    ch.card->SetBackgroundColour(cardBg);
    for (wxWindow *w : {(wxWindow *)ch.name_lbl, (wxWindow *)ch.pct_lbl, (wxWindow *)ch.glyph, (wxWindow *)ch.icon}) {
        if (!w) continue;
        w->SetBackgroundColour(cardBg);
    }
    if (ch.name_lbl) ch.name_lbl->SetForegroundColour(text);
    if (ch.pct_lbl)  ch.pct_lbl->SetForegroundColour(dim);
    if (ch.glyph)    ch.glyph->SetForegroundColour(text);
    if (ch.fader) ch.fader->SetColours(cardBg, FromColorref(theme_.surface), FromColorref(theme_.accent), text);
    if (ch.meter) ch.meter->SetBgColour(FromColorref(theme_.preview_bg));
    if (ch.menu_btn) ch.menu_btn->SetColours(FromColorref(theme_.surface), text);
    ch.card->Refresh(false);
}

void AudioPanel::UpdateCardState(Channel &ch) {
    if (!ch.card) return;
    const wxColour neutralBg = FromColorref(theme_.surface);
    const wxColour neutralFg = FromColorref(theme_.text);
    const wxColour mutedBg = FromColorref(theme_.warning);
    const wxColour mutedFg = FromColorref(theme_.bg);

    if (ch.mute_btn) {
        ch.mute_btn->SetLabelText2(ch.muted ? "Unmute" : "Mute");
        ch.mute_btn->SetColours(ch.muted ? mutedBg : neutralBg, ch.muted ? mutedFg : neutralFg);
    }
    const bool off = !ch.online;
    wxString shown = ch.name;
    if (off && ch.id.size()) shown += ch.kind == "file" ? " (loading...)" : " (offline)";
    if (ch.name_lbl) ch.name_lbl->SetLabel(shown);
    if (ch.name_lbl) ch.name_lbl->SetToolTip(ch.kind == "app" && !ch.window_title.empty()
                                              ? wxString::FromUTF8(ch.window_title) : ch.name);
    if (ch.fader) ch.fader->SetDimmed(ch.muted || off);
    if (ch.meter) ch.meter->SetDimmed(ch.muted || off);
    if (ch.pct_lbl) ch.pct_lbl->SetLabel(wxString::Format("%d%%", (int)std::lround(ch.vol * 100)));
}

void AudioPanel::ApplyTheme(const ThemeColors &theme) {
    theme_ = theme;
    const wxColour surface = FromColorref(theme.surface);
    SetBackgroundColour(surface);
    if (scroller_) scroller_->SetBackgroundColour(surface);
    if (add_btn_) add_btn_->SetColours(FromColorref(theme.accent), FromColorref(theme.bg));
    for (int i = 0; i < ChannelCount(); ++i) {
        ApplyChannelColours(At(i));
        UpdateCardState(At(i));
    }
    Refresh(true);
}

void AudioPanel::ApplyMeterStyle() {
    const std::string want = state_.audio_meter_style == "vertical" ? "vertical" : "horizontal";
    if (want != built_style_) BuildLayout();
}

int AudioPanel::IndexOfWindow(wxObject *) const { return -1; }

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------
void AudioPanel::PushBuiltinVolumes() {
    hr_audio_set_volumes(slot_[0].vol, slot_[1].vol, slot_[0].muted ? 1 : 0, slot_[1].muted ? 1 : 0);
}

void AudioPanel::PushExtra(Channel &ch) {
    if (ch.handle > 0) hr_audio_extra_set_volume(ch.handle, ch.vol, ch.muted ? 1 : 0);
    for (AudioSourceDef &d : state_.audio_sources) {
        if (d.id == ch.id) { d.volume = (int)std::lround(ch.vol * 100); d.muted = ch.muted; break; }
    }
}

void AudioPanel::OnFader(wxCommandEvent &evt) {
    const int idx = evt.GetId() - ID_AUDIO_FADER_BASE;
    if (idx < 0 || idx >= ChannelCount()) return;
    Channel &ch = At(idx);
    ch.vol = evt.GetInt() / 100.0f;
    if (ch.pct_lbl) ch.pct_lbl->SetLabel(wxString::Format("%d%%", evt.GetInt()));
    if (idx < 2) PushBuiltinVolumes(); else PushExtra(ch);
}

void AudioPanel::OnMute(wxCommandEvent &evt) {
    const int idx = evt.GetId() - ID_AUDIO_MUTE_BASE;
    if (idx < 0 || idx >= ChannelCount()) return;
    Channel &ch = At(idx);
    ch.muted = !ch.muted;
    UpdateCardState(ch);
    if (idx < 2) PushBuiltinVolumes(); else PushExtra(ch);
}

void AudioPanel::OnMenu(wxCommandEvent &evt) {
    const int idx = evt.GetId() - ID_AUDIO_MENU_BASE;
    if (idx >= 2 && idx < ChannelCount()) ShowChannelMenu(idx);
}

bool AudioPanel::AddingBlocked(const wxString &what) {
    if (!state_.recording) return false;
    wxMessageBox(what + " can't be changed while a recording is running - stop the recording first.",
                 "Audio Mixer", wxOK | wxICON_INFORMATION, this);
    return true;
}

std::string AudioPanel::NextSourceId() const {
    int n = 1;
    for (;;) {
        const std::string id = "as_" + std::to_string(n);
        bool used = false;
        for (const auto &d : state_.audio_sources) if (d.id == id) { used = true; break; }
        if (!used) return id;
        ++n;
    }
}

void AudioPanel::ShowAddMenu() {
    if (AddingBlocked("The list of audio sources")) return;
    if (state_.audio_sources.size() >= 32) {
        wxMessageBox("That's the maximum number of extra audio sources (32).", "Audio Mixer", wxOK | wxICON_INFORMATION, this);
        return;
    }

    enum { kMicBase = 3000, kBrowserBase = 3200, kWindow = 3400, kFile = 3401 };
    const bool loopback_ok = HrProcessLoopbackSupported();
    const std::vector<HrMicDevice> mics = HrEnumerateMics();
    const std::vector<HrAudioApp> browsers = loopback_ok ? HrEnumAudioApps(true) : std::vector<HrAudioApp>();

    wxMenu menu;
    auto *micMenu = new wxMenu();
    if (mics.empty()) micMenu->Append(kMicBase + 199, "(no input devices found)")->Enable(false);
    for (size_t i = 0; i < mics.size() && i < 150; ++i)
        micMenu->Append(kMicBase + (int)i, wxString::FromUTF8(mics[i].name));
    menu.AppendSubMenu(micMenu, "Microphone / input device");

    auto *brMenu = new wxMenu();
    if (!loopback_ok) brMenu->Append(kBrowserBase + 199, "(needs Windows 10 version 2004 or newer)")->Enable(false);
    else if (browsers.empty()) brMenu->Append(kBrowserBase + 199, "(no browser is running)")->Enable(false);
    for (size_t i = 0; i < browsers.size() && i < 150; ++i)
        brMenu->Append(kBrowserBase + (int)i, wxString::FromUTF8(WithoutExe(browsers[i].exe) + "  -  " + Truncate(browsers[i].title, 40)));
    menu.AppendSubMenu(brMenu, "Browser");

    menu.Append(kWindow, wxString::FromUTF8("Window / program sound\u2026"))->Enable(loopback_ok);
    menu.Append(kFile, wxString::FromUTF8("Audio file\u2026"));

    const wxPoint pos = add_btn_->GetPosition() + wxPoint(0, add_btn_->GetSize().GetHeight());
    const int cmd = GetPopupMenuSelectionFromUser(menu, pos);

    AudioSourceDef def;
    def.id = NextSourceId();
    if (cmd >= kMicBase && cmd < kMicBase + (int)mics.size()) {
        def.kind = "mic";
        def.name = mics[(size_t)(cmd - kMicBase)].name;
        def.target = mics[(size_t)(cmd - kMicBase)].id;
    } else if (cmd >= kBrowserBase && cmd < kBrowserBase + (int)browsers.size()) {
        const HrAudioApp &a = browsers[(size_t)(cmd - kBrowserBase)];
        def.kind = "app";
        def.name = WithoutExe(a.exe);
        def.target = a.exe;
        def.window_title = a.title;
    } else if (cmd == kWindow) {
        const std::vector<HrAudioApp> apps = HrEnumAudioApps(false);
        if (apps.empty()) {
            wxMessageBox("No windows with sound-capable programs were found.", "Audio Mixer", wxOK | wxICON_INFORMATION, this);
            return;
        }
        wxArrayString labels;
        for (const auto &a : apps) labels.Add(wxString::FromUTF8(WithoutExe(a.exe) + "  -  " + Truncate(a.title, 60)));
        wxSingleChoiceDialog dlg(this, "Capture only the sound of this program:", "Window / program sound", labels);
        if (dlg.ShowModal() != wxID_OK) return;
        const HrAudioApp &a = apps[(size_t)dlg.GetSelection()];
        def.kind = "app";
        def.name = Truncate(a.title.empty() ? WithoutExe(a.exe) : a.title, 28);
        def.target = a.exe;
        def.window_title = a.title;
    } else if (cmd == kFile) {
        wxFileDialog dlg(this, "Choose an audio file to mix into the recording", "", "",
                         "Audio / video files|*.mp3;*.wav;*.flac;*.ogg;*.opus;*.m4a;*.aac;*.wma;*.mp4;*.mkv;*.webm|All files (*.*)|*.*",
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK) return;
        def.kind = "file";
        def.target = dlg.GetPath().ToUTF8().data();
        def.name = wxFileName(dlg.GetPath()).GetFullName().ToUTF8().data();
    } else {
        return;   // dismissed
    }
    AddSource(def, /*persist=*/true);
}

void AudioPanel::ShowChannelMenu(int index) {
    if (index < 2 || index >= ChannelCount()) return;
    Channel &ch = At(index);
    enum { kRename = 1, kReset, kReconnect, kRemove };
    wxMenu menu;
    menu.Append(kRename, wxString::FromUTF8("Rename\u2026"));
    menu.Append(kReset, "Reset volume to 100%");
    if (!ch.online && ch.kind != "file") menu.Append(kReconnect, "Reconnect now");
    menu.AppendSeparator();
    menu.Append(kRemove, "Remove source");

    const wxPoint pos = ch.menu_btn ? ch.menu_btn->GetScreenPosition() + wxPoint(0, ch.menu_btn->GetSize().GetHeight())
                                    : wxGetMousePosition();
    const int cmd = GetPopupMenuSelectionFromUser(menu, ScreenToClient(pos));
    switch (cmd) {
        case kRename: {
            wxTextEntryDialog dlg(this, "Name shown in the mixer:", "Rename source", ch.name);
            if (dlg.ShowModal() != wxID_OK) return;
            wxString n = dlg.GetValue();
            n.Trim(true).Trim(false);
            if (n.empty()) return;
            ch.name = n;
            for (auto &d : state_.audio_sources) if (d.id == ch.id) { d.name = n.ToUTF8().data(); break; }
            UpdateCardState(ch);
            if (on_changed) on_changed();
            break;
        }
        case kReset:
            ch.vol = 1.0f;
            if (ch.fader) ch.fader->SetValue(100);
            UpdateCardState(ch);
            PushExtra(ch);
            break;
        case kReconnect:
            if (AddingBlocked("Sources")) return;
            for (const auto &d : state_.audio_sources) if (d.id == ch.id) { DisconnectSource(ch); ConnectSource(ch, d); break; }
            UpdateCardState(ch);
            break;
        case kRemove:
            RemoveSource(index);
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// sources
// ---------------------------------------------------------------------------
void AudioPanel::AddSource(const AudioSourceDef &def, bool persist) {
    Channel ch;
    ch.id = def.id; ch.kind = def.kind; ch.name = wxString::FromUTF8(def.name);
    ch.target = def.target; ch.window_title = def.window_title;
    ch.vol = std::max(0, std::min(150, def.volume)) / 100.0f;
    ch.muted = def.muted;
    ch.online = false;
    if (persist) state_.audio_sources.push_back(def);
    extras_.push_back(std::move(ch));
    BuildLayout();
    ConnectSource(extras_.back(), def);
    UpdateCardState(extras_.back());
    if (persist && on_changed) on_changed();
}

void AudioPanel::RemoveSource(int index) {
    if (index < 2 || index >= ChannelCount()) return;
    if (AddingBlocked("The list of audio sources")) return;
    Channel &ch = At(index);
    const std::string id = ch.id;
    DisconnectSource(ch);
    state_.audio_sources.erase(std::remove_if(state_.audio_sources.begin(), state_.audio_sources.end(),
                               [&](const AudioSourceDef &d) { return d.id == id; }), state_.audio_sources.end());
    extras_.erase(extras_.begin() + (index - 2));
    BuildLayout();
    if (on_changed) on_changed();
}

void AudioPanel::DisconnectSource(Channel &ch) {
    if (ch.handle > 0) hr_audio_extra_remove(ch.handle);
    ch.handle = 0;
    ch.pid = 0;
    ch.online = false;
}

bool AudioPanel::ConnectSource(Channel &ch, const AudioSourceDef &def) {
    ch.handle = 0;
    ch.pid = 0;
    if (def.kind == "mic") {
        const int h = hr_audio_extra_add(0, WideOf(def.target).c_str(), 0, ch.vol, ch.muted ? 1 : 0);
        ch.handle = h > 0 ? h : 0;
    } else if (def.kind == "app") {
        if (HrProcessLoopbackSupported()) {
            const unsigned long pid = HrResolveAudioAppPid(def.target, def.window_title);
            if (pid) {
                const int h = hr_audio_extra_add(1, L"", pid, ch.vol, ch.muted ? 1 : 0);
                if (h > 0) { ch.handle = h; ch.pid = pid; }
            }
        }
    } else if (def.kind == "file") {
        ch.online = false;
        BeginFileDecode(def);          // connects itself when the decode finishes (maybe right now)
        return ch.online;
    }
    ch.online = ch.handle > 0;
    return ch.online;
}

void AudioPanel::BeginFileDecode(const AudioSourceDef &def) {
    if (!rec_.ffmpeg_found()) {
        HrLog::Warn("Audio Mixer: ffmpeg not found - can't prepare '" + def.name + "'.");
        return;
    }
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const size_t h = std::hash<std::string>{}(def.target);
    char hex[32];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);
    const std::string wav = NarrowOf(tmp) + "homrec_ax_" + hex + ".wav";

    if (GetFileAttributesW(WideOf(wav).c_str()) != INVALID_FILE_ATTRIBUTES) {
        OnFileDecoded(def.id, true, wav);            // already decoded earlier
        return;
    }
    const std::wstring ff = rec_.resolved_ffmpeg_path();
    const std::wstring in = WideOf(def.target);
    const std::wstring out = WideOf(wav);
    const std::string id = def.id;
    std::shared_ptr<bool> alive = alive_;
    AudioPanel *self = this;
    std::thread([ff, in, out, id, wav, alive, self]() {
        bool ok = false;
        try { ok = hr_decode_audio_to_wav(ff.c_str(), in.c_str(), out.c_str()) != 0; } catch (...) {}
        if (wxTheApp) {
            wxTheApp->CallAfter([alive, self, id, ok, wav]() {
                if (alive && *alive) self->OnFileDecoded(id, ok, wav);
            });
        }
    }).detach();
}

void AudioPanel::OnFileDecoded(const std::string &id, bool ok, const std::string &wav) {
    for (Channel &ch : extras_) {
        if (ch.id != id) continue;
        if (!ok) {
            HrLog::Warn("Audio Mixer: couldn't decode '" + NarrowOf(ch.name.ToStdWstring()) + "'.");
            ch.online = false;
            UpdateCardState(ch);
            return;
        }
        ch.temp_wav = wav;
        const int h = hr_audio_extra_add(2, WideOf(wav).c_str(), 0, ch.vol, ch.muted ? 1 : 0);
        ch.handle = h > 0 ? h : 0;
        ch.online = ch.handle > 0;
        UpdateCardState(ch);
        return;
    }
}

void AudioPanel::RetryOfflineSources() {
    if (state_.recording || extras_.empty()) return;
    const unsigned long now = GetTickCount();
    if (now - last_retry_tick_ < 3000) return;
    last_retry_tick_ = now;

    for (size_t i = 0; i < extras_.size() && i < state_.audio_sources.size(); ++i) {
        Channel &ch = extras_[i];
        const AudioSourceDef &def = state_.audio_sources[i];
        if (def.kind == "file") continue;
        if (def.kind == "app" && HrProcessLoopbackSupported()) {
            const unsigned long pid = HrResolveAudioAppPid(def.target, def.window_title);
            if (ch.online && pid == ch.pid) continue;          // still the same process
            if (ch.online) DisconnectSource(ch);                // program restarted / closed
            if (pid) { ConnectSource(ch, def); UpdateCardState(ch); }
            else if (ch.card) UpdateCardState(ch);
        } else if (def.kind == "mic" && !ch.online) {
            if (ConnectSource(ch, def)) UpdateCardState(ch);
        }
    }
}

void AudioPanel::PollLevels() {
    if (!IsShownOnScreen()) return;       // docked-away / minimised: nothing to draw
    int mic = 0, sys = 0;
    hr_audio_get_levels(&mic, &sys);
    if (slot_[0].meter) slot_[0].meter->SetLevel(mic);
    if (slot_[1].meter) slot_[1].meter->SetLevel(sys);
    for (Channel &ch : extras_) {
        if (!ch.meter) continue;
        ch.meter->SetLevel(ch.online && ch.handle > 0 ? hr_audio_extra_level(ch.handle) : 0);
    }
    RetryOfflineSources();
}
