#include "overlay_editor_dialog.h"
#include "recording_controller.h"
#include "themed_widgets.h"
#include "hrc_config.h"
#include "../hr_webcam_enum.h"
#include "../hr_input_overlay.h"
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/choice.h>
#include <wx/clrpicker.h>
#include <wx/statline.h>
#include <wx/scrolwin.h>
#include <algorithm>
#include <cstring>

namespace {

// ---------------------------------------------------------------------
// Position + live preview canvas - same screenshot/drag/resize approach
// overlay_placement_dialog.cpp's OverlayCanvas uses, narrowed to a single
// OverlayDef (this window only ever edits one), and actually rendering
// the overlay's content on top of the handle/frame instead of a bare
// dashed rectangle, so "how will this look" and "where will this sit"
// can both be judged without leaving the window.
class SingleOverlayCanvas : public wxPanel {
public:
    SingleOverlayCanvas(wxWindow *parent, OverlayDef &ov)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                   wxFULL_REPAINT_ON_RESIZE | wxBORDER_NONE),
          ov_(ov) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &SingleOverlayCanvas::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &SingleOverlayCanvas::OnLeftDown, this);
        Bind(wxEVT_MOTION, &SingleOverlayCanvas::OnMouseMove, this);
        Bind(wxEVT_LEFT_UP, &SingleOverlayCanvas::OnLeftUp, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &SingleOverlayCanvas::OnCaptureLost, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) { cached_dst_w_ = -1; Refresh(); evt.Skip(); });
    }

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

    void SetNativeSize(int w, int h) {
        if (w > 0 && h > 0) { native_w_ = w; native_h_ = h; }
    }

    // Reloads whatever file the overlay's content preview needs (image/gif
    // first frame, or the input-overlay spritesheet) - called once up
    // front and again whenever the settings panel changes the path, so a
    // freshly-picked file shows up immediately instead of only after
    // reopening the window.
    void ReloadContentImage() {
        content_img_ = wxImage();
        std::string path = (ov_.type == "input_overlay") ? ov_.input_png_path : ov_.image_path;
        if (!path.empty()) {
            wxImage img;
            if (img.LoadFile(wxString::FromUTF8(path.c_str()))) content_img_ = img;
        }
        Refresh();
    }

