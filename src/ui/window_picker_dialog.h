// window_picker_dialog.h - Feature-parity pass
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "app_state.h"

// Lists visible top-level windows and lets the user pick one to record, or fall back to
// full-desktop capture.
void ShowWindowPickerDialog(HWND parent, HINSTANCE hInst, AppState &state);

void ShowRegionPickerOverlay(HWND parent, HINSTANCE hInst, AppState &state);

// Resolves a window title (as stored in AppState::capture_window_title)
bool HR_ResolveCaptureWindow(const std::string &title, HWND &out_hwnd, RECT &out_rect,
                             HWND preferred_hwnd = nullptr, bool client_only = false);

// 7th pass: finding the window again by more than its title.
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

// 7th pass: connected monitors, in the order AppState::monitor_id counts them (monitor_id - 1 = index;
// the primary monitor is always first). Used by the capture-source dialog and the Layers panel.
struct HrMonitorInfo {
    int x = 0, y = 0, w = 0, h = 0;
    bool primary = false;
};
std::vector<HrMonitorInfo> HrListMonitors();
HrWindowTarget HrWindowTargetFromLayer(const OverlayDef &ov);

void HrFitLayerRect(int src_w, int src_h, int canvas_w, int canvas_h, bool allow_upscale,
                    int &x, int &y, int &w, int &h);

bool HrCanvasSizeForState(const AppState &state, int &w, int &h);
bool HrBuildWindowLayer(const AppState &picked, int canvas_w, int canvas_h, bool allow_upscale, OverlayDef &out);
bool HrBuildRegionLayer(const AppState &picked, int canvas_w, int canvas_h, bool allow_upscale, OverlayDef &out);

void HrMigrateCaptureSourceToLayers(AppState &state);
