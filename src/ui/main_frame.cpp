#include "main_frame.h"
#include "version.h"
#include "settings_dialog.h"
#include "preset_dialog.h"
// (overlay_manager.h removed - see overlays_dock_panel.h)
#include "welcome_dialog.h"
#include "overlay_placement_dialog.h"
#include "pc_analytics_dialog.h"
#include "log_viewer_dialog.h"
#include "window_picker_dialog.h"
#include "hide_window_dialog.h"
#include "custom_messagebox.h"
#include "hrc_config.h"
#include "win32_theme.h"
#include "../hr_log.h"
#include "../hr_pc_log.h"
#include "../hr_plugin_log.h"
#include "../hr_update.h"
#include <wx/dcbuffer.h>
#include <wx/msw/private.h>
#include <wx/filedlg.h>
#include <thread> 
#include <atomic>
#include <sstream>
#include <wx/msgdlg.h>
#include <functional>
#include <algorithm>
#include <string>
#include <cctype>
#include <cstring>
#include <cmath>
#include <ctime>

extern "C" {
    void *hr_hk_create();
    void hr_hk_destroy(void *handle);
    void hr_hk_set_callbacks(void *handle, void (*start_stop)(), void (*pause)(), void (*fullscreen)(), void (*save_replay)());
    void hr_hk_configure(void *handle, const char *start_stop_str, const char *pause_str, const char *fullscreen_str, const char *save_replay_str);
    int hr_hk_start(void *handle);
    void hr_hk_stop(void *handle);
    // Custom action hotkeys - see hr_hotkey.cpp's own doc comment on these.
    void hr_hk_set_custom_callback(void *handle, void (*cb)(int id));
    void hr_hk_clear_custom(void *handle);
    int hr_hk_add_custom(void *handle, int id, const char *keystring);

    void *hr_settings_create();
    void hr_settings_destroy(void *handle);
    int hr_settings_load(void *handle, const char *path);
    const char *hr_settings_get_output_folder(const void *h);
    int hr_settings_get_quality(const void *h);
    int hr_settings_get_fps(const void *h);
    int hr_settings_get_monitor(const void *h);
    int hr_settings_get_resolution_pct(const void *h);
    int hr_settings_get_resolution_mode(const void *h);
    int hr_settings_get_resolution_w(const void *h);
    int hr_settings_get_resolution_h(const void *h);
    int hr_settings_get_preview_quality_pct(const void *h);
    int hr_settings_get_preview_fps(const void *h);
    const char *hr_settings_get_codec(const void *h);
    const char *hr_settings_get_theme(const void *h);
    int hr_settings_get_flag(const void *h, const char *name);
    // Phase 1 (see commands.md): the set_*/save side of this API is no
    // longer used here - PersistSettings() (this file) and HrcConfig::Save
    // (hrc_config.h) replace it. Only the one-time migration read path
    // above (get_* + load) is still needed, for upgrading an install that
    // only ever had homrec_settings.json.
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
        // The overlay dock's raw HWNDs (the STATIC "panel"
        // background and the LISTBOX row list -- the two "+"/"x" buttons
        // already theme themselves via HrWin32Theme::ThemeButton()) send
        // WM_CTLCOLORSTATIC/WM_CTLCOLORLISTBOX here, to their *immediate*
        // parent, same as WM_HSCROLL/WM_DRAWITEM above. Nothing answered
        // them, so they fell through to wxPanel's default handling and
        // came back as plain system white/gray instead of the app's dark
        // theme -- the same fix every other raw-Win32 dialog in this app
        // already applies (see e.g. window_picker_dialog.cpp).
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
std::atomic<HomRecMainFrame *> g_frame{nullptr};

constexpr int kOuterPad   = 15;
constexpr int kLeftPanelW = 240;

wxDEFINE_EVENT(EVT_HOTKEY_START_STOP, wxThreadEvent);
wxDEFINE_EVENT(EVT_HOTKEY_PAUSE, wxThreadEvent);
wxDEFINE_EVENT(EVT_HOTKEY_FULLSCREEN, wxThreadEvent);
wxDEFINE_EVENT(EVT_HOTKEY_SAVE_REPLAY, wxThreadEvent);
wxDEFINE_EVENT(EVT_HOTKEY_CUSTOM, wxThreadEvent);
wxDEFINE_EVENT(EVT_HOM_UPDATES_CHECKED, wxThreadEvent);

void HotkeyStartStopThunk() { if (auto *f = g_frame.load()) wxQueueEvent(f, new wxThreadEvent(EVT_HOTKEY_START_STOP)); }
void HotkeyPauseThunk()     { if (auto *f = g_frame.load()) wxQueueEvent(f, new wxThreadEvent(EVT_HOTKEY_PAUSE)); }
void HotkeyFullscreenThunk(){ if (auto *f = g_frame.load()) wxQueueEvent(f, new wxThreadEvent(EVT_HOTKEY_FULLSCREEN)); }
void HotkeySaveReplayThunk(){ if (auto *f = g_frame.load()) wxQueueEvent(f, new wxThreadEvent(EVT_HOTKEY_SAVE_REPLAY)); }

// Custom (user-defined) hotkeys: hr_hotkey.cpp only ever hands us back the
// numeric id we picked when registering the binding (see
// ConfigureHotkeysFromState() below) - g_custom_hotkey_actions maps that id
// (offset from HK_CUSTOM_BASE=100) back to the action string the user typed
// in Settings > Hotkeys, so OnHotkeyEvent's EVT_HOTKEY_CUSTOM case knows
// what to actually run. Same single-frame-per-process assumption as
// g_frame above.
constexpr int kCustomHotkeyBase = 100;
std::vector<std::string> g_custom_hotkey_actions;

void HotkeyCustomThunk(int id) {
    if (auto *f = g_frame.load()) {
        auto *evt = new wxThreadEvent(EVT_HOTKEY_CUSTOM);
        evt->SetInt(id);
        wxQueueEvent(f, evt);
    }
}

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
namespace {
// Size (px) of the square resize handles drawn on/hit-tested against each
// overlay frame - big enough to grab comfortably on a scaled-down preview.
constexpr int kOverlayHandle = 10;

// Where an overlay's rectangle lands inside the on-screen preview bitmap.
// `prev` is the bitmap's rect within the panel, sx/sy convert overlay-space
// (native capture) pixels to on-screen pixels.
wxRect OverlayScreenRect(const OverlayDef &ov, const wxRect &prev, double sx, double sy) {
    int rx = prev.GetX() + (int)(ov.x * sx);
    int ry = prev.GetY() + (int)(ov.y * sy);
    int rw = std::max(kOverlayHandle * 2, (int)(ov.w * sx));
    int rh = std::max(kOverlayHandle * 2, (int)(ov.h * sy));
    return wxRect(rx, ry, rw, rh);
}
} // namespace

PreviewPanel::PreviewPanel(wxWindow *parent, RecordingController *&rec, AppState &state)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS),
      rec_(rec), state_(state), overlay_save_timer_(this) {
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
    Bind(wxEVT_RIGHT_UP, &PreviewPanel::OnRightUp, this);
    Bind(wxEVT_KEY_DOWN, &PreviewPanel::OnKeyDown, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &PreviewPanel::OnCaptureLost, this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { FlushOverlaySave(); }, overlay_save_timer_.GetId());
}

void PreviewPanel::ScheduleOverlaySave() {
    overlay_save_pending_ = true;
    overlay_save_timer_.StartOnce(400); // restarts on every call = debounce
}

void PreviewPanel::FlushOverlaySave() {
    overlay_save_timer_.Stop();
    if (!overlay_save_pending_) return;
    overlay_save_pending_ = false;
    HrcConfig::SaveOverlaysOnly(state_.overlays, HrcConfig::kOverlaysAutosavePath);
}

void PreviewPanel::PollRefresh() {
    if (snapshot_mode_) { Refresh(false); return; } // static image; painting is cached and cheap
    if (state_.disable_preview) {
        if ((++idle_ticks_ % 10) == 0) Refresh(false);
        return;
    }
    const uint64_t cur = rec_ ? rec_->PreviewSeq() : 0;
    if (cur != pv_seq_ || (cur == 0 && have_frame_)) { Refresh(false); return; }
    if ((++idle_ticks_ % 10) == 0) Refresh(false);
}

// The coordinate space overlays are measured in for whatever is currently
// on screen: the pipeline's real composite size for the live feed (the
// cropped window rect in window-capture mode, the full monitor otherwise),
// or the size the one-off screenshot was taken at in snapshot mode. Using
// capture_width()/height() for this instead is wrong whenever a window crop
// is active - it would scale every frame by the monitor size, not the
// picture actually shown.
bool PreviewPanel::GetNativeSize(int &w, int &h) const {
    w = h = 0;
    if (snapshot_mode_) {
        w = snapshot_native_w_ > 0 ? snapshot_native_w_ : snapshot_w_;
        h = snapshot_native_h_ > 0 ? snapshot_native_h_ : snapshot_h_;
        return w > 0 && h > 0;
    }
    if (have_frame_ && frame_native_w_ > 0 && frame_native_h_ > 0) {
        w = frame_native_w_;
        h = frame_native_h_;
        return true;
    }
    return rec_ && rec_->GetPreviewNativeSize(w, h) && w > 0 && h > 0;
}

