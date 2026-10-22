// wgc_capture.h - Windows.Graphics.Capture window source (2.4).
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
