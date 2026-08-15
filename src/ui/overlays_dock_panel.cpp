#include "overlays_dock_panel.h"
#include "win32_theme.h"
#include "overlay_add_dialogs.h"
#include "../hr_input_overlay.h"
#include "../hr_input_overlay_registry.h"
#include "../hr_webcam_enum.h"
#include <commdlg.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <string>

extern "C" {
    void *hr_settings_create();
    void hr_settings_destroy(void *handle);
    int hr_settings_load(void *handle, const char *path);
    int hr_settings_save(const void *handle, const char *path);
    void hr_settings_set_flag(void *h, const char *name, int v);
}

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
    // BUGFIX: without OFN_NOCHANGEDIR, GetOpenFileNameW() silently changes
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
std::wstring RowLabel(const OverlayDef &ov) {
    const wchar_t *dot = ov.visible ? L"\u25CF" : L"\u25CB"; // ● / ○
    const wchar_t *icon = L"?";
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
    if (name.size() > 14) name = name.substr(0, 14);

    std::wstring row = dot;
    row += L" ";
    row += icon;
    row += L" ";
    row += name;
    return row;
}

void PersistShowOverlaysPanelFlag(bool show) {
    void *settings = hr_settings_create();
    hr_settings_load(settings, "homrec_settings.json");
    hr_settings_set_flag(settings, "show_overlays_panel", show ? 1 : 0);
    hr_settings_save(settings, "homrec_settings.json");
    hr_settings_destroy(settings);
}

// TrackPopupMenu(TPM_RETURNCMD) result codes for the "+" menu -- local to
// this file, never go through WM_COMMAND, so no risk of colliding with the
// ID_OVDOCK_* control ids.
enum { kMenuAddText = 1, kMenuAddImage, kMenuAddWebcam, kMenuAddExternal, kMenuAddInputOverlay };

// TrackPopupMenu(TPM_RETURNCMD) result codes for the per-row right-click
// menu -- same reasoning as kMenuAdd* above, a separate numbering space
// local to this file.
enum { kCtxToggle = 1, kCtxRename, kCtxEdit, kCtxDelete, kCtxApplyNoPreview, kCtxRefreshSnapshot };

} // namespace

OverlaysDockPanel::OverlaysDockPanel(AppState &state) : state_(state) {}

HWND OverlaysDockPanel::Create(HWND parent, HINSTANCE hInst, int x, int y, int w, int h) {
    hwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
                             WS_CHILD | (state_.show_overlays_panel ? WS_VISIBLE : 0) | SS_SUNKEN,
                             x, y, w, h, parent, nullptr, hInst, nullptr);

    int cy = y + 6;
    HrWin32Theme::ThemeButton(CreateWindowExW(0, L"BUTTON", L"\uFF0B", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     x + 6, cy, 28, 24, parent, (HMENU)ID_OVDOCK_ADD, hInst, nullptr));
    HrWin32Theme::ThemeButton(CreateWindowExW(0, L"BUTTON", L"\u2715", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     x + w - 34, cy, 28, 24, parent, (HMENU)ID_OVDOCK_CLOSE, hInst, nullptr));

    // Bottom buttons that used to live here (Show/Hide, Remove) are gone --
    // right-clicking a row now covers both of those plus Rename/Edit
    // Parameters (see ShowRowContextMenu()), so the list gets that
    // reclaimed vertical space instead.
    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
                             x + 6, cy + 30, w - 12, h - 42, parent, (HMENU)ID_OVDOCK_LIST, hInst, nullptr);

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
    if (msg == WM_CONTEXTMENU && self) {
        self->OnListContextMenu(lParam);
        return 0;
    }
    WNDPROC orig = self ? self->orig_list_proc_ : nullptr;
    return orig ? CallWindowProcW(orig, hwnd, msg, wParam, lParam)
                : DefWindowProcW(hwnd, msg, wParam, lParam);
}

void OverlaysDockPanel::OnListContextMenu(LPARAM lparam) {
    if (!list_ || state_.overlays.empty()) return;

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
        LRESULT hit = SendMessageW(list_, LB_ITEMFROMPOINT, 0, MAKELPARAM(client_pt.x, client_pt.y));
        if (HIWORD(hit) != 0) return; // click landed outside any actual row
        sel = LOWORD(hit);
        SendMessageW(list_, LB_SETCURSEL, sel, 0);
    }

    if (sel < 0 || sel >= (int)state_.overlays.size()) return;
    ShowRowContextMenu(GetParent(hwnd_), screen_pt, (size_t)sel);
}

