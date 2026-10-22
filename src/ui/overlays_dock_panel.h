// overlays_dock_panel.h - Panel-only pass
#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include <vector>
#include "app_state.h"
#include "window_picker_dialog.h"   // HrMonitorInfo

class OverlaysDockPanel {
public:
    explicit OverlaysDockPanel(AppState &state);
    // 2.4 fix pass 2: un-subclasses the list. The list HWND is owned by the wx host panel and
    // outlives this object during frame teardown; its subclass proc used to keep a dangling
    // `this` in GWLP_USERDATA and call through freed memory -> crash on close.
    ~OverlaysDockPanel();

    HWND Create(HWND parent, HINSTANCE hInst, int x, int y, int w, int h);

    // Rebuilds the visible list from state_.overlays.
    void Refresh();

    void OnCommand(int id);

    // 2.4: lays the native children out for a w x h host (called on every host WM_SIZE).
    void Resize(int w, int h);

    // Invalidates + repaints the whole panel (after AUI floats/docks the host window).
    void ForceRepaint();

    // 2.4: WM_DRAWITEM for the owner-drawn list. `dark` selects the palette.
    void HandleDrawItem(DRAWITEMSTRUCT *dis, bool dark);

    // Shows/hides the panel's HWNDs to match AppState.show_overlays_panel
    // without destroying/recreating them.
    void SetVisible(bool visible);

    HWND hwnd() const { return hwnd_; }

    // "Edit Overlay..." (row context menu) - opens the merged position + settings + preview
    // window for the overlay at this index (see the header comment above and
    // overlay_editor_dialog.h).
    std::function<void(size_t idx)> on_edit_overlay;
    std::function<void()> on_overlay_added;
    std::function<void()> on_overlay_removed;

    // 7th pass - wiring done by main_frame.cpp (the panel itself knows neither the language table nor the
    // recorder):
    // tr               - text for a language key ("" -> the key itself is shown)
    enum SourceAction { kSrcToggleDesktop = 1 };   // the Desktop row's eye (also Space / context menu)
    std::function<std::string(const char *key)> tr;
    std::function<bool(int &w, int &h)> get_canvas_size;
    std::function<int()> get_source_mode;
    std::function<void()> on_edit_source;
    std::function<void(int action)> on_source_action;

    // Re-reads what the source row shows (called from the UI timer; repaints only when it changed).
    void RefreshSourceRow();

private:
    // Popup menu anchored under the "+" button; dispatches to one of the
    // Add* methods below based on what was picked.
    void ShowAddMenu(HWND parent, HINSTANCE hInst);

    // 8th pass: window / region recording as layers (picker -> new layer fitted into the picture).
    void AddWindowLayer(HWND parent, HINSTANCE hInst);
    void AddRegionLayer(HWND parent, HINSTANCE hInst);
    void AddTextOverlay(HWND parent, HINSTANCE hInst);
    void AddImageOverlay(HWND parent, HINSTANCE hInst);
    // "GIF" -- same idea as AddImageOverlay, but restricted to .gif and
    // stored with type == "gif" so the capture pipeline (hr_overlay_render)
    // animates it frame-by-frame instead of treating it as a static image.
    void AddGifOverlay(HWND parent, HINSTANCE hInst);
    void AddWebcamOverlay(HWND parent, HINSTANCE hInst);
    // "External Overlay" -- the user picks a .json layout file and a .png spritesheet directly
    // (two plain file pickers) -- NOT a .hrp plugin package; see input_overlay.h's
    // HrInputOverlayLayout for what the .json needs to look like.
    void AddExternalOverlay(HWND parent, HINSTANCE hInst);
    // "Select Input-Overlay" -- lists presets a plugin has registered via
    // homrec.register_input_overlay() (input_overlay_registry.h) and adds the chosen one.
    void AddFromInputOverlayRegistry(HWND parent, HINSTANCE hInst);

    void ToggleVisibility(size_t idx);
    void RemoveAt(size_t idx);
    void RenameAt(size_t idx);
    void DuplicateAt(size_t idx);
    void MoveBy(size_t idx, int delta);     // -1 = one row up, +1 = one row down
    void CenterAt(size_t idx);
    void ClosePanel();

    // 7th pass list helpers. Row 0 is the capture source, row r >= 1 is overlay r-1 (or the "no overlays yet"
    // hint when there are none).
    void RebuildList();                      // refills the rows, keeps selection and scroll position
    void SelectOverlay(size_t idx);
    int  RowAtPoint(POINT client_pt, RECT *rc_out) const;
    int  OverlayOfRow(int row) const;        // -1 for the source row / the hint row
    void OnListMouse(UINT msg, LPARAM lparam, bool &handled);
    void OnListKey(WPARAM vk, bool &handled);
    void ShowSourceContextMenu(HWND owner, POINT screen_pt);
    std::string T(const char *key, const char *fallback) const;
    std::wstring SourceText(std::wstring *tag) const;
    std::string NewOverlayId() const;

    // Right-click / keyboard context-menu support for a single row -- list_ is subclassed (see
    // Create()) so this panel can react to WM_CONTEXTMENU sent to it directly, without
    // main_frame.cpp needing to know or care that rows have a menu at all.
    void OnListContextMenu(LPARAM lparam);
    void ShowRowContextMenu(HWND owner, POINT screen_pt, size_t idx);
    static LRESULT CALLBACK ListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    AppState &state_;
    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;
    WNDPROC orig_list_proc_ = nullptr;
    int next_id_ = 1; // resets per session; NewOverlayId() skips ids that are already taken
    std::wstring last_source_sig_;
    // Monitor list for the source row: enumerating displays is not free and the row is repainted often,
    // so it is re-read at most every few seconds (a monitor plugged in shows up within that).
    const std::vector<HrMonitorInfo> &Monitors() const;
    mutable std::vector<HrMonitorInfo> mon_cache_;
    mutable DWORD mon_cache_tick_ = 0;
};

enum OverlaysDockPanelControlId {
    ID_OVDOCK_ADD      = 3001,
    ID_OVDOCK_CLOSE    = 3003,
    ID_OVDOCK_LIST     = 3004,
};
