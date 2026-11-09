// homrec_tui.h - the text side of the homrec terminal UI: colours, column fitting, the recording
// status line, the saving bar, menu lines and the ru/en texts.
//
// Pure functions only (std::string in, std::string out, no console calls, no <windows.h>), so they
// can be unit-tested on any platform (scripts/tests/test_homrec_cli.cpp). The console I/O itself
// (raw keys, redraw in place) lives in homrec.cpp.
//
// Every function takes `fancy`: true = ANSI colours and box/block characters, false = plain ASCII.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace hrcli {
namespace tui {

// ---------------------------------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------------------------------

// Wraps `s` in an ANSI colour when `color` is on, leaves it alone otherwise.
inline std::string Paint(bool color, const char *sgr, const std::string &s) {
    return color ? std::string("\x1b[") + sgr + "m" + s + "\x1b[0m" : s;
}

constexpr const char *kBold = "1";
constexpr const char *kDim = "90";
constexpr const char *kRed = "91";
constexpr const char *kGreen = "92";
constexpr const char *kYellow = "93";
constexpr const char *kCyan = "36";
constexpr const char *kBoldCyan = "1;36";

// ---------------------------------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------------------------------

inline std::string FormatClock(double sec) {
    const long long t = sec < 0 ? 0 : (long long)sec;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    return buf;
}

inline std::string FormatSize(double mb) {
    char buf[32];
    if (mb >= 1024.0) std::snprintf(buf, sizeof(buf), "%.2f GB", mb / 1024.0);
    else              std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
    return buf;
}

// ---------------------------------------------------------------------------------------------------
// Column fitting (UTF-8). A "column" is one code point; East Asian wide characters are not
// special-cased - paths and overlay names are the only text that gets cut.
// ---------------------------------------------------------------------------------------------------

inline size_t Width(const std::string &s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// Byte offset of the code point number `cp` (cp == Width(s) gives s.size()).
inline size_t ByteOffset(const std::string &s, size_t cp) {
    size_t seen = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (seen == cp) return i;
            ++seen;
        }
    }
    return s.size();
}

inline const char *Ellipsis(bool fancy) { return fancy ? "\xE2\x80\xA6" : "..."; }
inline size_t EllipsisWidth(bool fancy) { return fancy ? 1 : 3; }

// Cuts the END off a text that is too wide ("A very long na…").
inline std::string Fit(const std::string &s, size_t cols, bool fancy) {
    if (Width(s) <= cols) return s;
    const size_t ew = EllipsisWidth(fancy);
    if (cols <= ew) return std::string(Ellipsis(fancy)).substr(0, cols);
    return s.substr(0, ByteOffset(s, cols - ew)) + Ellipsis(fancy);
}

// Cuts the START off a path that is too wide, because the end of a path is the part that matters
// ("…\Videos\HomRec").
inline std::string FitTail(const std::string &s, size_t cols, bool fancy) {
    const size_t w = Width(s);
    if (w <= cols) return s;
    const size_t ew = EllipsisWidth(fancy);
    if (cols <= ew) return std::string(Ellipsis(fancy)).substr(0, cols);
    return Ellipsis(fancy) + s.substr(ByteOffset(s, w - (cols - ew)));
}

inline std::string PadRight(const std::string &s, size_t cols) {
    const size_t w = Width(s);
    return w >= cols ? s : s + std::string(cols - w, ' ');
}

inline std::string PadLeft(const std::string &s, size_t cols) {
    const size_t w = Width(s);
    return w >= cols ? s : std::string(cols - w, ' ') + s;
}

inline std::string Rule(size_t cols, bool fancy) {
    std::string out;
    for (size_t i = 0; i < cols; ++i) out += fancy ? "\xE2\x94\x80" : "-";
    return fancy ? Paint(true, kDim, out) : out;
}

// ---------------------------------------------------------------------------------------------------
// Languages and texts
// ---------------------------------------------------------------------------------------------------

