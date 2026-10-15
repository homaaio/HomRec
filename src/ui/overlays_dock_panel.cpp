#include "overlays_dock_panel.h"
#include "win32_theme.h"
#include "overlay_add_dialogs.h"
#include "hrc_config.h"
#include "../core/input_overlay.h"
#include "../core/input_overlay_registry.h"
#include "../core/webcam_enum.h"
#include "icons.h"
#include "window_picker_dialog.h"   // HrListMonitors()
#include <commdlg.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <string>
#include <algorithm>

// Phase 1 (see commands.md): show_overlays_panel is now persisted via
// HrcConfig::Save() below, same as everything else in AppState - the old
// hr_settings_* JSON engine's fixed field whitelist is no longer touched
// here at all.

namespace {

std::wstring WideFromNarrow(const std::string &s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

std::string NarrowFromWide(const std::wstring &w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// Runs a standard Open-File dialog with the given filter/title. Returns
// the chosen path (UTF-8), or empty if the user cancelled.
std::string PickOpenFile(HWND parent, const wchar_t *filter, const wchar_t *title) {
    wchar_t file_buf[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = parent;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = file_buf;
    ofn.nMaxFile    = MAX_PATH;
    // Without OFN_NOCHANGEDIR, GetOpenFileNameW() silently changes
    // the process's current working directory to the folder the user just
    // picked a file from. Every settings read/write in this app uses a
    // path relative to the app root (see kSettingsPath in
    // settings_dialog.cpp - "relative to app root"), so after picking an
    // external overlay's .json/.png from some other folder, the next
    // save (e.g. PersistShowOverlaysPanelFlag() below) wrote
    // homrec_settings.json into *that* folder instead - looking exactly
    // like a stray file appearing next to the file you just selected.
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    ofn.lpstrTitle  = title;
    if (!GetOpenFileNameW(&ofn)) return {};
    return NarrowFromWide(file_buf);
}

// Mirrors OverlaysDockPanel.refresh()'s per-row label logic in
// overlays_dock_panel.py (dot + kind icon + truncated name), adapted to
// this port's OverlayDef field names (type/visible/image_path/
// webcam_index instead of kind/enabled/path/cam_index).
const wchar_t *GlyphFor(const OverlayDef &ov) {
    if (ov.type == "text")  return L"\U0001F4DD";   // 📝
    if (ov.type == "image") return L"\U0001F5BC";   // 🖼
    if (ov.type == "gif")   return L"\U0001F3AC";   // 🎬
    if (ov.type == "input_overlay") return L"\u2328"; // ⌨
    return L"\U0001F4F7";                           // 📷 webcam
}

// 2.4: the row text no longer carries the type icon (HandleDrawItem draws the PNG, or the
// glyph above when the PNG is missing) and is no longer cut to 14 characters - the list
// can be as wide as the user makes the panel, and long names are ellipsised when painted.
std::wstring RowLabel(const OverlayDef &ov) {
    const wchar_t *dot = ov.visible ? L"\u25CF" : L"\u25CB"; // ● / ○
    const wchar_t *icon = L"";
    std::wstring name;

    // A user-assigned name (right-click > Rename...) always wins over the
    // auto-generated label below, whatever the overlay's type.
    if (!ov.name.empty()) name = WideFromNarrow(ov.name);

    if (ov.type == "text") {
        icon = L"\U0001F4DD"; // 📝
        if (name.empty()) name = WideFromNarrow(ov.text);
        if (name.empty()) name = L"(empty text)";
    } else if (ov.type == "image") {
        icon = L"\U0001F5BC"; // 🖼
        if (name.empty()) {
            std::string path = ov.image_path;
            size_t slash = path.find_last_of("\\/");
            std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
            name = base.empty() ? L"(no file)" : WideFromNarrow(base);
        }
    } else if (ov.type == "gif") {
        icon = L"\U0001F3AC"; // 🎬
        if (name.empty()) {
            std::string path = ov.image_path;
            size_t slash = path.find_last_of("\\/");
            std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
            name = base.empty() ? L"(no file)" : WideFromNarrow(base);
        }
    } else if (ov.type == "input_overlay") {
        icon = L"\u2328"; // ⌨
        if (name.empty()) {
            std::string path = ov.input_json_path;
            size_t slash = path.find_last_of("\\/");
            std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
            name = base.empty() ? L"(input overlay)" : WideFromNarrow(base);
        }
    } else { // "webcam"
        icon = L"\U0001F4F7"; // 📷
        if (name.empty()) {
            name = ov.webcam_name.empty() ? (L"Cam#" + std::to_wstring(ov.webcam_index))
                                           : WideFromNarrow(ov.webcam_name);
        }
    }
    (void)icon;
    (void)dot;
    return name;       // the eye on the row's right edge shows visible / hidden now
}

// Persists the *entire* current AppState (not just show_overlays_panel -
// see the header comment above on settings.cpp's old JSON whitelist
// bug class) to whichever .hrc path is configured, mirroring the same
// path resolution / default-location mirroring settings_dialog.cpp's
// OnSave uses, so this auto-save and an explicit Settings > Save can't
// disagree about where "the settings file" is.
void PersistState(AppState &state) {
    std::wstring target = HrcConfig::ResolveSettingsPath(state);
    HrcConfig::Save(state, target);
    if (target != HrcConfig::kDefaultSettingsPath) HrcConfig::Save(state, HrcConfig::kDefaultSettingsPath);
}

// TrackPopupMenu(TPM_RETURNCMD) result codes for the "+" menu -- local to
// this file, never go through WM_COMMAND, so no risk of colliding with the
// ID_OVDOCK_* control ids.
enum { kMenuAddText = 1, kMenuAddImage, kMenuAddGif, kMenuAddWebcam, kMenuAddExternal, kMenuAddInputOverlay };

// TrackPopupMenu(TPM_RETURNCMD) result codes for the per-row right-click
// menu -- same reasoning as kMenuAdd* above, a separate numbering space
// local to this file.
enum { kCtxToggle = 1, kCtxRename, kCtxEdit, kCtxDelete, kCtxDuplicate, kCtxUp, kCtxDown, kCtxCenter, kCtxLock, kCtxKeepProp };
// ... and for the capture-source row's menu.
enum { kSrcCtxSettings = 1, kSrcCtxDesktop, kSrcCtxWindow, kSrcCtxRegion };
// Width of the clickable eye column on the right edge of an overlay row.
constexpr int kEyeColW = 30;

} // namespace

OverlaysDockPanel::OverlaysDockPanel(AppState &state) : state_(state) {}

OverlaysDockPanel::~OverlaysDockPanel() {
    // The list window is a child of the wx host panel and is destroyed AFTER this object
    // (frame members go first, children later). Detach our subclass proc while the window
    // is still alive so no message can ever reach a dangling `this`.
    if (list_ && IsWindow(list_)) {
        SetWindowLongPtrW(list_, GWLP_USERDATA, 0);
        if (orig_list_proc_ &&
            (WNDPROC)GetWindowLongPtrW(list_, GWLP_WNDPROC) == ListSubclassProc) {
            SetWindowLongPtrW(list_, GWLP_WNDPROC, (LONG_PTR)orig_list_proc_);
        }
    }
    orig_list_proc_ = nullptr;
    list_ = nullptr;
    hwnd_ = nullptr;
}

namespace {
// The "+" button used to inherit the old bitmap system font, which renders a tiny, thin plus.
// One shared, DPI-aware bold UI font for it (created once, lives for the process).
HFONT AddButtonFont() {
    static HFONT f = nullptr;
    if (!f) {
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            LOGFONTW lf = ncm.lfMessageFont;
            lf.lfHeight = lf.lfHeight * 14 / 10;   // ~40% larger than the message font
            lf.lfWeight = FW_BOLD;
            f = CreateFontIndirectW(&lf);
        }
    }
    return f;   // may stay null -> the button keeps the default font
}
} // namespace

HWND OverlaysDockPanel::Create(HWND parent, HINSTANCE hInst, int x, int y, int w, int h) {
    // WS_CLIPSIBLINGS on all three siblings is the fix for "the + button is invisible until
    // the mouse touches its corner": the sunken STATIC below covers the whole host and is a
    // SIBLING of the button and the list (not their parent). Without WS_CLIPSIBLINGS every
    // repaint of the STATIC (size change, ForceRepaint(), theme change) painted straight over
    // them, and the button only came back when it repainted itself on mouse-over.
    hwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
                             WS_CHILD | WS_CLIPSIBLINGS | (state_.show_overlays_panel ? WS_VISIBLE : 0) | SS_SUNKEN,
                             x, y, w, h, parent, nullptr, hInst, nullptr);
    // Explicitly the bottom-most sibling, whatever order AUI re-parenting leaves them in.
    SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    int cy = y + 6;
    HWND add_btn = CreateWindowExW(0, L"BUTTON", L"+",
                     WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_PUSHBUTTON,
                     x + 6, cy, 28, 24, parent, (HMENU)ID_OVDOCK_ADD, hInst, nullptr);
    if (HFONT bf = AddButtonFont()) SendMessageW(add_btn, WM_SETFONT, (WPARAM)bf, FALSE);
    HrWin32Theme::ThemeButton(add_btn);
    // 2.4: no "x" button of our own any more - the dock pane's caption (and the floating
    // window's title bar) already has one, and having two was confusing.

    // Bottom buttons that used to live here (Show/Hide, Remove) are gone --
    // right-clicking a row now covers both of those plus Rename/Edit
    // Parameters (see ShowRowContextMenu()), so the list gets that
    // reclaimed vertical space instead.
    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | LBS_NOTIFY | WS_VSCROLL |
                             LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                             x + 6, cy + 30, w - 12, h - 42, parent, (HMENU)ID_OVDOCK_LIST, hInst, nullptr);
    // Owner-drawn rows (icon + text). Fixed row height is set explicitly because the
    // WM_MEASUREITEM that would normally supply it is sent before this panel can answer.
    SendMessageW(list_, LB_SETITEMHEIGHT, 0, 28);

