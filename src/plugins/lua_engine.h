// lua_engine.h
//
// A Lua-scripted plugin engine: plugins get full filesystem and network
// access (`luaL_openlibs` is used unmodified - `io`/`os` are NOT
// sandboxed/restricted - plus a `homrec.http_get`/`http_post` pair for
// network access Lua's stdlib doesn't provide on its own).
//
// DEPENDENCY: this links against the standard Lua 5.4 C API
// (lua.h/lauxlib.h/lualib.h, -llua), which isn't vendored in this repo.
// Install it via `vcpkg install lua`, or drop the Lua 5.4 amalgamation
// into this project yourself (see the Makefile's LUA_CFLAGS/LUA_LDFLAGS
// comment for how to point the build at it).
//
// ARCHIVE SUPPORT: plugins can also ship as a single .hrp file (a .zip
// renamed) instead of a bare directory. LoadAll() picks up both shapes
// from plugins_dir:
//   plugins/<id>/plugin.json + <entry>.lua      (directory, as before)
//   plugins/<name>.hrp                            (archive)
// .hrp archives are extracted (via hr_archive.h -- see that header for why
// no zip library needed to be vendored to get this working) to
// plugins/.installed/<name>/ once, then loaded the same way a directory
// always was. Re-extraction on every LoadAll() would stomp any per-plugin
// .store file PluginStore writes alongside plugin.json, so an archive is
// only (re-)extracted when its .hrp is newer than the existing extracted
// copy - see LoadPluginArchive().
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

    // Loads a single .hrp archive: extracts it (if not already extracted,
    // or if the .hrp is newer than what's currently extracted) to
    // plugins_dir/.installed/<archive-name>/, then calls LoadPlugin() on
    // that directory. Returns false if extraction or loading failed.
    bool LoadPluginArchive(const std::string &hrp_path);

    void UnloadPlugin(const std::string &id);
    void UnloadAll();

    // Hook dispatch - calls the named global Lua function in every loaded
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
