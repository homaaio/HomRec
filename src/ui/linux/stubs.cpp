// stubs.cpp - Linux answers for the few engine functions the shared UI files call, until the real
// backends exist. Each one is deleted when its backend lands (see platform/linux/backend_stubs.cpp
// for the same idea one layer down).
#include "../app_state.h"

// Window capture crash guard (wgc_capture.cpp on Windows): there is no WGC here, nothing can trip it.
void HrWgcResetCrashGuard() {}
bool HrWgcCrashGuardTripped() { return false; }

// Old settings files may still say "record this window / region". That became a layer on Windows;
// layers come with the overlays, so for now the capture source simply goes back to the desktop.
void HrMigrateCaptureSourceToLayers(AppState &state) {
    state.capture_mode = CaptureMode::Desktop;
}
