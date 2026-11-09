// scenes_panel.cpp - see scenes_panel.h.
#include "scenes_panel.h"
#include <wx/dcbuffer.h>
#include <algorithm>

namespace {
enum { kBtnNew = 6101, kBtnDel, kBtnUp, kBtnDown };
enum { kMenuRename = 6201, kMenuDup, kMenuUp, kMenuDown, kMenuDel };
constexpr size_t kMaxScenes = 64;   // same limit as hrc_config.cpp
}

ScenesPanel::ScenesPanel(wxWindow *parent, AppState &state)
    : wxPanel(parent, wxID_ANY), state_(state) {
    list_ = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                 wxVSCROLL | wxWANTS_CHARS | wxBORDER_NONE);
    list_->SetBackgroundStyle(wxBG_STYLE_PAINT);
    list_->SetMinSize(wxSize(120, 60));
    list_->Bind(wxEVT_PAINT, &ScenesPanel::OnPaint, this);
    list_->Bind(wxEVT_SIZE, [this](wxSizeEvent &e) { list_->Refresh(false); e.Skip(); });
    list_->Bind(wxEVT_LEFT_DOWN, &ScenesPanel::OnMouse, this);
    list_->Bind(wxEVT_LEFT_DCLICK, &ScenesPanel::OnMouse, this);
    list_->Bind(wxEVT_RIGHT_DOWN, &ScenesPanel::OnMouse, this);
    list_->Bind(wxEVT_MOTION, &ScenesPanel::OnMouse, this);
    list_->Bind(wxEVT_LEAVE_WINDOW, &ScenesPanel::OnMouse, this);
    list_->Bind(wxEVT_KEY_DOWN, &ScenesPanel::OnKey, this);

    add_btn_  = new ColorButton(this, kBtnNew,  wxString());
    del_btn_  = new ColorButton(this, kBtnDel,  wxString());
    up_btn_   = new ColorButton(this, kBtnUp,   wxString());
    down_btn_ = new ColorButton(this, kBtnDown, wxString());
    add_btn_->SetGlyph(ColorButton::Glyph::Plus);
    del_btn_->SetGlyph(ColorButton::Glyph::Minus);
    up_btn_->SetGlyph(ColorButton::Glyph::Up);
    down_btn_->SetGlyph(ColorButton::Glyph::Down);
    for (ColorButton *b : { add_btn_, del_btn_, up_btn_, down_btn_ }) {
        b->SetMinSize(wxSize(FromDIP(40), FromDIP(32)));
        b->SetCornerRadius(2.0);   // nearly square corners
    }

    auto *bar = new wxBoxSizer(wxHORIZONTAL);
    bar->Add(add_btn_, 0, wxALL, 3);
    bar->Add(del_btn_, 0, wxTOP | wxBOTTOM | wxRIGHT, 3);
    bar->AddStretchSpacer(1);
    bar->Add(up_btn_, 0, wxTOP | wxBOTTOM, 3);
    bar->Add(down_btn_, 0, wxALL, 3);
    auto *root = new wxBoxSizer(wxVERTICAL);
    root->Add(list_, 1, wxEXPAND);
    root->Add(bar, 0, wxEXPAND);
    SetSizer(root);

    Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { if (on_new) on_new(); }, kBtnNew);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { if (on_delete && sel_ >= 0) on_delete(IdAt(sel_)); }, kBtnDel);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { if (on_move && sel_ >= 0) on_move(IdAt(sel_), -1); }, kBtnUp);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { if (on_move && sel_ >= 0) on_move(IdAt(sel_), +1); }, kBtnDown);

    Reload();
}

int ScenesPanel::RowH() const { return list_->FromDIP(28); }

std::string ScenesPanel::T(const char *key, const char *fallback) const {
    if (tr) {
        std::string s = tr(key);
        if (!s.empty()) return s;
    }
    return fallback;
}

std::string ScenesPanel::IdAt(int row) const {
    return (row >= 0 && (size_t)row < state_.scenes.size()) ? state_.scenes[(size_t)row].id : std::string();
}

int ScenesPanel::RowAt(const wxPoint &pt) const {
    const wxPoint up = list_->CalcUnscrolledPosition(pt);
    if (up.y < 0) return -1;
    const int r = up.y / RowH();
    return (size_t)r < state_.scenes.size() ? r : -1;
}

