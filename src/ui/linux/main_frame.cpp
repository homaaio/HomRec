// main_frame.cpp - the Linux (wxGTK) main window. See main_frame.h for what is real and what is "not now".
#include "main_frame.h"
#include "dialogs.h"
#include "../hrc_config.h"
#include "../preset_dialog.h"
#include "../settings_dialog.h"
#include "../version.h"
#include "../../platform/paths.h"
#include "../../utils/log.h"
#include "../../utils/pc_log.h"
#include "../../utils/plugin_log.h"
#include "../../utils/str_convert.h"
#include "../../utils/wgc_log.h"

#include <algorithm>
#include <cctype>
#include <sys/stat.h>
#include <wx/dir.h>
#include <wx/display.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/utils.h>

namespace {

constexpr int kLeftPanelW = 230;
constexpr int kOuterPad   = 10;

wxFont Font(int pt, bool bold = false, bool mono = false) {
    wxFontInfo fi(pt);
    if (bold) fi.Bold();
    if (mono) fi.Family(wxFONTFAMILY_TELETYPE);
    return wxFont(fi);
}

wxString U(const std::string &s) { return wxString::FromUTF8(s); }

// The AUI perspective string contains '|', ';', '=' and user-language captions. The .hrc format is
// one "key=value" per line, so it is stored hex-encoded (same as the Windows window does).
std::string HexEncodeUtf8(const wxString &w) {
    const wxScopedCharBuffer u = w.utf8_str();
    static const char *d = "0123456789abcdef";
    std::string out;
    out.reserve(u.length() * 2);
    for (size_t i = 0; i < u.length(); ++i) {
        unsigned char c = (unsigned char)u.data()[i];
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}

wxString HexDecodeUtf8(const std::string &h) {
    if (h.empty() || (h.size() & 1)) return wxString();
    std::string raw;
    raw.reserve(h.size() / 2);
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < h.size(); i += 2) {
        int a = val(h[i]), b = val(h[i + 1]);
        if (a < 0 || b < 0) return wxString();
        raw += (char)((a << 4) | b);
    }
    return wxString::FromUTF8(raw.data(), raw.size());
}

// "Fullscreen\tF11" style menu text. Spaces are dropped ("Ctrl + F" -> "Ctrl+F"); a binding wx can't
// parse as an accelerator just leaves the item without a shortcut.
wxString FullscreenMenuText(const wxString &label, const std::string &keys) {
    std::string k;
    for (char c : keys)
        if (!std::isspace(static_cast<unsigned char>(c))) k += c;
    if (k.empty()) return label;
    const wxString wk = wxString::FromUTF8(k);
    wxAcceleratorEntry probe;
    if (!probe.FromString(wk)) return label;
    return label + "\t" + wk;
}

// Where the live picture will be. For now: the "not now" note in the middle.
class PreviewBox : public wxPanel {
public:
    explicit PreviewBox(wxWindow *parent) : wxPanel(parent) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &PreviewBox::OnPaint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent &e) { Refresh(); e.Skip(); });
    }
    void SetText(const wxString &t, wxColour fg) { text_ = t; fg_ = fg; Refresh(); }

private:
    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        dc.SetFont(Font(13));
        dc.SetTextForeground(fg_);
        const wxSize cs = GetClientSize();
        const wxSize ts = dc.GetMultiLineTextExtent(text_);
        dc.DrawLabel(text_, wxRect(0, 0, cs.x, cs.y), wxALIGN_CENTRE);
        (void)ts;
    }
    wxString text_;
    wxColour fg_ = *wxWHITE;
};

}  // namespace

