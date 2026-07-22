#include "welcome_dialog.h"
#include "version.h"
#include "win32_theme.h"
#include <string>

namespace {

enum { IDC_CHANGELOG = 7001, IDC_GITHUB, IDC_WEBSITE, IDC_GETSTARTED, IDT_PULSE = 1 };

struct WelcomeCtx {
    bool pulse_on = true;
};

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
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT client; GetClientRect(hwnd, &client);

            HBRUSH bgBrush = CreateSolidBrush(RGB(0x0f, 0x0f, 0x17));
            FillRect(hdc, &client, bgBrush);
            DeleteObject(bgBrush);

            RECT header = { 0, 0, client.right, 110 };
            HBRUSH cardBrush = CreateSolidBrush(RGB(0x1a, 0x1a, 0x2e));
            FillRect(hdc, &header, cardBrush);
            DeleteObject(cardBrush);

            HBRUSH ringBrush = CreateSolidBrush(RGB(0x18, 0x18, 0x30));
            HPEN accentPen = CreatePen(PS_SOLID, 2, RGB(0x89, 0xb4, 0xfa));
            HGDIOBJ oldBrush = SelectObject(hdc, ringBrush);
            HGDIOBJ oldPen = SelectObject(hdc, accentPen);
            Ellipse(hdc, 18, 18, 92, 92);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(ringBrush);
            DeleteObject(accentPen);

            HBRUSH dotBrush = CreateSolidBrush(ctx->pulse_on ? RGB(0xf3, 0x8b, 0xa8) : RGB(0xa0, 0x20, 0x3a));
            oldBrush = SelectObject(hdc, dotBrush);
            HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
            oldPen = SelectObject(hdc, nullPen);
            Ellipse(hdc, 43, 43, 67, 67);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(dotBrush);

            SetBkMode(hdc, TRANSPARENT);
            HFONT titleFont = CreateFontW(-28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT subFont = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

            HFONT oldFont = (HFONT)SelectObject(hdc, titleFont);
            SetTextColor(hdc, RGB(0x89, 0xb4, 0xfa));
            TextOutW(hdc, 110, 24, L"HomRec", 6);
            SelectObject(hdc, subFont);
            SetTextColor(hdc, RGB(0xa6, 0xad, 0xc8));
            std::wstring verLine = L"Screen Recorder  v" HR_APP_VERSION_W;
            TextOutW(hdc, 110, 62, verLine.c_str(), (int)verLine.size());
            SetTextColor(hdc, RGB(0x45, 0x47, 0x5a));
            TextOutW(hdc, 110, 82, L"by homaaio", 10);
            SelectObject(hdc, oldFont);
            DeleteObject(titleFont);
            DeleteObject(subFont);

            // Body text
            HFONT bodyBold = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            oldFont = (HFONT)SelectObject(hdc, bodyBold);
            SetTextColor(hdc, RGB(0xcd, 0xd6, 0xf4));
            TextOutW(hdc, 28, 150, L"Hello,", 6);
            SelectObject(hdc, oldFont);
            DeleteObject(bodyBold);

            RECT msgRect = { 28, 178, client.right - 28, 240 };
            SetTextColor(hdc, RGB(0xa6, 0xad, 0xc8));
            std::wstring msg = L"Welcome to HomRec! If you have any issues, reach out on GitHub.\n\nEnjoy. \u2014 homaaio";
            DrawTextW(hdc, msg.c_str(), -1, &msgRect, DT_LEFT | DT_WORDBREAK);

            RECT tipsRect = { 28, 252, client.right - 28, 300 };
            HBRUSH tipsBg = CreateSolidBrush(RGB(0x1a, 0x1a, 0x2e));
            FillRect(hdc, &tipsRect, tipsBg);
            DeleteObject(tipsBg);
            SetTextColor(hdc, RGB(0x89, 0xb4, 0xfa));
            TextOutW(hdc, 40, 258, L"Quick tips:", 11);
            SetTextColor(hdc, RGB(0xa6, 0xad, 0xc8));
            std::wstring tips = L"F9 = Start/Stop   F10 = Pause   F11 = Fullscreen   Ctrl+Shift+T = Console";
            TextOutW(hdc, 40, 278, tips.c_str(), (int)tips.size());

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            switch (id) {
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
                case IDC_GETSTARTED:
                    DestroyWindow(hwnd);
                    break;
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, IDT_PULSE);
            return 0; // nested modal loop — no PostQuitMessage, see settings_dialog.cpp
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

void ShowWelcomeDialog(HWND parent, HINSTANCE hInst) {
    static const wchar_t kClass[] = L"HomRecWelcomeDialog";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WelcomeProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    const int W = 580, H = 440;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    WelcomeCtx ctx;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"Welcome to HomRec",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 (sw - W) / 2, (sh - H) / 2, W, H,
                                 parent, nullptr, hInst, &ctx);
    HrWin32Theme::ApplyDarkTitleBar(hwnd);

    CreateWindowExW(0, L"BUTTON", L"Changelog", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     24, H - 76, 100, 30, hwnd, (HMENU)IDC_CHANGELOG, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"GitHub", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     130, H - 76, 90, 30, hwnd, (HMENU)IDC_GITHUB, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Website", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     226, H - 76, 90, 30, hwnd, (HMENU)IDC_WEBSITE, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Get Started \u2192", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                     W - 154, H - 76, 130, 30, hwnd, (HMENU)IDC_GETSTARTED, hInst, nullptr);

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
