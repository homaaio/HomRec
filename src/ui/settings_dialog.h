// settings_dialog.h - tabbed rewrite.
#pragma once

#include <wx/wx.h>
#include "app_state.h"
#include "theme.h"
#include "language.h"

class RecordingController;

// Shows the modal dialog on the General tab. Returns true if the user
// clicked Save (in which case `state` has been updated and persisted
// via hr_settings_save).
bool ShowSettingsDialog(wxWindow *parent, AppState &state, const ThemeColors &theme,
                         const LanguageTable &lang, RecordingController *rec = nullptr);

// Same dialog, but pass one of the old tab indices (1 = former "Video/ Codec", 4 = former
// "Advanced") to open on that tab directly - used by the "Advanced Settings..." menu item
// so picking it still lands the user on the codec/CRF/etc.
bool ShowSettingsDialogTab(wxWindow *parent, AppState &state, const ThemeColors &theme,
                            const LanguageTable &lang, int tab_index, RecordingController *rec = nullptr);
