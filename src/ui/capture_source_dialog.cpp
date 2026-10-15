#include "capture_source_dialog.h"
#include "window_picker_dialog.h"
#include "themed_widgets.h"
#include "../core/wgc_capture.h"
#include "../utils/log.h"
#include "../utils/wgc_log.h"
#include "../utils/str_convert.h"
#include <wx/msw/private.h>   // wxGetInstance()
#include <wx/spinctrl.h>
#include <wx/scrolwin.h>
#include <wx/statline.h>
#include <algorithm>
#include <cctype>

namespace {

// Results of one opening of the dialog besides wxID_OK / wxID_CANCEL.
constexpr int kResultPickWindow = wxID_HIGHEST + 301;
constexpr int kResultPickRegion = wxID_HIGHEST + 302;

// The capture fields this dialog edits - a plain copy, written back to AppState only on OK.
struct Src {
    CaptureMode mode = CaptureMode::Desktop;
    int monitor_id = 1;
    std::string title, exe, cls;
    HWND hwnd = nullptr;
    bool match_exe = true, match_class = true, match_title = false;
    std::string title_mode = "exact";
    bool follow = true;
    std::string method = "screen";     // "screen" | "wgc"
    std::string area = "window";       // "window" | "client"
    bool smart = true;
    int fps_cap = 0;
    int rx = 0, ry = 0, rw = 0, rh = 0;

    static Src From(const AppState &s) {
        Src o;
        o.mode = s.capture_mode;
        o.monitor_id = s.monitor_id;
        o.title = s.capture_window_title;
        o.exe = s.capture_window_exe;
        o.cls = s.capture_window_class;
        o.hwnd = s.capture_window_hwnd;
        o.match_exe = s.window_match_exe;
        o.match_class = s.window_match_class;
        o.match_title = s.window_match_title;
        o.title_mode = s.window_title_mode;
        o.follow = s.window_follow_hwnd;
        // legacy values ("crop" / "auto") mean the screen crop
        o.method = s.WindowCaptureWantsWgc() ? "wgc" : "screen";
        o.area = s.window_capture_area == "client" ? "client" : "window";
        o.smart = s.window_capture_smart;
        o.fps_cap = s.window_capture_fps_cap;
        o.rx = s.region_x; o.ry = s.region_y; o.rw = s.region_w; o.rh = s.region_h;
        return o;
    }

    void ApplyTo(AppState &s) const {
        s.capture_mode = mode;
        s.monitor_id = monitor_id;
        s.capture_window_title = title;
        s.capture_window_exe = exe;
        s.capture_window_class = cls;
        s.capture_window_hwnd = hwnd;
        s.window_match_exe = match_exe;
        s.window_match_class = match_class;
        s.window_match_title = match_title;
        s.window_title_mode = title_mode;
        s.window_follow_hwnd = follow;
        s.window_capture_method = method;
        s.window_capture_area = area;
        s.window_capture_smart = smart;
        s.window_capture_fps_cap = fps_cap;
        s.region_x = rx; s.region_y = ry; s.region_w = rw; s.region_h = rh;
    }

