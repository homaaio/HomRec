#pragma once
// overlay_render.h - Bakes configured overlays (text / image / gif / webcam) directly into captured frames.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "input_overlay.h"
#include "webcam_capture.h"
#include "gpu_overlay.h"     // HrGpuLayer (overlay layers blended on the GPU)

struct HrOverlayDesc {
    char type[16];        // "text" | "image" | "gif" | "webcam" | "input_overlay"
    int  x, y, w, h;       // position/size in capture-resolution pixels
    char text[256];        // for type == "text"
    unsigned char text_r = 255, text_g = 255, text_b = 255; // for type == "text"
    char font_name[64] = "Segoe UI"; // for type == "text" - see RenderTextBgra()
    char image_path[260];  // for type == "image" or "gif"
    int  visible;           // 0/1
    // 0-255, applies on top of whatever per-pixel alpha the rendered layer already has (text
    // coverage, an image's own alpha channel, etc.) - see CompositeBgra().
    unsigned char opacity = 255;

    // For type == "webcam" -- see webcam_enum.h for how these get set (device picker) and
    // webcam_capture.h for how they're used to open the actual device.
    char webcam_name[128];
    int  webcam_index;

    // For type == "input_overlay" -- see input_overlay.h.
    char input_json_path[260];
    char input_png_path[260];

    // 8th pass (Desktop split) - layers that show a piece of the screen or a window:
    // "region"         src_x/src_y/src_w/src_h = the rectangle cut out of the monitor picture
    // (monitor-relative pixels), drawn scaled into x/y/w/h.
    int       src_x = 0, src_y = 0, src_w = 0, src_h = 0;
    int       win_method = 0;       // 0 = cut out of the monitor picture, 1 = the window itself
    int       win_client_only = 0;  // win_method 1: content area only
    int       win_fps_cap = 0;      // win_method 1: limit of exact captures per second (0 = none)
    long long hwnd = 0;             // win_method 1: the window to capture (0 = not found right now)
};

// True when compositing this overlay can change a single pixel of the frame.
inline bool HrOverlayDrawsAnything(const HrOverlayDesc &ov) {
    return ov.visible && ov.w > 0 && ov.h > 0 && ov.opacity > 0;
}

// True when this overlay reads (or blanks) the captured monitor picture itself, which only exists as BGRA
// on the CPU path - the GPU colour-conversion path cannot be used while such an overlay is drawn.
inline bool HrOverlayNeedsBgraBase(const HrOverlayDesc &ov) {
    if (!HrOverlayDrawsAnything(ov)) return false;
    return std::strcmp(ov.type, "region") == 0 || std::strcmp(ov.type, "window") == 0 ||
           std::strcmp(ov.type, "desktop_hidden") == 0;
}

// Renders + caches overlay content and composites it onto captured frames.
// Not thread-safe on its own -- the capture pipeline only ever touches it
// from the single capture thread, which is all it needs.
class OverlayCompositor {
public:
    // base_bgra: capture_w x capture_h, 4 bytes/pixel (B,G,R,A), row pitch
    // base_stride bytes (usually capture_w*4). Composites every visible
    // overlay in `overlays` directly onto it, clipped to its bounds.
    void Apply(uint8_t *base_bgra, int base_w, int base_h, int base_stride,
                const std::vector<HrOverlayDesc> &overlays, double current_fps = 0.0,
                const uint8_t *source_bgra = nullptr);

    // Same overlays, same cached layers - but blended straight into an NV12 frame (w x h,
    // tightly packed: w*h luma bytes then w*h/2 interleaved CbCr).
    void ApplyNv12(uint8_t *nv12, int w, int h,
                   const std::vector<HrOverlayDesc> &overlays, double current_fps = 0.0);

    // The same layers ApplyNv12() would draw, but NOT drawn - handed out so the GPU can blend them.
    // `out` receives one entry per drawn layer (bottom to top), in OVERLAY space (dst_x/dst_y = the overlay's
    // x/y, dst_w/dst_h = the bitmap size); the caller maps them to the space of the texture they go onto.
    // `bgra` points into this object's caches and stays valid only until the next call on this object.
    // Layers that need the monitor picture itself (region / window / hidden desktop) are NOT handled here -
    // callers keep those on the CPU path (see HrOverlayNeedsBgraBase()).
    void CollectGpuLayers(const std::vector<HrOverlayDesc> &overlays, double current_fps,
                          std::vector<HrGpuLayer> &out);

private:
    struct CachedLayer;   // defined below; ForEachLayer() hands them to a callback
    // Shared by Apply()/ApplyNv12(): prunes stale caches, skips overlays that
    // draw nothing, renders (or fetches from cache) each remaining overlay's
    // layer - with the per-overlay exception guard - and calls `draw` with it.
    void ForEachLayer(const std::vector<HrOverlayDesc> &overlays, double current_fps,
                      const std::function<void(const CachedLayer &, const HrOverlayDesc &)> &draw);

    double current_fps_ = 0.0;
    struct CachedLayer {
        std::vector<uint8_t> bgra; // w*h*4
        int w = 0, h = 0;
        std::string key;           // what this was rendered from, to detect staleness
        // Set from layer_gen_ every time `bgra` is (re)written, so the GPU copy knows when to re-upload.
        unsigned long long gen = 0;
    };
    unsigned long long layer_gen_ = 0;   // source of CachedLayer::gen (never reused)

