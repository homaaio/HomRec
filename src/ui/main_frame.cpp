#include "main_frame.h"
#include "version.h"
#include "settings_dialog.h"
#include "overlay_manager.h"
#include "welcome_dialog.h"
#include "pc_analytics_dialog.h"
#include "log_viewer_dialog.h"
#include "window_picker_dialog.h"
#include "../hr_log.h"
#include <wx/dcbuffer.h>
#include <wx/msw/private.h>
#include <functional>
#include <algorithm>
#include <string>

extern "C" {
    void *hr_hk_create();
    void hr_hk_destroy(void *handle);
    void hr_hk_set_callbacks(void *handle, void (*start_stop)(), void (*pause)(), void (*fullscreen)());
    void hr_hk_configure(void *handle, const char *start_stop_str, const char *pause_str, const char *fullscreen_str);
    int hr_hk_start(void *handle);
    void hr_hk_stop(void *handle);

    void *hr_settings_create();
    void hr_settings_destroy(void *handle);
    int hr_settings_load(void *handle, const char *path);
    const char *hr_settings_get_output_folder(const void *h);
    int hr_settings_get_quality(const void *h);
    int hr_settings_get_fps(const void *h);
    int hr_settings_get_monitor(const void *h);
    int hr_settings_get_flag(const void *h, const char *name);
}

// FromColorref/ColorButton/StatusDot moved to themed_widgets.cpp.

// Raw-Win32 OverlaysDockPanel creates real child HWNDs (list/buttons)
// owner-drawn level meters) that need WM_HSCROLL/WM_DRAWITEM delivered —
// Windows sends both to the control's *immediate parent* HWND, not the
// top-level frame. This wxPanel subclass exists purely to be that immediate
// parent and forward the two messages on, via MSWWindowProc (the same hook
// wx itself uses internally for this sort of thing).
//
// Defined here at global/file scope (NOT inside the anonymous namespace
// below) because main_frame.h forward-declares it at global scope too
// (`class NativeHostPanel *overlays_host_`) — a class defined inside an
// anonymous namespace is a different, invisible type from one of the same
// name forward-declared at global scope, which is exactly what produced
// the "invalid use of incomplete type" build errors.
class NativeHostPanel : public wxPanel {
public:
    explicit NativeHostPanel(wxWindow *parent) : wxPanel(parent) {}
    std::function<void(HWND, int)> on_hscroll;
    std::function<void(DRAWITEMSTRUCT *)> on_drawitem;

protected:
    WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam) override {
        if (nMsg == WM_HSCROLL && on_hscroll) {
            on_hscroll(reinterpret_cast<HWND>(lParam), LOWORD(wParam));
            return 0;
        }
        if (nMsg == WM_DRAWITEM && on_drawitem) {
            on_drawitem(reinterpret_cast<DRAWITEMSTRUCT *>(lParam));
            return TRUE;
        }
        return wxPanel::MSWWindowProc(nMsg, wParam, lParam);
    }
};

namespace {
// Only one main frame exists per process — the hotkey manager's callbacks
// (hr_hotkey.cpp's HR_HK_CB is a plain no-arg function pointer, see its
// header comment) fire on a background thread and need a way back to "the"
// frame to wxQueueEvent() onto the UI thread; this is it.
HomRecMainFrame *g_frame = nullptr;

constexpr int kOuterPad   = 15;
constexpr int kLeftPanelW = 240;

wxDEFINE_EVENT(EVT_HOTKEY_START_STOP, wxThreadEvent);
wxDEFINE_EVENT(EVT_HOTKEY_PAUSE, wxThreadEvent);
wxDEFINE_EVENT(EVT_HOTKEY_FULLSCREEN, wxThreadEvent);

void HotkeyStartStopThunk() { if (g_frame) wxQueueEvent(g_frame, new wxThreadEvent(EVT_HOTKEY_START_STOP)); }
void HotkeyPauseThunk()     { if (g_frame) wxQueueEvent(g_frame, new wxThreadEvent(EVT_HOTKEY_PAUSE)); }
void HotkeyFullscreenThunk(){ if (g_frame) wxQueueEvent(g_frame, new wxThreadEvent(EVT_HOTKEY_FULLSCREEN)); }

class TrayIcon : public wxTaskBarIcon {
public:
    explicit TrayIcon(HomRecMainFrame *frame) : frame_(frame) {}
    wxMenu *CreatePopupMenu() override {
        auto *menu = new wxMenu();
        menu->Append(ID_TRAY_RESTORE, "Restore");
        menu->Append(ID_TRAY_EXIT, "Exit");
        return menu;
    }
private:
    HomRecMainFrame *frame_;
};
} // namespace

