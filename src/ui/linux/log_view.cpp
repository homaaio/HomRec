// log_view.cpp - View > Show Log: reads the files of the log folder (homrec.log, pc.log, plugins.log,
// wgc.log, ...) into a read-only text box. Refresh re-reads, Clear empties the shown file.
#include "dialogs.h"
#include "../themed_widgets.h"
#include "../../platform/paths.h"
#include "../../utils/log_paths.h"
#include "../../utils/str_convert.h"

#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <vector>
#include <wx/utils.h>

namespace {

// Only the tail of a big file is shown - a 5 MB log in a wxTextCtrl is slow for no benefit.
constexpr size_t kMaxShown = 256 * 1024;

class LogViewDialog : public wxDialog {
public:
    LogViewDialog(wxWindow *parent, const ThemeColors &theme)
        : wxDialog(parent, wxID_ANY, "Log", wxDefaultPosition, wxSize(760, 520),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          theme_(theme) {
        auto *root = new wxBoxSizer(wxVERTICAL);

        auto *top = new wxBoxSizer(wxHORIZONTAL);
        top->Add(new wxStaticText(this, wxID_ANY, "File:"), 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
        file_choice_ = new wxChoice(this, wxID_ANY);
        top->Add(file_choice_, 0, wxRIGHT, 12);
        path_lbl_ = new wxStaticText(this, wxID_ANY, "");
        top->Add(path_lbl_, 1, wxALIGN_CENTRE_VERTICAL);
        root->Add(top, 0, wxEXPAND | wxALL, 12);

        text_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                               wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP | wxHSCROLL);
        text_->SetFont(wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE)));
        root->Add(text_, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

        auto *btns = new wxBoxSizer(wxHORIZONTAL);
        auto mk = [&](int id, const char *label, wxColour bg, wxColour fg) {
            auto *b = new ColorButton(this, id, label);
            b->SetColours(bg, fg);
            b->SetMinSize(wxSize(110, 30));
            btns->Add(b, 0, wxRIGHT, 8);
        };
        mk(kRefresh, "Refresh", FromColorref(theme_.accent), FromColorref(theme_.bg));
        mk(kOpenDir, "Open folder", FromColorref(theme_.surface_light), FromColorref(theme_.text));
        mk(kClear, "Clear", FromColorref(theme_.warning), FromColorref(theme_.bg));
        btns->AddStretchSpacer(1);
        mk(wxID_CLOSE, "Close", FromColorref(theme_.surface_light), FromColorref(theme_.text));
        root->Add(btns, 0, wxEXPAND | wxALL, 12);

        SetSizer(root);
        ThemeWindow(this, theme_);

        FillFiles();
        Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { Load(); });
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { FillFiles(); }, kRefresh);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            const std::string dir = HrNarrow(HrLogPaths::LogsDir());
            if (!wxLaunchDefaultApplication(wxString::FromUTF8(dir)))
                wxMessageBox("Couldn't open the folder:\n" + wxString::FromUTF8(dir), "Log", wxOK | wxICON_WARNING, this);
        }, kOpenDir);
        Bind(wxEVT_BUTTON, &LogViewDialog::OnClear, this, kClear);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CLOSE); }, wxID_CLOSE);
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &) { EndModal(wxID_CLOSE); });
        CentreOnParent();
    }

private:
    enum { kRefresh = wxID_HIGHEST + 1, kOpenDir, kClear };

    // The *.log files of the folder; homrec.log first (and selected on a fresh open).
    void FillFiles() {
        wxString keep = file_choice_->GetSelection() >= 0 ? file_choice_->GetStringSelection() : wxString("homrec.log");
        std::vector<std::string> names;
        const std::string dir = HrNarrow(HrLogPaths::LogsDir());
        if (DIR *d = opendir(dir.c_str())) {
            while (dirent *e = readdir(d)) {
                const std::string n = e->d_name;
                if (n.size() > 4 && n.compare(n.size() - 4, 4, ".log") == 0) names.push_back(n);
            }
            closedir(d);
        }
        std::sort(names.begin(), names.end());
        if (std::find(names.begin(), names.end(), "homrec.log") == names.end()) names.insert(names.begin(), "homrec.log");
        file_choice_->Clear();
        for (const auto &n : names) file_choice_->Append(wxString::FromUTF8(n));
        int sel = file_choice_->FindString(keep);
        file_choice_->SetSelection(sel >= 0 ? sel : 0);
        Load();
    }

    std::string CurrentPath() const {
        const int i = file_choice_->GetSelection();
        if (i < 0) return std::string();
        return HrNarrow(HrLogPaths::LogsDir()) + "/" + std::string(file_choice_->GetString((unsigned)i).ToUTF8().data());
    }

    void Load() {
        const std::string path = CurrentPath();
        path_lbl_->SetLabel(wxString::FromUTF8(path));
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            text_->SetValue("Log file not found:\n" + wxString::FromUTF8(path));
            return;
        }
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.size() > kMaxShown) {
            bytes.erase(0, bytes.size() - kMaxShown);
            const size_t nl = bytes.find('\n');
            if (nl != std::string::npos) bytes.erase(0, nl + 1);      // start on a whole line
            bytes.insert(0, "[... only the last part of the file is shown ...]\n");
        }
        wxString s = wxString::FromUTF8(bytes.data(), bytes.size());
        if (s.empty() && !bytes.empty()) s = wxString::From8BitData(bytes.data(), bytes.size());   // not UTF-8
        text_->SetValue(s);
        text_->ShowPosition(text_->GetLastPosition());                   // newest lines at the bottom
    }

    void OnClear(wxCommandEvent &) {
        const std::string path = CurrentPath();
        if (path.empty()) return;
        if (wxMessageBox("Empty " + file_choice_->GetStringSelection() + "? This can't be undone.", "Log",
                         wxYES_NO | wxICON_QUESTION, this) != wxYES) return;
        if (HrLogPaths::TruncateLogFile(HrWide(path)) < 0)
            wxMessageBox("Couldn't write to that file.", "Log", wxOK | wxICON_WARNING, this);
        Load();
    }

    ThemeColors theme_;
    wxChoice *file_choice_ = nullptr;
    wxStaticText *path_lbl_ = nullptr;
    wxTextCtrl *text_ = nullptr;
};

}  // namespace

void ShowLogViewerDialog(wxWindow *parent, const ThemeColors &theme) {
    LogViewDialog dlg(parent, theme);
    dlg.ShowModal();
}
