// hide_window_dialog.h
//
// Lets the user exclude specific windows (a chat app, notifications, a
// password manager, etc.) from whatever HomRec captures, without having
// to actually close them - most useful in full-desktop capture mode,
// where a window that stays open just behind/beside whatever's actually
// being recorded would otherwise still show up in the frame.
//
// Implemented via SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) -
// the modern (Windows 10 2004+) OS-level API for this, which DWM honors
// for every capture consumer (DXGI Desktop Duplication - what this app's
// own capture uses - GDI BitBlt, Windows.Graphics.Capture, PrintWindow),
// not just HomRec's. The window stays perfectly normal/visible to the
// user on their own screen; it just renders as excluded in whatever any
// capture API hands back.
#pragma once
#include <windows.h>
#include "app_state.h"

void ShowHideWindowDialog(HWND parent, HINSTANCE hInst, AppState &state);

// Clears every exclusion this session applied (state.hidden_capture_
// windows) and empties the list. MUST be called on every app-exit path -
// see the long comment on AppState::hidden_capture_windows for why this
// can't just be left to clean itself up.
void ClearAllHiddenCaptureWindows(AppState &state);