void ScenesPanel::ApplyTheme(const ThemeColors &theme, bool dark) {
    theme_ = theme;
    dark_ = dark;
    SetBackgroundColour(FromColorref(theme.bg));
    for (ColorButton *b : { add_btn_, del_btn_, up_btn_, down_btn_ })
        b->SetColours(FromColorref(theme.surface_light), FromColorref(theme.text));
    add_btn_->SetColours(FromColorref(theme.accent), FromColorref(theme.bg));
    Refresh();
    list_->Refresh();
}

void ScenesPanel::Reload() {
    const int n = (int)state_.scenes.size();
    int active = -1;
    for (int i = 0; i < n; ++i) if (state_.scenes[(size_t)i].id == state_.active_scene_id) active = i;
    if (last_active_ != state_.active_scene_id || sel_ < 0) sel_ = active;
    if (sel_ >= n) sel_ = n - 1;
    last_active_ = state_.active_scene_id;
    if (hot_ >= n) hot_ = -1;

    int vx = 0, vy = 0;
    list_->GetViewStart(&vx, &vy);
    list_->SetScrollbars(0, RowH(), 0, n, 0, std::max(0, std::min(vy, n - 1)));

    add_btn_->SetToolTip(wxString::FromUTF8(T("scene_new", "New scene").c_str()));
    del_btn_->SetToolTip(wxString::FromUTF8(T("scene_delete", "Delete scene").c_str()));
    up_btn_->SetToolTip(wxString::FromUTF8(T("scene_up", "Move up").c_str()));
    down_btn_->SetToolTip(wxString::FromUTF8(T("scene_down", "Move down").c_str()));
    UpdateButtons();
    list_->Refresh(false);
}

void ScenesPanel::UpdateButtons() {
    const int n = (int)state_.scenes.size();
    const bool has = sel_ >= 0 && sel_ < n;
    add_btn_->Enable2((size_t)n < kMaxScenes);
    del_btn_->Enable2(has && n > 1 && state_.scenes[(size_t)sel_].id != state_.active_scene_id);
    up_btn_->Enable2(has && sel_ > 0);
    down_btn_->Enable2(has && sel_ + 1 < n);
}

void ScenesPanel::KeepVisible(int row) {
    int vx = 0, vy = 0;
    list_->GetViewStart(&vx, &vy);
    const int rows_shown = std::max(1, list_->GetClientSize().y / RowH());
    if (row < vy) list_->Scroll(0, row);
    else if (row >= vy + rows_shown) list_->Scroll(0, row - rows_shown + 1);
}

void ScenesPanel::OnPaint(wxPaintEvent &) {
    wxAutoBufferedPaintDC dc(list_);
    list_->DoPrepareDC(dc);

    const wxColour bg    = dark_ ? wxColour(0x31, 0x32, 0x44) : wxColour(0xff, 0xff, 0xff);
    const wxColour bgSel = dark_ ? wxColour(0x45, 0x47, 0x5a) : wxColour(0xcc, 0xdd, 0xff);
    const wxColour bgHot = dark_ ? wxColour(0x3b, 0x3d, 0x52) : wxColour(0xe6, 0xee, 0xff);
    const wxColour fg    = dark_ ? wxColour(0xcd, 0xd6, 0xf4) : wxColour(0x20, 0x20, 0x30);
    const wxColour line  = dark_ ? wxColour(0x45, 0x47, 0x5a) : wxColour(0xc4, 0xcc, 0xe0);
    const wxColour accent = FromColorref(theme_.accent);
    const wxColour live   = FromColorref(theme_.success);

    dc.SetBackground(wxBrush(bg));
    dc.Clear();

    wxFont normal(wxFontInfo(9).FaceName("Segoe UI"));
    wxFont bold = normal;
    bold.SetWeight(wxFONTWEIGHT_BOLD);

    const int w = list_->GetClientSize().x, rowH = RowH();
    for (size_t i = 0; i < state_.scenes.size(); ++i) {
        const SceneInfo &sc = state_.scenes[i];
        const int y = (int)i * rowH;
        const bool active = sc.id == state_.active_scene_id;

        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush((int)i == sel_ ? bgSel : ((int)i == hot_ ? bgHot : bg)));
        dc.DrawRectangle(0, y, w, rowH);
        if (active) {
            dc.SetBrush(wxBrush(accent));
            dc.DrawRectangle(0, y, 3, rowH);
        }
        dc.SetPen(wxPen(line));
        dc.DrawLine(0, y + rowH - 1, w, y + rowH - 1);

        dc.SetFont(active ? bold : normal);
        dc.SetTextForeground(active ? accent : fg);
        const int tx = 12, right = w - 10 - (active ? 14 : 0);
        const wxString name = wxControl::Ellipsize(wxString::FromUTF8(sc.name.c_str()), dc,
                                                   wxELLIPSIZE_END, std::max(10, right - tx));
        dc.DrawText(name, tx, y + (rowH - dc.GetTextExtent(name).y) / 2);

        if (active) {   // the scene that is being shown / recorded
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(live));
            dc.DrawCircle(w - 14, y + rowH / 2, 4);
        }
    }
}

