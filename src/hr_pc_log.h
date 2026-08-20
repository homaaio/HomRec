// hr_pc_log.h - periodic "how's the machine doing" logger (logs\pc.log).
//
// Separate from homrec.log (events: recording started, settings saved,
// errors) - this is numeric hardware/program-state samples instead, on
// their own timeline, so someone comparing performance across two
// different PCs (the actual point of this file) can pull just the
// samples without homrec.log's event noise in between, and vice versa.
#pragma once

#include <string>

namespace HrPcLog {
    // Call from an existing periodic tick (main_frame.cpp's stats_timer_,
    // ~500ms) - this function throttles itself internally (one line
    // roughly every kIntervalSeconds, see the .cpp) so callers don't need
    // to manage their own timer just for this, and does its own CPU/mem/
    // disk sampling without blocking (no Sleep(), unlike hr_get_sys_stats()
    // in hr_ui_utils.cpp - that one's ~100ms blocking sample is fine for
    // the on-demand PC Analytics dialog it was built for, not for a call
    // sitting on stats_timer_'s 500ms UI-thread tick indefinitely).
    // is_recording/current_fps/hw_encoder_active let one sample line show
    // both machine load and what the app itself was doing at that moment;
    // recordings_folder reuses the same disk-space check
    // RecordingController::Start() already does (hr_get_free_disk_mb()) so
    // pc.log's disk number is the one that actually matters for this app
    // (the configured output folder's volume) rather than just the drive
    // HomRec happens to be installed on.
    void MaybeLogSnapshot(bool is_recording, double current_fps, bool hw_encoder_active,
                           const std::string &recordings_folder);
}
