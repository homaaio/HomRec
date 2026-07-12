// win_main.cpp — Phase 1
//
// Port of the __main__ block in src/homrec.py: single-instance guard via
// the existing hr_acquire_single_instance() (hr_app_logic.cpp) — no
// reimplementation needed, just a direct call instead of going through the
// ctypes bridge homrec_native.py used to provide.
#include <windows.h>
#include "ui/main_window.h"
#include "ui/version.h"

extern "C" int hr_acquire_single_instance(const char *mutex_name);

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    if (!hr_acquire_single_instance(HR_SINGLE_INSTANCE_MUTEX_NAME)) {
        MessageBoxW(nullptr, L"HomRec is already running.", L"HomRec", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    HomRecMainWindow *window = HomRecMainWindow::Create(hInstance, nCmdShow);
    if (!window) {
        MessageBoxW(nullptr, L"Failed to create the main window.", L"HomRec", MB_OK | MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
