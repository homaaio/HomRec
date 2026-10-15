// wgc_capture.h - Windows.Graphics.Capture window source (2.4).
//
// Records ONE WINDOW itself - the whole window, even when other windows are on top of it or it is
// partly off-screen / on another monitor - instead of cutting its rectangle out of a screen
// duplication (which only ever shows "the area where the window currently is").
//
// It plugs into pipeline.cpp exactly where the DXGI duplication source sits (same
// create / capture / get_size / reset / destroy shape), so the encoder, preview, overlays and
// pause logic are shared. The pipeline asks HrWgcIsHandle() to tell the two sources apart.
//
// The picture is delivered into a fixed-size "canvas" (the window's size when recording starts):
//   * same size            -> straight row copy
//   * window got smaller   -> centred 1:1, black bars
//   * window got bigger    -> scaled down to fit, aspect ratio kept, black bars
// so the recording's resolution never changes mid-file but the WHOLE window stays visible.
//
// Needs Windows 10 version 1903 (build 18362) for native WGC; where that is unusable (or has faulted on
// this PC) HrWgcCreate() hands out a PrintWindow-based source instead (Windows 8.1+, same API, more CPU).
// HrWgcSupported() says whether either can work here. If both fail the caller falls back to the old
// screen-crop path.
#pragma once
#include <cstdint>
#include <windows.h>

bool  HrWgcSupported();

// Crash sentinel (see wgc_capture.cpp): true when the PREVIOUS run died while inside native WGC
// code. HrWgcSupported() then returns false so window recording falls back to the screen crop
// instead of crashing again. HrWgcResetCrashGuard() lets the user give WGC another chance.
bool  HrWgcCrashGuardTripped();
void  HrWgcResetCrashGuard();

// Size the window would be recorded at (client_only = content area only). False if hwnd is gone.
bool  HrWgcQueryWindowSize(HWND hwnd, bool client_only, int *w, int *h);

// Cheap check that Windows will hand out a capture item for this window at all (protected /
// elevated / odd windows refuse). No device or session is created.
bool  HrWgcProbeWindow(HWND hwnd);

// 7th pass options of the PrintWindow backend (native WGC ignores them).
//   smart   - while nothing covers the window its picture is cut out of a cheap screen duplication (the
//             same cost as a desktop recording, and nothing is recomputed while nothing changes); the
//             moment another window overlaps it, it switches to the exact capture (PrintWindow) and back.
//   fps_cap - upper limit of exact captures per second (PrintWindow is expensive and makes the target
//             window redraw itself); 0 = no limit beyond the recording's own frame rate.
struct HrWgcOptions {
    bool smart   = true;
    int  fps_cap = 0;
};

// Returns nullptr on failure (details are logged to logs\wgc.log and homrec.log).
void *HrWgcCreate(HWND hwnd, int canvas_w, int canvas_h, bool client_only, bool draw_cursor,
                  const HrWgcOptions *opt = nullptr);
void  HrWgcDestroy(void *handle);
bool  HrWgcIsHandle(void *handle);

// Return codes mirror hr_dx_capture: 0 = new frame written to out_bgra, 1 = no new frame (window
// unchanged / minimised), 2 = source lost (device reset - the caller should HrWgcReset()), -1 = error.
int   HrWgcCapture(void *handle, uint8_t *out_bgra, int timeout_ms);
int   HrWgcGetSize(void *handle, int *w, int *h);      // the canvas size
int   HrWgcReset(void *handle);                         // rebuilds device + session; 1 = ok
void  HrWgcSetCursor(void *handle, bool on);

// 7th pass: point a running window source at another window (the program opened a new main window, the
// window was re-created...). Only the PrintWindow / smart backends can follow; native WGC is bound to
// its window and returns false. The canvas keeps its size (the new window is fitted into it).
bool  HrWgcSetTarget(void *handle, HWND hwnd);

// What this source is doing right now: 0 = unknown handle, 1 = native Windows.Graphics.Capture,
// 2 = exact window capture (PrintWindow), 3 = smart mode currently reading the screen duplication.
int   HrWgcMode(void *handle);