    bool SameAs(const Src &o) const {
        return mode == o.mode && monitor_id == o.monitor_id && title == o.title && exe == o.exe && cls == o.cls &&
               hwnd == o.hwnd && match_exe == o.match_exe && match_class == o.match_class &&
               match_title == o.match_title && title_mode == o.title_mode && follow == o.follow &&
               method == o.method && area == o.area && smart == o.smart && fps_cap == o.fps_cap &&
               rx == o.rx && ry == o.ry && rw == o.rw && rh == o.rh;
    }
};

std::string LowerStr(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string Trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

class SourceDialog : public wxDialog {
public:
    SourceDialog(wxWindow *parent, const Src &src, const ThemeColors &theme, const LanguageTable &lang, int live_mode)
        : wxDialog(parent, wxID_ANY, Tr("src_dialog_title", lang), wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          lang_(lang), src_(src) {
        bg_ = FromColorref(theme.bg);
        text_ = FromColorref(theme.text);
        dim_ = FromColorref(theme.text_secondary);
        accent_ = FromColorref(theme.accent);
        SetBackgroundColour(bg_);

        auto *root = new wxBoxSizer(wxVERTICAL);
        scroll_ = new wxScrolledWindow(this);
        scroll_->SetBackgroundColour(bg_);
        scroll_->SetScrollRate(0, 12);
        auto *col = new wxBoxSizer(wxVERTICAL);

        // ---- what -----------------------------------------------------------------------------
        Heading(col, T("src_what"));
        rb_desktop_ = Radio(col, T("src_desktop"), true);
        rb_window_ = Radio(col, T("src_window"), false);
        rb_region_ = Radio(col, T("src_region"), false);

        // ---- desktop: monitor -----------------------------------------------------------------
        Heading(col, T("src_desktop_head"));
        {
            auto *row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(Label(T("src_monitor")), 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
            monitor_choice_ = new wxChoice(scroll_, wxID_ANY);
            const auto mons = HrListMonitors();
            if (mons.empty()) {
                for (int i = 0; i < 4; ++i) monitor_choice_->Append(wxString::Format("%s %d", T("src_monitor_word"), i + 1));
            } else {
                for (size_t i = 0; i < mons.size(); ++i) {
                    wxString l = wxString::Format("%s %d — %dx%d", T("src_monitor_word"), (int)i + 1, mons[i].w, mons[i].h);
                    if (mons[i].primary) l += " " + T("src_primary");
                    monitor_choice_->Append(l);
                }
            }
            int sel = src_.monitor_id > 0 ? src_.monitor_id - 1 : 0;
            if (sel >= (int)monitor_choice_->GetCount()) sel = 0;
            monitor_choice_->SetSelection(sel);
            row->Add(monitor_choice_, 1, wxALIGN_CENTRE_VERTICAL);
            col->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, 16);
        }

        // ---- window ---------------------------------------------------------------------------
        Heading(col, T("src_window_head"));
        {
            auto *row = new wxBoxSizer(wxHORIZONTAL);
            window_lbl_ = new wxStaticText(scroll_, wxID_ANY, wxEmptyString);
            window_lbl_->SetForegroundColour(text_);
            window_lbl_->SetBackgroundColour(bg_);
            row->Add(window_lbl_, 1, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
            pick_window_btn_ = new wxButton(scroll_, wxID_ANY, T("src_pick_window"));
            row->Add(pick_window_btn_, 0);
            col->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, 16);
        }
        col->AddSpacer(6);
        col->Add(Dim(T("src_find_by")), 0, wxLEFT | wxRIGHT, 16);
        col->AddSpacer(4);
        {
            auto *g = new wxFlexGridSizer(3, 6, 8);
            g->AddGrowableCol(2, 1);

            chk_exe_ = Check(T("src_match_exe"), src_.match_exe);
            g->Add(chk_exe_, 0, wxALIGN_CENTRE_VERTICAL);
            g->AddSpacer(1);
            edit_exe_ = new wxTextCtrl(scroll_, wxID_ANY, wxString::FromUTF8(src_.exe.c_str()));
            g->Add(edit_exe_, 1, wxEXPAND);

            chk_class_ = Check(T("src_match_class"), src_.match_class);
            g->Add(chk_class_, 0, wxALIGN_CENTRE_VERTICAL);
            g->AddSpacer(1);
            edit_class_ = new wxTextCtrl(scroll_, wxID_ANY, wxString::FromUTF8(src_.cls.c_str()));
            g->Add(edit_class_, 1, wxEXPAND);

            chk_title_ = Check(T("src_match_title"), src_.match_title);
            g->Add(chk_title_, 0, wxALIGN_CENTRE_VERTICAL);
            title_mode_choice_ = new wxChoice(scroll_, wxID_ANY);
            title_mode_choice_->Append(T("src_title_exact"));
            title_mode_choice_->Append(T("src_title_prefix"));
            title_mode_choice_->Append(T("src_title_contains"));
            title_mode_choice_->SetSelection(HrTitleModeFromStr(src_.title_mode));
            g->Add(title_mode_choice_, 0, wxALIGN_CENTRE_VERTICAL);
            edit_title_ = new wxTextCtrl(scroll_, wxID_ANY, wxString::FromUTF8(src_.title.c_str()));
            g->Add(edit_title_, 1, wxEXPAND);

            col->Add(g, 0, wxEXPAND | wxLEFT | wxRIGHT, 16);
        }
        col->AddSpacer(4);
        chk_follow_ = Check(T("src_follow_hwnd"), src_.follow);
        col->Add(chk_follow_, 0, wxLEFT | wxRIGHT, 16);
        col->AddSpacer(2);
        col->Add(Dim(T("src_find_hint")), 0, wxLEFT | wxRIGHT, 16);
        col->AddSpacer(6);
        {
            auto *row = new wxBoxSizer(wxHORIZONTAL);
            test_btn_ = new wxButton(scroll_, wxID_ANY, T("src_test_match"));
            row->Add(test_btn_, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 10);
            test_lbl_ = new wxStaticText(scroll_, wxID_ANY, wxEmptyString);
            test_lbl_->SetForegroundColour(dim_);
            test_lbl_->SetBackgroundColour(bg_);
            row->Add(test_lbl_, 1, wxALIGN_CENTRE_VERTICAL);
            col->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, 16);
        }

        // ---- how a window is recorded ---------------------------------------------------------
        Heading(col, T("src_how"));
        method_choice_ = new wxChoice(scroll_, wxID_ANY);
        method_choice_->Append(T("src_method_window"));
        method_choice_->Append(T("src_method_screen"));
        method_choice_->SetSelection(src_.method == "wgc" ? 0 : 1);
        col->Add(method_choice_, 0, wxEXPAND | wxLEFT | wxRIGHT, 16);
        col->AddSpacer(6);
        area_choice_ = new wxChoice(scroll_, wxID_ANY);
        area_choice_->Append(T("src_area_window"));
        area_choice_->Append(T("src_area_client"));
        area_choice_->SetSelection(src_.area == "client" ? 1 : 0);
        col->Add(area_choice_, 0, wxEXPAND | wxLEFT | wxRIGHT, 16);
        col->AddSpacer(6);
        chk_smart_ = Check(T("src_smart"), src_.smart);
        col->Add(chk_smart_, 0, wxLEFT | wxRIGHT, 16);
        col->Add(Dim(T("src_smart_hint")), 0, wxLEFT | wxRIGHT, 36);
        col->AddSpacer(6);
        {
            auto *row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(Label(T("src_fps_cap")), 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
            fps_cap_spin_ = new wxSpinCtrl(scroll_, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                           wxSP_ARROW_KEYS, 0, 240, std::max(0, std::min(240, src_.fps_cap)));
            row->Add(fps_cap_spin_, 0, wxALIGN_CENTRE_VERTICAL);
            col->Add(row, 0, wxLEFT | wxRIGHT, 16);
        }
        col->AddSpacer(6);
        live_lbl_ = new wxStaticText(scroll_, wxID_ANY, LiveText(live_mode));
        live_lbl_->SetForegroundColour(accent_);
        live_lbl_->SetBackgroundColour(bg_);
        col->Add(live_lbl_, 0, wxLEFT | wxRIGHT, 16);
        col->AddSpacer(6);
        retry_btn_ = new wxButton(scroll_, wxID_ANY, T("src_retry_native"));
        col->Add(retry_btn_, 0, wxLEFT | wxRIGHT, 16);

        // ---- region ---------------------------------------------------------------------------
        Heading(col, T("src_region_head"));
        {
            auto *row = new wxBoxSizer(wxHORIZONTAL);
            region_lbl_ = new wxStaticText(scroll_, wxID_ANY, wxEmptyString);
            region_lbl_->SetForegroundColour(text_);
            region_lbl_->SetBackgroundColour(bg_);
            row->Add(region_lbl_, 1, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
            pick_region_btn_ = new wxButton(scroll_, wxID_ANY, T("src_pick_region"));
            row->Add(pick_region_btn_, 0);
            col->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, 16);
        }
        col->AddSpacer(12);

        scroll_->SetSizer(col);
        root->Add(scroll_, 1, wxEXPAND | wxALL, 4);
        root->Add(new wxStaticLine(this), 0, wxEXPAND);
        auto *btnRow = new wxBoxSizer(wxHORIZONTAL);
        btnRow->AddStretchSpacer(1);
        btnRow->Add(new wxButton(this, wxID_CANCEL, T("cancel")), 0, wxALL, 8);
        auto *ok = new wxButton(this, wxID_OK, T("src_ok"));
        ok->SetDefault();
        btnRow->Add(ok, 0, wxALL, 8);
        root->Add(btnRow, 0, wxEXPAND);
        SetSizer(root);

        // initial state
        rb_desktop_->SetValue(src_.mode == CaptureMode::Desktop);
        rb_window_->SetValue(src_.mode == CaptureMode::Window);
        rb_region_->SetValue(src_.mode == CaptureMode::Region);
        RefreshLabels();
        UpdateEnabled();

        for (wxRadioButton *rb : {rb_desktop_, rb_window_, rb_region_})
            rb->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent &) { UpdateEnabled(); });
        for (wxCheckBox *c : {chk_exe_, chk_class_, chk_title_})
            c->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { UpdateEnabled(); });
        method_choice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { UpdateEnabled(); });
        pick_window_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            Collect();
            EndModal(kResultPickWindow);
        });
        pick_region_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            Collect();
            EndModal(kResultPickRegion);
        });
        test_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { OnTest(); });
        retry_btn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { OnRetryNative(); });
        Bind(wxEVT_BUTTON, &SourceDialog::OnOk, this, wxID_OK);

        // size: content height, but never taller than the screen
        scroll_->FitInside();
        wxSize best = GetBestSize();
        const wxRect disp = wxGetClientDisplayRect();
        best.x = std::max(best.x, 700);
        best.y = std::min(std::max(best.y, 520), std::max(420, disp.height - 80));
        SetClientSize(best);
        CentreOnParent();
    }

    // The values the controls hold right now (also what a picker run starts from).
    const Src &Collected() const { return src_; }

    void Collect() {
        src_.mode = rb_window_->GetValue() ? CaptureMode::Window
                  : rb_region_->GetValue() ? CaptureMode::Region : CaptureMode::Desktop;
        const int msel = monitor_choice_->GetSelection();
        src_.monitor_id = msel >= 0 ? msel + 1 : 1;
        src_.exe = LowerStr(Trim(edit_exe_->GetValue().ToUTF8().data()));
        src_.cls = Trim(edit_class_->GetValue().ToUTF8().data());
        src_.title = edit_title_->GetValue().ToUTF8().data();
        src_.match_exe = chk_exe_->GetValue();
        src_.match_class = chk_class_->GetValue();
        src_.match_title = chk_title_->GetValue();
        src_.title_mode = HrTitleModeToStr(title_mode_choice_->GetSelection());
        src_.follow = chk_follow_->GetValue();
        src_.method = method_choice_->GetSelection() == 0 ? "wgc" : "screen";
        src_.area = area_choice_->GetSelection() == 1 ? "client" : "window";
        src_.smart = chk_smart_->GetValue();
        src_.fps_cap = fps_cap_spin_->GetValue();
    }