enum class Lang { En, Ru };

// "ru" (any case, "ru-RU" too) -> Russian, anything else -> English.
inline Lang LangFromCode(const std::string &code) {
    if (code.size() >= 2 && (code[0] == 'r' || code[0] == 'R') && (code[1] == 'u' || code[1] == 'U')) return Lang::Ru;
    return Lang::En;
}

enum class Msg {
    SetupTitle, UseThisConfig, Yes, No, EnterPath, AskConfigPath, CreateConfigQ,
    OutputFolder, KeepCurrent, ChooseOther, AskFolder, FolderCreateFailed,
    VideoFormat, Fps, SetupSaved, SetupSkipped, SetupSaveFailed,
    KeysHint, KeysHintPlain, PressEnter, Cancelled,
    HubTitle, HubStart, HubOverlays, HubSetup, HubExit,
    OverlayTitle, OverlayNone, OverlayAdd, OverlayEdit, OverlayDelete, OverlayToggle, OverlayBack,
    OverlayKeys, OverlayType, OverlayText, OverlayImage, OverlayGif, OverlayWebcam, OverlayInput,
    AskText, AskImage, AskGif, AskJson, AskPng, AskValue, PickWebcam, NoWebcam,
    OverlayAdded, OverlayRemoved, OverlayChanged, OverlaySaveFailed, DeleteQ,
    Overlays, Active, Saved, Recording, Paused, Starting, Saving, Cancel
};

