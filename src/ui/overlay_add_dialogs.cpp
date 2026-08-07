#include "overlay_add_dialogs.h"
#include "win32_theme.h"
#include <string>
#include <vector>

namespace {

std::wstring WideFromNarrow(const std::string &s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

// ---------------------------------------------------------------------------
// Text prompt
// ---------------------------------------------------------------------------
enum { IDC_TP_EDIT = 6001, IDC_TP_OK, IDC_TP_CANCEL };

struct TextPromptCtx {
    HWND edit = nullptr;
    std::wstring result;
    bool confirmed = false;
};

LRESULT CALLBACK TextPromptProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = reinterpret_cast<TextPromptCtx *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
            return 0; // nested modal loop, see overlays_dock_panel.cpp's callers
        case WM_CTLCOLORSTATIC:
            return (LRESULT)HrWin32Theme::ColorStatic((HDC)wParam);
        case WM_CTLCOLOREDIT:
            return (LRESULT)HrWin32Theme::ColorEdit((HDC)wParam);
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_TP_OK) {
                wchar_t buf[512] = {};
                GetWindowTextW(ctx->edit, buf, 512);
                ctx->result = buf;
                ctx->confirmed = true;
                DestroyWindow(hwnd);
            } else if (id == IDC_TP_CANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// Input-overlay source picker
// ---------------------------------------------------------------------------
enum { IDC_IOP_LIST = 6101, IDC_IOP_OK, IDC_IOP_CANCEL };

struct SourcePickerCtx {
    HWND list = nullptr;
    int selected = -1;
    bool confirmed = false;
};

LRESULT CALLBACK SourcePickerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = reinterpret_cast<SourcePickerCtx *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
            return 0;
        case WM_CTLCOLORSTATIC:
            return (LRESULT)HrWin32Theme::ColorStatic((HDC)wParam);
        case WM_CTLCOLORLISTBOX:
            return (LRESULT)HrWin32Theme::ColorEdit((HDC)wParam);
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int notify = HIWORD(wParam);
            if (id == IDC_IOP_LIST && notify == LBN_DBLCLK) {
                ctx->selected = (int)SendMessageW(ctx->list, LB_GETCURSEL, 0, 0);
                if (ctx->selected >= 0) { ctx->confirmed = true; DestroyWindow(hwnd); }
                return 0;
            }
            if (id == IDC_IOP_OK) {
                ctx->selected = (int)SendMessageW(ctx->list, LB_GETCURSEL, 0, 0);
                if (ctx->selected >= 0) ctx->confirmed = true;
                DestroyWindow(hwnd);
            } else if (id == IDC_IOP_CANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// Webcam device picker
// ---------------------------------------------------------------------------
enum { IDC_WCP_LIST = 6201, IDC_WCP_OK, IDC_WCP_CANCEL };

struct WebcamPickerCtx {
    HWND list = nullptr;
    int selected = -1;
    bool confirmed = false;
};

LRESULT CALLBACK WebcamPickerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = reinterpret_cast<WebcamPickerCtx *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
            return 0;
        case WM_CTLCOLORSTATIC:
            return (LRESULT)HrWin32Theme::ColorStatic((HDC)wParam);
        case WM_CTLCOLORLISTBOX:
            return (LRESULT)HrWin32Theme::ColorEdit((HDC)wParam);
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int notify = HIWORD(wParam);
            if (id == IDC_WCP_LIST && notify == LBN_DBLCLK) {
                ctx->selected = (int)SendMessageW(ctx->list, LB_GETCURSEL, 0, 0);
                if (ctx->selected >= 0) { ctx->confirmed = true; DestroyWindow(hwnd); }
                return 0;
            }
            if (id == IDC_WCP_OK) {
                ctx->selected = (int)SendMessageW(ctx->list, LB_GETCURSEL, 0, 0);
                if (ctx->selected >= 0) ctx->confirmed = true;
                DestroyWindow(hwnd);
            } else if (id == IDC_WCP_CANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

bool HrPromptForText(HWND parent, HINSTANCE hInst, const std::wstring &title,
                     const std::wstring &label, std::wstring &value)
{
    static bool registered = false;
    static const wchar_t kClass[] = L"HomRecTextPrompt";
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = TextPromptProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kClass;
        wc.hbrBackground = HrWin32Theme::BgBrush();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    TextPromptCtx ctx;
    ctx.result = value;

    int ex, ey, ew, eh;
    HrWin32Theme::CenteredWindowRect(320, 130, WS_POPUP | WS_CAPTION | WS_SYSMENU, ex, ey, ew, eh);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, title.c_str(),
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 ex, ey, ew, eh, parent, nullptr, hInst, &ctx);
    HrWin32Theme::ApplyDarkTitleBar(hwnd);

    CreateWindowExW(0, L"STATIC", label.c_str(), WS_CHILD | WS_VISIBLE, 12, 14, 280, 20, hwnd, nullptr, hInst, nullptr);
    ctx.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", value.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                12, 38, 280, 24, hwnd, (HMENU)IDC_TP_EDIT, hInst, nullptr);

    HrWin32Theme::ThemeButton(CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                     124, 74, 80, 26, hwnd, (HMENU)IDC_TP_OK, hInst, nullptr));
    HrWin32Theme::ThemeButton(CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     212, 74, 80, 26, hwnd, (HMENU)IDC_TP_CANCEL, hInst, nullptr));

    SetFocus(ctx.edit);
    SendMessageW(ctx.edit, EM_SETSEL, 0, -1);

    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (!IsWindow(hwnd)) break;
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    if (ctx.confirmed) value = ctx.result;
    return ctx.confirmed;
}

bool HrPromptForInputOverlaySource(HWND parent, HINSTANCE hInst,
                                    const std::vector<HrInputOverlaySource> &sources,
                                    size_t &out_index)
{
    if (sources.empty()) return false;

    static bool registered = false;
    static const wchar_t kClass[] = L"HomRecInputOverlayPicker";
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = SourcePickerProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kClass;
        wc.hbrBackground = HrWin32Theme::BgBrush();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    SourcePickerCtx ctx;

    int ex, ey, ew, eh;
    HrWin32Theme::CenteredWindowRect(340, 320, WS_POPUP | WS_CAPTION | WS_SYSMENU, ex, ey, ew, eh);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"Select Input Overlay",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 ex, ey, ew, eh, parent, nullptr, hInst, &ctx);
    HrWin32Theme::ApplyDarkTitleBar(hwnd);

