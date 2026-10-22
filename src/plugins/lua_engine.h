// lua_engine.h - Lua plugin engine (Lua 5.4, linked with -llua).
// Plugins get unrestricted io/os access plus homrec.http_get/http_post.
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

struct lua_State; // fwd-declare, real definition comes from lua.h in the .cpp
struct PluginPermissions {
    bool network    = true;  // homrec.http_get()/http_post()
    bool filesystem = true;
    bool store      = true;  // homrec.store_get()/store_set()
};

struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string author;    // optional; shown in the plugin list UI as "by <author>"
    std::string entry = "main.lua";
    PluginPermissions permissions;
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

    // Loads a single .hrp archive: extracts it (if not already extracted, or if the .hrp is
    // newer than what's currently extracted) to plugins_dir/.installed/<archive-name>/, then
    // calls LoadPlugin() on that directory.
    bool LoadPluginArchive(const std::string &hrp_path);

    void UnloadPlugin(const std::string &id);
    void UnloadAll();

    // Hook dispatch - calls the named global Lua function in every loaded
    // plugin's state, if defined, matching emit_hook()'s "call this method
    // on every plugin that has it" semantics.
    void EmitHook(const char *hook_name);
    void EmitHookWithColors(const char *hook_name, const ThemeColors &colors);

    // emit()/on_custom_event(): a plugin-defined event broadcast to every OTHER loaded
    // plugin's `on_custom_event(event, ...)` function, if present.
    void EmitCustomEvent(const std::string &from_plugin_id, const std::string &event, const std::string &arg);

    // --- Plugin-registered console commands ----------------------------- A plugin calls
    // homrec.register_command(name, description, fn) from on_load(); this is the C++ side of
    // that (see lua_api.cpp's L_register_command).
    struct RegisteredCommand {
        std::string plugin_id;
        std::string description;
        int lua_ref = -1; // LUA_REGISTRYINDEX ref in that plugin's own lua_State
    };
    void RegisterCommand(const std::string &plugin_id, const std::string &name,
                          const std::string &description, int lua_ref);

    // Looked up case-insensitively.
    bool DispatchCommand(const std::string &name, const std::string &raw_line,
                         std::vector<std::string> &out_lines);

    std::vector<std::string> *print_sink = nullptr;

    // All registered commands, for a `help`/`commands` listing -
    // name -> (owning plugin id, description).
    const std::unordered_map<std::string, RegisteredCommand> &commands() const { return commands_; }

    // --- Plugin-registered settings --------------------------------------- A plugin calls
    // homrec.register_setting(name, description, get_fn, set_fn) from on_load(); this is the
    // C++ side of that (see lua_api.cpp's L_register_setting).
    struct RegisteredSetting {
        std::string plugin_id;
        std::string description;
        int get_ref = -1; // lua_State ref: function() -> value
        int set_ref = -1; // lua_State ref: function(raw_value_string) -> ok?
    };
    void RegisterSetting(const std::string &plugin_id, const std::string &name,
                          const std::string &description, int get_ref, int set_ref);

    // Looked up case-insensitively, same "false means fall through, not failure" contract as
    // DispatchCommand() above.
    bool DispatchSetting(const std::string &name, const std::string &raw_value,
                         std::vector<std::string> &out_lines);

    const std::unordered_map<std::string, RegisteredSetting> &plugin_settings() const { return settings_; }

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
    std::unordered_map<std::string, RegisteredCommand> commands_; // key: lowercased command name
    std::unordered_map<std::string, RegisteredSetting> settings_; // key: lowercased setting name

    RecordingController *rec_ = nullptr;
    const ThemeColors *colors_ = nullptr;
};

// Per-plugin persistent key/value store, replacing
// engine._plugin_store_set/get. One flat file per plugin:
namespace PluginStore {
    void Set(const std::string &plugin_dir, const std::string &key, const std::string &value);
    std::string Get(const std::string &plugin_dir, const std::string &key, const std::string &default_value);
}