void PreviewPanel::OnPaint(wxPaintEvent &) {
    wxAutoBufferedPaintDC dc(this);

    if (state_.disable_preview && !snapshot_mode_) {
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
        int smileyY = (cs.GetHeight() - ext.GetHeight()) / 2;
        dc.DrawText(smiley, (cs.GetWidth() - ext.GetWidth()) / 2, smileyY);

        // Preview being off means there's no live image to drag overlays
        // against - clicking anywhere in this area grabs a one-off
        // screenshot and edits them on that instead (see OnLeftDown() /
        // BeginSnapshotEditing()). This is just the pointer to it, and
        // it's what "edit settings hint-no-overlay false" (bter plugin
        // console command) turns off.
        if (state_.hint_no_overlay) {
            wxFont hintFont = GetFont();
            dc.SetFont(hintFont);
            wxString hint = "Preview is off - click here to move/resize your overlays on a screenshot.";
            wxSize hintExt = dc.GetTextExtent(hint);
            dc.DrawText(hint, (cs.GetWidth() - hintExt.GetWidth()) / 2, smileyY + ext.GetHeight() + 8);
        }
        return;
    }

    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();

    // Where this paint's pixels come from. No per-paint copies any more: the
    // snapshot is read in place, and the live path asks the pipeline for a
    // frame only if its sequence number moved (GetPreviewFrameIfNew).
    const uint8_t *src = nullptr;
    int w = 0, h = 0;
    bool new_frame = false;
    if (snapshot_mode_) {
        w = snapshot_w_; h = snapshot_h_;
        if (w > 0 && h > 0 && snapshot_buf_.size() >= (size_t)w * (size_t)h * 3) {
            src = snapshot_buf_.data();
            new_frame = snapshot_dirty_;
        }
    } else if (rec_) {
        const int r = rec_->GetPreviewFrameIfNew(frame_buf_, frame_w_, frame_h_,
                                                 frame_native_w_, frame_native_h_, pv_seq_);
        if (r == 0) {
            pv_seq_ = 0;
            have_frame_ = false;
        } else {
            have_frame_ = true;
            new_frame = (r == 1);
            if (frame_w_ > 0 && frame_h_ > 0 &&
                frame_buf_.size() >= (size_t)frame_w_ * (size_t)frame_h_ * 3) {
                src = frame_buf_.data();
                w = frame_w_; h = frame_h_;
            }
        }
    }
    if (!src) {
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
        const size_t need = (size_t)w * (size_t)h * 3;
        const bool geometry_changed = w != cached_src_w_ || h != cached_src_h_ ||
                                      cs.GetWidth() != cached_panel_w_ ||
                                      cs.GetHeight() != cached_panel_h_;
        bool rescale = cache_dirty_ || !cached_bmp_.IsOk() || geometry_changed;
        // A new sequence number doesn't always mean new pixels (a static
        // desktop republishes the same thumbnail); the byte compare is far
        // cheaper than the bilinear rescale it can avoid.
        if (!rescale && new_frame)
            rescale = last_frame_buf_.size() != need ||
                      std::memcmp(src, last_frame_buf_.data(), need) != 0;

        if (rescale) {
            // Preserve aspect ratio (letterbox/pillarbox) instead of stretching
            // to fill the panel.
            wxImage img(w, h, const_cast<uint8_t *>(src), /*static_data=*/true);
            double scale = std::min((double)cs.GetWidth() / w, (double)cs.GetHeight() / h);
            int dw = std::max(1, (int)(w * scale));
            int dh = std::max(1, (int)(h * scale));
            wxImage scaled = img.Scale(dw, dh, wxIMAGE_QUALITY_BILINEAR);
            cached_bmp_ = wxBitmap(scaled);
            cached_src_w_ = w; cached_src_h_ = h;
            cached_dst_w_ = dw; cached_dst_h_ = dh;
            cached_panel_w_ = cs.GetWidth(); cached_panel_h_ = cs.GetHeight();
            last_frame_buf_.assign(src, src + need);
            cache_dirty_ = false;
        }
        if (snapshot_mode_) snapshot_dirty_ = false;
        dc.DrawBitmap(cached_bmp_, (cs.GetWidth() - cached_dst_w_) / 2, (cs.GetHeight() - cached_dst_h_) / 2);
    }

    // Draw a frame around EVERY overlay (with its name/type above it and a
    // resize handle on the top-left and bottom-right corners) directly on
    // top of the preview, so it can be clicked, moved and resized right
    // here instead of only through the separate "Position Overlays..."
    // window. A clicked overlay stays selected (gold, thicker frame) after
    // the mouse button is released, so it's always obvious which one is
    // being edited.
    wxRect prevRect;
    int nw = 0, nh = 0;
    if (GetPreviewRect(prevRect) && GetNativeSize(nw, nh)) {
        double sx = (double)prevRect.GetWidth() / nw;
        double sy = (double)prevRect.GetHeight() / nh;
        if (selected_overlay_index_ >= (int)state_.overlays.size()) selected_overlay_index_ = -1;

        for (size_t i = 0; i < state_.overlays.size(); ++i) {
            const auto &ov = state_.overlays[i];
            // In snapshot mode every overlay is shown/draggable, even a
            // hidden one - that's the only way left to reach it when the
            // live preview (where it'd otherwise be temporarily unhidden
            // via Show/Hide) is off.
            if (!ov.visible && !snapshot_mode_) continue;
            wxRect r = OverlayScreenRect(ov, prevRect, sx, sy);
            bool active = ((int)i == drag_overlay_index_) || ((int)i == selected_overlay_index_);
            wxColour accent = active ? wxColour(255, 210, 90) : wxColour(120, 170, 250);

            // Dark outer line first so the frame stays readable on top of
            // both bright and dark screen content.
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(wxColour(0, 0, 0), 1));
            dc.DrawRectangle(r.GetX() - 1, r.GetY() - 1, r.GetWidth() + 2, r.GetHeight() + 2);
            dc.SetPen(wxPen(accent, active ? 2 : 1));
            dc.DrawRectangle(r);

            dc.SetBrush(wxBrush(accent));
            dc.SetPen(wxPen(wxColour(0, 0, 0), 1));
            dc.DrawRectangle(r.GetRight() - kOverlayHandle + 1, r.GetBottom() - kOverlayHandle + 1,
                             kOverlayHandle, kOverlayHandle);
            dc.DrawRectangle(r.GetX(), r.GetY(), kOverlayHandle, kOverlayHandle);

            wxString label = wxString::FromUTF8((ov.name.empty() ? ov.type : ov.name).c_str());
            if (!ov.visible) label += " (hidden)";
            if (!label.empty()) {
                wxSize te = dc.GetTextExtent(label);
                int ty = r.GetY() >= te.GetHeight() + 2 ? r.GetY() - te.GetHeight() - 2
                                                         : r.GetY() + kOverlayHandle + 2;
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(wxColour(20, 20, 26)));
                dc.DrawRectangle(r.GetX(), ty, te.GetWidth() + 6, te.GetHeight() + 2);
                dc.SetTextForeground(accent);
                dc.DrawText(label, r.GetX() + 3, ty + 1);
            }
        }
    }

    if (snapshot_mode_) {
        wxString msg = "Editing overlays on a screenshot - right-click: Refresh / Done (Esc)";
        wxSize te = dc.GetTextExtent(msg);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(wxColour(20, 20, 26)));
        dc.DrawRectangle(0, 0, te.GetWidth() + 12, te.GetHeight() + 6);
        dc.SetTextForeground(wxColour(255, 210, 90));
        dc.DrawText(msg, 6, 3);
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

// Topmost overlay under (mx,my), or -1. Later entries paint on top of
// earlier ones, so walk backwards to grab the one the user actually sees.
// `corner` reports whether a resize handle (vs. the plain body) was hit.
int PreviewPanel::HitTestOverlay(int mx, int my, Corner &corner) const {
    corner = Corner::kNone;
    wxRect prevRect;
    int nw = 0, nh = 0;
    if (!GetPreviewRect(prevRect) || !GetNativeSize(nw, nh)) return -1;
    double sx = (double)prevRect.GetWidth() / nw;
    double sy = (double)prevRect.GetHeight() / nh;

    for (int i = (int)state_.overlays.size() - 1; i >= 0; --i) {
        const auto &ov = state_.overlays[(size_t)i];
        if (!ov.visible && !snapshot_mode_) continue;
        wxRect r = OverlayScreenRect(ov, prevRect, sx, sy);
        wxRect brHandle(r.GetRight() - kOverlayHandle + 1, r.GetBottom() - kOverlayHandle + 1,
                        kOverlayHandle, kOverlayHandle);
        wxRect tlHandle(r.GetX(), r.GetY(), kOverlayHandle, kOverlayHandle);
        if (brHandle.Contains(mx, my))      { corner = Corner::kBottomRight; return i; }
        if (tlHandle.Contains(mx, my))      { corner = Corner::kTopLeft;     return i; }
        if (r.Contains(mx, my))             { corner = Corner::kNone;        return i; }
    }
    return -1;
}

void PreviewPanel::OnLeftDown(wxMouseEvent &evt) {
    SetFocus(); // so the arrow keys / Esc reach OnKeyDown()

    // Preview is off: there's no live picture to grab an overlay on, so a
    // click here starts an edit-on-screenshot session instead (nothing
    // visible happened at all on click before this).
    if (state_.disable_preview && !snapshot_mode_) {
        if (!state_.overlays.empty()) BeginSnapshotEditing();
        evt.Skip();
        return;
    }

    Corner corner = Corner::kNone;
    int hit = HitTestOverlay(evt.GetX(), evt.GetY(), corner);
    if (hit >= 0) {
        auto &ov = state_.overlays[(size_t)hit];
        drag_corner_ = corner;
        drag_overlay_index_ = hit;
        selected_overlay_index_ = hit;
        drag_start_mouse_x_ = evt.GetX();
        drag_start_mouse_y_ = evt.GetY();
        drag_start_ov_x_ = ov.x;
        drag_start_ov_y_ = ov.y;
        drag_start_ov_w_ = ov.w;
        drag_start_ov_h_ = ov.h;
        drag_moved_ = false;
        if (!HasCapture()) CaptureMouse();
        Refresh();
        return;
    }

    // A plain click on empty space deselects, and also clears any drag
    // state left stuck by a drag that never got a matching OnLeftUp/
    // OnCaptureLost (mouse capture silently stolen by another window
    // mid-drag, a modal dialog popping up while the button was held...).
    if (drag_overlay_index_ >= 0 || selected_overlay_index_ >= 0) {
        if (HasCapture()) ReleaseMouse();
        drag_overlay_index_ = -1;
        selected_overlay_index_ = -1;
        drag_corner_ = Corner::kNone;
        Refresh();
    }
    evt.Skip();
}

void PreviewPanel::OnMouseMove(wxMouseEvent &evt) {
    // Button released without us ever seeing the LEFT_UP (released over a
    // modal/other window): finish the drag properly instead of leaving it
    // stuck and the position unsaved.
    if (drag_overlay_index_ >= 0 && !evt.LeftIsDown()) EndDrag();
    if (drag_overlay_index_ < 0 || !evt.LeftIsDown()) {
        // Not dragging - just tell the user what's grabbable here.
        Corner corner = Corner::kNone;
        int hit = (state_.disable_preview && !snapshot_mode_)
                      ? -1 : HitTestOverlay(evt.GetX(), evt.GetY(), corner);
        wxStockCursor cur = wxCURSOR_ARROW;
        if (state_.disable_preview && !snapshot_mode_ && !state_.overlays.empty()) cur = wxCURSOR_HAND;
        else if (hit >= 0) {
            cur = corner == Corner::kBottomRight ? wxCURSOR_SIZENWSE
                : corner == Corner::kTopLeft     ? wxCURSOR_SIZENWSE
                                                 : wxCURSOR_SIZING;
        }
        SetCursor(wxCursor(cur));
        evt.Skip();
        return;
    }
    if (drag_overlay_index_ >= (int)state_.overlays.size()) { // list changed under us
        drag_overlay_index_ = -1;
        evt.Skip();
        return;
    }
    wxRect prevRect;
    int cw = 0, ch = 0;
    if (!GetPreviewRect(prevRect) || prevRect.GetWidth() <= 0 || prevRect.GetHeight() <= 0 ||
        !GetNativeSize(cw, ch)) {
        evt.Skip();
        return;
    }
    double sx = (double)prevRect.GetWidth() / cw;
    double sy = (double)prevRect.GetHeight() / ch;

    // Convert the mouse delta from on-screen preview pixels back to real
    // capture-resolution pixels (the space OverlayDef::x/y/w/h -- and the
    // actual recording -- live in), not thumbnail pixels.
    int dx = (int)std::lround((evt.GetX() - drag_start_mouse_x_) / sx);
    int dy = (int)std::lround((evt.GetY() - drag_start_mouse_y_) / sy);

    auto &ov = state_.overlays[(size_t)drag_overlay_index_];
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
    if (dx != 0 || dy != 0) drag_moved_ = true;
    Refresh();
}

void PreviewPanel::EndDrag() {
    if (drag_overlay_index_ < 0) return;
    if (HasCapture()) ReleaseMouse();
    drag_overlay_index_ = -1; // selected_overlay_index_ stays - the frame stays highlighted
    drag_corner_ = Corner::kNone;
    // This in-place drag-on-the-preview path bypasses both
    // OverlaysDockPanel::Refresh() and ShowOverlayPlacementDialog() (the
    // other places that persist state_.overlays), so a finished drag has to
    // save itself - but only if the overlay actually moved: a plain click
    // used to rewrite the whole autosave file for nothing.
    if (drag_moved_) {
        drag_moved_ = false;
        overlay_save_pending_ = true;
        FlushOverlaySave();
    }
    Refresh();
}

