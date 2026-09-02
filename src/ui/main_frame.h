// main_frame.h - wxWidgets rewrite of the main shell.
//
// Supersedes main_window.h/.cpp. The raw-GDI version (see git history /
// main_window.cpp.bak if kept) rendered its own text and colors by hand
// with CreateFontW(-heightInPixels, ...), which does not participate in
// Windows DPI scaling the way real widgets do - that's what produced the
// "tiny fonts" complaint. wxWidgets gives real retained-mode widgets
// (wxStaticText/wxButton/wxPanel - conceptually the same model as
// Tkinter's Label/Button/Frame), proper DPI-aware font point sizes, and a
// wxImage/wxBitmap pipeline for the live preview instead of a hand-rolled
// StretchDIBits call, which is also what was silently failing to show any
// preview at all.
//
// Scope of this pass: the shell (menu, left sidebar, preview, bottom bar),
// AudioPanel (mic/desktop mixer strip), and the Settings dialog are now
// wx. RecordingController/OverlaysDockPanel/the remaining dialogs
// (Advanced Settings, Welcome, Console, Overlay Manager, Log Viewer, PC
// Analytics, Window Picker) are still untouched raw-Win32 code - a wxFrame
// is still a real HWND under the hood on Windows (GetHandle()), so they
// mount onto it/get launched with it as HWND parent exactly like before.
// Porting each of
// those to wx widgets too is follow-up work, not done here.
#pragma once

#include <wx/wx.h>
#include <wx/taskbar.h>
#include <memory>
#include <vector>
#include <cstdint>
#include "app_state.h"
#include "theme.h"
#include "themed_widgets.h"
#include "language.h"
#include "recording_controller.h"
#include "audio_panel.h"
#include "console_window.h"
#include "overlays_dock_panel.h"
#include "../plugins/lua_engine.h"

// Menu/control IDs - kept identical to the old main_window.h enum so every
// ShowXDialog()/OnCommand-style callsite elsewhere didn't need renumbering.
enum MenuCommandId {
    ID_FILE_OPEN_RECORDINGS = 1001,
    ID_FILE_EXIT            = 1002,
    ID_VIEW_ALWAYS_ON_TOP   = 1003,
    ID_VIEW_FULLSCREEN      = 1004,
    ID_THEME_DARK           = 1005,
    ID_THEME_LIGHT          = 1006,
    ID_HELP_ABOUT           = 1007,
    ID_HELP_CHECK_UPDATES   = 1008,
    ID_SETTINGS_OPEN        = 1009,
    ID_SETTINGS_ADVANCED    = 1010,
    ID_OVERLAYS_MANAGE      = 1011,
    ID_HELP_CONSOLE         = 1012,
    ID_HELP_WELCOME         = 1013,
    ID_TRAY_RESTORE         = 1014,
    ID_TRAY_EXIT            = 1015,
    ID_START_BTN            = 1016,
    ID_PAUSE_BTN            = 1017,
    ID_VIEW_PC_ANALYTICS    = 1018,
    ID_VIEW_LOG             = 1019,
    ID_FILE_SELECT_WINDOW   = 1020,
    ID_VIEW_OVERLAYS_PANEL  = 1021,
    ID_FILE_EXPORT_HRC      = 1022,
    ID_FILE_IMPORT_HRC      = 1023,
    ID_VIEW_AUDIO_PANEL     = 1024,
    ID_FILE_HIDE_WINDOW     = 1025,
};

// ColorButton and StatusDot moved to themed_widgets.h/.cpp so audio_panel
// and settings_dialog (also now wx-based) can share them instead of
// duplicating - see that header for their docs.

// Draws the live capture preview from RecordingController::GetPreviewFrame's
// raw RGB24 buffer (converted to a wxImage/wxBitmap and scaled to fit) -
// this is the actual fix for "no preview": the old code's StretchDIBits
// call was structurally fine but nothing was verified to reach it; wx's
// wxImage path is simpler to get right and easier to debug.
class PreviewPanel : public wxPanel {
public:
    PreviewPanel(wxWindow *parent, RecordingController *&rec, AppState &state);
    void SetPlaceholderText(const wxString &text) { placeholder_ = text; Refresh(); }

