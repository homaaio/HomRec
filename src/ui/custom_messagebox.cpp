#include "custom_messagebox.h"

namespace {

enum { IDC_YES = 8001, IDC_MSGBOX_NO, IDC_DONTSHOW };

struct MsgCtx {
    const ThemeColors *colors;
    ThemeBrushes brushes;
    std::wstring headline, info_text;
    HWND dontshow_chk = nullptr;
    bool result = false;
    bool dontshow_result = false;
};

LRESULT CALLBACK MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = reinterpret_cast<MsgCtx *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_CREATE:
            ctx->brushes.Rebuild(*ctx->colors);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT client; GetClientRect(hwnd, &client);
            FillRect(hdc, &client, ctx->brushes.bg);

            RECT topBar = { 0, 0, client.right, 6 };
            HBRUSH successBrush = CreateSolidBrush(ctx->colors->success);
            FillRect(hdc, &topBar, successBrush);
            DeleteObject(successBrush);

            SetBkMode(hdc, TRANSPARENT);
            HFONT bigFont = CreateFontW(-36, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(hdc, bigFont);
            SetTextColor(hdc, ctx->colors->success);
            TextOutW(hdc, 24, 26, L"\u2705", 1);
            SelectObject(hdc, oldFont);
            DeleteObject(bigFont);

            HFONT titleFont = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            oldFont = (HFONT)SelectObject(hdc, titleFont);
            SetTextColor(hdc, ctx->colors->text);
            TextOutW(hdc, 78, 30, ctx->headline.c_str(), (int)ctx->headline.size());
            SelectObject(hdc, oldFont);
            DeleteObject(titleFont);

            SetTextColor(hdc, ctx->colors->text_secondary);
            TextOutW(hdc, 78, 54, L"Recording complete", 19);

            RECT infoRect = { 20, 96, client.right - 20, client.bottom - 100 };
            FillRect(hdc, &infoRect, ctx->brushes.surface);
            RECT infoTextRect = infoRect;
            InflateRect(&infoTextRect, -16, -12);
            HFONT monoFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
            oldFont = (HFONT)SelectObject(hdc, monoFont);
            SetTextColor(hdc, ctx->colors->text);
            DrawTextW(hdc, ctx->info_text.c_str(), -1, &infoTextRect, DT_LEFT | DT_WORDBREAK);
            SelectObject(hdc, oldFont);
            DeleteObject(monoFont);

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_YES || id == IDC_MSGBOX_NO) {
                // Capture checkbox state BEFORE destroying the window — once
                // DestroyWindow runs, this child HWND (and its BM_GETCHECK
                // state) is gone, so reading it afterward would silently
                // return 0/false instead of the user's actual choice.
                ctx->result = (id == IDC_YES);
                ctx->dontshow_result = (SendMessageW(ctx->dontshow_chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            return 0; // nested modal loop — no PostQuitMessage, see settings_dialog.cpp
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

bool ShowCustomMessageBox(HWND parent, HINSTANCE hInst, const ThemeColors &colors,
                           const std::wstring &title, const std::wstring &headline,
                           const std::wstring &info_text, bool &dont_show_again) {
    static const wchar_t kClass[] = L"HomRecCustomMessageBox";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = MsgProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    MsgCtx ctx;
    ctx.colors = &colors;
    ctx.headline = headline;
    ctx.info_text = info_text;

    const int W = 520, H = 420;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, title.c_str(),
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 (sw - W) / 2, (sh - H) / 2, W, H,
                                 parent, nullptr, hInst, &ctx);

    ctx.dontshow_chk = CreateWindowExW(0, L"BUTTON", L"Don't show again", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                        20, H - 130, 200, 22, hwnd, (HMENU)IDC_DONTSHOW, hInst, nullptr);
    SendMessageW(ctx.dontshow_chk, BM_SETCHECK, dont_show_again ? BST_CHECKED : BST_UNCHECKED, 0);

    CreateWindowExW(0, L"BUTTON", L"Open Folder", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                     100, H - 90, 140, 32, hwnd, (HMENU)IDC_YES, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     260, H - 90, 140, 32, hwnd, (HMENU)IDC_MSGBOX_NO, hInst, nullptr);

    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (!IsWindow(hwnd)) break;
    }
    dont_show_again = ctx.dontshow_result;
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    return ctx.result;
}