void PreviewPanel::OnLeftUp(wxMouseEvent &evt) {
    EndDrag();
    evt.Skip();
}

// Right-click while editing on a screenshot: Refresh (re-take it) / Done.
void PreviewPanel::OnRightUp(wxMouseEvent &evt) {
    if (!snapshot_mode_) { evt.Skip(); return; }
    wxMenu menu;
    menu.Append(wxID_REFRESH, "Refresh screenshot");
    menu.Append(wxID_CLOSE, "Done editing\tEsc");
    int id = GetPopupMenuSelectionFromUser(menu);
    if (id == wxID_REFRESH) RefreshSnapshot();
    else if (id == wxID_CLOSE) EndSnapshotEditingSession();
}

void PreviewPanel::OnKeyDown(wxKeyEvent &evt) {
    int key = evt.GetKeyCode();
    if (key == WXK_ESCAPE && snapshot_mode_) {
        EndSnapshotEditingSession();
        return;
    }
    // Arrow keys nudge the selected overlay by 1 native pixel (10 with
    // Shift) - precise placement that's fiddly to do with the mouse on a
    // scaled-down preview.
    bool arrow = key == WXK_LEFT || key == WXK_RIGHT || key == WXK_UP || key == WXK_DOWN;
    if (arrow && selected_overlay_index_ >= 0 &&
        selected_overlay_index_ < (int)state_.overlays.size() && drag_overlay_index_ < 0) {
        int cw = 0, ch = 0;
        if (GetNativeSize(cw, ch)) {
            int step = evt.ShiftDown() ? 10 : 1;
            auto &ov = state_.overlays[(size_t)selected_overlay_index_];
            if (key == WXK_LEFT)  ov.x -= step;
            if (key == WXK_RIGHT) ov.x += step;
            if (key == WXK_UP)    ov.y -= step;
            if (key == WXK_DOWN)  ov.y += step;
            ov.x = std::clamp(ov.x, 0, std::max(0, cw - ov.w));
            ov.y = std::clamp(ov.y, 0, std::max(0, ch - ov.h));
            ScheduleOverlaySave(); // debounced - a held arrow key repeats ~30x/sec
            Refresh();
            return;
        }
    }
    evt.Skip();
}

void PreviewPanel::OnCaptureLost(wxMouseCaptureLostEvent &) {
    drag_overlay_index_ = -1;
    drag_corner_ = Corner::kNone;
    Refresh();
}

// Preview is off: grab one screenshot and edit overlays on it, right here in
// the preview area. Ends with Esc / right-click > Done editing.
bool PreviewPanel::BeginSnapshotEditing() {
    if (!rec_) return false;
    std::vector<uint8_t> buf;
    int w = 0, h = 0, nw = 0, nh = 0;
    if (!rec_->CaptureSnapshotFrame(buf, w, h, nw, nh, /*first_call=*/true)) {
        rec_->EndSnapshotEditing();
        wxMessageBox("Couldn't capture a screenshot to edit overlays against - try again in a moment.",
                     "HomRec", wxOK | wxICON_WARNING, this);
        return false;
    }
    EnterSnapshotMode(buf, w, h, nw, nh);
    return true;
}

void PreviewPanel::RefreshSnapshot() {
    if (!rec_ || !snapshot_mode_) return;
    std::vector<uint8_t> buf;
    int w = 0, h = 0, nw = 0, nh = 0;
    if (rec_->CaptureSnapshotFrame(buf, w, h, nw, nh, /*first_call=*/false)) {
        UpdateSnapshotFrame(buf, w, h, nw, nh);
    }
}

void PreviewPanel::EndSnapshotEditingSession() {
    if (!snapshot_mode_) return;
    ExitSnapshotMode();
    // Lets the temporary preview pipeline CaptureSnapshotFrame() started go
    // away again if Disable live preview is still on.
    if (rec_) rec_->EndSnapshotEditing();
}

void PreviewPanel::EnterSnapshotMode(const std::vector<uint8_t> &buf, int w, int h,
                                     int native_w, int native_h) {
    snapshot_mode_ = true;
    UpdateSnapshotFrame(buf, w, h, native_w, native_h);
}

void PreviewPanel::UpdateSnapshotFrame(const std::vector<uint8_t> &buf, int w, int h,
                                       int native_w, int native_h) {
    snapshot_buf_ = buf;
    snapshot_w_ = w;
    snapshot_h_ = h;
    snapshot_native_w_ = native_w;
    snapshot_native_h_ = native_h;
    snapshot_dirty_ = true;
    cache_dirty_ = true;
    pv_seq_ = 0; // live frame must be re-fetched when snapshot mode ends
    Refresh();
}

void PreviewPanel::ExitSnapshotMode() {
    if (HasCapture()) ReleaseMouse();
    snapshot_mode_ = false;
    snapshot_buf_.clear();
    snapshot_w_ = snapshot_h_ = 0;
    snapshot_native_w_ = snapshot_native_h_ = 0;
    snapshot_dirty_ = false;
    cache_dirty_ = true;
    pv_seq_ = 0;
    FlushOverlaySave();
    drag_overlay_index_ = -1;
    selected_overlay_index_ = -1;
    drag_corner_ = Corner::kNone;
    SetCursor(wxCursor(wxCURSOR_ARROW));
    Refresh();
}

// ---------------------------------------------------------------------------
// HomRecMainFrame
// ---------------------------------------------------------------------------
HomRecMainFrame::HomRecMainFrame()
    : wxFrame(nullptr, wxID_ANY, "HomRec", wxDefaultPosition, wxSize(1300, 750)),
      countdown_timer_(this), schedule_timer_(this),
      preview_timer_(this), stats_timer_(this), level_meter_timer_(this), restore_topmost_timer_(this) {
    g_frame = this;
    SetIcon(wxIcon("#1", wxBITMAP_TYPE_ICO_RESOURCE));

    // ====== Settings load (Phase 1 storage migration - see commands.md) ======
    // homrec.hrc (HrcConfig, .hrc format) replaces homrec_settings.json
    // (hr_settings.cpp) as the app's own auto-managed settings file - see
    // hr_settings.cpp's header comment for the JSON-whitelist bug class
    // this sidesteps (a field missing from that whitelist silently
    // reverting to its compiled-in default every launch, which is exactly
    // what happened to show_summary/show_overlays_panel there). Bootstrap
    // order:
    //   1. Read the default location (HrcConfig::kDefaultSettingsPath) if
    //      present. settings_dialog.cpp's OnSave always keeps a live
    //      mirror of the real settings there even when a custom path is
    //      configured, purely so this one fixed location is always
    //      enough to find - or BE - the real settings, without a separate
    //      pointer-file format just for that redirect.
    //   2. If that mirror declares a different settings_path, load from
    //      there too (the actual file, in case it's been hand-edited more
    //      recently than the last in-app Save mirrored it over).
    //   3. If neither exists yet, this is either a fresh install or an
    //      upgrade from a version that only ever had homrec_settings.json
    //      - read that once with the old JSON engine and immediately
    //      write a .hrc so every later launch takes the fast, direct path
    //      above instead of this one-time migration branch.
    std::wstring hrc_path = HrcConfig::kDefaultSettingsPath;
    bool have_hrc = HrcConfig::Load(state_, hrc_path);
    if (have_hrc && !state_.settings_path.empty()) {
        std::wstring custom_path = WideFromNarrow(state_.settings_path);
        if (custom_path != hrc_path) HrcConfig::Load(state_, custom_path);
    }

    if (!have_hrc) {
        void *settings = hr_settings_create();
        if (hr_settings_load(settings, "homrec_settings.json")) {
            const char *folder = hr_settings_get_output_folder(settings);
            state_.output_folder = (folder && folder[0]) ? folder : "recordings";
            state_.quality = hr_settings_get_quality(settings);
            state_.target_fps = hr_settings_get_fps(settings);
            state_.monitor_id = hr_settings_get_monitor(settings);
            state_.scale_factor = hr_settings_get_resolution_pct(settings) / 100.0;
            state_.resolution_mode = hr_settings_get_resolution_mode(settings) != 0
                                          ? ResolutionMode::Absolute : ResolutionMode::Percent;
            state_.resolution_w = hr_settings_get_resolution_w(settings);
            state_.resolution_h = hr_settings_get_resolution_h(settings);
            state_.preview_quality_pct = hr_settings_get_preview_quality_pct(settings);
            state_.preview_fps = hr_settings_get_preview_fps(settings);
            state_.countdown_enabled = hr_settings_get_flag(settings, "countdown") != 0;
            state_.timestamp_enabled = hr_settings_get_flag(settings, "timestamp") != 0;
            state_.cursor_enabled = hr_settings_get_flag(settings, "cursor") != 0;
            state_.show_summary = hr_settings_get_flag(settings, "show_summary") != 0;
            state_.show_overlays_panel = hr_settings_get_flag(settings, "show_overlays_panel") != 0;
            state_.show_audio_panel = hr_settings_get_flag(settings, "show_audio_panel") != 0;
            state_.disable_preview = hr_settings_get_flag(settings, "disable_preview") != 0;
            state_.minimize_to_tray = hr_settings_get_flag(settings, "minimize_tray") != 0;
            state_.hint_no_overlay = hr_settings_get_flag(settings, "hint_no_overlay") != 0;
            state_.system_logging_enabled = hr_settings_get_flag(settings, "system_logging_enabled") != 0;
            state_.plugin_logging_enabled = hr_settings_get_flag(settings, "plugin_logging_enabled") != 0;
            const char *codec = hr_settings_get_codec(settings);
            if (codec && codec[0]) state_.video_codec = codec;
            const char *theme = hr_settings_get_theme(settings);
            if (theme && theme[0]) state_.current_theme = theme;

            // One-time migration: adopt .hrc as the authoritative format
            // from now on, so this branch isn't taken again next launch
            // (have_hrc will be true above once this file exists).
            HrcConfig::Save(state_, hrc_path);
        } else {
            state_.output_folder = "recordings";
            state_.first_launch = true;
        }
        hr_settings_destroy(settings);
    }

    // Load back whatever the Overlays panel had last saved (see
    // hrc_config.h's comment on SaveOverlaysOnly/LoadOverlaysOnly) - this
    // is what's missing before, overlays used to always start empty every
    // launch even though everything else in state_ was restored above.
    HrcConfig::LoadOverlaysOnly(state_.overlays, HrcConfig::kOverlaysAutosavePath);

    // Apply the Security tab's logging toggles immediately at startup -
    // both loggers default to enabled internally, so this is a no-op
    // unless the user had actually turned one off last session.
    HrPcLog::SetEnabled(state_.system_logging_enabled);
    HrPluginLog::SetEnabled(state_.plugin_logging_enabled);

    lang_ = LanguageTable::Load(state_.current_language, "Assets\\L");
    theme_ = GetBuiltinTheme(state_.current_theme);

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

    if (state_.instant_replay_enabled) {
        std::wstring err;
        if (!rec_->EnableInstantReplay(err)) {
            HrLog::Error(std::string("Instant Replay: failed to start at launch - ") +
                         std::string(err.begin(), err.end()));
        }
    }

    CheckHomUpdatesAsync();

    plugins_ = std::make_unique<LuaPluginEngine>("plugins");
    plugins_->SetContext(rec_.get(), &theme_);
    plugins_->LoadAll();

    // ====== cfg/autoexec.cfg ======
    // Console is normally created lazily, the first time someone opens it
    // (Help > Console / Ctrl+Shift+T) - EnsureCreated() here builds the
    // window (and its output_ control) up front instead, WITHOUT showing
    // it, purely so RunCfgFile() below has somewhere for its output to
    // go. Must come after plugins_->LoadAll() above, not before - a
    // plugin's homrec.register_command() calls need to have already run
    // for an autoexec.cfg line to be able to invoke one (e.g. a command
    // the Bter plugin registers).
    console_ = std::make_unique<ConsoleWindow>(state_, rec_.get(), GetHWND(), plugins_.get());
    console_->EnsureCreated(wxGetInstance());
    console_->RunCfgFile(L"autoexec");

    // ====== cfg/config.cfg (Phase 1+2 settings-storage migration, see
    // commands.md) ======
    // Runs after autoexec.cfg, and after the .hrc/JSON-migration settings
    // load earlier in this ctor - so a hand-written config.cfg can start
    // with "sethrc homrec.hrc 1" to seed itself from whatever the app
    // already loaded, then override just the handful of settings it
    // actually cares about on the lines that follow (see commands.md's
    // "sethrc" section for why that doesn't need any extra priority
    // syntax beyond normal top-to-bottom execution).
    console_->RunCfgFile(L"config");

    Bind(wxEVT_TIMER, &HomRecMainFrame::OnPreviewTimer, this, preview_timer_.GetId());
    Bind(wxEVT_TIMER, &HomRecMainFrame::OnStatsTimer, this, stats_timer_.GetId());
    Bind(wxEVT_TIMER, &HomRecMainFrame::OnLevelMeterTimer, this, level_meter_timer_.GetId());
    Bind(wxEVT_TIMER, &HomRecMainFrame::OnRestoreTopmostTimer, this, restore_topmost_timer_.GetId());
    Bind(wxEVT_TIMER, &HomRecMainFrame::OnCountdownTimer, this, countdown_timer_.GetId());
    Bind(wxEVT_TIMER, &HomRecMainFrame::OnScheduleTimer, this, schedule_timer_.GetId());
    preview_timer_.Start(1000 / 20);
    stats_timer_.Start(500);
    schedule_timer_.Start(1000);
    RestartLevelMeterTimer();

    // Upper bound extended to ID_FILE_IMPORT_HRP (1028) - it's the
    // newest menu ID and this Bind() is an inclusive ID *range*, so
    // adding an ID after ID_FILE_SET_PRESET without updating this bound
    // would silently leave its menu item's clicks unhandled.
    Bind(wxEVT_MENU, &HomRecMainFrame::OnMenu, this, ID_FILE_OPEN_RECORDINGS, ID_FILE_IMPORT_HRP);
    Bind(wxEVT_CLOSE_WINDOW, &HomRecMainFrame::OnClose, this);
    Bind(wxEVT_ICONIZE, &HomRecMainFrame::OnIconize, this);
    Bind(wxEVT_SHOW, &HomRecMainFrame::OnShowEvent, this);
    Bind(EVT_HOTKEY_START_STOP, &HomRecMainFrame::OnHotkeyEvent, this);
    Bind(EVT_HOTKEY_PAUSE, &HomRecMainFrame::OnHotkeyEvent, this);
    Bind(EVT_HOTKEY_FULLSCREEN, &HomRecMainFrame::OnHotkeyEvent, this);
    Bind(EVT_HOTKEY_SAVE_REPLAY, &HomRecMainFrame::OnHotkeyEvent, this);
    Bind(EVT_HOTKEY_CUSTOM, &HomRecMainFrame::OnHotkeyEvent, this);
    Bind(EVT_HOM_UPDATES_CHECKED, &HomRecMainFrame::OnHomUpdatesChecked, this);

    if (state_.first_launch) {
        ShowWelcomeDialog(GetHWND(), wxGetInstance(), state_);
    }
}

