#include "themed_widgets.h"
#include <wx/dcbuffer.h>
#include <algorithm>
#include <cmath>
#ifdef _WIN32
  #include <uxtheme.h>
#endif

wxColour FromColorref(COLORREF c) {
    return wxColour(GetRValue(c), GetGValue(c), GetBValue(c));
}

// ---------------------------------------------------------------------------
// ColorButton
// ---------------------------------------------------------------------------
ColorButton::ColorButton(wxWindow *parent, wxWindowID id, const wxString &label)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
      label_(label), bg_(*wxGREEN), fg_(*wxBLACK), disabled_bg_(200, 200, 200), cmd_id_(id) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCursor(wxCursor(wxCURSOR_HAND));
    Bind(wxEVT_PAINT, &ColorButton::OnPaint, this);
    Bind(wxEVT_LEFT_UP, &ColorButton::OnLeftUp, this);
    Bind(wxEVT_ENTER_WINDOW, &ColorButton::OnEnter, this);
    Bind(wxEVT_LEAVE_WINDOW, &ColorButton::OnLeave, this);
    // See ColorSlider's ctor / themed_widgets.h note on why this matters.
    Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) { Refresh(true); evt.Skip(); });
}

void ColorButton::SetColours(wxColour bg, wxColour fg) {
    bg_ = bg; fg_ = fg;
    Refresh();
}

void ColorButton::Enable2(bool enabled) {
    enabled_ = enabled;
    SetCursor(wxCursor(enabled ? wxCURSOR_HAND : wxCURSOR_ARROW));
    Refresh();
}

void ColorButton::OnEnter(wxMouseEvent &) { hover_ = true; Refresh(); }
void ColorButton::OnLeave(wxMouseEvent &) { hover_ = false; Refresh(); }

void ColorButton::OnLeftUp(wxMouseEvent &evt) {
    if (!enabled_) return;
    wxCommandEvent click(wxEVT_BUTTON, cmd_id_);
    click.SetEventObject(this);
    ProcessWindowEvent(click);
    evt.Skip();
}

void ColorButton::OnPaint(wxPaintEvent &) {
    wxAutoBufferedPaintDC dc(this);
    wxColour parentBg = GetParent() ? GetParent()->GetBackgroundColour() : *wxWHITE;
    dc.SetBackground(wxBrush(parentBg));
    dc.Clear();

    wxColour fill = enabled_ ? bg_ : disabled_bg_;
    if (enabled_ && hover_) {
        fill = wxColour(std::max(0, fill.Red() - 20), std::max(0, fill.Green() - 20), std::max(0, fill.Blue() - 20));
    }
    wxSize cs0 = GetClientSize();
    double radius = std::min(8.0, std::min(cs0.GetWidth(), cs0.GetHeight()) / 2.0);
    dc.SetBrush(wxBrush(fill));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRoundedRectangle(GetClientRect(), radius);
    dc.SetTextForeground(enabled_ ? fg_ : wxColour(120, 120, 120));
    dc.SetFont(GetFont());
    wxSize ext = dc.GetTextExtent(label_);
    wxSize cs = GetClientSize();
    dc.DrawText(label_, (cs.GetWidth() - ext.GetWidth()) / 2, (cs.GetHeight() - ext.GetHeight()) / 2);
}

// ---------------------------------------------------------------------------
// StatusDot
// ---------------------------------------------------------------------------
StatusDot::StatusDot(wxWindow *parent, wxColour color, int diameter)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(diameter, diameter), wxBORDER_NONE),
      color_(color), diameter_(diameter) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &StatusDot::OnPaint, this);
}

void StatusDot::SetColor(wxColour color) {
    color_ = color;
    Refresh();
}

void StatusDot::OnPaint(wxPaintEvent &) {
    wxAutoBufferedPaintDC dc(this);
    wxColour parentBg = GetParent() ? GetParent()->GetBackgroundColour() : *wxWHITE;
    dc.SetBackground(wxBrush(parentBg));
    dc.Clear();
    dc.SetBrush(wxBrush(color_));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawEllipse(0, 0, diameter_, diameter_);
}

