// audio_backend.h - capturing sound: microphones, "what you hear" (system loopback) and one
// application's sound. WASAPI on Windows; PipeWire (preferred) or PulseAudio on Linux.
//
// The sink has the SAME shape as HrAudioSinkFn in src/core/audio.cpp (interleaved 16-bit stereo),
// so the existing mixer (audio_mixer.h) and live-audio path can be fed from either platform.
#pragma once

#include "platform.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hr {
namespace platform {

enum class AudioSourceKind {
    Microphone,        // an input device
    SystemLoopback,    // everything the machine plays (a PipeWire/Pulse ".monitor" source on Linux)
    Application,       // one program's sound (PipeWire stream node; WASAPI process loopback on Windows)
};

struct AudioDeviceInfo {
    std::string     id;            // stable id: WASAPI endpoint id / PipeWire node.name / Pulse source name
    std::string     name;          // UTF-8 friendly name for the picker
    AudioSourceKind kind       = AudioSourceKind::Microphone;
    bool            is_default = false;
};

// A running program that makes sound (Linux: a PipeWire/Pulse playback stream). Mirrors HrAudioApp.
struct AudioAppInfo {
    uint64_t    id = 0;            // PipeWire node id / pid on Windows
    std::string exe;               // "firefox", "chrome.exe"
    std::string title;             // window or stream title
    bool        browser = false;
};

struct AudioCaps {
    bool        available       = false;
    bool        microphone      = false;
    bool        system_loopback = false;
    bool        per_application = false;
    std::string backend_name;      // "pipewire", "pulseaudio", "wasapi", "none"
    std::string unavailable_reason;
};

// Receives interleaved stereo int16 samples at the rate passed to Open(). Called from the backend's
// own thread - keep it short, never block.
typedef void (*AudioSinkFn)(void *user, const int16_t *stereo, size_t frames);

struct AudioSourceSpec {
    AudioSourceKind kind      = AudioSourceKind::Microphone;
    std::string     device_id;      // Microphone / SystemLoopback: AudioDeviceInfo::id ("" = default device)
    uint64_t        app_id    = 0;  // Application: AudioAppInfo::id
    int             rate_hz   = 48000;   // 44100 or 48000, like hr_audio_live_start()
};

class IAudioCapture {
public:
    virtual ~IAudioCapture() = default;

    virtual bool Open(const AudioSourceSpec &spec, std::string *err) = 0;
    virtual bool Start(AudioSinkFn sink, void *user) = 0;
    virtual void Stop() = 0;                  // idempotent; no sink call happens after it returns
    virtual void Close() = 0;

    virtual void SetPaused(bool paused) = 0;  // while paused the backend delivers nothing
    virtual int  GetLevel() const = 0;        // 0..100 peak, for the mixer meters (like hr_audio_get_levels)
    virtual bool IsAlive() const = 0;         // false once the device/stream disappeared
};

AudioCaps QueryAudioCaps();
std::vector<AudioDeviceInfo> EnumerateAudioDevices(AudioSourceKind kind, std::string *err);
std::vector<AudioAppInfo>    EnumerateAudioApps(bool browsers_only, std::string *err);
std::unique_ptr<IAudioCapture> CreateAudioCapture(std::string *why_not);

}  // namespace platform
}  // namespace hr