    // "Apply with preview off" (overlays_dock_panel.cpp's row context
    // menu, wired up in main_frame.cpp) - lets overlays still be dragged/
    // resized here even with Settings > Disable live preview on, using a
    // one-off screenshot instead of the live feed. While active, every
    // overlay is drawn/hit-testable regardless of OverlayDef::visible (the
    // user needs to be able to reach a hidden one to reposition it too),
    // not just the ones normally shown live.
    void EnterSnapshotMode(const std::vector<uint8_t> &buf, int w, int h);
    void UpdateSnapshotFrame(const std::vector<uint8_t> &buf, int w, int h);
    void ExitSnapshotMode();
    bool InSnapshotMode() const { return snapshot_mode_; }

private:
    void OnPaint(wxPaintEvent &evt);
    void OnLeftDown(wxMouseEvent &evt);
    void OnMouseMove(wxMouseEvent &evt);
    void OnLeftUp(wxMouseEvent &evt);
    void OnCaptureLost(wxMouseCaptureLostEvent &evt);

    // Maps the current preview bitmap's on-screen rect within the panel
    // (position + scale), so overlay coordinates (always stored in real
    // source/capture pixels, matching what actually gets recorded) can be
    // translated to/from the panel's on-screen pixels for hit-testing,
    // dragging, and drawing.
    bool GetPreviewRect(wxRect &out) const;

    RecordingController *&rec_;
    AppState &state_;
    // Shown only for the brief moment before the very first preview frame
    // has arrived (EnsurePreview()'s pipeline starts at app launch and
    // keeps running the whole session now - see RecordingController -
    // so this is no longer "you must be recording to see anything", just
    // "still warming up").
    wxString placeholder_ = "Preview loading...";
    std::vector<uint8_t> frame_buf_;

    // -- "Apply with preview off" snapshot mode (see EnterSnapshotMode()) --
    bool snapshot_mode_ = false;
    std::vector<uint8_t> snapshot_buf_;
    int snapshot_w_ = 0, snapshot_h_ = 0;

    // -- direct overlay drag/resize on the preview -------------------------
    // Overlays previously could only be repositioned via a
    // separate full-screen "Position Overlays" window opened from the
    // Overlay Manager dialog. Users expect to just grab the overlay on the
    // live preview shown in the main window instead -- this makes that the
    // primary way to move/resize them.
    int drag_overlay_index_ = -1;
    // Which corner (if any) is being dragged to resize. kNone means a
    // plain move-drag (grabbed the body, not a handle).
    enum class Corner { kNone, kTopLeft, kBottomRight };
    Corner drag_corner_ = Corner::kNone;
    int drag_start_mouse_x_ = 0, drag_start_mouse_y_ = 0;
    int drag_start_ov_x_ = 0, drag_start_ov_y_ = 0, drag_start_ov_w_ = 0, drag_start_ov_h_ = 0;

    // The underlying preview frame updates far less often than the 30fps
    // paint timer used to assume (throttled in hr_pipeline.cpp, more so
    // when idle) - re-running the bilinear Scale() on an unchanged frame
    // every single paint was pure wasted CPU, most annoyingly right when
    // a recording is also competing for the same cores. Cache the last
    // scaled bitmap and only redo the scale when the source frame (or the
    // panel's size) actually changed.
    std::vector<uint8_t> last_frame_buf_;
    wxBitmap cached_bmp_;
    int cached_src_w_ = -1, cached_src_h_ = -1;
    int cached_dst_w_ = -1, cached_dst_h_ = -1;
    int cached_panel_w_ = -1, cached_panel_h_ = -1;
};

class HomRecMainFrame : public wxFrame {
public:
    HomRecMainFrame();
    ~HomRecMainFrame() override;

    HWND GetHWND() const { return (HWND)GetHandle(); }

private:
    void BuildMenuBar();
    void BuildLeftPanel(wxWindow *parent, wxSizer *parentSizer);
    void BuildPreviewPanel(wxWindow *parent, wxSizer *parentSizer);
    void BuildBottomBar(wxWindow *parent, wxSizer *parentSizer);
    void ApplyThemeColours();
    void ApplyLanguageText();
    // Phase 1 settings-storage migration (see commands.md): persists the
    // *entire* current state_ via HrcConfig::Save(), mirroring to the
    // default location too if a custom settings path is configured (see
    // hrc_config.h's ResolveSettingsPath()). Used by every menu-toggle /
    // panel-close handler that used to hand-roll its own
    // hr_settings_set_flag()+hr_settings_save() pair against the old,
    // no-longer-authoritative homrec_settings.json.
    void PersistSettings();

    void SetupHotkeys();
    void ConfigureHotkeysFromState();
    void SetupTrayIcon();

