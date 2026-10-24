// main_frame.h - wxWidgets rewrite of the main shell.
#pragma once

#include <wx/wx.h>
#include <wx/taskbar.h>
#include <wx/aui/aui.h>      // 2.4: dockable / floating panels
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
    ID_FILE_SET_PRESET      = 1026,
    ID_FILE_OPEN_PROGRAM_FILES = 1027,
    ID_FILE_IMPORT_HRP      = 1028,
    ID_FILE_SELECT_REGION   = 1029,
    // 2.4 - panel docking (View menu). ID_VIEW_DOCK_FIRST + pane * 8 + slot.
    ID_VIEW_LOCK_UI         = 1034,
    ID_FILE_FULL_DESKTOP    = 1035,
    ID_VIEW_RESET_LAYOUT    = 1036,
    ID_PREVIEW_TOGGLE_BTN   = 1037,
    ID_VIEW_DOCK_FIRST      = 1040,
    ID_VIEW_DOCK_LAST       = 1069,
};

// ColorButton and StatusDot moved to themed_widgets.h/.cpp so audio_panel
// and settings_dialog (also now wx-based) can share them instead of
// duplicating - see that header for their docs.

class PreviewPanel : public wxPanel {
public:
    PreviewPanel(wxWindow *parent, RecordingController *&rec, AppState &state);
    void SetPlaceholderText(const wxString &text) {
        if (placeholder_ == text) return; // called every timer tick - don't repaint for nothing
        placeholder_ = text;
        Refresh();
    }

    void PollRefresh();

    // Writes any debounced overlay edit (arrow-key nudges) to disk right now.
    void FlushOverlaySave();

    void EnterSnapshotMode(const std::vector<uint8_t> &buf, int w, int h,
                           int native_w = 0, int native_h = 0);
    void UpdateSnapshotFrame(const std::vector<uint8_t> &buf, int w, int h,
                             int native_w = 0, int native_h = 0);
    void ExitSnapshotMode();
    bool InSnapshotMode() const { return snapshot_mode_; }

private:
    void OnPaint(wxPaintEvent &evt);
    void OnLeftDown(wxMouseEvent &evt);
    void OnMouseMove(wxMouseEvent &evt);
    void OnLeftUp(wxMouseEvent &evt);
    void OnRightUp(wxMouseEvent &evt);
    void OnKeyDown(wxKeyEvent &evt);
    void OnCaptureLost(wxMouseCaptureLostEvent &evt);

    // Preview-off editing session on a one-off screenshot (see .cpp).
    bool BeginSnapshotEditing();
    void RefreshSnapshot();
    void EndSnapshotEditingSession();
    // Size overlay x/y/w/h are measured in for what's currently shown.
    bool GetNativeSize(int &w, int &h) const;
    void EndDrag();               // shared by OnLeftUp / stuck-drag recovery
    void ScheduleOverlaySave();   // debounced (400ms) autosave of state_.overlays

    bool GetPreviewRect(wxRect &out) const;

    RecordingController *&rec_;
    AppState &state_;
    wxString placeholder_ = "Preview loading...";
    std::vector<uint8_t> frame_buf_;

    // -- "Apply with preview off" snapshot mode (see EnterSnapshotMode()) --
    bool snapshot_mode_ = false;
    std::vector<uint8_t> snapshot_buf_;
    int snapshot_w_ = 0, snapshot_h_ = 0;
    int snapshot_native_w_ = 0, snapshot_native_h_ = 0;

    int drag_overlay_index_ = -1;
    // Which corner (if any) is being dragged to resize. kNone means a
    // plain move-drag (grabbed the body, not a handle).
    enum class Corner { kNone, kTopLeft, kBottomRight };
    Corner drag_corner_ = Corner::kNone;
    // Topmost overlay under the point (or -1); sets `corner` if a resize
    // handle was hit.
    int HitTestOverlay(int mx, int my, Corner &corner) const;
    // Overlay the user last clicked - stays highlighted after mouse-up.
    int selected_overlay_index_ = -1;
    int drag_start_mouse_x_ = 0, drag_start_mouse_y_ = 0;
    int drag_start_ov_x_ = 0, drag_start_ov_y_ = 0, drag_start_ov_w_ = 0, drag_start_ov_h_ = 0;

