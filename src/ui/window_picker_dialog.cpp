#include "window_picker_dialog.h"
#include "win32_theme.h"
#include <string>
#include <vector>
#include <algorithm>

// dwmapi.h ships with this project's MinGW-w64 toolchain (already linked
// via -ldwmapi, see win32_theme.cpp), but a couple of the attribute
// constants used below predate some SDK header snapshots, so they're
// given by number with a fallback name, same convention as win32_theme.cpp.
#include <dwmapi.h>
#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif
#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace {

constexpr wchar_t kClassName[] = L"HomRecWindowPicker";
enum { IDC_WP_LIST = 8301, IDC_WP_RECORD, IDC_WP_DESKTOP, IDC_WP_COUNT_LABEL };

// Thumbnail tile size. 16:9 at a size big enough to actually read what's
// on screen, small enough that capturing a few dozen of them stays fast
// and the list stays scannable (matches the row height below).
constexpr int kThumbW = 160;
constexpr int kThumbH = 90;
constexpr int kRowPad = 8;
constexpr int kRowHeight = kThumbH + kRowPad * 2;

std::string NarrowFromWide(const std::wstring &w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

struct WindowEntry {
    HWND hwnd = nullptr;
    std::wstring title;
    HBITMAP thumb = nullptr;  // always kThumbW x kThumbH once captured, or null on failure
};

// Same "is this actually a window worth showing" heuristics OBS itself
// uses for its window-capture source list (see obs-studio's
// get-windows.cpp): visible, not owned by another window (owned popups
// are secondary UI, not standalone capture targets), not a tool window
// unless it explicitly opts back in via WS_EX_APPWINDOW, and not
// DWM-cloaked (suspended UWP apps / other-virtual-desktop windows report
// as "visible" but are actually invisible and would just capture blank).
// Filtering these out up front also means we don't waste time trying to
// thumbnail windows nobody would ever pick.
std::vector<WindowEntry> EnumCandidateWindows(HWND exclude) {
    struct Ctx {
        std::vector<WindowEntry> *entries;
        HWND exclude;
    };
    std::vector<WindowEntry> entries;
    Ctx ctx{&entries, exclude};

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(lp);
        if (hwnd == c->exclude) return TRUE;
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((exStyle & WS_EX_TOOLWINDOW) && !(exStyle & WS_EX_APPWINDOW)) return TRUE;

        DWORD cloaked = 0;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;

        int len = GetWindowTextLengthW(hwnd);
        if (len <= 0) return TRUE;
        std::wstring title(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(hwnd, title.data(), len + 1);
        title.resize(wcslen(title.c_str()));
        if (title.empty()) return TRUE;

        WindowEntry entry;
        entry.hwnd = hwnd;
        entry.title = std::move(title);
        c->entries->push_back(std::move(entry));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return entries;
}

// Renders a live thumbnail of hwnd's current on-screen content into a
// kThumbW x kThumbH tile (letterboxed to preserve aspect ratio), or
// returns nullptr if that isn't possible (minimized window, or an app
// that doesn't support PrintWindow) -- callers fall back to the window's
// own icon in that case rather than showing a blank/black tile.
HBITMAP CaptureWindowThumbnail(HWND hwnd, int tileW, int tileH) {
    if (!IsWindow(hwnd)) return nullptr;

    RECT rc{};
    // Extended frame bounds excludes the invisible resize-border/drop-
    // shadow margin DWM pads every top-level window with on Win10/11, so
    // the thumbnail isn't mostly empty margin. Falls back to the raw
    // window rect on anything that doesn't support the attribute.
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc)))) {
        if (!GetWindowRect(hwnd, &rc)) return nullptr;
    }
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    constexpr int kMaxCaptureDim = 8192;  // sanity cap against bogus/huge reported bounds
    if (w <= 0 || h <= 0 || w > kMaxCaptureDim || h > kMaxCaptureDim) return nullptr;

    HDC screenDC = GetDC(nullptr);
    HDC tileDC = CreateCompatibleDC(screenDC);
    HBITMAP tileBmp = CreateCompatibleBitmap(screenDC, tileW, tileH);
    HBITMAP oldTile = static_cast<HBITMAP>(SelectObject(tileDC, tileBmp));

    HBRUSH bg = CreateSolidBrush(HrWin32Theme::kSurface);
    RECT full{0, 0, tileW, tileH};
    FillRect(tileDC, &full, bg);
    DeleteObject(bg);

    bool drew = false;
    if (!IsIconic(hwnd)) {
        HDC srcDC = CreateCompatibleDC(screenDC);
        HBITMAP srcBmp = CreateCompatibleBitmap(screenDC, w, h);
        HBITMAP oldSrc = static_cast<HBITMAP>(SelectObject(srcDC, srcBmp));

        // PW_RENDERFULLCONTENT is needed for anything GPU-composited
        // (browsers, games, most modern apps) -- without it PrintWindow
        // often just yields a black/blank rectangle for those. Fall back
        // to the plain flag for older apps that don't recognize it.
        BOOL ok = PrintWindow(hwnd, srcDC, PW_RENDERFULLCONTENT);
        if (!ok) ok = PrintWindow(hwnd, srcDC, 0);
        if (ok) {
            float scale = std::min(static_cast<float>(tileW) / w, static_cast<float>(tileH) / h);
            int dw = std::max(1, static_cast<int>(w * scale));
            int dh = std::max(1, static_cast<int>(h * scale));
            int dx = (tileW - dw) / 2;
            int dy = (tileH - dh) / 2;
            SetStretchBltMode(tileDC, HALFTONE);
            SetBrushOrgEx(tileDC, 0, 0, nullptr);
            StretchBlt(tileDC, dx, dy, dw, dh, srcDC, 0, 0, w, h, SRCCOPY);
            drew = true;
        }

        SelectObject(srcDC, oldSrc);
        DeleteObject(srcBmp);
        DeleteDC(srcDC);
    }

    if (!drew) {
        // Minimized window, or an app PrintWindow can't render -- use its
        // own icon centered in the tile instead of leaving it blank.
        HICON icon = nullptr;
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0,
                                 SMTO_ABORTIFHUNG, 200, &result) && result) {
            icon = reinterpret_cast<HICON>(result);
        }
        if (!icon) {
            icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
        }
        if (!icon) {
            result = 0;
            if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL, 0,
                                     SMTO_ABORTIFHUNG, 200, &result) && result) {
                icon = reinterpret_cast<HICON>(result);
            }
        }
        if (icon) {
            int iconSize = std::min(tileW, tileH) / 2;
            DrawIconEx(tileDC, (tileW - iconSize) / 2, (tileH - iconSize) / 2,
                       icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
        }
    }

    SelectObject(tileDC, oldTile);
    DeleteDC(tileDC);
    ReleaseDC(nullptr, screenDC);
    return tileBmp;
}

