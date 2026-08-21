// settings_dialog.h - tabbed rewrite.
//
// Split into a wxNotebook with 7 tabs: General / Video & Codec / Audio /
// Hotkeys / Advanced / Security / System. Replaces the previous single
// scrollable page (everyday fields always visible + a hide-by-default
// "Advanced" panel toggled by one button) - that layout put every field
// in one place regardless of how often it's actually touched, which
// stopped scaling once Security's and System's fields were added. See
// settings_dialog.cpp's header comment for the full per-tab breakdown
// and BuildAdvancedTab()'s note for which fields persist to disk.
#pragma once

#include <wx/wx.h>
#include "app_state.h"
#include "theme.h"

// Shows the modal dialog on the General tab. Returns true if the user
// clicked Save (in which case `state` has been updated and persisted
// via hr_settings_save).
bool ShowSettingsDialog(wxWindow *parent, AppState &state, const ThemeColors &theme);

// Same dialog, but pass one of the old tab indices (1 = former "Video/
// Codec", 4 = former "Advanced") to open on that tab directly - used by
// the "Advanced Settings..." menu item so picking it still lands the
// user on the codec/CRF/etc. fields instead of the General tab. Any
// other index leaves the dialog on the General tab.
bool ShowSettingsDialogTab(wxWindow *parent, AppState &state, const ThemeColors &theme, int tab_index);
