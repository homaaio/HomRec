#include "settings_dialog.h"
#include <commctrl.h>
#include <shlobj.h>
#include <string>

extern "C" {
    void *hr_settings_create();
    void hr_settings_destroy(void *handle);
    int hr_settings_load(void *handle, const char *path);
    int hr_settings_save(const void *handle, const char *path);
    void hr_settings_set_output_folder(void *h, const char *v);
    void hr_settings_set_quality(void *h, int v);
    void hr_settings_set_fps(void *h, int v);
    void hr_settings_set_monitor(void *h, int v);
    void hr_settings_set_flag(void *h, const char *name, int v);
}

namespace {
constexpr wchar_t kSettingsPath[] = L"homrec_settings.json"; // relative to app root, same as SETTINGS_PATH in constants.py

struct DialogCtx {
    AppState *state;
    void *settings;
    HWND quality_slider, fps_edit, monitor_edit, folder_edit;
    HWND countdown_chk, timestamp_chk, cursor_chk, notify_chk;
    bool saved = false;
};

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

enum {
    IDC_QUALITY = 3001, IDC_FPS, IDC_MONITOR, IDC_FOLDER, IDC_BROWSE,
    IDC_COUNTDOWN, IDC_TIMESTAMP, IDC_CURSOR, IDC_NOTIFY,
    IDC_SAVE, IDC_CANCEL,
};

LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = reinterpret_cast<DialogCtx *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            // NOTE: deliberately NOT calling PostQuitMessage here. This is a
            // nested modal loop sharing the same thread's message queue as
            // the main window's WinMain loop — posting WM_QUIT here would
            // leak into that outer loop and quit the whole app the moment
            // Settings is closed. The `while (IsWindow(hwnd) && ...)` loop
            // below exits on its own once the window is gone.
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_BROWSE) {
                wchar_t path[MAX_PATH] = {};
                BROWSEINFOW bi = {};
                bi.hwndOwner = hwnd;
                bi.lpszTitle = L"Select output folder";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                if (pidl) {
                    SHGetPathFromIDListW(pidl, path);
                    SetWindowTextW(ctx->folder_edit, path);
                    CoTaskMemFree(pidl);
                }
            } else if (id == IDC_SAVE) {
                wchar_t buf[512] = {};
                GetWindowTextW(ctx->folder_edit, buf, 512);
                ctx->state->output_folder = NarrowFromWide(buf);
                hr_settings_set_output_folder(ctx->settings, ctx->state->output_folder.c_str());

                ctx->state->quality = (int)SendMessageW(ctx->quality_slider, TBM_GETPOS, 0, 0);
                hr_settings_set_quality(ctx->settings, ctx->state->quality);

                GetWindowTextW(ctx->fps_edit, buf, 512);
                ctx->state->target_fps = _wtoi(buf);
                hr_settings_set_fps(ctx->settings, ctx->state->target_fps);

                GetWindowTextW(ctx->monitor_edit, buf, 512);
                ctx->state->monitor_id = _wtoi(buf);
                hr_settings_set_monitor(ctx->settings, ctx->state->monitor_id);

                ctx->state->countdown_enabled = (SendMessageW(ctx->countdown_chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->state->timestamp_enabled = (SendMessageW(ctx->timestamp_chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->state->cursor_enabled    = (SendMessageW(ctx->cursor_chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->state->show_summary      = (SendMessageW(ctx->notify_chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                hr_settings_set_flag(ctx->settings, "countdown", ctx->state->countdown_enabled ? 1 : 0);
                hr_settings_set_flag(ctx->settings, "timestamp", ctx->state->timestamp_enabled ? 1 : 0);
                hr_settings_set_flag(ctx->settings, "cursor", ctx->state->cursor_enabled ? 1 : 0);
                hr_settings_set_flag(ctx->settings, "show_summary", ctx->state->show_summary ? 1 : 0);

                hr_settings_save(ctx->settings, NarrowFromWide(kSettingsPath).c_str());
                ctx->saved = true;
                DestroyWindow(hwnd);
            } else if (id == IDC_CANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

bool ShowSettingsDialog(HWND parent, HINSTANCE hInst, AppState &state) {
    static const wchar_t kClassName[] = L"HomRecSettingsDialog";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DlgProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClassName;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc); // fine to call repeatedly; ERROR_CLASS_ALREADY_EXISTS is harmless here

    DialogCtx ctx = {};
    ctx.state = &state;
    ctx.settings = hr_settings_create();
    hr_settings_load(ctx.settings, NarrowFromWide(kSettingsPath).c_str());

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, L"Settings",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 420, 360,
                                 parent, nullptr, hInst, &ctx);

    int y = 16;
    CreateWindowExW(0, L"STATIC", L"Quality:", WS_CHILD | WS_VISIBLE, 16, y, 80, 20, hwnd, nullptr, hInst, nullptr);
    ctx.quality_slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ,
                                          100, y, 280, 24, hwnd, (HMENU)IDC_QUALITY, hInst, nullptr);
    SendMessageW(ctx.quality_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(ctx.quality_slider, TBM_SETPOS, TRUE, state.quality);

    y += 36;
    CreateWindowExW(0, L"STATIC", L"Target FPS:", WS_CHILD | WS_VISIBLE, 16, y, 80, 20, hwnd, nullptr, hInst, nullptr);
    ctx.fps_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(state.target_fps).c_str(),
                                    WS_CHILD | WS_VISIBLE, 100, y, 60, 22, hwnd, (HMENU)IDC_FPS, hInst, nullptr);

    y += 34;
    CreateWindowExW(0, L"STATIC", L"Monitor:", WS_CHILD | WS_VISIBLE, 16, y, 80, 20, hwnd, nullptr, hInst, nullptr);
    ctx.monitor_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(state.monitor_id).c_str(),
                                        WS_CHILD | WS_VISIBLE, 100, y, 60, 22, hwnd, (HMENU)IDC_MONITOR, hInst, nullptr);

    y += 34;
    CreateWindowExW(0, L"STATIC", L"Output folder:", WS_CHILD | WS_VISIBLE, 16, y, 90, 20, hwnd, nullptr, hInst, nullptr);
    ctx.folder_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", WideFromNarrow(state.output_folder).c_str(),
                                       WS_CHILD | WS_VISIBLE, 16, y + 22, 300, 22, hwnd, nullptr, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Browse", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    322, y + 22, 60, 22, hwnd, (HMENU)IDC_BROWSE, hInst, nullptr);

    y += 60;
    ctx.countdown_chk = CreateWindowExW(0, L"BUTTON", L"Countdown (3s)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                         16, y, 180, 22, hwnd, (HMENU)IDC_COUNTDOWN, hInst, nullptr);
    SendMessageW(ctx.countdown_chk, BM_SETCHECK, state.countdown_enabled ? BST_CHECKED : BST_UNCHECKED, 0);

    y += 26;
    ctx.timestamp_chk = CreateWindowExW(0, L"BUTTON", L"Timestamp", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                         16, y, 180, 22, hwnd, (HMENU)IDC_TIMESTAMP, hInst, nullptr);
    SendMessageW(ctx.timestamp_chk, BM_SETCHECK, state.timestamp_enabled ? BST_CHECKED : BST_UNCHECKED, 0);

    y += 26;
    ctx.cursor_chk = CreateWindowExW(0, L"BUTTON", L"Cursor", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                      16, y, 180, 22, hwnd, (HMENU)IDC_CURSOR, hInst, nullptr);
    SendMessageW(ctx.cursor_chk, BM_SETCHECK, state.cursor_enabled ? BST_CHECKED : BST_UNCHECKED, 0);

    y += 26;
    ctx.notify_chk = CreateWindowExW(0, L"BUTTON", L"Show summary", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                      16, y, 180, 22, hwnd, (HMENU)IDC_NOTIFY, hInst, nullptr);
    SendMessageW(ctx.notify_chk, BM_SETCHECK, state.show_summary ? BST_CHECKED : BST_UNCHECKED, 0);

    y += 36;
    CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                     220, y, 80, 26, hwnd, (HMENU)IDC_SAVE, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     310, y, 80, 26, hwnd, (HMENU)IDC_CANCEL, hInst, nullptr);

    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(hwnd)) break;
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    bool saved = ctx.saved;
    hr_settings_destroy(ctx.settings);
    return saved;
}
