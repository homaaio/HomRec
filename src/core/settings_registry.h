// settings_registry.h - one table of every AppState setting: key, .hrc section and access rules.
// Shared by .hrc files, the console "<key> = <value>" syntax, cfg scripts and the Lua settings API.
#pragma once

#include "../ui/app_state.h"
#include <string>
#include <vector>
#include <functional>

namespace HrSettingsRegistry {

// What a setting's text form actually represents - lets a caller that deals in native
// types (homrec.get_setting()'s Lua bridge, see lua_api.cpp) push the right one back
// without re-guessing it from the key's spelling.
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
    std::function<bool(AppState &, const std::string &)> set;
};

// Declared in definition order == .hrc [section] emission order, so
// HrcConfig::Save() walking this list reproduces the file's historical
// section grouping without needing its own separate ordering.
const std::vector<SettingDef> &All();

// Case-insensitive (ASCII) lookup by canonical key OR any of its aliases.
const SettingDef *Find(const std::string &key);

} // namespace HrSettingsRegistry
