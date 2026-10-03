// hr_wgc_capture.h - Windows.Graphics.Capture window source (2.4).
//
// Records ONE WINDOW itself - the whole window, even when other windows are on top of it or it is
// partly off-screen / on another monitor - instead of cutting its rectangle out of a screen
// duplication (which only ever shows "the area where the window currently is").
//
// It plugs into hr_pipeline.cpp exactly where the DXGI duplication source sits (same
// create / capture / get_size / reset / destroy shape), so the encoder, preview, overlays and
// pause logic are shared. The pipeline asks HrWgcIsHandle() to tell the two sources apart.
//
// The picture is delivered into a fixed-size "canvas" (the window's size when recording starts):
//   * same size            -> straight row copy
//   * window got smaller   -> centred 1:1, black bars
//   * window got bigger    -> scaled down to fit, aspect ratio kept, black bars
// so the recording's resolution never changes mid-file but the WHOLE window stays visible.
//
// Needs Windows 10 version 1903 (build 18362); HrWgcSupported() says whether it can work here. If
// anything fails at HrWgcCreate() the caller falls back to the old screen-crop path.
#pragma once
#include <cstdint>
#include <windows.h>

bool  HrWgcSupported();

// Size the window would be recorded at (client_only = content area only). False if hwnd is gone.
bool  HrWgcQueryWindowSize(HWND hwnd, bool client_only, int *w, int *h);

// Cheap check that Windows will hand out a capture item for this window at all (protected /
// elevated / odd windows refuse). No device or session is created.
bool  HrWgcProbeWindow(HWND hwnd);

// Returns nullptr on failure (details are logged).
void *HrWgcCreate(HWND hwnd, int canvas_w, int canvas_h, bool client_only, bool draw_cursor);
void  HrWgcDestroy(void *handle);
bool  HrWgcIsHandle(void *handle);

// Return codes mirror hr_dx_capture: 0 = new frame written to out_bgra, 1 = no new frame (window
// unchanged / minimised), 2 = source lost (device reset - the caller should HrWgcReset()), -1 = error.
int   HrWgcCapture(void *handle, uint8_t *out_bgra, int timeout_ms);
int   HrWgcGetSize(void *handle, int *w, int *h);      // the canvas size
int   HrWgcReset(void *handle);                         // rebuilds device + session; 1 = ok
void  HrWgcSetCursor(void *handle, bool on);
