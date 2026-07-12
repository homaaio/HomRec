// lua_engine.h — Phase 9
//
// Replaces homrec_app/hr_plugin_engine.py's PluginEngine + HomRecPluginBase
// with a Lua-scripted equivalent, per your instructions: full filesystem
// and network access for plugins (so `luaL_openlibs` is used unmodified —
// `io`/`os` are NOT sandboxed/restricted — plus a `homrec.http_get`/
// `http_post` pair for network access Lua's stdlib doesn't provide on its
// own).
//
// DEPENDENCY YOU NEED TO SUPPLY: this links against the standard Lua 5.4 C
// API (lua.h/lauxlib.h/lualib.h, -llua). I have no network access in my
// sandbox to fetch/vendor Lua's source, so you'll need to either
// `vcpkg install lua` or drop the Lua 5.4 amalgamation into this project
// yourself — see README_PHASE9.md for exact steps.
//
// SCOPE NOTE vs. the Python engine: the Python version loads plugins from
// .hrp/.jar/.zip archives (via `zipfile`) extracted to a temp folder. I
// have not vendored a zip-reading library here (miniz or similar), so this
// port loads plugins from plain directories instead:
//   plugins/<id>/plugin.json
//   plugins/<id>/<entry>.lua
// Archive support (.hrp) is a clean follow-on addition once a zip library
// is added — flagging it now rather than writing an untested custom zip
// parser from scratch.
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

struct lua_State; // fwd-declare, real definition comes from lua.h in the .cpp

struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string entry = "main.lua";
};

struct LoadedPlugin {
    PluginManifest manifest;
    lua_State *L = nullptr;
    bool loaded_ok = false;
    void *api_handle = nullptr; // opaque handle from LuaApi::Install, freed via LuaApi::Uninstall on unload
};

class RecordingController;
struct ThemeColors;

class LuaPluginEngine {
public:
    explicit LuaPluginEngine(const std::string &plugins_dir);
    ~LuaPluginEngine();

    // Scans plugins_dir for subdirectories containing plugin.json and loads
    // each one. Mirrors load_all().
    void LoadAll();

    // Loads a single plugin directory. Returns true on success (manifest
    // parsed, entry script ran without error, on_load() succeeded).
    bool LoadPlugin(const std::string &plugin_dir_path);

    void UnloadPlugin(const std::string &id);
    void UnloadAll();

    // Hook dispatch — calls the named global Lua function in every loaded
    // plugin's state, if defined, matching emit_hook()'s "call this method
    // on every plugin that has it" semantics.
    void EmitHook(const char *hook_name);
    void EmitHookWithColors(const char *hook_name, const ThemeColors &colors);

    // emit()/on_custom_event(): a plugin-defined event broadcast to every
    // OTHER loaded plugin's `on_custom_event(event, ...)` function, if
    // present. `arg` is passed through as a single Lua string for now
    // (matching the common case; richer payloads can go through
    // store_set/store_get instead).
    void EmitCustomEvent(const std::string &from_plugin_id, const std::string &event, const std::string &arg);

    const std::vector<std::string> &loaded_ids() const { return loaded_ids_; }
    const PluginManifest *GetManifest(const std::string &id) const;

    // Wired by main_window at startup so homrec.get_ffmpeg()/get_colors()/
    // show_toast() have something real to read from.
    void SetContext(RecordingController *rec, const ThemeColors *colors) {
        rec_ = rec; colors_ = colors;
    }

    RecordingController *recording_controller() const { return rec_; }
    const ThemeColors *colors() const { return colors_; }

private:
    std::string plugins_dir_;
    std::unordered_map<std::string, std::unique_ptr<LoadedPlugin>> plugins_;
    std::vector<std::string> loaded_ids_;

    RecordingController *rec_ = nullptr;
    const ThemeColors *colors_ = nullptr;
};

// Per-plugin persistent key/value store, replacing
// engine._plugin_store_set/get. One flat file per plugin:
// plugins/<id>/.store (line-oriented "key\ttype\tvalue"). Kept intentionally
// simple (string/number/bool only) rather than pulling in a JSON dependency
// for what's usually small plugin config values.
namespace PluginStore {
    void Set(const std::string &plugin_dir, const std::string &key, const std::string &value);
    std::string Get(const std::string &plugin_dir, const std::string &key, const std::string &default_value);
}
