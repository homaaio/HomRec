// app.cpp - wxWidgets app entry point for Linux (the Windows one is src/win_main.cpp).
#include <wx/wx.h>
#include <wx/image.h>
#include <wx/snglinst.h>
#include <memory>

#include "main_frame.h"
#include "../version.h"
#include "../../platform/paths.h"
#include "../../utils/log.h"

class HomRecApp : public wxApp {
public:
    bool OnInit() override {
        SetAppName("homrec");
        SetVendorName("HomRec");

        if (!wxApp::OnInit()) return false;
        wxInitAllImageHandlers();

        // One HomRec per user: the lock file sits in the config folder.
        hr::platform::EnsureDir(hr::platform::ConfigDir());
        checker_ = std::make_unique<wxSingleInstanceChecker>();
        if (!checker_->Create("homrec-" + wxGetUserId() + ".lock", wxString::FromUTF8(hr::platform::ConfigDir()))) {
            HrLog::Warn("Single-instance check could not be created - starting anyway.");
        } else if (checker_->IsAnotherRunning()) {
            wxMessageBox("HomRec is already running.", "HomRec", wxOK | wxICON_INFORMATION);
            return false;
        }

        auto *frame = new MainFrame();
        frame->Show(true);
        return true;
    }

private:
    std::unique_ptr<wxSingleInstanceChecker> checker_;
};

wxIMPLEMENT_APP(HomRecApp);
