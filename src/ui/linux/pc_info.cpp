// pc_info.cpp - View > PC Analytics: cores, CPU, RAM and the disk of the output folder.
// Numbers come from hr_get_sys_stats() (utils/ui_utils.cpp, reads /proc and statvfs on Linux).
#include "dialogs.h"
#include "../themed_widgets.h"

#include <cstdint>
#include <wx/timer.h>

extern "C" {
// Same layout as HrSysStats in ui_utils.cpp.
struct HrSysStats {
    float    cpu_percent;
    uint64_t ram_total_mb;
    uint64_t ram_avail_mb;
    float    ram_percent;
    uint64_t disk_total_gb;
    uint64_t disk_free_gb;
    float    disk_percent;
    int      cpu_count;
};
int hr_get_sys_stats(const char *disk_path, HrSysStats *out);
}

namespace {

class PcInfoDialog : public wxDialog {
public:
    PcInfoDialog(wxWindow *parent, const ThemeColors &theme, const std::string &disk_path)
        : wxDialog(parent, wxID_ANY, "PC Analytics", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE),
          disk_path_(disk_path), timer_(this) {
        static const char *const kRows[8] = {
            "Cores:", "CPU usage:", "RAM total:", "RAM avail:", "RAM used:",
            "Disk total:", "Disk free:", "Disk used:",
        };
        auto *root = new wxBoxSizer(wxVERTICAL);
        auto *grid = new wxFlexGridSizer(2, 8, 20);
        for (int i = 0; i < 8; ++i) {
            grid->Add(new wxStaticText(this, wxID_ANY, kRows[i]), 0, wxALIGN_CENTRE_VERTICAL);
            value_[i] = new wxStaticText(this, wxID_ANY, "...", wxDefaultPosition, wxSize(130, -1));
            grid->Add(value_[i], 0, wxALIGN_CENTRE_VERTICAL);
        }
        root->Add(grid, 0, wxALL, 20);

        auto *btns = new wxBoxSizer(wxHORIZONTAL);
        auto *refresh = new ColorButton(this, kRefresh, "Refresh");
        refresh->SetColours(FromColorref(theme.accent), FromColorref(theme.bg));
        refresh->SetMinSize(wxSize(100, 30));
        auto *close = new ColorButton(this, wxID_CLOSE, "Close");
        close->SetColours(FromColorref(theme.surface_light), FromColorref(theme.text));
        close->SetMinSize(wxSize(100, 30));
        btns->Add(refresh, 0, wxRIGHT, 8);
        btns->Add(close, 0);
        root->Add(btns, 0, wxALIGN_CENTRE_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, 20);

        SetSizerAndFit(root);
        ThemeWindow(this, theme);

        Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { ReadStats(); }, kRefresh);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CLOSE); }, wxID_CLOSE);
        Bind(wxEVT_TIMER, [this](wxTimerEvent &) { ReadStats(); });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &) { EndModal(wxID_CLOSE); });
        ReadStats();
        timer_.Start(1000);     // the CPU figure is a delta between two calls - a steady tick keeps it meaningful
        CentreOnParent();
    }

private:
    enum { kRefresh = wxID_HIGHEST + 1 };

    void ReadStats() {
        HrSysStats s{};
        if (!hr_get_sys_stats(disk_path_.c_str(), &s)) {
            for (auto *v : value_) v->SetLabel("n/a");
            return;
        }
        value_[0]->SetLabel(wxString::Format("%d", s.cpu_count));
        value_[1]->SetLabel(wxString::Format("%.0f %%", s.cpu_percent));
        value_[2]->SetLabel(wxString::Format("%llu MB", (unsigned long long)s.ram_total_mb));
        value_[3]->SetLabel(wxString::Format("%llu MB", (unsigned long long)s.ram_avail_mb));
        value_[4]->SetLabel(wxString::Format("%.0f %%", s.ram_percent));
        value_[5]->SetLabel(wxString::Format("%llu GB", (unsigned long long)s.disk_total_gb));
        value_[6]->SetLabel(wxString::Format("%llu GB", (unsigned long long)s.disk_free_gb));
        value_[7]->SetLabel(wxString::Format("%.0f %%", s.disk_percent));
    }

    std::string disk_path_;
    wxStaticText *value_[8] = {};
    wxTimer timer_;
};

}  // namespace

void ShowPcAnalyticsDialog(wxWindow *parent, const ThemeColors &theme, const std::string &disk_path) {
    PcInfoDialog dlg(parent, theme, disk_path);
    dlg.ShowModal();
}
