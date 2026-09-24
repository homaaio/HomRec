#include "preset_dialog.h"
#include "themed_widgets.h"
#include "hrc_config.h"
#include "../hr_log.h"

#include <wx/listctrl.h>
#include <wx/filedlg.h>
#include <wx/textdlg.h>
#include <wx/filename.h>

#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>

namespace {

const wchar_t kPresetsDir[] = L"presets";

// Strips characters Windows filenames can't contain, same denylist
// RecordingController::ResolveCaptureAppName() uses for the {app}
// template placeholder - kept consistent since both end up as a bare
// filename component.
std::wstring SanitizeFileNamePart(std::wstring s) {
    for (wchar_t &c : s) {
        if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    }
    // Trim trailing dots/spaces - Windows silently strips these from the
    // *actual* filename on disk, so leaving them in would make the name
    // shown in the list quietly disagree with what CreateFile() gives you.
    while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) s.pop_back();
    return s;
}

struct PresetEntry {
    wxString display_name;
    std::wstring path;
    bool is_main = false;  // the app's own auto-managed config - always first, never removable
};

class PresetDialog : public wxDialog {
public:
    PresetDialog(wxWindow *parent, AppState &state, const ThemeColors &theme, const LanguageTable &lang)
        : wxDialog(parent, wxID_ANY, wxString::FromUTF8("Set Preset"),
                   wxDefaultPosition, wxSize(480, 420),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          state_(state), theme_(theme), lang_(lang) {
        (void)lang_;
        wxColour bg = FromColorref(theme_.bg);
        wxColour surface = FromColorref(theme_.surface);
        wxColour text = FromColorref(theme_.text);
        wxColour textDim = FromColorref(theme_.text_secondary);
        SetBackgroundColour(bg);

        EnsurePresetsDir();

        auto *root = new wxBoxSizer(wxVERTICAL);

        auto *titleLbl = new wxStaticText(this, wxID_ANY, wxString::FromUTF8("Set Preset"));
        wxFont titleFont = titleLbl->GetFont();
        titleFont.SetPointSize(titleFont.GetPointSize() + 3);
        titleFont.SetWeight(wxFONTWEIGHT_BOLD);
        titleLbl->SetFont(titleFont);
        titleLbl->SetForegroundColour(text);
        titleLbl->SetBackgroundColour(bg);
        root->Add(titleLbl, 0, wxALL, 12);

        auto *note = new wxStaticText(this, wxID_ANY, wxString::FromUTF8(
            "A preset is a .hrc configuration file - the same format Export/Import "
            "Settings uses. \"Switch to\" loads one and makes it the active config; "
            "an overlay-only .hrc (e.g. a copy of homrec_overlays.hrc) layers just "
            "its overlays on top instead of replacing everything else."));
        note->Wrap(440);
        note->SetForegroundColour(textDim);
        note->SetBackgroundColour(bg);
        root->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

        list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SIMPLE);
        list_->InsertColumn(0, "Preset", wxLIST_FORMAT_LEFT, 300);
        list_->InsertColumn(1, "Type", wxLIST_FORMAT_LEFT, 100);
        list_->SetBackgroundColour(surface);
        list_->SetForegroundColour(text);
        root->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

        auto *btnCol = new wxBoxSizer(wxHORIZONTAL);
        auto *switchBtn = new ColorButton(this, IDC_SWITCH, "Switch to Selected");
        switchBtn->SetColours(FromColorref(theme_.success), FromColorref(theme_.bg));
        switchBtn->SetMinSize(wxSize(140, 28));
        btnCol->Add(switchBtn, 0, wxRIGHT, 8);

        auto *saveNewBtn = new ColorButton(this, IDC_SAVE_NEW, "Save Current As...");
        saveNewBtn->SetColours(surface, text);
        saveNewBtn->SetMinSize(wxSize(140, 28));
        btnCol->Add(saveNewBtn, 0, wxRIGHT, 8);

        auto *addBtn = new ColorButton(this, IDC_ADD, "Add Existing...");
        addBtn->SetColours(surface, text);
        addBtn->SetMinSize(wxSize(120, 28));
        btnCol->Add(addBtn, 0, wxRIGHT, 8);

        auto *removeBtn = new ColorButton(this, IDC_REMOVE, "Remove");
        removeBtn->SetColours(FromColorref(theme_.warning), FromColorref(theme_.bg));
        removeBtn->SetMinSize(wxSize(90, 28));
        btnCol->Add(removeBtn, 0);
        root->Add(btnCol, 0, wxEXPAND | wxALL, 12);

        auto *closeRow = new wxBoxSizer(wxHORIZONTAL);
        closeRow->AddStretchSpacer(1);
        auto *closeBtn = new ColorButton(this, wxID_CANCEL, "Close");
        closeBtn->SetColours(surface, text);
        closeBtn->SetMinSize(wxSize(80, 28));
        closeRow->Add(closeBtn, 0);
        root->Add(closeRow, 0, wxEXPAND | wxALL, 12);

        SetSizer(root);

        Bind(wxEVT_BUTTON, &PresetDialog::OnSwitch, this, IDC_SWITCH);
        Bind(wxEVT_BUTTON, &PresetDialog::OnSaveNew, this, IDC_SAVE_NEW);
        Bind(wxEVT_BUTTON, &PresetDialog::OnAdd, this, IDC_ADD);
        Bind(wxEVT_BUTTON, &PresetDialog::OnRemove, this, IDC_REMOVE);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); }, wxID_CANCEL);
        list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent &) { DoSwitch(); });

        RefreshList();
    }

    bool switched_something() const { return switched_; }

