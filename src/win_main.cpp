// win_main.cpp - wxWidgets app entry point.
#include <wx/wx.h>
#include "ui/main_frame.h"
#include "ui/version.h"
#include "utils/crash_handler.h"
#include "utils/log.h"

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
typedef HANDLE DPI_AWARENESS_CONTEXT;
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

extern "C" int hr_acquire_single_instance(const char *mutex_name);
// Implemented in pipeline.cpp - see g_handed_off_pipelines' comment
// there for what this is waiting on and why OnExit() below calls it.
extern "C" int hr_pl_wait_all_detached(int timeout_ms);

namespace {

void EnablePerMonitorDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        // Windows 10 1703+
        using SetCtxFn = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
        auto setCtx = reinterpret_cast<SetCtxFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setCtx && setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        // Windows 8.1+
        using SetAwarenessFn = HRESULT(WINAPI *)(int);
        auto setAwareness = reinterpret_cast<SetAwarenessFn>(
            GetProcAddress(shcore, "SetProcessDpiAwareness"));
        constexpr int kPerMonitorAware = 2; // PROCESS_PER_MONITOR_DPI_AWARE
        bool ok = setAwareness && SUCCEEDED(setAwareness(kPerMonitorAware));
        FreeLibrary(shcore);
        if (ok) return;
    }
    SetProcessDPIAware();
}

} // namespace

class HomRecApp : public wxApp {
public:
    bool OnInit() override {
        // Must run before any window/monitor coordinate is ever queried -
        // before wx creates anything, before HrCrashHandler::Install()
        EnablePerMonitorDpiAwareness();
        HrCrashHandler::Install();

        if (!wxApp::OnInit()) return false;
        wxInitAllImageHandlers();

        if (!hr_acquire_single_instance(HR_SINGLE_INSTANCE_MUTEX_NAME)) {
            wxMessageBox("HomRec is already running.", "HomRec", wxOK | wxICON_INFORMATION);
            return false;
        }

        auto *frame = new HomRecMainFrame();
        frame->Show(true);
        return true;
    }

    int OnExit() override {
        if (!hr_pl_wait_all_detached(5000)) {
            HrLog::Error("Shutdown: a capture pipeline still hadn't finished cleaning up after "
                         "5s - closing anyway. If this keeps happening, it may show up as a "
                         "crash right around app close.");
        }
        return wxApp::OnExit();
    }
};

wxIMPLEMENT_APP(HomRecApp);