// ---------------------------------------------------------------------------
// ColorSlider
// ---------------------------------------------------------------------------
ColorSlider::ColorSlider(wxWindow *parent, wxWindowID id, int value, int minVal, int maxVal)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 28), wxBORDER_NONE),
      value_(value), min_(minVal), max_(maxVal),
      track_(70, 70, 80), fill_(100, 200, 150), thumb_(230, 230, 230), cmd_id_(id) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCursor(wxCursor(wxCURSOR_HAND));
    Bind(wxEVT_PAINT, &ColorSlider::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &ColorSlider::OnMouseDown, this);
    Bind(wxEVT_LEFT_UP, &ColorSlider::OnMouseUp, this);
    Bind(wxEVT_MOTION, &ColorSlider::OnMouseMove, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &ColorSlider::OnCaptureLost, this);
    Bind(wxEVT_ENTER_WINDOW, &ColorSlider::OnMouseEnter, this);
    Bind(wxEVT_LEAVE_WINDOW, &ColorSlider::OnMouseLeave, this);
    // See themed_widgets.h note: without this, a sizer-driven resize (e.g.
    // closing a sibling panel and everything reflowing) only repaints the
    // newly-exposed strip, leaving the old thumb position on screen as a
    // ghost dot next to the real one.
    Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) { Refresh(true); evt.Skip(); });
}

void ColorSlider::SetTheme(wxColour track, wxColour fill, wxColour thumb) {
    track_ = track; fill_ = fill; thumb_ = thumb;
    Refresh();
}

void ColorSlider::SetValue(int v) {
    value_ = std::max(min_, std::min(max_, v));
    Refresh();
}

void ColorSlider::Notify() {
    wxCommandEvent evt(wxEVT_SLIDER, cmd_id_);
    evt.SetEventObject(this);
    evt.SetInt(value_);
    ProcessWindowEvent(evt);
}

void ColorSlider::UpdateFromX(int x) {
    wxSize cs = GetClientSize();
    int usable = std::max(1, cs.GetWidth() - 16); // matches OnPaint's margin for the new larger thumb
    double t = std::clamp((double)(x - 8) / usable, 0.0, 1.0);
    int newVal = min_ + (int)std::lround(t * (max_ - min_));
    if (newVal != value_) {
        value_ = newVal;
        Refresh();
        Notify();
    }
}

void ColorSlider::OnMouseDown(wxMouseEvent &evt) {
    dragging_ = true;
    CaptureMouse();
    UpdateFromX(evt.GetX());
}

void ColorSlider::OnMouseUp(wxMouseEvent &) {
    if (dragging_ && HasCapture()) ReleaseMouse();
    dragging_ = false;
}

void ColorSlider::OnMouseMove(wxMouseEvent &evt) {
    if (dragging_ && evt.LeftIsDown()) UpdateFromX(evt.GetX());
}

void ColorSlider::OnCaptureLost(wxMouseCaptureLostEvent &) {
    dragging_ = false;
}

void ColorSlider::OnMouseEnter(wxMouseEvent &) {
    hot_ = true;
    Refresh();
}

void ColorSlider::OnMouseLeave(wxMouseEvent &) {
    hot_ = false;
    Refresh();
}