    std::vector<uint8_t> last_frame_buf_;
    wxBitmap cached_bmp_;
    int cached_src_w_ = -1, cached_src_h_ = -1;
    int cached_dst_w_ = -1, cached_dst_h_ = -1;
    int cached_panel_w_ = -1, cached_panel_h_ = -1;

    // -- change tracking (see PollRefresh()/OnPaint()) ----------------------
    uint64_t pv_seq_ = 0;            // last preview sequence number painted
    int frame_w_ = 0, frame_h_ = 0;  // size of frame_buf_'s valid pixels
    int frame_native_w_ = 0, frame_native_h_ = 0; // overlay coordinate space
    bool have_frame_ = false;
    bool snapshot_dirty_ = false;    // snapshot_buf_ changed since last scale
    bool cache_dirty_ = true;        // cached_bmp_ no longer matches the source
    int idle_ticks_ = 0;
    int req_pv_w_ = 0, req_pv_h_ = 0;  // last thumbnail size requested from the pipeline
    bool drag_moved_ = false;        // a drag actually changed the overlay
    // 7th pass: the overlay being dragged is snapped to the picture's centre on this axis right now (draws a guide line).
    bool snap_center_x_ = false, snap_center_y_ = false;
    bool overlay_save_pending_ = false;
    wxTimer overlay_save_timer_;
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
    // Phase 1 settings-storage migration (see commands.md): persists the *entire* current
    // state_ via HrcConfig::Save(), mirroring to the default location too if a custom settings
    // path is configured (see hrc_config.h's ResolveSettingsPath()).
    void PersistSettings();
    void ToggleLivePreview();          // preview on/off (button above the preview); persists the choice
    void UpdatePreviewFpsLabel();      // FPS readout next to the Preview ON/OFF button (always visible, fixed width)
    void UpdatePreviewToggleButton();  // label + colours of that button follow state_.disable_preview / theme
    void OnCaptureTargetChanged(); // after the capture source changed (Layers panel) - see main_frame.cpp
    // 7th pass: the capture source lives in the Layers panel now (File > Select Window / Region / Full Desktop
    // are gone). OpenCaptureSourceDialog() = the full "Capture source" window; RunSourceAction() = the quick
    // items of the source row's context menu (OverlaysDockPanel::SourceAction).
    void OpenCaptureSourceDialog();
    void RunSourceAction(int action);

    enum DockSlot { kDockLeft = 0, kDockRight, kDockTop, kDockBottom, kDockFloat };
    void SetupDocking();
    void ApplyDockArtColours();
    void ApplyUiLock();
    void SetPaneShown(const char *name, bool show, bool persist);
    void DockPaneTo(const char *name, DockSlot slot);
    void ResetDockLayout();
    void SaveDockLayoutToState();
    void OnAuiPaneClose(wxAuiManagerEvent &evt);
    // The Controls pane re-flows: stacked vertically when docked left/right (or floating
    // tall), laid out in a row when docked top/bottom.
    void LayoutControlsPanel(bool horizontal);
    void UpdateControlsOrientation();

    wxAuiManager aui_;
    wxPanel *dock_host_ = nullptr;
    bool dock_ready_ = false;
    bool ui_lock_applied_ = false;
    bool controls_horizontal_ = false;
    bool controls_reflow_pending_ = false;
    wxString default_perspective_;

    void SetupHotkeys();
    void ConfigureHotkeysFromState();
    void SetupTrayIcon();

