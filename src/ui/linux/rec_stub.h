// rec_stub.h - RecordingController on Linux: the "not now" placeholder.
//
// The Windows RecordingController (ui/recording_controller.h) drives DXGI / WASAPI / ffmpeg. On
// Linux the recording backend comes later (docs/LINUX_PORT_PLAN.md, phases 2-4), so the GUI gets
// this small class with the same name and the calls the dialogs make. Everything that would need
// a capture or encoder backend answers "not now" instead of failing silently.
#pragma once

#include "../app_state.h"
#include <string>

class RecordingController {
public:
    explicit RecordingController(AppState &state) : state_(state) {}

    static const wchar_t *NotNow() { return L"Recording is not available on Linux yet (not now)."; }

    bool Start(std::wstring &error_out) { error_out = NotNow(); return false; }
    void Stop() {}

    // Settings > Video > "Test Recording" (the button stays disabled while there is no controller).
    struct QuickTestResult {
        double avg_fps = 0.0;
        long long frames = 0;
        long long drops = 0;
        int width = 0, height = 0;
        std::string encoder_used;
        bool overloaded = false;
    };
    bool StartQuickTest(const std::string &, const std::string &, int, std::wstring &error_out) {
        error_out = NotNow();
        return false;
    }
    bool PollQuickTest() { return false; }
    QuickTestResult FinishQuickTest() { return QuickTestResult(); }

private:
    [[maybe_unused]] AppState &state_;
};