private:
    wxString T(const char *key) const { return Tr(key, lang_); }
    static wxString Tr(const char *key, const LanguageTable &lang) { return wxString::FromUTF8(lang.Get(key).c_str()); }

    void Heading(wxSizer *s, const wxString &t) {
        auto *l = new wxStaticText(scroll_, wxID_ANY, t);
        wxFont f = l->GetFont();
        f.SetWeight(wxFONTWEIGHT_BOLD);
        l->SetFont(f);
        l->SetForegroundColour(accent_);
        l->SetBackgroundColour(bg_);
        s->Add(l, 0, wxLEFT | wxRIGHT | wxTOP, 16);
        s->AddSpacer(4);
    }
    wxStaticText *Label(const wxString &t) {
        auto *l = new wxStaticText(scroll_, wxID_ANY, t);
        l->SetForegroundColour(text_);
        l->SetBackgroundColour(bg_);
        return l;
    }
    wxStaticText *Dim(const wxString &t) {
        auto *l = new wxStaticText(scroll_, wxID_ANY, t);
        l->SetForegroundColour(dim_);
        l->SetBackgroundColour(bg_);
        l->Wrap(640);
        return l;
    }
    wxCheckBox *Check(const wxString &t, bool v) {
        auto *c = new wxCheckBox(scroll_, wxID_ANY, t);
        c->SetValue(v);
        c->SetForegroundColour(text_);
        c->SetBackgroundColour(bg_);
        return c;
    }
    wxRadioButton *Radio(wxSizer *s, const wxString &t, bool first) {
        auto *r = new wxRadioButton(scroll_, wxID_ANY, t, wxDefaultPosition, wxDefaultSize, first ? wxRB_GROUP : 0);
        r->SetForegroundColour(text_);
        r->SetBackgroundColour(bg_);
        s->Add(r, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);
        return r;
    }

    wxString LiveText(int live_mode) const {
        const char *k = live_mode == 1 ? "src_live_native" : live_mode == 2 ? "src_live_exact"
                      : live_mode == 3 ? "src_live_smart" : nullptr;
        if (!k) return wxString();
        return T("src_live_now") + " " + T(k);
    }

    void RefreshLabels() {
        // picked window
        wxString w;
        if (src_.title.empty() && src_.exe.empty() && src_.cls.empty()) {
            w = T("src_window_none");
        } else {
            w = wxString::FromUTF8(src_.exe.empty() ? "" : (src_.exe + "  —  ").c_str());
            w += wxString::FromUTF8(src_.title.empty() ? src_.cls.c_str() : src_.title.c_str());
        }
        window_lbl_->SetLabel(w);
        window_lbl_->Wrap(520);
        edit_exe_->ChangeValue(wxString::FromUTF8(src_.exe.c_str()));
        edit_class_->ChangeValue(wxString::FromUTF8(src_.cls.c_str()));
        edit_title_->ChangeValue(wxString::FromUTF8(src_.title.c_str()));
        // region
        if (src_.rw > 0 && src_.rh > 0)
            region_lbl_->SetLabel(wxString::Format("%d, %d   %d × %d", src_.rx, src_.ry, src_.rw, src_.rh));
        else
            region_lbl_->SetLabel(T("src_region_none"));
    }

    void UpdateEnabled() {
        const bool desk = rb_desktop_->GetValue(), win = rb_window_->GetValue(), reg = rb_region_->GetValue();
        monitor_choice_->Enable(desk);
        for (wxWindow *w : std::initializer_list<wxWindow *>{ pick_window_btn_, chk_exe_, chk_class_, chk_title_,
                                                                chk_follow_, test_btn_, method_choice_, area_choice_,
                                                                retry_btn_ })
            w->Enable(win);
        edit_exe_->Enable(win && chk_exe_->GetValue());
        edit_class_->Enable(win && chk_class_->GetValue());
        edit_title_->Enable(win && chk_title_->GetValue());
        title_mode_choice_->Enable(win && chk_title_->GetValue());
        const bool wgc = method_choice_->GetSelection() == 0;
        chk_smart_->Enable(win && wgc);
        fps_cap_spin_->Enable(win && wgc);
        pick_region_btn_->Enable(reg);
    }

    void OnTest() {
        Collect();
        HrWindowTarget t;
        t.title = src_.title;
        t.exe = src_.exe;
        t.cls = src_.cls;
        t.match_title = src_.match_title;
        t.match_exe = src_.match_exe;
        t.match_class = src_.match_class;
        t.title_mode = HrTitleModeFromStr(src_.title_mode);
        t.follow_hwnd = src_.follow;
        t.preferred = nullptr;     // "as after a restart": find it by the rules alone
        t.client_only = src_.area == "client";
        HWND hwnd = nullptr;
        RECT r{};
        std::string how;
        if (HR_ResolveWindowTarget(t, hwnd, r, &how) && hwnd) {
            std::string d = "'" + HR_WindowTitleUtf8(hwnd) + "'  (" + HR_WindowExeName(hwnd) + " / " +
                            HR_WindowClassName(hwnd) + ")";
            test_lbl_->SetForegroundColour(FromColorref(RGB(0x40, 0xb0, 0x60)));
            test_lbl_->SetLabel(T("src_test_found") + " " + wxString::FromUTF8(d.c_str()));
            HrWgcLog::Info("Capture source dialog: test match -> found " + d + " by: " + how);
        } else {
            test_lbl_->SetForegroundColour(FromColorref(RGB(0xe0, 0x70, 0x60)));
            test_lbl_->SetLabel(T("src_test_notfound"));
            HrWgcLog::Info("Capture source dialog: test match -> not found");
        }
        test_lbl_->Wrap(520);
        scroll_->Layout();
    }

    void OnRetryNative() {
        wxMessageDialog md(this, T("src_retry_native_msg"), T("src_dialog_title"), wxYES_NO | wxICON_WARNING);
        if (md.ShowModal() != wxID_YES) return;
        HrWgcResetCrashGuard();
        HrWgcLog::Note("Capture source dialog: crash protection reset by the user - native Windows capture will be tried again.");
    }

    void OnOk(wxCommandEvent &evt) {
        Collect();
        if (src_.mode == CaptureMode::Window && src_.title.empty() && src_.exe.empty() && src_.cls.empty()) {
            wxMessageBox(T("src_need_window"), T("src_dialog_title"), wxOK | wxICON_INFORMATION, this);
            return;   // stay open
        }
        if (src_.mode == CaptureMode::Window && !src_.match_exe && !src_.match_class && !src_.match_title &&
            !src_.follow && !src_.hwnd) {
            wxMessageBox(T("src_need_criteria"), T("src_dialog_title"), wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (src_.mode == CaptureMode::Region && (src_.rw <= 0 || src_.rh <= 0)) {
            wxMessageBox(T("src_need_region"), T("src_dialog_title"), wxOK | wxICON_INFORMATION, this);
            return;
        }
        evt.Skip();   // -> EndModal(wxID_OK)
    }

    const LanguageTable &lang_;
    Src src_;
    wxColour bg_, text_, dim_, accent_;
    wxScrolledWindow *scroll_ = nullptr;
    wxRadioButton *rb_desktop_ = nullptr, *rb_window_ = nullptr, *rb_region_ = nullptr;
    wxChoice *monitor_choice_ = nullptr, *title_mode_choice_ = nullptr, *method_choice_ = nullptr, *area_choice_ = nullptr;
    wxStaticText *window_lbl_ = nullptr, *test_lbl_ = nullptr, *live_lbl_ = nullptr, *region_lbl_ = nullptr;
    wxButton *pick_window_btn_ = nullptr, *pick_region_btn_ = nullptr, *test_btn_ = nullptr, *retry_btn_ = nullptr;
    wxCheckBox *chk_exe_ = nullptr, *chk_class_ = nullptr, *chk_title_ = nullptr, *chk_follow_ = nullptr, *chk_smart_ = nullptr;
    wxTextCtrl *edit_exe_ = nullptr, *edit_class_ = nullptr, *edit_title_ = nullptr;
    wxSpinCtrl *fps_cap_spin_ = nullptr;
};

} // namespace

