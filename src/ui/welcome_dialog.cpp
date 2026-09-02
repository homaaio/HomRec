#include "welcome_dialog.h"
#include "version.h"
#include "win32_theme.h"
#include "hrc_config.h"
#include <string>
#include <shlobj.h>

// Phase 1 (see commands.md): persistence now goes through HrcConfig::Save
// (hrc_config.h), which writes the entire AppState in one shot - the old
// hr_settings_* JSON engine's fixed field whitelist is no longer touched
// from this wizard at all.

namespace {

enum {
    IDC_CHANGELOG = 7001, IDC_GITHUB, IDC_WEBSITE, IDC_GETSTARTED,
    IDC_BACK, IDC_NEXT, IDC_SKIP, IDC_BROWSE, IDC_FOLDER_EDIT,
    IDC_RES_COMBO, IDC_FPS_COMBO,
    IDT_PULSE = 1
};

constexpr int kResPct[] = { 100, 75, 50, 25 };
constexpr int kFpsOpt[] = { 15, 24, 30, 60 };

enum class Page { Greeting = 0, Settings = 1, Finish = 2 };

struct WelcomeCtx {
    AppState *state = nullptr;
    Page page = Page::Greeting;
    bool pulse_on = true;

    // Page 1 (basic settings)
    HWND hSkipChk = nullptr;
    HWND hFolderLbl = nullptr, hFolderEdit = nullptr, hBrowseBtn = nullptr;
    HWND hResLbl = nullptr, hResCombo = nullptr;
    HWND hFpsLbl = nullptr, hFpsCombo = nullptr;
    HWND hSettingsTitle = nullptr;

    // Page 2 (finish)
    HWND hFinishTitle = nullptr, hFinishBody = nullptr;
    HWND hChangelogBtn = nullptr, hGithubBtn = nullptr, hWebsiteBtn = nullptr;

