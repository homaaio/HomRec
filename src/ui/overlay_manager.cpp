#include "overlay_manager.h"
#include <commctrl.h>
#include <windowsx.h>
#include <string>
#include <sstream>

extern "C" {
    void *hr_di_create();
    void hr_di_destroy(void *handle);
    void hr_di_refresh(void *handle);
    int hr_di_primary(void *handle, int *x, int *y, int *w, int *h, float *dpi);
}

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

std::wstring SummaryFor(const OverlayDef &ov) {
    std::wostringstream oss;
    oss << (ov.visible ? L"[on]  " : L"[off] ") << WideFromNarrow(ov.type) << L"  \""
        << WideFromNarrow(ov.text.empty() ? ov.image_path : ov.text) << L"\"  ("
        << ov.x << L"," << ov.y << L" " << ov.w << L"x" << ov.h << L")";
    return oss.str();
}

// ---------------------------------------------------------------------------
// Property editor for a single overlay — replaces _build_editor/_apply_editor.
// ---------------------------------------------------------------------------

enum {
    IDC_ED_TYPE = 5001, IDC_ED_TEXT, IDC_ED_IMAGE, IDC_ED_WEBCAM,
    IDC_ED_X, IDC_ED_Y, IDC_ED_W, IDC_ED_H, IDC_ED_VISIBLE,
    IDC_ED_OK, IDC_ED_CANCEL,
};

struct EditorCtx {
    OverlayDef *ov;
    bool saved = false;
    HWND type_combo, text_edit, image_edit, webcam_edit, x_edit, y_edit, w_edit, h_edit, visible_chk;
};

LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = reinterpret_cast<EditorCtx *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
            return 0; // nested modal loop — see settings_dialog.cpp for why no PostQuitMessage here
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_ED_OK) {
                wchar_t buf[512] = {};
                GetWindowTextW(ctx->text_edit, buf, 512); ctx->ov->text = NarrowFromWide(buf);
                GetWindowTextW(ctx->image_edit, buf, 512); ctx->ov->image_path = NarrowFromWide(buf);
                GetWindowTextW(ctx->webcam_edit, buf, 512); ctx->ov->webcam_index = _wtoi(buf);
                GetWindowTextW(ctx->x_edit, buf, 512); ctx->ov->x = _wtoi(buf);
                GetWindowTextW(ctx->y_edit, buf, 512); ctx->ov->y = _wtoi(buf);
                GetWindowTextW(ctx->w_edit, buf, 512); ctx->ov->w = _wtoi(buf);
                GetWindowTextW(ctx->h_edit, buf, 512); ctx->ov->h = _wtoi(buf);
                ctx->ov->visible = (SendMessageW(ctx->visible_chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                int sel = (int)SendMessageW(ctx->type_combo, CB_GETCURSEL, 0, 0);
                ctx->ov->type = (sel == 1) ? "image" : (sel == 2) ? "webcam" : "text";
                ctx->saved = true;
                DestroyWindow(hwnd);
            } else if (id == IDC_ED_CANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool EditOverlayDialog(HWND parent, HINSTANCE hInst, OverlayDef &ov) {
    static const wchar_t kClass[] = L"HomRecOverlayEditor";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = EditorProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClass;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    EditorCtx ctx = {};
    ctx.ov = &ov;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"Edit Overlay",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 340, 320,
                                 parent, nullptr, hInst, &ctx);

    auto label = [&](const wchar_t *t, int y) {
        CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE, 12, y, 80, 20, hwnd, nullptr, hInst, nullptr);
    };
    auto edit = [&](const std::wstring &t, int id, int y) {
        return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", t.c_str(), WS_CHILD | WS_VISIBLE,
                                100, y, 200, 22, hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
    };

    int y = 12;
    label(L"Type:", y);
    ctx.type_combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                      100, y, 200, 100, hwnd, (HMENU)IDC_ED_TYPE, hInst, nullptr);
    SendMessageW(ctx.type_combo, CB_ADDSTRING, 0, (LPARAM)L"text");
    SendMessageW(ctx.type_combo, CB_ADDSTRING, 0, (LPARAM)L"image");
    SendMessageW(ctx.type_combo, CB_ADDSTRING, 0, (LPARAM)L"webcam");
    SendMessageW(ctx.type_combo, CB_SETCURSEL, ov.type == "image" ? 1 : ov.type == "webcam" ? 2 : 0, 0);

    y += 30; label(L"Text:", y);
    ctx.text_edit = edit(WideFromNarrow(ov.text), IDC_ED_TEXT, y);
    y += 30; label(L"Image path:", y);
    ctx.image_edit = edit(WideFromNarrow(ov.image_path), IDC_ED_IMAGE, y);
    y += 30; label(L"Webcam idx:", y);
    ctx.webcam_edit = edit(std::to_wstring(ov.webcam_index), IDC_ED_WEBCAM, y);
    y += 30; label(L"X:", y);
    ctx.x_edit = edit(std::to_wstring(ov.x), IDC_ED_X, y);
    y += 30; label(L"Y:", y);
    ctx.y_edit = edit(std::to_wstring(ov.y), IDC_ED_Y, y);
    y += 30; label(L"Width:", y);
    ctx.w_edit = edit(std::to_wstring(ov.w), IDC_ED_W, y);
    y += 30; label(L"Height:", y);
    ctx.h_edit = edit(std::to_wstring(ov.h), IDC_ED_H, y);

    y += 30;
    ctx.visible_chk = CreateWindowExW(0, L"BUTTON", L"Visible", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                       100, y, 100, 22, hwnd, (HMENU)IDC_ED_VISIBLE, hInst, nullptr);
    SendMessageW(ctx.visible_chk, BM_SETCHECK, ov.visible ? BST_CHECKED : BST_UNCHECKED, 0);

    y += 34;
    CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                     100, y, 80, 26, hwnd, (HMENU)IDC_ED_OK, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     190, y, 80, 26, hwnd, (HMENU)IDC_ED_CANCEL, hInst, nullptr);

    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (!IsWindow(hwnd)) break;
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return ctx.saved;
}