    CreateWindowExW(0, L"STATIC", L"Choose a preset (double-click, or select + OK):",
                     WS_CHILD | WS_VISIBLE, 12, 12, 300, 20, hwnd, nullptr, hInst, nullptr);
    ctx.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP,
                                12, 36, 300, 200, hwnd, (HMENU)IDC_IOP_LIST, hInst, nullptr);
    for (const auto &src : sources) {
        std::wstring row = L"[" + WideFromNarrow(src.category) + L"]  " + WideFromNarrow(src.label);
        SendMessageW(ctx.list, LB_ADDSTRING, 0, (LPARAM)row.c_str());
    }
    SendMessageW(ctx.list, LB_SETCURSEL, 0, 0);

    HrWin32Theme::ThemeButton(CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                     144, 250, 80, 26, hwnd, (HMENU)IDC_IOP_OK, hInst, nullptr));
    HrWin32Theme::ThemeButton(CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     232, 250, 80, 26, hwnd, (HMENU)IDC_IOP_CANCEL, hInst, nullptr));

    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (!IsWindow(hwnd)) break;
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    if (ctx.confirmed && ctx.selected >= 0 && (size_t)ctx.selected < sources.size()) {
        out_index = (size_t)ctx.selected;
        return true;
    }
    return false;
}

bool HrPromptForWebcamDevice(HWND parent, HINSTANCE hInst,
                              const std::vector<HrWebcamDevice> &devices,
                              size_t &out_index)
{
    if (devices.empty()) return false;

    static bool registered = false;
    static const wchar_t kClass[] = L"HomRecWebcamPicker";
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = WebcamPickerProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kClass;
        wc.hbrBackground = HrWin32Theme::BgBrush();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    WebcamPickerCtx ctx;

    int ex, ey, ew, eh;
    HrWin32Theme::CenteredWindowRect(340, 320, WS_POPUP | WS_CAPTION | WS_SYSMENU, ex, ey, ew, eh);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"Choose a Webcam",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 ex, ey, ew, eh, parent, nullptr, hInst, &ctx);
    HrWin32Theme::ApplyDarkTitleBar(hwnd);

    CreateWindowExW(0, L"STATIC", L"Choose a camera (double-click, or select + OK):",
                     WS_CHILD | WS_VISIBLE, 12, 12, 300, 20, hwnd, nullptr, hInst, nullptr);
    ctx.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP,
                                12, 36, 300, 200, hwnd, (HMENU)IDC_WCP_LIST, hInst, nullptr);
    for (const auto &dev : devices) {
        std::wstring row = WideFromNarrow(dev.name);
        SendMessageW(ctx.list, LB_ADDSTRING, 0, (LPARAM)row.c_str());
    }
    SendMessageW(ctx.list, LB_SETCURSEL, 0, 0);

    HrWin32Theme::ThemeButton(CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                     144, 250, 80, 26, hwnd, (HMENU)IDC_WCP_OK, hInst, nullptr));
    HrWin32Theme::ThemeButton(CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     232, 250, 80, 26, hwnd, (HMENU)IDC_WCP_CANCEL, hInst, nullptr));

    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (!IsWindow(hwnd)) break;
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    if (ctx.confirmed && ctx.selected >= 0 && (size_t)ctx.selected < devices.size()) {
        out_index = (size_t)ctx.selected;
        return true;
    }
    return false;
}
