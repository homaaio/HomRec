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
// plugin has registered any presets, see hr_input_overlay_registry.h) and
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
// Like AudioPanel (see audio_panel.h), this panel is created once at a
// fixed rect and doesn't reflow on WM_SIZE.
#pragma once

#include <windows.h>
#include <functional>
#include "app_state.h"

class OverlaysDockPanel {
public:
    explicit OverlaysDockPanel(AppState &state);

    HWND Create(HWND parent, HINSTANCE hInst, int x, int y, int w, int h);

    // Rebuilds the visible list from state_.overlays.
    void Refresh();

    void OnCommand(int id);

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

private:
    // Popup menu anchored under the "+" button; dispatches to one of the
    // Add* methods below based on what was picked.
    void ShowAddMenu(HWND parent, HINSTANCE hInst);

    void AddTextOverlay(HWND parent, HINSTANCE hInst);
    void AddImageOverlay(HWND parent, HINSTANCE hInst);
    // "GIF" -- same idea as AddImageOverlay, but restricted to .gif and
    // stored with type == "gif" so the capture pipeline (hr_overlay_render)
    // animates it frame-by-frame instead of treating it as a static image.
    void AddGifOverlay(HWND parent, HINSTANCE hInst);
    void AddWebcamOverlay(HWND parent, HINSTANCE hInst);
    // "External Overlay" -- the user picks a .json layout file and a .png
    // spritesheet directly (two plain file pickers) -- NOT a .hrp plugin
    // package; see hr_input_overlay.h's HrInputOverlayLayout for what the
    // .json needs to look like.
    void AddExternalOverlay(HWND parent, HINSTANCE hInst);
    // "Select Input-Overlay" -- lists presets a plugin has registered via
    // homrec.register_input_overlay() (hr_input_overlay_registry.h) and
    // adds the chosen one. Only reachable from the menu when at least one
    // preset is registered.
    void AddFromInputOverlayRegistry(HWND parent, HINSTANCE hInst);

    void ToggleVisibility(size_t idx);
    void RemoveAt(size_t idx);
    void RenameAt(size_t idx);
    void ClosePanel();

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
    int next_id_ = 1; // resets per session, not globally unique - pre-existing behavior, not new here
};

enum OverlaysDockPanelControlId {
    ID_OVDOCK_ADD      = 3001,
    ID_OVDOCK_CLOSE    = 3003,
    ID_OVDOCK_LIST     = 3004,
};
