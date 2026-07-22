// settings_dialog.h — wxWidgets rewrite, now tabbed.
//
// Was a raw CreateWindowExW dialog with hbrBackground = COLOR_BTNFACE (plain
// white/grey) — never themed, which is exactly the "white window with ugly
// text" complaint. Rebuilt as a themed wxDialog, then split into a
// wxNotebook (General / Video & Codec / Audio / Hotkeys / Advanced) so each
// settings group has its own tab, folding in the fields that used to live
// in the separate raw-Win32 Advanced Settings dialog. Persistence is
// unchanged: still goes through hr_settings_create/load/save/get_*/set_*
// (see settings_dialog.cpp's header comment for which fields that covers).
#pragma once

#include <wx/wx.h>
#include "app_state.h"
#include "theme.h"

// Shows the modal dialog on its first ("General") tab. Returns true if the
// user clicked Save (in which case `state` has been updated and persisted
// via hr_settings_save).
bool ShowSettingsDialog(wxWindow *parent, AppState &state, const ThemeColors &theme);

// Same dialog, opened on a specific tab (0=General, 1=Video/Codec,
// 2=Audio, 3=Hotkeys, 4=Advanced) — used by the "Advanced Settings..."
// menu item so it still feels like its own entry point.
bool ShowSettingsDialogTab(wxWindow *parent, AppState &state, const ThemeColors &theme, int tab_index);
