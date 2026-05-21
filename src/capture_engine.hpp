#pragma once
// capture_engine.hpp — HomRec Legacy C++ Capture/Record/Preview Engine
// Compiled into a DLL called by Python via ctypes.
// Targets Windows XP SP3+ (32-bit safe, no C++17 stdlib FS).
// Compiler: MSVC 2019 or MinGW-w64 GCC 8+
// Dependencies: none except Win32 API + libjpeg-turbo (optional) or GDI

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>        // timeBeginPeriod
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <functional>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// ---------------------- Public C API ----------------------------------------
// All functions exported with C linkage so ctypes can call them easily.
extern "C" {

// --- Screen capture ---
// Capture current screen region into caller-owned RGB24 buffer.
// Returns bytes written, or -1 on error.
__declspec(dllexport) int  HR_CaptureScreen(
    int x, int y, int w, int h,          // capture region
    uint8_t* out_rgb24,                  // caller pre-allocates w*h*3 bytes
    int out_w, int out_h                 // scale to this size (fast bilinear)
);

// --- Preview thread ---
// Start/stop a background thread that continuously captures and
// writes scaled JPEG or raw RGB frames into a shared ring buffer.
__declspec(dllexport) int  HR_PreviewStart(int x, int y, int w, int h,
                                           int prev_w, int prev_h,
                                           int fps_limit);
__declspec(dllexport) void HR_PreviewStop();
// Copy latest preview frame (RGB24, prev_w*prev_h*3 bytes) → dst.
// Returns 1 if a new frame was available, 0 otherwise.
__declspec(dllexport) int  HR_PreviewGetFrame(uint8_t* dst, int* out_w, int* out_h);

// --- Recording ---
// Record via FFmpeg subprocess started by engine.
// ffmpeg_path: full path to ffmpeg.exe (UTF-8).
// out_file:    output .mp4 path (UTF-8).
// Returns 0 on success, non-zero on error.
__declspec(dllexport) int  HR_RecordStart(
    const char* ffmpeg_path,
    const char* out_file,
    int x, int y, int w, int h,
    int fps,
    const char* codec,                   // e.g. "libx264"
    const char* preset,                  // e.g. "ultrafast"
    int crf,
    const char* pix_fmt,                 // e.g. "yuv420p"
    const char* hw_accel,                // "auto","none","cuda",...
    int capture_window,                  // 0=desktop, 1=window by title
    const char* window_title             // used only when capture_window=1
);
__declspec(dllexport) void HR_RecordStop();
__declspec(dllexport) int  HR_RecordIsRunning();
__declspec(dllexport) int  HR_RecordGetFrameCount();

// --- Audio ---
// Simple WASAPI-loopback + microphone capture to WAV.
__declspec(dllexport) int  HR_AudioStart(
    const char* out_wav_mic,            // mic output path (UTF-8)
    const char* out_wav_sys,            // system audio output path (UTF-8, can be "" to skip)
    int sample_rate,
    int channels,
    float mic_vol,                       // 0.0-1.0
    float sys_vol                        // 0.0-1.0
);
__declspec(dllexport) void HR_AudioStop();
__declspec(dllexport) float HR_AudioGetMicLevel();   // 0-100
__declspec(dllexport) float HR_AudioGetSysLevel();   // 0-100

// --- Utility ---
__declspec(dllexport) int  HR_GetVersion(char* buf, int buf_size); // writes "1.4.4 (Legacy)"
__declspec(dllexport) void HR_SetTimerResolution(int on);          // 1ms timer
__declspec(dllexport) int  HR_GetMonitorCount();
__declspec(dllexport) int  HR_GetMonitorRect(int idx, int* x, int* y, int* w, int* h);

} // extern "C"
