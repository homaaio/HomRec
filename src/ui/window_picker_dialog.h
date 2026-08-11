// window_picker_dialog.h - Feature-parity pass
//
// Port of homrec_app/mixins/ui_mixin.py's open_window_picker() (plus the
// one-liner set_capture_desktop()). The backend half of this already
// existed - hr_enum_windows() in hr_app_logic.cpp enumerates the visible
// top-level windows - and AppState already has CaptureMode::Window +
// capture_window_title fields ready to receive a selection. What was
// missing was the picker UI and a menu entry to reach it;
// RecordingController now resolves capture_window_title back to a live
// HWND + rect via HR_ResolveCaptureWindow() below and crops the captured
// monitor frame to it (see hr_pl_set_capture_rect() in hr_pipeline.cpp) -
// this used to be stored but never actually consumed anywhere, which is
// why "record just this window" silently fell back to full-desktop
// capture (and the preview never reflected it either, since preview and
// recording share the same pipeline).
#pragma once
#include <windows.h>
#include <string>
#include "app_state.h"

// Lists visible top-level windows and lets the user pick one to record,
// or fall back to full-desktop capture. Mutates state.capture_mode /
// state.capture_window_title directly (no separate "OK/Cancel, then
// commit" step).
void ShowWindowPickerDialog(HWND parent, HINSTANCE hInst, AppState &state);

// Resolves a window title (as stored in AppState::capture_window_title)
// back to its live HWND and current screen rect (DWM's visual bounds,
// not the raw GetWindowRect() which includes the invisible resize-grip
// margin on Win10/11 - see the .cpp for why that distinction matters for
// capture). Uses the same "is this actually a capturable window" filter
// ShowWindowPickerDialog()'s list does, so a stale/ambiguous title can't
// resolve to some unrelated tool window.
//
// Returns false (leaving out_hwnd/out_rect untouched) if no currently
// open window has that exact title - e.g. it was closed since being
// picked - so the caller can fall back to full-desktop capture instead
// of capturing garbage or crashing.
bool HR_ResolveCaptureWindow(const std::string &title, HWND &out_hwnd, RECT &out_rect);
