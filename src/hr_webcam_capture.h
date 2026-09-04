#pragma once
// -----------------------------------------------------------------------------
// hr_webcam_capture.h
//
// Opens one video-input device via Media Foundation and continuously reads
// frames from it on a dedicated background thread, exposing only "whatever
// the most recent frame was" to the caller -- the same one-slot,
// always-latest-never-queued contract hr_dxgi_capture.cpp uses for screen
// capture. This is what actually backs a "webcam" overlay during recording
// (see hr_overlay_render.cpp's GetOrRenderWebcam()); before this existed, a
// webcam overlay could be positioned in the UI but never made it into the
// recording at all -- only a one-time log warning. hr_webcam_enum.h (kept
// separate, DirectShow-based) only *lists* devices for the picker; it was
// never meant to also own an open capture session.
//
// Media Foundation over DirectShow's ISampleGrabber deliberately: the
// Sample Grabber approach needs qedit.dll, which Microsoft deprecated and
// which isn't reliably registered on modern 64-bit Windows -- IMFSourceReader
// is the current, fully-supported way to pull frames from a capture device.
// -----------------------------------------------------------------------------
#include <cstdint>
#include <string>
#include <vector>

class HrWebcamCapture {
public:
    // device_name: friendly name from HrEnumerateWebcams() -- primary match
    // key. Matching by name (rather than a bare index) survives a camera
    // being unplugged/replugged into a different USB port, which can
    // shuffle enumeration order even though it's the same physical camera.
    // device_index: fallback, used when no device's name matches exactly
    // (a renamed/driver-updated camera) or to disambiguate two attached
    // devices that happen to share the same name (picks the Nth match, same
    // semantics as hr_webcam_enum.h's own index).
    //
    // Never returns nullptr (short of allocation failure) -- opening the
    // actual device happens asynchronously on the background thread, so
    // this returns immediately without stalling the capture pipeline's own
    // thread on camera driver I/O. Whether it actually found/opened
    // anything is only knowable a little later, via IsAlive()/
    // GetLatestFrame() below.
    static HrWebcamCapture *Open(const std::string &device_name, int device_index);
    ~HrWebcamCapture();

    HrWebcamCapture(const HrWebcamCapture &) = delete;
    HrWebcamCapture &operator=(const HrWebcamCapture &) = delete;

    // True + fills out_bgra/out_w/out_h with the most recently decoded frame
    // (top-down BGRA, tightly packed, stride == out_w*4) if at least one
    // has arrived since Open(); false otherwise (nothing yet -- normal for
    // the first handful of calls right after Open() -- or the device has
    // stopped delivering samples). Callers should keep showing whatever
    // frame they last got on a `false` return rather than treating it as
    // fatal; see GetOrRenderWebcam() in hr_overlay_render.cpp.
    bool GetLatestFrame(std::vector<uint8_t> &out_bgra, int &out_w, int &out_h);

    // False once the background thread has given up trying to open or read
    // from the device (device not found, in use elsewhere, disconnected
    // mid-stream, etc.) -- lets a caller that's never gotten a single frame
    // tell "still starting up" apart from "this isn't going to work" and
    // log/react accordingly instead of waiting forever.
    bool IsAlive() const;

private:
    HrWebcamCapture();
    struct Impl;
    Impl *impl_;

    // Defined in the .cpp; needs access to the private Impl type since it
    // runs as a plain std::thread entry point rather than a member
    // function (std::thread doesn't need `this` to be a HrWebcamCapture,
    // just Impl -- keeping the whole capture/decode loop out of the class
    // body keeps this header a plain, stable public interface).
    friend void HrWebcamCaptureThreadMain(Impl *impl);
};
