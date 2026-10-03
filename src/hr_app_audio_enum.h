// hr_app_audio_enum.h - finds programs the Audio Mixer can capture the sound of (2.4).
//
// "Browser" and "Window audio" sources use WASAPI per-process loopback: Windows hands
// back only the sound produced by one program (and the processes it started), so
// a browser's music can be recorded without the rest of the desktop audio. The
// functions here only locate the programs; hr_audio.cpp does the capturing.
#pragma once
#include <string>
#include <vector>

struct HrAudioApp {
    unsigned long pid      = 0;   // process owning the window that was found
    unsigned long root_pid = 0;   // top of its process tree - what gets captured (browsers spawn many children)
    std::string   exe;            // "chrome.exe"
    std::string   title;          // window title (UTF-8)
    bool          browser = false;
};

// Visible top-level windows with a title (HomRec's own excluded). When
// browsers_only is set, only known browsers are returned, one entry per browser.
std::vector<HrAudioApp> HrEnumAudioApps(bool browsers_only);

// Re-finds a saved source after a restart: first by window title, then by
// executable name. Returns 0 when the program isn't running.
unsigned long HrResolveAudioAppPid(const std::string &exe, const std::string &window_title);

// Per-process loopback needs Windows 10 version 2004 (build 19041) or newer.
bool HrProcessLoopbackSupported();
