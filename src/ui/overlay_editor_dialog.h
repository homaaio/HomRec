// overlay_editor_dialog.h - single "Edit Overlay" window.
//
// Replaces the previous two-separate-flows setup (the row context menu's
// "Edit Parameters..." - a sequence of plain HrPromptForText/HrPromptForChoice
// popups, see the now-removed OverlaysDockPanel::EditParametersAt() - and
// "Position Overlays..." - overlay_placement_dialog.h's full-screen, every-
// overlay-at-once drag window) with one window scoped to a single overlay:
//
//   - top: the same screenshot-backed drag/resize canvas
//     overlay_placement_dialog.h used, but showing only this overlay (drawn
//     with an actual rendered preview of its content - text/image/gif/
//     webcam-label/input-overlay spritesheet - instead of a bare rectangle)
//     so position and appearance can both be judged in the same place;
//   - below: this overlay's own settings (name, visibility, opacity, and
//     whatever else its type needs - text content/font/color, image/gif
//     file, webcam device, or input-overlay layout+spritesheet files) laid
//     out with the same themed controls (LabeledSlider, ColorButton, etc. -
//     see themed_widgets.h) settings_dialog.cpp uses, instead of the old
//     sequence of raw text-entry popups.
//
// Both position and settings edit a local working copy; nothing touches
// state.overlays[idx] (or disk) unless the user clicks Save.
#pragma once
#include <cstddef>
#include <wx/wx.h>
#include "app_state.h"
#include "theme.h"

class RecordingController;

// Returns true if Save was clicked (state.overlays[idx] and the on-disk
// overlays autosave were updated to match the working copy edited in the
// window); false on Cancel or the [X] button (state.overlays is left
// completely untouched). Always tears down the temporary snapshot preview
// pipeline itself before returning - same reasoning as
// ShowOverlayPlacementDialog() (see overlay_placement_dialog.h).
bool ShowOverlayEditorDialog(wxWindow *parent, AppState &state, size_t idx,
                              RecordingController *rec, const ThemeColors &theme);
