// capture_source_dialog.h - "Capture source" window (7th pass).
#pragma once
#include <wx/wx.h>
#include "app_state.h"
#include "theme.h"
#include "language.h"

// live_mode: RecordingController::WindowSourceMode() (0 = unknown / not a window source) - shown as a
// "Right now:" line so the person can see what the recorder is actually doing.
bool ShowCaptureSourceDialog(wxWindow *parent, AppState &state, const ThemeColors &theme,
                             const LanguageTable &lang, int live_mode);

// 8th pass (Desktop split): the Desktop layer's dialog above only offers the monitor now. Window and
// region recording are layers - this edits one of them (ov.type "window" or "region": how the window is
// found / how it is captured, or the screen rectangle). Same rules: OK + changed -> true, `ov` updated.
bool ShowLayerSourceDialog(wxWindow *parent, AppState &state, OverlayDef &ov, const ThemeColors &theme,
                           const LanguageTable &lang);