void ScenesPanel::OnMouse(wxMouseEvent &evt) {
    const wxEventType t = evt.GetEventType();
    const int row = (t == wxEVT_LEAVE_WINDOW) ? -1 : RowAt(evt.GetPosition());
    if (t == wxEVT_MOTION || t == wxEVT_LEAVE_WINDOW) {
        if (row != hot_) { hot_ = row; list_->Refresh(false); }
        evt.Skip();
        return;
    }
    list_->SetFocus();
    if (row < 0) return;
    sel_ = row;
    UpdateButtons();
    list_->Refresh(false);
    if (t == wxEVT_LEFT_DOWN) {
        if (on_switch) on_switch(IdAt(row));
    } else if (t == wxEVT_LEFT_DCLICK) {
        if (on_rename) on_rename(IdAt(row));
    } else if (t == wxEVT_RIGHT_DOWN) {
        ShowMenu(row, evt.GetPosition());
    }
}

void ScenesPanel::OnKey(wxKeyEvent &evt) {
    const int n = (int)state_.scenes.size();
    switch (evt.GetKeyCode()) {
        case WXK_UP:
        case WXK_DOWN: {
            const int next = sel_ + (evt.GetKeyCode() == WXK_UP ? -1 : 1);
            if (next < 0 || next >= n) break;
            sel_ = next;
            KeepVisible(sel_);
            UpdateButtons();
            list_->Refresh(false);
            if (on_switch) on_switch(IdAt(sel_));
            break;
        }
        case WXK_RETURN:
        case WXK_NUMPAD_ENTER:
            if (sel_ >= 0 && on_switch) on_switch(IdAt(sel_));
            break;
        case WXK_F2:
            if (sel_ >= 0 && on_rename) on_rename(IdAt(sel_));
            break;
        case WXK_DELETE:
            if (sel_ >= 0 && on_delete) on_delete(IdAt(sel_));
            break;
        default:
            evt.Skip();
    }
}

void ScenesPanel::ShowMenu(int row, const wxPoint &pos) {
    const std::string id = IdAt(row);
    if (id.empty()) return;
    const int n = (int)state_.scenes.size();
    wxMenu menu;
    menu.Append(kMenuRename, wxString::FromUTF8(T("scene_rename", "Rename...").c_str()) + "\tF2");
    menu.Append(kMenuDup, wxString::FromUTF8(T("scene_dup", "Duplicate...").c_str()));
    menu.AppendSeparator();
    menu.Append(kMenuUp, wxString::FromUTF8(T("scene_up", "Move up").c_str()))->Enable(row > 0);
    menu.Append(kMenuDown, wxString::FromUTF8(T("scene_down", "Move down").c_str()))->Enable(row + 1 < n);
    menu.AppendSeparator();
    menu.Append(kMenuDel, wxString::FromUTF8(T("scene_delete", "Delete scene").c_str()) + "\tDel")
        ->Enable(n > 1 && id != state_.active_scene_id);
    menu.Bind(wxEVT_MENU, [this, id](wxCommandEvent &e) {
        switch (e.GetId()) {
            case kMenuRename: if (on_rename) on_rename(id); break;
            case kMenuDup:    if (on_duplicate) on_duplicate(id); break;
            case kMenuUp:     if (on_move) on_move(id, -1); break;
            case kMenuDown:   if (on_move) on_move(id, +1); break;
            case kMenuDel:    if (on_delete) on_delete(id); break;
        }
    });
    list_->PopupMenu(&menu, pos);
}
