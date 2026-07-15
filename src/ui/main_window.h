// main_window.h — Integration pass
//
// Wires together everything built in Phases 1-9: RecordingController,
// AudioPanel, Settings/Advanced Settings dialogs, Overlay manager, Welcome
// dialog, Console window, and the Lua plugin engine. This is the "does it
// actually do something" milestone.
#pragma once

#include <windows.h>
#include <memory>
#include <string>
#include "app_state.h"
#include "theme.h"
#include "language.h"
#include "recording_controller.h"
#include "audio_panel.h"
#include "console_window.h"
#include "overlays_dock_panel.h"
#include "../plugins/lua_engine.h"

class HomRecMainWindow {
public:
    static HomRecMainWindow *Create(HINSTANCE hInstance, int nCmdShow);
    ~HomRecMainWindow();

    HWND hwnd() const { return hwnd_; }

private:
    HomRecMainWindow() = default;

    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    bool RegisterWindowClass(HINSTANCE hInstance);
    void OnCreate();
    void OnPaint();
    void OnEraseBkgnd(HDC hdc);
    void OnSize(int width, int height);
    void OnCommand(int id);
    void OnHScroll(HWND ctrl, int pos);
    void OnDrawItem(DRAWITEMSTRUCT *dis);
    void OnTimer(UINT_PTR id);
    void OnTrayMessage(LPARAM lParam);
    void OnDestroy();

    void BuildMenu();
    void BuildLeftPanel();
    void BuildFonts();
    void ReleaseFonts();
    void ApplyTheme();
    void ApplyLanguage();
    void ToggleTheme();
    void ToggleAlwaysOnTop();
    void ToggleFullscreen();

    void SetupTrayIcon();
    void RemoveTrayIcon();
    void SetupHotkeys();
    void ConfigureHotkeysFromState();

    void DoStart();
    void DoStop();
    void DoPause();
    void RenderPreviewFrame(HDC hdc);
    void ComputeLayout(int width, int height);
    void DrawLeftPanel(HDC hdc);
    void DrawPreviewChrome(HDC hdc);
    void DrawBottomBar(HDC hdc);
    void DrawStartButton(DRAWITEMSTRUCT *dis);
    void DrawPauseButton(DRAWITEMSTRUCT *dis);
    void SetStatusState(const wchar_t *text, COLORREF dotColor);

    // Hotkey callbacks are plain function pointers (HR_HK_CB has no
    // user-data param — see hr_hotkey.cpp) firing on a background thread,
    // so they can't touch UI state directly. They PostMessage a custom
    // WM_APP+N to this window instead, handled back on the UI thread.
    static void HotkeyStartStopThunk();
    static void HotkeyPauseThunk();
    static void HotkeyFullscreenThunk();

    HINSTANCE hInstance_ = nullptr;
    HWND hwnd_ = nullptr;
    HMENU menu_ = nullptr;

    AppState state_;
    LanguageTable lang_;
    ThemeColors theme_ = GetBuiltinTheme("dark");
    ThemeBrushes brushes_;

    std::unique_ptr<RecordingController> rec_;
    std::unique_ptr<AudioPanel> audio_panel_;
    std::unique_ptr<ConsoleWindow> console_;
    std::unique_ptr<OverlaysDockPanel> overlays_panel_;
    std::unique_ptr<LuaPluginEngine> plugins_;

    HWND start_btn_ = nullptr;
    HWND pause_btn_ = nullptr;
    RECT preview_rect_ = {};

    // Left-panel / preview-chrome / bottom-bar layout, recomputed by
    // ComputeLayout() on WM_SIZE and drawn directly with GDI in OnPaint
    // (DrawLeftPanel/DrawPreviewChrome/DrawBottomBar) rather than as a
    // pile of child STATIC controls — see comment above BuildLeftPanel()
    // in main_window.cpp for why.
    RECT left_panel_rect_ = {};
    RECT preview_container_rect_ = {};
    RECT preview_header_rect_ = {};
    RECT bottom_bar_rect_ = {};

    // Dynamic label text + colors, mirrors the Python widgets' .config()
    // calls (status_label/status_icon/time_label/fps_label/res_label/
    // file_label) since these are now just fields redrawn on InvalidateRect
    // rather than real HWNDs.
    std::wstring status_text_ = L"Ready";
    COLORREF status_dot_color_ = 0;      // set from theme_.error in BuildFonts()/ApplyTheme()
    std::wstring time_text_ = L"00:00:00";
    std::wstring fps_text_;
    std::wstring res_text_;
    std::wstring file_label_text_ = L"Ready";
    COLORREF start_btn_bg_ = 0;          // theme_.success (idle) / theme_.error (recording)
    COLORREF pause_btn_bg_ = 0;          // theme_.warning (normal) / theme_.success (paused)
    std::wstring start_btn_text_ = L"\u25B6 START";
    std::wstring pause_btn_text_ = L"\u23F8 PAUSE";

    HFONT font_title_ = nullptr;      // Segoe UI 22 bold — "HomRec"
    HFONT font_version_ = nullptr;    // Segoe UI 11 — "v1.7.2"
    HFONT font_section_ = nullptr;    // Segoe UI 11 bold — "STATUS"/"TIME"/"STATS"
    HFONT font_body_ = nullptr;       // Segoe UI 11 — status text
    HFONT font_dot_ = nullptr;        // Arial 18 — status dot glyph
    HFONT font_time_ = nullptr;       // Consolas 24 bold — big timer
    HFONT font_mono_ = nullptr;       // Consolas 11 — fps/resolution
    HFONT font_btn_start_ = nullptr;  // Segoe UI 11 bold — START/STOP button
    HFONT font_btn_pause_ = nullptr;  // Segoe UI 10 bold — PAUSE/RESUME button
    HFONT font_header_ = nullptr;     // Segoe UI 9 bold — preview header
    HFONT font_small_ = nullptr;      // Segoe UI 8 — preview fps / native indicator / version
    HFONT font_bar_ = nullptr;        // Segoe UI 9 — bottom bar file label
    HFONT font_bar_bold_ = nullptr;   // Segoe UI 9 bold — "made by"

    // True borderless fullscreen (Python's toggle_fullscreen(), not just
    // maximize): remembers the pre-fullscreen style/rect so it can be
    // restored exactly, since WS_OVERLAPPEDWINDOW gets stripped.
    bool fullscreen_ = false;
    LONG_PTR saved_style_ = 0;
    RECT saved_rect_ = {};

    NOTIFYICONDATAW tray_nid_ = {};
    bool tray_added_ = false;

    void *hotkey_handle_ = nullptr; // hr_hk_create() handle

    static constexpr UINT_PTR kPreviewTimerId = 1;
    static constexpr UINT_PTR kStatsTimerId = 2;
    static constexpr UINT kTrayMessage = WM_APP + 1;
    static constexpr UINT kHotkeyStartStopMsg = WM_APP + 2;
    static constexpr UINT kHotkeyPauseMsg = WM_APP + 3;
    static constexpr UINT kHotkeyFullscreenMsg = WM_APP + 4;
};

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
