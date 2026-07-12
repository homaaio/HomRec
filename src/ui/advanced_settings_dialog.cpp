#include "advanced_settings_dialog.h"
#include <commctrl.h>
#include <string>

namespace {

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
    IDC_TAB = 4001,
    IDC_CODEC, IDC_HWACCEL, IDC_PRESET, IDC_CRF, IDC_PIXFMT, IDC_CUSTOM_ARGS,
    IDC_HK_STARTSTOP, IDC_HK_PAUSE, IDC_HK_FULLSCREEN,
    IDC_SAMPLE_RATE, IDC_AAC_BITRATE, IDC_CHANNELS, IDC_FNAME_TEMPLATE,
    IDC_AUTOSTOP, IDC_REPLAY_BUF, IDC_SEPARATE_MP3,
    IDC_SAVE, IDC_CANCEL,
};

struct Page {
    HWND hwnd = nullptr;
};

struct DialogCtx {
    AppState *state;
    HWND tab = nullptr;
    Page pages[3];
    bool saved = false;

    HWND codec_edit, hwaccel_edit, preset_edit, crf_edit, pixfmt_edit, custom_args_edit;
    HWND hk_startstop, hk_pause, hk_fullscreen;
    HWND sample_rate_edit, aac_bitrate_edit, channels_edit, fname_template_edit,
         autostop_edit, replay_buf_edit;
    HWND separate_mp3_chk;
};

void ShowPage(DialogCtx *ctx, int index) {
    for (int i = 0; i < 3; ++i)
        if (ctx->pages[i].hwnd) ShowWindow(ctx->pages[i].hwnd, i == index ? SW_SHOW : SW_HIDE);
}

HWND MakeLabel(HWND parent, HINSTANCE hInst, const wchar_t *text, int x, int y, int w = 120) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, 20, parent, nullptr, hInst, nullptr);
}
HWND MakeEdit(HWND parent, HINSTANCE hInst, const std::wstring &text, int id, int x, int y, int w = 160) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE,
                            x, y, w, 22, parent, (HMENU)(INT_PTR)id, hInst, nullptr);
}

LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = reinterpret_cast<DialogCtx *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_NOTIFY: {
            auto *nm = reinterpret_cast<NMHDR *>(lParam);
            if (ctx && nm->hwndFrom == ctx->tab && nm->code == TCN_SELCHANGE) {
                ShowPage(ctx, TabCtrl_GetCurSel(ctx->tab));
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            // Deliberately NOT calling PostQuitMessage — see settings_dialog.cpp
            // for why (nested modal loop shares the thread's message queue).
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_SAVE) {
                wchar_t buf[512] = {};
                auto readEdit = [&](HWND e) -> std::string {
                    GetWindowTextW(e, buf, 512);
                    return NarrowFromWide(buf);
                };
                auto readInt = [&](HWND e) -> int {
                    GetWindowTextW(e, buf, 512);
                    return _wtoi(buf);
                };

                ctx->state->video_codec        = readEdit(ctx->codec_edit);
                ctx->state->hw_accel           = readEdit(ctx->hwaccel_edit);
                ctx->state->enc_preset         = readEdit(ctx->preset_edit);
                ctx->state->enc_crf            = readInt(ctx->crf_edit);
                ctx->state->pix_fmt            = readEdit(ctx->pixfmt_edit);
                ctx->state->custom_ffmpeg_args = readEdit(ctx->custom_args_edit);

                ctx->state->hotkey_start_stop  = readEdit(ctx->hk_startstop);
                ctx->state->hotkey_pause       = readEdit(ctx->hk_pause);
                ctx->state->hotkey_fullscreen  = readEdit(ctx->hk_fullscreen);

                ctx->state->audio_sample_rate  = readInt(ctx->sample_rate_edit);
                ctx->state->audio_aac_bitrate  = readEdit(ctx->aac_bitrate_edit);
                ctx->state->audio_out_channels = readInt(ctx->channels_edit);
                ctx->state->filename_template  = readEdit(ctx->fname_template_edit);
                ctx->state->auto_stop_min      = readInt(ctx->autostop_edit);
                ctx->state->replay_buffer_sec  = readInt(ctx->replay_buf_edit);
                ctx->state->separate_audio_mp3 =
                    (SendMessageW(ctx->separate_mp3_chk, BM_GETCHECK, 0, 0) == BST_CHECKED);

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

bool ShowAdvancedSettingsDialog(HWND parent, HINSTANCE hInst, AppState &state) {
    static const wchar_t kClassName[] = L"HomRecAdvancedSettingsDialog";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DlgProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClassName;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);

    DialogCtx ctx = {};
    ctx.state = &state;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, L"Advanced Settings",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 460, 420,
                                 parent, nullptr, hInst, &ctx);

    ctx.tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE,
                               8, 8, 428, 320, hwnd, (HMENU)IDC_TAB, hInst, nullptr);
    TCITEMW tie = {};
    tie.mask = TCIF_TEXT;
    tie.pszText = (LPWSTR)L"Video / Codec"; TabCtrl_InsertItem(ctx.tab, 0, &tie);
    tie.pszText = (LPWSTR)L"Hotkeys";       TabCtrl_InsertItem(ctx.tab, 1, &tie);
    tie.pszText = (LPWSTR)L"Audio / Misc";  TabCtrl_InsertItem(ctx.tab, 2, &tie);

    // --- Page 0: Video / Codec ------------------------------------------------
    ctx.pages[0].hwnd = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                         16, 36, 408, 284, hwnd, nullptr, hInst, nullptr);
    HWND p0 = ctx.pages[0].hwnd;
    int y = 8;
    MakeLabel(p0, hInst, L"Video codec:", 8, y);
    ctx.codec_edit = MakeEdit(p0, hInst, WideFromNarrow(state.video_codec), IDC_CODEC, 140, y);
    y += 30;
    MakeLabel(p0, hInst, L"HW accel:", 8, y);
    ctx.hwaccel_edit = MakeEdit(p0, hInst, WideFromNarrow(state.hw_accel), IDC_HWACCEL, 140, y);
    y += 30;
    MakeLabel(p0, hInst, L"Encoder preset:", 8, y);
    ctx.preset_edit = MakeEdit(p0, hInst, WideFromNarrow(state.enc_preset), IDC_PRESET, 140, y);
    y += 30;
    MakeLabel(p0, hInst, L"CRF:", 8, y);
    ctx.crf_edit = MakeEdit(p0, hInst, std::to_wstring(state.enc_crf), IDC_CRF, 140, y, 60);
    y += 30;
    MakeLabel(p0, hInst, L"Pixel format:", 8, y);
    ctx.pixfmt_edit = MakeEdit(p0, hInst, WideFromNarrow(state.pix_fmt), IDC_PIXFMT, 140, y);
    y += 30;
    MakeLabel(p0, hInst, L"Custom FFmpeg args:", 8, y, 300);
    y += 22;
    ctx.custom_args_edit = MakeEdit(p0, hInst, WideFromNarrow(state.custom_ffmpeg_args), IDC_CUSTOM_ARGS, 8, y, 380);

    // --- Page 1: Hotkeys -------------------------------------------------------
    ctx.pages[1].hwnd = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
                                         16, 36, 408, 284, hwnd, nullptr, hInst, nullptr);
    HWND p1 = ctx.pages[1].hwnd;
    y = 8;
    MakeLabel(p1, hInst, L"Start/Stop:", 8, y);
    ctx.hk_startstop = MakeEdit(p1, hInst, WideFromNarrow(state.hotkey_start_stop), IDC_HK_STARTSTOP, 140, y, 100);
    y += 30;
    MakeLabel(p1, hInst, L"Pause:", 8, y);
    ctx.hk_pause = MakeEdit(p1, hInst, WideFromNarrow(state.hotkey_pause), IDC_HK_PAUSE, 140, y, 100);
    y += 30;
    MakeLabel(p1, hInst, L"Fullscreen:", 8, y);
    ctx.hk_fullscreen = MakeEdit(p1, hInst, WideFromNarrow(state.hotkey_fullscreen), IDC_HK_FULLSCREEN, 140, y, 100);

    // --- Page 2: Audio / Misc ---------------------------------------------------
    ctx.pages[2].hwnd = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
                                         16, 36, 408, 284, hwnd, nullptr, hInst, nullptr);
    HWND p2 = ctx.pages[2].hwnd;
    y = 8;
    MakeLabel(p2, hInst, L"Sample rate:", 8, y);
    ctx.sample_rate_edit = MakeEdit(p2, hInst, std::to_wstring(state.audio_sample_rate), IDC_SAMPLE_RATE, 140, y, 80);
    y += 30;
    MakeLabel(p2, hInst, L"AAC bitrate:", 8, y);
    ctx.aac_bitrate_edit = MakeEdit(p2, hInst, WideFromNarrow(state.audio_aac_bitrate), IDC_AAC_BITRATE, 140, y, 80);
    y += 30;
    MakeLabel(p2, hInst, L"Channels:", 8, y);
    ctx.channels_edit = MakeEdit(p2, hInst, std::to_wstring(state.audio_out_channels), IDC_CHANNELS, 140, y, 60);
    y += 30;
    MakeLabel(p2, hInst, L"Filename template:", 8, y, 300);
    y += 22;
    ctx.fname_template_edit = MakeEdit(p2, hInst, WideFromNarrow(state.filename_template), IDC_FNAME_TEMPLATE, 8, y, 380);
    y += 30;
    MakeLabel(p2, hInst, L"Auto-stop (min, 0=off):", 8, y, 160);
    ctx.autostop_edit = MakeEdit(p2, hInst, std::to_wstring(state.auto_stop_min), IDC_AUTOSTOP, 200, y, 60);
    y += 30;
    MakeLabel(p2, hInst, L"Replay buffer (sec, 0=off):", 8, y, 180);
    ctx.replay_buf_edit = MakeEdit(p2, hInst, std::to_wstring(state.replay_buffer_sec), IDC_REPLAY_BUF, 200, y, 60);
    y += 34;
    ctx.separate_mp3_chk = CreateWindowExW(0, L"BUTTON", L"Save audio as separate MP3",
                                            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                            8, y, 260, 22, p2, (HMENU)IDC_SEPARATE_MP3, hInst, nullptr);
    SendMessageW(ctx.separate_mp3_chk, BM_SETCHECK, state.separate_audio_mp3 ? BST_CHECKED : BST_UNCHECKED, 0);

    ShowPage(&ctx, 0);

    CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                     260, 344, 80, 26, hwnd, (HMENU)IDC_SAVE, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     350, 344, 80, 26, hwnd, (HMENU)IDC_CANCEL, hInst, nullptr);

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

    return ctx.saved;
}
