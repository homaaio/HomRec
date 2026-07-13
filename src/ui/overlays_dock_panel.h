// overlays_dock_panel.h — Feature-parity pass
//
// Port of homrec_app/dialogs/overlays_dock_panel.py's OverlaysDockPanel.
//
// The earlier overlay_manager.cpp port (Phase 6) deliberately folded this,
// OverlayManagerWindow, and OverlayPreviewDialog into one manager window +
// one drag-preview window, reachable only via the Settings > Overlays...
// menu item — a real, intentional simplification, but it means there was
// no persistent at-a-glance overlay list living in the main window the
// way there is in Python (toggled by AppState.show_overlays_panel, which
// already existed in app_state.h with no UI ever reading it). This adds
// that back as an actual embedded child panel.
//
// Interaction differs slightly from the Python version by necessity: Tk's
// per-row "⋮" button opens a small popup menu (More… / Remove) anchored
// to that row. A plain Win32 LISTBOX can't anchor per-row popups without
// owner-draw + hit-testing machinery, so this uses "select a row, then
// press More… or Remove" instead — same two actions, one fewer click
// style than a floating menu. Quick-add and Position-on-Preview behave
// the same as Python (immediate, no confirmation step).
//
// Like AudioPanel (see audio_panel.h), this panel is created once at a
// fixed rect and doesn't reflow on WM_SIZE — main_window.cpp's OnSize()
// doesn't have a real layout system yet for either panel, so this follows
// the same already-flagged limitation rather than inventing a
// resize story just for this one panel.
#pragma once

#include <windows.h>
#include "app_state.h"

class OverlaysDockPanel {
public:
    explicit OverlaysDockPanel(AppState &state);

    HWND Create(HWND parent, HINSTANCE hInst, int x, int y, int w, int h);

    // Rebuilds the visible list from state_.overlays — call after any
    // external code (e.g. the full Overlay Manager) changes the vector,
    // same as Python's refresh() being called from multiple call sites.
    void Refresh();

    void OnCommand(int id);

    // Shows/hides the panel's HWNDs to match AppState.show_overlays_panel
    // without destroying/recreating them (cheaper, and avoids re-deriving
    // control ids). Mirrors Python's recreate_widgets() being triggered by
    // the close button, minus the actual teardown/rebuild of the whole
    // main window that the Tk version does for this.
    void SetVisible(bool visible);

    HWND hwnd() const { return hwnd_; }

private:
    void QuickAdd();
    void RemoveSelected();
    void OpenMoreForSelected(HWND parent, HINSTANCE hInst);
    void OpenDragPreview(HWND parent, HINSTANCE hInst);
    void ClosePanel();

    AppState &state_;
    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;
    int next_id_ = 1; // matches overlay_manager.cpp's ManagerCtx::next_id convention (resets per session, not globally unique — pre-existing behavior, not new here)
};

enum OverlaysDockPanelControlId {
    ID_OVDOCK_ADD      = 3001,
    ID_OVDOCK_POSITION = 3002,
    ID_OVDOCK_CLOSE    = 3003,
    ID_OVDOCK_LIST     = 3004,
    ID_OVDOCK_MORE     = 3005,
    ID_OVDOCK_REMOVE   = 3006,
};
