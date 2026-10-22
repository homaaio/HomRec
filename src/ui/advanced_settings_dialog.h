// advanced_settings_dialog.h - non-overlay advanced settings: codec/hardware, hotkeys, audio/misc.
#pragma once

#include <windows.h>
#include "app_state.h"

bool ShowAdvancedSettingsDialog(HWND parent, HINSTANCE hInst, AppState &state);
