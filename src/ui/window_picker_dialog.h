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
// state.capture_window_title / state.capture_window_hwnd directly (no separate "OK/Cancel, then
// commit" step).
void ShowWindowPickerDialog(HWND parent, HINSTANCE hInst, AppState &state);

// Full-screen click-and-drag rectangle selector (todo2.3.md section 3,
// "Захват произвольной области экрана") - sets state.capture_mode to
// CaptureMode::Region and state.region_x/y/w/h to the dragged rect (in
// virtual-desktop pixel coordinates, spanning however many monitors are
// connected - same coordinate space GetWindowRect() uses). Esc, or a
// click-drag that ends up under 8x8 pixels (a stray click, not a real
// drag), cancels without changing state at all. Modal in the same sense
// ShowWindowPickerDialog() above is (blocks pumping parent's message
// loop until it closes).
void ShowRegionPickerOverlay(HWND parent, HINSTANCE hInst, AppState &state);

// Resolves a window title (as stored in AppState::capture_window_title)
// back to its live HWND and current screen rect (DWM's visual bounds,
// not the raw GetWindowRect() which includes the invisible resize-grip
// margin on Win10/11 - see the .cpp for why that distinction matters for
// capture). Uses the same "is this actually a capturable window" filter
// ShowWindowPickerDialog()'s list does, so a stale/ambiguous title can't
// resolve to some unrelated tool window.
//
// preferred_hwnd (AppState::capture_window_hwnd) is tried FIRST: if that
// exact window is still alive, capturable, and not minimized it wins
// regardless of what its title is now - titles change all the time
// (browser tabs, unsaved-changes markers), and matching by title alone
// used to lose the window and silently record the whole desktop instead.
// Only when it's null/gone does this fall back to matching by title
// (exact first, then case-insensitive).
//
// Returns false (leaving out_hwnd/out_rect untouched) if the window can't
// be found or is minimized/degenerate - e.g. it was closed since being
// picked - so the caller can refuse to start (or fall back for a preview)
// instead of capturing the wrong thing.
bool HR_ResolveCaptureWindow(const std::string &title, HWND &out_hwnd, RECT &out_rect,
                             HWND preferred_hwnd = nullptr);