// ===========================================================================
// start up / shut down
// ===========================================================================

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "HomRec", wxDefaultPosition, wxSize(1100, 700)), rec_(state_) {
    // Window icon: the png next to the program (icons/hom.png in the source tree / a tarball).
    {
        const std::string exe = hr::platform::ExecutableDir();
        for (const std::string &p : { exe + "/icons/hom.png", exe + "/../icons/hom.png",
                                       exe + "/../../icons/hom.png", hr::platform::DataDir() + "/hom.png" }) {
            struct stat st;
            if (::stat(p.c_str(), &st) != 0) continue;
            wxIcon icon;
            if (icon.LoadFile(U(p), wxBITMAP_TYPE_PNG)) { SetIcon(icon); break; }
        }
    }

    LoadState();

    HrPcLog::SetEnabled(state_.system_logging_enabled);
    HrPluginLog::SetEnabled(state_.plugin_logging_enabled);
    HrWgcLog::SetEnabled(state_.wgc_logging_enabled);

    lang_ = LanguageTable::Load(state_.current_language, HrLangsDir());
    theme_ = GetBuiltinTheme(state_.current_theme);

    // The saved minimum size comes from a Windows desktop; never let it be bigger than this screen.
    {
        const wxRect area = wxDisplay(wxDisplay::GetFromWindow(this) >= 0 ? (unsigned)wxDisplay::GetFromWindow(this) : 0u).GetClientArea();
        SetMinSize(wxSize(std::min(state_.window_min_w, std::max(640, area.width - 80)),
                          std::min(state_.window_min_h, std::max(420, area.height - 120))));
    }

    BuildMenuBar();

    auto *root = new wxPanel(this);
    auto *rootSizer = new wxBoxSizer(wxVERTICAL);

    // Controls, Preview, Audio Mixer and Layers are wxAUI panes inside dock_host_ (SetupDocking below).
    dock_host_ = new wxPanel(root);
    BuildControls(dock_host_);
    BuildPreview(dock_host_);

    audio_panel_ = new wxPanel(dock_host_);
    {
        auto *s = new wxBoxSizer(wxVERTICAL);
        audio_lbl_ = new wxStaticText(audio_panel_, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
        s->AddStretchSpacer(1);
        s->Add(audio_lbl_, 0, wxEXPAND | wxALL, 12);
        s->AddStretchSpacer(1);
        audio_panel_->SetSizer(s);
    }
    overlays_panel_ = new wxPanel(dock_host_);
    {
        auto *s = new wxBoxSizer(wxVERTICAL);
        overlays_lbl_ = new wxStaticText(overlays_panel_, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
        s->AddStretchSpacer(1);
        s->Add(overlays_lbl_, 0, wxEXPAND | wxALL, 12);
        s->AddStretchSpacer(1);
        overlays_panel_->SetSizer(s);
    }

    rootSizer->Add(dock_host_, 1, wxEXPAND | wxALL, kOuterPad);
    BuildBottomBar(root, rootSizer);
    root->SetSizer(rootSizer);

    auto *frameSizer = new wxBoxSizer(wxVERTICAL);
    frameSizer->Add(root, 1, wxEXPAND);
    SetSizer(frameSizer);

    ApplyThemeColours();
    SetupDocking();
    ApplyLanguageText();
    HrLog::Info("HomRec " HR_APP_VERSION " (Linux) started");

    if (state_.always_on_top) ToggleWindowStyle(wxSTAY_ON_TOP);

    Bind(wxEVT_MENU, &MainFrame::OnMenu, this, ID_FILE_OPEN_RECORDINGS, ID_FILE_EXIT);
    Bind(wxEVT_MENU, &MainFrame::OnMenu, this, ID_VIEW_ALWAYS_ON_TOP, ID_HELP_ABOUT);
    Bind(wxEVT_MENU, &MainFrame::OnMenu, this, ID_VIEW_DOCK_FIRST, ID_VIEW_DOCK_LAST);
    // wxAuiManagerEvents are not command events: they do not bubble up to the frame, so bind on the manager.
    aui_.Bind(wxEVT_AUI_PANE_CLOSE, &MainFrame::OnAuiPaneClose, this);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);
    Bind(wxEVT_BUTTON, &MainFrame::OnStartClicked, this, ID_START_BTN);
    Bind(wxEVT_BUTTON, &MainFrame::OnPauseClicked, this, ID_PAUSE_BTN);

    if (state_.first_launch) {
        // After the window is up, so the wizard has something to sit on.
        CallAfter([this]() { ShowWelcomeDialog(this, state_, theme_); UpdateStatsText(); });
    }
}

MainFrame::~MainFrame() {
    dock_ready_ = false;   // nothing may touch the (about to be uninitialised) AUI manager after this
    aui_.UnInit();         // must happen before dock_host_ and the panes are destroyed
}

void MainFrame::LoadState() {
    const std::wstring hrc_path = HrcConfig::kDefaultSettingsPath;
    const bool have_hrc = HrcConfig::Load(state_, hrc_path);
    if (have_hrc && !state_.settings_path.empty()) {
        const std::wstring custom = HrWide(state_.settings_path);
        if (custom != hrc_path) HrcConfig::Load(state_, custom);
    }
    if (!have_hrc) {
        state_.first_launch = true;
        HrLog::Info("First launch: no settings file yet.");
    }
    HrcConfig::LoadOverlaysOnly(state_.overlays, HrcConfig::kOverlaysAutosavePath);

    // Windows files say "recordings" (a folder next to hr.exe); here the default is ~/Videos.
    if (state_.output_folder.empty() || state_.output_folder == "recordings")
        state_.output_folder = hr::platform::VideosDir();
    if (state_.ffmpeg_path.empty()) state_.ffmpeg_path = hr::platform::FindFfmpeg();
}

void MainFrame::PersistSettings() {
    SaveDockLayoutToState();   // panel positions travel with every settings write
    const std::wstring target = HrcConfig::ResolveSettingsPath(state_);
    HrcConfig::Save(state_, target);
    if (target != HrcConfig::kDefaultSettingsPath) HrcConfig::Save(state_, HrcConfig::kDefaultSettingsPath);
}

void MainFrame::OnClose(wxCloseEvent &evt) {
    PersistSettings();
    HrLog::Info("HomRec closed");
    evt.Skip();   // the default handler destroys the window
}

// ===========================================================================
// menu bar
// ===========================================================================

void MainFrame::BuildMenuBar() {
    if (wxMenuBar *old = GetMenuBar()) {
        SetMenuBar(nullptr);
        delete old;
    }
    auto *menuBar = new wxMenuBar();

    auto *fileMenu = new wxMenu();
    fileMenu->Append(ID_FILE_OPEN_RECORDINGS, U(lang_.Get("open_recordings")));
    fileMenu->Append(ID_FILE_OPEN_CONFIG, "Open Config Folder");
    fileMenu->AppendSeparator();
    fileMenu->Append(ID_FILE_EXPORT_HRC, U(lang_.Get("export_hrc")));
    fileMenu->Append(ID_FILE_IMPORT_HRC, U(lang_.Get("import_hrc")));
    fileMenu->Append(ID_FILE_IMPORT_HRP, U(lang_.Get("import_hrp")));
    fileMenu->Append(ID_FILE_SET_PRESET, U(lang_.Get("set_preset")));
    fileMenu->AppendSeparator();
    fileMenu->Append(ID_FILE_EXIT, U(lang_.Get("exit")) + "\tCtrl+Q");
    menuBar->Append(fileMenu, U(lang_.Get("file_menu")));

    auto *viewMenu = new wxMenu();
    viewMenu->AppendCheckItem(ID_VIEW_ALWAYS_ON_TOP, U(lang_.Get("always_on_top")));
    viewMenu->Check(ID_VIEW_ALWAYS_ON_TOP, state_.always_on_top);
    viewMenu->Append(ID_VIEW_FULLSCREEN, FullscreenMenuText(U(lang_.Get("fullscreen")), state_.hotkey_fullscreen));
    viewMenu->AppendCheckItem(ID_VIEW_OVERLAYS_PANEL, U(lang_.Get("layers_panel")));
    viewMenu->Check(ID_VIEW_OVERLAYS_PANEL, state_.show_overlays_panel);
    viewMenu->AppendCheckItem(ID_VIEW_AUDIO_PANEL, U(lang_.Get("audio_mixer")));
    viewMenu->Check(ID_VIEW_AUDIO_PANEL, state_.show_audio_panel);
    {
        // Send a panel straight to a side or tear it out into its own window. (You can also just
        // drag a panel's title bar - wxAUI shows where it will dock.)
        auto *moveMenu = new wxMenu();
        const wxString paneNames[3] = { "Controls (Start / Pause)", U(lang_.Get("audio_mixer")), U(lang_.Get("layers_panel")) };
        const wxString slotNames[5] = { "Left", "Right", "Top", "Bottom", "Floating window" };
        for (int p = 0; p < 3; ++p) {
            auto *sub = new wxMenu();
            for (int sl = 0; sl < 5; ++sl) sub->Append(ID_VIEW_DOCK_FIRST + p * 8 + sl, slotNames[sl]);
            moveMenu->AppendSubMenu(sub, paneNames[p]);
        }
        viewMenu->AppendSubMenu(moveMenu, "Move panel to");
        viewMenu->AppendCheckItem(ID_VIEW_LOCK_UI, "Lock layout");
        viewMenu->Check(ID_VIEW_LOCK_UI, state_.ui_locked);
        viewMenu->Append(ID_VIEW_RESET_LAYOUT, "Reset layout");
    }
    viewMenu->AppendSeparator();
    viewMenu->Append(ID_VIEW_PC_ANALYTICS, U(lang_.Get("pc_analytics")));
    viewMenu->Append(ID_VIEW_LOG, U(lang_.Get("show_log")));
    menuBar->Append(viewMenu, U(lang_.Get("view_menu")));

    auto *themeMenu = new wxMenu();
    themeMenu->Append(ID_THEME_DARK, U(lang_.Get("dark")));
    themeMenu->Append(ID_THEME_LIGHT, U(lang_.Get("light")));
    auto *settingsMenu = new wxMenu();
    settingsMenu->Append(ID_SETTINGS_OPEN, U(lang_.Get("preferences")) + "\tCtrl+,");
    settingsMenu->AppendSubMenu(themeMenu, U(lang_.Get("theme")));
    menuBar->Append(settingsMenu, U(lang_.Get("settings_menu")));

    auto *helpMenu = new wxMenu();
    helpMenu->Append(ID_HELP_CHECK_UPDATES, U(lang_.Get("check_updates")));
    helpMenu->Append(ID_HELP_CONSOLE, U(lang_.Get("console")) + "\tCtrl+Shift+T");
    helpMenu->Append(ID_HELP_WELCOME, U(lang_.Get("show_welcome")));
    helpMenu->Append(ID_HELP_ABOUT, U(lang_.Get("about")));
    menuBar->Append(helpMenu, U(lang_.Get("help_menu")));

    SetMenuBar(menuBar);
}

// ===========================================================================
// panes
// ===========================================================================

void MainFrame::BuildControls(wxWindow *parent) {
    left_panel_ = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(kLeftPanelW + 30, 420));

    start_btn_ = new ColorButton(left_panel_, ID_START_BTN, U(lang_.Get("start")));
    start_btn_->SetFont(Font(11, true));
    start_btn_->SetMinSize(wxSize(-1, 48));

    pause_btn_ = new ColorButton(left_panel_, ID_PAUSE_BTN, U(lang_.Get("pause")));
    pause_btn_->SetFont(Font(10, true));
    pause_btn_->SetMinSize(wxSize(-1, 32));
    pause_btn_->Enable2(false);

    auto section = [&](const wxString &t) {
        auto *l = new wxStaticText(left_panel_, wxID_ANY, t);
        l->SetFont(Font(11, true));
        return l;
    };
    section_time_lbl_ = section(U(lang_.Get("time")));
    time_lbl_ = new wxStaticText(left_panel_, wxID_ANY, "00:00:00", wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    time_lbl_->SetFont(Font(24, true, true));

    section_stats_lbl_ = section(U(lang_.Get("stats")));
    fps_lbl_ = new wxStaticText(left_panel_, wxID_ANY, "");
    fps_lbl_->SetFont(Font(11, false, true));
    res_lbl_ = new wxStaticText(left_panel_, wxID_ANY, "");
    res_lbl_->SetFont(Font(11, false, true));

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(start_btn_, 0, wxEXPAND | wxTOP, 16);
    sizer->Add(pause_btn_, 0, wxEXPAND | wxTOP, 4);
    sizer->Add(section_time_lbl_, 0, wxEXPAND | wxTOP, 15);
    sizer->Add(time_lbl_, 0, wxEXPAND | wxTOP, 8);
    sizer->Add(section_stats_lbl_, 0, wxEXPAND | wxTOP, 15);
    sizer->Add(fps_lbl_, 0, wxEXPAND | wxTOP, 4);
    sizer->Add(res_lbl_, 0, wxEXPAND | wxTOP, 2);
    sizer->AddStretchSpacer(1);
    auto *padded = new wxBoxSizer(wxVERTICAL);
    padded->Add(sizer, 1, wxEXPAND | wxLEFT | wxRIGHT, 15);
    left_panel_->SetSizer(padded);
}

void MainFrame::BuildPreview(wxWindow *parent) {
    preview_container_ = new wxPanel(parent);
    auto *sizer = new wxBoxSizer(wxVERTICAL);

    preview_header_ = new wxPanel(preview_container_);
    auto *hs = new wxBoxSizer(wxHORIZONTAL);
    preview_title_lbl_ = new wxStaticText(preview_header_, wxID_ANY, "");
    preview_title_lbl_->SetFont(Font(11, true));
    preview_note_lbl_ = new wxStaticText(preview_header_, wxID_ANY, "");
    preview_note_lbl_->SetFont(Font(9));
    hs->Add(preview_title_lbl_, 0, wxALIGN_CENTRE_VERTICAL | wxLEFT, 10);
    hs->AddStretchSpacer(1);
    hs->Add(preview_note_lbl_, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 10);
    preview_header_->SetSizer(hs);
    preview_header_->SetMinSize(wxSize(-1, 30));
    sizer->Add(preview_header_, 0, wxEXPAND);

    preview_box_ = new PreviewBox(preview_container_);
    sizer->Add(preview_box_, 1, wxEXPAND);
    preview_container_->SetSizer(sizer);
}

void MainFrame::BuildBottomBar(wxWindow *parent, wxSizer *parentSizer) {
    bottom_bar_ = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 32));
    auto *sizer = new wxBoxSizer(wxHORIZONTAL);

    bottom_dot_ = new StatusDot(bottom_bar_, FromColorref(theme_.text_secondary), 14);
    sizer->Add(bottom_dot_, 0, wxALIGN_CENTRE_VERTICAL | wxLEFT, kOuterPad);

    file_lbl_ = new wxStaticText(bottom_bar_, wxID_ANY, U(lang_.Get("ready")));
    sizer->Add(file_lbl_, 1, wxALIGN_CENTRE_VERTICAL | wxLEFT, 8);

    made_by_lbl_ = new wxStaticText(bottom_bar_, wxID_ANY, U(lang_.Get("made_by")));
    made_by_lbl_->SetFont(Font(9, true));
    sizer->Add(made_by_lbl_, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 10);

    version_bar_lbl_ = new wxStaticText(bottom_bar_, wxID_ANY, "v" HR_APP_VERSION);
    version_bar_lbl_->SetFont(Font(8));
    sizer->Add(version_bar_lbl_, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, kOuterPad);

    bottom_bar_->SetSizer(sizer);
    parentSizer->Add(bottom_bar_, 0, wxEXPAND);
}