void OverlaysDockPanel::ShowRowContextMenu(HWND owner, POINT screen_pt, size_t idx) {
    const OverlayDef &ov = state_.overlays[idx];

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCtxToggle, ov.visible ? L"Hide" : L"Show");
    AppendMenuW(menu, MF_STRING, kCtxRename, L"Rename\u2026");
    AppendMenuW(menu, MF_STRING, kCtxEdit,   L"Edit Parameters\u2026");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCtxDelete, L"Delete");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // Live preview normally has to be running for the drag-to-position/
    // resize handles on the preview panel to appear at all - if the user's
    // turned it off (Settings > Disable live preview), this is the only
    // way left to reposition an overlay. Grabs a one-off screenshot and
    // shows every overlay (not just this row's) on top of it in the same
    // preview panel, editable the normal way; "Refresh" re-takes the
    // screenshot without leaving that mode.
    AppendMenuW(menu, MF_STRING, kCtxApplyNoPreview, L"Apply with preview off");
    AppendMenuW(menu, MF_STRING, kCtxRefreshSnapshot, L"Refresh screenshot");

    SetForegroundWindow(owner);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                              screen_pt.x, screen_pt.y, 0, owner, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
        case kCtxToggle: ToggleVisibility(idx); break;
        case kCtxRename: RenameAt(idx); break;
        case kCtxEdit:   EditParametersAt(idx); break;
        case kCtxDelete: RemoveAt(idx); break;
        case kCtxApplyNoPreview: if (on_apply_no_preview) on_apply_no_preview(false); break;
        case kCtxRefreshSnapshot: if (on_apply_no_preview) on_apply_no_preview(true); break;
        default: break; // dismissed without a choice
    }
}

void OverlaysDockPanel::Refresh() {
    if (!list_) return;
    SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    if (state_.overlays.empty()) {
        SendMessageW(list_, LB_ADDSTRING, 0, (LPARAM)L"No overlays yet. Click \uFF0B to add one.");
        EnableWindow(list_, FALSE);
        return;
    }
    EnableWindow(list_, TRUE);
    for (const auto &ov : state_.overlays) {
        SendMessageW(list_, LB_ADDSTRING, 0, (LPARAM)RowLabel(ov).c_str());
    }
}

void OverlaysDockPanel::ShowAddMenu(HWND parent, HINSTANCE hInst) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuAddText, L"Text");
    AppendMenuW(menu, MF_STRING, kMenuAddImage, L"Image");
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
    ov.id = "ov_" + std::to_string(next_id_++);
    ov.type = "text";
    ov.text = NarrowFromWide(text);
    ov.x = 40; ov.y = 40; ov.w = 200; ov.h = 60;
    ov.visible = true;
    state_.overlays.push_back(ov);
    Refresh();
}

void OverlaysDockPanel::AddImageOverlay(HWND parent, HINSTANCE hInst) {
    (void)hInst;
    std::string path = PickOpenFile(parent,
        L"Image files (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0All files\0*.*\0",
        L"Add Image Overlay");
    if (path.empty()) return;

    OverlayDef ov;
    ov.id = "ov_" + std::to_string(next_id_++);
    ov.type = "image";
    ov.image_path = path;
    ov.x = 40; ov.y = 40; ov.w = 200; ov.h = 150;
    ov.visible = true;
    state_.overlays.push_back(ov);
    Refresh();
}

void OverlaysDockPanel::AddWebcamOverlay(HWND parent, HINSTANCE hInst) {
    // BUGFIX: previously asked the user to type a raw camera index from
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
    ov.id = "ov_" + std::to_string(next_id_++);
    ov.type = "webcam";
    ov.webcam_index = devices[chosen].index;
    ov.webcam_name  = devices[chosen].name;
    ov.x = 40; ov.y = 40; ov.w = 240; ov.h = 180;
    ov.visible = true;
    state_.overlays.push_back(ov);
    Refresh();
}

