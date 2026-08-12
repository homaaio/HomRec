// win_main.cpp - wxWidgets app entry point.
//
// Replaces the old raw wWinMain()+GetMessage/DispatchMessage loop:
// wxIMPLEMENT_APP() below generates the actual WinMain and runs wx's own
// message loop (which is still a real Win32 GetMessage/DispatchMessage
// loop under the hood on MSW, just wrapped). Single-instance check moved
// into wxApp::OnInit(), same hr_acquire_single_instance() call as before -
// no reimplementation, just relocated.
#include <wx/wx.h>
#include "ui/main_frame.h"
#include "ui/version.h"
#include "hr_crash_handler.h"

// Both predate some SDK header snapshots this project's MinGW-w64
// toolchain may ship with (window_picker_dialog.cpp hits the same thing
// for a couple of DWM constants) - given by hand with a fallback name so
// EnablePerMonitorDpiAwareness() below compiles either way.
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
typedef HANDLE DPI_AWARENESS_CONTEXT;
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

extern "C" int hr_acquire_single_instance(const char *mutex_name);

namespace {

// Without this, Windows treats the process as DPI-unaware and silently
// virtualizes every GetWindowRect()/GetMonitorInfoW() coordinate down to a
// 96-DPI space on any display running above 100% scaling (the Windows
// default on most modern laptop panels, including higher-res ones like a
// UHD-graphics 1920x1080 or higher screen). DXGI Desktop Duplication
// (hr_dxgi_capture.cpp) is never virtualized - it always reports/captures
// true physical pixels, regardless of the caller's DPI awareness. That
// mismatch is exactly what made window-capture crop selection resolve to
// the wrong rectangle (computed in one coordinate space, applied to a
// buffer in another) - see RecordingController::ResolveCaptureSize()'s
// crop math and Pipeline::src_w/h in hr_pipeline.cpp for where those two
// spaces actually collide. Declaring Per-Monitor-V2 awareness here (the
// programmatic equivalent of an app manifest's <dpiAwareness> entry, and
// preferred over one since it works without shipping a separate .manifest
// file) makes every one of those APIs agree with DXGI from the start.
//
// Tried newest-to-oldest and loaded dynamically (GetProcAddress, not a
// direct link) since not all three exist on every supported Windows
// version - same reasoning/pattern as hr_crash_handler.cpp's dbghelp.dll
// loading and hr_display_info.cpp's GetDpiForMonitor lookup.
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
    // Vista+ fallback: still fixes the crop-math mismatch on a single
    // monitor (the common case this codebase's own comments describe -
    // "if the person has a second monitor" is flagged elsewhere as a
    // separate, not-yet-handled case), just without correct scaling if the
    // window is later dragged between two monitors running different DPIs.
    SetProcessDPIAware();
}

} // namespace

class HomRecApp : public wxApp {
public:
    bool OnInit() override {
        // Must run before any window/monitor coordinate is ever queried -
        // before wx creates anything, before HrCrashHandler::Install()
        // even (that one doesn't query coordinates, so the exact order
        // between the two doesn't matter, but this comes first on
        // principle: "nothing touches a screen coordinate before this
        // runs" is a much easier invariant to keep than "nothing except
        // the crash handler").
        EnablePerMonitorDpiAwareness();
        HrCrashHandler::Install();

        if (!wxApp::OnInit()) return false;

        if (!hr_acquire_single_instance(HR_SINGLE_INSTANCE_MUTEX_NAME)) {
            wxMessageBox("HomRec is already running.", "HomRec", wxOK | wxICON_INFORMATION);
            return false;
        }

        auto *frame = new HomRecMainFrame();
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(HomRecApp);