void MainFrame::SetStatusState(const wxString &text, COLORREF dot_colour) {
    if (file_lbl_) file_lbl_->SetLabel(text);
    if (bottom_dot_) bottom_dot_->SetColor(FromColorref(dot_colour));
}

void MainFrame::UpdateStatsText() {
    if (fps_lbl_) fps_lbl_->SetLabel(U(lang_.Get("fps")) + wxString::Format(" %d", state_.target_fps));
    if (res_lbl_) {
        wxString r;
        if (state_.resolution_mode == ResolutionMode::Absolute)
            r = wxString::Format("%dx%d", state_.resolution_w, state_.resolution_h);
        else
            r = wxString::Format("%d%%", (int)(state_.scale_factor * 100.0 + 0.5));
        res_lbl_->SetLabel(U(lang_.Get("resolution")) + " " + r);
    }
    if (left_panel_) left_panel_->Layout();
}

// ===========================================================================
// theme + language
// ===========================================================================

void MainFrame::ApplyThemeColours() {
    const wxColour bg = FromColorref(theme_.bg);
    const wxColour surface = FromColorref(theme_.surface);
    const wxColour surfaceLight = FromColorref(theme_.surface_light);
    const wxColour previewBg = FromColorref(theme_.preview_bg);
    const wxColour text = FromColorref(theme_.text);
    const wxColour textSecondary = FromColorref(theme_.text_secondary);
    const wxColour accent = FromColorref(theme_.accent);

    auto label = [](wxStaticText *l, wxColour fg, wxColour back) {
        if (!l) return;
        l->SetForegroundColour(fg);
        l->SetBackgroundColour(back);
    };

    SetBackgroundColour(bg);
    if (dock_host_) dock_host_->SetBackgroundColour(bg);
    if (left_panel_) left_panel_->SetBackgroundColour(surface);
    if (preview_container_) preview_container_->SetBackgroundColour(surfaceLight);
    if (preview_header_) preview_header_->SetBackgroundColour(surfaceLight);
    if (preview_box_) {
        preview_box_->SetBackgroundColour(previewBg);
        static_cast<PreviewBox *>(preview_box_)->SetText(
            "Live preview\nnot now", textSecondary);
    }
    if (audio_panel_) audio_panel_->SetBackgroundColour(surface);
    if (overlays_panel_) overlays_panel_->SetBackgroundColour(surface);
    if (bottom_bar_) bottom_bar_->SetBackgroundColour(surface);

    label(section_time_lbl_, accent, surface);
    label(section_stats_lbl_, accent, surface);
    label(time_lbl_, accent, surface);
    label(fps_lbl_, text, surface);
    label(res_lbl_, text, surface);
    label(preview_title_lbl_, accent, surfaceLight);
    label(preview_note_lbl_, textSecondary, surfaceLight);
    label(audio_lbl_, textSecondary, surface);
    label(overlays_lbl_, textSecondary, surface);
    label(file_lbl_, text, surface);
    label(made_by_lbl_, textSecondary, surface);
    label(version_bar_lbl_, textSecondary, surface);

    if (start_btn_) start_btn_->SetColours(FromColorref(theme_.success), bg);
    if (pause_btn_) pause_btn_->SetColours(FromColorref(theme_.warning), bg);
    if (bottom_dot_) bottom_dot_->SetColor(textSecondary);
    if (dock_ready_) ApplyDockArtColours();

    Refresh(true);
}

