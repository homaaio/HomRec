// overlays_dock_panel.h - Panel-only pass
//
// Port of homrec_app/dialogs/overlays_dock_panel.py's OverlaysDockPanel.
//
// UPDATE: the separate full editor ("overlay window", ShowOverlayManager)
// and the separate drag-to-position popup (ShowOverlayDragPreview) - both
// previously in overlay_manager.cpp/.h, now deleted - are gone per an
// explicit ask to keep only this panel. Position/resize already happens by
// dragging the overlay rectangles directly on the live preview in the main
// window (see main_frame.cpp's drag_overlay_index_ handling), which made
// the separate "Position" popup redundant, not just extra.
//
// Since there's no other place left to add/edit overlays, this panel's "+"
// button now opens a small dropdown itself (Text / Image / GIF / Webcam /
// External Overlay / Select Input-Overlay - the last one only appears if a
// plugin has registered any presets, see input_overlay_registry.h) and
// prompts for whatever it needs up front (see overlay_add_dialogs.h),
// rather than adding a placeholder to edit later in a now-nonexistent
// editor.
//
// UPDATE: the separate "Show/Hide"/"Remove" buttons that used to sit below
// the list are gone -- right-clicking a row (or selecting it and pressing
// the Menu/Shift+F10 key) now opens a context menu with Hide/Show, Rename,
// Edit Overlay, and Delete for that row instead, freeing up the vertical
// space those two buttons took for the list itself. "Rename" sets
// OverlayDef::name (a free-form label shown in place of the row's usual
// auto-generated one).
//
// UPDATE: the old "Edit Parameters..." (a sequence of raw text-entry
// popups - text/image path/webcam device, opacity, then a font picker for
// text overlays) and "Position Overlays..." (a separate full-screen,
// every-overlay-at-once drag window, ShowOverlayPlacementDialog - now
// deleted along with overlay_placement_dialog.h/.cpp, superseded below)
// context menu items are now a single "Edit Overlay..." item, per an
// explicit ask: this panel has no access to RecordingController/theme_
// itself (main_frame owns both), so it asks main_frame.cpp to open the
// merged position+settings+preview window (see overlay_editor_dialog.h)
// via on_edit_overlay instead of doing either of the two old flows itself.
//
// 7th pass: the panel is now called "Layers" and its FIRST row is not an overlay but the capture source -
// what is being recorded (desktop + monitor, one window, a region). Double-click it (or its context menu)
// opens the capture-source window that replaced File > Select Window / Region / Full Desktop. Overlays
// follow from row 2; rows lower in the list are drawn over the ones above them. Every overlay row has an
// eye on its right edge that hides / shows the overlay with one click; double-click a row opens that
// overlay's settings; the context menu got Duplicate, Move up/down, Center, Lock position and size and
// Keep proportions.
//
// 2.4: the panel now reflows - Resize(w, h) re-positions the raw child controls, which
// is what lets it live in a resizable / floating dock pane - and the list is owner-drawn
// (HandleDrawItem) so each row can show the PNG icon for its overlay type
// (icons/overlays/text.png, gif.png, image.png, webcam.png; see icons.h). Rows fall
// back to the old emoji glyph when a PNG is missing.
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

    // "Edit Overlay..." (row context menu) - opens the merged position +
    // settings + preview window for the overlay at this index (see the
    // header comment above and overlay_editor_dialog.h). This panel has no
    // access to RecordingController/theme_ itself (main_frame owns both),
    // so it just asks main_frame.cpp to open it.
    std::function<void(size_t idx)> on_edit_overlay;
    std::function<void()> on_overlay_added;
    std::function<void()> on_overlay_removed;

    // 7th pass - wiring done by main_frame.cpp (the panel itself knows neither the language table nor the
    // recorder):
    //   tr               - text for a language key ("" -> the key itself is shown)
    //   get_canvas_size  - native size of the picture the overlays are placed in ("Center" needs it)
    //   get_source_mode  - RecordingController::WindowSourceMode() (tag on the source row)
    //   on_edit_source   - open the capture-source window
    //   on_source_action - quick actions of the source row's context menu
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
    // "External Overlay" -- the user picks a .json layout file and a .png
    // spritesheet directly (two plain file pickers) -- NOT a .hrp plugin
    // package; see input_overlay.h's HrInputOverlayLayout for what the
    // .json needs to look like.
    void AddExternalOverlay(HWND parent, HINSTANCE hInst);
    // "Select Input-Overlay" -- lists presets a plugin has registered via
    // homrec.register_input_overlay() (input_overlay_registry.h) and
    // adds the chosen one. Only reachable from the menu when at least one
    // preset is registered.
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

    // Right-click / keyboard context-menu support for a single row --
    // list_ is subclassed (see Create()) so this panel can react to
    // WM_CONTEXTMENU sent to it directly, without main_frame.cpp needing to
    // know or care that rows have a menu at all.
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
