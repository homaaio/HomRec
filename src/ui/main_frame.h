// main_frame.h — wxWidgets rewrite of the main shell.
//
// Supersedes main_window.h/.cpp. The raw-GDI version (see git history /
// main_window.cpp.bak if kept) rendered its own text and colors by hand
// with CreateFontW(-heightInPixels, ...), which does not participate in
// Windows DPI scaling the way real widgets do — that's what produced the
// "tiny fonts" complaint. wxWidgets gives real retained-mode widgets
// (wxStaticText/wxButton/wxPanel — conceptually the same model as
// Tkinter's Label/Button/Frame), proper DPI-aware font point sizes, and a
// wxImage/wxBitmap pipeline for the live preview instead of a hand-rolled
// StretchDIBits call, which is also what was silently failing to show any
// preview at all.
//
// Scope of this pass: the shell (menu, left sidebar, preview, bottom bar),
// AudioPanel (mic/desktop mixer strip), and the Settings dialog are now
// wx. RecordingController/OverlaysDockPanel/the remaining dialogs
// (Advanced Settings, Welcome, Console, Overlay Manager, Log Viewer, PC
// Analytics, Window Picker) are still untouched raw-Win32 code — a wxFrame
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

// Menu/control IDs — kept identical to the old main_window.h enum so every
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
};

// ColorButton and StatusDot moved to themed_widgets.h/.cpp so audio_panel
// and settings_dialog (also now wx-based) can share them instead of
// duplicating — see that header for their docs.

// Draws the live capture preview from RecordingController::GetPreviewFrame's
// raw RGB24 buffer (converted to a wxImage/wxBitmap and scaled to fit) —
// this is the actual fix for "no preview": the old code's StretchDIBits
// call was structurally fine but nothing was verified to reach it; wx's
// wxImage path is simpler to get right and easier to debug.
class PreviewPanel : public wxPanel {
public:
    PreviewPanel(wxWindow *parent, RecordingController *&rec, AppState &state);
    void SetPlaceholderText(const wxString &text) { placeholder_ = text; Refresh(); }

private:
    void OnPaint(wxPaintEvent &evt);
    RecordingController *&rec_;
    AppState &state_;
    wxString placeholder_ = "Preview (start recording to see it)";
    std::vector<uint8_t> frame_buf_;
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

    void SetupHotkeys();
    void ConfigureHotkeysFromState();
    void SetupTrayIcon();

    void DoStart();
    void DoStop();
    void DoPause();
    void SetStatusState(const wxString &text, COLORREF dotColor);
    void ToggleFullscreenNative();

    // wx event handlers
    void OnStartClicked(wxCommandEvent &evt);
    void OnPauseClicked(wxCommandEvent &evt);
    void OnMenu(wxCommandEvent &evt);
    void OnPreviewTimer(wxTimerEvent &evt);
    void OnStatsTimer(wxTimerEvent &evt);
    void OnClose(wxCloseEvent &evt);
    void OnIconize(wxIconizeEvent &evt);
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

    // OverlaysDockPanel is still raw-Win32 (creates real child HWNDs —
    // list/buttons — that need WM_DRAWITEM delivered to their immediate
    // parent); this wx panel exists purely to be that immediate parent.
    // AudioPanel no longer needs one of these — it's real wx widgets now.
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

    wxTaskBarIcon *tray_icon_ = nullptr;

    void *hotkey_handle_ = nullptr;

    bool fullscreen_ = false;
};
