// overlay_placement_dialog.h - "Apply with preview off" / "Refresh
// screenshot" (see overlays_dock_panel.h's on_apply_no_preview), now a
// dedicated top-level window instead of repurposing the main window's
// live PreviewPanel in place.
//
// UPDATE: this used to just flip main_frame.cpp's PreviewPanel into a
// static-screenshot "snapshot mode" (see PreviewPanel::EnterSnapshotMode
// in main_frame.cpp) so overlays could still be dragged the normal way,
// in the same spot they're normally dragged in. Per an explicit ask,
// that's now a separate window instead: opens over a screenshot of the
// screen, overlays can be dragged/resized freely on it exactly like the
// old in-place mode did, and a bottom bar has Refresh (re-take the
// screenshot) / Apply (commit the new positions) / Cancel.
//
// Owning its own lifecycle this way also fixes a resource-leak bug the
// old approach had: CaptureSnapshotFrame() starts a temporary preview
// pipeline even when "Disable live preview" is on (see
// recording_controller.cpp), and tearing it back down
// (RecordingController::EndSnapshotEditing()) used to only happen if the
// user reopened Settings afterward - if they never did, that pipeline
// (and the DXGI Desktop Duplication handle it holds) stayed alive
// indefinitely, which could make a subsequent recording fail to start
// with "dx_create() returned null" until the app was restarted. This
// dialog calls EndSnapshotEditing() itself on every exit path (Apply,
// Cancel, or the [X] button), so the temporary pipeline is guaranteed to
// come down as soon as the window closes, not "whenever Settings happens
// to be reopened next."
#pragma once
#include <wx/wx.h>
#include "app_state.h"
#include "theme.h"

class RecordingController;

// Returns true if the user clicked Apply (state.overlays' x/y/w/h were
// updated to match whatever was dragged in the window); false on Cancel
// or closing the window (state.overlays is left completely untouched -
// the window drags a local working copy, not the live list, until
// Apply). Always tears down the temporary snapshot pipeline itself
// before returning (see the big comment above) - the caller doesn't
// need to call RecordingController::EndSnapshotEditing() afterward.
bool ShowOverlayPlacementDialog(wxWindow *parent, AppState &state,
                                 RecordingController *rec, const ThemeColors &theme);
