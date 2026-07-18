#include "themed_widgets.h"
#include <wx/dcbuffer.h>
#include <algorithm>
#include <cmath>

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
    wxColour fill = enabled_ ? bg_ : disabled_bg_;
    if (enabled_ && hover_) {
        fill = wxColour(std::max(0, fill.Red() - 20), std::max(0, fill.Green() - 20), std::max(0, fill.Blue() - 20));
    }
    dc.SetBrush(wxBrush(fill));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(GetClientRect());
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
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 24), wxBORDER_NONE),
      value_(value), min_(minVal), max_(maxVal),
      track_(70, 70, 80), fill_(100, 200, 150), thumb_(230, 230, 230), cmd_id_(id) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCursor(wxCursor(wxCURSOR_HAND));
    Bind(wxEVT_PAINT, &ColorSlider::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &ColorSlider::OnMouseDown, this);
    Bind(wxEVT_LEFT_UP, &ColorSlider::OnMouseUp, this);
    Bind(wxEVT_MOTION, &ColorSlider::OnMouseMove, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &ColorSlider::OnCaptureLost, this);
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
    int usable = std::max(1, cs.GetWidth() - 12); // 12 = thumb diameter, keeps thumb center in-bounds
    double t = std::clamp((double)(x - 6) / usable, 0.0, 1.0);
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

void ColorSlider::OnPaint(wxPaintEvent &) {
    wxAutoBufferedPaintDC dc(this);
    wxColour parentBg = GetParent() ? GetParent()->GetBackgroundColour() : *wxWHITE;
    dc.SetBackground(wxBrush(parentBg));
    dc.Clear();

    wxSize cs = GetClientSize();
    int trackH = 4;
    int trackY = (cs.GetHeight() - trackH) / 2;
    int usable = std::max(1, cs.GetWidth() - 12);
    double t = (max_ > min_) ? (double)(value_ - min_) / (max_ - min_) : 0.0;
    int thumbX = 6 + (int)std::lround(t * usable);

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(track_));
    dc.DrawRoundedRectangle(0, trackY, cs.GetWidth(), trackH, trackH / 2.0);

    if (thumbX > 6) {
        dc.SetBrush(wxBrush(fill_));
        dc.DrawRoundedRectangle(0, trackY, thumbX, trackH, trackH / 2.0);
    }

    dc.SetBrush(wxBrush(thumb_));
    dc.SetPen(wxPen(fill_, 1));
    dc.DrawCircle(thumbX, cs.GetHeight() / 2, 6);
}