// ---------------------------------------------------------------------------
// PreviewPanel
// ---------------------------------------------------------------------------
PreviewPanel::PreviewPanel(wxWindow *parent, RecordingController *&rec, AppState &state)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
      rec_(rec), state_(state) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &PreviewPanel::OnPaint, this);
}

void PreviewPanel::OnPaint(wxPaintEvent &) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();

    int w = 0, h = 0;
    bool got = rec_ && rec_->GetPreviewFrame(frame_buf_, w, h);
    if (!got || w <= 0 || h <= 0) {
        dc.SetTextForeground(wxColour(150, 150, 160));
        wxFont f = GetFont();
        dc.SetFont(f);
        wxSize ext = dc.GetTextExtent(placeholder_);
        wxSize cs = GetClientSize();
        dc.DrawText(placeholder_, (cs.GetWidth() - ext.GetWidth()) / 2, (cs.GetHeight() - ext.GetHeight()) / 2);
        return;
    }

    // hr_pipeline.cpp's bgra_to_thumb() writes true RGB order (o[0]=r,
    // o[1]=g, o[2]=b — see its BGR->RGB nearest-neighbour fallback branch),
    // so this can go straight into a wxImage with no channel swap.
    wxImage img(w, h, frame_buf_.data(), /*static_data=*/true);
    wxSize cs = GetClientSize();
    if (cs.GetWidth() > 0 && cs.GetHeight() > 0) {
        // Preserve aspect ratio, matching Tkinter's Image.thumbnail() used
        // in the Python preview instead of stretching to fill.
        double scale = std::min((double)cs.GetWidth() / w, (double)cs.GetHeight() / h);
        int dw = std::max(1, (int)(w * scale));
        int dh = std::max(1, (int)(h * scale));
        wxImage scaled = img.Scale(dw, dh, wxIMAGE_QUALITY_BILINEAR);
        wxBitmap bmp(scaled);
        dc.DrawBitmap(bmp, (cs.GetWidth() - dw) / 2, (cs.GetHeight() - dh) / 2);
    }
}

// ---------------------------------------------------------------------------
// HomRecMainFrame
// ---------------------------------------------------------------------------
HomRecMainFrame::HomRecMainFrame()
    : wxFrame(nullptr, wxID_ANY, "HomRec", wxDefaultPosition, wxSize(1300, 750)),
      preview_timer_(this), stats_timer_(this) {
    g_frame = this;
    SetIcon(wxIcon("#1", wxBITMAP_TYPE_ICO_RESOURCE));

    lang_ = LanguageTable::Load(state_.current_language, "Assets\\L");
    theme_ = GetBuiltinTheme(state_.current_theme);

    // Load whatever Settings previously saved — see main_window.cpp's
    // original OnCreate() comment; unchanged behavior, just moved here.
    void *settings = hr_settings_create();
    if (hr_settings_load(settings, "homrec_settings.json")) {
        const char *folder = hr_settings_get_output_folder(settings);
        state_.output_folder = (folder && folder[0]) ? folder : "recordings";
        state_.quality = hr_settings_get_quality(settings);
        state_.target_fps = hr_settings_get_fps(settings);
        state_.monitor_id = hr_settings_get_monitor(settings);
        state_.countdown_enabled = hr_settings_get_flag(settings, "countdown") != 0;
        state_.timestamp_enabled = hr_settings_get_flag(settings, "timestamp") != 0;
        state_.cursor_enabled = hr_settings_get_flag(settings, "cursor") != 0;
        state_.show_summary = hr_settings_get_flag(settings, "show_summary") != 0;
        state_.show_overlays_panel = hr_settings_get_flag(settings, "show_overlays_panel") != 0;
    } else {
        state_.output_folder = "recordings";
    }
    hr_settings_destroy(settings);

    SetMinSize(wxSize(state_.window_min_w, state_.window_min_h));

    BuildMenuBar();

    rec_ = std::make_unique<RecordingController>(state_);
    rec_->Initialize();
    rec_raw_ = rec_.get();

    auto *root = new wxPanel(this);
    auto *rootSizer = new wxBoxSizer(wxVERTICAL);
    auto *contentSizer = new wxBoxSizer(wxHORIZONTAL);

    BuildLeftPanel(root, contentSizer);
    BuildPreviewPanel(root, contentSizer);

    rootSizer->Add(contentSizer, 1, wxEXPAND | wxALL, kOuterPad);
    BuildBottomBar(root, rootSizer);
    root->SetSizer(rootSizer);

    auto *frameSizer = new wxBoxSizer(wxVERTICAL);
    frameSizer->Add(root, 1, wxEXPAND);
    SetSizer(frameSizer);

    ApplyThemeColours();
    ApplyLanguageText();
    SetStatusState(wxString::FromUTF8(lang_.Get("ready")), theme_.text_secondary);
    HrLog::Info("HomRec " HR_APP_VERSION " started");

    SetupTrayIcon();
    SetupHotkeys();

    plugins_ = std::make_unique<LuaPluginEngine>("plugins");
    plugins_->SetContext(rec_.get(), &theme_);
    plugins_->LoadAll();

    Bind(wxEVT_TIMER, &HomRecMainFrame::OnPreviewTimer, this, preview_timer_.GetId());
    Bind(wxEVT_TIMER, &HomRecMainFrame::OnStatsTimer, this, stats_timer_.GetId());
    preview_timer_.Start(1000 / 30);
    stats_timer_.Start(500);

    Bind(wxEVT_MENU, &HomRecMainFrame::OnMenu, this, ID_FILE_OPEN_RECORDINGS, ID_VIEW_OVERLAYS_PANEL);
    Bind(wxEVT_CLOSE_WINDOW, &HomRecMainFrame::OnClose, this);
    Bind(wxEVT_ICONIZE, &HomRecMainFrame::OnIconize, this);
    Bind(EVT_HOTKEY_START_STOP, &HomRecMainFrame::OnHotkeyEvent, this);
    Bind(EVT_HOTKEY_PAUSE, &HomRecMainFrame::OnHotkeyEvent, this);
    Bind(EVT_HOTKEY_FULLSCREEN, &HomRecMainFrame::OnHotkeyEvent, this);

    if (state_.first_launch) {
        ShowWelcomeDialog(GetHWND(), wxGetInstance());
    }
}

