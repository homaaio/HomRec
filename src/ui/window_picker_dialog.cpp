#include "window_picker_dialog.h"
#include "win32_theme.h"
#include <string>
#include <vector>
#include <algorithm>
#include <windowsx.h> // GET_X_LPARAM/GET_Y_LPARAM - ShowRegionPickerOverlay()'s drag tracking

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

std::wstring WideFromNarrow(const std::string &s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

// True for the same "actually a capturable top-level window" candidates
// ShowWindowPickerDialog()'s list uses - kept as one shared check so
// HR_ResolveCaptureWindow() below can't resolve a title to some window
// the picker itself would never have shown as an option.
bool IsCapturableWindow(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) && !(exStyle & WS_EX_APPWINDOW)) return false;

    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;

    return true;
}

} // namespace

namespace {

// ASCII/BMP-safe case-insensitive equality via the OS (handles non-Latin
// titles that a naive towlower() loop would get wrong).
bool TitlesEqualNoCase(const std::wstring &a, const std::wstring &b) {
    if (a.size() != b.size()) return false;
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

std::wstring WindowTitleOf(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring t(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, t.data(), len + 1);
    t.resize(wcslen(t.c_str()));
    return t;
}

} // namespace

bool HR_ResolveCaptureWindow(const std::string &title, HWND &out_hwnd, RECT &out_rect,
                             HWND preferred_hwnd) {
    HWND found = nullptr;

    // 1) The exact window that was picked - immune to title changes.
    if (preferred_hwnd && IsWindow(preferred_hwnd) && IsCapturableWindow(preferred_hwnd)) {
        found = preferred_hwnd;
    }

    // 2) Fall back to the stored title (the only thing left after an app
    //    restart, or once the picked window was closed and re-opened).
    if (!found && !title.empty()) {
        std::wstring wtitle = WideFromNarrow(title);

        struct Ctx {
            const std::wstring *wanted;
            HWND exact = nullptr;
            HWND loose = nullptr;   // case-insensitive match, only used if nothing matches exactly
        };
        Ctx ctx{&wtitle};

        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto *c = reinterpret_cast<Ctx *>(lp);
            if (!IsCapturableWindow(hwnd)) return TRUE;

            std::wstring t = WindowTitleOf(hwnd);
            if (t.empty()) return TRUE;

            if (t == *c->wanted) {
                c->exact = hwnd;
                return FALSE;  // exact match, stop enumerating
            }
            if (!c->loose && TitlesEqualNoCase(t, *c->wanted)) c->loose = hwnd;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

        found = ctx.exact ? ctx.exact : ctx.loose;
    }

    if (!found) return false;

    // A minimized window has no on-screen pixels to crop to (its rect is
    // parked at -32000,-32000) - treat it as "not available" rather than
    // letting the caller compute a crop rect that lands off every monitor.
    if (IsIconic(found)) return false;

    // DWMWA_EXTENDED_FRAME_BOUNDS, not GetWindowRect(): on Win10/11,
    // GetWindowRect() includes several pixels of invisible resize-grip
    // margin around most app windows (part of how the new-style thin
    // borders are implemented) - cropping DXGI's frame to *that* rect
    // would capture a sliver of desktop wallpaper around three edges of
    // the window instead of the window itself. The DWM attribute gives
    // the actual visible bounds, matching what the user sees on screen.
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(found, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
        if (!GetWindowRect(found, &r)) return false;  // last-resort fallback
    }
    if (r.right <= r.left || r.bottom <= r.top) return false;  // degenerate

    out_hwnd = found;
    out_rect = r;
    return true;
}

namespace {

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
    // BUGFIX (UI freeze): PrintWindow() sends WM_PRINT/WM_PAINT-family
    // messages to hwnd's own message queue and blocks until it's
    // processed them - it has no timeout. ShowWindowPickerDialog() calls
    // this once per visible top-level window *before* the picker dialog
    // is even shown, so a single hung/not-responding app anywhere on the
    // user's desktop (a common, everyday occurrence - a stuck dialog, a
    // frozen webpage, a long modal file-save) used to freeze the entire
    // "Select Window to Record" picker, with no window on screen yet to
    // even show what's wrong. IsHungAppWindow() is the cheap, documented
    // way to check first; a hung window just falls through to the
    // already-existing icon-based fallback below instead of risking the
    // indefinite block.
    if (!IsIconic(hwnd) && !IsHungAppWindow(hwnd)) {
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
                    // Remember the exact window too - see AppState::capture_window_hwnd.
                    ctx->state->capture_window_hwnd = (*ctx->entries)[(size_t)sel].hwnd;
                    ctx->state->capture_mode = CaptureMode::Window;
                    DestroyWindow(hwnd);
                }
            } else if (id == IDC_WP_DESKTOP) {
                ctx->state->capture_mode = CaptureMode::Desktop;
                ctx->state->capture_window_title.clear();
                ctx->state->capture_window_hwnd = nullptr;
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
    // The remembered HWND beats a title match (titles can be duplicated or
    // have changed since the window was picked).
    if (state.capture_window_hwnd) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].hwnd == state.capture_window_hwnd) { preselect = (int)i; break; }
        }
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