struct PickerCtx {
    AppState *state;
    std::vector<WindowEntry> *entries;
};

LRESULT CALLBACK PickerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = reinterpret_cast<PickerCtx *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_WP_RECORD) {
                HWND list = GetDlgItem(hwnd, IDC_WP_LIST);
                int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)ctx->entries->size()) {
                    ctx->state->capture_window_title = NarrowFromWide((*ctx->entries)[(size_t)sel].title);
                    ctx->state->capture_mode = CaptureMode::Window;
                    DestroyWindow(hwnd);
                }
            } else if (id == IDC_WP_DESKTOP) {
                ctx->state->capture_mode = CaptureMode::Desktop;
                ctx->state->capture_window_title.clear();
                DestroyWindow(hwnd);
            } else if (id == IDC_WP_LIST && HIWORD(wParam) == LBN_DBLCLK) {
                // Double-click a row = same as "Record this window", matching
                // the natural double-click expectation for a list box.
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_WP_RECORD, 0), 0);
            }
            return 0;
        }
        case WM_MEASUREITEM: {
            auto *mis = reinterpret_cast<MEASUREITEMSTRUCT *>(lParam);
            if (mis->CtlID == IDC_WP_LIST) {
                mis->itemHeight = kRowHeight;
                return TRUE;
            }
            return FALSE;
        }
        case WM_DRAWITEM: {
            auto *dis = reinterpret_cast<DRAWITEMSTRUCT *>(lParam);
            if (dis->CtlID != IDC_WP_LIST || dis->itemID == (UINT)-1 ||
                !ctx || dis->itemID >= ctx->entries->size()) {
                return FALSE;
            }
            const WindowEntry &e = (*ctx->entries)[dis->itemID];
            bool selected = (dis->itemState & ODS_SELECTED) != 0;

            HBRUSH rowBrush = CreateSolidBrush(selected ? HrWin32Theme::kSurfaceLight : HrWin32Theme::kBg);
            FillRect(dis->hDC, &dis->rcItem, rowBrush);
            DeleteObject(rowBrush);

            int thumbX = dis->rcItem.left + kRowPad;
            int thumbY = dis->rcItem.top + kRowPad;

            if (e.thumb) {
                HDC memDC = CreateCompatibleDC(dis->hDC);
                HBITMAP old = static_cast<HBITMAP>(SelectObject(memDC, e.thumb));
                BitBlt(dis->hDC, thumbX, thumbY, kThumbW, kThumbH, memDC, 0, 0, SRCCOPY);
                SelectObject(memDC, old);
                DeleteDC(memDC);
            } else {
                RECT tr{thumbX, thumbY, thumbX + kThumbW, thumbY + kThumbH};
                HBRUSH ph = CreateSolidBrush(HrWin32Theme::kSurface);
                FillRect(dis->hDC, &tr, ph);
                DeleteObject(ph);
            }

            RECT border{thumbX, thumbY, thumbX + kThumbW, thumbY + kThumbH};
            HBRUSH frameBrush = CreateSolidBrush(HrWin32Theme::kSurfaceLight);
            FrameRect(dis->hDC, &border, frameBrush);
            DeleteObject(frameBrush);

            RECT textRect{thumbX + kThumbW + 12, dis->rcItem.top,
                           dis->rcItem.right - kRowPad, dis->rcItem.bottom};
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, selected ? HrWin32Theme::kText : HrWin32Theme::kTextDim);
            DrawTextW(dis->hDC, e.title.c_str(), -1, &textRect,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            return TRUE;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (ctx && ctx->entries) {
                for (auto &e : *ctx->entries) {
                    if (e.thumb) {
                        DeleteObject(e.thumb);
                        e.thumb = nullptr;
                    }
                }
            }
            return 0;
        case WM_CTLCOLORSTATIC:
            return (LRESULT)HrWin32Theme::ColorStatic((HDC)wParam);
        case WM_CTLCOLORLISTBOX:
            return (LRESULT)HrWin32Theme::ColorEdit((HDC)wParam);
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

void ShowWindowPickerDialog(HWND parent, HINSTANCE hInst, AppState &state) {
    std::vector<WindowEntry> entries = EnumCandidateWindows(parent);
    if (entries.empty()) {
        MessageBoxW(parent, L"No open windows found.", L"Info", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Thumbnails are captured once, up front, and cached in `entries` for
    // the life of the dialog -- WM_DRAWITEM just blits the cached bitmap,
    // it never re-captures on scroll/selection/redraw.
    for (auto &e : entries) {
        e.thumb = CaptureWindowThumbnail(e.hwnd, kThumbW, kThumbH);
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = PickerProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClassName;
    wc.hbrBackground = HrWin32Theme::BgBrush();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    const int W = 560, H = 520;

    PickerCtx ctx;
    ctx.state = &state;
    ctx.entries = &entries;

    int wx, wy, ww, wh;
    HrWin32Theme::CenteredWindowRect(W, H, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, wx, wy, ww, wh);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, L"Select Window to Record",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                                 wx, wy, ww, wh,
                                 parent, nullptr, hInst, &ctx);
    HrWin32Theme::ApplyDarkTitleBar(hwnd);

    std::wstring countLabel = std::to_wstring(entries.size()) + L" windows found";
    CreateWindowExW(0, L"STATIC", countLabel.c_str(), WS_CHILD | WS_VISIBLE,
                     15, 10, 300, 18, hwnd, (HMENU)IDC_WP_COUNT_LABEL, hInst, nullptr);

    HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                 WS_CHILD | WS_VISIBLE | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                                 LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
                                 15, 34, W - 32, H - 100, hwnd, (HMENU)IDC_WP_LIST, hInst, nullptr);
    int preselect = -1;
    for (size_t i = 0; i < entries.size(); ++i) {
        // LB_ADDSTRING still gives the listbox each item's text (used for
        // type-ahead search and LB_GETTEXT) even though owner-draw means
        // *we* do all the actual on-screen rendering in WM_DRAWITEM above.
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)entries[i].title.c_str());
        if (NarrowFromWide(entries[i].title) == state.capture_window_title) preselect = (int)i;
    }
    if (preselect >= 0) {
        SendMessageW(list, LB_SETCURSEL, (WPARAM)preselect, 0);
    }

    HrWin32Theme::ThemeButton(CreateWindowExW(0, L"BUTTON", L"Record this window", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                     15, H - 56, 180, 30, hwnd, (HMENU)IDC_WP_RECORD, hInst, nullptr));
    HrWin32Theme::ThemeButton(CreateWindowExW(0, L"BUTTON", L"Use full desktop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     205, H - 56, 150, 30, hwnd, (HMENU)IDC_WP_DESKTOP, hInst, nullptr));

    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (!IsWindow(hwnd)) break;
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}
