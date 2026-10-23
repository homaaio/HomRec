// themed_widgets.h - small reusable themed wx controls.
#pragma once

#include <wx/wx.h>
#include <wx/spinctrl.h>
#include "theme.h"

wxColour FromColorref(COLORREF c);

// Makes the mouse wheel harmless over every number box (wxSpinCtrl) and drop-down (wxChoice /
// wxComboBox) inside `root`, recursively: scrolling the page with the cursor resting on such a
// control used to change its value by accident. The wheel is passed on to the page behind the
// control instead, so the page still scrolls. An OPEN drop-down list keeps its own wheel scrolling,
// and the sliders / faders that advertise the wheel in their tooltip are not touched.
// Call it once after the dialog's controls are built.
void HrInstallWheelGuard(wxWindow *root);

// Flat, explicitly-colored button - used instead of plain wxButton because
// native-themed wxButton on Windows can silently ignore SetBackgroundColour()
class ColorButton : public wxPanel {
public:
    ColorButton(wxWindow *parent, wxWindowID id, const wxString &label);
    void SetColours(wxColour bg, wxColour fg);
    void SetLabelText2(const wxString &label) { label_ = label; Refresh(); }
    void Enable2(bool enabled);
    bool IsEnabled2() const { return enabled_; }

private:
    void OnPaint(wxPaintEvent &evt);
    void OnLeftUp(wxMouseEvent &evt);
    void OnEnter(wxMouseEvent &evt);
    void OnLeave(wxMouseEvent &evt);

    wxString label_;
    wxColour bg_, fg_, disabled_bg_;
    bool enabled_ = true;
    bool hover_ = false;
    wxWindowID cmd_id_;
};

// Small colored-circle indicator - sidebar STATUS row / bottom bar dot.
class StatusDot : public wxPanel {
public:
    StatusDot(wxWindow *parent, wxColour color, int diameter = 14);
    void SetColor(wxColour color);

private:
    void OnPaint(wxPaintEvent &evt);
    wxColour color_;
    int diameter_;
};

class ColorSlider : public wxPanel {
public:
    ColorSlider(wxWindow *parent, wxWindowID id, int value, int minVal, int maxVal);
    void SetTheme(wxColour track, wxColour fill, wxColour thumb);
    int GetValue() const { return value_; }
    void SetValue(int v);

private:
    void OnPaint(wxPaintEvent &evt);
    void OnMouseDown(wxMouseEvent &evt);
    void OnMouseUp(wxMouseEvent &evt);
    void OnMouseMove(wxMouseEvent &evt);
    void OnMouseEnter(wxMouseEvent &evt);
    void OnMouseLeave(wxMouseEvent &evt);
    void OnCaptureLost(wxMouseCaptureLostEvent &evt);
    void UpdateFromX(int x);
    void Notify();

    int value_, min_, max_;
    wxColour track_, fill_, thumb_;
    bool dragging_ = false;
    bool hot_ = false;
    wxWindowID cmd_id_;
};

// Slim fader used by the 2.4 Audio Mixer: horizontal OR vertical, rounded groove,
// filled part, round thumb, mouse-wheel (+-5) and double-click (reset to unity).
class FlatFader : public wxPanel {
public:
    FlatFader(wxWindow *parent, wxWindowID id, int value, int minVal, int maxVal, bool vertical = false);
    void SetColours(wxColour bg, wxColour track, wxColour fill, wxColour thumb);
    void SetVertical(bool v);
    bool IsVertical() const { return vertical_; }
    void SetUnity(int v) { unity_ = v; Refresh(false); }   // tick mark / double-click target
    void SetDimmed(bool d) { if (dimmed_ != d) { dimmed_ = d; Refresh(false); } }
    int  GetValue() const { return value_; }
    void SetValue(int v);

protected:
    wxSize DoGetBestSize() const override;

private:
    void OnPaint(wxPaintEvent &evt);
    void OnMouseDown(wxMouseEvent &evt);
    void OnMouseUp(wxMouseEvent &evt);
    void OnMouseMove(wxMouseEvent &evt);
    void OnWheel(wxMouseEvent &evt);
    void OnDClick(wxMouseEvent &evt);
    void OnEnter(wxMouseEvent &) { if (!hot_) { hot_ = true; Refresh(false); } }
    void OnLeave(wxMouseEvent &) { if (hot_) { hot_ = false; Refresh(false); } }
    void OnCaptureLost(wxMouseCaptureLostEvent &) { dragging_ = false; Refresh(false); }
    void UpdateFromPos(const wxPoint &p);
    void SetAndNotify(int v);

    int value_, min_, max_, unity_ = 100;
    bool vertical_;
    wxColour bg_, track_, fill_, thumb_;
    bool dragging_ = false, hot_ = false, dimmed_ = false;
    wxWindowID cmd_id_;
};

// Every slider in the app used to be a bare ColorSlider with no numeric readout - fine for
// a rough adjustment, useless for "set this to exactly 62".
class LabeledSlider : public wxPanel {
public:
    LabeledSlider(wxWindow *parent, wxWindowID id, int value, int minVal, int maxVal);
    void SetTheme(wxColour track, wxColour fill, wxColour thumb, wxColour fieldBg, wxColour text);
    int GetValue() const { return value_; }
    void SetValue(int v);

private:
    void OnSliderChanged(wxCommandEvent &evt);
    void OnSpinChanged(wxSpinEvent &evt);
    void OnSpinText(wxCommandEvent &evt);
    void Notify();

    ColorSlider *slider_ = nullptr;
    wxSpinCtrl *spin_ = nullptr;
    int value_;
    bool updating_ = false; // guards against slider<->spin update feedback loops
    wxWindowID cmd_id_;
};

// Click-to-capture hotkey control: shows the current binding (e.g.
class HotkeyButton : public wxPanel {
public:
    HotkeyButton(wxWindow *parent, wxWindowID id, const wxString &initialValue);
    void SetColours(wxColour bg, wxColour fg, wxColour accent);
    wxString GetValue() const { return value_; }
    void SetValue(const wxString &v) { value_ = v; Refresh(); }

protected:
    wxSize DoGetBestSize() const override;

private:
    void OnPaint(wxPaintEvent &evt);
    void OnLeftUp(wxMouseEvent &evt);
    void OnKeyDown(wxKeyEvent &evt);
    void OnKillFocus(wxFocusEvent &evt);
    void StartCapture();
    void EndCapture(bool cancelled);

    wxString value_;
    wxColour bg_, fg_, accent_;
    bool capturing_ = false;
    wxWindowID cmd_id_;
};
