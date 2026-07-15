#include "main_window.h"
#include "version.h"
#include "settings_dialog.h"
#include "advanced_settings_dialog.h"
#include "overlay_manager.h"
#include "welcome_dialog.h"
#include "pc_analytics_dialog.h"
#include "log_viewer_dialog.h"
#include "window_picker_dialog.h"
#include <commctrl.h>
#include <windowsx.h>
#include <string>
#include <algorithm>
#include <cstdio>

extern "C" {
    int hr_acquire_single_instance(const char *mutex_name); // unused here, called once in win_main.cpp
    void *hr_hk_create();
    void hr_hk_destroy(void *handle);
    void hr_hk_set_callbacks(void *handle, void (*start_stop)(), void (*pause)(), void (*fullscreen)());
    void hr_hk_configure(void *handle, const char *start_stop_str, const char *pause_str, const char *fullscreen_str);
    int hr_hk_start(void *handle);
    void hr_hk_stop(void *handle);

    // hr_settings.cpp — loaded once at startup so a restart actually
    // remembers what Settings previously saved (previously state_ only
    // ever reflected persisted settings while the Settings dialog itself
    // was open, since only settings_dialog.cpp called hr_settings_load).
    void *hr_settings_create();
    void hr_settings_destroy(void *handle);
    int hr_settings_load(void *handle, const char *path);
    const char *hr_settings_get_output_folder(const void *h);
    int hr_settings_get_quality(const void *h);
    int hr_settings_get_fps(const void *h);
    int hr_settings_get_monitor(const void *h);
    int hr_settings_get_flag(const void *h, const char *name);
}

namespace {
constexpr wchar_t kWindowClassName[] = L"HomRecMainWindow";

// Only one main window exists per process — hotkey callbacks (plain
// function pointers, no user-data slot, see hr_hotkey.cpp) need a way back
// to "the" window, and this is it.
HomRecMainWindow *g_instance = nullptr;

std::wstring WideFromNarrow(const std::string &s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

// Left-sidebar layout, mirrors ui_mixin.py's create_widgets() stacking
// order top-to-bottom (title -> START/PAUSE -> STATUS -> TIME -> STATS),
// all inset by kPad from the sidebar's left/right edges (matches the
// Python frames' padx=15). Kept as constants shared between
// ComputeLayout() (positions the two real button HWNDs) and
// DrawLeftPanel() (draws everything else at the same Y offsets), so they
// can't drift apart.
constexpr int kOuterPad     = 15;
constexpr int kBottomBarH   = 32;
constexpr int kLeftPanelW   = 240;
constexpr int kPad          = 15;   // inner padx within the sidebar

constexpr int kTitleY       = 20;
constexpr int kTitleH       = 30;
constexpr int kVersionH     = 20;

constexpr int kBtnGapAbove  = 25;   // btn_frame's pady
constexpr int kStartBtnH    = 48;
constexpr int kBtnGapMid    = 4;
constexpr int kPauseBtnH    = 32;

constexpr int kStatusGap    = 15;   // status_frame's pady
constexpr int kSectionLblH  = 20;
constexpr int kStatusRowGap = 8;
constexpr int kStatusRowH   = 26;

constexpr int kTimerGap     = 15;
constexpr int kTimeValGap   = 8;
constexpr int kTimeValH     = 34;

constexpr int kStatsGap     = 15;
constexpr int kStatsLineH   = 24;
} // namespace

HomRecMainWindow *HomRecMainWindow::Create(HINSTANCE hInstance, int nCmdShow) {
    auto *self = new HomRecMainWindow();
    self->hInstance_ = hInstance;
    g_instance = self;

    if (!self->RegisterWindowClass(hInstance)) {
        delete self;
        g_instance = nullptr;
        return nullptr;
    }

    self->hwnd_ = CreateWindowExW(
        0, kWindowClassName, L"HomRec",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        self->state_.window_w, self->state_.window_h,
        nullptr, nullptr, hInstance, self);

    if (!self->hwnd_) {
        delete self;
        g_instance = nullptr;
        return nullptr;
    }

    ShowWindow(self->hwnd_, nCmdShow);
    UpdateWindow(self->hwnd_);
    return self;
}

HomRecMainWindow::~HomRecMainWindow() {
    RemoveTrayIcon();
    ReleaseFonts();
    if (hotkey_handle_) {
        hr_hk_stop(hotkey_handle_);
        hr_hk_destroy(hotkey_handle_);
    }
    if (g_instance == this) g_instance = nullptr;
}

bool HomRecMainWindow::RegisterWindowClass(HINSTANCE hInstance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &HomRecMainWindow::WindowProcThunk;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kWindowClassName;
    wc.hbrBackground = nullptr;
    return RegisterClassExW(&wc) != 0;
}

LRESULT CALLBACK HomRecMainWindow::WindowProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HomRecMainWindow *self = nullptr;
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        self = reinterpret_cast<HomRecMainWindow *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        // FIX: hwnd_ used to only get set by Create()'s
        // `self->hwnd_ = CreateWindowExW(...)` assignment, which can't run
        // until CreateWindowExW returns. But WM_CREATE — which calls
        // OnCreate(), which parents every child control (menu, status
        // label, AudioPanel, OverlaysDockPanel, ...) to hwnd_ — is sent
        // synchronously from *inside* that same CreateWindowExW call, so
        // hwnd_ was still nullptr for all of OnCreate(). WS_CHILD windows
        // require a real parent HWND, so every one of those child
        // CreateWindowExW calls was failing outright (ERROR_INVALID_
        // PARAMETER) — and very likely what surfaced as "Failed to create
        // the main window" too. The real HWND already exists by
        // WM_NCCREATE (it's right here as this function's own `hwnd`
        // parameter); stash it immediately instead of waiting.
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<HomRecMainWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT HomRecMainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            OnCreate();
            return 0;
        case WM_ERASEBKGND:
            OnEraseBkgnd(reinterpret_cast<HDC>(wParam));
            return 1;
        case WM_PAINT:
            OnPaint();
            return 0;
        case WM_SIZE:
            OnSize(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wParam));
            return 0;
        case WM_HSCROLL:
            OnHScroll((HWND)lParam, (int)LOWORD(wParam));
            return 0;
        case WM_DRAWITEM:
            OnDrawItem(reinterpret_cast<DRAWITEMSTRUCT *>(lParam));
            return TRUE;
        case WM_TIMER:
            OnTimer((UINT_PTR)wParam);
            return 0;
        case kTrayMessage:
            OnTrayMessage(lParam);
            return 0;
        case kHotkeyStartStopMsg:
            if (state_.recording) DoStop(); else DoStart();
            return 0;
        case kHotkeyPauseMsg:
            DoPause();
            return 0;
        case kHotkeyFullscreenMsg:
            ToggleFullscreen();
            return 0;
        case WM_GETMINMAXINFO: {
            auto *mmi = reinterpret_cast<MINMAXINFO *>(lParam);
            mmi->ptMinTrackSize.x = state_.window_min_w;
            mmi->ptMinTrackSize.y = state_.window_min_h;
            return 0;
        }
        case WM_INITMENUPOPUP:
            // Keeps the "Overlays Panel" checkmark honest even when the
            // panel's own ✕ button (not this menu) is what changed
            // state_.show_overlays_panel — see overlays_dock_panel.cpp's
            // ClosePanel().
            CheckMenuItem(menu_, ID_VIEW_OVERLAYS_PANEL,
                          MF_BYCOMMAND | (state_.show_overlays_panel ? MF_CHECKED : MF_UNCHECKED));
            return 0;
        case WM_CLOSE:
            if (state_.minimize_to_tray && tray_added_) {
                ShowWindow(hwnd_, SW_HIDE);
                return 0;
            }
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            OnDestroy();
            return 0;
        default:
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}

