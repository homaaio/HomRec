// preset_dialog.h - File > Set Preset... (.hrc profile manager)
#pragma once

#include <wx/wx.h>
#include "app_state.h"
#include "theme.h"
#include "language.h"

// Shows the modal "Set Preset" dialog.
bool ShowPresetDialog(wxWindow *parent, AppState &state, const ThemeColors &theme,
                       const LanguageTable &lang);