private:
    enum { IDC_SWITCH = wxID_HIGHEST + 1, IDC_SAVE_NEW, IDC_ADD, IDC_REMOVE };

    static void EnsurePresetsDir() {
        // Fine if it already exists - CreateDirectoryW just fails
        // harmlessly (ERROR_ALREADY_EXISTS) and every path built off
        // kPresetsDir below still resolves the same either way.
        CreateDirectoryW(kPresetsDir, nullptr);
    }

    // True for a file that only has an [overlays] section (no other
    // registry keys at all) - i.e. one written by SaveOverlaysOnly() or a
    // hand-trimmed copy of one, as opposed to a full settings export.
    // Purely cosmetic (the "Type" column) - HrcConfig::Load() doesn't
    // care either way, it just applies whatever keys are actually
    // present in the file.
    static bool LooksOverlayOnly(const std::wstring &path) {
        std::ifstream f(path.c_str(), std::ios::binary);
        if (!f) return false;
        std::string line;
        bool sawOverlaysHeader = false, sawOtherKey = false;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            if (line[0] == '[') {
                sawOverlaysHeader = sawOverlaysHeader || (line.rfind("[overlays]", 0) == 0);
                continue;
            }
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            if (key.rfind("overlay_", 0) != 0 && key != "overlay_count") sawOtherKey = true;
        }
        return sawOverlaysHeader && !sawOtherKey;
    }

    void RefreshList() {
        list_->DeleteAllItems();
        entries_.clear();

        PresetEntry main;
        main.display_name = "homrec.hrc  (main config)";
        main.path = HrcConfig::ResolveSettingsPath(state_);
        main.is_main = true;
        entries_.push_back(main);

        std::wstring pattern = std::wstring(kPresetsDir) + L"\\*.hrc";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        std::vector<std::wstring> names;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(names.begin(), names.end());
        for (const auto &name : names) {
            PresetEntry e;
            e.display_name = wxString(name.c_str());
            e.path = std::wstring(kPresetsDir) + L"\\" + name;
            entries_.push_back(e);
        }

        for (size_t i = 0; i < entries_.size(); ++i) {
            long row = list_->InsertItem((long)i, entries_[i].display_name);
            wxString type = entries_[i].is_main ? wxString("Active")
                             : LooksOverlayOnly(entries_[i].path) ? wxString("Overlays only")
                                                                    : wxString("Full config");
            list_->SetItem(row, 1, type);
        }
        if (!entries_.empty()) list_->SetItemState(0, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }

    int SelectedIndex() const {
        long sel = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (sel < 0 || (size_t)sel >= entries_.size()) return -1;
        return (int)sel;
    }

    void DoSwitch() {
        int idx = SelectedIndex();
        if (idx < 0) return;
        const PresetEntry &e = entries_[(size_t)idx];

        if (e.is_main) {
            // Just re-loads the currently-active config - a cheap way to
            // discard unsaved in-session tweaks and go back to what's on
            // disk, without having to leave the dialog.
            if (!HrcConfig::Load(state_, e.path)) {
                wxMessageBox("Couldn't read homrec.hrc.", "Set Preset", wxOK | wxICON_ERROR, this);
                return;
            }
            state_.active_preset_name = "default";
            HrLog::Info("Preset: reloaded the active config (homrec.hrc).");
        } else {
            // Explicit, interactive click on a preset the user picked
            // themselves - same trust level as the manual "Import
            // Settings..." menu item, so custom_ffmpeg_args is allowed
            // through (see HrcConfig::Load()'s header comment on
            // allow_sensitive_fields).
            if (!HrcConfig::Load(state_, e.path)) {
                wxMessageBox("Couldn't read that preset file.", "Set Preset", wxOK | wxICON_ERROR, this);
                return;
            }
            // Re-save as the active config so the switch survives a
            // restart - homrec.hrc (or whatever Settings > Advanced >
            // "Settings file path" points at) is what main_frame.cpp
            // loads on startup, and presets\ itself is never read at
            // startup on its own.
            std::wstring active_path = HrcConfig::ResolveSettingsPath(state_);
            if (!HrcConfig::Save(state_, active_path)) {
                HrLog::Error("Preset: switched in-session but couldn't persist to the active config file.");
            }
            {
                std::string base(e.display_name.ToUTF8());
                if (base.size() > 4 && base.compare(base.size() - 4, 4, ".hrc") == 0) {
                    base.resize(base.size() - 4);
                }
                state_.active_preset_name = base;
            }
            HrLog::Info("Preset: switched to " + std::string(e.display_name.ToUTF8()));
        }
        switched_ = true;
        EndModal(wxID_OK);
    }

    void OnSwitch(wxCommandEvent &) { DoSwitch(); }

    void OnSaveNew(wxCommandEvent &) {
        wxTextEntryDialog dlg(this, "Name for this preset:", "Save Current As...", "");
        if (dlg.ShowModal() != wxID_OK) return;
        std::wstring name = SanitizeFileNamePart(dlg.GetValue().ToStdWstring());
        if (name.empty()) {
            wxMessageBox("That name isn't usable as a filename.", "Save Current As...", wxOK | wxICON_WARNING, this);
            return;
        }
        std::wstring path = std::wstring(kPresetsDir) + L"\\" + name + L".hrc";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (wxMessageBox("A preset with that name already exists. Overwrite it?",
                              "Save Current As...", wxYES_NO | wxICON_WARNING, this) != wxYES) {
                return;
            }
        }
        if (HrcConfig::Save(state_, path)) {
            HrLog::Info("Preset: saved current settings as " + std::string(name.begin(), name.end()));
            RefreshList();
        } else {
            wxMessageBox("Couldn't write that preset file.", "Save Current As...", wxOK | wxICON_ERROR, this);
        }
    }

    void OnAdd(wxCommandEvent &) {
        wxFileDialog dlg(this, "Add Existing Preset", wxEmptyString, wxEmptyString,
                          "HomRec Config (*.hrc)|*.hrc|All files (*.*)|*.*",
                          wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK) return;

        std::wstring src = dlg.GetPath().ToStdWstring();
        std::wstring base = wxFileName(dlg.GetPath()).GetName().ToStdWstring();
        base = SanitizeFileNamePart(base);
        if (base.empty()) base = L"preset";
        std::wstring dest = std::wstring(kPresetsDir) + L"\\" + base + L".hrc";
        // Don't silently clobber an existing preset that happens to share
        // a name - suffix " (2)", " (3)", ... until a free name is found,
        // same de-duplication idea a Windows Explorer copy uses.
        int n = 2;
        while (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
            dest = std::wstring(kPresetsDir) + L"\\" + base + L" (" + std::to_wstring(n++) + L").hrc";
        }
        if (CopyFileW(src.c_str(), dest.c_str(), TRUE)) {
            HrLog::Info("Preset: added " + std::string(dlg.GetPath().ToUTF8()));
            RefreshList();
        } else {
            wxMessageBox("Couldn't copy that file into the presets folder.", "Add Existing Preset",
                         wxOK | wxICON_ERROR, this);
        }
    }

    void OnRemove(wxCommandEvent &) {
        int idx = SelectedIndex();
        if (idx < 0) return;
        const PresetEntry &e = entries_[(size_t)idx];
        if (e.is_main) {
            wxMessageBox("The main config (homrec.hrc) can't be removed here - "
                         "it's the file the app itself loads on startup.",
                         "Remove Preset", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (wxMessageBox("Remove \"" + e.display_name + "\"? This deletes the file and can't be undone.",
                          "Remove Preset", wxYES_NO | wxICON_WARNING, this) != wxYES) {
            return;
        }
        if (DeleteFileW(e.path.c_str())) {
            HrLog::Info("Preset: removed " + std::string(e.display_name.ToUTF8()));
            RefreshList();
        } else {
            wxMessageBox("Couldn't delete that file.", "Remove Preset", wxOK | wxICON_ERROR, this);
        }
    }

    AppState &state_;
    ThemeColors theme_;
    LanguageTable lang_;
    wxListCtrl *list_ = nullptr;
    std::vector<PresetEntry> entries_;
    bool switched_ = false;
};

} // namespace

bool ShowPresetDialog(wxWindow *parent, AppState &state, const ThemeColors &theme, const LanguageTable &lang) {
    PresetDialog dlg(parent, state, theme, lang);
    dlg.ShowModal();
    return dlg.switched_something();
}
