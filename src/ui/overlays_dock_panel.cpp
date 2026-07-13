#include "overlays_dock_panel.h"
#include "overlay_manager.h"
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

// Mirrors OverlaysDockPanel.refresh()'s per-row label logic in
// overlays_dock_panel.py (dot + kind icon + truncated name), adapted to
// this port's OverlayDef field names (type/visible/image_path/
// webcam_index instead of kind/enabled/path/cam_index).
std::wstring RowLabel(const OverlayDef &ov) {
    const wchar_t *dot = ov.visible ? L"\u25CF" : L"\u25CB"; // ● / ○
    const wchar_t *icon = L"?";
    std::wstring name;

    if (ov.type == "text") {
        icon = L"\U0001F4DD"; // 📝
        name = WideFromNarrow(ov.text);
        if (name.empty()) name = L"(empty text)";
    } else if (ov.type == "image") {
        icon = L"\U0001F5BC"; // 🖼
        std::string path = ov.image_path;
        size_t slash = path.find_last_of("\\/");
        std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
        name = base.empty() ? L"(no file)" : WideFromNarrow(base);
    } else { // "webcam"
        icon = L"\U0001F4F7"; // 📷
        name = L"Cam#" + std::to_wstring(ov.webcam_index);
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

} // namespace

OverlaysDockPanel::OverlaysDockPanel(AppState &state) : state_(state) {}

HWND OverlaysDockPanel::Create(HWND parent, HINSTANCE hInst, int x, int y, int w, int h) {
    hwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
                             WS_CHILD | (state_.show_overlays_panel ? WS_VISIBLE : 0) | SS_SUNKEN,
                             x, y, w, h, parent, nullptr, hInst, nullptr);

    int cy = y + 6;
    CreateWindowExW(0, L"BUTTON", L"\uFF0B", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     x + 6, cy, 28, 24, parent, (HMENU)ID_OVDOCK_ADD, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"\U0001F441 Position", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     x + 40, cy, 100, 24, parent, (HMENU)ID_OVDOCK_POSITION, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"\u2715", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     x + w - 34, cy, 28, 24, parent, (HMENU)ID_OVDOCK_CLOSE, hInst, nullptr);

    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
                             x + 6, cy + 30, w - 12, h - 106, parent, (HMENU)ID_OVDOCK_LIST, hInst, nullptr);

    CreateWindowExW(0, L"BUTTON", L"More\u2026", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     x + 6, y + h - 34, 70, 26, parent, (HMENU)ID_OVDOCK_MORE, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     x + 82, y + h - 34, 70, 26, parent, (HMENU)ID_OVDOCK_REMOVE, hInst, nullptr);

    Refresh();
    SetVisible(state_.show_overlays_panel);
    return hwnd_;
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

void OverlaysDockPanel::QuickAdd() {
    OverlayDef ov;
    ov.id = "ov_" + std::to_string(next_id_++);
    ov.type = "text";
    ov.text = "New Text";
    ov.x = 40; ov.y = 40; ov.w = 200; ov.h = 60;
    ov.visible = true;
    state_.overlays.push_back(ov);
    Refresh();
    // NOTE: like the rest of this port's overlay handling (see
    // overlay_manager.cpp), this only changes the in-memory AppState —
    // there's no hr_profile_io/hr_settings persistence for the overlays
    // list itself yet, so a restart still loses them. Python's
    // save_settings(silent=True) call here has no real C++ counterpart
    // to call into; flagging rather than pretending this persists.
}

void OverlaysDockPanel::RemoveSelected() {
    if (!list_) return;
    int sel = (int)SendMessageW(list_, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)state_.overlays.size()) return;
    state_.overlays.erase(state_.overlays.begin() + sel);
    Refresh();
}

void OverlaysDockPanel::OpenMoreForSelected(HWND parent, HINSTANCE hInst) {
    // Python's _open_more() opens OverlayManagerWindow preselected to this
    // row; ShowOverlayManager() has no preselect parameter (it's a plain
    // list-and-buttons window, see overlay_manager.h), so this opens the
    // same manager window without preselection — same destination, one
    // fewer click saved than the Python version for this specific path.
    ShowOverlayManager(parent, hInst, state_);
    Refresh();
}

void OverlaysDockPanel::OpenDragPreview(HWND parent, HINSTANCE hInst) {
    ShowOverlayDragPreview(parent, hInst, state_);
    Refresh();
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
    // hwnd_ (a plain STATIC used only as a visual frame here — Win32
    // static controls aren't proper containers the way a Tk Frame is), so
    // they have to be hidden individually too.
    HWND parent = GetParent(hwnd_);
    for (int id : { ID_OVDOCK_ADD, ID_OVDOCK_POSITION, ID_OVDOCK_CLOSE,
                     ID_OVDOCK_LIST, ID_OVDOCK_MORE, ID_OVDOCK_REMOVE }) {
        HWND ctrl = GetDlgItem(parent, id);
        if (ctrl) ShowWindow(ctrl, cmd);
    }
}

void OverlaysDockPanel::OnCommand(int id) {
    HWND parent = GetParent(hwnd_);
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    switch (id) {
        case ID_OVDOCK_ADD:      QuickAdd(); break;
        case ID_OVDOCK_REMOVE:   RemoveSelected(); break;
        case ID_OVDOCK_MORE:     OpenMoreForSelected(parent, hInst); break;
        case ID_OVDOCK_POSITION: OpenDragPreview(parent, hInst); break;
        case ID_OVDOCK_CLOSE:    ClosePanel(); break;
        default: break;
    }
}