private:
    enum class Corner { kNone, kTopLeft, kBottomRight };

    int NativeW() const { return native_w_ > 0 ? native_w_ : shot_w_; }
    int NativeH() const { return native_h_ > 0 ? native_h_ : shot_h_; }

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

    static wxColour ParseHexColor(const std::string &hex, wxColour fallback) {
        if (hex.size() != 7 || hex[0] != '#') return fallback;
        auto hx = [&](int pos) {
            return (int)strtol(hex.substr(pos, 2).c_str(), nullptr, 16);
        };
        return wxColour(hx(1), hx(3), hx(5));
    }

    // Draws an approximation of the overlay's actual content inside
    // `rect` - not the exact same code path the recording pipeline uses
    // (hr_overlay_render.cpp), but enough to judge font/color/framing/
    // cropping before committing, which the old rectangle-only preview
    // (and the old raw-popup edit flow, which had no preview at all)
    // couldn't offer.
    void DrawContent(wxDC &dc, const wxRect &rect) {
        dc.SetClippingRegion(rect);
        if (ov_.type == "text") {
            wxColour col = ParseHexColor(ov_.text_color, *wxWHITE);
            dc.SetTextForeground(col);
            wxFont f(wxFontInfo(std::max(8, rect.GetHeight() / 4))
                         .FaceName(wxString::FromUTF8(ov_.font_family.c_str())));
            dc.SetFont(f);
            wxString txt = wxString::FromUTF8(ov_.text.c_str());
            dc.DrawText(txt, rect.GetX() + 4, rect.GetY() + 4);
        } else if ((ov_.type == "image" || ov_.type == "gif" || ov_.type == "input_overlay")
                   && content_img_.IsOk()) {
            wxImage scaled = content_img_.Scale(std::max(1, rect.GetWidth()), std::max(1, rect.GetHeight()),
                                                 wxIMAGE_QUALITY_BILINEAR);
            dc.DrawBitmap(wxBitmap(scaled), rect.GetX(), rect.GetY());
        } else {
            const char *icon = (ov_.type == "webcam") ? "Webcam" : "(no preview)";
            wxString label = wxString::FromUTF8(
                ov_.type == "webcam"
                    ? (ov_.webcam_name.empty() ? std::string("Webcam") : ov_.webcam_name)
                    : std::string(icon));
            dc.SetTextForeground(wxColour(200, 200, 210));
            wxSize ext = dc.GetTextExtent(label);
            dc.DrawText(label, rect.GetX() + (rect.GetWidth() - ext.GetWidth()) / 2,
                        rect.GetY() + (rect.GetHeight() - ext.GetHeight()) / 2);
        }
        dc.DestroyClippingRegion();
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

        double sx = (double)imgRect.GetWidth() / NativeW();
        double sy = (double)imgRect.GetHeight() / NativeH();
        const int handle = 9;

        int rx = imgRect.GetX() + (int)(ov_.x * sx);
        int ry = imgRect.GetY() + (int)(ov_.y * sy);
        int rw = std::max(4, (int)(ov_.w * sx));
        int rh = std::max(4, (int)(ov_.h * sy));
        wxRect rect(rx, ry, rw, rh);

        if (ov_.visible) DrawContent(dc, rect);

        wxColour accent(255, 210, 90);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(accent, dragging_ ? 2 : 1, wxPENSTYLE_SHORT_DASH));
        dc.DrawRectangle(rect);

        dc.SetBrush(wxBrush(accent));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(rx + rw - handle, ry + rh - handle, handle, handle);
        dc.DrawRectangle(rx, ry, handle, handle);
    }

    void OnLeftDown(wxMouseEvent &evt) {
        wxRect imgRect;
        if (!GetImageRect(imgRect)) { evt.Skip(); return; }
        double sx = (double)imgRect.GetWidth() / NativeW();
        double sy = (double)imgRect.GetHeight() / NativeH();
        int mx = evt.GetX(), my = evt.GetY();
        const int handle = 9;

        int rx = imgRect.GetX() + (int)(ov_.x * sx);
        int ry = imgRect.GetY() + (int)(ov_.y * sy);
        int rw = std::max(4, (int)(ov_.w * sx));
        int rh = std::max(4, (int)(ov_.h * sy));
        wxRect body(rx, ry, rw, rh);
        wxRect brHandle(rx + rw - handle, ry + rh - handle, handle, handle);
        wxRect tlHandle(rx, ry, handle, handle);

        if (brHandle.Contains(mx, my)) drag_corner_ = Corner::kBottomRight;
        else if (tlHandle.Contains(mx, my)) drag_corner_ = Corner::kTopLeft;
        else if (body.Contains(mx, my)) drag_corner_ = Corner::kNone;
        else { evt.Skip(); return; }

        dragging_ = true;
        drag_start_mx_ = mx; drag_start_my_ = my;
        drag_start_x_ = ov_.x; drag_start_y_ = ov_.y;
        drag_start_w_ = ov_.w; drag_start_h_ = ov_.h;
        CaptureMouse();
        Refresh();
    }

    void OnMouseMove(wxMouseEvent &evt) {
        if (!dragging_ || !evt.LeftIsDown()) { evt.Skip(); return; }
        wxRect imgRect;
        if (!GetImageRect(imgRect) || imgRect.GetWidth() <= 0 || imgRect.GetHeight() <= 0) {
            evt.Skip();
            return;
        }
        double sx = (double)imgRect.GetWidth() / NativeW();
        double sy = (double)imgRect.GetHeight() / NativeH();
        int dx = (int)std::lround((evt.GetX() - drag_start_mx_) / sx);
        int dy = (int)std::lround((evt.GetY() - drag_start_my_) / sy);

        int cw = NativeW(), ch = NativeH();
        if (drag_corner_ == Corner::kBottomRight) {
            ov_.w = std::max(10, drag_start_w_ + dx);
            ov_.h = std::max(10, drag_start_h_ + dy);
        } else if (drag_corner_ == Corner::kTopLeft) {
            int new_w = std::max(10, drag_start_w_ - dx);
            int new_h = std::max(10, drag_start_h_ - dy);
            int anchor_right  = drag_start_x_ + drag_start_w_;
            int anchor_bottom = drag_start_y_ + drag_start_h_;
            ov_.w = new_w;
            ov_.h = new_h;
            ov_.x = std::clamp(anchor_right - new_w, 0, std::max(0, cw - new_w));
            ov_.y = std::clamp(anchor_bottom - new_h, 0, std::max(0, ch - new_h));
        } else {
            ov_.x = std::clamp(drag_start_x_ + dx, 0, std::max(0, cw - ov_.w));
            ov_.y = std::clamp(drag_start_y_ + dy, 0, std::max(0, ch - ov_.h));
        }
        if (on_moved) on_moved();
        Refresh();
    }

    void OnLeftUp(wxMouseEvent &evt) {
        if (dragging_) {
            if (HasCapture()) ReleaseMouse();
            dragging_ = false;
            drag_corner_ = Corner::kNone;
            Refresh();
        }
        evt.Skip();
    }

    void OnCaptureLost(wxMouseCaptureLostEvent &) {
        dragging_ = false;
        drag_corner_ = Corner::kNone;
        Refresh();
    }