    // Subclass the list so right-click (or Shift+F10/the Menu key, which
    // Windows turns into the same WM_CONTEXTMENU) can be handled without
    // main_frame.cpp needing any awareness that rows have a context menu.
    orig_list_proc_ = (WNDPROC)SetWindowLongPtrW(list_, GWLP_WNDPROC, (LONG_PTR)ListSubclassProc);
    SetWindowLongPtrW(list_, GWLP_USERDATA, (LONG_PTR)this);

    Refresh();
    SetVisible(state_.show_overlays_panel);
    return hwnd_;
}

LRESULT CALLBACK OverlaysDockPanel::ListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto *self = reinterpret_cast<OverlaysDockPanel *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);   // detached (panel already gone)
    if (msg == WM_CONTEXTMENU) {
        self->OnListContextMenu(lParam);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        bool handled = false;
        self->OnListMouse(msg, lParam, handled);
        if (handled) return 0;
    }
    if (msg == WM_KEYDOWN) {
        bool handled = false;
        self->OnListKey(wParam, handled);
        if (handled) return 0;
    }
    WNDPROC orig = self->orig_list_proc_;
    if (msg == WM_NCDESTROY) {
        // The window is going away: unhook ourselves so nothing can reach `self` afterwards.
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (orig) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)orig);
    }
    return orig ? CallWindowProcW(orig, hwnd, msg, wParam, lParam)
                : DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---- list helpers --------------------------------------------------------------------------------