void OverlaysDockPanel::AddExternalOverlay(HWND parent, HINSTANCE hInst) {
    (void)hInst;
    // Two plain files, picked directly -- NOT a .hrp plugin package. See
    // hr_input_overlay.h for what the JSON layout needs to contain
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
    ov.id = "ov_" + std::to_string(next_id_++);
    ov.type = "input_overlay";
    ov.input_json_path = json_path;
    ov.input_png_path  = png_path;
    ov.x = 40; ov.y = 40;
    ov.w = layout.width  > 0 ? layout.width  : 300;
    ov.h = layout.height > 0 ? layout.height : 150;
    ov.visible = true;
    state_.overlays.push_back(ov);
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
    ov.id = "ov_" + std::to_string(next_id_++);
    ov.type = "input_overlay";
    ov.input_json_path = src.json_path;
    ov.input_png_path  = src.png_path;
    ov.x = 40; ov.y = 40;
    ov.w = layout.width  > 0 ? layout.width  : 300;
    ov.h = layout.height > 0 ? layout.height : 150;
    ov.visible = true;
    state_.overlays.push_back(ov);
    Refresh();
}

void OverlaysDockPanel::ToggleVisibility(size_t idx) {
    if (idx >= state_.overlays.size()) return;
    state_.overlays[idx].visible = !state_.overlays[idx].visible;
    Refresh();
    SendMessageW(list_, LB_SETCURSEL, (WPARAM)idx, 0); // Refresh() clears selection; restore it
}

void OverlaysDockPanel::RemoveAt(size_t idx) {
    if (idx >= state_.overlays.size()) return;
    state_.overlays.erase(state_.overlays.begin() + (ptrdiff_t)idx);
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
    SendMessageW(list_, LB_SETCURSEL, (WPARAM)idx, 0);
}

void OverlaysDockPanel::EditParametersAt(size_t idx) {
    if (idx >= state_.overlays.size() || !list_) return;
    HWND parent = GetParent(hwnd_);
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    OverlayDef &ov = state_.overlays[idx];

    if (ov.type == "text") {
        std::wstring value = WideFromNarrow(ov.text);
        if (!HrPromptForText(parent, hInst, L"Edit Text Overlay", L"Text to display:", value)) return;
        ov.text = NarrowFromWide(value);
    } else if (ov.type == "image") {
        std::string path = PickOpenFile(parent,
            L"Image files (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0All files\0*.*\0",
            L"Change Image");
        if (path.empty()) return;
        ov.image_path = path;
    } else if (ov.type == "webcam") {
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
        ov.webcam_index = devices[chosen].index;
        ov.webcam_name  = devices[chosen].name;
    } else if (ov.type == "input_overlay") {
        // Covers both how an input_overlay can be added -- a manually-picked
        // "External Overlay" .json+.png pair, or a plugin-registered preset
        // -- by simply letting the user re-pick both files, same as adding
        // an External Overlay does. That's a strict superset of "re-pick a
        // preset", since any registered preset is itself just a .json+.png
        // pair on disk (see hr_input_overlay_registry.h).
        std::string json_path = PickOpenFile(parent, L"Overlay layout (*.json)\0*.json\0",
                                              L"Choose the .json layout");
        if (json_path.empty()) return;
        std::string png_path = PickOpenFile(parent, L"Spritesheet image (*.png)\0*.png\0",
                                             L"Choose the .png spritesheet");
        if (png_path.empty()) return;

        HrInputOverlayLayout layout;
        if (!layout.Load(json_path)) {
            MessageBoxW(parent,
                        L"That .json file couldn't be read as an input-overlay layout. "
                        L"Make sure it's the plain layout file, not something else.",
                        L"Couldn't Change Overlay", MB_OK | MB_ICONWARNING);
            return;
        }
        ov.input_json_path = json_path;
        ov.input_png_path  = png_path;
    } else {
        return;
    }
    Refresh();
    SendMessageW(list_, LB_SETCURSEL, (WPARAM)idx, 0);
}

void OverlaysDockPanel::ClosePanel() {
    state_.show_overlays_panel = false;
    PersistShowOverlaysPanelFlag(false);
    SetVisible(false);
}

void OverlaysDockPanel::SetVisible(bool visible) {
    int cmd = visible ? SW_SHOW : SW_HIDE;
    ShowWindow(hwnd_, cmd);
    // Sibling buttons/list were created as children of `parent`, not of
    // hwnd_ (a plain STATIC used only as a visual frame here), so they
    // have to be hidden individually too.
    HWND parent = GetParent(hwnd_);
    for (int id : { ID_OVDOCK_ADD, ID_OVDOCK_CLOSE, ID_OVDOCK_LIST }) {
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
