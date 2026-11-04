// console.cpp - Help > Console on Linux: a small command window.
//
// Windows' console_window.cpp is a raw-Win32 window tied to plugins and the recorder. This one is
// wx and knows the commands that make sense without a recording backend:
//   help, clear, echo, status, info, paths, log [N], ls [path], vars [filter], save,
//   <key>            show a setting          (every key of core/settings_registry.cpp)
//   <key> = <value>  change a setting        ("save" writes it to the .hrc file)
// Settings that are marked "sensitive" (they run programs) can not be changed from here yet.
#include "dialogs.h"
#include "../themed_widgets.h"
#include "../hrc_config.h"
#include "../version.h"
#include "../../commands/ls.h"
#include "../../core/settings_registry.h"
#include "../../platform/platform.h"
#include "../../platform/paths.h"
#include "../../utils/log_paths.h"
#include "../../utils/str_convert.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <vector>
#include <wx/frame.h>

namespace {

std::string Trim(const std::string &s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::string Lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

class ConsoleFrame : public wxFrame {
public:
    ConsoleFrame(wxWindow *parent, AppState &state, const ThemeColors &theme)
        : wxFrame(parent, wxID_ANY, "HomRec Console", wxDefaultPosition, wxSize(720, 460)),
          state_(state), theme_(theme) {
        auto *root = new wxPanel(this);
        auto *sizer = new wxBoxSizer(wxVERTICAL);

        out_ = new wxTextCtrl(root, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                              wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxTE_DONTWRAP);
        out_->SetFont(wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE)));
        sizer->Add(out_, 1, wxEXPAND | wxALL, 8);

        in_ = new wxTextCtrl(root, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                             wxTE_PROCESS_ENTER);
        in_->SetFont(out_->GetFont());
        sizer->Add(in_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        root->SetSizer(sizer);

        auto *fs = new wxBoxSizer(wxVERTICAL);
        fs->Add(root, 1, wxEXPAND);
        SetSizer(fs);

        ThemeWindow(root, theme_);
        out_->SetBackgroundColour(FromColorref(theme_.preview_bg));
        in_->SetBackgroundColour(FromColorref(theme_.surface));
        SetBackgroundColour(FromColorref(theme_.bg));

        in_->Bind(wxEVT_TEXT_ENTER, &ConsoleFrame::OnEnter, this);
        in_->Bind(wxEVT_KEY_DOWN, &ConsoleFrame::OnKey, this);
        Bind(wxEVT_CLOSE_WINDOW, &ConsoleFrame::OnClose, this);

        Print("HomRec " HR_APP_VERSION " console (Linux). Type 'help'.", Kind::Info);
        in_->SetFocus();
    }

    void SetSelfPointer(ConsoleFrame **slot) { slot_ = slot; }

private:
    enum class Kind { Plain, Info, Warn, Err, Cmd };

    // Three overloads so "text" (a literal), std::string and wxString all pick one without ambiguity.
    void Print(const char *line, Kind k = Kind::Plain) { Print(wxString::FromUTF8(line), k); }
    void Print(const std::string &line, Kind k = Kind::Plain) { Print(wxString::FromUTF8(line), k); }

    void Print(const wxString &line, Kind k = Kind::Plain) {
        wxColour c = FromColorref(theme_.text);
        switch (k) {
            case Kind::Info: c = FromColorref(theme_.accent); break;
            case Kind::Warn: c = FromColorref(theme_.warning); break;
            case Kind::Err:  c = FromColorref(theme_.error); break;
            case Kind::Cmd:  c = FromColorref(theme_.success); break;
            default: break;
        }
        out_->SetDefaultStyle(wxTextAttr(c));
        out_->AppendText(line + "\n");
    }

    void OnKey(wxKeyEvent &evt) {
        const int code = evt.GetKeyCode();
        if (code == WXK_UP && !history_.empty()) {
            if (hist_pos_ > 0) --hist_pos_;
            in_->SetValue(wxString::FromUTF8(history_[hist_pos_]));
            in_->SetInsertionPointEnd();
            return;
        }
        if (code == WXK_DOWN && !history_.empty()) {
            if (hist_pos_ + 1 < history_.size()) {
                ++hist_pos_;
                in_->SetValue(wxString::FromUTF8(history_[hist_pos_]));
            } else {
                hist_pos_ = history_.size();
                in_->Clear();
            }
            in_->SetInsertionPointEnd();
            return;
        }
        evt.Skip();
    }

    void OnEnter(wxCommandEvent &) {
        const std::string line = Trim(in_->GetValue().ToUTF8().data());
        in_->Clear();
        if (line.empty()) return;
        if (history_.empty() || history_.back() != line) history_.push_back(line);
        hist_pos_ = history_.size();
        Print("> " + line, Kind::Cmd);
        Run(line);
    }

    void OnClose(wxCloseEvent &evt) {
        if (slot_) *slot_ = nullptr;
        evt.Skip();      // the default handler destroys the window
    }

    // ---- commands -----------------------------------------------------------------------------

    void Run(const std::string &line) {
        // "<key> = <value>"
        const size_t eq = line.find('=');
        if (eq != std::string::npos && line.compare(0, 2, "ls") != 0) {
            SetVar(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)));
            return;
        }

        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;
        std::string rest;
        std::getline(ss, rest);
        rest = Trim(rest);
        const std::string lc = Lower(cmd);

