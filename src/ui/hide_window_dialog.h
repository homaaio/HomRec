// hide_window_dialog.h - exclude chosen windows from capture via SetWindowDisplayAffinity (WDA_EXCLUDEFROMCAPTURE).
#pragma once
#include <windows.h>
#include "app_state.h"

void ShowHideWindowDialog(HWND parent, HINSTANCE hInst, AppState &state);

// Clears every exclusion this session applied (state.hidden_capture_ windows) and empties
// the list.
void ClearAllHiddenCaptureWindows(AppState &state);
