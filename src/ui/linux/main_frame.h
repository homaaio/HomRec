// main_frame.h - the Linux (wxGTK) main window.
//
// Same layout as the Windows window (ui/main_frame.h): Controls, Preview, Audio Mixer and Layers
// panes in a wxAUI dock, menu bar, status bar. What needs a backend that is not there yet shows a
// "not now" placeholder: recording (Start / Pause), the live preview, the audio mixer, the layers /
// overlays panel, global hotkeys, the tray icon, plugins and update checks. Settings, presets,
// import / export, the console, the log, PC analytics, About and Welcome all work.
#pragma once

#include <wx/wx.h>
#include <wx/aui/aui.h>

#include "../app_state.h"
#include "../theme.h"
#include "../language.h"
#include "../themed_widgets.h"
#include "rec_stub.h"

class MainFrame : public wxFrame {
public:
    MainFrame();
    ~MainFrame() override;

private:
    enum {
        ID_FILE_OPEN_RECORDINGS = wxID_HIGHEST + 100,
        ID_FILE_OPEN_CONFIG,
        ID_FILE_EXPORT_HRC,
        ID_FILE_IMPORT_HRC,
        ID_FILE_IMPORT_HRP,
        ID_FILE_SET_PRESET,
        ID_FILE_EXIT,
        ID_VIEW_ALWAYS_ON_TOP,
        ID_VIEW_FULLSCREEN,
        ID_VIEW_OVERLAYS_PANEL,
        ID_VIEW_AUDIO_PANEL,
        ID_VIEW_LOCK_UI,
        ID_VIEW_RESET_LAYOUT,
        ID_VIEW_PC_ANALYTICS,
        ID_VIEW_LOG,
        ID_THEME_DARK,
        ID_THEME_LIGHT,
        ID_SETTINGS_OPEN,
        ID_HELP_CHECK_UPDATES,
        ID_HELP_CONSOLE,
        ID_HELP_WELCOME,
        ID_HELP_ABOUT,
        ID_START_BTN,
        ID_PAUSE_BTN,
        // "View > Move panel to": 3 panes x up to 8 slots
        ID_VIEW_DOCK_FIRST = wxID_HIGHEST + 300,
        ID_VIEW_DOCK_LAST  = ID_VIEW_DOCK_FIRST + 3 * 8 - 1,
    };
    enum DockSlot { kDockLeft, kDockRight, kDockTop, kDockBottom, kDockFloat };

    // startup / settings
    void LoadState();
    void PersistSettings();

    // building the window
    void BuildMenuBar();
    void BuildControls(wxWindow *parent);
    void BuildPreview(wxWindow *parent);
    void BuildBottomBar(wxWindow *parent, wxSizer *sizer);
    void ApplyThemeColours();
    void ApplyLanguageText();
    void UpdateStatsText();
    void SetStatusState(const wxString &text, COLORREF dot_colour);

    // docking
    void SetupDocking();
    void ApplyDockArtColours();
    void ApplyUiLock();
    void SaveDockLayoutToState();
    void SetPaneShown(const char *name, bool show, bool persist);
    void DockPaneTo(const char *name, DockSlot slot);
    void ResetDockLayout();
    void OnAuiPaneClose(wxAuiManagerEvent &evt);

    // actions
    void OnMenu(wxCommandEvent &evt);
    void OnStartClicked(wxCommandEvent &);
    void OnPauseClicked(wxCommandEvent &);
    void OnClose(wxCloseEvent &evt);
    void OpenRecordingsFolder();
    void OpenFolder(const std::string &path, const wxString &what);
    void ExportSettings();
    void ImportSettings();
    void AfterSettingsChanged();

    AppState state_;
    ThemeColors theme_;
    LanguageTable lang_;
    RecordingController rec_;          // placeholder, see rec_stub.h

    // layout
    wxAuiManager aui_;
    wxString default_perspective_;
    bool dock_ready_ = false;
    wxPanel *dock_host_ = nullptr;

    // controls pane
    wxPanel *left_panel_ = nullptr;
    ColorButton *start_btn_ = nullptr, *pause_btn_ = nullptr;
    wxStaticText *section_time_lbl_ = nullptr, *time_lbl_ = nullptr;
    wxStaticText *section_stats_lbl_ = nullptr, *fps_lbl_ = nullptr, *res_lbl_ = nullptr;

    // preview pane
    wxPanel *preview_container_ = nullptr, *preview_header_ = nullptr, *preview_box_ = nullptr;
    wxStaticText *preview_title_lbl_ = nullptr, *preview_note_lbl_ = nullptr;

    // side panes (placeholders)
    wxPanel *audio_panel_ = nullptr, *overlays_panel_ = nullptr;
    wxStaticText *audio_lbl_ = nullptr, *overlays_lbl_ = nullptr;

    // bottom bar
    wxPanel *bottom_bar_ = nullptr;
    StatusDot *bottom_dot_ = nullptr;
    wxStaticText *file_lbl_ = nullptr, *made_by_lbl_ = nullptr, *version_bar_lbl_ = nullptr;
};
