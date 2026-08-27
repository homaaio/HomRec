// welcome_dialog.h
//
// First-run wizard shown on a fresh install (AppState::first_launch) and
// reachable any time afterward via Help > Welcome. Three pages in one
// window (no child dialogs): greeting -> basic settings (output folder /
// resolution / fps, or "I understand" to skip them) -> finish (links to
// docs/changelog, "Get Started" to close). See welcome_dialog.cpp for the
// page-visibility mechanics.
#pragma once
#include <windows.h>
#include "app_state.h"

// `state` is read for its current defaults (output_folder/target_fps/
// scale_factor) and written back (plus persisted via HrcConfig::Save() -
// see commands.md's Phase 1 settings-storage migration) if the user fills
// in page 2 instead of checking "I understand".
void ShowWelcomeDialog(HWND parent, HINSTANCE hInst, AppState &state);