void ColorSlider::OnPaint(wxPaintEvent &) {
    wxAutoBufferedPaintDC dc(this);
    wxColour parentBg = GetParent() ? GetParent()->GetBackgroundColour() : *wxWHITE;
    dc.SetBackground(wxBrush(parentBg));
    dc.Clear();

    wxSize cs = GetClientSize();
    const int trackH = 6;
    const int trackY = (cs.GetHeight() - trackH) / 2;
    const int usable = std::max(1, cs.GetWidth() - 16);
    double t = (max_ > min_) ? (double)(value_ - min_) / (max_ - min_) : 0.0;
    int thumbX = 8 + (int)std::lround(t * usable);

    // Track: a slightly recessed rounded groove rather than a flat line -
    // gives the slider actual visual depth instead of reading as a bare
    // gauge line with two dots on it.
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(track_));
    dc.DrawRoundedRectangle(4, trackY, cs.GetWidth() - 8, trackH, trackH / 2.0);

    if (thumbX > 8) {
        dc.SetBrush(wxBrush(fill_));
        dc.DrawRoundedRectangle(4, trackY, thumbX - 4, trackH, trackH / 2.0);
    }

    // Unity-gain tick (100 out of a 0-150 range) - without this the only
    // way to tell "am I at 100% or 110%?" was to eyeball a bare track.
    if (max_ > min_ && min_ <= 100 && 100 <= max_) {
        double ut = (double)(100 - min_) / (max_ - min_);
        int tickX = 8 + (int)std::lround(ut * usable);
        dc.SetPen(wxPen(parentBg, 2));
        dc.DrawLine(tickX, trackY - 1, tickX, trackY + trackH + 1);
    }

    // Hover glow: a soft ring behind the thumb so the control gives some
    // feedback before you're actually dragging it, not just a static dot.
    if (hot_ || dragging_) {
        wxColour glow = fill_;
        dc.SetBrush(wxBrush(wxColour(glow.Red(), glow.Green(), glow.Blue(), 60)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawCircle(thumbX, cs.GetHeight() / 2, dragging_ ? 13 : 11);
    }

    dc.SetBrush(wxBrush(thumb_));
    dc.SetPen(wxPen(fill_, 2));
    dc.DrawCircle(thumbX, cs.GetHeight() / 2, dragging_ ? 9 : 8);
}

// ---------------------------------------------------------------------------
// LabeledSlider
// ---------------------------------------------------------------------------

LabeledSlider::LabeledSlider(wxWindow *parent, wxWindowID id, int value, int minVal, int maxVal)
    : wxPanel(parent, wxID_ANY), value_(value), cmd_id_(id) {
    // 2.4 FIX ("white frames around every slider"): this wrapper panel was never
    // given a colour, so it painted the system's default WHITE, and ColorSlider
    // (which clears itself with its *parent's* colour) inherited that white
    // rectangle. Start from the real parent's colour; SetTheme() keeps it in sync.
    if (parent) SetBackgroundColour(parent->GetBackgroundColour());
    auto *sizer = new wxBoxSizer(wxHORIZONTAL);

    slider_ = new ColorSlider(this, wxID_ANY, value, minVal, maxVal);
    sizer->Add(slider_, 1, wxEXPAND | wxRIGHT, 8);

    spin_ = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(64, -1),
                            wxSP_ARROW_KEYS, minVal, maxVal, value);
    sizer->Add(spin_, 0, wxALIGN_CENTRE_VERTICAL);

#ifdef _WIN32
    // Fixes a "white square" artifact next to every slider: wxSpinCtrl's
    // buddy Edit control is drawn by Windows' visual-styles theme engine
    // (on by default via the app's ComCtl32 v6 manifest), which -- unlike
    // the classic/unthemed renderer -- ignores whatever background colour
    // SetTheme() below asks for and always paints its own white client
    // background. Opting this specific control out of themed drawing
    // falls back to classic rendering, which does respect a custom
    // background brush. Enumerate rather than assume
    // spin_->GetHandle() is the Edit itself -- wxSpinCtrl on MSW is really
    // an Edit + an UpDown control as separate sibling HWNDs, and which one
    // GetHandle() returns isn't something worth depending on here.
    EnumChildWindows((HWND)GetHandle(), [](HWND hwnd, LPARAM) -> BOOL {
        wchar_t cls[32] = {};
        GetClassNameW(hwnd, cls, 32);
        if (wcscmp(cls, L"Edit") == 0) SetWindowTheme(hwnd, L"", L"");
        return TRUE;
    }, 0);
#endif

    SetSizer(sizer);

    slider_->Bind(wxEVT_SLIDER, &LabeledSlider::OnSliderChanged, this);
    spin_->Bind(wxEVT_SPINCTRL, &LabeledSlider::OnSpinChanged, this);
    // wxEVT_TEXT fires as the user types digits directly into the spin
    // box, not just when using its up/down arrows - without this, typing
    // a value and clicking away wouldn't move the slider until some other
    // interaction happened to sync them.
    spin_->Bind(wxEVT_TEXT, &LabeledSlider::OnSpinText, this);
}

void LabeledSlider::SetTheme(wxColour track, wxColour fill, wxColour thumb, wxColour fieldBg, wxColour text) {
    if (GetParent()) SetBackgroundColour(GetParent()->GetBackgroundColour());  // see ctor: no white frame
    slider_->SetTheme(track, fill, thumb);
    spin_->SetBackgroundColour(fieldBg);
    spin_->SetForegroundColour(text);
}

void LabeledSlider::SetValue(int v) {
    if (updating_) return;
    updating_ = true;
    value_ = v;
    slider_->SetValue(v);
    spin_->SetValue(v);
    updating_ = false;
}

void LabeledSlider::OnSliderChanged(wxCommandEvent &evt) {
    if (updating_) return;
    updating_ = true;
    value_ = evt.GetInt();
    spin_->SetValue(value_);
    updating_ = false;
    Notify();
}