HomRecMainFrame::~HomRecMainFrame() {
    if (preview_panel_) preview_panel_->FlushOverlaySave(); // children + state_ still alive here
    if (state_.recording && rec_) rec_->Stop();
    // MUST run on every exit path, not just a clean menu-driven Exit -
    // SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) persists on a
    // window until explicitly cleared, independent of HomRec's own
    // lifetime, and affects every capture tool system-wide (not just
    // this app). Without this, closing HomRec (including via the X
    // button, Alt+F4, or a crash reaching this destructor via a
    // higher-level handler) could leave some other window permanently
    // invisible to screen-sharing/capture tools with no obvious reason
    // why, long after HomRec itself is gone. See the comment on
    // AppState::hidden_capture_windows for the full explanation.
    ClearAllHiddenCaptureWindows(state_);
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

void HomRecMainFrame::PersistSettings() {
    std::wstring target = HrcConfig::ResolveSettingsPath(state_);
    HrcConfig::Save(state_, target);
    if (target != HrcConfig::kDefaultSettingsPath) HrcConfig::Save(state_, HrcConfig::kDefaultSettingsPath);
}

void HomRecMainFrame::BuildMenuBar() {
    // Every label below now comes from lang_ instead of being a hardcoded
    // English literal - previously this was the single biggest reason a
    // dropped .hrl only ever retranslated the recording buttons/stats:
    // this function (and SettingsDialog) never consulted lang_ at all, so
    // no amount of language-file content could reach File/View/Settings/
    // Help. Accelerators (\tF11, \tCtrl+Shift+T) are appended here in code
    // rather than baked into the translated string, so a translator's .hrl
    // never has to know about them.
    //
    // Called again from ApplyLanguageText() whenever the language changes
    // (Settings > Language, or importing a .hrl), not just once at
    // startup - wxFrame::SetMenuBar() detaches but does not delete the
    // previous menu bar, so the old one is explicitly deleted first to
    // avoid leaking one wxMenuBar per language switch.
    if (wxMenuBar *old = GetMenuBar()) {
        SetMenuBar(nullptr);
        delete old;
    }

    auto *menuBar = new wxMenuBar();

    auto *fileMenu = new wxMenu();
    fileMenu->Append(ID_FILE_OPEN_RECORDINGS, wxString::FromUTF8(lang_.Get("open_recordings")));
    fileMenu->Append(ID_FILE_OPEN_PROGRAM_FILES, wxString::FromUTF8(lang_.Get("open_program_files")));
    fileMenu->Append(ID_FILE_SELECT_WINDOW, wxString::FromUTF8(lang_.Get("select_window")));
    fileMenu->Append(ID_FILE_SELECT_REGION, wxString::FromUTF8(lang_.Get("select_region")));
    fileMenu->Append(ID_FILE_HIDE_WINDOW, wxString::FromUTF8(lang_.Get("hide_window")));
    fileMenu->AppendSeparator();
    fileMenu->Append(ID_FILE_EXPORT_HRC, wxString::FromUTF8(lang_.Get("export_hrc")));
    fileMenu->Append(ID_FILE_IMPORT_HRC, wxString::FromUTF8(lang_.Get("import_hrc")));
    fileMenu->Append(ID_FILE_IMPORT_HRP, wxString::FromUTF8(lang_.Get("import_hrp")));
    fileMenu->Append(ID_FILE_SET_PRESET, wxString::FromUTF8(lang_.Get("set_preset")));
    fileMenu->AppendSeparator();
    fileMenu->Append(ID_FILE_EXIT, wxString::FromUTF8(lang_.Get("exit")));
    menuBar->Append(fileMenu, wxString::FromUTF8(lang_.Get("file_menu")));

    auto *viewMenu = new wxMenu();
    viewMenu->Append(ID_VIEW_ALWAYS_ON_TOP, wxString::FromUTF8(lang_.Get("always_on_top")));
    viewMenu->Append(ID_VIEW_FULLSCREEN, wxString::FromUTF8(lang_.Get("fullscreen")) + "\tF11");
    viewMenu->AppendCheckItem(ID_VIEW_OVERLAYS_PANEL, wxString::FromUTF8(lang_.Get("overlays_panel")));
    viewMenu->Check(ID_VIEW_OVERLAYS_PANEL, state_.show_overlays_panel);
    viewMenu->AppendCheckItem(ID_VIEW_AUDIO_PANEL, wxString::FromUTF8(lang_.Get("audio_mixer")));
    viewMenu->Check(ID_VIEW_AUDIO_PANEL, state_.show_audio_panel);
    viewMenu->AppendSeparator();
    viewMenu->Append(ID_VIEW_PC_ANALYTICS, wxString::FromUTF8(lang_.Get("pc_analytics")));
    viewMenu->Append(ID_VIEW_LOG, wxString::FromUTF8(lang_.Get("show_log")));
    menuBar->Append(viewMenu, wxString::FromUTF8(lang_.Get("view_menu")));

    auto *themeMenu = new wxMenu();
    themeMenu->Append(ID_THEME_DARK, wxString::FromUTF8(lang_.Get("dark")));
    themeMenu->Append(ID_THEME_LIGHT, wxString::FromUTF8(lang_.Get("light")));
    auto *settingsMenu = new wxMenu();
    settingsMenu->Append(ID_SETTINGS_OPEN, wxString::FromUTF8(lang_.Get("preferences")));
    // "Overlays..." (ID_OVERLAYS_MANAGE, the full editor window) removed --
    // the OverlaysDockPanel (View > Overlays Panel) is now the only way to
    // manage overlays; see overlays_dock_panel.h's header comment for why.
    settingsMenu->AppendSubMenu(themeMenu, wxString::FromUTF8(lang_.Get("theme")));
    menuBar->Append(settingsMenu, wxString::FromUTF8(lang_.Get("settings_menu")));

    auto *helpMenu = new wxMenu();
    helpMenu->Append(ID_HELP_CHECK_UPDATES, wxString::FromUTF8(lang_.Get("check_updates")));
    helpMenu->Append(ID_HELP_CONSOLE, wxString::FromUTF8(lang_.Get("console")) + "\tCtrl+Shift+T");
    helpMenu->Append(ID_HELP_WELCOME, wxString::FromUTF8(lang_.Get("show_welcome")));
    helpMenu->Append(ID_HELP_ABOUT, wxString::FromUTF8(lang_.Get("about")));
    menuBar->Append(helpMenu, wxString::FromUTF8(lang_.Get("help_menu")));

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

    start_color_btn_ = new ColorButton(left_panel_, ID_START_BTN, wxString::FromUTF8(lang_.Get("start")));
    start_color_btn_->SetFont(wxFont(wxFontInfo(11).FaceName("Segoe UI").Bold()));
    start_color_btn_->SetMinSize(wxSize(-1, 48));
    sizer->Add(start_color_btn_, 0, wxEXPAND | wxTOP, 25);
    Bind(wxEVT_BUTTON, &HomRecMainFrame::OnStartClicked, this, ID_START_BTN);

    pause_color_btn_ = new ColorButton(left_panel_, ID_PAUSE_BTN, wxString::FromUTF8(lang_.Get("pause")));
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

    section_status_lbl_ = addSection(wxString::FromUTF8(lang_.Get("status")));
    auto *statusRow = new wxBoxSizer(wxHORIZONTAL);
    status_dot_ = new StatusDot(left_panel_, FromColorref(theme_.text_secondary), 14);
    statusRow->Add(status_dot_, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
    status_lbl_ = new wxStaticText(left_panel_, wxID_ANY, wxString::FromUTF8(lang_.Get("ready")));
    status_lbl_->SetFont(BodyFont());
    statusRow->Add(status_lbl_, 1, wxALIGN_CENTRE_VERTICAL);
    sizer->Add(statusRow, 0, wxEXPAND | wxTOP, 8);

    section_time_lbl_ = addSection(wxString::FromUTF8(lang_.Get("time")));
    time_lbl_ = new wxStaticText(left_panel_, wxID_ANY, "00:00:00", wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    time_lbl_->SetFont(wxFont(wxFontInfo(24).FaceName("Consolas").Bold()));
    sizer->Add(time_lbl_, 0, wxEXPAND | wxTOP, 8);

    section_stats_lbl_ = addSection(wxString::FromUTF8(lang_.Get("stats")));
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
        // Same immediate persist as the ID_VIEW_AUDIO_PANEL menu toggle -
        // this is the panel's own [x] close button, i.e. exactly the path
        // that previously left show_audio_panel unsaved.
        PersistSettings();
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
    // "Position Overlays..." (row context menu, see overlays_dock_panel.h) -
    // OverlaysDockPanel has no access to RecordingController/theme_ itself,
    // so it just asks main_frame.cpp to open the window.
    overlays_panel_->on_apply_no_preview = [this](bool /*unused - see header*/) {
        if (!rec_raw_) return;
        ShowOverlayPlacementDialog(this, state_, rec_raw_, theme_);
    };
    overlays_panel_->on_overlay_added = [this]() { if (plugins_) plugins_->EmitHook("on_overlay_added"); };
    overlays_panel_->on_overlay_removed = [this]() { if (plugins_) plugins_->EmitHook("on_overlay_removed"); };
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

    // Empty (and therefore invisible - wxStaticText with no text takes no
    // visible space) until CheckHomUpdatesAsync() finds something to
    // report. Deliberately just a clickable label + a details popup, not
    // a plugin browser - hom itself already has search/show/install for
    // that; this is only meant to be "something told me updates exist".
    plugin_updates_lbl_ = new wxStaticText(bottom_bar_, wxID_ANY, wxEmptyString);
    plugin_updates_lbl_->SetFont(wxFont(wxFontInfo(9).FaceName("Segoe UI").Bold()));
    plugin_updates_lbl_->SetCursor(wxCursor(wxCURSOR_HAND));
    plugin_updates_lbl_->Bind(wxEVT_LEFT_DOWN, &HomRecMainFrame::OnPluginUpdatesClick, this);
    sizer->Add(plugin_updates_lbl_, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 10);

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
    // Previously this only retitled the window - every other piece of
    // chrome (section headers, Start/Pause buttons, preview title, bottom
    // bar) was set from lang_ exactly once at startup and never revisited,
    // so switching languages via Settings (or importing a .hrl) left all
    // of it stuck in whatever language was active when the window was
    // built. The only text that ever appeared to "follow" a language
    // switch was the FPS/resolution readout and the recording/paused word
    // in the bottom bar - and only while actively recording - because
    // those happen to re-read lang_.Get() on every OnStatsTimer tick, not
    // because anything here was actually updating them.
    std::string title = lang_.Get("app_title");
    if (title.empty()) title = std::string("HomRec v") + HR_APP_VERSION;
    SetTitle(wxString::FromUTF8(title));

    // File/View/Settings/Help never followed a language switch before -
    // BuildMenuBar() reads lang_ now, so rebuilding it here is what
    // actually retranslates the menu bar (see that function's own comment
    // for why the old menu bar is deleted rather than just replaced).
    BuildMenuBar();

    if (section_status_lbl_) section_status_lbl_->SetLabel(wxString::FromUTF8(lang_.Get("status")));
    if (section_time_lbl_) section_time_lbl_->SetLabel(wxString::FromUTF8(lang_.Get("time")));
    if (section_stats_lbl_) section_stats_lbl_->SetLabel(wxString::FromUTF8(lang_.Get("stats")));

    if (preview_title_lbl_) {
        preview_title_lbl_->SetLabel(wxString::FromUTF8("\u25CF ") + wxString::FromUTF8(lang_.Get("live_preview")));
    }
    if (made_by_lbl_) made_by_lbl_->SetLabel(wxString::FromUTF8(lang_.Get("made_by")));

    // Start/Pause button text and the status/file labels are state-
    // dependent (Ready/Recording/Paused, Start/Stop, Pause/Resume) - redo
    // whichever branch matches the current state_ instead of just
    // reapplying a single fixed string, so a language switch mid-recording
    // (or mid-pause) still lands on the right word for that state.
    if (state_.recording) {
        if (start_color_btn_) start_color_btn_->SetLabelText2(wxString::FromUTF8(lang_.Get("stop")));
        if (state_.paused) {
            if (pause_color_btn_) pause_color_btn_->SetLabelText2(wxString::FromUTF8(lang_.Get("resume")));
            SetStatusState(wxString::FromUTF8(lang_.Get("paused")), theme_.warning);
        } else {
            if (pause_color_btn_) pause_color_btn_->SetLabelText2(wxString::FromUTF8(lang_.Get("pause")));
            SetStatusState(wxString::FromUTF8(lang_.Get("recording")), theme_.success);
        }
        // file_lbl_ during an active recording is overwritten again on the
        // very next OnStatsTimer tick (500ms), so it isn't touched here -
        // doing so would just race that tick and occasionally show the
        // stale state_word for a moment.
    } else {
        if (start_color_btn_) start_color_btn_->SetLabelText2(wxString::FromUTF8(lang_.Get("start")));
        if (pause_color_btn_) pause_color_btn_->SetLabelText2(wxString::FromUTF8(lang_.Get("pause")));
        SetStatusState(wxString::FromUTF8(lang_.Get("ready")), theme_.text_secondary);
        if (file_lbl_) file_lbl_->SetLabel(wxString::FromUTF8(lang_.Get("ready")));
    }

    // Translated strings vary a lot in length ("STATISTICS" vs "STATS" vs
    // a CJK equivalent), so re-layout rather than leaving controls sized
    // for whatever the previous language's text happened to need.
    if (left_panel_) left_panel_->Layout();
    Layout();
}

void HomRecMainFrame::SetupTrayIcon() {
    tray_icon_ = new TrayIcon(this);
    // "#2" = icons/tray.ico (resource.rc) - a separate, smaller asset from
    // "#1"/main.ico used for the window/taskbar icon. Used to be the same
    // "#1" resource as the window icon, which is why the tray icon had
    // the same white outline the full-size app icon does - a 16x16 tray
    // render of an icon drawn for taskbar/Alt-Tab sizes just isn't the
    // same asset.
    tray_icon_->SetIcon(wxIcon("#2", wxBITMAP_TYPE_ICO_RESOURCE), "HomRec");
    tray_icon_->Bind(wxEVT_TASKBAR_LEFT_DCLICK, [this](wxTaskBarIconEvent &) {
        RestoreFromTray();
    });
    // wxTaskBarIcon::PopupMenu() (invoked from TrayIcon::CreatePopupMenu()'s
    // right-click menu) dispatches wxEVT_MENU to the tray icon object
    // itself, not the owning frame - bind here, not via the frame's
    // ID_FILE_OPEN_RECORDINGS..ID_VIEW_OVERLAYS_PANEL range Bind.
    tray_icon_->Bind(wxEVT_MENU, [this](wxCommandEvent &) { RestoreFromTray(); }, ID_TRAY_RESTORE);
    tray_icon_->Bind(wxEVT_MENU, [this](wxCommandEvent &) { Close(true); }, ID_TRAY_EXIT);
}

void HomRecMainFrame::RestoreFromTray() {
    if (IsIconized()) Iconize(false);
    Show(true);
    Raise();
    Layout();
}

void HomRecMainFrame::SetupHotkeys() {
    hotkey_handle_ = hr_hk_create();
    hr_hk_set_callbacks(hotkey_handle_, &HotkeyStartStopThunk, &HotkeyPauseThunk, &HotkeyFullscreenThunk, &HotkeySaveReplayThunk);
    hr_hk_set_custom_callback(hotkey_handle_, &HotkeyCustomThunk);
    ConfigureHotkeysFromState();
    if (!hr_hk_start(hotkey_handle_)) {
        wxLogDebug("HomRec: global hotkeys failed to register.");
    }
}

void HomRecMainFrame::ConfigureHotkeysFromState() {
    if (!hotkey_handle_) return;
    hr_hk_configure(hotkey_handle_, state_.hotkey_start_stop.c_str(),
                     state_.hotkey_pause.c_str(), state_.hotkey_fullscreen.c_str(),
                     state_.hotkey_save_replay.c_str());

    // Custom action hotkeys (Settings > Hotkeys > "Add Hotkey"). Slot i's
    // OS-level id is kCustomHotkeyBase+i regardless of whether its combo
    // actually parsed, so g_custom_hotkey_actions[id-base] always lines up
    // with state_.custom_hotkeys[i] - a bad combo just never fires, it
    // doesn't shift every binding after it.
    hr_hk_clear_custom(hotkey_handle_);
    g_custom_hotkey_actions.clear();
    g_custom_hotkey_actions.reserve(state_.custom_hotkeys.size());
    for (size_t i = 0; i < state_.custom_hotkeys.size(); ++i) {
        const std::string &action = state_.custom_hotkeys[i].first;
        std::string keys;
        for (char c : state_.custom_hotkeys[i].second)
            if (!std::isspace(static_cast<unsigned char>(c))) keys += c; // "Ctrl + B" -> "Ctrl+B"
        g_custom_hotkey_actions.push_back(action);
        hr_hk_add_custom(hotkey_handle_, kCustomHotkeyBase + static_cast<int>(i), keys.c_str());
    }
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
    wxString msg = wxString::Format(wxString::FromUTF8("Starting in %d\u2026"), countdown_remaining_);
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
    wxString msg = wxString::Format(wxString::FromUTF8("Starting in %d\u2026"), countdown_remaining_);
    SetStatusState(msg, theme_.warning);
    if (file_lbl_) file_lbl_->SetLabel(msg);
}
static bool ParseScheduledTime(const std::string &raw, int &hh, int &mm) {
    size_t i = 0, n = raw.size();
    while (i < n && isspace((unsigned char)raw[i])) ++i;
    size_t j = n;
    while (j > i && isspace((unsigned char)raw[j - 1])) --j;
    const size_t colon = raw.find(':', i);
    if (colon == std::string::npos || colon >= j) return false;
    const std::string h_str = raw.substr(i, colon - i);
    const std::string m_str = raw.substr(colon + 1, j - (colon + 1));
    if (h_str.empty() || h_str.size() > 2 || m_str.empty() || m_str.size() > 2) return false;
    if (!std::all_of(h_str.begin(), h_str.end(), ::isdigit)) return false;
    if (!std::all_of(m_str.begin(), m_str.end(), ::isdigit)) return false;
    hh = atoi(h_str.c_str());
    mm = atoi(m_str.c_str());
    return hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59;
}

void HomRecMainFrame::OnScheduleTimer(wxTimerEvent &) {
    if (!state_.scheduled_start_enabled || state_.scheduled_start_time.empty()) return;
    if (state_.recording) return;

    int target_hh = 0, target_mm = 0;
    if (!ParseScheduledTime(state_.scheduled_start_time, target_hh, target_mm)) {
        if (!schedule_parse_warned_) {
            schedule_parse_warned_ = true;
            HrLog::Warn("Recording: scheduled_start_time (\"" + state_.scheduled_start_time +
                        "\") isn't a valid HH:MM time - scheduled start will never fire until it's fixed.");
        }
        return;
    }
    schedule_parse_warned_ = false;

    time_t now = time(nullptr);
    struct tm *lt = localtime(&now);
    if (!lt) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", lt->tm_hour, lt->tm_min);
    std::string current_minute = buf;

    char target_buf[8];
    snprintf(target_buf, sizeof(target_buf), "%02d:%02d", target_hh, target_mm);

    // Only fire the instant the current minute first matches the target
    // (schedule_last_matched_minute_ != current_minute) - without this,
    // "12:00" would re-trigger RequestStart() every second for the
    // entire 12:00-12:01 minute. Once the minute rolls over past the
    // target, this naturally re-arms for the same time tomorrow, with no
    // date-tracking needed - a mismatch just means "not the trigger
    // instant right now".
    if (current_minute == target_buf && current_minute != schedule_last_matched_minute_) {
        schedule_last_matched_minute_ = current_minute;
        HrLog::Info("Recording: scheduled start time (" + current_minute + ") reached - starting.");
        RequestStart();
    }
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
    start_color_btn_->SetLabelText2(wxString::FromUTF8(lang_.Get("stop")));
    start_color_btn_->SetColours(FromColorref(theme_.error), FromColorref(theme_.bg));
    pause_color_btn_->Enable2(true);
    SetStatusState(wxString::FromUTF8(lang_.Get("recording")), theme_.success);
    if (plugins_) plugins_->EmitHook("on_recording_start");
    // cfg/startrec.cfg: same RunCfgFile() autoexec.cfg already uses at
    // startup, just re-triggered on every new recording instead of once
    // at launch. Runs after the plugin hook above so a startrec.cfg line
    // can rely on plugin-registered commands having already reacted to
    // on_recording_start if it needs to. Missing file = silent no-op,
    // same as autoexec.
    if (console_) console_->RunCfgFile(L"startrec");
}

void HomRecMainFrame::DoStop() {
    SetStatusState(wxString::FromUTF8("Saving\u2026"), theme_.warning);
    if (time_lbl_) time_lbl_->SetLabel("00:00:00");
    if (file_lbl_) file_lbl_->SetLabel(wxString::FromUTF8("Processing\u2026"));
    start_color_btn_->Enable2(false);
    pause_color_btn_->Enable2(false);
    Update();

    rec_->StopAsync([this]() {
        // Running on RecordingController's background finalize thread -
        // every line below touches wx widgets, so it all has to get back
        // to the UI thread first.
        CallAfter([this]() { OnRecordingFinalized(); });
    });
}

void HomRecMainFrame::HandleRecordingCrashed() {
    HrLog::Error("Recording: stopping after the ffmpeg process ended unexpectedly.");

    // Same UI-side sequence DoStop() runs - the status dot/time/file label
    // otherwise stay stuck on whatever they last showed (see rec_->
    // crashed()'s doc comment for why that used to happen forever, not
    // just until the next tick).
    SetStatusState(wxString::FromUTF8("Saving\u2026"), theme_.warning);
    if (time_lbl_) time_lbl_->SetLabel("00:00:00");
    if (file_lbl_) file_lbl_->SetLabel(wxString::FromUTF8("Processing\u2026"));
    start_color_btn_->Enable2(false);
    pause_color_btn_->Enable2(false);
    Update();

    recording_crashed_pending_notice_ = true;
    rec_->StopAsync([this]() {
        CallAfter([this]() { OnRecordingFinalized(); });
    });
}

void HomRecMainFrame::OnRecordingFinalized() {
    start_color_btn_->Enable2(true);
    start_color_btn_->SetLabelText2(wxString::FromUTF8(lang_.Get("start")));
    start_color_btn_->SetColours(FromColorref(theme_.success), FromColorref(theme_.bg));
    pause_color_btn_->SetLabelText2(wxString::FromUTF8(lang_.Get("pause")));
    pause_color_btn_->SetColours(FromColorref(theme_.warning), FromColorref(theme_.bg));
    pause_color_btn_->Enable2(false);
    SetStatusState(wxString::FromUTF8(lang_.Get("ready")), theme_.text_secondary);
    if (file_lbl_) file_lbl_->SetLabel(wxString::FromUTF8(lang_.Get("ready")));
    if (plugins_) plugins_->EmitHook("on_recording_stop");

    // Came from HandleRecordingCrashed() rather than a normal DoStop() -
    // let the user know *why* recording just stopped on its own instead
    // of silently landing back on Ready as if they'd clicked Stop
    // themselves. Shown before the normal "recording saved" summary below
    // (still worth showing - whatever was captured up to the crash was
    // still finalized and kept).
    if (recording_crashed_pending_notice_) {
        recording_crashed_pending_notice_ = false;
        wxMessageBox(
            wxString::FromUTF8(
                "Recording stopped unexpectedly \u2014 the encoder process ended on its "
                "own (it may have crashed, been closed externally, or hit a fatal "
                "error).\n\nWhatever was captured up to that point has been finalized "
                "and saved."),
            "HomRec - Recording Interrupted", wxOK | wxICON_WARNING, this);
    }

    // "Show summary" setting: the "recording saved, open folder?" popup.
    // Ported from custom_messagebox.h (already used elsewhere in this
    // file, e.g. ShowWelcomeDialog) - this was the one consumer that had
    // never actually been wired up (state_.show_summary loaded from
    // settings and shown as a checkbox, but nothing read it at Stop()
    // time). Skipped if the setting is off, if the user checked "Don't
    // show again" earlier, or if there's nowhere to open (empty
    // output_folder).
    if (state_.show_summary && !summary_dont_show_again_ && !state_.output_folder.empty()) {
        // This used to pass lang_.Get("recording_saved") as *both*
        // the title and the headline, and just the bare output folder as
        // the body - so the popup never actually said anything about the
        // recording that just finished (no filename, duration, resolution,
        // or size), which read as "no information about the entry". Build
        // a real summary instead, from the last-* snapshot RecordingController
        // takes in Stop() (the live accessors are already zeroed by now).
        std::wstring path = rec_->last_output_path();
        std::wstring filename = path;
        size_t slash = filename.find_last_of(L"\\/");
        if (slash != std::wstring::npos) filename = filename.substr(slash + 1);
        if (filename.empty()) filename = L"(unknown file)";

        wchar_t sizebuf[32];
        swprintf(sizebuf, 32, L"%.1f MB", rec_->last_output_size_mb());

        std::wstring info = filename + L"\r\n\r\n" +
            L"Duration: " + rec_->last_duration_formatted() + L"\r\n" +
            L"Resolution: " + std::to_wstring(rec_->output_width()) + L"x" +
                std::to_wstring(rec_->output_height()) + L"\r\n" +
            L"Size: " + sizebuf + L"\r\n" +
            L"Saved to: " + WideFromNarrow(state_.output_folder);

        bool dont_show = summary_dont_show_again_;
        bool open_folder = ShowCustomMessageBox(
            GetHWND(), wxGetInstance(), theme_,
            WideFromNarrow(lang_.Get("recording_saved")),
            filename,
            info, dont_show);
        summary_dont_show_again_ = dont_show;

        // Checking "Don't show again" only ever set the in-memory
        // summary_dont_show_again_ flag above, which suppresses the popup
        // for the rest of this run but is gone the moment the app is
        // relaunched - from the user's side that reads as the checkbox
        // "doing nothing". Make it actually stick: turn the real
        // show_summary setting off and persist that, same as unchecking
        // "Show summary" in Settings would.
        if (dont_show) {
            state_.show_summary = false;
            PersistSettings();
        }

        if (open_folder) OpenRecordingsFolder();
    }
}

void HomRecMainFrame::DoPause() {
    if (!state_.recording) return;
    rec_->TogglePause();
    if (state_.paused) {
        pause_color_btn_->SetLabelText2(wxString::FromUTF8(lang_.Get("resume")));
        pause_color_btn_->SetColours(FromColorref(theme_.success), FromColorref(theme_.bg));
        SetStatusState(wxString::FromUTF8(lang_.Get("paused")), theme_.warning);
    } else {
        pause_color_btn_->SetLabelText2(wxString::FromUTF8(lang_.Get("pause")));
        pause_color_btn_->SetColours(FromColorref(theme_.warning), FromColorref(theme_.bg));
        SetStatusState(wxString::FromUTF8(lang_.Get("recording")), theme_.success);
    }
}

void HomRecMainFrame::DoSaveReplay() {
    if (!rec_->instant_replay_active()) {
        SetStatusState(wxString::FromUTF8("Instant Replay isn't running right now"), theme_.warning);
        return;
    }
    std::wstring err, saved_path;
    if (!rec_->SaveReplay(err, &saved_path)) {
        wxMessageBox(wxString(err.c_str()), "HomRec", wxOK | wxICON_WARNING, this);
        return;
    }
    std::wstring filename = saved_path;
    size_t slash = filename.find_last_of(L"\\/");
    if (slash != std::wstring::npos) filename = filename.substr(slash + 1);
    if (file_lbl_) file_lbl_->SetLabel(wxString::FromUTF8("Replay saved: ") + wxString(filename.c_str()));
    if (plugins_) plugins_->EmitHook("on_replay_saved");
}

namespace {
std::wstring MainFrameExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full = path;
    size_t pos = full.find_last_of(L"\\/");
    return pos == std::wstring::npos ? full : full.substr(0, pos);
}

bool RunCapturedProcessForBadge(const std::wstring &cmdline, const std::wstring &cwd,
                                 DWORD timeout_ms, std::wstring *out_text) {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mut_cmd(cmdline.begin(), cmdline.end());
    mut_cmd.push_back(L'\0');
    if (!CreateProcessW(nullptr, mut_cmd.data(), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW, nullptr,
                         cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return false;
    }
    CloseHandle(hWrite);

    std::string raw;
    std::thread reader([&]() {
        char buf[4096];
        DWORD br = 0;
        while (ReadFile(hRead, buf, sizeof(buf) - 1, &br, nullptr) && br) {
            buf[br] = '\0';
            raw += buf;
        }
    });

    DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    reader.join();
    CloseHandle(hRead);

    int wl = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, nullptr, 0);
    std::wstring w(wl > 0 ? wl - 1 : 0, L'\0');
    if (wl > 1) MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, w.data(), wl);
    *out_text = w;
    return true;
}
} // namespace

// Runs `hom.exe list --upgradable` on a background thread (this shells
// out and hits the network - hom's search/show/list all fetch
// Hom/plugins/index.json live, see tools/hom/hom.cpp - so it must not
// block the UI thread) and posts EVT_HOM_UPDATES_CHECKED back with the
// result. Silently does nothing if hom.exe isn't sitting next to this
// exe (an unmodified/portable install without it is a normal, expected
// case, not an error worth surfacing).
void HomRecMainFrame::CheckHomUpdatesAsync() {
    std::wstring hom_path = MainFrameExeDir() + L"\\hom.exe";
    if (GetFileAttributesW(hom_path.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    std::thread([hom_path]() {
        std::wstring cmdline = L"\"" + hom_path + L"\" list --upgradable";
        std::wstring out;
        if (!RunCapturedProcessForBadge(cmdline, MainFrameExeDir(), 15000, &out)) return;

        // hom prints one "name: old -> new" line per upgradable plugin
        // (see tools/hom/hom.cpp's CmdList()) and nothing else useful to
        // this badge - everything else it might print ("hom: no
        // upgrades available.", the "N plugin(s) skipped..." note, a
        // connection error) starts with "hom:" and isn't a plugin line.
        int count = 0;
        wxString summary;
        std::wistringstream iss(out);
        std::wstring line;
        while (std::getline(iss, line)) {
            if (line.empty() || line.rfind(L"hom:", 0) == 0) continue;
            if (line.find(L" -> ") == std::wstring::npos) continue;
            ++count;
            if (!summary.empty()) summary += "\n";
            summary += wxString(line.c_str());
        }

        if (HomRecMainFrame *frame = g_frame.load()) {
            auto *evt = new wxThreadEvent(EVT_HOM_UPDATES_CHECKED);
            evt->SetInt(count);
            evt->SetString(summary);
            wxQueueEvent(frame, evt);
        }
    }).detach();
}

void HomRecMainFrame::OnHomUpdatesChecked(wxThreadEvent &evt) {
    int count = evt.GetInt();
    plugin_updates_summary_ = evt.GetString();
    if (!plugin_updates_lbl_) return;
    if (count <= 0) {
        plugin_updates_lbl_->SetLabel(wxEmptyString);
    } else {
        plugin_updates_lbl_->SetLabel(wxString::Format("\u2B06 %d plugin update%s", count, count == 1 ? "" : "s"));
        plugin_updates_lbl_->SetForegroundColour(FromColorref(theme_.success));
    }
    if (bottom_bar_) bottom_bar_->Layout();
}

void HomRecMainFrame::OnPluginUpdatesClick(wxMouseEvent & /*evt*/) {
    wxString body = plugin_updates_summary_.IsEmpty()
        ? wxString("No plugin updates found.")
        : ("Updates available:\n\n" + plugin_updates_summary_ +
           "\n\nRun 'hom upgrade' in the console (or 'inwid hom upgrade' - "
           "upgrade itself doesn't need the prefix, see commands.md) to install them.");
    wxMessageBox(body, "Plugin Updates", wxOK | wxICON_INFORMATION, this);
}

void HomRecMainFrame::ToggleFullscreenNative() {
    fullscreen_ = !fullscreen_;
    ShowFullScreen(fullscreen_, wxFULLSCREEN_NOBORDER | wxFULLSCREEN_NOCAPTION);
}

void HomRecMainFrame::OpenProgramFilesFolder() {
    // Opens the folder hr.exe itself lives in - the installed/portable
    // program's own files (ffmpeg/, plugins/, icons/, logs/, homrec.hrc,
    // etc.) - as distinct from ID_FILE_OPEN_RECORDINGS above, which opens
    // wherever the user has configured recordings to be *saved*
    // (Settings > General > Output folder), frequently a different drive
    // or folder entirely from where the program is installed. Useful for
    // reaching plugins/, dropping in a .hrp by hand, or grabbing a log
    // file to attach to a bug report without hunting down the install
    // path first.
    std::wstring dir = MainFrameExeDir();
    if (dir.empty()) return;

    // Same foreground/topmost handling as OpenRecordingsFolder() above -
    // see its own comment for why both steps are needed.
    AllowSetForegroundWindow(ASFW_ANY);
    bool was_topmost = (GetWindowStyleFlag() & wxSTAY_ON_TOP) != 0;
    if (was_topmost) {
        SetWindowStyleFlag(GetWindowStyleFlag() & ~wxSTAY_ON_TOP);
        pending_restore_topmost_ = true;
        restore_topmost_timer_.StartOnce(1500);
    }

    ShellExecuteW(GetHWND(), L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void HomRecMainFrame::ImportHrpPlugin() {
    // The README's documented install path for a packaged plugin
    // ("download it and drop it into plugins/ as-is, then restart
    // HomRec") requires manually finding the plugins folder and
    // relaunching the app. This does the same copy from an ordinary file
    // picker and loads the plugin immediately via
    // plugins_->LoadPluginArchive() (lua_engine.cpp - already used
    // internally by LoadAll() for every *.hrp sitting in plugins/, see
    // its own header comment) instead.
    wxFileDialog dlg(this, "Import Plugin (.hrp)", wxEmptyString, wxEmptyString,
                      "HomRec Plugin (*.hrp)|*.hrp|All files (*.*)|*.*",
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;

    std::string srcPath = dlg.GetPath().ToUTF8().data();

    // Same "plugins" relative directory LuaPluginEngine was constructed
    // with (plugins_ = std::make_unique<LuaPluginEngine>("plugins"),
    // below) - its constructor already CreateDirectoryA()s it, so by the
    // time this can run (plugins_ exists) it's already there.
    std::string base = srcPath;
    size_t slash = base.find_last_of("\\/");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    if (base.empty()) base = "plugin";
    // LoadPluginArchive() derives the plugin's extracted-folder name from
    // the filename up to the last '.', so force the copy to actually end
    // in ".hrp" regardless of what the source file was called - picking
    // a renamed "cool-plugin.zip" via the "All files" filter above should
    // still land as plugins\cool-plugin.hrp, not
    // plugins\cool-plugin.zip.hrp.
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    std::string destPath = "plugins\\" + base + ".hrp";

    if (GetFileAttributesA(destPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        wxString msg = wxString::Format(
            "A plugin named \"%s\" is already installed. Overwrite it?",
            wxString::FromUTF8(base.c_str()));
        if (wxMessageBox(msg, "Import Plugin", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
            return;
        }
    }

    if (!CopyFileA(srcPath.c_str(), destPath.c_str(), FALSE)) {
        HrLog::Error("Failed to import plugin from " + srcPath);
        wxMessageBox("Couldn't copy that file into the plugins folder.",
                     "Import Plugin", wxOK | wxICON_ERROR, this);
        return;
    }

    bool loaded = plugins_ && plugins_->LoadPluginArchive(destPath);
    HrLog::Info("Imported plugin from " + srcPath + " -> " + destPath);
    if (loaded) {
        wxMessageBox("Plugin installed and loaded.", "Import Plugin",
                     wxOK | wxICON_INFORMATION, this);
    } else {
        // Copied fine, but LoadPluginArchive() itself failed (bad
        // plugin.json, missing entry script, on_load() threw, etc. - see
        // plugins.log for specifics) or plugins_ isn't up yet. The file
        // is still sitting in plugins/ either way, so the *next* full
        // restart's plugins_->LoadAll() will try it again from scratch,
        // same as if it had been dropped in by hand per the README.
        wxMessageBox("Plugin copied into the plugins folder, but couldn't be "
                      "loaded right now - check the Console/plugins.log for "
                      "details. It will be tried again the next time HomRec starts.",
                     "Import Plugin", wxOK | wxICON_WARNING, this);
    }
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

// Called right after the window/region pickers return. Both pickers only
// write into state_ - without this the live preview kept showing (and
// re-using, for Start()) a pipeline built for the OLD target until some
// unrelated Settings change happened to rebuild it, so it looked like the
// pick had been ignored and the full screen was still being captured.
// Skipped while recording: the running pipeline keeps its crop until Stop(),
// and RefreshPreviewSettings()'s change-detection snapshot deliberately
// stays stale so the next call after Stop() rebuilds for the new target.
void HomRecMainFrame::OnCaptureTargetChanged() {
    if (!state_.recording && rec_raw_) rec_raw_->RefreshPreviewSettings();
    PersistSettings();
}

void HomRecMainFrame::OnMenu(wxCommandEvent &evt) {
    switch (evt.GetId()) {
        case ID_FILE_EXIT: Close(true); break;
        case ID_FILE_OPEN_RECORDINGS:
            OpenRecordingsFolder();
            break;
        case ID_FILE_OPEN_PROGRAM_FILES:
            OpenProgramFilesFolder();
            break;
        case ID_FILE_SELECT_WINDOW:
            ShowWindowPickerDialog(GetHWND(), wxGetInstance(), state_);
            OnCaptureTargetChanged();
            break;
        case ID_FILE_SELECT_REGION:
            ShowRegionPickerOverlay(GetHWND(), wxGetInstance(), state_);
            OnCaptureTargetChanged();
            break;
        case ID_FILE_HIDE_WINDOW:
            ShowHideWindowDialog(GetHWND(), wxGetInstance(), state_);
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
        case ID_FILE_SET_PRESET: {
            if (ShowPresetDialog(this, state_, theme_, lang_)) {
                ApplyThemeColours();
                ApplyLanguageText();
                if (hotkey_handle_) { hr_hk_stop(hotkey_handle_); hr_hk_destroy(hotkey_handle_); hotkey_handle_ = nullptr; }
                SetupHotkeys();
                RestartLevelMeterTimer();
                if (rec_raw_) rec_raw_->RefreshPreviewSettings();
            }
            break;
        }
        case ID_FILE_IMPORT_HRP:
            ImportHrpPlugin();
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
            // Persisted immediately (matching the ID_THEME_DARK/LIGHT
            // handlers below) rather than waiting for OnClose(), since
            // minimize-to-tray vetoes the close event entirely - that was
            // exactly why toggling a panel closed never stuck across a
            // restart before.
            PersistSettings();
            break;
        case ID_VIEW_AUDIO_PANEL:
            state_.show_audio_panel = !state_.show_audio_panel;
            if (audio_panel_) audio_panel_->Show(state_.show_audio_panel);
            if (auto *mb = GetMenuBar()) mb->Check(ID_VIEW_AUDIO_PANEL, state_.show_audio_panel);
            Layout();
            PersistSettings();
            break;
        case ID_VIEW_PC_ANALYTICS: ShowPcAnalyticsDialog(GetHWND(), wxGetInstance(), state_.output_folder); break;
        case ID_VIEW_LOG: ShowLogViewerDialog(GetHWND(), wxGetInstance()); break;
        case ID_THEME_DARK:
            state_.current_theme = "dark"; theme_ = GetBuiltinTheme("dark"); ApplyThemeColours();
            PersistSettings();
            break;
        case ID_THEME_LIGHT:
            state_.current_theme = "light"; theme_ = GetBuiltinTheme("light"); ApplyThemeColours();
            PersistSettings();
            break;
        case ID_SETTINGS_OPEN:
            if (ShowSettingsDialog(this, state_, theme_, lang_, rec_raw_) && rec_raw_) {
                // Settings dialog's General tab can now change
                // state_.current_language (including to a language just
                // imported via "Add Language...") - reload it here so
                // Save takes effect immediately instead of only after
                // the next restart, same as the theme handlers below do
                // for state_.current_theme.
                lang_ = LanguageTable::Load(state_.current_language, "Assets\\L");
                ApplyLanguageText();

                rec_raw_->RefreshPreviewSettings();
                // Live preview is authoritative again the moment it's back
                // on - drop the "Apply with preview off" screenshot (if any
                // editing session was active) and let CaptureSnapshotFrame's
                // temporary pipeline go back to being torn down normally
                // when Disable live preview is still checked.
                if (preview_panel_ && preview_panel_->InSnapshotMode() && !state_.disable_preview) {
                    preview_panel_->ExitSnapshotMode();
                }
                if (rec_raw_) rec_raw_->EndSnapshotEditing();
            }
            // Hotkeys live on a tab of this same dialog - re-apply them in
            // case they changed, same as the old (now-removed) "Advanced
            // Settings" entry used to do.
            if (hotkey_handle_) { hr_hk_stop(hotkey_handle_); hr_hk_destroy(hotkey_handle_); hotkey_handle_ = nullptr; }
            SetupHotkeys();
            // Level meter refresh rate (Settings > Audio) also lives on
            // this dialog - resync the timer so a change takes effect
            // immediately instead of only after a restart. Harmless
            // no-op restart if the dialog was cancelled or the rate
            // wasn't touched, same reasoning as SetupHotkeys() above.
            RestartLevelMeterTimer();
            // Instant Replay's on/off checkbox and buffer-size spinner also
            // live on this dialog - apply whichever way it changed. Calling
            // both Enable/Disable unconditionally is harmless either way:
            // each is a no-op if already in the requested state (see their
            // header comments in recording_controller.h).
            if (rec_) {
                if (state_.instant_replay_enabled) {
                    std::wstring err;
                    if (!rec_->EnableInstantReplay(err)) {
                        wxMessageBox(wxString(err.c_str()), "HomRec", wxOK | wxICON_WARNING, this);
                    }
                } else {
                    rec_->DisableInstantReplay();
                }
            }
            break;
        // ID_OVERLAYS_MANAGE removed along with overlay_manager.cpp's
        // ShowOverlayManager() -- see overlays_dock_panel.h.
        case ID_HELP_CHECK_UPDATES:
            OnCheckForUpdates();
            break;
        case ID_HELP_CONSOLE:
            // console_ is always non-null by this point now (constructed
            // at startup so cfg/autoexec.cfg has somewhere to print to -
            // see plugins_->LoadAll() above) - this null-check is just
            // defensive leftover from when construction was fully lazy.
            if (!console_) console_ = std::make_unique<ConsoleWindow>(state_, rec_.get(), GetHWND(), plugins_.get());
            console_->Show(wxGetInstance());
            break;
        case ID_HELP_WELCOME: ShowWelcomeDialog(GetHWND(), wxGetInstance(), state_); break;
        case ID_HELP_ABOUT:
            wxMessageBox("HomRec " HR_APP_VERSION, "About", wxOK, this);
            break;
        default: break;
    }
}

// Help > Check for Updates. Hits GitHub on a background thread (see
// hr_update.cpp) so the UI never blocks on the network call, then hops
// back to the UI thread via CallAfter before touching any widgets or
// showing a message box.
void HomRecMainFrame::OnCheckForUpdates() {
    HrUpdate::CheckForUpdateAsync([this](HrUpdate::UpdateInfo info) {
        CallAfter([this, info]() {
            if (!info.checked_ok) {
                wxMessageBox("Couldn't check for updates - check your internet connection and try again.",
                             "Check for Updates", wxOK | wxICON_WARNING, this);
                return;
            }
            if (!info.available) {
                wxMessageBox("You're on the latest version (" HR_APP_VERSION ").",
                             "Check for Updates", wxOK | wxICON_INFORMATION, this);
                return;
            }

            wxString msg = wxString::Format(
                "A new version is available: %s (you have " HR_APP_VERSION ").\n\n",
                wxString::FromUTF8(info.latest_version));
            if (!info.download_url.empty()) {
                msg += "Download and install it now? HomRec will close to finish updating.";
                int choice = wxMessageBox(msg, "Update Available", wxYES_NO | wxICON_INFORMATION, this);
                if (choice != wxYES) return;
                if (HrUpdate::DownloadAndLaunchInstaller(info)) {
                    Close(true);
                } else {
                    wxMessageBox("Couldn't download the update. Try again later, or grab it from the "
                                 "release page.", "Update Available", wxOK | wxICON_ERROR, this);
                }
            } else {
                msg += "No installer was found in that release - open its page in your browser?";
                int choice = wxMessageBox(msg, "Update Available", wxYES_NO | wxICON_INFORMATION, this);
                if (choice == wxYES && !info.html_url.empty()) {
                    std::wstring urlW = wxString::FromUTF8(info.html_url).ToStdWstring();
                    ShellExecuteW(nullptr, L"open", urlW.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
        });
    });
}

void HomRecMainFrame::OnPreviewTimer(wxTimerEvent &) {
    if (rec_) rec_->SyncOverlays();
    // Once EnsurePreview() has failed enough times in a row (DXGI
    // dx_create() stuck - RDP, a virtual display, a display mode that
    // just changed), swap the generic "Preview loading..." placeholder
    // for something that actually explains it instead of looking like a
    // permanent freeze. Reverts the instant capture recovers (monitor
    // reconnected, RDP session promoted to a real session, etc.).
    if (preview_panel_) {
        static const wxString kUnavailableText =
            wxString::FromUTF8("Screen capture unavailable \u2014 check you're not on RDP/a "
                                "virtual display, and that the selected monitor is connected.");
        static const wxString kLoadingText("Preview loading...");
        bool unavailable = rec_ && rec_->preview_capture_unavailable();
        preview_panel_->SetPlaceholderText(unavailable ? kUnavailableText : kLoadingText);
        if (!IsIconized()) preview_panel_->PollRefresh(); // repaints only on a new frame
    }
}

void HomRecMainFrame::OnStatsTimer(wxTimerEvent &) {
    if (rec_) rec_->PollStats();

    // See rec_->crashed()'s doc comment - PollStats() just flagged this,
    // it doesn't stop anything itself (that touches wx UI state, which
    // belongs here, not in RecordingController). Bail out of the rest of
    // this tick immediately - state_.recording is still true until
    // HandleRecordingCrashed()'s StopAsync() finishes, but there's nothing
    // useful left to poll from the now-dead ffmpeg process.
    if (rec_ && rec_->crashed()) {
        rec_->AcknowledgeCrash();
        HandleRecordingCrashed();
        return;
    }

    // logs\pc.log - throttles itself internally to ~once every 10s, so
    // piggybacking on this existing 500ms tick (rather than adding a
    // dedicated timer just for this) doesn't mean 500ms-resolution writes.
    HrPcLog::MaybeLogSnapshot(state_.recording, rec_ ? rec_->current_fps() : 0.0,
                               // resolved_hw_encoder() is only filled when hw_accel == "auto";
                               // an explicitly chosen h264_qsv/nvenc/amf codec is hardware too.
                               rec_ && (!rec_->resolved_hw_encoder().empty() ||
                                        state_.video_codec.find("nvenc") != std::string::npos ||
                                        state_.video_codec.find("qsv")   != std::string::npos ||
                                        state_.video_codec.find("amf")   != std::string::npos),
                               state_.output_folder);

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
        // Overload warning: rec_->overloaded() only turns on after a
        // sustained streak of real frame drops (see PollStats()), so this
        // isn't fighting the normal "recording"/"paused" status text for
        // attention on every tick - just while the pipeline is actually
        // behind.
        if (rec_ && rec_->overloaded()) {
            SetStatusState(wxString::FromUTF8("\u26A0 System overloaded \u2014 dropping frames"),
                            theme_.warning);
        }
    }
    // Labels only change while recording - relayouting the panel every
    // 500ms while idle was wasted work.
    if (state_.recording) left_panel_->Layout();
}

void HomRecMainFrame::OnLevelMeterTimer(wxTimerEvent &) {
    if (audio_panel_) audio_panel_->PollLevels();
}

void HomRecMainFrame::RestartLevelMeterTimer() {
    int fps = state_.level_meter_fps;
    if (fps < 10) fps = 10;
    if (fps > 60) fps = 60;
    level_meter_timer_.Start(1000 / fps);
}

void HomRecMainFrame::OnClose(wxCloseEvent &evt) {
    if (IsBeingDeleted()) { evt.Skip(); return; }

    if (state_.minimize_to_tray && tray_icon_ && evt.CanVeto()) {
        Show(false);
        evt.Veto();
        return;
    }
    // Clear g_frame here, on the real-close path, rather than waiting for
    // ~HomRecMainFrame() - Destroy() below only *schedules* the actual C++
    // destruction, which can happen several event-loop turns later. Doing
    // it now closes (most of) the window where CheckHomUpdatesAsync()'s
    // background thread could still wxQueueEvent() onto a frame that's
    // about to be freed. See the comment on g_frame's declaration.
    if (g_frame == this) g_frame = nullptr;
    // BUGFIX (app froze ~3-6s on close while Instant Replay was on - log:
    // "HomRec closing" then, 3s later, "Instant Replay: segment writer didn't
    // finish gracefully in time - killing it"): TeardownPreview() below
    // detaches pipeline_ (sets it to nullptr) even though Instant Replay's
    // segment writer is still piping into it, so ~RecordingController()'s
    // later StopInstantReplayEncoder() had no pipeline to close the pipe on,
    // ffmpeg never saw EOF, and both 3s waits ran out before it got killed.
    // Stop Instant Replay properly first, while pipeline_ still exists.
    if (rec_ && rec_->instant_replay_enabled()) rec_->DisableInstantReplay();
    if (rec_) rec_->TeardownPreview();
    if (hotkey_handle_) { hr_hk_stop(hotkey_handle_); hr_hk_destroy(hotkey_handle_); hotkey_handle_ = nullptr; }
    HrLog::Info("HomRec closing");
    Destroy();
}

void HomRecMainFrame::OnIconize(wxIconizeEvent &evt) {
    if (evt.IsIconized() && state_.minimize_to_tray) Show(false);
    // Plain minimize (not minimize-to-tray) doesn't Hide() the window, so
    // it never reaches OnShowEvent below - the window is still
    // "shown" as far as wx is concerned, just iconized in the taskbar.
    // Gate the preview pipeline on that too: nothing is visible to show
    // the live thumbnail to either way.
    if (rec_) rec_->SetPreviewVisible(!evt.IsIconized());
    evt.Skip();
}

void HomRecMainFrame::OnShowEvent(wxShowEvent &evt) {
    // Pauses/resumes the preview-only capture pipeline based on whether
    // the window is actually visible - see SetPreviewVisible()'s comment
    // in recording_controller.h for why (this is the idle-CPU fix: the
    // pipeline used to run unconditionally the whole time the app was
    // open, minimized-to-tray or not). Covers minimize-to-tray, both
    // tray-restore paths (double-click and the menu item), and a plain
    // taskbar minimize/restore alike, since they all route through
    // Show()/Hide() one way or another.
    if (rec_) rec_->SetPreviewVisible(evt.IsShown());
    evt.Skip();
}

void HomRecMainFrame::OnHotkeyEvent(wxThreadEvent &evt) {
    wxEventType t = evt.GetEventType();
    if (t == EVT_HOTKEY_START_STOP) { if (state_.recording) DoStop(); else RequestStart(); }
    else if (t == EVT_HOTKEY_PAUSE) { DoPause(); }
    else if (t == EVT_HOTKEY_FULLSCREEN) { ToggleFullscreenNative(); }
    else if (t == EVT_HOTKEY_SAVE_REPLAY) { DoSaveReplay(); }
    else if (t == EVT_HOTKEY_CUSTOM) {
        int idx = evt.GetInt() - kCustomHotkeyBase;
        if (idx >= 0 && idx < static_cast<int>(g_custom_hotkey_actions.size()) && console_) {
            // Run the exact same line a user could type into the console or
            // put in autoexec.cfg - built-in commands, "setting = value"
            // assignments (e.g. "disable_preview = true"), and anything a
            // plugin registered via homrec.register_command/register_setting
            // all go through this one parser, so a hotkey is just another
            // way of "typing" the action. `true` = pre-confirmed, since
            // there's no console window open for a hotkey to prompt in.
            console_->RunSingleCommand(WideFromNarrow(g_custom_hotkey_actions[idx]), true);
        }
    }
}