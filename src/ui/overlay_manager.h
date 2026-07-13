// overlay_manager.h — Phase 6
//
// Port of homrec_app/dialogs/overlay_manager.py (OverlayManagerWindow +
// OverlayPreviewDialog), originally collapsed together with
// overlays_dock_panel.py's list/quick-add behavior into one manager
// window + one drag-to-position preview window, on the reasoning that
// the Python version's split into three separate dialog files was mostly
// about Tk layout and didn't need to carry over here.
//
// UPDATE (feature-parity pass): a real, persistent OverlaysDockPanel was
// added back (see overlays_dock_panel.h) per an explicit ask to match the
// Python version's UI 1:1, so the "collapsed away" framing above is now
// only half true — the dock panel exists again, this file just still
// owns the full editor + drag-preview windows it opens into.
//
// Operates directly on AppState.overlays (std::vector<OverlayDef>, see
// app_state.h). Actual overlay *compositing* onto the recorded video
// happens in the existing native pipeline — this is purely the editing UI.
#pragma once

#include <windows.h>
#include "app_state.h"

// Opens the manager window (list + add/edit/delete). Blocking modal call,
// same as the Python version's Toplevel + wait_window pattern. Returns
// true if the overlay list was changed and saved.
bool ShowOverlayManager(HWND parent, HINSTANCE hInst, AppState &state);

// Exported wrapper around the drag-to-position preview window (the
// anonymous-namespace ShowDragPreview() in overlay_manager.cpp isn't
// linkable from other translation units). Added for overlays_dock_panel.h,
// whose "Position on Preview" button calls this directly — mirroring
// Python's OverlaysDockPanel._open_drag_preview(), which opens
// OverlayPreviewDialog straight from the dock panel rather than going
// through the full manager window first.
void ShowOverlayDragPreview(HWND parent, HINSTANCE hInst, AppState &state);