void MainFrame::ApplyLanguageText() {
    SetTitle("HomRec v" HR_APP_VERSION);
    BuildMenuBar();

    if (dock_ready_) {
        // Pane captions (the perspective restored at startup carries the old language's text).
        aui_.GetPane("controls").Caption("HomRec");
        aui_.GetPane("audio").Caption(U(lang_.Get("audio_mixer")));
        aui_.GetPane("overlays").Caption(U(lang_.Get("layers_panel")));
        aui_.Update();
    }

    if (start_btn_) start_btn_->SetLabelText2(U(lang_.Get("start")));
    if (pause_btn_) pause_btn_->SetLabelText2(U(lang_.Get("pause")));
    if (section_time_lbl_) section_time_lbl_->SetLabel(U(lang_.Get("time")));
    if (section_stats_lbl_) section_stats_lbl_->SetLabel(U(lang_.Get("stats")));
    if (preview_title_lbl_) preview_title_lbl_->SetLabel(U("\xE2\x97\x8F ") + U(lang_.Get("live_preview")));
    if (preview_note_lbl_) preview_note_lbl_->SetLabel("not now");
    if (audio_lbl_) audio_lbl_->SetLabel(U(lang_.Get("audio_mixer")) + "\n\nnot now - comes with the audio backend");
    if (overlays_lbl_) overlays_lbl_->SetLabel(U(lang_.Get("layers_panel")) + "\n\nnot now - comes with overlays");
    if (made_by_lbl_) made_by_lbl_->SetLabel(U(lang_.Get("made_by")));
    SetStatusState(U(lang_.Get("ready")), theme_.text_secondary);
    UpdateStatsText();

    // Translated strings vary a lot in length, so re-layout instead of keeping the old sizes.
    if (left_panel_) left_panel_->Layout();
    Layout();
}