// ---------------------------------------------------------------------------
// Drag-to-position preview — replaces OverlayPreviewDialog's canvas drag
// logic. A layered, semi-transparent, click-through-except-overlays window
// sized to the primary monitor; drag rectangles to reposition them.
// ---------------------------------------------------------------------------

struct PreviewCtx {
    std::vector<OverlayDef> *overlays;
    int drag_index = -1;
    int drag_offset_x = 0, drag_offset_y = 0;
};

int HitTest(PreviewCtx *ctx, int mx, int my) {
    auto &ovs = *ctx->overlays;
    for (int i = (int)ovs.size() - 1; i >= 0; --i) {
        const auto &ov = ovs[(size_t)i];
        if (mx >= ov.x && mx < ov.x + ov.w && my >= ov.y && my < ov.y + ov.h) return i;
    }
    return -1;
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = reinterpret_cast<PreviewCtx *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_LBUTTONDOWN: {
            int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
            ctx->drag_index = HitTest(ctx, mx, my);
            if (ctx->drag_index >= 0) {
                auto &ov = (*ctx->overlays)[(size_t)ctx->drag_index];
                ctx->drag_offset_x = mx - ov.x;
                ctx->drag_offset_y = my - ov.y;
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (ctx->drag_index >= 0) {
                int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
                auto &ov = (*ctx->overlays)[(size_t)ctx->drag_index];
                ov.x = mx - ctx->drag_offset_x;
                ov.y = my - ctx->drag_offset_y;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (ctx->drag_index >= 0) { ReleaseCapture(); ctx->drag_index = -1; }
            return 0;
        case WM_RBUTTONUP:
        case WM_KEYDOWN:
            if (msg == WM_RBUTTONUP || wParam == VK_ESCAPE || wParam == VK_RETURN) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT client; GetClientRect(hwnd, &client);
            HBRUSH bg = CreateSolidBrush(RGB(20, 20, 20));
            FillRect(hdc, &client, bg);
            DeleteObject(bg);

            SetBkMode(hdc, TRANSPARENT);
            for (const auto &ov : *ctx->overlays) {
                if (!ov.visible) continue;
                RECT r = { ov.x, ov.y, ov.x + ov.w, ov.y + ov.h };
                HBRUSH box = CreateSolidBrush(RGB(137, 180, 250));
                FrameRect(hdc, &r, box);
                DeleteObject(box);
                SetTextColor(hdc, RGB(205, 214, 244));
                std::wstring label = WideFromNarrow(ov.text.empty() ? ov.type : ov.text);
                DrawTextW(hdc, label.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            std::wstring hint = L"Drag overlays to reposition. Right-click or Esc to finish.";
            RECT hintRect = { 12, 12, client.right - 12, 40 };
            SetTextColor(hdc, RGB(166, 173, 200));
            DrawTextW(hdc, hint.c_str(), -1, &hintRect, DT_LEFT);

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            return 0; // caller's message loop owns the quit condition
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void ShowDragPreview(HWND parent, HINSTANCE hInst, std::vector<OverlayDef> &overlays) {
    static const wchar_t kClass[] = L"HomRecOverlayPreview";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = PreviewProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    RegisterClassW(&wc);

    void *di = hr_di_create();
    hr_di_refresh(di);
    int mx = 0, my = 0, mw = 1280, mh = 720;
    float dpi = 96.0f;
    hr_di_primary(di, &mx, &my, &mw, &mh, &dpi);
    hr_di_destroy(di);

    PreviewCtx ctx;
    ctx.overlays = &overlays;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, kClass, L"Position Overlays",
                                 WS_POPUP, mx, my, mw, mh, parent, nullptr, hInst, &ctx);
    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!IsWindow(hwnd)) break;
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}

// ---------------------------------------------------------------------------
// Manager window — list + add/edit/delete/position/save.
// ---------------------------------------------------------------------------

enum {
    IDC_LIST = 6001, IDC_ADD, IDC_EDIT, IDC_DELETE, IDC_POSITION, IDC_SAVE, IDC_CANCEL,
};

struct ManagerCtx {
    AppState *state;
    HWND list;
    bool saved = false;
    int next_id = 1;
};

void RefreshList(ManagerCtx *ctx) {
    SendMessageW(ctx->list, LB_RESETCONTENT, 0, 0);
    for (const auto &ov : ctx->state->overlays) {
        SendMessageW(ctx->list, LB_ADDSTRING, 0, (LPARAM)SummaryFor(ov).c_str());
    }
}

LRESULT CALLBACK ManagerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *ctx = reinterpret_cast<ManagerCtx *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int sel = (int)SendMessageW(ctx->list, LB_GETCURSEL, 0, 0);
            HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);

            if (id == IDC_ADD) {
                OverlayDef ov;
                ov.id = "ov_" + std::to_string(ctx->next_id++);
                ov.type = "text"; ov.text = "New overlay"; ov.x = 40; ov.y = 40; ov.w = 200; ov.h = 60;
                if (EditOverlayDialog(hwnd, hInst, ov)) {
                    ctx->state->overlays.push_back(ov);
                    RefreshList(ctx);
                }
            } else if (id == IDC_EDIT && sel >= 0) {
                if (EditOverlayDialog(hwnd, hInst, ctx->state->overlays[(size_t)sel])) {
                    RefreshList(ctx);
                }
            } else if (id == IDC_DELETE && sel >= 0) {
                ctx->state->overlays.erase(ctx->state->overlays.begin() + sel);
                RefreshList(ctx);
            } else if (id == IDC_POSITION) {
                ShowDragPreview(hwnd, hInst, ctx->state->overlays);
                RefreshList(ctx);
            } else if (id == IDC_SAVE) {
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

bool ShowOverlayManager(HWND parent, HINSTANCE hInst, AppState &state) {
    static const wchar_t kClass[] = L"HomRecOverlayManager";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = ManagerProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClass;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    ManagerCtx ctx = {};
    ctx.state = &state;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"Overlays",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 420, 360,
                                 parent, nullptr, hInst, &ctx);

    ctx.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
                                12, 12, 396, 220, hwnd, (HMENU)IDC_LIST, hInst, nullptr);
    RefreshList(&ctx);

    CreateWindowExW(0, L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     12, 244, 70, 26, hwnd, (HMENU)IDC_ADD, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Edit", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     90, 244, 70, 26, hwnd, (HMENU)IDC_EDIT, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Delete", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     168, 244, 70, 26, hwnd, (HMENU)IDC_DELETE, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Position...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     246, 244, 90, 26, hwnd, (HMENU)IDC_POSITION, hInst, nullptr);

    CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                     246, 286, 80, 26, hwnd, (HMENU)IDC_SAVE, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     332, 286, 76, 26, hwnd, (HMENU)IDC_CANCEL, hInst, nullptr);

    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (!IsWindow(hwnd)) break;
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    return ctx.saved;
}

void ShowOverlayDragPreview(HWND parent, HINSTANCE hInst, AppState &state) {
    ShowDragPreview(parent, hInst, state.overlays);
}
