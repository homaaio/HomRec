// webcam_capture.h - opens a video input device (Media Foundation) and keeps only the latest frame.
// Backs the "webcam" overlay during recording.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class HrWebcamCapture {
public:
    // device_name: friendly name from HrEnumerateWebcams() -- primary match key.
    static HrWebcamCapture *Open(const std::string &device_name, int device_index);
    ~HrWebcamCapture();

    HrWebcamCapture(const HrWebcamCapture &) = delete;
    HrWebcamCapture &operator=(const HrWebcamCapture &) = delete;

    bool GetLatestFrame(std::vector<uint8_t> &out_bgra, int &out_w, int &out_h);

    bool IsAlive() const;

private:
    HrWebcamCapture();
    struct Impl;
    Impl *impl_;

    friend void HrWebcamCaptureThreadMain(Impl *impl);
};
