// capture_source_dialog.h - "Capture source" window (7th pass).
//
// Replaces File > Select Window... / Select Region... / Full Desktop (those menu items are gone).
// Opens from the Layers panel: double-click the first row, or its context menu.
//
//   * What is recorded: the whole desktop (and WHICH monitor), one window, or a region of the screen.
//   * For a window: how it is found again - by program, by window class and/or by title (each one
//     switchable; the title can be "exactly / starts with / contains"), whether to keep following the
//     same window while its title changes (browser tabs!), and a "test: find it now" button.
//   * How a window is recorded: the window itself (other windows on top of it do not show up) or a part
//     of the screen; whole window or content area only; the smart (cheap when uncovered) mode and the
//     limit for the exact capture.
//
// Works on a copy - nothing in `state` changes unless OK is pressed. The window / region pickers are
// run between two openings of the dialog (they are full-screen / modal windows of their own).
#pragma once
#include <wx/wx.h>
#include "app_state.h"
#include "theme.h"
#include "language.h"

// live_mode: RecordingController::WindowSourceMode() (0 = unknown / not a window source) - shown as a
// "Right now:" line so the person can see what the recorder is actually doing.
// Returns true when OK was pressed AND something changed (state updated; the caller then refreshes the
// preview pipeline - HomRecMainFrame::OnCaptureTargetChanged()).
bool ShowCaptureSourceDialog(wxWindow *parent, AppState &state, const ThemeColors &theme,
                             const LanguageTable &lang, int live_mode);

// 8th pass (Desktop split): the Desktop layer's dialog above only offers the monitor now. Window and
// region recording are layers - this edits one of them (ov.type "window" or "region": how the window is
// found / how it is captured, or the screen rectangle). Same rules: OK + changed -> true, `ov` updated.
bool ShowLayerSourceDialog(wxWindow *parent, AppState &state, OverlayDef &ov, const ThemeColors &theme,
                           const LanguageTable &lang);
