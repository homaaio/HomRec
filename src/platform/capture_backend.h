// capture_backend.h - screen / window / region capture, the part of HomRec that differs most
// between operating systems (DXGI + WGC on Windows; X11 or PipeWire-portal on Linux).
//
// Contract (deliberately the same shape as the Windows engine, so pipeline.cpp can be moved onto
// it without re-learning anything):
//   * Grab() writes ONE tightly packed top-down BGRA frame (width*height*4 bytes, no row padding)
//     into a caller-owned buffer - exactly what hr_dx_capture() / HrWgcCapture() do today.
//   * Grab() return values mirror hr_dx_capture(): NewFrame=0, NoChange=1, SourceLost=2, Error=-1.
//   * One IScreenCapture = one source. Capture runs on the caller's thread (the pipeline's capture
//     thread); an implementation may use internal threads but must not call back into the caller.
//
// GPU-resident frames (hr_dx_capture_nv12_tex & co.) are NOT part of this interface on purpose:
// they stay a Windows-only fast path until a VA-API / DMA-BUF path exists (plan, phase 6).
#pragma once

#include "platform.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hr {
namespace platform {

enum class CaptureStatus : int {
    NewFrame   = 0,    // a new picture was written to the buffer
    NoChange   = 1,    // nothing changed since the last call (static screen / minimised window)
    SourceLost = 2,    // the source went away or must be rebuilt - the caller should call Reset()
    Error      = -1,
};

struct MonitorInfo {
    int         index   = 0;       // 1-based, like hr_get_monitor_info()
    int         left    = 0;       // position in the virtual desktop
    int         top     = 0;
    int         width   = 0;       // in physical pixels
    int         height  = 0;
    double      scale   = 1.0;     // HiDPI factor (1.0, 1.25, 2.0 ...)
    bool        primary = false;
    std::string name;              // "DP-1", "HDMI-A-1", "Monitor 2" ...
};

enum class CaptureKind { Monitor, Window, Region };

struct CaptureTarget {
    CaptureKind kind          = CaptureKind::Monitor;
    int         monitor_index = 1;         // Monitor: 1-based; Region: the monitor the rect is relative to (0 = virtual desktop)
    uint64_t    window_id     = 0;         // Window: X11 window id (Window) / ignored on Wayland (the portal asks the user)
    int         x = 0, y = 0, w = 0, h = 0;  // Region: rectangle in pixels
    bool        draw_cursor   = true;
};

// What a backend can do. Lets the UI/CLI grey things out instead of failing later.
struct CaptureCaps {
    bool        available            = false;  // the backend can be created in this session at all
    bool        monitor              = false;
    bool        window               = false;
    bool        region               = false;
    bool        cursor_toggle        = false;  // the cursor can be left out of the picture
    bool        needs_user_prompt    = false;  // Open() shows a system picker (Wayland ScreenCast portal)
    bool        reports_dirty_rows   = false;  // like hr_dx_get_dirty_rows()
    std::string backend_name;                  // "x11-shm", "x11-xcomposite", "pipewire-portal", "none"
    std::string unavailable_reason;            // human readable, filled when available == false
};

class IScreenCapture {
public:
    virtual ~IScreenCapture() = default;

    // Opens (or, for the Wayland portal, asks the user for) the source. May block - for the portal
    // until the user picked something. On failure returns false and fills *err when it is not null.
    virtual bool Open(const CaptureTarget &target, std::string *err) = 0;
    virtual void Close() = 0;

    // Size of the frames Grab() delivers. False before a successful Open().
    virtual bool GetSize(int *w, int *h) const = 0;

    // out_bgra must hold at least width*height*4 bytes. Waits up to timeout_ms for a new frame.
    virtual CaptureStatus Grab(uint8_t *out_bgra, int timeout_ms) = 0;

    // Rebuilds the source after Grab() returned SourceLost (display re-configured, window re-created).
    virtual bool Reset() = 0;

    virtual void SetCursorVisible(bool on) = 0;
    virtual const char *BackendName() const = 0;
};

// Capabilities of the backend that CreateScreenCapture() would pick for this display server.
CaptureCaps QueryCaptureCaps(DisplayServer ds);

// Creates the backend for the display server. Returns nullptr when there is none; *why_not then
// says why (not implemented yet, no $DISPLAY, portal missing ...). Never returns a half-working object.
std::unique_ptr<IScreenCapture> CreateScreenCapture(DisplayServer ds, std::string *why_not);

// Monitors of the current session (empty on failure, *err filled). Replaces hr_get_monitor_info /
// hr_monitor_count for the portable code; the Windows implementation can wrap the old functions.
std::vector<MonitorInfo> EnumerateMonitors(std::string *err);

}  // namespace platform
}  // namespace hr