    void DoStart();
    void DoStop();
    void DoPause();
    // Wraps DoStart() with the "Countdown (3s)" setting: if
    // state_.countdown_enabled is on, shows a 3-2-1 countdown in the
    // status label (cancellable by clicking Start again) before actually
    // calling DoStart(); otherwise starts immediately, same as before.
    // Both places that used to call DoStart() directly for a fresh start
    // (the Start button and the global hotkey) now go through this.
    void RequestStart();
    void OnCountdownTimer(wxTimerEvent &evt);
    wxTimer countdown_timer_;
    int countdown_remaining_ = 0;
    // Session-only (not persisted to settings.json) - resets each launch,
    // same lifetime as the Python original's in-memory dont_show_again
    // this ports from. state_.show_summary (the actual settings toggle)
    // is what's persisted.
    bool summary_dont_show_again_ = false;
    void SetStatusState(const wxString &text, COLORREF dotColor);
    void ToggleFullscreenNative();
    // Un-minimizes + un-hides the window from the tray, in that order -
    // see SetupTrayIcon()'s comment for why both steps are needed.
    void RestoreFromTray();
    // Opens state_.output_folder in Explorer and makes sure it actually
    // ends up in front of HomRec instead of silently behind it - see the
    // comment at its definition for why that isn't automatic.
    void OpenRecordingsFolder();
    void OnRestoreTopmostTimer(wxTimerEvent &evt);
    bool pending_restore_topmost_ = false;

    // wx event handlers
    void OnStartClicked(wxCommandEvent &evt);
    void OnPauseClicked(wxCommandEvent &evt);
    void OnMenu(wxCommandEvent &evt);
    void OnCheckForUpdates();
    void OnPreviewTimer(wxTimerEvent &evt);
    void OnStatsTimer(wxTimerEvent &evt);
    void OnClose(wxCloseEvent &evt);
    void OnIconize(wxIconizeEvent &evt);
    void OnShowEvent(wxShowEvent &evt);
    void OnHotkeyEvent(wxThreadEvent &evt); // posted from hr_hotkey.cpp's background thread

    AppState state_;
    LanguageTable lang_;
    ThemeColors theme_ = GetBuiltinTheme("dark");

    std::unique_ptr<RecordingController> rec_;
    RecordingController *rec_raw_ = nullptr; // stable lvalue for PreviewPanel's RecordingController*& ctor param
    std::unique_ptr<AudioPanel> audio_panel_;
    std::unique_ptr<ConsoleWindow> console_;
    std::unique_ptr<OverlaysDockPanel> overlays_panel_;
    std::unique_ptr<LuaPluginEngine> plugins_;

    // OverlaysDockPanel is still raw-Win32 (creates real child HWNDs -
    // list/buttons - that need WM_DRAWITEM delivered to their immediate
    // parent); this wx panel exists purely to be that immediate parent.
    // AudioPanel no longer needs one of these - it's real wx widgets now.
    class NativeHostPanel *overlays_host_ = nullptr;

    wxPanel *left_panel_ = nullptr;
    wxStaticText *title_lbl_ = nullptr;
    wxStaticText *version_lbl_ = nullptr;
    ColorButton *start_color_btn_ = nullptr;
    ColorButton *pause_color_btn_ = nullptr;
    StatusDot *status_dot_ = nullptr;
    wxStaticText *status_lbl_ = nullptr;
    wxStaticText *time_lbl_ = nullptr;
    wxStaticText *fps_lbl_ = nullptr;
    wxStaticText *res_lbl_ = nullptr;
    // "STATUS"/"TIME"/"STATS" section headers built inline by
    // BuildLeftPanel's addSection() lambda - kept as named members (rather
    // than discarded, as before) so ApplyLanguageText() can retranslate
    // them after a language switch instead of only re-theming their colour.
    wxStaticText *section_status_lbl_ = nullptr;
    wxStaticText *section_time_lbl_ = nullptr;
    wxStaticText *section_stats_lbl_ = nullptr;

    wxPanel *preview_container_ = nullptr;
    wxPanel *preview_header_ = nullptr;
    wxStaticText *preview_title_lbl_ = nullptr;
    wxStaticText *preview_fps_lbl_ = nullptr;
    PreviewPanel *preview_panel_ = nullptr;

    wxPanel *bottom_bar_ = nullptr;
    StatusDot *bottom_dot_ = nullptr;
    wxStaticText *file_lbl_ = nullptr;
    wxStaticText *made_by_lbl_ = nullptr;
    wxStaticText *version_bar_lbl_ = nullptr;

    wxTimer preview_timer_;
    wxTimer stats_timer_;
    wxTimer restore_topmost_timer_;

    wxTaskBarIcon *tray_icon_ = nullptr;

    void *hotkey_handle_ = nullptr;

    bool fullscreen_ = false;
};