    // Snapshot of the HrOverlayDesc fields that affect text/image rendering, used to skip the
    // (fairly cheap but non-zero) key-string rebuild in GetOrRenderText/GetOrRenderImage on
    // every single frame.
    struct TextImageSnapshot {
        bool  valid = false;
        char  type[16] = {0};     // "text" | "image" -- guards against the cache
                                   // key at this idx belonging to a different overlay type if the user changes an overlay's
                                   // type at runtime (same idx, otherwise-matching leftover fields).
        int   w = 0, h = 0;
        unsigned char text_r = 0, text_g = 0, text_b = 0;
        char  text[256] = {0};
        char  font_name[64] = {0};
        char  image_path[260] = {0};
    };
    std::unordered_map<size_t, TextImageSnapshot> snapshots_;
    // Keyed by the overlay's position in the list (stable enough for our
    // purposes -- overlays are edited in place, not frequently reordered).
    std::unordered_map<size_t, CachedLayer> cache_;

    struct InputOverlayCache {
        std::string json_path, png_path;
        HrInputOverlayLayout layout;
        std::vector<uint8_t> sheet_bgra; // sheet_w * sheet_h * 4, native resolution
        int sheet_w = 0, sheet_h = 0;
        bool valid = false;
        // Set alongside json_path/png_path whenever a load is *attempted* for that path pair
        // (regardless of outcome).
        bool attempted = false;

        // Scratch buffer for the native-resolution composite, reused frame-to-frame instead of a
        // brand-new std::vector being heap-allocated (and immediately freed) on every single frame
        // for as long as the overlay is on screen.
        std::vector<uint8_t> native_scratch;

        // --- per-frame work avoidance (see GetOrRenderInputOverlay) --- Virtual-key of every
        // layout element, resolved ONCE when the layout is loaded (0 = not key-driven).
        std::vector<unsigned> vks;
        // Which elements were pressed when `cache_[idx]` was last rebuilt.
        std::vector<uint8_t> last_pressed;
        std::vector<uint8_t> pressed_scratch;   // this frame's key state (reused, no per-frame allocation)
        std::string layer_key;       // cache_[idx].key this state belongs to
        int  last_ov_w = 0, last_ov_h = 0;
        bool layer_valid = false;
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

    struct GifFrameCache {
        std::vector<uint8_t> bgra; // canvas_w * canvas_h * 4
        int delay_ms = 100;
    };
    struct GifSourceCache {
        std::string path;
        std::vector<GifFrameCache> frames;
        int canvas_w = 0, canvas_h = 0;
        long long total_duration_ms = 0;
        bool valid = false;
        bool attempted = false;
        // Wall-clock anchor for "what frame is due right now" - set once when the GIF is
        // (re)loaded, not per Apply() call, so the loop plays at its authored speed no matter how
        // often frames get composited.
        std::chrono::steady_clock::time_point start_time;
    };
    std::unordered_map<size_t, GifSourceCache> gif_cache_;

    // One live capture session for a "webcam" overlay -- see webcam_capture.h.
    struct WebcamSourceCache {
        std::string device_name;
        int device_index = -1;
        HrWebcamCapture *capture = nullptr;
        CachedLayer layer;
        bool have_layer = false;
        bool warned_dead = false; // log "camera unavailable" once, not every frame
        WebcamSourceCache() = default;
        WebcamSourceCache(const WebcamSourceCache &) = delete;
        WebcamSourceCache &operator=(const WebcamSourceCache &) = delete;
        ~WebcamSourceCache();
    };
    std::unordered_map<size_t, WebcamSourceCache> webcam_cache_;

    // 8th pass: "window" layers that capture the window itself (win_method 1) keep one wgc_capture.h
    // source each, plus the last scaled picture (kept while the window has no new frame).
    struct WindowSourceCache {
        void *handle = nullptr;
        long long hwnd = 0;
        int canvas_w = 0, canvas_h = 0;
        CachedLayer layer;
        bool have_layer = false;
        std::vector<uint8_t> frame;
        std::chrono::steady_clock::time_point retry_at{};
        bool warned = false;
        WindowSourceCache() = default;
        WindowSourceCache(const WindowSourceCache &) = delete;
        WindowSourceCache &operator=(const WindowSourceCache &) = delete;
        ~WindowSourceCache();
    };
    std::unordered_map<size_t, WindowSourceCache> window_cache_;

    // The monitor picture the region / window layers read (set at the start of Apply()).
    const uint8_t *src_ptr_ = nullptr;
    int src_w_ = 0, src_h_ = 0;
    std::vector<uint8_t> src_copy_;   // used when the caller gave no pristine picture
    const CachedLayer *GetOrRenderRegion(size_t idx, const HrOverlayDesc &ov);
    const CachedLayer *GetOrRenderWindow(size_t idx, const HrOverlayDesc &ov);
    const CachedLayer *CropFromSource(size_t idx, const HrOverlayDesc &ov);

    // Drops every entry in the six idx-keyed caches above whose idx is no
    // longer in range for `overlay_count` -- called at the top of Apply()
    void PruneStaleCaches(size_t overlay_count);

    const CachedLayer *GetOrRenderText(size_t idx, const HrOverlayDesc &ov);
    const CachedLayer *GetOrRenderImage(size_t idx, const HrOverlayDesc &ov);
    const CachedLayer *GetOrRenderGif(size_t idx, const HrOverlayDesc &ov);
    const CachedLayer *GetOrRenderWebcam(size_t idx, const HrOverlayDesc &ov);
    const CachedLayer *GetOrRenderInputOverlay(size_t idx, const HrOverlayDesc &ov);
};