        if (lc == "help" || lc == "?")      return Help();
        if (lc == "clear" || lc == "cls")   { out_->Clear(); return; }
        if (lc == "echo")                   { Print(rest); return; }
        if (lc == "status")                 return Status();
        if (lc == "info")                   return Info();
        if (lc == "paths")                  return Paths();
        if (lc == "log")                    return ShowLog(rest);
        if (lc == "ls")                     return Ls(line);
        if (lc == "vars")                   return Vars(rest);
        if (lc == "save")                   return Save();
        if (lc == "exit" || lc == "close")  { Close(); return; }

        // A bare setting name shows its value.
        if (const auto *def = HrSettingsRegistry::Find(cmd)) {
            Print(def->key + " = " + def->get(state_));
            return;
        }
        Print("Unknown command or setting '" + cmd + "'. Type 'help'.", Kind::Err);
    }

    void Help() {
        Print("Commands:", Kind::Info);
        Print("  help                 this text");
        Print("  clear                empty the window");
        Print("  echo <text>          print text");
        Print("  status               version, recording state");
        Print("  info                 session, sound server, ffmpeg");
        Print("  paths                config / data / log / recordings folders");
        Print("  log [N]              last N lines of homrec.log (default 20)");
        Print("  ls [path]            list files (. = the config folder)");
        Print("  vars [filter]        list setting names");
        Print("  <key>                show a setting");
        Print("  <key> = <value>      change a setting (save writes it to the .hrc file)");
        Print("  save                 write the settings file");
        Print("  exit                 close the console");
    }

    void Status() {
        Print("HomRec " HR_APP_VERSION " on Linux");
        Print("recording : not available yet (not now)");
        Print(std::string("theme     : ") + state_.current_theme + ", language: " + state_.current_language);
        Print("output    : " + state_.output_folder);
    }

    void Info() {
        const hr::platform::SessionInfo s = hr::platform::DetectSession();
        Print(std::string("display server : ") + hr::platform::DisplayServerName(s.display_server) +
              (s.xwayland_available ? " (+XWayland)" : ""));
        Print("desktop        : " + (s.desktop.empty() ? std::string("(unknown)") : s.desktop));
        Print(std::string("sound server   : ") + hr::platform::AudioServerName(s.audio_server));
        const std::string ff = hr::platform::FindFfmpeg();
        Print("ffmpeg         : " + (ff.empty() ? std::string("NOT FOUND (install ffmpeg or set HOMREC_FFMPEG)") : ff),
              ff.empty() ? Kind::Warn : Kind::Plain);
    }

    void Paths() {
        Print(std::string("portable mode : ") + (hr::platform::IsPortableMode() ? "yes" : "no"));
        Print("config dir    : " + hr::platform::ConfigDir());
        Print("data dir      : " + hr::platform::DataDir());
        Print("log dir       : " + hr::platform::LogDir());
        Print("recordings    : " + hr::platform::VideosDir());
        Print("settings file : " + HrNarrow(HrcConfig::ResolveSettingsPath(state_)));
    }

    void ShowLog(const std::string &arg) {
        size_t n = 20;
        if (!arg.empty()) n = (size_t)std::max(1, std::atoi(arg.c_str()));
        std::ifstream in(HrNarrow(HrLogPaths::LogFilePath(L"homrec.log")), std::ios::binary);
        if (!in) { Print("No homrec.log yet.", Kind::Warn); return; }
        std::deque<std::string> tail;
        std::string l;
        while (std::getline(in, l)) {
            tail.push_back(l);
            if (tail.size() > n) tail.pop_front();
        }
        for (const auto &t : tail) Print(t);
    }

    void Ls(const std::string &line) {
        const HrCmdLs::Args a = HrCmdLs::Parse(HrWide(line));
        const std::vector<HrCmdLs::Line> lines = HrCmdLs::Run(a, HrWide(hr::platform::ConfigDir()));
        for (const auto &l : lines) {
            Kind k = Kind::Plain;
            switch (l.kind) {
                case HrCmdLs::Kind::Header: k = Kind::Info; break;
                case HrCmdLs::Kind::Warn:   k = Kind::Warn; break;
                case HrCmdLs::Kind::Err:    k = Kind::Err; break;
                default: break;
            }
            Print(wxString(l.text.c_str()), k);
        }
    }

    void Vars(const std::string &filter) {
        const std::string f = Lower(filter);
        int n = 0;
        for (const auto &d : HrSettingsRegistry::All()) {
            if (!f.empty() && Lower(d.key).find(f) == std::string::npos) continue;
            Print(d.key + (d.sensitive ? "   (sensitive)" : ""));
            ++n;
        }
        Print(std::to_string(n) + " setting(s)", Kind::Info);
    }

    void SetVar(const std::string &key, const std::string &value) {
        const auto *def = HrSettingsRegistry::Find(key);
        if (!def) { Print("No setting called '" + key + "'.", Kind::Err); return; }
        if (def->sensitive) {
            Print("'" + def->key + "' runs programs, so it can't be changed from the console yet.", Kind::Warn);
            return;
        }
        if (!def->set(state_, value)) { Print("'" + value + "' is not a valid value for " + def->key + ".", Kind::Err); return; }
        Print(def->key + " = " + def->get(state_) + "   (in memory - 'save' writes it to the file)");
    }

    void Save() {
        const std::wstring target = HrcConfig::ResolveSettingsPath(state_);
        if (HrcConfig::Save(state_, target)) Print("Saved " + HrNarrow(target), Kind::Info);
        else Print("Couldn't write " + HrNarrow(target), Kind::Err);
    }

    AppState &state_;
    ThemeColors theme_;
    wxTextCtrl *out_ = nullptr, *in_ = nullptr;
    std::vector<std::string> history_;
    size_t hist_pos_ = 0;
    ConsoleFrame **slot_ = nullptr;
};

ConsoleFrame *g_console = nullptr;   // the one console window (null while it is closed)

}  // namespace

void ShowConsoleWindow(wxWindow *parent, AppState &state, const ThemeColors &theme) {
    if (!g_console) {
        g_console = new ConsoleFrame(parent, state, theme);
        g_console->SetSelfPointer(&g_console);
    }
    g_console->Show(true);
    g_console->Raise();
}
