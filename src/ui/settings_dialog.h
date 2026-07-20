// settings_dialog.h — wxWidgets rewrite.
//
// Was a raw CreateWindowExW dialog with hbrBackground = COLOR_BTNFACE (plain
// white/grey) — never themed, which is exactly the "white window with ugly
// text" complaint. Rebuilt as a themed wxDialog. Persistence is unchanged:
// still goes through hr_settings_create/load/save/get_*/set_*.
#pragma once

#include <wx/wx.h>
#include "app_state.h"
#include "theme.h"

// Shows the modal dialog. Returns true if the user clicked Save (in which
// case `state` has been updated and persisted via hr_settings_save).
bool ShowSettingsDialog(wxWindow *parent, AppState &state, const ThemeColors &theme);
