// settings_dialog.h - progressive-disclosure rewrite.
//
// Single scrollable page instead of a wxNotebook: everyday fields (output
// folder, quality, fps, monitor, resolution, checkboxes, mic, hotkeys)
// are always visible. Fields a casual user has no reason to touch (codec,
// hw-accel, encoder preset, CRF, pixel format, custom ffmpeg args, sample
// rate/bitrate/channels, filename template, auto-stop, replay buffer) are
// grouped into one panel that's hidden by default and toggled by a single
// button in the header - clicking it adds/removes that panel from THIS
// window in place; it never opens a second window. See
// settings_dialog.cpp's header comment for more, and its note above
// BuildAdvancedSection() for which fields persist to disk vs. session-only.
#pragma once

#include <wx/wx.h>
#include "app_state.h"
#include "theme.h"

// Shows the modal dialog with the advanced panel collapsed. Returns true
// if the user clicked Save (in which case `state` has been updated and
// persisted via hr_settings_save).
bool ShowSettingsDialog(wxWindow *parent, AppState &state, const ThemeColors &theme);

// Same dialog, but pass one of the old tab indices (1 = former "Video/
// Codec", 4 = former "Advanced") to open with the advanced panel already
// expanded - used by the "Advanced Settings..." menu item so picking it
// still lands the user on the codec/CRF/etc. fields directly instead of
// having to click the toggle themselves. Any other index leaves the
// advanced panel collapsed (those fields - Audio/Hotkeys basics - are
// already always visible in the basic section).
bool ShowSettingsDialogTab(wxWindow *parent, AppState &state, const ThemeColors &theme, int tab_index);