    void DoStart();
    void DoStop();
    // Runs on the UI thread (via CallAfter from RecordingController's background finalize
    // thread) once StopAsync()'s tail is fully done - the rest of what DoStop() used to do
    // synchronously right after rec_->Stop() returned.
    void OnRecordingFinalized();
    void HandleRecordingCrashed();
    void DoPause();
    void DoSaveReplay(); // Instant Replay hotkey/menu action - see recording_controller.h's SaveReplay()
    void RequestStart();
    void OnCountdownTimer(wxTimerEvent &evt);
    // Ticks every second; fires RequestStart() once local time reaches
    // AppState::scheduled_start_time, if scheduled_start_enabled and nothing's already
    // recording.
    void OnScheduleTimer(wxTimerEvent &evt);
    wxTimer countdown_timer_;
    // Scheduled recording start (todo2.3.md section 3) - checked once a second against
    // AppState::scheduled_start_time/_enabled by OnScheduleTimer().
    wxTimer schedule_timer_;
    std::string schedule_last_matched_minute_;
    bool schedule_parse_warned_ = false; // avoid re-logging every second while scheduled_start_time is malformed
    int countdown_remaining_ = 0;
    // Session-only (not persisted to settings.json) - resets each launch,
    // same lifetime as the Python original's in-memory dont_show_again
    // this ports from. state_.show_summary (the actual settings toggle)
    bool summary_dont_show_again_ = false;
    bool recording_crashed_pending_notice_ = false;
    // Stop was pressed (or the encoder died) and the recording is being finalized in the background
    bool stop_pending_ = false;
    // The crash was the full-GPU (ddagrab) ffmpeg dying (e.g. a game changed the display mode): once the
    // interrupted recording is finalized, a new one is started right away with the normal pipeline.
    bool recording_restart_after_crash_ = false;
    void SetStatusState(const wxString &text, COLORREF dotColor);
    void ToggleFullscreenNative();
    // Un-minimizes + un-hides the window from the tray, in that order -
    // see SetupTrayIcon()'s comment for why both steps are needed.
    void RestoreFromTray();
    // Opens state_.output_folder in Explorer and makes sure it actually
    // ends up in front of HomRec instead of silently behind it - see the
    // comment at its definition for why that isn't automatic.
    void OpenRecordingsFolder();
    void OpenProgramFilesFolder();
    void ImportHrpPlugin();
    void OnRestoreTopmostTimer(wxTimerEvent &evt);
    bool pending_restore_topmost_ = false;

    // wx event handlers
    void OnStartClicked(wxCommandEvent &evt);
    void OnPauseClicked(wxCommandEvent &evt);
    void OnMenu(wxCommandEvent &evt);
    void OnCheckForUpdates();
    void OnPreviewTimer(wxTimerEvent &evt);
    void OnStatsTimer(wxTimerEvent &evt);
    // Own timer for the Audio Mixer's VU meter bars (see the .cpp for why this isn't just
    // piggybacked on stats_timer_ anymore) - also (re)started from ID_SETTINGS_OPEN's handler
    // whenever state_.level_meter_fps changes.
    void OnLevelMeterTimer(wxTimerEvent &evt);
    void RestartLevelMeterTimer();
    void OnClose(wxCloseEvent &evt);
    void OnIconize(wxIconizeEvent &evt);
    void OnShowEvent(wxShowEvent &evt);
    void OnHotkeyEvent(wxThreadEvent &evt); // posted from hotkey.cpp's background thread

    // hom plugin-update badge (bottom bar) - see CheckHomUpdatesAsync()'s
    // comment for why this is a plain background `hom list --upgradable`
    // check plus a click-for-details message box, not a plugin browser.
    void CheckHomUpdatesAsync();
    void OnHomUpdatesChecked(wxThreadEvent &evt); // posted from CheckHomUpdatesAsync()'s background thread
    void OnPluginUpdatesClick(wxMouseEvent &evt);

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
    class NativeHostPanel *overlays_host_ = nullptr;

    wxPanel *left_panel_ = nullptr;
    wxStaticText *title_lbl_ = nullptr;
    wxStaticText *version_lbl_ = nullptr;
    ColorButton *start_color_btn_ = nullptr;
    ColorButton *preview_toggle_btn_ = nullptr;   // "Preview: ON / OFF" in the preview header
    ColorButton *pause_color_btn_ = nullptr;
    StatusDot *status_dot_ = nullptr;
    wxStaticText *status_lbl_ = nullptr;
    wxStaticText *time_lbl_ = nullptr;
    wxStaticText *fps_lbl_ = nullptr;
    wxStaticText *res_lbl_ = nullptr;
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
    // Bottom-bar "N plugin update(s)" badge - hidden (empty label) until
    // CheckHomUpdatesAsync()'s background `hom list --upgradable` finds something.
    wxStaticText *plugin_updates_lbl_ = nullptr;
    wxString plugin_updates_summary_;
    wxStaticText *version_bar_lbl_ = nullptr;

    wxTimer preview_timer_;
    wxTimer stats_timer_;
    wxTimer level_meter_timer_;
    wxTimer restore_topmost_timer_;

    wxTaskBarIcon *tray_icon_ = nullptr;

    void *hotkey_handle_ = nullptr;

    bool fullscreen_ = false;
};
