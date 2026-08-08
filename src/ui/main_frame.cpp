#include "main_frame.h"
#include "version.h"
#include "settings_dialog.h"
// (overlay_manager.h removed -- see overlays_dock_panel.h)
#include "welcome_dialog.h"
#include "pc_analytics_dialog.h"
#include "log_viewer_dialog.h"
#include "window_picker_dialog.h"
#include "custom_messagebox.h"
#include "hrc_config.h"
#include "win32_theme.h"
#include "../hr_log.h"
#include <wx/dcbuffer.h>
#include <wx/msw/private.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <functional>
#include <algorithm>
#include <string>
#include <cstring>
#include <cmath>

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
    int hr_settings_get_resolution_pct(const void *h);
    int hr_settings_get_flag(const void *h, const char *name);
}

// FromColorref/ColorButton/StatusDot moved to themed_widgets.cpp.

// Raw-Win32 OverlaysDockPanel creates real child HWNDs (list/buttons)
// owner-drawn level meters) that need WM_HSCROLL/WM_DRAWITEM delivered -
// Windows sends both to the control's *immediate parent* HWND, not the
// top-level frame. This wxPanel subclass exists purely to be that immediate
// parent and forward the two messages on, via MSWWindowProc (the same hook
// wx itself uses internally for this sort of thing).
//
// Defined here at global/file scope (NOT inside the anonymous namespace
// below) because main_frame.h forward-declares it at global scope too
// (`class NativeHostPanel *overlays_host_`) - a class defined inside an
// anonymous namespace is a different, invisible type from one of the same
// name forward-declared at global scope, which is exactly what produced
// the "invalid use of incomplete type" build errors.
class NativeHostPanel : public wxPanel {
public:
    explicit NativeHostPanel(wxWindow *parent) : wxPanel(parent) {}
    std::function<void(HWND, int)> on_hscroll;
    std::function<void(DRAWITEMSTRUCT *)> on_drawitem;
    std::function<void(int)> on_command;

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
        if (nMsg == WM_COMMAND && on_command && HIWORD(wParam) == BN_CLICKED) {
            on_command(LOWORD(wParam));
            return 0;
        }
        if (nMsg == WM_CTLCOLORSTATIC) {
            return HrWin32Theme::ColorStatic(reinterpret_cast<HDC>(wParam));
        }
        if (nMsg == WM_CTLCOLORLISTBOX) {
            return HrWin32Theme::ColorEdit(reinterpret_cast<HDC>(wParam));
        }
        return wxPanel::MSWWindowProc(nMsg, wParam, lParam);
    }
};

namespace {
// UTF-8 -> UTF-16, for handing lang_.Get()'s narrow strings and
// state_.output_folder to the raw-Win32 dialogs in this file (custom
// messagebox, etc.) that take std::wstring.
std::wstring WideFromNarrow(const std::string &s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len > 0 ? len - 1 : 0, L'\0');
    if (len > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

// Only one main frame exists per process - the hotkey manager's callbacks
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
    // See ColorSlider's ctor (themed_widgets.cpp) for why: a buffered
    // paint DC only blits the invalidated strip back to screen on
    // resize, not the whole client area, which can leave stale pixels
    // behind when the window is enlarged.
    Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) { Refresh(true); evt.Skip(); });
    Bind(wxEVT_LEFT_DOWN, &PreviewPanel::OnLeftDown, this);
    Bind(wxEVT_MOTION, &PreviewPanel::OnMouseMove, this);
    Bind(wxEVT_LEFT_UP, &PreviewPanel::OnLeftUp, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &PreviewPanel::OnCaptureLost, this);
}

