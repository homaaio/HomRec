// overlay_editor_dialog.h - single "Edit Overlay" window.
#pragma once
#include <cstddef>
#include <wx/wx.h>
#include "app_state.h"
#include "theme.h"

class RecordingController;

// Returns true if Save was clicked (state.overlays[idx] and the on-disk overlays autosave
// were updated to match the working copy edited in the window); false on Cancel or the [X]
// button (state.overlays is left completely untouched).
bool ShowOverlayEditorDialog(wxWindow *parent, AppState &state, size_t idx,
                              RecordingController *rec, const ThemeColors &theme);
