// themed_widgets.h - small reusable themed wx controls.
//
// Pulled out of main_frame.h/.cpp so other wx-ported pieces (audio_panel,
// settings_dialog, and whichever dialog gets ported next) can use the same
// flat colored button / status dot instead of everyone reinventing them.
#pragma once

#include <wx/wx.h>
#include <wx/spinctrl.h>
#include "theme.h"

wxColour FromColorref(COLORREF c);

// Flat, explicitly-colored button - used instead of plain wxButton because
// native-themed wxButton on Windows can silently ignore SetBackgroundColour()
// depending on whether UxTheme is active, which would just reproduce the
// "custom colors don't show up" problem this whole rewrite is meant to fix.
// Paints itself directly, so START/STOP/PAUSE/RESUME/Save/Mute colors always
// show regardless of the current theme.
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

// Flat custom slider - used instead of wxSlider because on Windows that's a
// native trackbar control (Win32 Trackbar/Slider class) with essentially no
// color customization available; it would still render as stock grey UI no
// matter what color the surrounding panel is, which is the same "ugly
// mismatched control" problem this file exists to avoid. Fires a plain
// wxEVT_SLIDER wxCommandEvent (GetInt() = new value), same as wxSlider,
// so callers don't need to know it isn't one.
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

// Every slider in the app used to be a bare ColorSlider with no numeric
// readout - fine for a rough adjustment, useless for "set this to exactly
// 62". LabeledSlider pairs a ColorSlider with a spin box showing the same
// value: drag the slider, or type/click-step an exact number, either one
// updates the other. Fires the same wxEVT_SLIDER wxCommandEvent a bare
// ColorSlider does (GetInt() = new value), so call sites that already
// Bind() a ColorSlider by id don't need to change.
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
// "Control+Shift+F9") as its label; clicking it puts it into "listening"
// mode ("Press a key...") and the next real key press (plus whatever
// Ctrl/Shift/Alt/Win are held at that moment) becomes the new binding,
// formatted the same way hr_hk_parse_keystring() expects. Escape cancels
// a capture in progress without changing the value. Replaces free-text
// entry, which meant knowing the exact string format (e.g. "Control+F9")
// to type by hand.
class HotkeyButton : public wxPanel {
public:
    HotkeyButton(wxWindow *parent, wxWindowID id, const wxString &initialValue);
    void SetColours(wxColour bg, wxColour fg, wxColour accent);
    wxString GetValue() const { return value_; }
    void SetValue(const wxString &v) { value_ = v; Refresh(); }

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
