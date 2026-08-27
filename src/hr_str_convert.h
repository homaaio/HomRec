// hr_str_convert.h
//
// Tiny header-only string<->value converters shared by hrc_config.cpp
// (the .hrc file reader/writer) and hr_settings_registry.cpp (the console
// "<key> = <value>" / homrec.get_setting()/set_setting() backing table).
//
// Before this file existed, hrc_config.cpp had its own private copies of
// ToBool()/FromBool() and the four enum<->string pairs below, and
// lua_api.cpp's BoolSettingByName() had its own separate hardcoded key
// list with slightly different spellings for the same fields
// (minimize_tray vs. minimize_to_tray, countdown vs. countdown_enabled,
// etc.) - two hand-maintained copies of "the same" mapping that had
// already drifted apart. Pulling the conversions in here and building
// hr_settings_registry.h's table from them once means .hrc, the console,
// and plugins all agree on both the key spelling and the value format by
// construction, not by convention.
#pragma once

#include "ui/app_state.h"
#include <string>
#include <cctype>

inline bool HrToBool(const std::string &v) { return v == "1" || v == "true" || v == "yes"; }
inline std::string HrFromBool(bool b) { return b ? "1" : "0"; }

inline std::string HrRecordingModeToStr(RecordingMode m) {
    switch (m) {
        case RecordingMode::Ultra: return "ultra";
        case RecordingMode::Turbo: return "turbo";
        case RecordingMode::Eco:   return "eco";
        default:                  return "balanced";
    }
}
inline RecordingMode HrRecordingModeFromStr(const std::string &s) {
    if (s == "ultra") return RecordingMode::Ultra;
    if (s == "turbo") return RecordingMode::Turbo;
    if (s == "eco")   return RecordingMode::Eco;
    return RecordingMode::Balanced;
}

inline std::string HrCaptureModeToStr(CaptureMode m) { return m == CaptureMode::Window ? "window" : "desktop"; }
inline CaptureMode HrCaptureModeFromStr(const std::string &s) { return s == "window" ? CaptureMode::Window : CaptureMode::Desktop; }

inline std::string HrVideoFormatToStr(VideoFormat f) { return f == VideoFormat::Mkv ? "mkv" : "mp4"; }
inline VideoFormat HrVideoFormatFromStr(const std::string &s) { return s == "mkv" ? VideoFormat::Mkv : VideoFormat::Mp4; }

// resolution_mode has always round-tripped through .hrc as a bare "0"/"1"
// (see hrc_config.cpp's original Save()/Load()), not as a word - kept
// exactly as-is here rather than "improving" it to a string, so existing
// hand-written .hrc files that already have "resolution_mode=1" in them
// don't silently stop working.
inline std::string HrResolutionModeToStr(ResolutionMode m) { return m == ResolutionMode::Absolute ? "1" : "0"; }
inline ResolutionMode HrResolutionModeFromStr(const std::string &s) {
    return atoi(s.c_str()) != 0 ? ResolutionMode::Absolute : ResolutionMode::Percent;
}