void PreviewPanel::OnPaint(wxPaintEvent &) {
    wxAutoBufferedPaintDC dc(this);

    if (state_.disable_preview) {
        // Deliberately distinct from the "no frame yet" placeholder below -
        // this is an intentional choice (Settings > Disable live preview),
        // not something that looks broken/waiting.
        wxColour grey(90, 90, 96);
        dc.SetBackground(wxBrush(grey));
        dc.Clear();
        dc.SetTextForeground(wxColour(230, 230, 235));
        wxFont f = GetFont();
        f.SetPointSize(f.GetPointSize() + 6);
        dc.SetFont(f);
        wxString smiley = ":)";
        wxSize ext = dc.GetTextExtent(smiley);
        wxSize cs = GetClientSize();
        dc.DrawText(smiley, (cs.GetWidth() - ext.GetWidth()) / 2, (cs.GetHeight() - ext.GetHeight()) / 2);
        return;
    }

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
    // o[1]=g, o[2]=b - see its BGR->RGB nearest-neighbour fallback branch),
    // so this can go straight into a wxImage with no channel swap.
    wxSize cs = GetClientSize();
    if (cs.GetWidth() > 0 && cs.GetHeight() > 0) {
        bool same_frame = (int)frame_buf_.size() == (int)last_frame_buf_.size() &&
                           w == cached_src_w_ && h == cached_src_h_ &&
                           cs.GetWidth() == cached_panel_w_ && cs.GetHeight() == cached_panel_h_ &&
                           cached_bmp_.IsOk() &&
                           std::memcmp(frame_buf_.data(), last_frame_buf_.data(), frame_buf_.size()) == 0;

        if (!same_frame) {
            // Preserve aspect ratio (letterbox/pillarbox) instead of stretching
            // to fill the panel.
            wxImage img(w, h, frame_buf_.data(), /*static_data=*/true);
            double scale = std::min((double)cs.GetWidth() / w, (double)cs.GetHeight() / h);
            int dw = std::max(1, (int)(w * scale));
            int dh = std::max(1, (int)(h * scale));
            wxImage scaled = img.Scale(dw, dh, wxIMAGE_QUALITY_BILINEAR);
            cached_bmp_ = wxBitmap(scaled);
            cached_src_w_ = w; cached_src_h_ = h;
            cached_dst_w_ = dw; cached_dst_h_ = dh;
            cached_panel_w_ = cs.GetWidth(); cached_panel_h_ = cs.GetHeight();
            last_frame_buf_ = frame_buf_;
        }
        dc.DrawBitmap(cached_bmp_, (cs.GetWidth() - cached_dst_w_) / 2, (cs.GetHeight() - cached_dst_h_) / 2);
    }

    // Draw each configured overlay's rectangle
    // (plus a small resize-handle square at its bottom-right corner)
    // directly on top of the live preview, so it can be grabbed with the
    // mouse right here instead of only through the separate full-screen
    // "Position Overlays" window.
    wxRect prevRect;
    if (GetPreviewRect(prevRect) && rec_ && rec_->capture_width() > 0 && rec_->capture_height() > 0) {
        double sx = (double)prevRect.GetWidth() / rec_->capture_width();
        double sy = (double)prevRect.GetHeight() / rec_->capture_height();
        const int handle = 8;
        for (size_t i = 0; i < state_.overlays.size(); ++i) {
            const auto &ov = state_.overlays[i];
            if (!ov.visible) continue;
            int rx = prevRect.GetX() + (int)(ov.x * sx);
            int ry = prevRect.GetY() + (int)(ov.y * sy);
            int rw = std::max(4, (int)(ov.w * sx));
            int rh = std::max(4, (int)(ov.h * sy));
            bool active = ((int)i == drag_overlay_index_);
            wxColour accent = active ? wxColour(255, 210, 90) : wxColour(120, 170, 250);

            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(accent, active ? 2 : 1, wxPENSTYLE_SHORT_DASH));
            dc.DrawRectangle(rx, ry, rw, rh);

            // Two resize handles -- top-left and bottom-right -- instead
            // of only bottom-right, so either corner can be grabbed to
            // resize (top-left drags the top-left edge in while keeping
            // the bottom-right corner anchored, and vice versa).
            dc.SetBrush(wxBrush(accent));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(rx + rw - handle, ry + rh - handle, handle, handle);
            dc.DrawRectangle(rx, ry, handle, handle);
        }
    }
}

bool PreviewPanel::GetPreviewRect(wxRect &out) const {
    if (cached_dst_w_ <= 0 || cached_dst_h_ <= 0) return false;
    wxSize cs = GetClientSize();
    int ox = (cs.GetWidth() - cached_dst_w_) / 2;
    int oy = (cs.GetHeight() - cached_dst_h_) / 2;
    out = wxRect(ox, oy, cached_dst_w_, cached_dst_h_);
    return true;
}