inline const char *Tr(Lang l, Msg m) {
    const bool ru = (l == Lang::Ru);
    switch (m) {
    case Msg::SetupTitle:        return ru ? "HomRec \xE2\x80\x94 \xD0\xBF\xD0\xB5\xD1\x80\xD0\xB2\xD0\xB0\xD1\x8F \xD0\xBD\xD0\xB0\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD0\xBA\xD0\xB0" : "HomRec \xE2\x80\x94 first-time setup";
    case Msg::UseThisConfig:     return ru ? "\xD0\x98\xD1\x81\xD0\xBF\xD0\xBE\xD0\xBB\xD1\x8C\xD0\xB7\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x82\xD1\x8C \xD1\x8D\xD1\x82\xD0\xBE\xD1\x82 \xD0\xBA\xD0\xBE\xD0\xBD\xD1\x84\xD0\xB8\xD0\xB3?" : "Use this config?";
    case Msg::Yes:               return ru ? "\xD0\x94\xD0\xB0" : "Yes";
    case Msg::No:                return ru ? "\xD0\x9D\xD0\xB5\xD1\x82" : "No";
    case Msg::EnterPath:         return ru ? "\xD0\xA3\xD0\xBA\xD0\xB0\xD0\xB7\xD0\xB0\xD1\x82\xD1\x8C \xD0\xBF\xD1\x83\xD1\x82\xD1\x8C" : "Enter a path";
    case Msg::AskConfigPath:     return ru ? "\xD0\x9F\xD1\x83\xD1\x82\xD1\x8C \xD0\xBA \xD0\xBA\xD0\xBE\xD0\xBD\xD1\x84\xD0\xB8\xD0\xB3\xD1\x83 .hrc (\xD0\xBF\xD1\x83\xD1\x81\xD1\x82\xD0\xBE \xE2\x80\x94 \xD0\xBE\xD1\x82\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0): " : ".hrc config path (empty = cancel): ";
    case Msg::CreateConfigQ:     return ru ? "\xD0\xA2\xD0\xB0\xD0\xBA\xD0\xBE\xD0\xB3\xD0\xBE \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xB0 \xD0\xBD\xD0\xB5\xD1\x82. \xD0\xA1\xD0\xBE\xD0\xB7\xD0\xB4\xD0\xB0\xD1\x82\xD1\x8C \xD1\x81 \xD0\xBD\xD0\xB0\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD0\xBA\xD0\xB0\xD0\xBC\xD0\xB8 \xD0\xBF\xD0\xBE \xD1\x83\xD0\xBC\xD0\xBE\xD0\xBB\xD1\x87\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x8E?" : "That file does not exist. Create it with default settings?";
    case Msg::OutputFolder:      return ru ? "\xD0\x9F\xD0\xB0\xD0\xBF\xD0\xBA\xD0\xB0 \xD0\xB4\xD0\xBB\xD1\x8F \xD0\xB7\xD0\xB0\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB5\xD0\xB9" : "Folder for recordings";
    case Msg::KeepCurrent:       return ru ? "\xD0\x9E\xD1\x81\xD1\x82\xD0\xB0\xD0\xB2\xD0\xB8\xD1\x82\xD1\x8C" : "Keep";
    case Msg::ChooseOther:       return ru ? "\xD0\xA3\xD0\xBA\xD0\xB0\xD0\xB7\xD0\xB0\xD1\x82\xD1\x8C \xD0\xB4\xD1\x80\xD1\x83\xD0\xB3\xD1\x83\xD1\x8E" : "Choose another";
    case Msg::AskFolder:         return ru ? "\xD0\x9F\xD1\x83\xD1\x82\xD1\x8C \xD0\xBA \xD0\xBF\xD0\xB0\xD0\xBF\xD0\xBA\xD0\xB5 (\xD0\xBF\xD1\x83\xD1\x81\xD1\x82\xD0\xBE \xE2\x80\x94 \xD0\xBE\xD1\x82\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0): " : "Folder path (empty = cancel): ";
    case Msg::FolderCreateFailed:return ru ? "\xD0\x9D\xD0\xB5 \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xBE\xD1\x81\xD1\x8C \xD1\x81\xD0\xBE\xD0\xB7\xD0\xB4\xD0\xB0\xD1\x82\xD1\x8C \xD0\xBF\xD0\xB0\xD0\xBF\xD0\xBA\xD1\x83" : "Could not create the folder";
    case Msg::VideoFormat:       return ru ? "\xD0\xA4\xD0\xBE\xD1\x80\xD0\xBC\xD0\xB0\xD1\x82 \xD0\xB2\xD0\xB8\xD0\xB4\xD0\xB5\xD0\xBE" : "Video format";
    case Msg::Fps:               return ru ? "\xD0\x9A\xD0\xB0\xD0\xB4\xD1\x80\xD0\xBE\xD0\xB2 \xD0\xB2 \xD1\x81\xD0\xB5\xD0\xBA\xD1\x83\xD0\xBD\xD0\xB4\xD1\x83" : "Frames per second";
    case Msg::SetupSaved:        return ru ? "\xD0\x9D\xD0\xB0\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD0\xBA\xD0\xB8 \xD1\x81\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB5\xD0\xBD\xD1\x8B" : "Settings saved";
    case Msg::SetupSkipped:      return ru ? "\xD0\x9D\xD0\xB0\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD0\xBA\xD0\xB0 \xD0\xBF\xD1\x80\xD0\xBE\xD0\xBF\xD1\x83\xD1\x89\xD0\xB5\xD0\xBD\xD0\xB0. \xD0\x97\xD0\xB0\xD0\xBF\xD1\x83\xD1\x81\xD1\x82\xD0\xB8\xD1\x82\xD0\xB5 'homrec setup' \xD0\xBF\xD0\xBE\xD0\xB7\xD0\xB6\xD0\xB5." : "Setup skipped. Run 'homrec setup' any time.";
    case Msg::SetupSaveFailed:   return ru ? "\xD0\x9D\xD0\xB5 \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xBE\xD1\x81\xD1\x8C \xD1\x81\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x82\xD1\x8C \xD0\xBD\xD0\xB0\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD0\xBA\xD0\xB8" : "Could not save the settings";
    case Msg::KeysHint:          return ru ? "\xE2\x86\x91/\xE2\x86\x93 \xD0\xB2\xD1\x8B\xD0\xB1\xD0\xBE\xD1\x80  \xC2\xB7  Enter \xD0\xBF\xD0\xBE\xD0\xB4\xD1\x82\xD0\xB2\xD0\xB5\xD1\x80\xD0\xB4\xD0\xB8\xD1\x82\xD1\x8C  \xC2\xB7  Esc \xD0\xBE\xD1\x82\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0" : "\xE2\x86\x91/\xE2\x86\x93 choose  \xC2\xB7  Enter confirm  \xC2\xB7  Esc cancel";
    case Msg::KeysHintPlain:     return "Up/Down - choose, Enter - confirm, Esc - cancel";   // plain ASCII for consoles without ANSI
    case Msg::PressEnter:        return ru ? "Enter \xE2\x80\x94 \xD0\xBF\xD1\x80\xD0\xBE\xD0\xB4\xD0\xBE\xD0\xBB\xD0\xB6\xD0\xB8\xD1\x82\xD1\x8C" : "Press Enter to continue";
    case Msg::Cancelled:         return ru ? "\xD0\x9E\xD1\x82\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xBE" : "Cancelled";
    case Msg::HubTitle:          return "HomRec";
    case Msg::HubStart:          return ru ? "\xD0\x9D\xD0\xB0\xD1\x87\xD0\xB0\xD1\x82\xD1\x8C \xD0\xB7\xD0\xB0\xD0\xBF\xD0\xB8\xD1\x81\xD1\x8C" : "Start recording";
    case Msg::HubOverlays:       return ru ? "\xD0\x9E\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBB\xD0\xB5\xD0\xB8" : "Overlays";
    case Msg::HubSetup:          return ru ? "\xD0\x9D\xD0\xB0\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD0\xBA\xD0\xB0" : "Setup";
    case Msg::HubExit:           return ru ? "\xD0\x92\xD1\x8B\xD1\x85\xD0\xBE\xD0\xB4" : "Exit";
    case Msg::OverlayTitle:      return ru ? "\xD0\x9E\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBB\xD0\xB5\xD0\xB8" : "Overlays";
    case Msg::OverlayNone:       return ru ? "(\xD0\xBF\xD1\x83\xD1\x81\xD1\x82\xD0\xBE \xE2\x80\x94 \xD0\xB4\xD0\xBE\xD0\xB1\xD0\xB0\xD0\xB2\xD1\x8C\xD1\x82\xD0\xB5 \xD0\xBF\xD0\xB5\xD1\x80\xD0\xB2\xD1\x8B\xD0\xB9 \xD0\xBA\xD0\xBB\xD0\xB0\xD0\xB2\xD0\xB8\xD1\x88\xD0\xB5\xD0\xB9 A)" : "(empty - press A to add one)";
    case Msg::OverlayAdd:        return ru ? "\xD0\x94\xD0\xBE\xD0\xB1\xD0\xB0\xD0\xB2\xD0\xB8\xD1\x82\xD1\x8C" : "Add";
    case Msg::OverlayEdit:       return ru ? "\xD0\x98\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x82\xD1\x8C" : "Edit";
    case Msg::OverlayDelete:     return ru ? "\xD0\xA3\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xB8\xD1\x82\xD1\x8C" : "Delete";
    case Msg::OverlayToggle:     return ru ? "\xD0\x9F\xD0\xBE\xD0\xBA\xD0\xB0\xD0\xB7\xD0\xB0\xD1\x82\xD1\x8C/\xD1\x81\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82\xD1\x8C" : "Show/hide";
    case Msg::OverlayBack:       return ru ? "\xD0\x9D\xD0\xB0\xD0\xB7\xD0\xB0\xD0\xB4" : "Back";
    case Msg::OverlayKeys:       return ru ? "\xE2\x86\x91/\xE2\x86\x93 \xD0\xB2\xD1\x8B\xD0\xB1\xD0\xBE\xD1\x80  Enter \xD0\xB8\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x82\xD1\x8C  A \xD0\xB4\xD0\xBE\xD0\xB1\xD0\xB0\xD0\xB2\xD0\xB8\xD1\x82\xD1\x8C  D \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xB8\xD1\x82\xD1\x8C  V \xD0\xBF\xD0\xBE\xD0\xBA\xD0\xB0\xD0\xB7\xD0\xB0\xD1\x82\xD1\x8C/\xD1\x81\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82\xD1\x8C  Esc \xD0\xBD\xD0\xB0\xD0\xB7\xD0\xB0\xD0\xB4" : "\xE2\x86\x91/\xE2\x86\x93 choose  Enter edit  A add  D delete  V show/hide  Esc back";
    case Msg::OverlayType:       return ru ? "\xD0\xA2\xD0\xB8\xD0\xBF \xD0\xBE\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBB\xD0\xB5\xD1\x8F" : "Overlay type";
    case Msg::OverlayText:       return ru ? "\xD0\xA2\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82" : "Text";
    case Msg::OverlayImage:      return ru ? "\xD0\x9A\xD0\xB0\xD1\x80\xD1\x82\xD0\xB8\xD0\xBD\xD0\xBA\xD0\xB0" : "Image";
    case Msg::OverlayGif:        return "GIF";
    case Msg::OverlayWebcam:     return ru ? "\xD0\x92\xD0\xB5\xD0\xB1-\xD0\xBA\xD0\xB0\xD0\xBC\xD0\xB5\xD1\x80\xD0\xB0" : "Webcam";
    case Msg::OverlayInput:      return ru ? "\xD0\x9A\xD0\xBB\xD0\xB0\xD0\xB2\xD0\xB8\xD0\xB0\xD1\x82\xD1\x83\xD1\x80\xD0\xB0/\xD0\xBC\xD1\x8B\xD1\x88\xD1\x8C (input overlay)" : "Keyboard/mouse (input overlay)";
    case Msg::AskText:           return ru ? "\xD0\xA2\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82 \xD0\xBE\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBB\xD0\xB5\xD1\x8F (\xD0\xBF\xD1\x83\xD1\x81\xD1\x82\xD0\xBE \xE2\x80\x94 \xD0\xBE\xD1\x82\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0): " : "Overlay text (empty = cancel): ";
    case Msg::AskImage:          return ru ? "\xD0\x9F\xD1\x83\xD1\x82\xD1\x8C \xD0\xBA \xD0\xBA\xD0\xB0\xD1\x80\xD1\x82\xD0\xB8\xD0\xBD\xD0\xBA\xD0\xB5 (\xD0\xBF\xD1\x83\xD1\x81\xD1\x82\xD0\xBE \xE2\x80\x94 \xD0\xBE\xD1\x82\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0): " : "Image path (empty = cancel): ";
    case Msg::AskGif:            return ru ? "\xD0\x9F\xD1\x83\xD1\x82\xD1\x8C \xD0\xBA .gif (\xD0\xBF\xD1\x83\xD1\x81\xD1\x82\xD0\xBE \xE2\x80\x94 \xD0\xBE\xD1\x82\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0): " : "Path of the .gif (empty = cancel): ";
    case Msg::AskJson:           return ru ? "\xD0\x9F\xD1\x83\xD1\x82\xD1\x8C \xD0\xBA .json \xD1\x80\xD0\xB0\xD1\x81\xD0\xBA\xD0\xBB\xD0\xB0\xD0\xB4\xD0\xBA\xD0\xB5 (\xD0\xBF\xD1\x83\xD1\x81\xD1\x82\xD0\xBE \xE2\x80\x94 \xD0\xBE\xD1\x82\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0): " : "Path of the .json layout (empty = cancel): ";
    case Msg::AskPng:            return ru ? "\xD0\x9F\xD1\x83\xD1\x82\xD1\x8C \xD0\xBA .png \xD1\x81\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB9\xD1\x82\xD0\xB0\xD0\xBC (\xD0\xBF\xD1\x83\xD1\x81\xD1\x82\xD0\xBE \xE2\x80\x94 \xD0\xBE\xD1\x82\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0): " : "Path of the .png spritesheet (empty = cancel): ";
    case Msg::AskValue:          return ru ? "\xD0\x9D\xD0\xBE\xD0\xB2\xD0\xBE\xD0\xB5 \xD0\xB7\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5 (\xD0\xBF\xD1\x83\xD1\x81\xD1\x82\xD0\xBE \xE2\x80\x94 \xD0\xBE\xD1\x82\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0): " : "New value (empty = cancel): ";
    case Msg::PickWebcam:        return ru ? "\xD0\x92\xD0\xB5\xD0\xB1-\xD0\xBA\xD0\xB0\xD0\xBC\xD0\xB5\xD1\x80\xD0\xB0" : "Webcam";
    case Msg::NoWebcam:          return ru ? "\xD0\x92\xD0\xB5\xD0\xB1-\xD0\xBA\xD0\xB0\xD0\xBC\xD0\xB5\xD1\x80\xD0\xB0 \xD0\xBD\xD0\xB5 \xD0\xBD\xD0\xB0\xD0\xB9\xD0\xB4\xD0\xB5\xD0\xBD\xD0\xB0" : "No webcam found";
    case Msg::OverlayAdded:      return ru ? "\xD0\x9E\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBB\xD0\xB5\xD0\xB9 \xD0\xB4\xD0\xBE\xD0\xB1\xD0\xB0\xD0\xB2\xD0\xBB\xD0\xB5\xD0\xBD" : "Overlay added";
    case Msg::OverlayRemoved:    return ru ? "\xD0\x9E\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBB\xD0\xB5\xD0\xB9 \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD1\x91\xD0\xBD" : "Overlay removed";
    case Msg::OverlayChanged:    return ru ? "\xD0\x98\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xBE" : "Changed";
    case Msg::OverlaySaveFailed: return ru ? "\xD0\x9D\xD0\xB5 \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xBE\xD1\x81\xD1\x8C \xD1\x81\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x82\xD1\x8C \xD0\xBE\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBB\xD0\xB5\xD0\xB8" : "Could not save the overlays";
    case Msg::DeleteQ:           return ru ? "\xD0\xA3\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xB8\xD1\x82\xD1\x8C \xD1\x8D\xD1\x82\xD0\xBE\xD1\x82 \xD0\xBE\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBB\xD0\xB5\xD0\xB9?" : "Delete this overlay?";
    case Msg::Overlays:          return ru ? "\xD0\xBE\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBB\xD0\xB5\xD0\xB8" : "overlays";
    case Msg::Active:            return ru ? "\xD0\xB0\xD0\xBA\xD1\x82\xD0\xB8\xD0\xB2\xD0\xBD\xD1\x8B" : "active";
    case Msg::Saved:             return ru ? "\xD0\xA1\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xBE" : "Saved";
    case Msg::Recording:         return "REC";
    case Msg::Paused:            return ru ? "\xD0\x9F\xD0\x90\xD0\xA3\xD0\x97\xD0\x90" : "PAUSED";
    case Msg::Starting:          return ru ? "\xD0\xA1\xD1\x82\xD0\xB0\xD1\x80\xD1\x82 \xD1\x87\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB7" : "Starting in";
    case Msg::Saving:            return ru ? "\xD0\xA1\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5" : "Saving";
    case Msg::Cancel:            return ru ? "\xD0\x9E\xD1\x82\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0" : "Cancel";
    }
    return "";
}

