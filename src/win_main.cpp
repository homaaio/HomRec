// win_main.cpp — wxWidgets app entry point.
//
// Replaces the old raw wWinMain()+GetMessage/DispatchMessage loop:
// wxIMPLEMENT_APP() below generates the actual WinMain and runs wx's own
// message loop (which is still a real Win32 GetMessage/DispatchMessage
// loop under the hood on MSW, just wrapped). Single-instance check moved
// into wxApp::OnInit(), same hr_acquire_single_instance() call as before —
// no reimplementation, just relocated.
#include <wx/wx.h>
#include "ui/main_frame.h"
#include "ui/version.h"

extern "C" int hr_acquire_single_instance(const char *mutex_name);

class HomRecApp : public wxApp {
public:
    bool OnInit() override {
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
