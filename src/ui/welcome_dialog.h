// welcome_dialog.h - First-run wizard shown on a fresh install (AppState::first_launch)
// and reachable any time afterward via Help > Welcome.
#pragma once
#include <windows.h>
#include "app_state.h"

void ShowWelcomeDialog(HWND parent, HINSTANCE hInst, AppState &state);