bool ShowCaptureSourceDialog(wxWindow *parent, AppState &state, const ThemeColors &theme,
                             const LanguageTable &lang, int live_mode) {
    const Src before = Src::From(state);
    Src work = before;
    const HWND parent_hwnd = parent ? (HWND)parent->GetHandle() : nullptr;

    for (int guard = 0; guard < 64; ++guard) {
        int rc;
        {
            SourceDialog dlg(parent, work, theme, lang, live_mode);
            rc = dlg.ShowModal();
            if (rc == wxID_OK || rc == kResultPickWindow || rc == kResultPickRegion) work = dlg.Collected();
        }
        if (rc == wxID_OK) break;
        if (rc == kResultPickWindow) {
            // The pickers write straight into an AppState, so give them a scratch copy that carries the
            // current working values and read the result back.
            AppState scratch = state;
            work.ApplyTo(scratch);
            HrLog::Info("UI: window picker opened (capture source dialog)");
            ShowWindowPickerDialog(parent_hwnd, wxGetInstance(), scratch);
            HrLog::Info("UI: window picker closed -> capture_mode=" + HrCaptureModeToStr(scratch.capture_mode) +
                        ", window='" + scratch.capture_window_title + "'");
            work = Src::From(scratch);
            continue;
        }
        if (rc == kResultPickRegion) {
            AppState scratch = state;
            work.ApplyTo(scratch);
            ShowRegionPickerOverlay(parent_hwnd, wxGetInstance(), scratch);
            work = Src::From(scratch);
            continue;
        }
        return false;   // Cancel / [X]
    }

    if (work.SameAs(before)) return false;
    work.ApplyTo(state);
    // The match flags / method only make sense together with what the person ended up with - log it all in
    // one line (wgc.log is where "why was this window recorded that way" is looked up first).
    const std::string mode = HrCaptureModeToStr(state.capture_mode);
    HrWgcLog::Note("Capture source changed: mode=" + mode + ", monitor=" + std::to_string(state.monitor_id) +
                   ", window='" + state.capture_window_title + "' (" + state.capture_window_exe + " / " +
                   state.capture_window_class + "), match exe=" + (state.window_match_exe ? "1" : "0") + " class=" +
                   (state.window_match_class ? "1" : "0") + " title=" + (state.window_match_title ? "1" : "0") + "/" +
                   state.window_title_mode + ", follow=" + (state.window_follow_hwnd ? "1" : "0") + ", method=" +
                   state.window_capture_method + ", area=" + state.window_capture_area + ", smart=" +
                   (state.window_capture_smart ? "1" : "0") + ", fps cap=" + std::to_string(state.window_capture_fps_cap) +
                   ", region=" + std::to_string(state.region_x) + "," + std::to_string(state.region_y) + " " +
                   std::to_string(state.region_w) + "x" + std::to_string(state.region_h));
    return true;
}