// ---------------------------------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------------------------------

// One menu row: "  > item" with the selected row highlighted, "    item" otherwise.
inline std::string MenuItemLine(const std::string &text, bool selected, bool fancy) {
    if (selected) return Paint(fancy, kBoldCyan, std::string("  ") + (fancy ? "\xE2\x9D\xAF" : ">") + " " + text);
    return "    " + text;
}

// ---------------------------------------------------------------------------------------------------
// Recording screen
// ---------------------------------------------------------------------------------------------------

constexpr int kBarWidth = 28;
constexpr int kWaveWidth = 7;

// "[   block   ]" bouncing left -> right -> left. While a recording is finalized the engine reports
// no progress, so this is a "working" indicator, not a percentage.
inline std::string SavingBar(int tick, bool fancy) {
    const int span = kBarWidth - kWaveWidth;   // positions the block can take
    const int period = 2 * span;
    const int t = tick % period;
    const int pos = t <= span ? t : period - t;

    std::string out = "[";
    if (fancy) {
        out += "\x1b[90m";
        for (int i = 0; i < pos; ++i) out += "\xE2\x96\x91";
        out += "\x1b[36m";
        for (int i = 0; i < kWaveWidth; ++i) out += "\xE2\x96\x88";
        out += "\x1b[90m";
        for (int i = pos + kWaveWidth; i < kBarWidth; ++i) out += "\xE2\x96\x91";
        out += "\x1b[0m";
    } else {
        for (int i = 0; i < kBarWidth; ++i) out += (i >= pos && i < pos + kWaveWidth) ? '#' : '.';
    }
    out += ']';
    return out;
}

