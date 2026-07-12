// settings_dialog.h — Phase 5
//
// Port of homrec_app/dialogs/settings_dialog.py (create_widgets/save_settings).
// Persistence goes through hr_settings_create/load/save/get_*/set_* — the
// existing C++ settings store — instead of re-implementing JSON I/O here.
//
// NOTE ON SCOPE: the Python dialog's create_widgets() builds ~15 controls
// (quality slider, fps spinner, mode dropdown, monitor dropdown, output
// folder + browse button, countdown/timestamp/cursor checkboxes, notify
// checkbox). This port covers that same field set. It does NOT yet cover
// AdvancedSettingsDialog's tabbed codec/hotkey/audio-format controls —
// that's `advanced_settings_dialog.h/.cpp`, still to come.
#pragma once

#include <windows.h>
#include "app_state.h"

// Shows the modal dialog. Returns true if the user clicked Save (in which
// case `state` has been updated and persisted via hr_settings_save).
bool ShowSettingsDialog(HWND parent, HINSTANCE hInst, AppState &state);