void HomRecMainWindow::OnCreate() {
    lang_ = LanguageTable::Load(state_.current_language, "Assets\\L");
    theme_ = GetBuiltinTheme(state_.current_theme);
    brushes_.Rebuild(theme_);

    // Load whatever Settings previously saved (homrec_settings.json) — was
    // previously never read at startup, so a relaunch silently forgot
    // quality/fps/monitor/output-folder/flags every time. hr_settings_*
    // getters return sane built-in defaults if the file doesn't exist yet
    // (first run), so this is safe to call unconditionally.
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
        state_.output_folder = "recordings"; // first run, no settings file yet
    }
    hr_settings_destroy(settings);

    BuildMenu();

    rec_ = std::make_unique<RecordingController>(state_);
    rec_->Initialize();

    // Right-panel content starts after the 240px left sidebar (15 outer
    // pad + 240 sidebar + 15 gap = 270), matching Python's
    // left_panel.pack(side="left", padx=(0, 15)) + main content layout —
    // see BuildLeftPanel()/ComputeLayout() for the rest of this geometry.
    const int kRightX = 15 + 240 + 15;
    audio_panel_ = std::make_unique<AudioPanel>(state_, *rec_);
    // Real position/size refined in OnSize once the client rect is known;
    // create it small here so child HWNDs exist before first layout pass.
    audio_panel_->Create(hwnd_, hInstance_, kRightX, 420, 900, 90);

    // Fixed sidebar rect, same "doesn't reflow on WM_SIZE" limitation as
    // AudioPanel above — see overlays_dock_panel.h's header comment.
    // Positioned against the default window size (app_state.h); OnSize()
    // shrinks preview_rect_ to leave room for it, but the panel's own
    // HWNDs stay put if the window is resized afterward.
    overlays_panel_ = std::make_unique<OverlaysDockPanel>(state_);
    overlays_panel_->Create(hwnd_, hInstance_,
                             state_.window_w - 232, 15, 220, state_.window_h - 15 - 32 - 15 - 15);

    BuildFonts();
    BuildLeftPanel();
    SetupTrayIcon();
    SetupHotkeys();

    plugins_ = std::make_unique<LuaPluginEngine>("plugins");
    plugins_->SetContext(rec_.get(), &theme_);
    plugins_->LoadAll();

    ApplyLanguage();
    status_dot_color_ = theme_.error; // idle/"Ready" dot is red — matches ui_mixin.py's status_icon default fg
    start_btn_bg_ = theme_.success;
    pause_btn_bg_ = theme_.warning;

    SetTimer(hwnd_, kPreviewTimerId, 1000 / 30, nullptr); // ~30fps preview redraw
    SetTimer(hwnd_, kStatsTimerId, 500, nullptr);          // stats/level polling, matches the Python after(500,...) cadence

    if (state_.first_launch) {
        ShowWelcomeDialog(hwnd_, hInstance_);
    }
}

