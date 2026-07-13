// window_picker_dialog.h — Feature-parity pass
//
// Port of homrec_app/mixins/ui_mixin.py's open_window_picker() (plus the
// one-liner set_capture_desktop()). The backend half of this already
// existed — hr_enum_windows() in hr_app_logic.cpp is explicitly commented
// as "Python equivalent of HomRecScreen.get_open_windows()" — and
// AppState already has CaptureMode::Window + capture_window_title fields
// ready to receive a selection. What was missing was the picker UI and a
// menu entry to reach it; RecordingController presumably still needs to
// actually branch on capture_mode when it starts capture (see the
// migration notes this pass adds), same as it did in Python.
#pragma once
#include <windows.h>
#include "app_state.h"

// Lists visible top-level windows and lets the user pick one to record,
// or fall back to full-desktop capture. Mutates state.capture_mode /
// state.capture_window_title directly, mirroring the Python version's
// direct attribute assignment (no separate "OK/Cancel, then commit" step).
void ShowWindowPickerDialog(HWND parent, HINSTANCE hInst, AppState &state);