// ===========================================================================
// docking (wxAUI)
// ===========================================================================

void MainFrame::ApplyDockArtColours() {
    wxAuiDockArt *art = aui_.GetArtProvider();
    if (!art) return;
    const wxColour bg = FromColorref(theme_.bg);
    const wxColour surface = FromColorref(theme_.surface);
    const wxColour surfaceLight = FromColorref(theme_.surface_light);
    const wxColour accent = FromColorref(theme_.accent);
    const wxColour text = FromColorref(theme_.text);
    art->SetColour(wxAUI_DOCKART_BACKGROUND_COLOUR, bg);
    art->SetColour(wxAUI_DOCKART_SASH_COLOUR, bg);
    art->SetColour(wxAUI_DOCKART_BORDER_COLOUR, surfaceLight);
    art->SetColour(wxAUI_DOCKART_GRIPPER_COLOUR, surfaceLight);
    art->SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_COLOUR, surfaceLight);
    art->SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_GRADIENT_COLOUR, surfaceLight);
    art->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_COLOUR, surface);
    art->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_GRADIENT_COLOUR, surface);
    art->SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_TEXT_COLOUR, accent);
    art->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_TEXT_COLOUR, text);
    art->SetMetric(wxAUI_DOCKART_GRADIENT_TYPE, wxAUI_GRADIENT_NONE);
    art->SetMetric(wxAUI_DOCKART_SASH_SIZE, 6);
    art->SetMetric(wxAUI_DOCKART_CAPTION_SIZE, 22);
    art->SetMetric(wxAUI_DOCKART_PANE_BORDER_SIZE, 1);
    aui_.Update();
}

