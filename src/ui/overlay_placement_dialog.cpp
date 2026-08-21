#include "overlay_placement_dialog.h"
#include "recording_controller.h"
#include "themed_widgets.h"
#include <wx/dcbuffer.h>
#include <algorithm>
#include <cstring>

namespace {

// Static-screenshot canvas with drag/resize on state.overlays - the same
// hit-testing/drag math PreviewPanel uses for the live in-place editing
// mode (main_frame.cpp), just against a still bitmap and this dialog's
// own local working copy instead of the live preview + state_.overlays
// directly (see ShowOverlayPlacementDialog()'s comment on why it's a
// copy).
class OverlayCanvas : public wxPanel {
public:
    OverlayCanvas(wxWindow *parent, std::vector<OverlayDef> &overlays)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                   wxFULL_REPAINT_ON_RESIZE | wxBORDER_NONE),
          overlays_(overlays) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &OverlayCanvas::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &OverlayCanvas::OnLeftDown, this);
        Bind(wxEVT_MOTION, &OverlayCanvas::OnMouseMove, this);
        Bind(wxEVT_LEFT_UP, &OverlayCanvas::OnLeftUp, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &OverlayCanvas::OnCaptureLost, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) { cached_dst_w_ = -1; Refresh(); evt.Skip(); });
    }

    // buf is tightly-packed RGB (w*h*3 bytes), same format
    // RecordingController::CaptureSnapshotFrame()/GetPreviewFrame() always
    // hand back (see main_frame.cpp's PreviewPanel::OnPaint for the same
    // assumption) - copied into bitmap_src_ since buf itself is a local
    // the caller reuses/discards right after this call.
    void SetScreenshot(const std::vector<uint8_t> &buf, int w, int h) {
        shot_w_ = w; shot_h_ = h;
        if (!buf.empty() && w > 0 && h > 0) {
            wxImage img(w, h, const_cast<unsigned char *>(buf.data()), /*static_data=*/true);
            bitmap_src_ = wxBitmap(img.Copy());
        } else {
            bitmap_src_ = wxBitmap();
        }
        cached_dst_w_ = -1;
        Refresh();
    }