public:
    // Fired after every drag-driven change to ov_.x/y/w/h, so the settings
    // panel's own x/y/w/h readout (if any is added later) or the dialog's
    // title could stay in sync. Currently used to keep the "dirty" hint
    // accurate.
    std::function<void()> on_moved;

private:
    OverlayDef &ov_;
    wxBitmap bitmap_src_, cached_bmp_;
    wxImage content_img_;
    int shot_w_ = 0, shot_h_ = 0;
    int native_w_ = 0, native_h_ = 0;
    int cached_dst_w_ = -1, cached_dst_h_ = -1;
    bool dragging_ = false;
    Corner drag_corner_ = Corner::kNone;
    int drag_start_mx_ = 0, drag_start_my_ = 0;
    int drag_start_x_ = 0, drag_start_y_ = 0, drag_start_w_ = 0, drag_start_h_ = 0;
};

// Same small layout helpers settings_dialog.cpp uses (AddLabel/AddCheck),
// duplicated locally rather than shared across translation units for a
// couple of one-line helpers.
wxStaticText *AddLabel(wxWindow *page, wxSizer *sizer, wxColour text, wxColour bg, const wxString &s) {
    auto *lbl = new wxStaticText(page, wxID_ANY, s);
    lbl->SetForegroundColour(text);
    lbl->SetBackgroundColour(bg);
    sizer->Add(lbl, 0, wxALIGN_CENTRE_VERTICAL);
    return lbl;
}

wxStaticText *AddSectionHeading(wxWindow *page, wxSizer *sizer, wxColour accent, wxColour bg, const wxString &s) {
    auto *lbl = new wxStaticText(page, wxID_ANY, s);
    wxFont f = lbl->GetFont();
    f.SetWeight(wxFONTWEIGHT_BOLD);
    lbl->SetFont(f);
    lbl->SetForegroundColour(accent);
    lbl->SetBackgroundColour(bg);
    sizer->Add(lbl, 0, wxTOP | wxBOTTOM, 6);
    return lbl;
}

