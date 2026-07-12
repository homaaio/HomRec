// overlay_manager.h — Phase 6
//
// Port of homrec_app/dialogs/overlay_manager.py (OverlayManagerWindow +
// OverlayPreviewDialog) and overlays_dock_panel.py's list/quick-add
// behavior, collapsed into one manager window + one drag-to-position
// preview window rather than three separate dialog classes — the Python
// version splits dock panel / manager / preview into separate files
// mostly for Tk layout reasons that don't apply here.
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