void MainFrame::SetupDocking() {
    aui_.SetManagedWindow(dock_host_);
    aui_.SetFlags(wxAUI_MGR_ALLOW_FLOATING | wxAUI_MGR_TRANSPARENT_HINT | wxAUI_MGR_NO_VENETIAN_BLINDS_FADE);

    aui_.AddPane(preview_container_, wxAuiPaneInfo().Name("preview").CenterPane().PaneBorder(false)
                                         .MinSize(wxSize(240, 160)));

    aui_.AddPane(left_panel_, wxAuiPaneInfo().Name("controls").Caption("HomRec")
                     .Left().Layer(1).Position(0)
                     .BestSize(wxSize(kLeftPanelW + 30, 420)).MinSize(wxSize(kLeftPanelW, 300))
                     .FloatingSize(wxSize(kLeftPanelW + 50, 480))
                     .Fixed().CloseButton(false).MaximizeButton(false).PinButton(false));

    aui_.AddPane(audio_panel_, wxAuiPaneInfo().Name("audio")
                     .Caption(U(lang_.Get("audio_mixer")))
                     .Bottom().Layer(0).Position(0)
                     .BestSize(wxSize(520, 160)).MinSize(wxSize(190, 100))
                     .FloatingSize(wxSize(400, 260))
                     .CloseButton(true).MaximizeButton(false).PinButton(false));

    aui_.AddPane(overlays_panel_, wxAuiPaneInfo().Name("overlays")
                     .Caption(U(lang_.Get("layers_panel")))
                     .Right().Layer(1).Position(0)
                     .BestSize(wxSize(250, 400)).MinSize(wxSize(170, 120))
                     .FloatingSize(wxSize(280, 360))
                     .CloseButton(true).MaximizeButton(false).PinButton(false));

    ApplyDockArtColours();
    default_perspective_ = aui_.SavePerspective();   // the factory layout, "Reset layout" returns here

    if (!state_.ui_layout.empty()) {
        const wxString persp = HexDecodeUtf8(state_.ui_layout);
        if (persp.empty() || !aui_.LoadPerspective(persp, false)) {
            HrLog::Warn("Saved panel layout couldn't be restored - using the default layout.");
            aui_.LoadPerspective(default_perspective_, false);
        }
    }

    // The flags in state_ (View menu / the panes' own close buttons) are authoritative for visibility.
    aui_.GetPane("preview").Show(true);
    aui_.GetPane("controls").Show(true);
    aui_.GetPane("audio").Show(state_.show_audio_panel);
    aui_.GetPane("overlays").Show(state_.show_overlays_panel);

    // A layout saved on another monitor setup could leave a floating panel off-screen.
    const int n_displays = (int)wxDisplay::GetCount();
    for (size_t i = 0; i < aui_.GetAllPanes().GetCount(); ++i) {
        wxAuiPaneInfo &p = aui_.GetAllPanes().Item(i);
        if (!p.IsFloating()) continue;
        bool visible = false;
        for (int d = 0; d < n_displays && !visible; ++d)
            visible = wxDisplay((unsigned)d).GetClientArea().Contains(p.floating_pos + wxPoint(40, 12));
        if (!visible) p.FloatingPosition(GetScreenPosition() + wxPoint(70 + (int)i * 30, 130));
    }

    dock_ready_ = true;
    ApplyUiLock();
}

void MainFrame::ApplyUiLock() {
    if (!dock_ready_) return;
    const bool lock = state_.ui_locked;
    wxAuiPaneInfoArray &panes = aui_.GetAllPanes();
    for (size_t i = 0; i < panes.GetCount(); ++i) {
        wxAuiPaneInfo &p = panes.Item(i);
        if (p.name == "preview") continue;
        const bool is_controls = (p.name == "controls");
        p.Movable(!lock).Floatable(!lock).Dockable(!lock);
        p.CloseButton(!lock && !is_controls);
        // Controls is a fixed-size pane by design; the others are resizable unless the layout is locked.
        p.Resizable(!lock && !is_controls);
        p.Gripper(false);
    }
    aui_.Update();
}

void MainFrame::SaveDockLayoutToState() {
    if (!dock_ready_) return;
    state_.ui_layout = HexEncodeUtf8(aui_.SavePerspective());
}

