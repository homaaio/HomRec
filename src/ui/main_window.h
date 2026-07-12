// main_window.h — Integration pass
//
// Wires together everything built in Phases 1-9: RecordingController,
// AudioPanel, Settings/Advanced Settings dialogs, Overlay manager, Welcome
// dialog, Console window, and the Lua plugin engine. This is the "does it
// actually do something" milestone.
#pragma once

#include <windows.h>
#include <memory>
#include "app_state.h"
#include "theme.h"
#include "language.h"
#include "recording_controller.h"
#include "audio_panel.h"
#include "console_window.h"
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
    void OnSize(int width, int height);
    void OnCommand(int id);
    void OnHScroll(HWND ctrl, int pos);
    void OnDrawItem(DRAWITEMSTRUCT *dis);
    void OnTimer(UINT_PTR id);
    void OnTrayMessage(LPARAM lParam);
    void OnDestroy();

    void BuildMenu();
    void BuildToolbar();
    void ApplyTheme();
    void ApplyLanguage();
    void ToggleTheme();
    void ToggleAlwaysOnTop();

    void SetupTrayIcon();
    void RemoveTrayIcon();
    void SetupHotkeys();

    void DoStart();
    void DoStop();
    void DoPause();
    void RenderPreviewFrame(HDC hdc);

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
    std::unique_ptr<LuaPluginEngine> plugins_;

    HWND status_label_ = nullptr;
    HWND start_btn_ = nullptr;
    HWND pause_btn_ = nullptr;
    RECT preview_rect_ = {};

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
};