private:
    enum class Corner { kNone, kTopLeft, kBottomRight };

    bool GetImageRect(wxRect &out) const {
        if (!bitmap_src_.IsOk() || shot_w_ <= 0 || shot_h_ <= 0) return false;
        wxSize cs = GetClientSize();
        if (cs.GetWidth() <= 0 || cs.GetHeight() <= 0) return false;
        double scale = std::min((double)cs.GetWidth() / shot_w_, (double)cs.GetHeight() / shot_h_);
        int dw = std::max(1, (int)(shot_w_ * scale));
        int dh = std::max(1, (int)(shot_h_ * scale));
        out = wxRect((cs.GetWidth() - dw) / 2, (cs.GetHeight() - dh) / 2, dw, dh);
        return true;
    }

    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(wxColour(24, 24, 30)));
        dc.Clear();

        wxRect imgRect;
        if (!GetImageRect(imgRect)) {
            dc.SetTextForeground(wxColour(180, 180, 190));
            wxString msg = "No screenshot yet \u2013 click Refresh below.";
            wxSize ext = dc.GetTextExtent(msg);
            wxSize cs = GetClientSize();
            dc.DrawText(msg, (cs.GetWidth() - ext.GetWidth()) / 2, (cs.GetHeight() - ext.GetHeight()) / 2);
            return;
        }

        if (imgRect.GetWidth() != cached_dst_w_ || imgRect.GetHeight() != cached_dst_h_) {
            wxImage src = bitmap_src_.ConvertToImage();
            wxImage scaled = src.Scale(imgRect.GetWidth(), imgRect.GetHeight(), wxIMAGE_QUALITY_BILINEAR);
            cached_bmp_ = wxBitmap(scaled);
            cached_dst_w_ = imgRect.GetWidth();
            cached_dst_h_ = imgRect.GetHeight();
        }
        dc.DrawBitmap(cached_bmp_, imgRect.GetX(), imgRect.GetY());

        double sx = (double)imgRect.GetWidth() / shot_w_;
        double sy = (double)imgRect.GetHeight() / shot_h_;
        const int handle = 8;
        for (size_t i = 0; i < overlays_.size(); ++i) {
            const auto &ov = overlays_[i];
            int rx = imgRect.GetX() + (int)(ov.x * sx);
            int ry = imgRect.GetY() + (int)(ov.y * sy);
            int rw = std::max(4, (int)(ov.w * sx));
            int rh = std::max(4, (int)(ov.h * sy));
            bool active = ((int)i == drag_index_);
            wxColour accent = active ? wxColour(255, 210, 90) : wxColour(120, 170, 250);

            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(accent, active ? 2 : 1, wxPENSTYLE_SHORT_DASH));
            dc.DrawRectangle(rx, ry, rw, rh);

            dc.SetBrush(wxBrush(accent));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(rx + rw - handle, ry + rh - handle, handle, handle);
            dc.DrawRectangle(rx, ry, handle, handle);

            wxString label = wxString::FromUTF8((ov.name.empty() ? ov.type : ov.name).c_str());
            if (!label.empty()) {
                dc.SetTextForeground(accent);
                dc.DrawText(label, rx + 2, std::max(0, ry - 16));
            }
        }
    }

    void OnLeftDown(wxMouseEvent &evt) {
        wxRect imgRect;
        if (!GetImageRect(imgRect)) { evt.Skip(); return; }
        double sx = (double)imgRect.GetWidth() / shot_w_;
        double sy = (double)imgRect.GetHeight() / shot_h_;
        int mx = evt.GetX(), my = evt.GetY();
        const int handle = 8;

        for (int i = (int)overlays_.size() - 1; i >= 0; --i) {
            auto &ov = overlays_[(size_t)i];
            int rx = imgRect.GetX() + (int)(ov.x * sx);
            int ry = imgRect.GetY() + (int)(ov.y * sy);
            int rw = std::max(4, (int)(ov.w * sx));
            int rh = std::max(4, (int)(ov.h * sy));
            wxRect body(rx, ry, rw, rh);
            wxRect brHandle(rx + rw - handle, ry + rh - handle, handle, handle);
            wxRect tlHandle(rx, ry, handle, handle);

            if (brHandle.Contains(mx, my)) drag_corner_ = Corner::kBottomRight;
            else if (tlHandle.Contains(mx, my)) drag_corner_ = Corner::kTopLeft;
            else if (body.Contains(mx, my)) drag_corner_ = Corner::kNone;
            else continue;

            drag_index_ = i;
            drag_start_mx_ = mx; drag_start_my_ = my;
            drag_start_x_ = ov.x; drag_start_y_ = ov.y;
            drag_start_w_ = ov.w; drag_start_h_ = ov.h;
            CaptureMouse();
            Refresh();
            return;
        }
        evt.Skip();
    }

    void OnMouseMove(wxMouseEvent &evt) {
        if (drag_index_ < 0 || !evt.LeftIsDown()) { evt.Skip(); return; }
        wxRect imgRect;
        if (!GetImageRect(imgRect) || imgRect.GetWidth() <= 0 || imgRect.GetHeight() <= 0) {
            evt.Skip();
            return;
        }
        double sx = (double)imgRect.GetWidth() / shot_w_;
        double sy = (double)imgRect.GetHeight() / shot_h_;
        int dx = (int)std::lround((evt.GetX() - drag_start_mx_) / sx);
        int dy = (int)std::lround((evt.GetY() - drag_start_my_) / sy);

        auto &ov = overlays_[(size_t)drag_index_];
        int cw = shot_w_, ch = shot_h_;
        if (drag_corner_ == Corner::kBottomRight) {
            ov.w = std::max(10, drag_start_w_ + dx);
            ov.h = std::max(10, drag_start_h_ + dy);
        } else if (drag_corner_ == Corner::kTopLeft) {
            int new_w = std::max(10, drag_start_w_ - dx);
            int new_h = std::max(10, drag_start_h_ - dy);
            int anchor_right  = drag_start_x_ + drag_start_w_;
            int anchor_bottom = drag_start_y_ + drag_start_h_;
            ov.w = new_w;
            ov.h = new_h;
            ov.x = std::clamp(anchor_right - new_w, 0, std::max(0, cw - new_w));
            ov.y = std::clamp(anchor_bottom - new_h, 0, std::max(0, ch - new_h));
        } else {
            ov.x = std::clamp(drag_start_x_ + dx, 0, std::max(0, cw - ov.w));
            ov.y = std::clamp(drag_start_y_ + dy, 0, std::max(0, ch - ov.h));
        }
        Refresh();
    }

    void OnLeftUp(wxMouseEvent &evt) {
        if (drag_index_ >= 0) {
            if (HasCapture()) ReleaseMouse();
            drag_index_ = -1;
            drag_corner_ = Corner::kNone;
            Refresh();
        }
        evt.Skip();
    }

    void OnCaptureLost(wxMouseCaptureLostEvent &) {
        drag_index_ = -1;
        drag_corner_ = Corner::kNone;
        Refresh();
    }

    std::vector<OverlayDef> &overlays_;
    wxBitmap bitmap_src_, cached_bmp_;
    int shot_w_ = 0, shot_h_ = 0;
    int cached_dst_w_ = -1, cached_dst_h_ = -1;
    int drag_index_ = -1;
    Corner drag_corner_ = Corner::kNone;
    int drag_start_mx_ = 0, drag_start_my_ = 0;
    int drag_start_x_ = 0, drag_start_y_ = 0, drag_start_w_ = 0, drag_start_h_ = 0;
};

} // namespace

