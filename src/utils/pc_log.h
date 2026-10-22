// pc_log.h - periodic "how's the machine doing" logger (logs\pc.log).
#pragma once

#include <string>

namespace HrPcLog {
    void SetEnabled(bool enabled);
    bool IsEnabled();

    void MaybeLogSnapshot(bool is_recording, double current_fps, bool hw_encoder_active,
                           const std::string &recordings_folder);
}
