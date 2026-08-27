#pragma once
// -----------------------------------------------------------------------------
// hr_overlay_render.h
//
// Bakes configured overlays (text / image; webcam is not yet supported -- see
// the warning in hr_overlay_render.cpp) directly into captured frames.
//
// Previously overlays only existed as UI state (src/ui/app_state.h's
// AppState::overlays, edited via the Overlay Manager dialog / drag-on-preview)
// and were never actually composited into anything -- recording "with an
// overlay" produced a plain screen recording with no overlay in it at all.
// This closes that gap: RecordingController pushes the current overlay list
// into the pipeline (hr_pl_set_overlays), and the capture thread calls
// OverlayCompositor::Apply() on every captured frame before it's handed off
// for YUV conversion/encoding.
//
// Deliberately POD (fixed-size char buffers, no std::string/std::vector in
// the public struct) so it stays consistent with the rest of this file's
// extern "C" boundary between the UI layer and the capture pipeline.
// -----------------------------------------------------------------------------
#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>
#include "hr_input_overlay.h"

struct HrOverlayDesc {
    char type[16];        // "text" | "image" | "webcam" | "input_overlay"
    int  x, y, w, h;       // position/size in capture-resolution pixels
    char text[256];        // for type == "text"
    unsigned char text_r = 255, text_g = 255, text_b = 255; // for type == "text"
    char image_path[260];  // for type == "image"
    int  visible;           // 0/1

    // For type == "input_overlay" -- see hr_input_overlay.h. The JSON
    // layout is parsed (and its parse result cached) inside
    // OverlayCompositor rather than here, to keep this struct POD/fixed-
    // size like the rest of it.
    char input_json_path[260];
    char input_png_path[260];
};

// Renders + caches overlay content and composites it onto captured frames.
// Not thread-safe on its own -- the capture pipeline only ever touches it
// from the single capture thread, which is all it needs.
class OverlayCompositor {
public:
    // base_bgra: capture_w x capture_h, 4 bytes/pixel (B,G,R,A), row pitch
    // base_stride bytes (usually capture_w*4). Composites every visible
    // overlay in `overlays` directly onto it, clipped to its bounds.
    void Apply(uint8_t *base_bgra, int base_w, int base_h, int base_stride,
               const std::vector<HrOverlayDesc> &overlays);

private:
    struct CachedLayer {
        std::vector<uint8_t> bgra; // w*h*4
        int w = 0, h = 0;
        std::string key;           // what this was rendered from, to detect staleness
    };

    // Snapshot of the HrOverlayDesc fields that affect text/image rendering,
    // used to skip the (fairly cheap but non-zero) key-string rebuild in
    // GetOrRenderText/GetOrRenderImage on every single frame. Comparing a
    // few ints/bytes directly is far cheaper than building a std::string
    // with several std::to_string() calls and then comparing that -- and
    // this runs on the capture thread for every visible overlay, every
    // captured frame, for as long as the overlay is on screen, so for a
    // static text/image overlay (the overwhelming common case) that string
    // work was pure waste.
    struct TextImageSnapshot {
        bool  valid = false;
        char  type[16] = {0};     // "text" | "image" -- guards against the cache
                                   // key at this idx belonging to a different
                                   // overlay type if the user changes an
                                   // overlay's type at runtime (same idx,
                                   // otherwise-matching leftover fields).
        int   w = 0, h = 0;
        unsigned char text_r = 0, text_g = 0, text_b = 0;
        char  text[256] = {0};
        char  image_path[260] = {0};
    };
    std::unordered_map<size_t, TextImageSnapshot> snapshots_;
    // Keyed by the overlay's position in the list (stable enough for our
    // purposes -- overlays are edited in place, not frequently reordered).
    //
    // Entries here used to live
    // forever once created -- nothing ever erased them. Every add/remove/
    // resize of an overlay (especially an image or input_overlay one,
    // whose CachedLayer/InputOverlayCache/ImageSourceCache entries can each
    // hold a full decoded bitmap) grew whichever of these maps had a fresh,
    // never-before-used idx a little further, and removing overlays never
    // shrank them back down since the *count* of overlays going down
    // doesn't remove the higher idx entries already sitting in the maps.
    // Over a session with a lot of overlay editing this is a real, growing
    // leak -- see PruneStaleCaches(), called at the top of Apply() every
    // frame with the current overlay count, which is now the single place
    // responsible for keeping all four of these maps trimmed to it.
    std::unordered_map<size_t, CachedLayer> cache_;

    // Decoded spritesheet + parsed layout for input_overlay entries,
    // reloaded only when the source paths change (unlike CachedLayer
    // above, the *composited* result for these is rebuilt every frame --
    // that's the whole point, it tracks live key state -- so it isn't
    // stored in `cache_`'s staleness-checked slot the same way).
    struct InputOverlayCache {
        std::string json_path, png_path;
        HrInputOverlayLayout layout;
        std::vector<uint8_t> sheet_bgra; // sheet_w * sheet_h * 4, native resolution
        int sheet_w = 0, sheet_h = 0;
        bool valid = false;
        // Set alongside json_path/png_path whenever a load is *attempted*
        // for that path pair (regardless of outcome). Distinct from
        // `valid` so a decode/parse failure is only retried once the
        // paths actually change again, instead of every single frame
        // forever (a real image/spritesheet swap always changes at least
        // one path, so this never blocks a genuine retry).
        bool attempted = false;

        // Scratch buffer for the
        // native-resolution composite, reused frame-to-frame instead of a
        // brand-new std::vector being heap-allocated (and immediately
        // freed) on every single frame for as long as the overlay is on
        // screen. Unlike sheet_bgra/layout above, this genuinely does get
        // rewritten every frame (it tracks live key state) -- the point
        // here is only to stop re-allocating the *storage* for it each
        // time, not to skip the redraw.
        std::vector<uint8_t> native_scratch;
    };
    std::unordered_map<size_t, InputOverlayCache> input_cache_;
    struct ImageSourceCache {
        std::string path;
        std::vector<uint8_t> native_bgra; // native_w * native_h * 4
        int native_w = 0, native_h = 0;
        bool valid = false;
        bool attempted = false; // see InputOverlayCache::attempted above
    };
    std::unordered_map<size_t, ImageSourceCache> image_source_cache_;

    // Drops every entry in the four idx-keyed caches above whose idx is no
    // longer in range for `overlay_count` -- called at the top of Apply()
    // every frame so removing/reordering overlays actually frees what they
    // were holding instead of leaving it cached forever at a now-unused
    // index (see the maps' own comments for the leak this fixes).
    void PruneStaleCaches(size_t overlay_count);

    const CachedLayer *GetOrRenderText(size_t idx, const HrOverlayDesc &ov);
    const CachedLayer *GetOrRenderImage(size_t idx, const HrOverlayDesc &ov);
    const CachedLayer *GetOrRenderInputOverlay(size_t idx, const HrOverlayDesc &ov);
};