// Row 0 = capture source; row r >= 1 = overlay r-1 (or, with no overlays, one non-interactive hint row).
int OverlaysDockPanel::RowAtPoint(POINT client_pt, RECT *rc_out) const {
    if (!list_) return -1;
    const LRESULT hit = SendMessageW(list_, LB_ITEMFROMPOINT, 0, MAKELPARAM(client_pt.x, client_pt.y));
    if (HIWORD(hit) != 0) return -1;     // below the last row
    const int row = (int)LOWORD(hit);
    // LB_ITEMFROMPOINT answers with the LAST item for a point in the empty space below it (the high word is
    // 0 for anything inside the client area) - so the item's own rectangle decides whether the point is
    // really on a row. Otherwise a click / right-click in the blank area under the list would hit the last
    // overlay (its eye would toggle).
    RECT rc{};
    if (SendMessageW(list_, LB_GETITEMRECT, (WPARAM)row, (LPARAM)&rc) == LB_ERR) return -1;
    if (!PtInRect(&rc, client_pt)) return -1;
    if (rc_out) *rc_out = rc;
    return row;
}

int OverlaysDockPanel::OverlayOfRow(int row) const {
    if (row >= 1 && row - 1 < (int)state_.overlays.size()) return row - 1;
    return -1;
}

std::string OverlaysDockPanel::T(const char *key, const char *fallback) const {
    if (tr) {
        std::string s = tr(key);
        if (!s.empty()) return s;
    }
    return fallback;
}

std::string OverlaysDockPanel::NewOverlayId() const {
    int n = next_id_;
    for (;;) {
        const std::string id = "ov_" + std::to_string(n);
        bool taken = false;
        for (const auto &o : state_.overlays) if (o.id == id) { taken = true; break; }
        if (!taken) return id;
        ++n;
    }
}

const std::vector<HrMonitorInfo> &OverlaysDockPanel::Monitors() const {
    const DWORD now = GetTickCount();
    if (mon_cache_.empty() || now - mon_cache_tick_ > 5000) {
        mon_cache_ = HrListMonitors();
        mon_cache_tick_ = now;
    }
    return mon_cache_;
}

// What the source row says. `tag` (optional) receives the short dim text on its right edge.
std::wstring OverlaysDockPanel::SourceText(std::wstring *tag) const {
    std::wstring text;
    std::wstring t;
    const std::string dash = " \xE2\x80\x94 ";   // " — "
    switch (state_.capture_mode) {
    case CaptureMode::Window: {
        text = WideFromNarrow(T("src_row_window", "Window") + dash);
        if (!state_.capture_window_exe.empty()) text += WideFromNarrow(state_.capture_window_exe) + L" · ";
        std::wstring title = WideFromNarrow(state_.capture_window_title);
        if (title.size() > 70) title = title.substr(0, 69) + L"…";
        if (title.empty()) title = WideFromNarrow(state_.capture_window_class);
        text += title;
        if (state_.WindowCaptureWantsWgc()) {
            const int mode = get_source_mode ? get_source_mode() : 0;
            if (mode == 1) t = WideFromNarrow(T("src_tag_native", "native"));
            else if (mode == 2) t = WideFromNarrow(T("src_tag_exact", "exact"));
            else if (mode == 3) t = WideFromNarrow(T("src_tag_smart", "smart"));
        } else {
            t = WideFromNarrow(T("src_tag_screen", "screen crop"));
        }
        break;
    }
    case CaptureMode::Region:
        text = WideFromNarrow(T("src_row_region", "Region") + dash) + std::to_wstring(state_.region_w) +
               L"×" + std::to_wstring(state_.region_h);
        break;
    default: {
        text = WideFromNarrow(T("src_row_desktop", "Desktop"));
        const auto &mons = Monitors();
        const int idx = state_.monitor_id > 0 ? state_.monitor_id - 1 : 0;
        if (mons.size() > 1 || idx > 0) {
            text += WideFromNarrow(dash + T("src_monitor_word", "Monitor")) + L" " + std::to_wstring(idx + 1);
            if (idx < (int)mons.size())
                t = std::to_wstring(mons[(size_t)idx].w) + L"×" + std::to_wstring(mons[(size_t)idx].h);
        } else if (!mons.empty()) {
            t = std::to_wstring(mons[0].w) + L"×" + std::to_wstring(mons[0].h);
        }
        break;
    }
    }
    if (tag) *tag = t;
    return text;
}

void OverlaysDockPanel::RefreshSourceRow() {
    if (!list_) return;
    std::wstring tag;
    std::wstring sig = SourceText(&tag) + L"\x1F" + tag;
    if (sig == last_source_sig_) return;
    last_source_sig_ = sig;
    RebuildList();
}

void OverlaysDockPanel::SelectOverlay(size_t idx) {
    if (!list_) return;
    SendMessageW(list_, LB_SETCURSEL, (WPARAM)(idx + 1), 0);
}