void LabeledSlider::OnSpinChanged(wxSpinEvent &evt) {
    if (updating_) return;
    updating_ = true;
    value_ = evt.GetPosition();
    slider_->SetValue(value_);
    updating_ = false;
    Notify();
}

void LabeledSlider::OnSpinText(wxCommandEvent &) {
    if (updating_) return;
    updating_ = true;
    value_ = spin_->GetValue(); // already clamped to [min,max] by wxSpinCtrl itself
    slider_->SetValue(value_);
    updating_ = false;
    Notify();
}

void LabeledSlider::Notify() {
    wxCommandEvent evt(wxEVT_SLIDER, cmd_id_);
    evt.SetEventObject(this);
    evt.SetInt(value_);
    ProcessWindowEvent(evt);
}

// ---------------------------------------------------------------------------
// FlatFader
// ---------------------------------------------------------------------------
FlatFader::FlatFader(wxWindow *parent, wxWindowID id, int value, int minVal, int maxVal, bool vertical)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
      value_(std::max(minVal, std::min(maxVal, value))), min_(minVal), max_(maxVal), vertical_(vertical),
      bg_(30, 30, 40), track_(60, 60, 75), fill_(100, 200, 150), thumb_(235, 235, 240), cmd_id_(id) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCursor(wxCursor(wxCURSOR_HAND));
    Bind(wxEVT_PAINT, &FlatFader::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &FlatFader::OnMouseDown, this);
    Bind(wxEVT_LEFT_UP, &FlatFader::OnMouseUp, this);
    Bind(wxEVT_LEFT_DCLICK, &FlatFader::OnDClick, this);
    Bind(wxEVT_MOTION, &FlatFader::OnMouseMove, this);
    Bind(wxEVT_MOUSEWHEEL, &FlatFader::OnWheel, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &FlatFader::OnCaptureLost, this);
    Bind(wxEVT_ENTER_WINDOW, &FlatFader::OnEnter, this);
    Bind(wxEVT_LEAVE_WINDOW, &FlatFader::OnLeave, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) { Refresh(false); evt.Skip(); });
}

wxSize FlatFader::DoGetBestSize() const {
    return vertical_ ? wxSize(24, 90) : wxSize(90, 24);
}

void FlatFader::SetColours(wxColour bg, wxColour track, wxColour fill, wxColour thumb) {
    bg_ = bg; track_ = track; fill_ = fill; thumb_ = thumb;
    Refresh(false);
}

void FlatFader::SetVertical(bool v) {
    if (vertical_ == v) return;
    vertical_ = v;
    InvalidateBestSize();
    Refresh(false);
}

void FlatFader::SetValue(int v) {
    v = std::max(min_, std::min(max_, v));
    if (v == value_) return;
    value_ = v;
    Refresh(false);
}

void FlatFader::SetAndNotify(int v) {
    v = std::max(min_, std::min(max_, v));
    if (v == value_) return;
    value_ = v;
    Refresh(false);
    wxCommandEvent evt(wxEVT_SLIDER, cmd_id_);
    evt.SetEventObject(this);
    evt.SetInt(value_);
    ProcessWindowEvent(evt);
}

void FlatFader::UpdateFromPos(const wxPoint &p) {
    const wxSize cs = GetClientSize();
    const int pad = 9;
    const int len = std::max(1, (vertical_ ? cs.GetHeight() : cs.GetWidth()) - pad * 2);
    double t = vertical_ ? 1.0 - (double)(p.y - pad) / len : (double)(p.x - pad) / len;
    t = std::clamp(t, 0.0, 1.0);
    SetAndNotify(min_ + (int)std::lround(t * (max_ - min_)));
}

void FlatFader::OnMouseDown(wxMouseEvent &evt) {
    dragging_ = true;
    if (!HasCapture()) CaptureMouse();
    UpdateFromPos(evt.GetPosition());
}
void FlatFader::OnMouseUp(wxMouseEvent &) {
    if (HasCapture()) ReleaseMouse();
    dragging_ = false;
    Refresh(false);
}
void FlatFader::OnMouseMove(wxMouseEvent &evt) {
    if (dragging_ && evt.LeftIsDown()) UpdateFromPos(evt.GetPosition());
}
void FlatFader::OnWheel(wxMouseEvent &evt) {
    const int steps = evt.GetWheelRotation() / std::max(1, evt.GetWheelDelta());
    if (steps != 0) SetAndNotify(value_ + steps * 5);
}
void FlatFader::OnDClick(wxMouseEvent &) { SetAndNotify(unity_); }