    // Nav (shared across pages)
    HWND hBackBtn = nullptr, hNextBtn = nullptr;
};

// Applies the settings-page fields to *ctx->state and persists the three
// fields this wizard actually edits (folder/fps/resolution) - mirrors
// SettingsDialog::OnSave()'s load-existing/set-a-few/save round trip so
// nothing else in homrec_settings.json gets clobbered. Skipped entirely
// if the user checked "I understand" (defaults are kept as-is).
void ApplyAndPersistSettings(WelcomeCtx *ctx) {
    if (!ctx->state) return;
    if (SendMessageW(ctx->hSkipChk, BM_GETCHECK, 0, 0) == BST_CHECKED) return;

    wchar_t folderW[MAX_PATH] = {};
    GetWindowTextW(ctx->hFolderEdit, folderW, MAX_PATH);
    int len = WideCharToMultiByte(CP_UTF8, 0, folderW, -1, nullptr, 0, nullptr, nullptr);
    std::string folder(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, folderW, -1, folder.data(), len, nullptr, nullptr);
    if (!folder.empty()) ctx->state->output_folder = folder;

    int resSel = (int)SendMessageW(ctx->hResCombo, CB_GETCURSEL, 0, 0);
    if (resSel >= 0 && resSel < 4) ctx->state->scale_factor = kResPct[resSel] / 100.0;

    int fpsSel = (int)SendMessageW(ctx->hFpsCombo, CB_GETCURSEL, 0, 0);
    if (fpsSel >= 0 && fpsSel < 4) ctx->state->target_fps = kFpsOpt[fpsSel];

    // Phase 1 (see commands.md): persist the whole AppState via HrcConfig
    // instead of the old JSON engine's fixed field whitelist - this first-
    // run wizard is often the very first save an install ever makes, so
    // it's what actually creates the initial homrec.hrc.
    HrcConfig::Save(*ctx->state, HrcConfig::ResolveSettingsPath(*ctx->state));
}

void SetPageVisibility(WelcomeCtx *ctx, HWND hwnd) {
    bool onSettings = ctx->page == Page::Settings;
    bool onFinish = ctx->page == Page::Finish;

    for (HWND h : { ctx->hSettingsTitle, ctx->hSkipChk, ctx->hFolderLbl, ctx->hFolderEdit,
                     ctx->hBrowseBtn, ctx->hResLbl, ctx->hResCombo, ctx->hFpsLbl, ctx->hFpsCombo })
        ShowWindow(h, onSettings ? SW_SHOW : SW_HIDE);

    for (HWND h : { ctx->hFinishTitle, ctx->hFinishBody, ctx->hChangelogBtn, ctx->hGithubBtn, ctx->hWebsiteBtn })
        ShowWindow(h, onFinish ? SW_SHOW : SW_HIDE);

    ShowWindow(ctx->hBackBtn, ctx->page != Page::Greeting ? SW_SHOW : SW_HIDE);
    SetWindowTextW(ctx->hNextBtn, ctx->page == Page::Finish ? L"Get Started \u2192" : L"Next \u2192");

    InvalidateRect(hwnd, nullptr, TRUE);
}

LRESULT CALLBACK WelcomeProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = reinterpret_cast<WelcomeCtx *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_CREATE:
            SetTimer(hwnd, IDT_PULSE, 600, nullptr);
            return 0;
        case WM_TIMER:
            if (wParam == IDT_PULSE) {
                ctx->pulse_on = !ctx->pulse_on;
                // Only the header (top ~100px) actually changes on the pulse
                // tick - restricting the invalidated rect avoids repainting
                // (and re-drawing text into) the page content underneath it
                // 600ms out of every second for no reason.
                RECT header; GetClientRect(hwnd, &header); header.bottom = 100;
                InvalidateRect(hwnd, &header, FALSE);
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT client; GetClientRect(hwnd, &client);

            HBRUSH bgBrush = CreateSolidBrush(RGB(0x0f, 0x0f, 0x17));
            FillRect(hdc, &client, bgBrush);
            DeleteObject(bgBrush);

            RECT header = { 0, 0, client.right, 100 };
            HBRUSH cardBrush = CreateSolidBrush(RGB(0x1a, 0x1a, 0x2e));
            FillRect(hdc, &header, cardBrush);
            DeleteObject(cardBrush);

            HBRUSH ringBrush = CreateSolidBrush(RGB(0x18, 0x18, 0x30));
            HPEN accentPen = CreatePen(PS_SOLID, 2, RGB(0x89, 0xb4, 0xfa));
            HGDIOBJ oldBrush = SelectObject(hdc, ringBrush);
            HGDIOBJ oldPen = SelectObject(hdc, accentPen);
            Ellipse(hdc, 18, 14, 82, 78);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(ringBrush);
            DeleteObject(accentPen);

            HBRUSH dotBrush = CreateSolidBrush(ctx->pulse_on ? RGB(0xf3, 0x8b, 0xa8) : RGB(0xa0, 0x20, 0x3a));
            oldBrush = SelectObject(hdc, dotBrush);
            HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
            oldPen = SelectObject(hdc, nullPen);
            Ellipse(hdc, 38, 34, 62, 58);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(dotBrush);

            SetBkMode(hdc, TRANSPARENT);
            static HFONT titleFont = CreateFontW(-26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            static HFONT subFont = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

            HFONT oldFont = (HFONT)SelectObject(hdc, titleFont);
            SetTextColor(hdc, RGB(0x89, 0xb4, 0xfa));
            TextOutW(hdc, 100, 18, L"HomRec", 6);
            SelectObject(hdc, subFont);
            SetTextColor(hdc, RGB(0xa6, 0xad, 0xc8));
            std::wstring verLine = L"Screen Recorder  v" HR_APP_VERSION_W;
            TextOutW(hdc, 100, 54, verLine.c_str(), (int)verLine.size());
            SetTextColor(hdc, RGB(0x45, 0x47, 0x5a));
            TextOutW(hdc, 100, 72, L"by homaaio", 10);
            SelectObject(hdc, oldFont);

            // Page 0 body (page 1/2 bodies are real child controls, shown/
            // hidden by SetPageVisibility() instead - only the greeting page
            // is static enough to just paint directly).
            if (ctx->page == Page::Greeting) {
                static HFONT bodyBold = CreateFontW(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                oldFont = (HFONT)SelectObject(hdc, bodyBold);
                SetTextColor(hdc, RGB(0xcd, 0xd6, 0xf4));
                TextOutW(hdc, 28, 132, L"Hello, and thanks for choosing HomRec!", 39);
                SelectObject(hdc, oldFont);

                RECT msgRect = { 28, 168, client.right - 28, 300 };
                SetTextColor(hdc, RGB(0xa6, 0xad, 0xc8));
                std::wstring msg =
                    L"This quick setup takes a few seconds and helps HomRec "
                    L"record at settings that actually fit your machine.\n\n"
                    L"Click Next to choose where recordings are saved and "
                    L"pick a resolution/fps - or check \"I understand\" to "
                    L"skip straight past and just use the defaults. Desktop "
                    L"shortcut, startup, and tray options are always "
                    L"available later in Settings > System.";
                DrawTextW(hdc, msg.c_str(), -1, &msgRect, DT_LEFT | DT_WORDBREAK);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            return (LRESULT)HrWin32Theme::ColorStatic((HDC)wParam);
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return (LRESULT)HrWin32Theme::ColorEdit((HDC)wParam);
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            switch (id) {
                case IDC_SKIP: {
                    bool skip = SendMessageW(ctx->hSkipChk, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    for (HWND h : { ctx->hFolderEdit, ctx->hBrowseBtn, ctx->hResCombo, ctx->hFpsCombo })
                        EnableWindow(h, !skip);
                    break;
                }
                case IDC_BROWSE: {
                    wchar_t path[MAX_PATH] = {};
                    BROWSEINFOW bi = {};
                    bi.hwndOwner = hwnd;
                    bi.lpszTitle = L"Choose where recordings are saved";
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                    if (pidl) {
                        if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(ctx->hFolderEdit, path);
                        CoTaskMemFree(pidl);
                    }
                    break;
                }
                case IDC_BACK:
                    if (ctx->page != Page::Greeting) {
                        ctx->page = (Page)((int)ctx->page - 1);
                        SetPageVisibility(ctx, hwnd);
                    }
                    break;
                case IDC_NEXT:
                    if (ctx->page == Page::Settings) ApplyAndPersistSettings(ctx);
                    if (ctx->page == Page::Finish) { DestroyWindow(hwnd); break; }
                    ctx->page = (Page)((int)ctx->page + 1);
                    SetPageVisibility(ctx, hwnd);
                    break;
                case IDC_CHANGELOG:
                    ShellExecuteW(hwnd, L"open", L"https://github.com/homaaio/HomREC/blob/main/CHANGELOG.txt",
                                  nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                case IDC_GITHUB:
                    ShellExecuteW(hwnd, L"open", L"https://github.com/homaaio/HomREC", nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                case IDC_WEBSITE:
                    ShellExecuteW(hwnd, L"open", L"https://homaaio.github.io/HomREC/", nullptr, nullptr, SW_SHOWNORMAL);
                    break;
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, IDT_PULSE);
            return 0; // nested modal loop - no PostQuitMessage, see settings_dialog.cpp
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

void ShowWelcomeDialog(HWND parent, HINSTANCE hInst, AppState &state) {
    static const wchar_t kClass[] = L"HomRecWelcomeDialog";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WelcomeProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    const int W = 580, H = 470;

    WelcomeCtx ctx;
    ctx.state = &state;
    int wx, wy, ww, wh;
    HrWin32Theme::CenteredWindowRect(W, H, WS_POPUP | WS_CAPTION | WS_SYSMENU, wx, wy, ww, wh);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"Welcome to HomRec",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 wx, wy, ww, wh,
                                 parent, nullptr, hInst, &ctx);
    HrWin32Theme::ApplyDarkTitleBar(hwnd);

    // -- Page 1: basic settings --------------------------------------------
    ctx.hSettingsTitle = CreateWindowExW(0, L"STATIC", L"A couple of basic settings",
        WS_CHILD | SS_LEFT, 28, 118, W - 56, 22, hwnd, nullptr, hInst, nullptr);
    // static: ShowWelcomeDialog() can be re-entered (Help > Welcome), and
    // this same font object is reused by both page titles below - a fresh
    // CreateFontW() per call with no matching DeleteObject would leak one
    // GDI font handle every time the wizard is reopened.
    static HFONT boldFont = CreateFontW(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SendMessageW(ctx.hSettingsTitle, WM_SETFONT, (WPARAM)boldFont, TRUE);

    ctx.hFolderLbl = CreateWindowExW(0, L"STATIC", L"Save recordings to:",
        WS_CHILD | SS_LEFT, 28, 152, 200, 20, hwnd, nullptr, hInst, nullptr);
    ctx.hFolderEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 28, 174, W - 56 - 90, 24,
        hwnd, (HMENU)IDC_FOLDER_EDIT, hInst, nullptr);
    {
        int len = MultiByteToWideChar(CP_UTF8, 0, state.output_folder.c_str(), -1, nullptr, 0);
        std::wstring w(len > 0 ? len - 1 : 0, L'\0');
        if (len > 0) MultiByteToWideChar(CP_UTF8, 0, state.output_folder.c_str(), -1, w.data(), len);
        SetWindowTextW(ctx.hFolderEdit, w.empty() ? L"recordings" : w.c_str());
    }
    ctx.hBrowseBtn = CreateWindowExW(0, L"BUTTON", L"Browse\u2026", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        W - 56 - 80 + 28, 173, 80, 26, hwnd, (HMENU)IDC_BROWSE, hInst, nullptr);
    HrWin32Theme::ThemeButton(ctx.hBrowseBtn);

    ctx.hResLbl = CreateWindowExW(0, L"STATIC", L"Resolution:", WS_CHILD | SS_LEFT,
        28, 214, 140, 20, hwnd, nullptr, hInst, nullptr);
    ctx.hResCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        28, 236, 160, 200, hwnd, (HMENU)IDC_RES_COMBO, hInst, nullptr);
    SendMessageW(ctx.hResCombo, CB_ADDSTRING, 0, (LPARAM)L"100% (Native)");
    SendMessageW(ctx.hResCombo, CB_ADDSTRING, 0, (LPARAM)L"75%");
    SendMessageW(ctx.hResCombo, CB_ADDSTRING, 0, (LPARAM)L"50%");
    SendMessageW(ctx.hResCombo, CB_ADDSTRING, 0, (LPARAM)L"25%");
    {
        int pct = (int)(state.scale_factor * 100.0 + 0.5);
        int sel = pct >= 100 ? 0 : pct >= 75 ? 1 : pct >= 50 ? 2 : 3;
        SendMessageW(ctx.hResCombo, CB_SETCURSEL, sel, 0);
    }

    ctx.hFpsLbl = CreateWindowExW(0, L"STATIC", L"FPS:", WS_CHILD | SS_LEFT,
        220, 214, 140, 20, hwnd, nullptr, hInst, nullptr);
    ctx.hFpsCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        220, 236, 120, 200, hwnd, (HMENU)IDC_FPS_COMBO, hInst, nullptr);
    SendMessageW(ctx.hFpsCombo, CB_ADDSTRING, 0, (LPARAM)L"15");
    SendMessageW(ctx.hFpsCombo, CB_ADDSTRING, 0, (LPARAM)L"24");
    SendMessageW(ctx.hFpsCombo, CB_ADDSTRING, 0, (LPARAM)L"30");
    SendMessageW(ctx.hFpsCombo, CB_ADDSTRING, 0, (LPARAM)L"60");
    {
        int sel = 0, best = 1 << 30;
        for (int i = 0; i < 4; ++i) {
            int d = abs(kFpsOpt[i] - state.target_fps);
            if (d < best) { best = d; sel = i; }
        }
        SendMessageW(ctx.hFpsCombo, CB_SETCURSEL, sel, 0);
    }

    ctx.hSkipChk = CreateWindowExW(0, L"BUTTON",
        L"I understand - skip this and just use the defaults",
        WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, 28, 300, W - 56, 24,
        hwnd, (HMENU)IDC_SKIP, hInst, nullptr);

    // -- Page 2: finish -------------------------------------------------------
    ctx.hFinishTitle = CreateWindowExW(0, L"STATIC", L"You're all set!", WS_CHILD | SS_LEFT,
        28, 130, W - 56, 26, hwnd, nullptr, hInst, nullptr);
    SendMessageW(ctx.hFinishTitle, WM_SETFONT, (WPARAM)boldFont, TRUE);
    ctx.hFinishBody = CreateWindowExW(0, L"STATIC",
        L"Good luck recording! If you'd like a tour of what HomRec can do, "
        L"the documentation and changelog are one click away below - and "
        L"you can always re-open this wizard later from Help > Welcome.",
        WS_CHILD | SS_LEFT, 28, 164, W - 56, 90, hwnd, nullptr, hInst, nullptr);

    ctx.hChangelogBtn = CreateWindowExW(0, L"BUTTON", L"Changelog", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        28, 270, 100, 30, hwnd, (HMENU)IDC_CHANGELOG, hInst, nullptr);
    HrWin32Theme::ThemeButton(ctx.hChangelogBtn);
    ctx.hGithubBtn = CreateWindowExW(0, L"BUTTON", L"GitHub", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        134, 270, 90, 30, hwnd, (HMENU)IDC_GITHUB, hInst, nullptr);
    HrWin32Theme::ThemeButton(ctx.hGithubBtn);
    ctx.hWebsiteBtn = CreateWindowExW(0, L"BUTTON", L"Documentation", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        230, 270, 120, 30, hwnd, (HMENU)IDC_WEBSITE, hInst, nullptr);
    HrWin32Theme::ThemeButton(ctx.hWebsiteBtn);

    // -- Nav (every page) -------------------------------------------------
    ctx.hBackBtn = CreateWindowExW(0, L"BUTTON", L"\u2190 Back", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        24, H - 66, 100, 30, hwnd, (HMENU)IDC_BACK, hInst, nullptr);
    HrWin32Theme::ThemeButton(ctx.hBackBtn);
    ctx.hNextBtn = CreateWindowExW(0, L"BUTTON", L"Next \u2192", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        W - 154, H - 66, 130, 30, hwnd, (HMENU)IDC_NEXT, hInst, nullptr);
    HrWin32Theme::ThemeButton(ctx.hNextBtn);

    SetPageVisibility(&ctx, hwnd);

    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!IsWindow(hwnd)) break;
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}