void PreviewPanel::OnLeftDown(wxMouseEvent &evt) {
    wxRect prevRect;
    if (!GetPreviewRect(prevRect) || !rec_ || rec_->capture_width() <= 0 || rec_->capture_height() <= 0) {
        evt.Skip();
        return;
    }
    double sx = (double)prevRect.GetWidth() / rec_->capture_width();
    double sy = (double)prevRect.GetHeight() / rec_->capture_height();
    int mx = evt.GetX(), my = evt.GetY();
    const int handle = 8;

    // Topmost-first (later in the vector paints on top of earlier ones),
    // so overlapping overlays grab the one the user actually sees on top.
    for (int i = (int)state_.overlays.size() - 1; i >= 0; --i) {
        auto &ov = state_.overlays[(size_t)i];
        if (!ov.visible) continue;
        int rx = prevRect.GetX() + (int)(ov.x * sx);
        int ry = prevRect.GetY() + (int)(ov.y * sy);
        int rw = std::max(4, (int)(ov.w * sx));
        int rh = std::max(4, (int)(ov.h * sy));
        wxRect body(rx, ry, rw, rh);
        wxRect brHandle(rx + rw - handle, ry + rh - handle, handle, handle);
        wxRect tlHandle(rx, ry, handle, handle);

        if (brHandle.Contains(mx, my)) {
            drag_corner_ = Corner::kBottomRight;
        } else if (tlHandle.Contains(mx, my)) {
            drag_corner_ = Corner::kTopLeft;
        } else if (body.Contains(mx, my)) {
            drag_corner_ = Corner::kNone;
        } else {
            continue;
        }

        drag_overlay_index_ = i;
        drag_start_mouse_x_ = mx;
        drag_start_mouse_y_ = my;
        drag_start_ov_x_ = ov.x;
        drag_start_ov_y_ = ov.y;
        drag_start_ov_w_ = ov.w;
        drag_start_ov_h_ = ov.h;
        CaptureMouse();
        Refresh();
        return;
    }
    evt.Skip();
}

void PreviewPanel::OnMouseMove(wxMouseEvent &evt) {
    if (drag_overlay_index_ < 0 || !evt.LeftIsDown() || !rec_ ||
        rec_->capture_width() <= 0 || rec_->capture_height() <= 0) {
        evt.Skip();
        return;
    }
    wxRect prevRect;
    if (!GetPreviewRect(prevRect) || prevRect.GetWidth() <= 0 || prevRect.GetHeight() <= 0) {
        evt.Skip();
        return;
    }
    double sx = (double)prevRect.GetWidth() / rec_->capture_width();
    double sy = (double)prevRect.GetHeight() / rec_->capture_height();

    // Convert the mouse delta from on-screen preview pixels back to real
    // capture-resolution pixels (the space OverlayDef::x/y/w/h -- and the
    // actual recording -- live in), not thumbnail pixels.
    int dx = (int)std::lround((evt.GetX() - drag_start_mouse_x_) / sx);
    int dy = (int)std::lround((evt.GetY() - drag_start_mouse_y_) / sy);

    auto &ov = state_.overlays[(size_t)drag_overlay_index_];
    int cw = rec_->capture_width(), ch = rec_->capture_height();
    if (drag_corner_ == Corner::kBottomRight) {
        // Top-left corner stays put; bottom-right corner follows the mouse.
        ov.w = std::max(10, drag_start_ov_w_ + dx);
        ov.h = std::max(10, drag_start_ov_h_ + dy);
    } else if (drag_corner_ == Corner::kTopLeft) {
        // Bottom-right corner (drag_start_ov_x_+w, drag_start_ov_y_+h)
        // stays put; the top-left corner follows the mouse instead. Clamp
        // width/height to a 10px floor the same way the other handle
        // does, and keep x/y consistent with whatever size that floor
        // ends up clamping to (so the box doesn't "jump" once the mouse
        // drags past the minimum size).
        int new_w = std::max(10, drag_start_ov_w_ - dx);
        int new_h = std::max(10, drag_start_ov_h_ - dy);
        int anchor_right  = drag_start_ov_x_ + drag_start_ov_w_;
        int anchor_bottom = drag_start_ov_y_ + drag_start_ov_h_;
        ov.w = new_w;
        ov.h = new_h;
        ov.x = std::clamp(anchor_right - new_w, 0, std::max(0, cw - new_w));
        ov.y = std::clamp(anchor_bottom - new_h, 0, std::max(0, ch - new_h));
    } else {
        ov.x = std::clamp(drag_start_ov_x_ + dx, 0, std::max(0, cw - ov.w));
        ov.y = std::clamp(drag_start_ov_y_ + dy, 0, std::max(0, ch - ov.h));
    }
    Refresh();
}