void OverlaysDockPanel::OnListMouse(UINT msg, LPARAM lparam, bool &handled) {
    POINT pt{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
    RECT rc{};
    const int row = RowAtPoint(pt, &rc);
    if (row < 0) return;
    const int idx = OverlayOfRow(row);
    const bool on_eye = idx >= 0 && pt.x >= rc.right - kEyeColW;
    if (on_eye) {
        // One click on the eye hides / shows the overlay (a double-click counts as a second click).
        SendMessageW(list_, LB_SETCURSEL, (WPARAM)row, 0);
        ToggleVisibility((size_t)idx);
        handled = true;
        return;
    }
    if (msg == WM_LBUTTONDBLCLK) {
        SendMessageW(list_, LB_SETCURSEL, (WPARAM)row, 0);
        if (row == 0) { if (on_edit_source) on_edit_source(); }
        else if (idx >= 0) { if (on_edit_overlay) on_edit_overlay((size_t)idx); }
        handled = true;
    }
}

void OverlaysDockPanel::OnListKey(WPARAM vk, bool &handled) {
    const int row = (int)SendMessageW(list_, LB_GETCURSEL, 0, 0);
    if (row < 0) return;
    const int idx = OverlayOfRow(row);
    switch (vk) {
    case VK_RETURN:
        if (row == 0) { if (on_edit_source) on_edit_source(); handled = true; }
        else if (idx >= 0) { if (on_edit_overlay) on_edit_overlay((size_t)idx); handled = true; }
        break;
    case VK_F2:
        if (idx >= 0) { RenameAt((size_t)idx); handled = true; }
        break;
    case VK_SPACE:
        if (idx >= 0) { ToggleVisibility((size_t)idx); handled = true; }
        break;
    default: break;
    }
}

void OverlaysDockPanel::OnListContextMenu(LPARAM lparam) {
    if (!list_) return;

    int x = GET_X_LPARAM(lparam), y = GET_Y_LPARAM(lparam);
    POINT screen_pt;
    int sel;

    if (x == -1 && y == -1) {
        // Keyboard-invoked (Shift+F10 / the Menu key) -- lParam doesn't
        // carry a position in that case, so anchor the menu under whatever
        // row is currently selected instead.
        sel = (int)SendMessageW(list_, LB_GETCURSEL, 0, 0);
        if (sel < 0) return;
        RECT r{};
        SendMessageW(list_, LB_GETITEMRECT, sel, (LPARAM)&r);
        screen_pt = { r.left, r.bottom };
        ClientToScreen(list_, &screen_pt);
    } else {
        screen_pt = { x, y };
        POINT client_pt = screen_pt;
        ScreenToClient(list_, &client_pt);
        sel = RowAtPoint(client_pt, nullptr);
        if (sel < 0) return; // click landed outside any actual row
        SendMessageW(list_, LB_SETCURSEL, sel, 0);
    }

    if (sel == 0) { ShowSourceContextMenu(GetParent(hwnd_), screen_pt); return; }
    const int idx = OverlayOfRow(sel);
    if (idx < 0) return;
    ShowRowContextMenu(GetParent(hwnd_), screen_pt, (size_t)idx);
}

void OverlaysDockPanel::ShowSourceContextMenu(HWND owner, POINT screen_pt) {
    const std::wstring s_settings = WideFromNarrow(T("ctx_src_settings", "Capture source…"));
    const std::wstring s_desktop  = WideFromNarrow(T("ctx_src_desktop", "Record the whole desktop"));
    const std::wstring s_window   = WideFromNarrow(T("ctx_src_window", "Pick a window…"));
    const std::wstring s_region   = WideFromNarrow(T("ctx_src_region", "Select a region…"));

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kSrcCtxSettings, s_settings.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (state_.capture_mode == CaptureMode::Desktop ? MF_CHECKED : 0), kSrcCtxDesktop, s_desktop.c_str());
    AppendMenuW(menu, MF_STRING, kSrcCtxWindow, s_window.c_str());
    AppendMenuW(menu, MF_STRING, kSrcCtxRegion, s_region.c_str());

    SetForegroundWindow(owner);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                   screen_pt.x, screen_pt.y, 0, owner, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
    case kSrcCtxSettings: if (on_edit_source) on_edit_source(); break;
    case kSrcCtxDesktop:  if (on_source_action) on_source_action(kSrcDesktop); break;
    case kSrcCtxWindow:   if (on_source_action) on_source_action(kSrcPickWindow); break;
    case kSrcCtxRegion:   if (on_source_action) on_source_action(kSrcRegion); break;
    default: break;
    }
}

void OverlaysDockPanel::ShowRowContextMenu(HWND owner, POINT screen_pt, size_t idx) {
    const OverlayDef &ov = state_.overlays[idx];
    const std::wstring s_toggle = WideFromNarrow(ov.visible ? T("ctx_hide", "Hide") : T("ctx_show", "Show"));
    const std::wstring s_rename = WideFromNarrow(T("ctx_rename", "Rename…"));
    const std::wstring s_edit   = WideFromNarrow(T("ctx_settings", "Settings…"));
    const std::wstring s_dup    = WideFromNarrow(T("ctx_duplicate", "Duplicate"));
    const std::wstring s_up     = WideFromNarrow(T("ctx_forward", "Move up"));
    const std::wstring s_down   = WideFromNarrow(T("ctx_backward", "Move down"));
    const std::wstring s_center = WideFromNarrow(T("ctx_center", "Center"));
    const std::wstring s_lock   = WideFromNarrow(T("ctx_lock", "Lock position and size"));
    const std::wstring s_prop   = WideFromNarrow(T("ctx_keep_prop", "Keep proportions when resizing"));
    const std::wstring s_del    = WideFromNarrow(T("ctx_delete", "Delete"));

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCtxToggle, s_toggle.c_str());
    AppendMenuW(menu, MF_STRING, kCtxRename, s_rename.c_str());
    // Opens the merged position + settings + preview window (see
    // overlay_editor_dialog.h) - replaces what used to be two separate
    // items here ("Edit Parameters..." and "Position Overlays...").
    AppendMenuW(menu, MF_STRING, kCtxEdit, s_edit.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCtxDuplicate, s_dup.c_str());
    AppendMenuW(menu, MF_STRING | (idx == 0 ? MF_GRAYED : 0), kCtxUp, s_up.c_str());
    AppendMenuW(menu, MF_STRING | (idx + 1 >= state_.overlays.size() ? MF_GRAYED : 0), kCtxDown, s_down.c_str());
    AppendMenuW(menu, MF_STRING | (ov.locked ? MF_GRAYED : 0), kCtxCenter, s_center.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (ov.locked ? MF_CHECKED : 0), kCtxLock, s_lock.c_str());
    AppendMenuW(menu, MF_STRING | (ov.lock_aspect ? MF_CHECKED : 0), kCtxKeepProp, s_prop.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCtxDelete, s_del.c_str());

    SetForegroundWindow(owner);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                              screen_pt.x, screen_pt.y, 0, owner, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
        case kCtxToggle:    ToggleVisibility(idx); break;
        case kCtxRename:    RenameAt(idx); break;
        case kCtxEdit:      if (on_edit_overlay) on_edit_overlay(idx); break;
        case kCtxDuplicate: DuplicateAt(idx); break;
        case kCtxUp:        MoveBy(idx, -1); break;
        case kCtxDown:      MoveBy(idx, +1); break;
        case kCtxCenter:    CenterAt(idx); break;
        case kCtxLock:
            if (idx < state_.overlays.size()) { state_.overlays[idx].locked = !state_.overlays[idx].locked; Refresh(); SelectOverlay(idx); }
            break;
        case kCtxKeepProp:
            if (idx < state_.overlays.size()) { state_.overlays[idx].lock_aspect = !state_.overlays[idx].lock_aspect; Refresh(); SelectOverlay(idx); }
            break;
        case kCtxDelete:    RemoveAt(idx); break;
        default: break; // dismissed without a choice
    }
}