void MainFrame::SetPaneShown(const char *name, bool show, bool persist) {
    if (!dock_ready_) return;
    wxAuiPaneInfo &p = aui_.GetPane(name);
    if (!p.IsOk()) return;
    const std::string n = name;
    if (n == "overlays") state_.show_overlays_panel = show;
    else if (n == "audio") state_.show_audio_panel = show;
    p.Show(show);
    aui_.Update();
    if (auto *mb = GetMenuBar()) {
        if (n == "overlays") mb->Check(ID_VIEW_OVERLAYS_PANEL, show);
        else if (n == "audio") mb->Check(ID_VIEW_AUDIO_PANEL, show);
    }
    if (persist) PersistSettings();
}

void MainFrame::OnAuiPaneClose(wxAuiManagerEvent &evt) {
    wxAuiPaneInfo *p = evt.GetPane();
    if (!p) return;
    if (state_.ui_locked || p->name == "controls" || p->name == "preview") {
        evt.Veto();
        return;
    }
    if (p->name == "overlays") {
        state_.show_overlays_panel = false;
        if (auto *mb = GetMenuBar()) mb->Check(ID_VIEW_OVERLAYS_PANEL, false);
    } else if (p->name == "audio") {
        state_.show_audio_panel = false;
        if (auto *mb = GetMenuBar()) mb->Check(ID_VIEW_AUDIO_PANEL, false);
    }
    // The pane itself is hidden by the manager right after this handler returns; defer the write
    // so SavePerspective() sees the final (hidden) state.
    CallAfter([this]() { PersistSettings(); });
}