HomRecMainFrame::~HomRecMainFrame() {
    if (state_.recording && rec_) rec_->Stop();
    if (hotkey_handle_) {
        hr_hk_stop(hotkey_handle_);
        hr_hk_destroy(hotkey_handle_);
    }
    if (tray_icon_) {
        tray_icon_->RemoveIcon();
        delete tray_icon_;
    }
    if (g_frame == this) g_frame = nullptr;
}

void HomRecMainFrame::BuildMenuBar() {
    auto *menuBar = new wxMenuBar();

    auto *fileMenu = new wxMenu();
    fileMenu->Append(ID_FILE_OPEN_RECORDINGS, "Open Recordings Folder");
    fileMenu->Append(ID_FILE_SELECT_WINDOW, "Select Window to Record...");
    fileMenu->AppendSeparator();
    fileMenu->Append(ID_FILE_EXIT, "Exit");
    menuBar->Append(fileMenu, "File");

    auto *viewMenu = new wxMenu();
    viewMenu->Append(ID_VIEW_ALWAYS_ON_TOP, "Always on Top");
    viewMenu->Append(ID_VIEW_FULLSCREEN, "Fullscreen\tF11");
    viewMenu->AppendCheckItem(ID_VIEW_OVERLAYS_PANEL, "Overlays Panel");
    viewMenu->Check(ID_VIEW_OVERLAYS_PANEL, state_.show_overlays_panel);
    viewMenu->AppendSeparator();
    viewMenu->Append(ID_VIEW_PC_ANALYTICS, "PC Analytics");
    viewMenu->Append(ID_VIEW_LOG, "Show Log");
    menuBar->Append(viewMenu, "View");

    auto *themeMenu = new wxMenu();
    themeMenu->Append(ID_THEME_DARK, "Dark");
    themeMenu->Append(ID_THEME_LIGHT, "Light");
    auto *settingsMenu = new wxMenu();
    settingsMenu->Append(ID_SETTINGS_OPEN, "Preferences...");
    settingsMenu->Append(ID_SETTINGS_ADVANCED, "Advanced Settings...");
    settingsMenu->Append(ID_OVERLAYS_MANAGE, "Overlays...");
    settingsMenu->AppendSubMenu(themeMenu, "Theme");
    menuBar->Append(settingsMenu, "Settings");

    auto *helpMenu = new wxMenu();
    helpMenu->Append(ID_HELP_CHECK_UPDATES, "Check for Updates");
    helpMenu->Append(ID_HELP_CONSOLE, "Console\tCtrl+Shift+T");
    helpMenu->Append(ID_HELP_WELCOME, "Show Welcome Screen");
    helpMenu->Append(ID_HELP_ABOUT, "About");
    menuBar->Append(helpMenu, "Help");

    SetMenuBar(menuBar);
}

namespace {
wxFont SectionFont() { return wxFont(wxFontInfo(11).FaceName("Segoe UI").Bold()); }
wxFont BodyFont()     { return wxFont(wxFontInfo(11).FaceName("Segoe UI")); }
wxFont MonoFont()     { return wxFont(wxFontInfo(11).FaceName("Consolas")); }
} // namespace