void HomRecMainWindow::BuildMenu() {
    menu_ = CreateMenu();

    HMENU fileMenu = CreatePopupMenu();
    AppendMenuA(fileMenu, MF_STRING, ID_FILE_OPEN_RECORDINGS, "Open Recordings Folder");
    AppendMenuA(fileMenu, MF_STRING, ID_FILE_SELECT_WINDOW, "Select Window to Record...");
    AppendMenuA(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(fileMenu, MF_STRING, ID_FILE_EXIT, "Exit");
    AppendMenuA(menu_, MF_POPUP, (UINT_PTR)fileMenu, "File");

    HMENU viewMenu = CreatePopupMenu();
    AppendMenuA(viewMenu, MF_STRING, ID_VIEW_ALWAYS_ON_TOP, "Always on Top");
    AppendMenuA(viewMenu, MF_STRING, ID_VIEW_FULLSCREEN, "Fullscreen\tF11");
    AppendMenuA(viewMenu, MF_STRING | (state_.show_overlays_panel ? MF_CHECKED : MF_UNCHECKED),
                ID_VIEW_OVERLAYS_PANEL, "Overlays Panel");
    AppendMenuA(viewMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(viewMenu, MF_STRING, ID_VIEW_PC_ANALYTICS, "PC Analytics");
    AppendMenuA(viewMenu, MF_STRING, ID_VIEW_LOG, "Show Log");
    AppendMenuA(menu_, MF_POPUP, (UINT_PTR)viewMenu, "View");

    HMENU themeMenu = CreatePopupMenu();
    AppendMenuA(themeMenu, MF_STRING, ID_THEME_DARK, "Dark");
    AppendMenuA(themeMenu, MF_STRING, ID_THEME_LIGHT, "Light");
    HMENU settingsMenu = CreatePopupMenu();
    AppendMenuA(settingsMenu, MF_STRING, ID_SETTINGS_OPEN, "Preferences...");
    AppendMenuA(settingsMenu, MF_STRING, ID_SETTINGS_ADVANCED, "Advanced Settings...");
    AppendMenuA(settingsMenu, MF_STRING, ID_OVERLAYS_MANAGE, "Overlays...");
    AppendMenuA(settingsMenu, MF_POPUP, (UINT_PTR)themeMenu, "Theme");
    AppendMenuA(menu_, MF_POPUP, (UINT_PTR)settingsMenu, "Settings");

    HMENU helpMenu = CreatePopupMenu();
    AppendMenuA(helpMenu, MF_STRING, ID_HELP_CHECK_UPDATES, "Check for Updates");
    AppendMenuA(helpMenu, MF_STRING, ID_HELP_CONSOLE, "Console\tCtrl+Shift+T");
    AppendMenuA(helpMenu, MF_STRING, ID_HELP_WELCOME, "Show Welcome Screen");
    AppendMenuA(helpMenu, MF_STRING, ID_HELP_ABOUT, "About");
    AppendMenuA(menu_, MF_POPUP, (UINT_PTR)helpMenu, "Help");

    SetMenu(hwnd_, menu_);
}

void HomRecMainWindow::BuildFonts() {
    ReleaseFonts();
    auto mk = [](int h, int weight, const wchar_t *face) {
        return CreateFontW(-h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    };
    font_title_     = mk(22, FW_BOLD,   L"Segoe UI");
    font_version_   = mk(11, FW_NORMAL, L"Segoe UI");
    font_section_   = mk(11, FW_BOLD,   L"Segoe UI");
    font_body_      = mk(11, FW_NORMAL, L"Segoe UI");
    font_dot_       = mk(18, FW_NORMAL, L"Arial");
    font_time_      = mk(24, FW_BOLD,   L"Consolas");
    font_mono_      = mk(11, FW_NORMAL, L"Consolas");
    font_btn_start_ = mk(11, FW_BOLD,   L"Segoe UI");
    font_btn_pause_ = mk(10, FW_BOLD,   L"Segoe UI");
    font_header_    = mk(9,  FW_BOLD,   L"Segoe UI");
    font_small_     = mk(8,  FW_NORMAL, L"Segoe UI");
    font_bar_       = mk(9,  FW_NORMAL, L"Segoe UI");
    font_bar_bold_  = mk(9,  FW_BOLD,   L"Segoe UI");
}

void HomRecMainWindow::ReleaseFonts() {
    HFONT *all[] = {&font_title_, &font_version_, &font_section_, &font_body_, &font_dot_,
                     &font_time_, &font_mono_, &font_btn_start_, &font_btn_pause_,
                     &font_header_, &font_small_, &font_bar_, &font_bar_bold_};
    for (HFONT *f : all) {
        if (*f) { DeleteObject(*f); *f = nullptr; }
    }
}

// The Python UI (ui_mixin.py's create_widgets) builds the left sidebar out
// of a dozen-plus tk.Frame/tk.Label widgets: title, START/PAUSE buttons,
// a STATUS row (colored dot + text), a big TIME readout, and a STATS block
// (FPS + resolution) — each with its own bg/fg color pulled from the theme,
// none of which change size or position once created.
//
// A literal-minded port would create ~15 child HWNDs (STATIC labels +
// BS_OWNERDRAW buttons) with WM_CTLCOLORSTATIC plumbing for each one. Doing
// that instead: only the two real buttons (which need actual click/keyboard
// handling) are HWNDs; everything else is plain GDI text drawn straight
// into OnPaint by DrawLeftPanel(), using the exact same colors, fonts, and
// stacked layout as the Python widgets. Fewer moving parts, identical
// pixels, and dynamic values (status text, dot color, timer, fps/res) just
// update a field + InvalidateRect instead of SetWindowTextW/.config().
void HomRecMainWindow::BuildLeftPanel() {
    start_btn_ = CreateWindowExW(0, L"BUTTON", start_btn_text_.c_str(),
                                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  0, 0, 10, 10, hwnd_, (HMENU)ID_START_BTN, hInstance_, nullptr);
    pause_btn_ = CreateWindowExW(0, L"BUTTON", pause_btn_text_.c_str(),
                                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  0, 0, 10, 10, hwnd_, (HMENU)ID_PAUSE_BTN, hInstance_, nullptr);
    EnableWindow(pause_btn_, FALSE);
}

void HomRecMainWindow::SetupTrayIcon() {
    tray_nid_.cbSize = sizeof(tray_nid_);
    tray_nid_.hWnd = hwnd_;
    tray_nid_.uID = 1;
    tray_nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray_nid_.uCallbackMessage = kTrayMessage;
    tray_nid_.hIcon = (HICON)LoadImageW(hInstance_, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    // Manual bounded copy instead of wcsncpy_s: that function isn't
    // guaranteed present on every MinGW runtime flavor (older msvcrt-based
    // toolchains lack it; ucrt-based ones have it) — this avoids a
    // link failure depending on which one you're building against.
    const wchar_t *tip = L"HomRec";
    size_t tipLen = wcslen(tip);
    size_t maxLen = sizeof(tray_nid_.szTip) / sizeof(wchar_t) - 1;
    wcsncpy(tray_nid_.szTip, tip, tipLen < maxLen ? tipLen : maxLen);
    tray_nid_.szTip[tipLen < maxLen ? tipLen : maxLen] = L'\0';
    tray_added_ = Shell_NotifyIconW(NIM_ADD, &tray_nid_) != 0;
}

void HomRecMainWindow::RemoveTrayIcon() {
    if (tray_added_) {
        Shell_NotifyIconW(NIM_DELETE, &tray_nid_);
        tray_added_ = false;
    }
}

void HomRecMainWindow::OnTrayMessage(LPARAM lParam) {
    if (lParam == WM_LBUTTONDBLCLK) {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
    } else if (lParam == WM_RBUTTONUP) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, ID_TRAY_RESTORE, L"Restore");
        AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Exit");
        POINT pt; GetCursorPos(&pt);
        SetForegroundWindow(hwnd_); // required so the popup menu dismisses correctly (documented Win32 quirk)
        TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
        DestroyMenu(m);
    }
}

void HomRecMainWindow::SetupHotkeys() {
    hotkey_handle_ = hr_hk_create();
    hr_hk_set_callbacks(hotkey_handle_, &HotkeyStartStopThunk, &HotkeyPauseThunk, &HotkeyFullscreenThunk);
    ConfigureHotkeysFromState(); // parses state_.hotkey_start_stop/pause/fullscreen, must run before hr_hk_start()
    if (!hr_hk_start(hotkey_handle_)) {
        OutputDebugStringA("HomRec: global hotkeys failed to register — another app may be using them.\n");
    }
}

// Re-applies state_.hotkey_start_stop / hotkey_pause / hotkey_fullscreen
// (editable in Advanced Settings, same "Control+F9"-style strings the
// Python app's hotkey recorder produces) to the hotkey manager. Any string
// that fails to parse silently keeps hr_hotkey.cpp's F9/F10/F11 default for
// that action, same as a fresh install with nothing customized yet.
void HomRecMainWindow::ConfigureHotkeysFromState() {
    if (!hotkey_handle_) return;
    hr_hk_configure(hotkey_handle_,
                     state_.hotkey_start_stop.c_str(),
                     state_.hotkey_pause.c_str(),
                     state_.hotkey_fullscreen.c_str());
}

void HomRecMainWindow::HotkeyStartStopThunk() {
    if (g_instance) PostMessageW(g_instance->hwnd_, kHotkeyStartStopMsg, 0, 0);
}
void HomRecMainWindow::HotkeyPauseThunk() {
    if (g_instance) PostMessageW(g_instance->hwnd_, kHotkeyPauseMsg, 0, 0);
}
void HomRecMainWindow::HotkeyFullscreenThunk() {
    if (g_instance) PostMessageW(g_instance->hwnd_, kHotkeyFullscreenMsg, 0, 0);
}

void HomRecMainWindow::DoStart() {
    if (audio_panel_) {
        rec_->SetAudioLevels(audio_panel_->mic_volume(), audio_panel_->sys_volume(),
                              audio_panel_->mic_muted(), audio_panel_->sys_muted());
    }
    std::wstring err;
    if (!rec_->Start(err)) {
        MessageBoxW(hwnd_, err.c_str(), L"HomRec", MB_OK | MB_ICONWARNING);
        return;
    }
    // Matches recording_mixin.py's start_recording(): record_btn -> STOP/error,
    // status dot -> success (green), status text -> "Recording".
    start_btn_text_ = L"\u25A0 STOP";
    start_btn_bg_ = theme_.error;
    pause_btn_bg_ = theme_.warning;
    EnableWindow(pause_btn_, TRUE);
    SetStatusState(WideFromNarrow(lang_.Get("recording")).c_str(), theme_.success);
    InvalidateRect(hwnd_, &left_panel_rect_, FALSE);
    InvalidateRect(hwnd_, nullptr, FALSE); // repaint owner-drawn buttons (new bg colors)
    if (plugins_) plugins_->EmitHook("on_recording_start");
}

void HomRecMainWindow::DoStop() {
    // Immediate "saving" feedback (matches stop_recording()'s synchronous
    // UI update before the finalize work), even though our Stop() below
    // runs synchronously rather than on a background thread like Python's.
    SetStatusState(L"Saving\u2026", theme_.warning);
    time_text_ = L"00:00:00";
    file_label_text_ = L"Processing\u2026";
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);

    rec_->Stop();

    start_btn_text_ = L"\u25B6 START";
    start_btn_bg_ = theme_.success;
    pause_btn_text_ = L"\u23F8 PAUSE";
    pause_btn_bg_ = theme_.warning;
    EnableWindow(pause_btn_, FALSE);
    // Matches _finalize_ui(): dot back to "error" (the same red used at
    // idle/creation — see BuildLeftPanel()'s comment / ui_mixin.py) and
    // status text back to "Ready".
    SetStatusState(WideFromNarrow(lang_.Get("ready")).c_str(), theme_.error);
    file_label_text_ = WideFromNarrow(lang_.Get("ready"));
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (plugins_) plugins_->EmitHook("on_recording_stop");
    // KNOWN GAP (unchanged from Phase 3): no "recording saved, open folder?"
    // CustomMessageBox popup wired here yet.
}

void HomRecMainWindow::DoPause() {
    if (!state_.recording) return;
    rec_->TogglePause();
    if (state_.paused) {
        // Matches the Python "paused" branch: pause_btn -> RESUME/success,
        // dot -> warning, status -> "Paused".
        pause_btn_text_ = L"\u25B6 RESUME";
        pause_btn_bg_ = theme_.success;
        SetStatusState(WideFromNarrow(lang_.Get("paused")).c_str(), theme_.warning);
    } else {
        pause_btn_text_ = L"\u23F8 PAUSE";
        pause_btn_bg_ = theme_.warning;
        SetStatusState(WideFromNarrow(lang_.Get("recording")).c_str(), theme_.success);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void HomRecMainWindow::SetStatusState(const wchar_t *text, COLORREF dotColor) {
    status_text_ = text;
    status_dot_color_ = dotColor;
}

void HomRecMainWindow::ApplyLanguage() {
    std::string title = lang_.Get("app_title");
    if (title.empty()) title = std::string("HomRec v") + HR_APP_VERSION;
    SetWindowTextW(hwnd_, WideFromNarrow(title).c_str());
}

void HomRecMainWindow::ApplyTheme() {
    brushes_.Rebuild(theme_);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void HomRecMainWindow::ToggleTheme() {
    state_.current_theme = (state_.current_theme == "dark") ? "light" : "dark";
    theme_ = GetBuiltinTheme(state_.current_theme);
    ApplyTheme();
    if (plugins_) plugins_->EmitHookWithColors("on_theme_change", theme_);
}

void HomRecMainWindow::ToggleAlwaysOnTop() {
    state_.always_on_top = !state_.always_on_top;
    SetWindowPos(hwnd_, state_.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

void HomRecMainWindow::ToggleFullscreen() {
    // Matches Python's root.attributes('-fullscreen', ...): a real
    // borderless fullscreen (no title bar/menu/border, covers the whole
    // monitor), not just a maximize. WS_OVERLAPPEDWINDOW is what supplies
    // the caption/border/thick-frame/menu chrome, so stripping it and
    // resizing to the monitor's full rect reproduces that; restoring puts
    // the saved style + rect back exactly.
    if (!fullscreen_) {
        saved_style_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        GetWindowRect(hwnd_, &saved_rect_);

        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(mon, &mi);

        SetWindowLongPtrW(hwnd_, GWL_STYLE,
                          static_cast<LONG_PTR>(saved_style_) & ~(WS_CAPTION | WS_THICKFRAME));
        SetWindowPos(hwnd_, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = true;
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, saved_style_);
        SetWindowPos(hwnd_, nullptr,
                     saved_rect_.left, saved_rect_.top,
                     saved_rect_.right - saved_rect_.left,
                     saved_rect_.bottom - saved_rect_.top,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = false;
    }
}

void HomRecMainWindow::RenderPreviewFrame(HDC hdc) {
    std::vector<uint8_t> frame;
    int w = 0, h = 0;
    if (!rec_ || !rec_->GetPreviewFrame(frame, w, h) || w <= 0 || h <= 0) {
        SetTextColor(hdc, theme_.text_secondary);
        SetBkMode(hdc, TRANSPARENT);
        SelectObject(hdc, font_body_);
        std::wstring msg = state_.recording ? L"Waiting for frames..." : L"Preview (start recording to see it)";
        DrawTextW(hdc, msg.c_str(), -1, &preview_rect_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // negative = top-down DIB, matches row-major RGB24 buffer order
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;

    int destW = preview_rect_.right - preview_rect_.left;
    int destH = preview_rect_.bottom - preview_rect_.top;
    SetStretchBltMode(hdc, HALFTONE);
    StretchDIBits(hdc, preview_rect_.left, preview_rect_.top, destW, destH,
                  0, 0, w, h, frame.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
}

// Recomputes every themed-chrome rect (left sidebar / preview container+
// header / bottom bar) from the client size, and repositions the two real
// button HWNDs to the same Y offsets DrawLeftPanel() draws its labels at.
// See the kOuterPad/kLeftPanelW/... constants above for the shared layout
// numbers, ported from ui_mixin.py's create_widgets() pack() stack.
void HomRecMainWindow::ComputeLayout(int width, int height) {
    left_panel_rect_ = { kOuterPad, kOuterPad,
                          kOuterPad + kLeftPanelW, std::max(height - kBottomBarH - kOuterPad, kOuterPad + 1) };
    bottom_bar_rect_ = { 0, height - kBottomBarH, width, height };

    int right_x = left_panel_rect_.right + 15;
    // Leaves room below the preview for AudioPanel's fixed-position mixer
    // row, same margin the pre-theming code used (AudioPanel doesn't
    // reflow — see audio_panel.h — so this is still an approximation, not
    // a real docked layout).
    int right_margin = state_.show_overlays_panel ? (232 + 15) : kOuterPad;
    RECT container = { right_x, kOuterPad,
                        std::max(width - right_margin, right_x + 1),
                        std::max(height - kBottomBarH - kOuterPad - 100, kOuterPad + 1) };
    preview_container_rect_ = container;
    preview_header_rect_ = { container.left, container.top, container.right, container.top + 30 };
    preview_rect_ = { container.left + 8, preview_header_rect_.bottom + 8,
                       container.right - 8, container.bottom - 8 };

    int bx = left_panel_rect_.left + kPad;
    int bw = kLeftPanelW - 2 * kPad;
    int by = left_panel_rect_.top + kTitleY + kTitleH + kVersionH + kBtnGapAbove;
    if (start_btn_) MoveWindow(start_btn_, bx, by, bw, kStartBtnH, TRUE);
    by += kStartBtnH + kBtnGapMid;
    if (pause_btn_) MoveWindow(pause_btn_, bx, by, bw, kPauseBtnH, TRUE);
}

// Draws everything in the left sidebar except the two real buttons: title,
// STATUS (dot + text), TIME, STATS — all GDI text at fixed Y offsets, see
// the header comment on BuildLeftPanel() for why these aren't child HWNDs.
void HomRecMainWindow::DrawLeftPanel(HDC hdc) {
    FillRect(hdc, &left_panel_rect_, brushes_.surface);
    SetBkMode(hdc, TRANSPARENT);
    int cx = left_panel_rect_.left + kPad;
    int cw = kLeftPanelW - 2 * kPad;

    auto drawLine = [&](int y, int h, HFONT font, COLORREF color, const std::wstring &text, UINT flags = DT_LEFT) {
        RECT r = { cx, left_panel_rect_.top + y, cx + cw, left_panel_rect_.top + y + h };
        SelectObject(hdc, font);
        SetTextColor(hdc, color);
        DrawTextW(hdc, text.c_str(), -1, &r, flags | DT_VCENTER | DT_SINGLELINE);
    };

    drawLine(kTitleY, kTitleH, font_title_, theme_.accent, L"HomRec", DT_CENTER);
    drawLine(kTitleY + kTitleH, kVersionH, font_version_,
             theme_.text_secondary, L"v" HR_APP_VERSION_W, DT_CENTER);

    int y = kTitleY + kTitleH + kVersionH + kBtnGapAbove + kStartBtnH + kBtnGapMid + kPauseBtnH;
    y += kStatusGap;
    drawLine(y, kSectionLblH, font_section_, theme_.accent, WideFromNarrow(lang_.Get("status")));
    {
        int rowY = left_panel_rect_.top + y + kSectionLblH + kStatusRowGap;
        RECT dotR = { cx, rowY, cx + 24, rowY + kStatusRowH };
        SelectObject(hdc, font_dot_);
        SetTextColor(hdc, status_dot_color_);
        DrawTextW(hdc, L"\u2B24", -1, &dotR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT textR = { cx + 26, rowY, cx + cw, rowY + kStatusRowH };
        SelectObject(hdc, font_body_);
        SetTextColor(hdc, theme_.text);
        DrawTextW(hdc, status_text_.c_str(), -1, &textR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    y += kSectionLblH + kStatusRowGap + kStatusRowH;

    y += kTimerGap;
    drawLine(y, kSectionLblH, font_section_, theme_.accent, WideFromNarrow(lang_.Get("time")));
    drawLine(y + kSectionLblH + kTimeValGap, kTimeValH, font_time_, theme_.accent, time_text_, DT_CENTER);
    y += kSectionLblH + kTimeValGap + kTimeValH;

    y += kStatsGap;
    drawLine(y, kSectionLblH, font_section_, theme_.accent, WideFromNarrow(lang_.Get("stats")));
    drawLine(y + kSectionLblH, kStatsLineH, font_mono_, theme_.text, fps_text_);
    drawLine(y + kSectionLblH + kStatsLineH, kStatsLineH, font_mono_, theme_.text, res_text_);
}

// Preview container border/header ("● Live Preview") + inner frame bg,
// matches ui_mixin.py's preview_container/preview_header/preview_frame.
void HomRecMainWindow::DrawPreviewChrome(HDC hdc) {
    FillRect(hdc, &preview_container_rect_, brushes_.surface_light);
    FillRect(hdc, &preview_header_rect_, brushes_.surface_light);

    RECT headerText = preview_header_rect_;
    headerText.left += 10;
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, font_header_);
    SetTextColor(hdc, theme_.accent);
    std::wstring live = L"\u25CF " + WideFromNarrow(lang_.Get("live_preview"));
    DrawTextW(hdc, live.c_str(), -1, &headerText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT fpsText = preview_header_rect_;
    fpsText.right -= 10;
    SelectObject(hdc, font_small_);
    SetTextColor(hdc, theme_.text_secondary);
    DrawTextW(hdc, fps_text_.c_str(), -1, &fpsText, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    FillRect(hdc, &preview_rect_, brushes_.preview_bg);
}

// Bottom status strip: dot, file/status text (left), "made by" + version (right).
void HomRecMainWindow::DrawBottomBar(HDC hdc) {
    FillRect(hdc, &bottom_bar_rect_, brushes_.surface);
    SetBkMode(hdc, TRANSPARENT);

    RECT dotR = bottom_bar_rect_;
    dotR.left += kOuterPad;
    dotR.right = dotR.left + 20;
    SelectObject(hdc, font_dot_);
    SetTextColor(hdc, status_dot_color_);
    DrawTextW(hdc, L"\u2B24", -1, &dotR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT fileR = bottom_bar_rect_;
    fileR.left = dotR.right + 6;
    fileR.right -= 220;
    SelectObject(hdc, font_bar_);
    SetTextColor(hdc, theme_.text);
    DrawTextW(hdc, file_label_text_.c_str(), -1, &fileR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT verR = bottom_bar_rect_;
    verR.right -= kOuterPad;
    verR.left = verR.right - 60;
    SelectObject(hdc, font_small_);
    SetTextColor(hdc, theme_.text_secondary);
    DrawTextW(hdc, L"v" HR_APP_VERSION_W, -1, &verR, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    RECT madeR = bottom_bar_rect_;
    madeR.right = verR.left - 10;
    madeR.left = madeR.right - 160;
    SelectObject(hdc, font_bar_bold_);
    SetTextColor(hdc, theme_.text_secondary);
    DrawTextW(hdc, WideFromNarrow(lang_.Get("made_by")).c_str(), -1, &madeR, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void HomRecMainWindow::DrawStartButton(DRAWITEMSTRUCT *dis) {
    HBRUSH bg = CreateSolidBrush(start_btn_bg_);
    FillRect(dis->hDC, &dis->rcItem, bg);
    DeleteObject(bg);
    SetBkMode(dis->hDC, TRANSPARENT);
    SelectObject(dis->hDC, font_btn_start_);
    // Python draws button text in the theme's "bg" color (dark text on the
    // light success/error button), not white — see ui_mixin.py's
    // record_btn fg=self.colors["bg"].
    SetTextColor(dis->hDC, theme_.bg);
    DrawTextW(dis->hDC, start_btn_text_.c_str(), -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (dis->itemState & ODS_FOCUS) DrawFocusRect(dis->hDC, &dis->rcItem);
}

void HomRecMainWindow::DrawPauseButton(DRAWITEMSTRUCT *dis) {
    bool enabled = (dis->itemState & ODS_DISABLED) == 0 && IsWindowEnabled(pause_btn_);
    HBRUSH bg = CreateSolidBrush(enabled ? pause_btn_bg_ : theme_.surface_light);
    FillRect(dis->hDC, &dis->rcItem, bg);
    DeleteObject(bg);
    SetBkMode(dis->hDC, TRANSPARENT);
    SelectObject(dis->hDC, font_btn_pause_);
    SetTextColor(dis->hDC, enabled ? theme_.bg : theme_.text_secondary);
    DrawTextW(dis->hDC, pause_btn_text_.c_str(), -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (dis->itemState & ODS_FOCUS) DrawFocusRect(dis->hDC, &dis->rcItem);
}

void HomRecMainWindow::OnEraseBkgnd(HDC /*hdc*/) {
    // No-op: OnPaint() fills the entire client rect itself (bg + sidebar +
    // preview chrome + bottom bar), so letting DefWindowProc's default
    // erase run first would just cause visible flicker on resize.
}

void HomRecMainWindow::OnPaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT client;
    GetClientRect(hwnd_, &client);
    FillRect(hdc, &client, brushes_.bg);

    DrawLeftPanel(hdc);
    DrawPreviewChrome(hdc);
    RenderPreviewFrame(hdc);
    DrawBottomBar(hdc);

    EndPaint(hwnd_, &ps);
}

void HomRecMainWindow::OnSize(int width, int height) {
    ComputeLayout(width, height);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void HomRecMainWindow::OnHScroll(HWND ctrl, int pos) {
    if (audio_panel_) audio_panel_->OnHScroll(ctrl, pos);
}

void HomRecMainWindow::OnDrawItem(DRAWITEMSTRUCT *dis) {
    if (dis->hwndItem == start_btn_) { DrawStartButton(dis); return; }
    if (dis->hwndItem == pause_btn_) { DrawPauseButton(dis); return; }
    if (audio_panel_) audio_panel_->HandleDrawItem(dis);
}

void HomRecMainWindow::OnTimer(UINT_PTR id) {
    if (id == kPreviewTimerId) {
        if (state_.recording) InvalidateRect(hwnd_, &preview_rect_, FALSE);
    } else if (id == kStatsTimerId) {
        if (rec_) rec_->PollStats();
        if (audio_panel_) audio_panel_->PollLevels();

        // Matches ui_mixin.py's _update_stats(): time_label/fps_label/
        // res_label/file_label all get re-.config()'d here; we just update
        // the fields DrawLeftPanel()/DrawPreviewChrome()/DrawBottomBar()
        // read, then repaint.
        if (state_.recording) {
            time_text_ = rec_ ? rec_->elapsed_formatted() : L"00:00:00";
            {
                wchar_t fpsBuf[16];
                swprintf(fpsBuf, 16, L"%.1f", rec_ ? rec_->current_fps() : 0.0);
                fps_text_ = WideFromNarrow(lang_.Get("fps")) + L" " + fpsBuf;
            }
            res_text_ = WideFromNarrow(lang_.Get("resolution")) + L" " +
                        std::to_wstring(rec_ ? rec_->capture_width() : 0) + L"x" +
                        std::to_wstring(rec_ ? rec_->capture_height() : 0);
            file_label_text_ = state_.paused
                ? (WideFromNarrow(lang_.Get("paused")) + L" \u2014 " + time_text_)
                : (WideFromNarrow(lang_.Get("recording")) + L" \u2014 " + time_text_);
        } else {
            fps_text_.clear();
            res_text_.clear();
        }
        InvalidateRect(hwnd_, &left_panel_rect_, FALSE);
        InvalidateRect(hwnd_, &bottom_bar_rect_, FALSE);
        InvalidateRect(hwnd_, &preview_header_rect_, FALSE);
    }
}


void HomRecMainWindow::OnCommand(int id) {
    switch (id) {
        case ID_FILE_EXIT:
        case ID_TRAY_EXIT:
            state_.minimize_to_tray = false; // force a real close even if minimize-to-tray is on
            DestroyWindow(hwnd_);
            break;
        case ID_FILE_OPEN_RECORDINGS:
            if (!state_.output_folder.empty()) {
                ShellExecuteA(hwnd_, "open", state_.output_folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        case ID_VIEW_ALWAYS_ON_TOP:
            ToggleAlwaysOnTop();
            break;
        case ID_VIEW_FULLSCREEN:
            ToggleFullscreen();
            break;
        case ID_THEME_DARK:
            state_.current_theme = "dark";
            theme_ = GetBuiltinTheme("dark");
            ApplyTheme();
            break;
        case ID_THEME_LIGHT:
            state_.current_theme = "light";
            theme_ = GetBuiltinTheme("light");
            ApplyTheme();
            break;
        case ID_SETTINGS_OPEN:
            ShowSettingsDialog(hwnd_, hInstance_, state_);
            break;
        case ID_SETTINGS_ADVANCED:
            ShowAdvancedSettingsDialog(hwnd_, hInstance_, state_);
            // Hotkey strings may have just changed; RegisterHotKey only
            // happens once at hr_hk_start(), so re-apply by stopping and
            // restarting the manager rather than trying to hot-swap live
            // (see hr_hk_configure()'s doc comment in hr_hotkey.cpp).
            if (hotkey_handle_) {
                hr_hk_stop(hotkey_handle_);
                hr_hk_destroy(hotkey_handle_);
                hotkey_handle_ = nullptr;
            }
            SetupHotkeys();
            break;
        case ID_OVERLAYS_MANAGE:
            ShowOverlayManager(hwnd_, hInstance_, state_);
            if (overlays_panel_) overlays_panel_->Refresh(); // manager mutates the same state_.overlays vector
            break;
        case ID_FILE_SELECT_WINDOW:
            ShowWindowPickerDialog(hwnd_, hInstance_, state_);
            break;
        case ID_VIEW_PC_ANALYTICS:
            ShowPcAnalyticsDialog(hwnd_, hInstance_, state_.output_folder);
            break;
        case ID_VIEW_LOG:
            ShowLogViewerDialog(hwnd_, hInstance_);
            break;
        case ID_VIEW_OVERLAYS_PANEL: {
            state_.show_overlays_panel = !state_.show_overlays_panel;
            if (overlays_panel_) overlays_panel_->SetVisible(state_.show_overlays_panel);
            CheckMenuItem(menu_, ID_VIEW_OVERLAYS_PANEL,
                          MF_BYCOMMAND | (state_.show_overlays_panel ? MF_CHECKED : MF_UNCHECKED));
            RECT client; GetClientRect(hwnd_, &client);
            OnSize(client.right, client.bottom); // recompute preview_rect_ for the new margin
            break;
        }
        case ID_HELP_CONSOLE:
            if (!console_) console_ = std::make_unique<ConsoleWindow>(state_, rec_.get(), hwnd_);
            console_->Show(hInstance_);
            break;
        case ID_HELP_WELCOME:
            ShowWelcomeDialog(hwnd_, hInstance_);
            break;
        case ID_HELP_ABOUT:
            MessageBoxA(hwnd_, "HomRec " HR_APP_VERSION, "About", MB_OK);
            break;
        case ID_TRAY_RESTORE:
            ShowWindow(hwnd_, SW_SHOW);
            SetForegroundWindow(hwnd_);
            break;
        case ID_START_BTN:
            if (state_.recording) DoStop(); else DoStart();
            break;
        case ID_PAUSE_BTN:
            DoPause();
            break;
        default:
            if (audio_panel_) audio_panel_->OnCommand(id);
            if (overlays_panel_) overlays_panel_->OnCommand(id);
            break;
    }
}

void HomRecMainWindow::OnDestroy() {
    if (state_.recording && rec_) rec_->Stop();
    KillTimer(hwnd_, kPreviewTimerId);
    KillTimer(hwnd_, kStatsTimerId);
    RemoveTrayIcon();
    // Doing real cleanup here rather than relying solely on the destructor:
    // win_main.cpp never calls `delete` on the HomRecMainWindow it creates
    // (the process exits right after the message loop ends), so the
    // destructor's cleanup would otherwise be dead code that never runs.
    if (hotkey_handle_) {
        hr_hk_stop(hotkey_handle_);
        hr_hk_destroy(hotkey_handle_);
        hotkey_handle_ = nullptr;
    }
    PostQuitMessage(0);
}