void FlatFader::OnPaint(wxPaintEvent &) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(bg_));
    dc.Clear();

    const wxSize cs = GetClientSize();
    const int pad = 9, grooveT = 6;
    const double t = (max_ > min_) ? (double)(value_ - min_) / (max_ - min_) : 0.0;
    const wxColour fill = dimmed_ ? wxColour(110, 110, 120) : fill_;

    dc.SetPen(*wxTRANSPARENT_PEN);
    if (!vertical_) {
        const int len = std::max(1, cs.GetWidth() - pad * 2);
        const int cy = cs.GetHeight() / 2;
        const int tx = pad + (int)std::lround(t * len);
        dc.SetBrush(wxBrush(track_));
        dc.DrawRoundedRectangle(pad, cy - grooveT / 2, len, grooveT, grooveT / 2.0);
        if (tx > pad) {
            dc.SetBrush(wxBrush(fill));
            dc.DrawRoundedRectangle(pad, cy - grooveT / 2, tx - pad, grooveT, grooveT / 2.0);
        }
        if (max_ > min_ && min_ < unity_ && unity_ < max_) {
            const int ux = pad + (int)std::lround((double)(unity_ - min_) / (max_ - min_) * len);
            dc.SetPen(wxPen(bg_, 2));
            dc.DrawLine(ux, cy - grooveT, ux, cy + grooveT);
            dc.SetPen(*wxTRANSPARENT_PEN);
        }
        dc.SetBrush(wxBrush(thumb_));
        dc.SetPen(wxPen(fill, 2));
        dc.DrawCircle(tx, cy, (dragging_ || hot_) ? 8 : 7);
    } else {
        const int len = std::max(1, cs.GetHeight() - pad * 2);
        const int cx = cs.GetWidth() / 2;
        const int ty = pad + (int)std::lround((1.0 - t) * len);
        dc.SetBrush(wxBrush(track_));
        dc.DrawRoundedRectangle(cx - grooveT / 2, pad, grooveT, len, grooveT / 2.0);
        if (ty < pad + len) {
            dc.SetBrush(wxBrush(fill));
            dc.DrawRoundedRectangle(cx - grooveT / 2, ty, grooveT, pad + len - ty, grooveT / 2.0);
        }
        if (max_ > min_ && min_ < unity_ && unity_ < max_) {
            const int uy = pad + (int)std::lround((1.0 - (double)(unity_ - min_) / (max_ - min_)) * len);
            dc.SetPen(wxPen(bg_, 2));
            dc.DrawLine(cx - grooveT, uy, cx + grooveT, uy);
            dc.SetPen(*wxTRANSPARENT_PEN);
        }
        dc.SetBrush(wxBrush(thumb_));
        dc.SetPen(wxPen(fill, 2));
        dc.DrawCircle(cx, ty, (dragging_ || hot_) ? 8 : 7);
    }
}

// ---------------------------------------------------------------------------
// HotkeyButton
// ---------------------------------------------------------------------------

namespace {
// Converts a wx key code (WXK_* / plain ASCII for letters+digits) into the
// token hr_hk_parse_keystring() recognizes for the "key" part of a binding
// (everything after the last '+'). Empty return means "don't finalize a
// capture on this keypress" - either a bare modifier or a key we don't
// have a mapping for.
wxString KeyCodeToToken(int code) {
    if (code >= WXK_F1 && code <= WXK_F24) return wxString::Format("F%d", code - WXK_F1 + 1);
    switch (code) {
        case WXK_ESCAPE:   return "";  // handled specially (cancels capture)
        case WXK_SPACE:    return "Space";
        case WXK_RETURN:
        case WXK_NUMPAD_ENTER: return "Return";
        case WXK_TAB:      return "Tab";
        case WXK_BACK:     return "BackSpace";
        case WXK_DELETE:   return "Delete";
        case WXK_INSERT:   return "Insert";
        case WXK_HOME:     return "Home";
        case WXK_END:      return "End";
        case WXK_PAGEUP:   return "PageUp";
        case WXK_PAGEDOWN: return "PageDown";
        case WXK_UP:       return "Up";
        case WXK_DOWN:     return "Down";
        case WXK_LEFT:     return "Left";
        case WXK_RIGHT:    return "Right";
        case WXK_SHIFT: case WXK_CONTROL: case WXK_ALT:
        case WXK_WINDOWS_LEFT: case WXK_WINDOWS_RIGHT:
            return ""; // bare modifier - keep listening
        default:
            if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9'))
                return wxString::Format("%c", (char)code);
            return "";
    }
}
} // namespace