void OverlaysDockPanel::Refresh() {
    if (!list_) return;

    // state_.overlays used to only ever be written out via the
    // manual "Export Settings (.hrc)..." menu item - anything set up here
    // (add/remove/rename/toggle/edit) silently vanished the moment the app
    // was closed and reopened. Refresh() runs after every single one of
    // those mutations (see the Add*/ToggleVisibility/RemoveAt/RenameAt/
    // EditParametersAt call sites below), including the initial one from
    // Create() that just repopulates the list from what LoadOverlaysOnly()
    // read at startup - so persisting here, once, covers every mutation
    // path without needing a matching save call at each one individually
    // (and the extra write on that initial no-op call is harmless, just a
    // few KB written back unchanged).
    HrcConfig::SaveOverlaysOnly(state_.overlays, HrcConfig::kOverlaysAutosavePath);
    RebuildList();
}

void OverlaysDockPanel::RebuildList() {
    if (!list_) return;
    // Keep what the person was looking at: the selected row and the scroll position (a plain
    // LB_RESETCONTENT used to throw both away - fine for 3 rows, annoying for 30, and unusable now that
    // a click on the eye rebuilds the list).
    const int sel = (int)SendMessageW(list_, LB_GETCURSEL, 0, 0);
    const int top = (int)SendMessageW(list_, LB_GETTOPINDEX, 0, 0);

    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list_, LB_RESETCONTENT, 0, 0);

    std::wstring tag;
    const std::wstring src = SourceText(&tag);
    last_source_sig_ = src + L"\x1F" + tag;
    SendMessageW(list_, LB_ADDSTRING, 0, (LPARAM)src.c_str());

    if (state_.overlays.empty()) {
        // Non-interactive hint row (HandleDrawItem paints it dim).
        const std::wstring hint = WideFromNarrow(T("layers_empty", "No overlays yet. Click + to add one."));
        SendMessageW(list_, LB_ADDSTRING, 0, (LPARAM)hint.c_str());
    } else {
        for (const auto &ov : state_.overlays) {
            SendMessageW(list_, LB_ADDSTRING, 0, (LPARAM)RowLabel(ov).c_str());
        }
    }
    const int count = (int)SendMessageW(list_, LB_GETCOUNT, 0, 0);
    if (top > 0 && top < count) SendMessageW(list_, LB_SETTOPINDEX, (WPARAM)top, 0);
    if (sel >= 0 && sel < count && !(sel == 1 && state_.overlays.empty()))
        SendMessageW(list_, LB_SETCURSEL, (WPARAM)sel, 0);
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);
}

void OverlaysDockPanel::ShowAddMenu(HWND parent, HINSTANCE hInst) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuAddText, L"Text");
    AppendMenuW(menu, MF_STRING, kMenuAddImage, L"Image");
    AppendMenuW(menu, MF_STRING, kMenuAddGif, L"GIF");
    AppendMenuW(menu, MF_STRING, kMenuAddWebcam, L"Webcam");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuAddExternal, L"External Overlay (.json + .png)\u2026");
    if (!HrInputOverlayRegistry::All().empty()) {
        AppendMenuW(menu, MF_STRING, kMenuAddInputOverlay, L"Select Input-Overlay\u2026");
    }

    RECT btn_rect{};
    GetWindowRect(GetDlgItem(parent, ID_OVDOCK_ADD), &btn_rect);

    SetForegroundWindow(parent);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                              btn_rect.left, btn_rect.bottom, 0, parent, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
        case kMenuAddText:         AddTextOverlay(parent, hInst); break;
        case kMenuAddImage:        AddImageOverlay(parent, hInst); break;
        case kMenuAddGif:          AddGifOverlay(parent, hInst); break;
        case kMenuAddWebcam:       AddWebcamOverlay(parent, hInst); break;
        case kMenuAddExternal:     AddExternalOverlay(parent, hInst); break;
        case kMenuAddInputOverlay: AddFromInputOverlayRegistry(parent, hInst); break;
        default: break; // menu dismissed without a choice
    }
}

void OverlaysDockPanel::AddTextOverlay(HWND parent, HINSTANCE hInst) {
    std::wstring text = L"New Text";
    if (!HrPromptForText(parent, hInst, L"Add Text Overlay", L"Text to display:", text)) return;

    OverlayDef ov;
    ov.id = NewOverlayId();
    ov.type = "text";
    ov.text = NarrowFromWide(text);
    ov.x = 40; ov.y = 40; ov.w = 200; ov.h = 60;
    ov.visible = true;
    state_.overlays.push_back(ov);
    if (on_overlay_added) on_overlay_added();
    Refresh();
}

void OverlaysDockPanel::AddImageOverlay(HWND parent, HINSTANCE hInst) {
    (void)hInst;
    std::string path = PickOpenFile(parent,
        L"Image files (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0All files\0*.*\0",
        L"Add Image Overlay");
    if (path.empty()) return;

    OverlayDef ov;
    ov.id = NewOverlayId();
    ov.type = "image";
    ov.image_path = path;
    ov.x = 40; ov.y = 40; ov.w = 200; ov.h = 150;
    ov.visible = true;
    state_.overlays.push_back(ov);
    if (on_overlay_added) on_overlay_added();
    Refresh();
}

