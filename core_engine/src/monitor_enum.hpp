#pragma once
/*
 * monitor_enum.hpp — HomRec 2.0
 * Lightweight DXGI monitor enumeration.
 *
 * Used by the UI layer (Python via bindings) to populate the monitor
 * drop-down without initialising a full capture pipeline.
 *
 * Usage (C++):
 *   auto monitors = homrec::enumerate_monitors();
 *   for (auto& m : monitors)
 *       printf("[%d] %s  %ux%u  %u Hz\n",
 *              m.index, m.device_name.c_str(),
 *              m.width, m.height, m.refresh_hz);
 *
 * Usage (Python via bindings):
 *   import homrec_core
 *   for m in homrec_core.enumerate_monitors():
 *       print(m.index, m.friendly_name, m.width, m.height, m.refresh_hz)
 */

#include <cstdint>
#include <string>
#include <vector>
#include <dxgi.h>

namespace homrec {

struct MonitorInfo {
    int         index        = 0;
    std::string device_name;   // e.g. "\\.\DISPLAY1"
    std::string friendly_name; // human-readable, e.g. "Dell U2723D (1)"
    UINT32      width        = 0;
    UINT32      height       = 0;
    UINT32      refresh_hz   = 0;
    bool        is_primary   = false;
    RECT        desktop_rect {};
};

// Enumerate all active DXGI outputs.
// Returns empty vector on failure (e.g. no D3D11 device available).
std::vector<MonitorInfo> enumerate_monitors();

} // namespace homrec
