// hr_settings_registry.h
//
// The single source of truth for every scalar AppState field that .hrc,
// the developer console's "<key> = <value>" line syntax, cfg scripts
// (autoexec/config/startrec.cfg - same parser, see console_window.cpp),
// and homrec.get_setting()/set_setting() all need to read or write by
// name. One place declares, for each field: its canonical key, which
// .hrc [section] it's grouped under, whether it's gated behind the
// console's "sec" fuse (only custom_ffmpeg_args today - see
// hrc_config.h's Load() comment for why), and how to convert it to/from
// the plain-text form every one of those callers already uses.
//
// Before this existed, hrc_config.cpp's Save()/Load() had one hand-written
// key list and lua_api.cpp's BoolSettingByName() had a second, SEPARATE
// hand-written key list for a subset of the same fields - using different
// spellings for several of them (minimize_tray vs. the .hrc file's
// minimize_to_tray, countdown vs. countdown_enabled, timestamp vs.
// timestamp_enabled, cursor vs. cursor_enabled). That's exactly the kind
// of drift this table exists to make structurally impossible: hrc_config.cpp
// and lua_api.cpp now both walk All() instead of keeping their own copies.
// The old short Lua-facing names are preserved as aliases (see
// SettingDef::aliases) purely for backward compatibility with existing
// plugin scripts - new code (built-in or plugin) should use the canonical
// key.
//
// Deliberately excludes:
//   - AppState::overlays (a vector, not a scalar - see hrc_config.cpp's
//     WriteOverlaysSection()/ReadOverlaysSection(), still hand-rolled).
//   - Runtime-only / non-persisted fields (recording, paused, frame_count,
//     start_time, stop_flag, hidden_capture_windows, ui_registry, and the
//     resolved-at-startup ffmpeg_path).
#pragma once

#include "ui/app_state.h"
#include <string>
#include <vector>
#include <functional>

namespace HrSettingsRegistry {

// What a setting's text form actually represents - lets a caller that
// deals in native types (homrec.get_setting()'s Lua bridge, see
// lua_api.cpp) push the right one back without re-guessing it from the
// key's spelling. .hrc/the console only ever see text either way, so this
// doesn't affect them.
enum class Kind { Bool, Int, Double, String, Enum };

struct SettingDef {
    std::string key;                    // canonical name, e.g. "disable_preview"
    std::string section;                // .hrc [section] this is grouped under (cosmetic)
    std::vector<std::string> aliases;   // old/alternate names accepted on lookup (Find() only)
    Kind kind = Kind::String;
    bool sensitive = false;             // gated behind the console's "sec" fuse

    // Every field round-trips through plain text - same contract the
    // original .hrc format already used (atoi/atof/ToBool going in,
    // std::to_string/FromBool coming out), just centralized here.
    std::function<std::string(const AppState &)> get;
    // Returns false if `value` isn't valid for this field's type (e.g.
    // non-numeric text for an int field) - callers treat that as "line
    // skipped", not a hard error, matching .hrc's existing tolerance for
    // a hand-edited file with a typo in it.
    std::function<bool(AppState &, const std::string &)> set;
};

// Declared in definition order == .hrc [section] emission order, so
// HrcConfig::Save() walking this list reproduces the file's historical
// section grouping without needing its own separate ordering.
const std::vector<SettingDef> &All();

// Case-insensitive (ASCII) lookup by canonical key OR any of its aliases.
// Returns nullptr if `key` isn't a known built-in setting - the caller
// (console_window.cpp) falls through to plugin-registered settings next,
// and then to "Unknown command" if neither matches.
const SettingDef *Find(const std::string &key);

} // namespace HrSettingsRegistry
