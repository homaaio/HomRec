// str_convert.h - Tiny header-only string<->value converters shared by hrc_config.cpp
// (the .hrc file reader/writer) and settings_registry.cpp (the console
// "<key> = <value>" / homrec.get_setting()/set_setting() backing table).
#pragma once

#include "../ui/app_state.h"
#include <string>
#include <cctype>
#include <cstdint>

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

inline std::string HrCaptureModeToStr(CaptureMode m) {
    if (m == CaptureMode::Window) return "window";
    if (m == CaptureMode::Region) return "region";
    return "desktop";
}
inline CaptureMode HrCaptureModeFromStr(const std::string &s) {
    if (s == "window") return CaptureMode::Window;
    if (s == "region") return CaptureMode::Region;
    return CaptureMode::Desktop;
}

inline std::string HrVideoFormatToStr(VideoFormat f) {
    if (f == VideoFormat::Mkv)  return "mkv";
    if (f == VideoFormat::Both) return "both";
    return "mp4";
}
inline VideoFormat HrVideoFormatFromStr(const std::string &s) {
    if (s == "mkv")  return VideoFormat::Mkv;
    if (s == "both" || s == "mp4+mkv" || s == "mkv+mp4") return VideoFormat::Both;
    return VideoFormat::Mp4;
}
// File extension the encoder writes to (Both records to .mkv and remuxes an .mp4 copy).
inline const char *HrVideoFormatExt(VideoFormat f) { return f == VideoFormat::Mp4 ? "mp4" : "mkv"; }

inline std::string HrPostRecordHookTypeToStr(AppState::PostRecordHookType t) {
    switch (t) {
        case AppState::PostRecordHookType::Script:     return "script";
        case AppState::PostRecordHookType::Move:       return "move";
        case AppState::PostRecordHookType::OpenFolder: return "open_folder";
        default:                                       return "none";
    }
}
inline AppState::PostRecordHookType HrPostRecordHookTypeFromStr(const std::string &s) {
    if (s == "script")      return AppState::PostRecordHookType::Script;
    if (s == "move")        return AppState::PostRecordHookType::Move;
    if (s == "open_folder") return AppState::PostRecordHookType::OpenFolder;
    return AppState::PostRecordHookType::None;
}

inline std::string HrResolutionModeToStr(ResolutionMode m) { return m == ResolutionMode::Absolute ? "1" : "0"; }
inline ResolutionMode HrResolutionModeFromStr(const std::string &s) {
    return atoi(s.c_str()) != 0 ? ResolutionMode::Absolute : ResolutionMode::Percent;
}

// ---- UTF-8 <-> wstring (same result on Windows (UTF-16) and Linux (UTF-32)) ----------------------
// AppState keeps every path as UTF-8; the Windows-era APIs (HrcConfig, HrLogPaths) take std::wstring.
inline std::wstring HrWide(const std::string &s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0xFFFD;
        size_t n = 1;
        if (c < 0x80) { cp = c; }
        else if ((c >> 5) == 0x6 && i + 1 < s.size())  { cp = ((c & 0x1F) << 6)  | (s[i + 1] & 0x3F); n = 2; }
        else if ((c >> 4) == 0xE && i + 2 < s.size())  { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); n = 3; }
        else if ((c >> 3) == 0x1E && i + 3 < s.size()) { cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); n = 4; }
        i += n;
        if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
            cp -= 0x10000;
            out += (wchar_t)(0xD800 + (cp >> 10));
            out += (wchar_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (wchar_t)cp;
        }
    }
    return out;
}

inline std::string HrNarrow(const std::wstring &w) {
    std::string out;
    out.reserve(w.size());
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t cp = (uint32_t)w[i];
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp < 0xDC00 && i + 1 < w.size()) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)w[i + 1] - 0xDC00);
            ++i;
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    return out;
}

// What std::ifstream / std::ofstream want as a file name: MinGW takes the wstring as it is,
// Linux needs the UTF-8 bytes. Use as `std::ofstream f(HrFsPath(p).c_str(), ...)`.
#ifdef _WIN32
inline const std::wstring &HrFsPath(const std::wstring &p) { return p; }
#else
inline std::string HrFsPath(const std::wstring &p) { return HrNarrow(p); }
#endif
