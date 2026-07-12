#include "main_window.h"
#include "version.h"
#include "settings_dialog.h"
#include "advanced_settings_dialog.h"
#include "overlay_manager.h"
#include "welcome_dialog.h"
#include <commctrl.h>
#include <windowsx.h>
#include <string>

extern "C" {
    int hr_acquire_single_instance(const char *mutex_name); // unused here, called once in win_main.cpp
    void *hr_hk_create();
    void hr_hk_destroy(void *handle);
    void hr_hk_set_callbacks(void *handle, void (*start_stop)(), void (*pause)(), void (*fullscreen)());
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
constexpr wchar_t kStatusClassName[] = L"STATIC";

// Only one main window exists per process — hotkey callbacks (plain
// function pointers, no user-data slot, see hr_hotkey.cpp) need a way back
// to "the" window, and this is it.
HomRecMainWindow *g_instance = nullptr;

std::string NarrowFromWide(const std::wstring &w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
std::wstring WideFromNarrow(const std::string &s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}
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
            // Fullscreen toggle: minimal version — maximize/restore. A true
            // borderless fullscreen (matching the Python app's behavior)
            // would also strip WS_OVERLAPPEDWINDOW; keeping this simple for
            // now and flagging it rather than silently under-delivering.
            ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto *mmi = reinterpret_cast<MINMAXINFO *>(lParam);
            mmi->ptMinTrackSize.x = state_.window_min_w;
            mmi->ptMinTrackSize.y = state_.window_min_h;
            return 0;
        }
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
    } else {
        state_.output_folder = "recordings"; // first run, no settings file yet
    }
    hr_settings_destroy(settings);

    BuildMenu();

    rec_ = std::make_unique<RecordingController>(state_);
    rec_->Initialize();

    audio_panel_ = std::make_unique<AudioPanel>(state_, *rec_);
    // Real position/size set in OnSize once the client rect is known;
    // create it small here so child HWNDs exist before first layout pass.
    audio_panel_->Create(hwnd_, hInstance_, 12, 420, 900, 90);

    status_label_ = CreateWindowExW(0, kStatusClassName, L"Ready",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     12, 12, 400, 24, hwnd_, nullptr, hInstance_, nullptr);

    BuildToolbar();
    SetupTrayIcon();
    SetupHotkeys();

    plugins_ = std::make_unique<LuaPluginEngine>("plugins");
    plugins_->SetContext(rec_.get(), &theme_);
    plugins_->LoadAll();

    ApplyLanguage();

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
    AppendMenuA(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(fileMenu, MF_STRING, ID_FILE_EXIT, "Exit");
    AppendMenuA(menu_, MF_POPUP, (UINT_PTR)fileMenu, "File");

    HMENU viewMenu = CreatePopupMenu();
    AppendMenuA(viewMenu, MF_STRING, ID_VIEW_ALWAYS_ON_TOP, "Always on Top");
    AppendMenuA(viewMenu, MF_STRING, ID_VIEW_FULLSCREEN, "Fullscreen\tF11");
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

void HomRecMainWindow::BuildToolbar() {
    start_btn_ = CreateWindowExW(0, L"BUTTON", L"\u25B6 START",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  12, 44, 120, 32, hwnd_, (HMENU)ID_START_BTN, hInstance_, nullptr);
    pause_btn_ = CreateWindowExW(0, L"BUTTON", L"\u23F8 PAUSE",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  140, 44, 120, 32, hwnd_, (HMENU)ID_PAUSE_BTN, hInstance_, nullptr);
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
    if (!hr_hk_start(hotkey_handle_)) {
        OutputDebugStringA("HomRec: global hotkeys (F9/F10/F11) failed to register — another app may be using them.\n");
    }
    // KNOWN GAP (see recording_controller / advanced_settings_dialog audit
    // notes): hr_hotkey.cpp hardcodes F9/F10/F11 — the custom key strings
    // in AppState.hotkey_start_stop/hotkey_pause/hotkey_fullscreen (editable
    // in Advanced Settings) aren't actually wired to anything yet. Making
    // them real requires extending hr_hotkey.cpp to accept a configurable
    // virtual-key instead of the fixed RegisterHotKey calls it makes today.
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
    std::wstring err;
    if (!rec_->Start(err)) {
        MessageBoxW(hwnd_, err.c_str(), L"HomRec", MB_OK | MB_ICONWARNING);
        return;
    }
    SetWindowTextW(start_btn_, L"\u25A0 STOP");
    EnableWindow(pause_btn_, TRUE);
    if (plugins_) plugins_->EmitHook("on_recording_start");
}

void HomRecMainWindow::DoStop() {
    rec_->Stop();
    SetWindowTextW(start_btn_, L"\u25B6 START");
    SetWindowTextW(pause_btn_, L"\u23F8 PAUSE");
    EnableWindow(pause_btn_, FALSE);
    if (plugins_) plugins_->EmitHook("on_recording_stop");
    // KNOWN GAP (unchanged from Phase 3): no "recording saved, open folder?"
    // CustomMessageBox popup wired here yet, and RecordingController::Stop()
    // still doesn't merge separate mic/system-audio WAVs into the output —
    // see recording_controller.cpp's Stop().
}

void HomRecMainWindow::DoPause() {
    if (!state_.recording) return;
    rec_->TogglePause();
    SetWindowTextW(pause_btn_, state_.paused ? L"\u25B6 RESUME" : L"\u23F8 PAUSE");
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

void HomRecMainWindow::RenderPreviewFrame(HDC hdc) {
    std::vector<uint8_t> frame;
    int w = 0, h = 0;
    if (!rec_ || !rec_->GetPreviewFrame(frame, w, h) || w <= 0 || h <= 0) {
        SetTextColor(hdc, theme_.text_secondary);
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, state_.recording ? L"Waiting for frames..." : L"Preview (start recording to see it)",
                  -1, &preview_rect_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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

void HomRecMainWindow::OnPaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT client;
    GetClientRect(hwnd_, &client);
    FillRect(hdc, &client, brushes_.bg);

    RenderPreviewFrame(hdc);

    EndPaint(hwnd_, &ps);
}

void HomRecMainWindow::OnSize(int width, int height) {
    RECT preview = { 12, 84, width - 12, height - 130 };
    preview_rect_ = preview;
    if (audio_panel_) {
        // AudioPanel doesn't expose a Resize() yet (see audio_panel.h) —
        // re-positioning its child controls here would need that method
        // added; for now it stays at its create-time rect. Flagging rather
        // than silently leaving this comment out.
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void HomRecMainWindow::OnHScroll(HWND ctrl, int pos) {
    if (audio_panel_) audio_panel_->OnHScroll(ctrl, pos);
}

void HomRecMainWindow::OnDrawItem(DRAWITEMSTRUCT *dis) {
    if (audio_panel_) audio_panel_->HandleDrawItem(dis);
}

void HomRecMainWindow::OnTimer(UINT_PTR id) {
    if (id == kPreviewTimerId) {
        if (state_.recording) InvalidateRect(hwnd_, &preview_rect_, FALSE);
    } else if (id == kStatsTimerId) {
        if (rec_) rec_->PollStats();
        if (audio_panel_) audio_panel_->PollLevels();
        if (status_label_) {
            std::wstring status = state_.recording
                ? (state_.paused ? L"Paused — " : L"Recording — ") +
                  (rec_ ? rec_->elapsed_formatted() : L"")
                : L"Ready";
            SetWindowTextW(status_label_, status.c_str());
        }
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
            ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
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
            break;
        case ID_OVERLAYS_MANAGE:
            ShowOverlayManager(hwnd_, hInstance_, state_);
            break;
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