void OverlaysDockPanel::AddGifOverlay(HWND parent, HINSTANCE hInst) {
    (void)hInst;
    std::string path = PickOpenFile(parent,
        L"GIF files (*.gif)\0*.gif\0All files\0*.*\0",
        L"Add GIF Overlay");
    if (path.empty()) return;

    OverlayDef ov;
    ov.id = NewOverlayId();
    ov.type = "gif";
    ov.image_path = path;
    ov.x = 40; ov.y = 40; ov.w = 200; ov.h = 150;
    ov.visible = true;
    state_.overlays.push_back(ov);
    if (on_overlay_added) on_overlay_added();
    Refresh();
}

void OverlaysDockPanel::AddWebcamOverlay(HWND parent, HINSTANCE hInst) {
    // Previously asked the user to type a raw camera index from
    // memory ("Camera index (0 = first camera)"). Enumerate what's actually
    // attached instead and let them pick it by name.
    std::vector<HrWebcamDevice> devices = HrEnumerateWebcams();
    if (devices.empty()) {
        MessageBoxW(parent,
                    L"No webcam was found. Make sure a camera is connected "
                    L"(and isn't already in use by another app), then try again.",
                    L"No Webcam Found", MB_OK | MB_ICONWARNING);
        return;
    }

    size_t chosen = 0;
    if (!HrPromptForWebcamDevice(parent, hInst, devices, chosen)) return;

    OverlayDef ov;
    ov.id = NewOverlayId();
    ov.type = "webcam";
    ov.webcam_index = devices[chosen].index;
    ov.webcam_name  = devices[chosen].name;
    ov.x = 40; ov.y = 40; ov.w = 240; ov.h = 180;
    ov.visible = true;
    state_.overlays.push_back(ov);
    if (on_overlay_added) on_overlay_added();
    Refresh();
}

void OverlaysDockPanel::AddExternalOverlay(HWND parent, HINSTANCE hInst) {
    (void)hInst;
    // Two plain files, picked directly -- NOT a .hrp plugin package. See
    // input_overlay.h for what the JSON layout needs to contain
    // ("elements": [...] with code/mapping/pos, same shape the bundled
    // input_overlay_presets plugin's assets use).
    std::string json_path = PickOpenFile(parent, L"Overlay layout (*.json)\0*.json\0", L"Choose the .json layout");
    if (json_path.empty()) return;
    std::string png_path = PickOpenFile(parent, L"Spritesheet image (*.png)\0*.png\0", L"Choose the .png spritesheet");
    if (png_path.empty()) return;

    HrInputOverlayLayout layout;
    if (!layout.Load(json_path)) {
        MessageBoxW(parent,
                    L"That .json file couldn't be read as an input-overlay layout. "
                    L"Make sure it's the plain layout file, not something else.",
                    L"Couldn't Add Overlay", MB_OK | MB_ICONWARNING);
        return;
    }

    OverlayDef ov;
    ov.id = NewOverlayId();
    ov.type = "input_overlay";
    ov.input_json_path = json_path;
    ov.input_png_path  = png_path;
    ov.x = 40; ov.y = 40;
    ov.w = layout.width  > 0 ? layout.width  : 300;
    ov.h = layout.height > 0 ? layout.height : 150;
    ov.visible = true;
    state_.overlays.push_back(ov);
    if (on_overlay_added) on_overlay_added();
    Refresh();
}

void OverlaysDockPanel::AddFromInputOverlayRegistry(HWND parent, HINSTANCE hInst) {
    const auto &sources = HrInputOverlayRegistry::All();
    size_t chosen = 0;
    if (!HrPromptForInputOverlaySource(parent, hInst, sources, chosen)) return;
    const HrInputOverlaySource &src = sources[chosen];

    HrInputOverlayLayout layout;
    if (!layout.Load(src.json_path)) {
        MessageBoxW(parent, L"That preset's layout file couldn't be read.",
                    L"Couldn't Add Overlay", MB_OK | MB_ICONWARNING);
        return;
    }

    OverlayDef ov;
    ov.id = NewOverlayId();
    ov.type = "input_overlay";
    ov.input_json_path = src.json_path;
    ov.input_png_path  = src.png_path;
    ov.x = 40; ov.y = 40;
    ov.w = layout.width  > 0 ? layout.width  : 300;
    ov.h = layout.height > 0 ? layout.height : 150;
    ov.visible = true;
    state_.overlays.push_back(ov);
    if (on_overlay_added) on_overlay_added();
    Refresh();
}

void OverlaysDockPanel::ToggleVisibility(size_t idx) {
    if (idx >= state_.overlays.size()) return;
    state_.overlays[idx].visible = !state_.overlays[idx].visible;
    Refresh();                 // keeps selection + scroll position (see RebuildList)
    SelectOverlay(idx);
}

void OverlaysDockPanel::RemoveAt(size_t idx) {
    if (idx >= state_.overlays.size()) return;
    state_.overlays.erase(state_.overlays.begin() + (ptrdiff_t)idx);
    if (on_overlay_removed) on_overlay_removed();
    Refresh();
}

void OverlaysDockPanel::RenameAt(size_t idx) {
    if (idx >= state_.overlays.size() || !list_) return;
    HWND parent = GetParent(hwnd_);
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);

    std::wstring value = WideFromNarrow(state_.overlays[idx].name);
    if (!HrPromptForText(parent, hInst, L"Rename Overlay",
                          L"Display name (leave blank to use the default label):", value)) {
        return;
    }
    state_.overlays[idx].name = NarrowFromWide(value);
    Refresh();
    SelectOverlay(idx);
}

void OverlaysDockPanel::DuplicateAt(size_t idx) {
    if (idx >= state_.overlays.size()) return;
    OverlayDef copy = state_.overlays[idx];
    copy.id = NewOverlayId();
    copy.x += 24;
    copy.y += 24;
    copy.locked = false;
    state_.overlays.insert(state_.overlays.begin() + (ptrdiff_t)idx + 1, copy);
    if (on_overlay_added) on_overlay_added();
    Refresh();
    SelectOverlay(idx + 1);
}