void HomRecMainFrame::BuildLeftPanel(wxWindow *parent, wxSizer *parentSizer) {
    left_panel_ = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(kLeftPanelW, -1));
    auto *sizer = new wxBoxSizer(wxVERTICAL);

    title_lbl_ = new wxStaticText(left_panel_, wxID_ANY, "HomRec", wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    title_lbl_->SetFont(wxFont(wxFontInfo(22).FaceName("Segoe UI").Bold()));
    sizer->Add(title_lbl_, 0, wxEXPAND | wxTOP, 20);

    version_lbl_ = new wxStaticText(left_panel_, wxID_ANY, "v" HR_APP_VERSION, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    version_lbl_->SetFont(BodyFont());
    sizer->Add(version_lbl_, 0, wxEXPAND | wxTOP, 4);

    start_color_btn_ = new ColorButton(left_panel_, ID_START_BTN, wxString::FromUTF8("\u25B6 START"));
    start_color_btn_->SetFont(wxFont(wxFontInfo(11).FaceName("Segoe UI").Bold()));
    start_color_btn_->SetMinSize(wxSize(-1, 48));
    sizer->Add(start_color_btn_, 0, wxEXPAND | wxTOP, 25);
    Bind(wxEVT_BUTTON, &HomRecMainFrame::OnStartClicked, this, ID_START_BTN);

    pause_color_btn_ = new ColorButton(left_panel_, ID_PAUSE_BTN, wxString::FromUTF8("\u23F8 PAUSE"));
    pause_color_btn_->SetFont(wxFont(wxFontInfo(10).FaceName("Segoe UI").Bold()));
    pause_color_btn_->SetMinSize(wxSize(-1, 32));
    pause_color_btn_->Enable2(false);
    sizer->Add(pause_color_btn_, 0, wxEXPAND | wxTOP, 4);
    Bind(wxEVT_BUTTON, &HomRecMainFrame::OnPauseClicked, this, ID_PAUSE_BTN);

    auto addSection = [&](const wxString &labelText) {
        auto *lbl = new wxStaticText(left_panel_, wxID_ANY, labelText);
        lbl->SetFont(SectionFont());
        sizer->Add(lbl, 0, wxEXPAND | wxTOP, 15);
        return lbl;
    };

    addSection(wxString::FromUTF8(lang_.Get("status")));
    auto *statusRow = new wxBoxSizer(wxHORIZONTAL);
    status_dot_ = new StatusDot(left_panel_, FromColorref(theme_.text_secondary), 14);
    statusRow->Add(status_dot_, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
    status_lbl_ = new wxStaticText(left_panel_, wxID_ANY, wxString::FromUTF8(lang_.Get("ready")));
    status_lbl_->SetFont(BodyFont());
    statusRow->Add(status_lbl_, 1, wxALIGN_CENTRE_VERTICAL);
    sizer->Add(statusRow, 0, wxEXPAND | wxTOP, 8);

    addSection(wxString::FromUTF8(lang_.Get("time")));
    time_lbl_ = new wxStaticText(left_panel_, wxID_ANY, "00:00:00", wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    time_lbl_->SetFont(wxFont(wxFontInfo(24).FaceName("Consolas").Bold()));
    sizer->Add(time_lbl_, 0, wxEXPAND | wxTOP, 8);

    addSection(wxString::FromUTF8(lang_.Get("stats")));
    fps_lbl_ = new wxStaticText(left_panel_, wxID_ANY, "");
    fps_lbl_->SetFont(MonoFont());
    sizer->Add(fps_lbl_, 0, wxEXPAND | wxTOP, 4);
    res_lbl_ = new wxStaticText(left_panel_, wxID_ANY, "");
    res_lbl_->SetFont(MonoFont());
    sizer->Add(res_lbl_, 0, wxEXPAND | wxTOP, 2);

    sizer->AddStretchSpacer(1);

    // Left sidebar's inner 15px padx, matching ui_mixin.py's frame padx=15.
    auto *padded = new wxBoxSizer(wxVERTICAL);
    padded->Add(sizer, 1, wxEXPAND | wxLEFT | wxRIGHT, 15);
    left_panel_->SetSizer(padded);

    parentSizer->Add(left_panel_, 0, wxEXPAND | wxRIGHT, 15);
}

void HomRecMainFrame::BuildPreviewPanel(wxWindow *parent, wxSizer *parentSizer) {
    auto *rightColumn = new wxBoxSizer(wxVERTICAL);

    preview_container_ = new wxPanel(parent);
    auto *pcSizer = new wxBoxSizer(wxVERTICAL);

    preview_header_ = new wxPanel(preview_container_, wxID_ANY, wxDefaultPosition, wxSize(-1, 30));
    auto *headerSizer = new wxBoxSizer(wxHORIZONTAL);
    preview_title_lbl_ = new wxStaticText(preview_header_, wxID_ANY, wxString::FromUTF8("\u25CF ") + wxString::FromUTF8(lang_.Get("live_preview")));
    preview_title_lbl_->SetFont(wxFont(wxFontInfo(9).FaceName("Segoe UI").Bold()));
    headerSizer->Add(preview_title_lbl_, 0, wxALIGN_CENTRE_VERTICAL | wxLEFT, 10);
    headerSizer->AddStretchSpacer(1);
    preview_fps_lbl_ = new wxStaticText(preview_header_, wxID_ANY, "");
    preview_fps_lbl_->SetFont(wxFont(wxFontInfo(8).FaceName("Segoe UI")));
    headerSizer->Add(preview_fps_lbl_, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 10);
    preview_header_->SetSizer(headerSizer);
    pcSizer->Add(preview_header_, 0, wxEXPAND);

    preview_panel_ = new PreviewPanel(preview_container_, rec_raw_, state_);
    pcSizer->Add(preview_panel_, 1, wxEXPAND | wxALL, 8);
    preview_container_->SetSizer(pcSizer);
    rightColumn->Add(preview_container_, 1, wxEXPAND);

    // Audio mixer strip lives below the preview — real wx widgets now
    // (ColorSlider/ColorButton/LevelMeterPanel from audio_panel.h), no
    // native-HWND hosting needed the way OverlaysDockPanel below still does.
    audio_panel_ = std::make_unique<AudioPanel>(parent, state_, *rec_);
    rightColumn->Add(audio_panel_.get(), 0, wxEXPAND | wxTOP, 15);

    parentSizer->Add(rightColumn, 1, wxEXPAND);

    // Overlays dock — also raw-Win32, same reasoning as AudioPanel.
    overlays_host_ = new NativeHostPanel(parent);
    overlays_host_->SetMinSize(wxSize(220, -1));
    overlays_panel_ = std::make_unique<OverlaysDockPanel>(state_);
    overlays_panel_->Create((HWND)overlays_host_->GetHandle(), wxGetInstance(), 0, 0, 220, 500);
    overlays_host_->on_drawitem = [this](DRAWITEMSTRUCT *dis) {
        // OverlaysDockPanel doesn't currently expose a HandleDrawItem the
        // way AudioPanel does (its list items aren't owner-drawn) — no-op
        // here, left as a documented hook if that changes.
        (void)dis;
    };
    parentSizer->Add(overlays_host_, 0, wxEXPAND | wxLEFT, 15);
    overlays_panel_->SetVisible(state_.show_overlays_panel);
    overlays_host_->Show(state_.show_overlays_panel);
}

void HomRecMainFrame::BuildBottomBar(wxWindow *parent, wxSizer *parentSizer) {
    bottom_bar_ = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 32));
    auto *sizer = new wxBoxSizer(wxHORIZONTAL);

    bottom_dot_ = new StatusDot(bottom_bar_, FromColorref(theme_.text_secondary), 12);
    sizer->Add(bottom_dot_, 0, wxALIGN_CENTRE_VERTICAL | wxLEFT | wxRIGHT, 6);
    sizer->AddSpacer(kOuterPad - 6);

    file_lbl_ = new wxStaticText(bottom_bar_, wxID_ANY, wxString::FromUTF8(lang_.Get("ready")));
    file_lbl_->SetFont(wxFont(wxFontInfo(9).FaceName("Segoe UI")));
    sizer->Add(file_lbl_, 1, wxALIGN_CENTRE_VERTICAL);

    made_by_lbl_ = new wxStaticText(bottom_bar_, wxID_ANY, wxString::FromUTF8(lang_.Get("made_by")));
    made_by_lbl_->SetFont(wxFont(wxFontInfo(9).FaceName("Segoe UI").Bold()));
    sizer->Add(made_by_lbl_, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 10);

    version_bar_lbl_ = new wxStaticText(bottom_bar_, wxID_ANY, "v" HR_APP_VERSION);
    version_bar_lbl_->SetFont(wxFont(wxFontInfo(8).FaceName("Segoe UI")));
    sizer->Add(version_bar_lbl_, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, kOuterPad);

    bottom_bar_->SetSizer(sizer);
    parentSizer->Add(bottom_bar_, 0, wxEXPAND);
}

void HomRecMainFrame::ApplyThemeColours() {
    wxColour bg = FromColorref(theme_.bg);
    wxColour surface = FromColorref(theme_.surface);
    wxColour surfaceLight = FromColorref(theme_.surface_light);
    wxColour previewBg = FromColorref(theme_.preview_bg);
    wxColour text = FromColorref(theme_.text);
    wxColour textSecondary = FromColorref(theme_.text_secondary);
    wxColour accent = FromColorref(theme_.accent);

    SetBackgroundColour(bg);
    if (left_panel_) left_panel_->SetBackgroundColour(surface);
    if (preview_container_) preview_container_->SetBackgroundColour(surfaceLight);
    if (preview_header_) preview_header_->SetBackgroundColour(surfaceLight);
    if (preview_panel_) preview_panel_->SetBackgroundColour(previewBg);
    if (bottom_bar_) bottom_bar_->SetBackgroundColour(surface);

    if (title_lbl_) { title_lbl_->SetForegroundColour(accent); title_lbl_->SetBackgroundColour(surface); }
    if (version_lbl_) { version_lbl_->SetForegroundColour(textSecondary); version_lbl_->SetBackgroundColour(surface); }
    if (status_lbl_) { status_lbl_->SetForegroundColour(text); status_lbl_->SetBackgroundColour(surface); }
    if (time_lbl_) { time_lbl_->SetForegroundColour(accent); time_lbl_->SetBackgroundColour(surface); }
    if (fps_lbl_) { fps_lbl_->SetForegroundColour(text); fps_lbl_->SetBackgroundColour(surface); }
    if (res_lbl_) { res_lbl_->SetForegroundColour(text); res_lbl_->SetBackgroundColour(surface); }
    if (preview_title_lbl_) { preview_title_lbl_->SetForegroundColour(accent); preview_title_lbl_->SetBackgroundColour(surfaceLight); }
    if (preview_fps_lbl_) { preview_fps_lbl_->SetForegroundColour(textSecondary); preview_fps_lbl_->SetBackgroundColour(surfaceLight); }
    if (file_lbl_) { file_lbl_->SetForegroundColour(text); file_lbl_->SetBackgroundColour(surface); }
    if (made_by_lbl_) { made_by_lbl_->SetForegroundColour(textSecondary); made_by_lbl_->SetBackgroundColour(surface); }
    if (version_bar_lbl_) { version_bar_lbl_->SetForegroundColour(textSecondary); version_bar_lbl_->SetBackgroundColour(surface); }

    // "STATUS"/"TIME"/"STATS" section labels aren't kept as individually
    // named members (built inline in BuildLeftPanel's addSection lambda),
    // so re-theme every direct child of left_panel_ uniformly instead —
    // matches them all having the same accent-on-surface styling anyway.
    if (left_panel_) {
        for (wxWindow *child : left_panel_->GetChildren()) {
            if (auto *st = dynamic_cast<wxStaticText *>(child)) {
                if (st != title_lbl_ && st != version_lbl_ && st != status_lbl_ &&
                    st != time_lbl_ && st != fps_lbl_ && st != res_lbl_) {
                    st->SetForegroundColour(accent);
                    st->SetBackgroundColour(surface);
                }
            }
        }
    }

    if (start_color_btn_) start_color_btn_->SetColours(FromColorref(theme_.success), FromColorref(theme_.bg));
    if (pause_color_btn_) pause_color_btn_->SetColours(FromColorref(theme_.warning), FromColorref(theme_.bg));
    if (audio_panel_) audio_panel_->ApplyTheme(theme_);

    Refresh(true);
}

void HomRecMainFrame::ApplyLanguageText() {
    std::string title = lang_.Get("app_title");
    if (title.empty()) title = std::string("HomRec v") + HR_APP_VERSION;
    SetTitle(wxString::FromUTF8(title));
}

void HomRecMainFrame::SetupTrayIcon() {
    tray_icon_ = new TrayIcon(this);
    tray_icon_->SetIcon(wxIcon("#1", wxBITMAP_TYPE_ICO_RESOURCE), "HomRec");
    tray_icon_->Bind(wxEVT_TASKBAR_LEFT_DCLICK, [this](wxTaskBarIconEvent &) {
        Show(true);
        Raise();
    });
    // wxTaskBarIcon::PopupMenu() (invoked from TrayIcon::CreatePopupMenu()'s
    // right-click menu) dispatches wxEVT_MENU to the tray icon object
    // itself, not the owning frame — bind here, not via the frame's
    // ID_FILE_OPEN_RECORDINGS..ID_VIEW_OVERLAYS_PANEL range Bind.
    tray_icon_->Bind(wxEVT_MENU, [this](wxCommandEvent &) { Show(true); Raise(); }, ID_TRAY_RESTORE);
    tray_icon_->Bind(wxEVT_MENU, [this](wxCommandEvent &) { Close(true); }, ID_TRAY_EXIT);
}

void HomRecMainFrame::SetupHotkeys() {
    hotkey_handle_ = hr_hk_create();
    hr_hk_set_callbacks(hotkey_handle_, &HotkeyStartStopThunk, &HotkeyPauseThunk, &HotkeyFullscreenThunk);
    ConfigureHotkeysFromState();
    if (!hr_hk_start(hotkey_handle_)) {
        wxLogDebug("HomRec: global hotkeys failed to register.");
    }
}

void HomRecMainFrame::ConfigureHotkeysFromState() {
    if (!hotkey_handle_) return;
    hr_hk_configure(hotkey_handle_, state_.hotkey_start_stop.c_str(),
                     state_.hotkey_pause.c_str(), state_.hotkey_fullscreen.c_str());
}

void HomRecMainFrame::SetStatusState(const wxString &text, COLORREF dotColor) {
    if (status_lbl_) status_lbl_->SetLabel(text);
    if (status_dot_) status_dot_->SetColor(FromColorref(dotColor));
    if (bottom_dot_) bottom_dot_->SetColor(FromColorref(dotColor));
    if (left_panel_) left_panel_->Layout();
}

void HomRecMainFrame::DoStart() {
    if (audio_panel_) {
        rec_->SetAudioLevels(audio_panel_->mic_volume(), audio_panel_->sys_volume(),
                              audio_panel_->mic_muted(), audio_panel_->sys_muted());
    }
    std::wstring err;
    if (!rec_->Start(err)) {
        wxMessageBox(wxString(err.c_str()), "HomRec", wxOK | wxICON_WARNING, this);
        return;
    }
    start_color_btn_->SetLabelText2(wxString::FromUTF8("\u25A0 STOP"));
    start_color_btn_->SetColours(FromColorref(theme_.error), FromColorref(theme_.bg));
    pause_color_btn_->Enable2(true);
    SetStatusState(wxString::FromUTF8(lang_.Get("recording")), theme_.success);
    if (plugins_) plugins_->EmitHook("on_recording_start");
}

void HomRecMainFrame::DoStop() {
    SetStatusState(wxString::FromUTF8("Saving\u2026"), theme_.warning);
    if (time_lbl_) time_lbl_->SetLabel("00:00:00");
    if (file_lbl_) file_lbl_->SetLabel(wxString::FromUTF8("Processing\u2026"));
    Update();

    rec_->Stop();

    start_color_btn_->SetLabelText2(wxString::FromUTF8("\u25B6 START"));
    start_color_btn_->SetColours(FromColorref(theme_.success), FromColorref(theme_.bg));
    pause_color_btn_->SetLabelText2(wxString::FromUTF8("\u23F8 PAUSE"));
    pause_color_btn_->SetColours(FromColorref(theme_.warning), FromColorref(theme_.bg));
    pause_color_btn_->Enable2(false);
    SetStatusState(wxString::FromUTF8(lang_.Get("ready")), theme_.text_secondary);
    if (file_lbl_) file_lbl_->SetLabel(wxString::FromUTF8(lang_.Get("ready")));
    if (plugins_) plugins_->EmitHook("on_recording_stop");
}

void HomRecMainFrame::DoPause() {
    if (!state_.recording) return;
    rec_->TogglePause();
    if (state_.paused) {
        pause_color_btn_->SetLabelText2(wxString::FromUTF8("\u25B6 RESUME"));
        pause_color_btn_->SetColours(FromColorref(theme_.success), FromColorref(theme_.bg));
        SetStatusState(wxString::FromUTF8(lang_.Get("paused")), theme_.warning);
    } else {
        pause_color_btn_->SetLabelText2(wxString::FromUTF8("\u23F8 PAUSE"));
        pause_color_btn_->SetColours(FromColorref(theme_.warning), FromColorref(theme_.bg));
        SetStatusState(wxString::FromUTF8(lang_.Get("recording")), theme_.success);
    }
}

void HomRecMainFrame::ToggleFullscreenNative() {
    fullscreen_ = !fullscreen_;
    ShowFullScreen(fullscreen_, wxFULLSCREEN_NOBORDER | wxFULLSCREEN_NOCAPTION);
}

void HomRecMainFrame::OnStartClicked(wxCommandEvent &) { if (state_.recording) DoStop(); else DoStart(); }
void HomRecMainFrame::OnPauseClicked(wxCommandEvent &) { DoPause(); }

void HomRecMainFrame::OnMenu(wxCommandEvent &evt) {
    switch (evt.GetId()) {
        case ID_FILE_EXIT: Close(true); break;
        case ID_FILE_OPEN_RECORDINGS:
            if (!state_.output_folder.empty())
                ShellExecuteA(GetHWND(), "open", state_.output_folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case ID_FILE_SELECT_WINDOW:
            ShowWindowPickerDialog(GetHWND(), wxGetInstance(), state_);
            break;
        case ID_VIEW_ALWAYS_ON_TOP: {
            long style = GetWindowStyleFlag();
            SetWindowStyleFlag(style ^ wxSTAY_ON_TOP);
            break;
        }
        case ID_VIEW_FULLSCREEN: ToggleFullscreenNative(); break;
        case ID_VIEW_OVERLAYS_PANEL:
            state_.show_overlays_panel = !state_.show_overlays_panel;
            if (overlays_panel_) overlays_panel_->SetVisible(state_.show_overlays_panel);
            if (overlays_host_) overlays_host_->Show(state_.show_overlays_panel);
            if (auto *mb = GetMenuBar()) mb->Check(ID_VIEW_OVERLAYS_PANEL, state_.show_overlays_panel);
            Layout();
            break;
        case ID_VIEW_PC_ANALYTICS: ShowPcAnalyticsDialog(GetHWND(), wxGetInstance(), state_.output_folder); break;
        case ID_VIEW_LOG: ShowLogViewerDialog(GetHWND(), wxGetInstance()); break;
        case ID_THEME_DARK:
            state_.current_theme = "dark"; theme_ = GetBuiltinTheme("dark"); ApplyThemeColours(); break;
        case ID_THEME_LIGHT:
            state_.current_theme = "light"; theme_ = GetBuiltinTheme("light"); ApplyThemeColours(); break;
        case ID_SETTINGS_OPEN: ShowSettingsDialog(this, state_, theme_); break;
        case ID_SETTINGS_ADVANCED:
            ShowSettingsDialogTab(this, state_, theme_, 1 /* Video / Codec */);
            if (hotkey_handle_) { hr_hk_stop(hotkey_handle_); hr_hk_destroy(hotkey_handle_); hotkey_handle_ = nullptr; }
            SetupHotkeys();
            break;
        case ID_OVERLAYS_MANAGE:
            ShowOverlayManager(GetHWND(), wxGetInstance(), state_);
            if (overlays_panel_) overlays_panel_->Refresh();
            break;
        case ID_HELP_CONSOLE:
            if (!console_) console_ = std::make_unique<ConsoleWindow>(state_, rec_.get(), GetHWND());
            console_->Show(wxGetInstance());
            break;
        case ID_HELP_WELCOME: ShowWelcomeDialog(GetHWND(), wxGetInstance()); break;
        case ID_HELP_ABOUT:
            wxMessageBox("HomRec " HR_APP_VERSION, "About", wxOK, this);
            break;
        default: break;
    }
}

void HomRecMainFrame::OnPreviewTimer(wxTimerEvent &) {
    if (state_.recording && preview_panel_) preview_panel_->Refresh(false);
}

void HomRecMainFrame::OnStatsTimer(wxTimerEvent &) {
    if (rec_) rec_->PollStats();
    if (audio_panel_) audio_panel_->PollLevels();

    if (state_.recording) {
        std::wstring elapsed = rec_ ? rec_->elapsed_formatted() : std::wstring(L"00:00:00");
        wxString t(elapsed.c_str());
        if (time_lbl_) time_lbl_->SetLabel(t);
        if (fps_lbl_) {
            wxString fps = wxString::FromUTF8(lang_.Get("fps")) +
                           wxString::Format(" %.1f", rec_ ? rec_->current_fps() : 0.0);
            fps_lbl_->SetLabel(fps);
        }
        if (res_lbl_) {
            wxString res = wxString::FromUTF8(lang_.Get("resolution")) +
                           wxString::Format(" %dx%d", rec_ ? rec_->capture_width() : 0,
                                            rec_ ? rec_->capture_height() : 0);
            res_lbl_->SetLabel(res);
        }
        if (preview_fps_lbl_) preview_fps_lbl_->SetLabel(fps_lbl_ ? fps_lbl_->GetLabel() : wxString());
        if (file_lbl_) {
            wxString state_word = wxString::FromUTF8(lang_.Get(state_.paused ? "paused" : "recording"));
            file_lbl_->SetLabel(state_word + wxString::FromUTF8(" \u2014 ") + t);
        }
    }
    left_panel_->Layout();
}

void HomRecMainFrame::OnClose(wxCloseEvent &evt) {
    if (state_.minimize_to_tray && tray_icon_ && evt.CanVeto()) {
        Show(false);
        evt.Veto();
        return;
    }
    if (hotkey_handle_) { hr_hk_stop(hotkey_handle_); hr_hk_destroy(hotkey_handle_); hotkey_handle_ = nullptr; }
    HrLog::Info("HomRec closing");
    Destroy();
}

void HomRecMainFrame::OnIconize(wxIconizeEvent &evt) {
    if (evt.IsIconized() && state_.minimize_to_tray) Show(false);
}

void HomRecMainFrame::OnHotkeyEvent(wxThreadEvent &evt) {
    wxEventType t = evt.GetEventType();
    if (t == EVT_HOTKEY_START_STOP) { if (state_.recording) DoStop(); else DoStart(); }
    else if (t == EVT_HOTKEY_PAUSE) { DoPause(); }
    else if (t == EVT_HOTKEY_FULLSCREEN) { ToggleFullscreenNative(); }
}