inline std::string DoneBar(bool fancy) {
    std::string out = "[";
    if (fancy) {
        out += "\x1b[92m";
        for (int i = 0; i < kBarWidth; ++i) out += "\xE2\x96\x88";
        out += "\x1b[0m";
    } else {
        out.append((size_t)kBarWidth, '#');
    }
    out += ']';
    return out;
}

// "* REC  00:01:23 | 42.5 MB | 60 fps" - kept well under 80 columns. The text only changes when
// a shown value changes, so the caller can skip the redraw when it is identical to the last one.
inline std::string RecLine(bool fancy, Lang lang, bool paused, double sec, double mb, double fps, bool dropping) {
    const std::string sep = Paint(fancy, kDim, fancy ? " \xE2\x94\x82 " : " | ");
    std::string s = paused ? Paint(fancy, kYellow, std::string(fancy ? "\xE2\x8F\xB8" : "||") + " " + Tr(lang, Msg::Paused))
                           : Paint(fancy, kRed, fancy ? "\xE2\x97\x8F" : "*") + " " + Tr(lang, Msg::Recording);
    s += "  " + FormatClock(sec) + sep + FormatSize(mb);
    if (!paused && fps >= 1.0) {
        char b[24];
        std::snprintf(b, sizeof(b), "%.0f fps", fps);
        s += sep + b;
    }
    if (dropping) s += sep + Paint(fancy, kYellow, "! dropping frames");
    return s;
}

// "Saving  [bar] (3.2s)" - the timer turns yellow after 2 s and red after 20 s, like the release builder.
inline std::string SavingLine(bool fancy, Lang lang, int tick, double sec, bool ctrlc_pressed) {
    std::string s = std::string(Tr(lang, Msg::Saving)) + "  " + SavingBar(tick, fancy);
    if (sec >= 2.0) {
        char b[24];
        std::snprintf(b, sizeof(b), " (%.1fs)", sec);
        s += Paint(fancy, sec >= 20.0 ? "31" : "33", b);
    }
    if (ctrlc_pressed) s += "  " + Paint(fancy, kDim, "(Ctrl+C: please wait)");
    return s;
}

// A "label   value" row of the start-up panel; the value is cut at the start (paths) to fit `cols`.
inline std::string InfoRow(const std::string &label, const std::string &value, size_t cols, bool fancy) {
    const size_t lw = 9;   // widest label + 2 spaces
    const size_t room = cols > lw + 2 ? cols - lw - 2 : 8;
    return "  " + Paint(fancy, kDim, PadRight(label, lw)) + FitTail(value, room, fancy);
}

} // namespace tui
} // namespace hrcli
