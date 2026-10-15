// overlay_geometry.h - the arithmetic of dragging / resizing an overlay (7th pass).
//
// Shared by the live preview (main_frame.cpp's PreviewPanel) and the "Edit Overlay" window's canvas
// (overlay_editor_dialog.cpp) so both behave identically, and kept free of wx / Win32 so it can be
// tested on its own. All values are in CANVAS pixels (the coordinate space of OverlayDef::x/y/w/h).
//
//   * Keep proportions: holding Shift while resizing keeps the overlay's aspect ratio (a 500x500 picture
//     can no longer be stretched to 1000x200). An overlay with "Keep proportions" ticked does that all the
//     time, and Shift then releases it (KeepAspect() = lock XOR shift).
//   * Resizing never pushes an overlay out of the picture (it used to: only moving was clamped, so a corner
//     could be dragged far past the edge and the overlay "slid under" the preview's border).
//   * Center snap: dragging an overlay close to the middle of the picture (per axis) locks it exactly
//     centred; hold Alt to move freely.
#pragma once
#include <algorithm>
#include <cmath>

namespace HrOverlayGeom {

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

constexpr int kMinSize = 10;

inline bool KeepAspect(bool overlay_lock_aspect, bool shift_down) { return overlay_lock_aspect != shift_down; }

// Scale factor limits for a proportional resize of a w x h overlay that has to stay >= kMinSize and fit
// into max_w x max_h.
inline void ScaleLimits(int w, int h, int max_w, int max_h, double &lo, double &hi) {
    lo = std::max((double)kMinSize / std::max(1, w), (double)kMinSize / std::max(1, h));
    hi = std::min((double)max_w / std::max(1, w), (double)max_h / std::max(1, h));
    if (hi < lo) hi = lo;       // canvas smaller than the minimum size: never go below the minimum
}

// Bottom-right handle: top-left corner stays, the opposite corner follows the mouse (dx/dy = mouse delta
// since the drag started). `s` is the overlay as it was when the drag started.
inline Rect ResizeBottomRight(const Rect &s, int dx, int dy, int cw, int ch, bool keep_aspect) {
    const int max_w = std::max(kMinSize, cw - s.x);
    const int max_h = std::max(kMinSize, ch - s.y);
    Rect o = s;
    if (!keep_aspect || s.w <= 0 || s.h <= 0) {
        o.w = std::clamp(s.w + dx, kMinSize, max_w);
        o.h = std::clamp(s.h + dy, kMinSize, max_h);
        return o;
    }
    const double rw = (double)(s.w + dx) / s.w, rh = (double)(s.h + dy) / s.h;
    double scale = std::fabs(rw - 1.0) >= std::fabs(rh - 1.0) ? rw : rh;   // follow the axis the mouse moved more
    double lo, hi;
    ScaleLimits(s.w, s.h, max_w, max_h, lo, hi);
    scale = std::clamp(scale, lo, hi);
    o.w = std::max(kMinSize, (int)std::lround(s.w * scale));
    o.h = std::max(kMinSize, (int)std::lround(s.h * scale));
    return o;
}

// Top-left handle: the bottom-right corner stays, the top-left one follows the mouse.
inline Rect ResizeTopLeft(const Rect &s, int dx, int dy, int cw, int ch, bool keep_aspect) {
    const int anchor_right = s.x + s.w, anchor_bottom = s.y + s.h;
    const int max_w = std::max(kMinSize, std::min(anchor_right, cw));
    const int max_h = std::max(kMinSize, std::min(anchor_bottom, ch));
    int nw, nh;
    if (!keep_aspect || s.w <= 0 || s.h <= 0) {
        nw = std::clamp(s.w - dx, kMinSize, max_w);
        nh = std::clamp(s.h - dy, kMinSize, max_h);
    } else {
        const double rw = (double)(s.w - dx) / s.w, rh = (double)(s.h - dy) / s.h;
        double scale = std::fabs(rw - 1.0) >= std::fabs(rh - 1.0) ? rw : rh;
        double lo, hi;
        ScaleLimits(s.w, s.h, max_w, max_h, lo, hi);
        scale = std::clamp(scale, lo, hi);
        nw = std::max(kMinSize, (int)std::lround(s.w * scale));
        nh = std::max(kMinSize, (int)std::lround(s.h * scale));
    }
    Rect o;
    o.w = nw;
    o.h = nh;
    o.x = std::clamp(anchor_right - nw, 0, std::max(0, cw - nw));
    o.y = std::clamp(anchor_bottom - nh, 0, std::max(0, ch - nh));
    return o;
}

// Plain move: keeps the overlay inside the canvas.
inline void ClampMove(Rect &r, int cw, int ch) {
    r.x = std::clamp(r.x, 0, std::max(0, cw - r.w));
    r.y = std::clamp(r.y, 0, std::max(0, ch - r.h));
}

struct Snap { bool x = false, y = false; };

// Center snap, per axis: when the overlay's centre is within `threshold` canvas pixels of the picture's
// centre it is placed exactly in the middle. Only for overlays smaller than the canvas on that axis.
inline Snap SnapToCenter(Rect &r, int cw, int ch, int threshold) {
    Snap s;
    if (r.w < cw) {
        const int target = (cw - r.w) / 2;
        if (std::abs(r.x - target) <= threshold) { r.x = target; s.x = true; }
    }
    if (r.h < ch) {
        const int target = (ch - r.h) / 2;
        if (std::abs(r.y - target) <= threshold) { r.y = target; s.y = true; }
    }
    return s;
}

} // namespace HrOverlayGeom