void PreviewPanel::OnLeftUp(wxMouseEvent &evt) {
    if (drag_overlay_index_ >= 0) {
        if (HasCapture()) ReleaseMouse();
        drag_overlay_index_ = -1;
        drag_corner_ = Corner::kNone;
        Refresh();
    }
    evt.Skip();
}

void PreviewPanel::OnCaptureLost(wxMouseCaptureLostEvent &) {
    drag_overlay_index_ = -1;
    drag_corner_ = Corner::kNone;
    Refresh();
}

// ---------------------------------------------------------------------------
// HomRecMainFrame
// ---------------------------------------------------------------------------
HomRecMainFrame::HomRecMainFrame()
    : wxFrame(nullptr, wxID_ANY, "HomRec", wxDefaultPosition, wxSize(1300, 750)),
      preview_timer_(this), stats_timer_(this), restore_topmost_timer_(this) {
    g_frame = this;
    SetIcon(wxIcon("#1", wxBITMAP_TYPE_ICO_RESOURCE));

    lang_ = LanguageTable::Load(state_.current_language, "Assets\\L");
    theme_ = GetBuiltinTheme(state_.current_theme);

    // Load whatever Settings previously saved - see main_window.cpp's
    // original OnCreate() comment; unchanged behavior, just moved here.
    void *settings = hr_settings_create();
    if (hr_settings_load(settings, "homrec_settings.json")) {
        const char *folder = hr_settings_get_output_folder(settings);
        state_.output_folder = (folder && folder[0]) ? folder : "recordings";
        state_.quality = hr_settings_get_quality(settings);
        state_.target_fps = hr_settings_get_fps(settings);
        state_.monitor_id = hr_settings_get_monitor(settings);
        state_.scale_factor = hr_settings_get_resolution_pct(settings) / 100.0;
        state_.countdown_enabled = hr_settings_get_flag(settings, "countdown") != 0;
        state_.timestamp_enabled = hr_settings_get_flag(settings, "timestamp") != 0;
        state_.cursor_enabled = hr_settings_get_flag(settings, "cursor") != 0;
        state_.show_summary = hr_settings_get_flag(settings, "show_summary") != 0;
        state_.show_overlays_panel = hr_settings_get_flag(settings, "show_overlays_panel") != 0;
        state_.disable_preview = hr_settings_get_flag(settings, "disable_preview") != 0;
    } else {
        state_.output_folder = "recordings";
    }
    hr_settings_destroy(settings);

    SetMinSize(wxSize(state_.window_min_w, state_.window_min_h));

    BuildMenuBar();

    rec_ = std::make_unique<RecordingController>(state_);
    rec_->Initialize();
    rec_raw_ = rec_.get();
    rec_->EnsurePreview();

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
    Bind(wxEVT_TIMER, &HomRecMainFrame::OnRestoreTopmostTimer, this, restore_topmost_timer_.GetId());
    Bind(wxEVT_TIMER, &HomRecMainFrame::OnCountdownTimer, this, countdown_timer_.GetId());
    preview_timer_.Start(1000 / 20);
    stats_timer_.Start(500);

    Bind(wxEVT_MENU, &HomRecMainFrame::OnMenu, this, ID_FILE_OPEN_RECORDINGS, ID_VIEW_AUDIO_PANEL);
    Bind(wxEVT_CLOSE_WINDOW, &HomRecMainFrame::OnClose, this);
    Bind(wxEVT_ICONIZE, &HomRecMainFrame::OnIconize, this);
    Bind(wxEVT_SHOW, &HomRecMainFrame::OnShowEvent, this);
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
    fileMenu->Append(ID_FILE_EXPORT_HRC, "Export Settings (.hrc)...");
    fileMenu->Append(ID_FILE_IMPORT_HRC, "Import Settings (.hrc)...");
    fileMenu->AppendSeparator();
    fileMenu->Append(ID_FILE_EXIT, "Exit");
    menuBar->Append(fileMenu, "File");

    auto *viewMenu = new wxMenu();
    viewMenu->Append(ID_VIEW_ALWAYS_ON_TOP, "Always on Top");
    viewMenu->Append(ID_VIEW_FULLSCREEN, "Fullscreen\tF11");
    viewMenu->AppendCheckItem(ID_VIEW_OVERLAYS_PANEL, "Overlays Panel");
    viewMenu->Check(ID_VIEW_OVERLAYS_PANEL, state_.show_overlays_panel);
    viewMenu->AppendCheckItem(ID_VIEW_AUDIO_PANEL, "Audio Mixer");
    viewMenu->Check(ID_VIEW_AUDIO_PANEL, state_.show_audio_panel);
    viewMenu->AppendSeparator();
    viewMenu->Append(ID_VIEW_PC_ANALYTICS, "PC Analytics");
    viewMenu->Append(ID_VIEW_LOG, "Show Log");
    menuBar->Append(viewMenu, "View");

    auto *themeMenu = new wxMenu();
    themeMenu->Append(ID_THEME_DARK, "Dark");
    themeMenu->Append(ID_THEME_LIGHT, "Light");
    auto *settingsMenu = new wxMenu();
    settingsMenu->Append(ID_SETTINGS_OPEN, "Preferences...");
    // "Overlays..." (ID_OVERLAYS_MANAGE, the full editor window) removed --
    // the OverlaysDockPanel (View > Overlays Panel) is now the only way to
    // manage overlays; see overlays_dock_panel.h's header comment for why.
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

    // Audio mixer strip lives below the preview - real wx widgets now
    // (ColorSlider/ColorButton/LevelMeterPanel from audio_panel.h), no
    // native-HWND hosting needed the way OverlaysDockPanel below still does.
    audio_panel_ = std::make_unique<AudioPanel>(parent, state_, *rec_);
    rightColumn->Add(audio_panel_.get(), 0, wxEXPAND | wxTOP, 15);
    audio_panel_->on_close = [this]() {
        state_.show_audio_panel = false;
        if (audio_panel_) audio_panel_->Show(false);
        if (auto *mb = GetMenuBar()) mb->Check(ID_VIEW_AUDIO_PANEL, false);
        Layout();
    };
    audio_panel_->Show(state_.show_audio_panel);

    parentSizer->Add(rightColumn, 1, wxEXPAND);

    // Overlays dock - also raw-Win32, same reasoning as AudioPanel.
    overlays_host_ = new NativeHostPanel(parent);
    overlays_host_->SetMinSize(wxSize(220, -1));
    overlays_panel_ = std::make_unique<OverlaysDockPanel>(state_);
    overlays_panel_->Create((HWND)overlays_host_->GetHandle(), wxGetInstance(), 0, 0, 220, 500);
    overlays_host_->on_drawitem = [this](DRAWITEMSTRUCT *dis) {
        // OverlaysDockPanel doesn't currently expose a HandleDrawItem the
        // way AudioPanel does (its list items aren't owner-drawn) - no-op
        // here, left as a documented hook if that changes.
        (void)dis;
    };
    overlays_host_->on_command = [this](int id) {
        if (!overlays_panel_) return;
        overlays_panel_->OnCommand(id);
        if (id == ID_OVDOCK_CLOSE) {
            // OverlaysDockPanel::OnCommand() only hides its own native
            // child controls; also collapse the wx-level host panel and
            // keep the View menu checkbox in sync, same as toggling it
            // from the View menu does.
            if (overlays_host_) overlays_host_->Show(state_.show_overlays_panel);
            if (auto *mb = GetMenuBar()) mb->Check(ID_VIEW_OVERLAYS_PANEL, state_.show_overlays_panel);
            Layout();
        }
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
    // so re-theme every direct child of left_panel_ uniformly instead -
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
    // itself, not the owning frame - bind here, not via the frame's
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

void HomRecMainFrame::RequestStart() {
    if (state_.recording) return;

    if (countdown_timer_.IsRunning()) {
        // Clicking Start again mid-countdown cancels it, rather than
        // stacking a second countdown or being ignored silently.
        countdown_timer_.Stop();
        SetStatusState(wxString::FromUTF8(lang_.Get("ready")), theme_.text_secondary);
        if (file_lbl_) file_lbl_->SetLabel(wxString::FromUTF8(lang_.Get("ready")));
        return;
    }

    if (!state_.countdown_enabled) {
        DoStart();
        return;
    }

    countdown_remaining_ = 3;
    wxString msg = wxString::Format("Starting in %d\u2026", countdown_remaining_);
    SetStatusState(msg, theme_.warning);
    if (file_lbl_) file_lbl_->SetLabel(msg);
    countdown_timer_.Start(1000);
}

void HomRecMainFrame::OnCountdownTimer(wxTimerEvent &) {
    --countdown_remaining_;
    if (countdown_remaining_ <= 0) {
        countdown_timer_.Stop();
        DoStart();
        return;
    }
    wxString msg = wxString::Format("Starting in %d\u2026", countdown_remaining_);
    SetStatusState(msg, theme_.warning);
    if (file_lbl_) file_lbl_->SetLabel(msg);
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

    // "Show summary" setting: the "recording saved, open folder?" popup.
    // Ported from custom_messagebox.h (already used elsewhere in this
    // file, e.g. ShowWelcomeDialog) - this was the one consumer that had
    // never actually been wired up (state_.show_summary loaded from
    // settings and shown as a checkbox, but nothing read it at Stop()
    // time). Skipped if the setting is off, if the user checked "Don't
    // show again" earlier this session, or if there's nowhere to open
    // (empty output_folder).
    if (state_.show_summary && !summary_dont_show_again_ && !state_.output_folder.empty()) {
        bool dont_show = summary_dont_show_again_;
        bool open_folder = ShowCustomMessageBox(
            GetHWND(), wxGetInstance(), theme_,
            WideFromNarrow(lang_.Get("recording_saved")),
            WideFromNarrow(lang_.Get("recording_saved")),
            WideFromNarrow(state_.output_folder), dont_show);
        summary_dont_show_again_ = dont_show;
        if (open_folder) OpenRecordingsFolder();
    }
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

void HomRecMainFrame::OpenRecordingsFolder() {
    if (state_.output_folder.empty()) return;

    // Two separate reasons this window can end up hidden behind HomRec
    // instead of in front of it, both worth guarding against since we
    // can't tell from here which one actually applies on the user's
    // machine:
    //
    // 1. Windows' foreground-lock heuristic can let a newly-launched
    //    process's window open without taking focus at all if the OS
    //    decides this isn't a "user-initiated" enough action - telling
    //    Windows explicitly that the next foreground request from any
    //    process is allowed works around that.
    AllowSetForegroundWindow(ASFW_ANY);

    // 2. If Always on Top is on, HomRec is WS_EX_TOPMOST - which by
    //    definition stays above every non-topmost window regardless of
    //    activation, so Explorer would open "behind" it no matter how
    //    hard it tries to come to the front. Drop topmost just long
    //    enough for the folder window to appear, then restore it.
    bool was_topmost = (GetWindowStyleFlag() & wxSTAY_ON_TOP) != 0;
    if (was_topmost) {
        SetWindowStyleFlag(GetWindowStyleFlag() & ~wxSTAY_ON_TOP);
        pending_restore_topmost_ = true;
        restore_topmost_timer_.StartOnce(1500);
    }

    ShellExecuteA(GetHWND(), "open", state_.output_folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void HomRecMainFrame::OnRestoreTopmostTimer(wxTimerEvent &) {
    if (pending_restore_topmost_) {
        pending_restore_topmost_ = false;
        SetWindowStyleFlag(GetWindowStyleFlag() | wxSTAY_ON_TOP);
    }
}

void HomRecMainFrame::OnStartClicked(wxCommandEvent &) { if (state_.recording) DoStop(); else RequestStart(); }
void HomRecMainFrame::OnPauseClicked(wxCommandEvent &) { DoPause(); }

void HomRecMainFrame::OnMenu(wxCommandEvent &evt) {
    switch (evt.GetId()) {
        case ID_FILE_EXIT: Close(true); break;
        case ID_FILE_OPEN_RECORDINGS:
            OpenRecordingsFolder();
            break;
        case ID_FILE_SELECT_WINDOW:
            ShowWindowPickerDialog(GetHWND(), wxGetInstance(), state_);
            break;
        case ID_FILE_EXPORT_HRC: {
            wxFileDialog dlg(this, "Export Settings", wxEmptyString, "homrec_config.hrc",
                              "HomRec Config (*.hrc)|*.hrc", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (dlg.ShowModal() == wxID_OK) {
                std::wstring wpath = dlg.GetPath().ToStdWstring();
                std::string logPath = dlg.GetPath().ToUTF8().data();
                if (HrcConfig::Save(state_, wpath)) {
                    HrLog::Info("Exported settings to " + logPath);
                } else {
                    HrLog::Error("Failed to export settings to " + logPath);
                    wxMessageBox("Couldn't write that file.", "Export Settings", wxOK | wxICON_ERROR, this);
                }
            }
            break;
        }
        case ID_FILE_IMPORT_HRC: {
            wxFileDialog dlg(this, "Import Settings", wxEmptyString, wxEmptyString,
                              "HomRec Config (*.hrc)|*.hrc|All files (*.*)|*.*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dlg.ShowModal() == wxID_OK) {
                std::wstring wpath = dlg.GetPath().ToStdWstring();
                std::string logPath = dlg.GetPath().ToUTF8().data();
                if (HrcConfig::Load(state_, wpath)) {
                    HrLog::Info("Imported settings from " + logPath);
                    ApplyThemeColours();
                    ApplyLanguageText();
                    if (hotkey_handle_) { hr_hk_stop(hotkey_handle_); hr_hk_destroy(hotkey_handle_); hotkey_handle_ = nullptr; }
                    SetupHotkeys();
                    wxMessageBox("Settings imported.", "Import Settings", wxOK | wxICON_INFORMATION, this);
                } else {
                    HrLog::Error("Failed to import settings from " + logPath);
                    wxMessageBox("Couldn't read that file.", "Import Settings", wxOK | wxICON_ERROR, this);
                }
            }
            break;
        }
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
        case ID_VIEW_AUDIO_PANEL:
            state_.show_audio_panel = !state_.show_audio_panel;
            if (audio_panel_) audio_panel_->Show(state_.show_audio_panel);
            if (auto *mb = GetMenuBar()) mb->Check(ID_VIEW_AUDIO_PANEL, state_.show_audio_panel);
            Layout();
            break;
        case ID_VIEW_PC_ANALYTICS: ShowPcAnalyticsDialog(GetHWND(), wxGetInstance(), state_.output_folder); break;
        case ID_VIEW_LOG: ShowLogViewerDialog(GetHWND(), wxGetInstance()); break;
        case ID_THEME_DARK:
            state_.current_theme = "dark"; theme_ = GetBuiltinTheme("dark"); ApplyThemeColours(); break;
        case ID_THEME_LIGHT:
            state_.current_theme = "light"; theme_ = GetBuiltinTheme("light"); ApplyThemeColours(); break;
        case ID_SETTINGS_OPEN:
            if (ShowSettingsDialog(this, state_, theme_) && rec_raw_) {
                rec_raw_->RefreshPreviewSettings();
            }
            // Hotkeys live on a tab of this same dialog - re-apply them in
            // case they changed, same as the old (now-removed) "Advanced
            // Settings" entry used to do.
            if (hotkey_handle_) { hr_hk_stop(hotkey_handle_); hr_hk_destroy(hotkey_handle_); hotkey_handle_ = nullptr; }
            SetupHotkeys();
            break;
        // ID_OVERLAYS_MANAGE removed along with overlay_manager.cpp's
        // ShowOverlayManager() -- see overlays_dock_panel.h.
        case ID_HELP_CONSOLE:
            if (!console_) console_ = std::make_unique<ConsoleWindow>(state_, rec_.get(), GetHWND(), plugins_.get());
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
    if (rec_) rec_->SyncOverlays();
    if (preview_panel_) preview_panel_->Refresh(false);
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
                           wxString::Format(" %dx%d", rec_ ? rec_->output_width() : 0,
                                            rec_ ? rec_->output_height() : 0);
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
    if (rec_) rec_->SetPreviewVisible(!evt.IsIconized());
    evt.Skip();
}

void HomRecMainFrame::OnShowEvent(wxShowEvent &evt) {
    if (rec_) rec_->SetPreviewVisible(evt.IsShown());
    evt.Skip();
}

void HomRecMainFrame::OnHotkeyEvent(wxThreadEvent &evt) {
    wxEventType t = evt.GetEventType();
    if (t == EVT_HOTKEY_START_STOP) { if (state_.recording) DoStop(); else RequestStart(); }
    else if (t == EVT_HOTKEY_PAUSE) { DoPause(); }
    else if (t == EVT_HOTKEY_FULLSCREEN) { ToggleFullscreenNative(); }
}