bool ShowOverlayPlacementDialog(wxWindow *parent, AppState &state,
                                 RecordingController *rec, const ThemeColors &theme) {
    if (!rec) return false;

    // Local working copy - state.overlays only gets overwritten if the
    // user clicks Apply, so Cancel (or the [X] button) truly discards
    // anything dragged/resized in this window.
    std::vector<OverlayDef> working = state.overlays;

    wxDialog dlg(parent, wxID_ANY, "Position Overlays", wxDefaultPosition, wxSize(900, 650),
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    wxColour bg = FromColorref(theme.bg);
    dlg.SetBackgroundColour(bg);

    auto *root = new wxBoxSizer(wxVERTICAL);

    auto *hint = new wxStaticText(&dlg, wxID_ANY,
        "Drag an overlay to reposition it, or drag a corner handle to resize it. "
        "Refresh re-takes the screenshot; Apply saves the new positions.");
    hint->SetForegroundColour(FromColorref(theme.text_secondary));
    hint->Wrap(860);
    root->Add(hint, 0, wxEXPAND | wxALL, 10);

    auto *canvas = new OverlayCanvas(&dlg, working);
    canvas->SetMinSize(wxSize(400, 300));
    root->Add(canvas, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    auto *btnRow = new wxBoxSizer(wxHORIZONTAL);
    auto *refreshBtn = new wxButton(&dlg, wxID_ANY, "Refresh");
    auto *cancelBtn  = new wxButton(&dlg, wxID_CANCEL, "Cancel");
    auto *applyBtn   = new wxButton(&dlg, wxID_OK, "Apply");
    btnRow->Add(refreshBtn, 0, wxALL, 8);
    btnRow->AddStretchSpacer(1);
    btnRow->Add(cancelBtn, 0, wxALL, 8);
    btnRow->Add(applyBtn, 0, wxALL, 8);
    root->Add(btnRow, 0, wxEXPAND);

    dlg.SetSizer(root);

    // Same call the old snapshot-mode flow used - see overlays_dock_panel.h.
    // first_call=true starts the temporary preview pipeline if one isn't
    // already running (e.g. "Disable live preview" is on) and waits
    // briefly for its first frame.
    auto takeScreenshot = [&](bool first_call) -> bool {
        std::vector<uint8_t> buf;
        int w = 0, h = 0;
        if (rec->CaptureSnapshotFrame(buf, w, h, first_call)) {
            canvas->SetScreenshot(buf, w, h);
            return true;
        }
        return false;
    };

    if (!takeScreenshot(/*first_call=*/true)) {
        wxMessageBox("Couldn't capture a screenshot to edit overlays against - try Refresh in a moment.",
                     "HomRec", wxOK | wxICON_WARNING, &dlg);
    }

    refreshBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { takeScreenshot(/*first_call=*/false); });

    int result = dlg.ShowModal();

    // Always tear down the temporary snapshot pipeline on the way out,
    // regardless of Apply/Cancel/[X] - see the header comment on why this
    // (rather than leaving it to "next time Settings happens to reopen")
    // is what fixes the old resource-leak bug.
    rec->EndSnapshotEditing();

    if (result == wxID_OK) {
        state.overlays = working;
        return true;
    }
    return false;
}