// ---------------------------------------------------------------------------
// ShowRegionPickerOverlay
// ---------------------------------------------------------------------------
namespace {

constexpr wchar_t kRegionClassName[] = L"HomRecRegionPicker";
constexpr int kMinDragPx = 8; // see the .h doc comment - anything smaller is treated as a stray click, not a drag

struct RegionPickerCtx {
    bool dragging = false;
    bool have_result = false;
    POINT start{};   // client coords (== screen coords minus the overlay's own top-left)
    POINT current{};
    RECT result{};   // screen (virtual-desktop) coords, filled in on a successful drag
};

LRESULT CALLBACK RegionPickerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = reinterpret_cast<RegionPickerCtx *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        case WM_LBUTTONDOWN: {
            if (!ctx) break;
            ctx->dragging = true;
            ctx->start.x = GET_X_LPARAM(lParam);
            ctx->start.y = GET_Y_LPARAM(lParam);
            ctx->current = ctx->start;
            SetCapture(hwnd);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!ctx || !ctx->dragging) break;
            ctx->current.x = GET_X_LPARAM(lParam);
            ctx->current.y = GET_Y_LPARAM(lParam);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!ctx || !ctx->dragging) break;
            ctx->dragging = false;
            ReleaseCapture();
            int x0 = std::min(ctx->start.x, ctx->current.x);
            int y0 = std::min(ctx->start.y, ctx->current.y);
            int x1 = std::max(ctx->start.x, ctx->current.x);
            int y1 = std::max(ctx->start.y, ctx->current.y);
            if ((x1 - x0) >= kMinDragPx && (y1 - y0) >= kMinDragPx) {
                RECT win_rect{};
                GetWindowRect(hwnd, &win_rect); // overlay's own screen origin -> client coords above are relative to it
                ctx->result.left = win_rect.left + x0;
                ctx->result.top = win_rect.top + y0;
                ctx->result.right = win_rect.left + x1;
                ctx->result.bottom = win_rect.top + y1;
                ctx->have_result = true;
            }
            // Either way (a real drag or a stray click) the overlay's done
            // its job - close it. A stray click leaves have_result false,
            // so ShowRegionPickerOverlay() below just leaves state as-is.
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);

            // Dim backdrop - the whole overlay window is later given
            // uniform alpha via SetLayeredWindowAttributes() below, so
            // this doesn't need to be very dark on its own; it just needs
            // to read as "an overlay is active" and give the bright
            // selection rectangle something to contrast against.
            HBRUSH bg = CreateSolidBrush(RGB(20, 20, 24));
            FillRect(dc, &client, bg);
            DeleteObject(bg);

            std::wstring hint = L"Drag to select the region to record  \u2014  Esc to cancel";
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(235, 235, 235));
            HFONT font = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(dc, font);
            RECT hintRect{client.left, client.top + 24, client.right, client.top + 60};
            DrawTextW(dc, hint.c_str(), -1, &hintRect, DT_CENTER | DT_SINGLELINE);
            SelectObject(dc, oldFont);
            DeleteObject(font);

            if (ctx && ctx->dragging) {
                int x0 = std::min(ctx->start.x, ctx->current.x);
                int y0 = std::min(ctx->start.y, ctx->current.y);
                int x1 = std::max(ctx->start.x, ctx->current.x);
                int y1 = std::max(ctx->start.y, ctx->current.y);

                // Punch out the selected area so it reads as "unveiled"
                // rather than just outlined on top of the dim fill.
                RECT sel{x0, y0, x1, y1};
                HBRUSH clear = CreateSolidBrush(RGB(60, 120, 220));
                FrameRect(dc, &sel, clear);
                DeleteObject(clear);

                HPEN pen = CreatePen(PS_SOLID, 2, RGB(90, 160, 250));
                HPEN oldPen = (HPEN)SelectObject(dc, pen);
                HBRUSH oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, x0, y0, x1, y1);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(pen);

                wchar_t dims[64];
                swprintf(dims, 64, L"%d x %d", x1 - x0, y1 - y0);
                RECT dimsRect{x0, y1 + 6, x1, y1 + 30};
                DrawTextW(dc, dims, -1, &dimsRect, DT_LEFT | DT_SINGLELINE);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

void ShowRegionPickerOverlay(HWND parent, HINSTANCE hInst, AppState &state) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = RegionPickerProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kRegionClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    RegisterClassW(&wc);

    RegionPickerCtx ctx;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED, kRegionClassName, L"",
                                 WS_POPUP, vx, vy, vw, vh,
                                 parent, nullptr, hInst, &ctx);
    if (!hwnd) return;
    // Uniform alpha over the whole overlay (backdrop + selection UI
    // together) - the simplest version of the usual screenshot-tool
    // "dim everything, draw a bright box for the selection" look,
    // without needing to composite an actual desktop screenshot first.
    SetLayeredWindowAttributes(hwnd, 0, 190, LWA_ALPHA);

    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd); // needed for WM_KEYDOWN (Esc) - a WS_POPUP with no WS_TABSTOP children doesn't get it automatically

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!IsWindow(hwnd)) break;
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    if (ctx.have_result) {
        state.capture_mode = CaptureMode::Region;
        state.region_x = ctx.result.left;
        state.region_y = ctx.result.top;
        state.region_w = ctx.result.right - ctx.result.left;
        state.region_h = ctx.result.bottom - ctx.result.top;
    }
}