void OverlaysDockPanel::MoveBy(size_t idx, int delta) {
    const long target = (long)idx + delta;
    if (idx >= state_.overlays.size() || target < 0 || target >= (long)state_.overlays.size()) return;
    std::swap(state_.overlays[idx], state_.overlays[(size_t)target]);
    Refresh();
    SelectOverlay((size_t)target);
}

void OverlaysDockPanel::CenterAt(size_t idx) {
    if (idx >= state_.overlays.size()) return;
    int cw = 0, ch = 0;
    if (!get_canvas_size || !get_canvas_size(cw, ch) || cw <= 0 || ch <= 0) return;
    OverlayDef &ov = state_.overlays[idx];
    ov.x = (cw - ov.w) / 2;
    ov.y = (ch - ov.h) / 2;
    Refresh();
    SelectOverlay(idx);
}

void OverlaysDockPanel::ClosePanel() {
    state_.show_overlays_panel = false;
    PersistState(state_);
    SetVisible(false);
}

void OverlaysDockPanel::Resize(int w, int h) {
    if (!hwnd_ || w < 40 || h < 60) return;
    HWND parent = GetParent(hwnd_);
    HWND add = GetDlgItem(parent, ID_OVDOCK_ADD);
    // Batched so a sash drag repositions all three children with one repaint pass.
    HDWP dwp = BeginDeferWindowPos(3);
    auto place = [&](HWND win, int px, int py, int pw, int ph) {
        if (!win) return;
        if (dwp) dwp = DeferWindowPos(dwp, win, nullptr, px, py, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
        else     SetWindowPos(win, nullptr, px, py, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    place(hwnd_, 0, 0, w, h);
    place(add, 6, 6, 28, 24);
    place(list_, 6, 36, std::max(20, w - 12), std::max(20, h - 42));
    if (dwp) EndDeferWindowPos(dwp);
    // The sunken STATIC is the lowest sibling; raise the controls above it.
    if (add)   SetWindowPos(add,   HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (list_) SetWindowPos(list_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Raw child HWNDs are not repainted by wx when AUI re-parents the host into a floating
// frame (or back into the dock): the pane stayed blank until something else invalidated it.
void OverlaysDockPanel::ForceRepaint() {
    if (!hwnd_) return;
    HWND parent = GetParent(hwnd_);
    if (parent) RedrawWindow(parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

namespace {

// ---- small GDI icons (no resources needed, always crisp, follow the text colour) -------------------
struct PenGuard {
    HDC dc; HPEN pen, old;
    PenGuard(HDC d, COLORREF c, int w) : dc(d), pen(CreatePen(PS_SOLID, w, c)), old((HPEN)SelectObject(d, pen)) {}
    ~PenGuard() { SelectObject(dc, old); DeleteObject(pen); }
};

// The eye: an almond outline with a pupil. Hidden = dimmed + a slash through it.
void DrawEye(HDC dc, const RECT &box, bool open, COLORREF c) {
    const int w = 16, h = 9;
    const int cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
    const int x0 = cx - w / 2, x1 = cx + w / 2;
    {
        PenGuard pg(dc, c, 2);
        const POINT pts[7] = {
            { x0, cy },
            { x0 + w / 4, cy - h }, { x0 + 3 * w / 4, cy - h }, { x1, cy },
            { x0 + 3 * w / 4, cy + h }, { x0 + w / 4, cy + h }, { x0, cy },
        };
        BeginPath(dc);
        PolyBezier(dc, pts, 7);
        EndPath(dc);
        StrokePath(dc);
        HBRUSH br = CreateSolidBrush(c);
        HBRUSH old = (HBRUSH)SelectObject(dc, br);
        const int r = open ? 3 : 2;
        Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
        SelectObject(dc, old);
        DeleteObject(br);
    }
    if (!open) {
        PenGuard pg(dc, c, 2);
        MoveToEx(dc, x0 + 1, cy + h - 1, nullptr);
        LineTo(dc, x1, cy - h + 1);
    }
}

void DrawLock(HDC dc, int x, int y, COLORREF c) {
    PenGuard pg(dc, c, 1);
    HBRUSH br = CreateSolidBrush(c);
    HBRUSH old = (HBRUSH)SelectObject(dc, br);
    Rectangle(dc, x, y + 5, x + 10, y + 12);
    SelectObject(dc, old);
    DeleteObject(br);
    Arc(dc, x + 1, y, x + 9, y + 10, x + 9, y + 5, x + 1, y + 5);
}

// kind: 0 monitor, 1 window, 2 region
void DrawSourceIcon(HDC dc, int x, int y, int kind, COLORREF c) {
    PenGuard pg(dc, c, 2);
    HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH old = (HBRUSH)SelectObject(dc, nb);
    if (kind == 0) {                       // monitor with a stand
        Rectangle(dc, x, y + 1, x + 18, y + 13);
        MoveToEx(dc, x + 9, y + 13, nullptr); LineTo(dc, x + 9, y + 17);
        MoveToEx(dc, x + 5, y + 17, nullptr); LineTo(dc, x + 14, y + 17);
    } else if (kind == 1) {                // window with a title bar
        Rectangle(dc, x, y + 2, x + 18, y + 16);
        MoveToEx(dc, x, y + 6, nullptr); LineTo(dc, x + 18, y + 6);
    } else {                               // dashed region frame
        LOGBRUSH lb{ BS_NULL, 0, 0 };
        (void)lb;
        PenGuard dash(dc, c, 1);
        HPEN dp = CreatePen(PS_DASH, 1, c);
        HPEN op = (HPEN)SelectObject(dc, dp);
        Rectangle(dc, x, y + 2, x + 18, y + 16);
        SelectObject(dc, op);
        DeleteObject(dp);
    }
    SelectObject(dc, old);
}

} // namespace

void OverlaysDockPanel::HandleDrawItem(DRAWITEMSTRUCT *dis, bool dark) {
    if (!dis || dis->CtlType != ODT_LISTBOX || dis->itemID == (UINT)-1) return;
    const COLORREF bg   = dark ? RGB(0x31, 0x32, 0x44) : RGB(0xff, 0xff, 0xff);
    const COLORREF bgSel= dark ? RGB(0x45, 0x47, 0x5a) : RGB(0xcc, 0xdd, 0xff);
    const COLORREF bgSrc= dark ? RGB(0x28, 0x2a, 0x3c) : RGB(0xe9, 0xee, 0xfa);
    const COLORREF bgSrcSel = dark ? RGB(0x3a, 0x3d, 0x55) : RGB(0xd2, 0xdf, 0xfb);
    const COLORREF fg   = dark ? RGB(0xcd, 0xd6, 0xf4) : RGB(0x20, 0x20, 0x30);
    const COLORREF fgDim= dark ? RGB(0x7f, 0x84, 0x9c) : RGB(0x90, 0x90, 0xa0);
    const COLORREF line = dark ? RGB(0x45, 0x47, 0x5a) : RGB(0xc4, 0xcc, 0xe0);
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;

    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool is_source = dis->itemID == 0;
    HBRUSH br = CreateSolidBrush(is_source ? (selected ? bgSrcSel : bgSrc) : (selected ? bgSel : bg));
    FillRect(dc, &rc, br);
    DeleteObject(br);

    wchar_t buf[512] = {};
    SendMessageW(list_, LB_GETTEXT, dis->itemID, (LPARAM)buf);
    SetBkMode(dc, TRANSPARENT);
    const int rowH = rc.bottom - rc.top;

    // ---- row 0: the capture source -------------------------------------------------------------
    if (is_source) {
        int x = rc.left + 8;
        const int kind = state_.capture_mode == CaptureMode::Window ? 1 : state_.capture_mode == CaptureMode::Region ? 2 : 0;
        DrawSourceIcon(dc, x, rc.top + (rowH - 18) / 2, kind, fg);
        x += 18 + 8;

        std::wstring tag;
        SourceText(&tag);
        int right = rc.right - 8;
        if (!tag.empty()) {
            SetTextColor(dc, fgDim);
            RECT tr_ = { x, rc.top, right, rc.bottom };
            DrawTextW(dc, tag.c_str(), -1, &tr_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SIZE sz{};
            GetTextExtentPoint32W(dc, tag.c_str(), (int)tag.size(), &sz);
            right -= sz.cx + 10;
        }
        SetTextColor(dc, fg);
        RECT tr_ = { x, rc.top, std::max(x + 10, (int)right), rc.bottom };
        DrawTextW(dc, buf, -1, &tr_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        // a thin line under the pinned row separates it from the overlays
        PenGuard pg(dc, line, 1);
        MoveToEx(dc, rc.left, rc.bottom - 1, nullptr);
        LineTo(dc, rc.right, rc.bottom - 1);
        return;
    }

    // ---- the "no overlays yet" hint row --------------------------------------------------------
    if (state_.overlays.empty() || dis->itemID - 1 >= state_.overlays.size()) {
        SetTextColor(dc, fgDim);
        RECT tr_ = rc; tr_.left += 8; tr_.right -= 8;
        DrawTextW(dc, buf, -1, &tr_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    // ---- an overlay row: [icon] label ........ [lock] [eye] -----------------------------------
    const OverlayDef &ov = state_.overlays[dis->itemID - 1];
    const int iconPx = 20;
    int x = rc.left + 8;
    const COLORREF tc = ov.visible ? fg : fgDim;

    HrIcons::Id iid;
    HICON hi = nullptr;
    if (HrIcons::IdForOverlayType(ov.type, iid)) hi = HrIcons::GetHIcon(iid, iconPx);
    if (hi) {
        DrawIconEx(dc, x, rc.top + (rowH - iconPx) / 2, hi, iconPx, iconPx, 0, nullptr, DI_NORMAL);
    } else {
        SetTextColor(dc, tc);
        RECT gr = { x, rc.top, x + iconPx, rc.bottom };
        DrawTextW(dc, GlyphFor(ov), -1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    x += iconPx + 8;

    // the eye column on the right edge (kEyeColW wide - OnListMouse() uses the same width)
    RECT eye = { rc.right - kEyeColW, rc.top, rc.right - 4, rc.bottom };
    DrawEye(dc, eye, ov.visible, ov.visible ? fg : fgDim);
    int right = rc.right - kEyeColW - 2;
    if (ov.locked) {
        DrawLock(dc, right - 12, rc.top + (rowH - 12) / 2, fgDim);
        right -= 18;
    }

    SetTextColor(dc, tc);
    RECT tr_ = { x, rc.top, std::max(x + 10, right), rc.bottom };
    DrawTextW(dc, buf, -1, &tr_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (dis->itemState & ODS_FOCUS) { /* no dotted focus rectangle - the highlight is enough */ }
}

void OverlaysDockPanel::SetVisible(bool visible) {
    int cmd = visible ? SW_SHOW : SW_HIDE;
    ShowWindow(hwnd_, cmd);
    // Sibling buttons/list were created as children of `parent`, not of
    // hwnd_ (a plain STATIC used only as a visual frame here), so they
    // have to be hidden individually too.
    HWND parent = GetParent(hwnd_);
    for (int id : { ID_OVDOCK_ADD, ID_OVDOCK_LIST }) {
        HWND ctrl = GetDlgItem(parent, id);
        if (ctrl) ShowWindow(ctrl, cmd);
    }
}

void OverlaysDockPanel::OnCommand(int id) {
    HWND parent = GetParent(hwnd_);
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    switch (id) {
        case ID_OVDOCK_ADD:      ShowAddMenu(parent, hInst); break;
        case ID_OVDOCK_CLOSE:    ClosePanel(); break;
        default: break;
    }
}