void MainFrame::DockPaneTo(const char *name, DockSlot slot) {
    if (!dock_ready_) return;
    if (state_.ui_locked) {
        wxMessageBox("The panel layout is locked. Unlock it first (View > Lock layout).", "HomRec",
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    wxAuiPaneInfo &p = aui_.GetPane(name);
    if (!p.IsOk()) return;
    const std::string n = name;
    if (n == "audio" && !state_.show_audio_panel) SetPaneShown("audio", true, false);
    if (n == "overlays" && !state_.show_overlays_panel) SetPaneShown("overlays", true, false);

    p.Show(true);
    switch (slot) {
        case kDockLeft:   p.Dock().Left().Layer(1).Row(0).Position(n == "controls" ? 0 : 1); break;
        case kDockRight:  p.Dock().Right().Layer(1).Row(0).Position(n == "overlays" ? 0 : 1); break;
        case kDockTop:    p.Dock().Top().Layer(1).Row(0).Position(0); break;
        case kDockBottom: p.Dock().Bottom().Layer(0).Row(0).Position(n == "audio" ? 0 : 1); break;
        case kDockFloat:
            p.Float();
            p.FloatingPosition(GetScreenPosition() + wxPoint(90, 140));
            break;
    }
    aui_.Update();
    PersistSettings();
}

void MainFrame::ResetDockLayout() {
    if (!dock_ready_) return;
    if (state_.ui_locked) {
        wxMessageBox("The panel layout is locked. Unlock it first (View > Lock layout).", "HomRec",
                     wxOK | wxICON_INFORMATION, this);
        return;
    }
    aui_.LoadPerspective(default_perspective_, false);
    aui_.GetPane("preview").Show(true);
    aui_.GetPane("controls").Show(true);
    aui_.GetPane("audio").Show(state_.show_audio_panel);
    aui_.GetPane("overlays").Show(state_.show_overlays_panel);
    ApplyUiLock();   // re-applies pane flags + Update()
    PersistSettings();
}

// ===========================================================================
// actions
// ===========================================================================

void MainFrame::OnStartClicked(wxCommandEvent &) { ShowNotNow(this, "Recording"); }
void MainFrame::OnPauseClicked(wxCommandEvent &) { ShowNotNow(this, "Recording"); }

void MainFrame::OpenFolder(const std::string &path, const wxString &what) {
    hr::platform::EnsureDir(path);
    if (!wxLaunchDefaultApplication(U(path)))
        wxMessageBox("Couldn't open the " + what + ":\n" + U(path) + "\n\nIs a file manager installed (xdg-open)?",
                     "HomRec", wxOK | wxICON_WARNING, this);
}

void MainFrame::OpenRecordingsFolder() { OpenFolder(state_.output_folder, "recordings folder"); }

void MainFrame::ExportSettings() {
    wxFileDialog dlg(this, "Export Settings", wxEmptyString, "homrec_config.hrc",
                     "HomRec Config (*.hrc)|*.hrc", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    const std::string logPath = dlg.GetPath().ToUTF8().data();
    if (HrcConfig::Save(state_, HrWide(logPath))) {
        HrLog::Info("Exported settings to " + logPath);
    } else {
        HrLog::Error("Failed to export settings to " + logPath);
        wxMessageBox("Couldn't write that file.", "Export Settings", wxOK | wxICON_ERROR, this);
    }
}

// Re-applies everything that depends on state_ after it was replaced (import, preset, settings dialog).
void MainFrame::AfterSettingsChanged() {
    lang_ = LanguageTable::Load(state_.current_language, HrLangsDir());
    theme_ = GetBuiltinTheme(state_.current_theme);
    HrPcLog::SetEnabled(state_.system_logging_enabled);
    HrPluginLog::SetEnabled(state_.plugin_logging_enabled);
    HrWgcLog::SetEnabled(state_.wgc_logging_enabled);
    ApplyThemeColours();
    ApplyLanguageText();
    ApplyUiLock();
    SetPaneShown("audio", state_.show_audio_panel, false);
    SetPaneShown("overlays", state_.show_overlays_panel, false);
    if (auto *mb = GetMenuBar()) mb->Check(ID_VIEW_LOCK_UI, state_.ui_locked);
}

void MainFrame::ImportSettings() {
    wxFileDialog dlg(this, "Import Settings", wxEmptyString, wxEmptyString,
                     "HomRec Config (*.hrc)|*.hrc|All files (*.*)|*.*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    const std::string logPath = dlg.GetPath().ToUTF8().data();
    if (HrcConfig::Load(state_, HrWide(logPath))) {
        HrLog::Info("Imported settings from " + logPath);
        AfterSettingsChanged();
        PersistSettings();
        wxMessageBox("Settings imported.", "Import Settings", wxOK | wxICON_INFORMATION, this);
    } else {
        HrLog::Error("Failed to import settings from " + logPath);
        wxMessageBox("Couldn't read that file.", "Import Settings", wxOK | wxICON_ERROR, this);
    }
}

void MainFrame::OnMenu(wxCommandEvent &evt) {
    const int id = evt.GetId();
    if (id >= ID_VIEW_DOCK_FIRST && id <= ID_VIEW_DOCK_LAST) {
        static const char *const kPaneNames[3] = { "controls", "audio", "overlays" };
        const int p = (id - ID_VIEW_DOCK_FIRST) / 8, sl = (id - ID_VIEW_DOCK_FIRST) % 8;
        if (p >= 0 && p < 3 && sl >= 0 && sl <= kDockFloat) DockPaneTo(kPaneNames[p], (DockSlot)sl);
        return;
    }
    switch (id) {
        case ID_FILE_OPEN_RECORDINGS: OpenRecordingsFolder(); break;
        case ID_FILE_OPEN_CONFIG:     OpenFolder(hr::platform::ConfigDir(), "config folder"); break;
        case ID_FILE_EXPORT_HRC:      ExportSettings(); break;
        case ID_FILE_IMPORT_HRC:      ImportSettings(); break;
        case ID_FILE_IMPORT_HRP:      ShowNotNow(this, "Plugins (.hrp)"); break;
        case ID_FILE_SET_PRESET:
            if (ShowPresetDialog(this, state_, theme_, lang_)) {
                AfterSettingsChanged();
                PersistSettings();
            }
            break;
        case ID_FILE_EXIT: Close(true); break;

        case ID_VIEW_ALWAYS_ON_TOP:
            state_.always_on_top = !state_.always_on_top;
            ToggleWindowStyle(wxSTAY_ON_TOP);
            if (auto *mb = GetMenuBar()) mb->Check(ID_VIEW_ALWAYS_ON_TOP, state_.always_on_top);
            break;
        case ID_VIEW_FULLSCREEN: ShowFullScreen(!IsFullScreen()); break;
        case ID_VIEW_OVERLAYS_PANEL: SetPaneShown("overlays", !state_.show_overlays_panel, true); break;
        case ID_VIEW_AUDIO_PANEL:    SetPaneShown("audio", !state_.show_audio_panel, true); break;
        case ID_VIEW_LOCK_UI:
            state_.ui_locked = !state_.ui_locked;
            ApplyUiLock();
            if (auto *mb = GetMenuBar()) mb->Check(ID_VIEW_LOCK_UI, state_.ui_locked);
            PersistSettings();
            break;
        case ID_VIEW_RESET_LAYOUT: ResetDockLayout(); break;
        case ID_VIEW_PC_ANALYTICS: ShowPcAnalyticsDialog(this, theme_, state_.output_folder); break;
        case ID_VIEW_LOG: ShowLogViewerDialog(this, theme_); break;

        case ID_THEME_DARK:
            state_.current_theme = "dark";
            theme_ = GetBuiltinTheme("dark");
            ApplyThemeColours();
            PersistSettings();
            break;
        case ID_THEME_LIGHT:
            state_.current_theme = "light";
            theme_ = GetBuiltinTheme("light");
            ApplyThemeColours();
            PersistSettings();
            break;

        case ID_SETTINGS_OPEN:
            // The dialog saves the .hrc file itself when OK is pressed.
            if (ShowSettingsDialog(this, state_, theme_, lang_, &rec_)) AfterSettingsChanged();
            break;

        case ID_HELP_CHECK_UPDATES: ShowNotNow(this, "Update check"); break;
        case ID_HELP_CONSOLE: ShowConsoleWindow(this, state_, theme_); break;
        case ID_HELP_WELCOME: ShowWelcomeDialog(this, state_, theme_); UpdateStatsText(); break;
        case ID_HELP_ABOUT:   ShowAboutDialog(this, theme_); break;
        default: break;
    }
}
