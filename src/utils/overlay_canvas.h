// overlay_canvas.h - item 8 (ddagrab + HomRec overlays). Portable, header-only.
//
// With ddagrab ffmpeg grabs the screen by itself, so HomRec cannot draw into its frames. Instead HomRec draws every
// overlay layer into a TRANSPARENT full-frame BGRA picture (this file) and hands that picture to ffmpeg as a second
// rawvideo input; ffmpeg's `overlay` filter blends it onto the grabbed frame.
//
// The canvas keeps STRAIGHT (non-premultiplied) alpha, which is what ffmpeg's `bgra` format and `overlay` expect.
// A layer is the same cached bitmap OverlayCompositor hands to the GPU path (HrGpuLayer, gpu_overlay.h): straight
// alpha, top-left at dst_x / dst_y, `opacity` 0..255 multiplied with the bitmap's own alpha.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

struct HrCanvasLayer {          // the fields of HrGpuLayer the canvas needs (kept separate so this header stands alone)
    const uint8_t *bgra;        // w * h * 4, straight alpha
    int            w, h;
    int            dst_x, dst_y;
    int            dst_w, dst_h;   // drawn size; != w/h = nearest-neighbour scaled
    int            opacity;     // 0..255
};

// Clears the canvas to fully transparent.
inline void hr_canvas_clear(uint8_t *canvas, int cw, int ch) {
    if (canvas && cw > 0 && ch > 0) std::memset(canvas, 0, (size_t)cw * (size_t)ch * 4);
}

// "source over destination" of one layer onto a straight-alpha BGRA canvas (cw x ch, tightly packed).
// Clips to the canvas; a layer that is fully outside draws nothing.
inline void hr_canvas_blend(uint8_t *canvas, int cw, int ch, const HrCanvasLayer &L) {
    if (!canvas || !L.bgra || cw <= 0 || ch <= 0 || L.w <= 0 || L.h <= 0) return;
    const int dw = L.dst_w > 0 ? L.dst_w : L.w;
    const int dh = L.dst_h > 0 ? L.dst_h : L.h;
    const int op = L.opacity < 0 ? 0 : (L.opacity > 255 ? 255 : L.opacity);
    if (op == 0) return;
    const int x0 = L.dst_x < 0 ? 0 : L.dst_x;
    const int y0 = L.dst_y < 0 ? 0 : L.dst_y;
    const int x1 = (L.dst_x + dw) > cw ? cw : (L.dst_x + dw);
    const int y1 = (L.dst_y + dh) > ch ? ch : (L.dst_y + dh);
    if (x0 >= x1 || y0 >= y1) return;
    const bool scaled = (dw != L.w || dh != L.h);
    for (int y = y0; y < y1; ++y) {
        const int ly = scaled ? (int)(((long long)(y - L.dst_y) * L.h) / dh) : (y - L.dst_y);
        const uint8_t *srow = L.bgra + (size_t)ly * (size_t)L.w * 4;
        uint8_t *drow = canvas + ((size_t)y * (size_t)cw + (size_t)x0) * 4;
        for (int x = x0; x < x1; ++x, drow += 4) {
            const int lx = scaled ? (int)(((long long)(x - L.dst_x) * L.w) / dw) : (x - L.dst_x);
            const uint8_t *s = srow + (size_t)lx * 4;
            const int sa = (s[3] * op + 127) / 255;         // source alpha after the layer opacity
            if (sa == 0) continue;
            const int da = drow[3];
            if (sa == 255 || da == 0) {                     // opaque source, or nothing underneath: just take it
                drow[0] = s[0]; drow[1] = s[1]; drow[2] = s[2]; drow[3] = (uint8_t)sa;
                continue;
            }
            // out_a = sa + da*(1-sa);  out_c = (sc*sa + dc*da*(1-sa)) / out_a   (all in 0..255 integer maths)
            const int inv = 255 - sa;
            const int dpart = (da * inv + 127) / 255;       // destination's remaining coverage
            const int oa = sa + dpart;                      // > 0 because sa > 0
            for (int c = 0; c < 3; ++c)
                drow[c] = (uint8_t)((s[c] * sa + drow[c] * dpart + oa / 2) / oa);
            drow[3] = (uint8_t)(oa > 255 ? 255 : oa);
        }
    }
}