std::string PathBaseName(const std::string &path) {
    if (path.empty()) return "(none selected)";
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

bool ShowOverlayEditorDialog(wxWindow *parent, AppState &state, size_t idx,
                              RecordingController *rec, const ThemeColors &theme) {
    if (!rec || idx >= state.overlays.size()) return false;

    // Local working copy - nothing in state.overlays is touched unless
    // Save is clicked, same "drag/edit freely, commit on demand" contract
    // ShowOverlayPlacementDialog() uses.
    OverlayDef working = state.overlays[idx];

    wxColour bg = FromColorref(theme.bg);
    wxColour text = FromColorref(theme.text);
    wxColour text2 = FromColorref(theme.text_secondary);
    wxColour accent = FromColorref(theme.accent);
    wxColour surface = FromColorref(theme.surface);

    wxString title = "Edit Overlay \u2013 " +
        wxString::FromUTF8((working.name.empty() ? working.type : working.name).c_str());
    wxDialog dlg(parent, wxID_ANY, title, wxDefaultPosition, wxSize(980, 800),
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    dlg.SetBackgroundColour(bg);

    auto *root = new wxBoxSizer(wxVERTICAL);

    // ---- Position (top) --------------------------------------------
    AddSectionHeading(&dlg, root, accent, bg, "Position");
    root->AddSpacer(-2);
    auto *hint = new wxStaticText(&dlg, wxID_ANY,
        "This is the overlay itself, shown over a screenshot \u2013 drag it to move, "
        "drag a corner handle to resize. Refresh re-takes the screenshot.");
    hint->SetForegroundColour(text2);
    hint->SetBackgroundColour(bg);
    hint->Wrap(940);
    root->Add(hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

    auto *canvas = new SingleOverlayCanvas(&dlg, working);
    canvas->SetMinSize(wxSize(400, 260));
    root->Add(canvas, 1, wxEXPAND | wxLEFT | wxRIGHT, 4);

    auto *refreshRow = new wxBoxSizer(wxHORIZONTAL);
    auto *refreshBtn = new wxButton(&dlg, wxID_ANY, "Refresh Screenshot");
    refreshRow->Add(refreshBtn, 0, wxTOP | wxBOTTOM, 6);
    root->Add(refreshRow, 0, wxLEFT, 4);

    root->Add(new wxStaticLine(&dlg), 0, wxEXPAND | wxTOP | wxBOTTOM, 8);

    // ---- Settings (bottom) - same field set EditParametersAt() used to
    // ask for one popup at a time, now laid out together with the same
    // themed controls settings_dialog.cpp's other tabs use. ------------
    AddSectionHeading(&dlg, root, accent, bg, "Overlay Settings");

    auto *settingsScroll = new wxScrolledWindow(&dlg);
    settingsScroll->SetBackgroundColour(bg);
    settingsScroll->SetScrollRate(0, 12);
    auto *grid = new wxFlexGridSizer(2, 8, 12);
    grid->AddGrowableCol(1, 1);

    AddLabel(settingsScroll, grid, text, bg, "Name:");
    auto *nameCtrl = new wxTextCtrl(settingsScroll, wxID_ANY, wxString::FromUTF8(working.name.c_str()));
    nameCtrl->SetToolTip("Leave blank to use the default auto-generated label.");
    grid->Add(nameCtrl, 1, wxEXPAND);

    AddLabel(settingsScroll, grid, text, bg, "Visible:");
    auto *visibleChk = new wxCheckBox(settingsScroll, wxID_ANY, "Show this overlay while recording");
    visibleChk->SetValue(working.visible);
    visibleChk->SetForegroundColour(text);
    visibleChk->SetBackgroundColour(bg);
    grid->Add(visibleChk, 0);

    AddLabel(settingsScroll, grid, text, bg, "Opacity:");
    auto *opacitySlider = new LabeledSlider(settingsScroll, wxID_ANY, working.opacity, 0, 100);
    opacitySlider->SetTheme(FromColorref(theme.surface_light), accent, FromColorref(theme.fg),
                             surface, text);
    grid->Add(opacitySlider, 1, wxEXPAND);

    // Type-specific fields.
    wxTextCtrl *textCtrl = nullptr;
    wxChoice *fontChoice = nullptr;
    wxColourPickerCtrl *colorPicker = nullptr;
    wxStaticText *imagePathLbl = nullptr;
    wxButton *imageBrowseBtn = nullptr;
    wxStaticText *jsonPathLbl = nullptr, *pngPathLbl = nullptr;
    wxButton *jsonBrowseBtn = nullptr, *pngBrowseBtn = nullptr;
    wxChoice *webcamChoice = nullptr;
    std::vector<HrWebcamDevice> webcamDevices;

    if (working.type == "text") {
        AddLabel(settingsScroll, grid, text, bg, "Text:");
        textCtrl = new wxTextCtrl(settingsScroll, wxID_ANY, wxString::FromUTF8(working.text.c_str()),
                                   wxDefaultPosition, wxSize(-1, 60), wxTE_MULTILINE);
        grid->Add(textCtrl, 1, wxEXPAND);

        AddLabel(settingsScroll, grid, text, bg, "Font:");
        static const wxString kFonts[] = { "Segoe UI", "Open Sans", "Roboto" };
        fontChoice = new wxChoice(settingsScroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, 3, kFonts);
        int curFont = 0;
        for (int i = 0; i < 3; ++i) if (kFonts[i].ToStdString() == working.font_family) curFont = i;
        fontChoice->SetSelection(curFont);
        grid->Add(fontChoice, 0);

        AddLabel(settingsScroll, grid, text, bg, "Color:");
        colorPicker = new wxColourPickerCtrl(settingsScroll, wxID_ANY, wxColour(255, 255, 255));
        // ParseHexColor lives in the anon namespace above (canvas file-local);
        // duplicate the tiny parse here rather than exposing it.
        {
            const std::string &hex = working.text_color;
            if (hex.size() == 7 && hex[0] == '#') {
                auto hx = [&](int pos) { return (int)strtol(hex.substr(pos, 2).c_str(), nullptr, 16); };
                colorPicker->SetColour(wxColour(hx(1), hx(3), hx(5)));
            }
        }
        grid->Add(colorPicker, 0);
    } else if (working.type == "image" || working.type == "gif") {
        AddLabel(settingsScroll, grid, text, bg, working.type == "gif" ? "GIF file:" : "Image file:");
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        imagePathLbl = new wxStaticText(settingsScroll, wxID_ANY,
                                         wxString::FromUTF8(PathBaseName(working.image_path).c_str()));
        imagePathLbl->SetForegroundColour(text2);
        imagePathLbl->SetBackgroundColour(bg);
        row->Add(imagePathLbl, 1, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
        imageBrowseBtn = new wxButton(settingsScroll, wxID_ANY, "Browse\u2026");
        row->Add(imageBrowseBtn, 0);
        grid->Add(row, 1, wxEXPAND);
    } else if (working.type == "webcam") {
        AddLabel(settingsScroll, grid, text, bg, "Device:");
        webcamDevices = HrEnumerateWebcams();
        wxArrayString choices;
        int curSel = -1;
        for (size_t i = 0; i < webcamDevices.size(); ++i) {
            choices.Add(wxString::FromUTF8(webcamDevices[i].name.c_str()));
            if (webcamDevices[i].index == working.webcam_index) curSel = (int)i;
        }
        webcamChoice = new wxChoice(settingsScroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
        if (webcamDevices.empty()) {
            webcamChoice->Append("(no webcam found)");
            webcamChoice->Disable();
        } else {
            webcamChoice->SetSelection(curSel >= 0 ? curSel : 0);
        }
        grid->Add(webcamChoice, 1, wxEXPAND);
    } else if (working.type == "input_overlay") {
        AddLabel(settingsScroll, grid, text, bg, "Layout (.json):");
        auto *jsonRow = new wxBoxSizer(wxHORIZONTAL);
        jsonPathLbl = new wxStaticText(settingsScroll, wxID_ANY,
                                        wxString::FromUTF8(PathBaseName(working.input_json_path).c_str()));
        jsonPathLbl->SetForegroundColour(text2);
        jsonPathLbl->SetBackgroundColour(bg);
        jsonRow->Add(jsonPathLbl, 1, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
        jsonBrowseBtn = new wxButton(settingsScroll, wxID_ANY, "Browse\u2026");
        jsonRow->Add(jsonBrowseBtn, 0);
        grid->Add(jsonRow, 1, wxEXPAND);

        AddLabel(settingsScroll, grid, text, bg, "Spritesheet (.png):");
        auto *pngRow = new wxBoxSizer(wxHORIZONTAL);
        pngPathLbl = new wxStaticText(settingsScroll, wxID_ANY,
                                       wxString::FromUTF8(PathBaseName(working.input_png_path).c_str()));
        pngPathLbl->SetForegroundColour(text2);
        pngPathLbl->SetBackgroundColour(bg);
        pngRow->Add(pngPathLbl, 1, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
        pngBrowseBtn = new wxButton(settingsScroll, wxID_ANY, "Browse\u2026");
        pngRow->Add(pngBrowseBtn, 0);
        grid->Add(pngRow, 1, wxEXPAND);
    }

    auto *scrollSizer = new wxBoxSizer(wxVERTICAL);
    scrollSizer->Add(grid, 1, wxEXPAND | wxALL, 4);
    settingsScroll->SetSizer(scrollSizer);
    settingsScroll->SetMinSize(wxSize(-1, 220));
    root->Add(settingsScroll, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

    root->Add(new wxStaticLine(&dlg), 0, wxEXPAND | wxTOP | wxBOTTOM, 8);

    auto *btnRow = new wxBoxSizer(wxHORIZONTAL);
    auto *cancelBtn = new wxButton(&dlg, wxID_CANCEL, "Cancel");
    auto *saveBtn   = new wxButton(&dlg, wxID_OK, "Save");
    btnRow->AddStretchSpacer(1);
    btnRow->Add(cancelBtn, 0, wxALL, 8);
    btnRow->Add(saveBtn, 0, wxALL, 8);
    root->Add(btnRow, 0, wxEXPAND);

    dlg.SetSizer(root);

    // Live-update the working copy (and therefore the canvas preview) as
    // soon as each field changes, rather than only reading them back on
    // Save - so what's drawn at the top always matches what's typed
    // below while the window is still open.
    if (textCtrl) textCtrl->Bind(wxEVT_TEXT, [&](wxCommandEvent &) {
        working.text = textCtrl->GetValue().ToUTF8().data();
        canvas->Refresh();
    });
    if (fontChoice) fontChoice->Bind(wxEVT_CHOICE, [&](wxCommandEvent &) {
        working.font_family = fontChoice->GetStringSelection().ToStdString();
        canvas->Refresh();
    });
    if (colorPicker) colorPicker->Bind(wxEVT_COLOURPICKER_CHANGED, [&](wxColourPickerEvent &) {
        wxColour c = colorPicker->GetColour();
        char buf[8];
        snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.Red(), c.Green(), c.Blue());
        working.text_color = buf;
        canvas->Refresh();
    });
    if (imageBrowseBtn) imageBrowseBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
        wxString filter = (working.type == "gif")
            ? "GIF files (*.gif)|*.gif"
            : "Image files (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp|All files|*.*";
        wxFileDialog fd(&dlg, working.type == "gif" ? "Change GIF" : "Change Image",
                         "", "", filter, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (fd.ShowModal() != wxID_OK) return;
        working.image_path = fd.GetPath().ToUTF8().data();
        imagePathLbl->SetLabel(wxString::FromUTF8(PathBaseName(working.image_path).c_str()));
        canvas->ReloadContentImage();
    });
    if (webcamChoice && !webcamDevices.empty()) webcamChoice->Bind(wxEVT_CHOICE, [&](wxCommandEvent &) {
        int sel = webcamChoice->GetSelection();
        if (sel >= 0 && (size_t)sel < webcamDevices.size()) {
            working.webcam_index = webcamDevices[(size_t)sel].index;
            working.webcam_name  = webcamDevices[(size_t)sel].name;
        }
        canvas->Refresh();
    });
    if (jsonBrowseBtn) jsonBrowseBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
        wxFileDialog fd(&dlg, "Choose the .json layout", "", "",
                         "Overlay layout (*.json)|*.json", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (fd.ShowModal() != wxID_OK) return;
        std::string path = fd.GetPath().ToUTF8().data();
        HrInputOverlayLayout layout;
        if (!layout.Load(path)) {
            wxMessageBox("That .json file couldn't be read as an input-overlay layout. "
                         "Make sure it's the plain layout file, not something else.",
                         "HomRec", wxOK | wxICON_WARNING, &dlg);
            return;
        }
        working.input_json_path = path;
        jsonPathLbl->SetLabel(wxString::FromUTF8(PathBaseName(path).c_str()));
    });
    if (pngBrowseBtn) pngBrowseBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
        wxFileDialog fd(&dlg, "Choose the .png spritesheet", "", "",
                         "Spritesheet image (*.png)|*.png", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (fd.ShowModal() != wxID_OK) return;
        working.input_png_path = fd.GetPath().ToUTF8().data();
        pngPathLbl->SetLabel(wxString::FromUTF8(PathBaseName(working.input_png_path).c_str()));
        canvas->ReloadContentImage();
    });
    visibleChk->Bind(wxEVT_CHECKBOX, [&](wxCommandEvent &) {
        working.visible = visibleChk->GetValue();
        canvas->Refresh();
    });
    opacitySlider->Bind(wxEVT_SLIDER, [&](wxCommandEvent &) {
        working.opacity = opacitySlider->GetValue();
        canvas->Refresh();
    });

    // Same temporary-preview-pipeline screenshot call
    // ShowOverlayPlacementDialog() uses (see recording_controller.h's
    // CaptureSnapshotFrame/EndSnapshotEditing).
    auto takeScreenshot = [&](bool first_call) -> bool {
        std::vector<uint8_t> buf;
        int w = 0, h = 0, native_w = 0, native_h = 0;
        if (rec->CaptureSnapshotFrame(buf, w, h, native_w, native_h, first_call)) {
            canvas->SetScreenshot(buf, w, h);
            canvas->SetNativeSize(native_w, native_h);
            return true;
        }
        return false;
    };

    canvas->ReloadContentImage();
    if (!takeScreenshot(/*first_call=*/true)) {
        wxMessageBox("Couldn't capture a screenshot to edit this overlay against - try Refresh Screenshot in a moment.",
                     "HomRec", wxOK | wxICON_WARNING, &dlg);
    }
    refreshBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { takeScreenshot(/*first_call=*/false); });

    int result = dlg.ShowModal();
    rec->EndSnapshotEditing();

    if (result == wxID_OK) {
        working.name = nameCtrl->GetValue().ToUTF8().data();
        state.overlays[idx] = working;
        // Bypasses OverlaysDockPanel::Refresh() (the panel's usual
        // auto-persist point - see its own comment), so save it here too,
        // exactly like ShowOverlayPlacementDialog() does for the same
        // reason: otherwise this would only stick for the rest of the
        // session and be gone again on next launch.
        HrcConfig::SaveOverlaysOnly(state.overlays, HrcConfig::kOverlaysAutosavePath);
        return true;
    }
    return false;
}