HotkeyButton::HotkeyButton(wxWindow *parent, wxWindowID id, const wxString &initialValue)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 28), wxBORDER_NONE),
      value_(initialValue), bg_(70, 70, 80), fg_(230, 230, 235), accent_(120, 170, 250),
      cmd_id_(id) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCursor(wxCursor(wxCURSOR_HAND));
    // Buttons in a tab-order dialog would otherwise swallow Tab/Enter for
    // navigation instead of us ever seeing them during capture - accepting
    // focus here is required for OnKeyDown to fire on this window at all.
    SetCanFocus(true);
    Bind(wxEVT_PAINT, &HotkeyButton::OnPaint, this);
    Bind(wxEVT_LEFT_UP, &HotkeyButton::OnLeftUp, this);
    Bind(wxEVT_KEY_DOWN, &HotkeyButton::OnKeyDown, this);
    Bind(wxEVT_KILL_FOCUS, &HotkeyButton::OnKillFocus, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) { Refresh(true); evt.Skip(); });
}

void HotkeyButton::SetColours(wxColour bg, wxColour fg, wxColour accent) {
    bg_ = bg; fg_ = fg; accent_ = accent;
    Refresh();
}

wxSize HotkeyButton::DoGetBestSize() const {
    wxClientDC dc(const_cast<HotkeyButton *>(this));
    dc.SetFont(GetFont());
    wxSize ext = dc.GetTextExtent("Control+Shift+PageDown");
    int w = std::max(ext.GetWidth() + 24, 110);
    int h = std::max(ext.GetHeight() + 12, 28);
    return wxSize(w, h);
}

void HotkeyButton::StartCapture() {
    capturing_ = true;
    SetFocus();
    Refresh();
}

void HotkeyButton::EndCapture(bool /*cancelled*/) {
    capturing_ = false;
    Refresh();
}

void HotkeyButton::OnLeftUp(wxMouseEvent &) {
    if (!capturing_) StartCapture();
}

void HotkeyButton::OnKillFocus(wxFocusEvent &evt) {
    if (capturing_) EndCapture(/*cancelled=*/true);
    evt.Skip();
}

void HotkeyButton::OnKeyDown(wxKeyEvent &evt) {
    if (!capturing_) { evt.Skip(); return; }

    if (evt.GetKeyCode() == WXK_ESCAPE) {
        EndCapture(/*cancelled=*/true);
        return;
    }

    wxString token = KeyCodeToToken(evt.GetKeyCode());
    if (token.empty()) return; // bare modifier or unmapped key - keep listening

    wxString combo;
    if (evt.ControlDown()) combo += "Control+";
    if (evt.ShiftDown())   combo += "Shift+";
    if (evt.AltDown())     combo += "Alt+";
#ifdef _WIN32
    if (::GetKeyState(VK_LWIN) < 0 || ::GetKeyState(VK_RWIN) < 0) combo += "Win+";
#endif
    combo += token;

    value_ = combo;
    EndCapture(/*cancelled=*/false);

    wxCommandEvent out(wxEVT_TEXT, cmd_id_);
    out.SetEventObject(this);
    out.SetString(value_);
    ProcessWindowEvent(out);
}

void HotkeyButton::OnPaint(wxPaintEvent &) {
    wxAutoBufferedPaintDC dc(this);
    wxColour parentBg = GetParent() ? GetParent()->GetBackgroundColour() : *wxWHITE;
    dc.SetBackground(wxBrush(parentBg));
    dc.Clear();

    wxSize cs = GetClientSize();
    wxRect rc(0, 0, cs.GetWidth(), cs.GetHeight());

    dc.SetBrush(wxBrush(bg_));
    dc.SetPen(wxPen(capturing_ ? accent_ : bg_, 2));
    dc.DrawRoundedRectangle(rc, 6);

    wxString text = capturing_ ? wxString("Press a key...") : (value_.empty() ? wxString("(none)") : value_);
    dc.SetTextForeground(capturing_ ? accent_ : fg_);
    dc.SetFont(GetFont());
    wxSize ext = dc.GetTextExtent(text);
    dc.DrawText(text, (cs.GetWidth() - ext.GetWidth()) / 2, (cs.GetHeight() - ext.GetHeight()) / 2);
}

