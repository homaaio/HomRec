// window_picker_dialog.h - Feature-parity pass
//
// Port of homrec_app/mixins/ui_mixin.py's open_window_picker() (plus the
// one-liner set_capture_desktop()). The backend half of this already
// existed - hr_enum_windows() in app_logic.cpp enumerates the visible
// top-level windows - and AppState already has CaptureMode::Window +
// capture_window_title fields ready to receive a selection. What was
// missing was the picker UI and a menu entry to reach it;
// RecordingController now resolves capture_window_title back to a live
// HWND + rect via HR_ResolveCaptureWindow() below and crops the captured
// monitor frame to it (see hr_pl_set_capture_rect() in pipeline.cpp) -
// this used to be stored but never actually consumed anywhere, which is
// why "record just this window" silently fell back to full-desktop
// capture (and the preview never reflected it either, since preview and
// recording share the same pipeline).
#pragma once
#include <windows.h>
#include <string>
#include <vector>
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
//
// 2.4: client_only = true returns the window's content area (no title bar / borders) instead
// of its whole visible frame (AppState::window_capture_area == "client").
bool HR_ResolveCaptureWindow(const std::string &title, HWND &out_hwnd, RECT &out_rect,
                             HWND preferred_hwnd = nullptr, bool client_only = false);

// ---------------------------------------------------------------------------------------------
// 7th pass: finding the window again by more than its title.
//
// A browser window's title changes with every tab, so "same title" lost the window the moment the user
// switched tabs. The target is now described by up to three criteria that are each switchable
// (Layers panel > Capture source...): the program (exe name), the window class, and the title (exact /
// starts with / contains). Every ENABLED criterion must match. If none of the enabled criteria has a
// stored value to compare (an old settings file that only knows the title), the stored title is used.
// ---------------------------------------------------------------------------------------------
struct HrWindowTarget {
    std::string title, exe, cls;       // what was picked (exe is lower-case, e.g. "chrome.exe")
    bool match_title = false, match_exe = true, match_class = true;
    int  title_mode = 0;               // 0 exact, 1 starts with, 2 contains
    bool follow_hwnd = true;           // keep the exact window while it exists, whatever its title is now
    HWND preferred = nullptr;          // the window recorded so far (runtime only)
    bool client_only = false;
};

HrWindowTarget HrWindowTargetFromState(const AppState &state);
int            HrTitleModeFromStr(const std::string &s);
const char    *HrTitleModeToStr(int mode);

// Resolves `t` to a live window and its on-screen rect (same rules as HR_ResolveCaptureWindow: DWM
// frame bounds, or the client area; minimized windows don't count). `how` (optional) receives a short
// description of the route taken ("same window handle", "matched exe+class", ...) for wgc.log.
bool HR_ResolveWindowTarget(const HrWindowTarget &t, HWND &out_hwnd, RECT &out_rect, std::string *how = nullptr);

// Convenience for everything that holds an AppState: resolves state's target, fills in the exe / class
// that were never stored (old settings files) from the window it found, and remembers the HWND in
// state.capture_window_hwnd. Same return contract as above.
bool HR_ResolveCaptureWindowFromState(AppState &state, HWND &out_hwnd, RECT &out_rect, std::string *how = nullptr);

// Writes a freshly picked window into state (title, hwnd, exe, class, mode = Window).
void HR_RememberPickedWindow(AppState &state, HWND hwnd, const std::string &title_utf8);

std::string HR_WindowExeName(HWND hwnd);     // lower-case base name, "" when it can't be read (elevated)
std::string HR_WindowClassName(HWND hwnd);
std::string HR_WindowTitleUtf8(HWND hwnd);

// ---------------------------------------------------------------------------------------------
// 7th pass: connected monitors, in the order AppState::monitor_id counts them (monitor_id - 1 = index;
// the primary monitor is always first). Used by the capture-source dialog and the Layers panel.
// ---------------------------------------------------------------------------------------------
struct HrMonitorInfo {
    int x = 0, y = 0, w = 0, h = 0;
    bool primary = false;
};
std::vector<HrMonitorInfo> HrListMonitors();
