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
    std::string author;    // optional; shown in the plugin list UI as "by <author>"
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

    // --- Plugin-registered console commands -----------------------------
    // A plugin calls homrec.register_command(name, description, fn) from
    // on_load(); this is the C++ side of that (see lua_api.cpp's
    // L_register_command). `fn` is stashed as a Lua registry reference in
    // the OWNING plugin's own lua_State - DispatchCommand() below looks
    // that state back up by plugin id when the command actually runs, so
    // this doesn't need to know anything Lua-specific itself.
    struct RegisteredCommand {
        std::string plugin_id;
        std::string description;
        int lua_ref = -1; // LUA_REGISTRYINDEX ref in that plugin's own lua_State
    };
    void RegisterCommand(const std::string &plugin_id, const std::string &name,
                          const std::string &description, int lua_ref);

    // Looked up case-insensitively. Returns true if some plugin had this
    // command registered (and ran it) - false means "not a plugin command,
    // fall through to your own unknown-command handling", NOT "the plugin
    // command failed" (a Lua-side error still counts as "handled", with
    // the error text delivered as an output line, same as e.g. a bad
    // argument would be - the caller just wanted a console command that
    // did *something*, not a Lua stack trace dumped over their prompt).
    bool DispatchCommand(const std::string &name, const std::string &raw_line,
                         std::vector<std::string> &out_lines);

    // homrec.print()'s destination while a command set up via
    // DispatchCommand() is running; nullptr the rest of the time (calling
    // homrec.print() from on_load()/a hook instead of a command handler is
    // simply a no-op, not an error, since there's no console output
    // stream running that isn't a scrollback the plugin can't reach).
    std::vector<std::string> *print_sink = nullptr;

    // All registered commands, for a `help`/`commands` listing -
    // name -> (owning plugin id, description).
    const std::unordered_map<std::string, RegisteredCommand> &commands() const { return commands_; }

    // --- Plugin-registered settings ---------------------------------------
    // A plugin calls homrec.register_setting(name, description, get_fn,
    // set_fn) from on_load(); this is the C++ side of that (see
    // lua_api.cpp's L_register_setting). Mirrors RegisteredCommand above -
    // get_fn/set_fn are Lua registry refs in the OWNING plugin's own
    // lua_State - so a plugin-defined setting gets exactly the same
    // "<name> = <value>" console/cfg-file syntax as a built-in .hrc
    // setting does (see console_window.cpp's ConsoleWindow::RunCommand),
    // without HomRec's console needing to know anything about how that
    // plugin actually stores the value (typically homrec.store_get/
    // store_set - see lua_api.cpp - but a plugin could just as easily back
    // it with in-memory Lua state instead).
    struct RegisteredSetting {
        std::string plugin_id;
        std::string description;
        int get_ref = -1; // lua_State ref: function() -> value
        int set_ref = -1; // lua_State ref: function(raw_value_string) -> ok?
    };
    void RegisterSetting(const std::string &plugin_id, const std::string &name,
                          const std::string &description, int get_ref, int set_ref);

    // Looked up case-insensitively, same "false means fall through, not
    // failure" contract as DispatchCommand() above. `raw_value` empty means
    // "print current value" (calls get_fn); non-empty means "assign"
    // (calls set_fn(raw_value)). Either way, anything the plugin's get_fn/
    // set_fn passes to homrec.print() is collected into out_lines - if
    // set_fn didn't print anything itself, a default "<name> = <value>"
    // confirmation line is appended so a plugin setting behaves the same
    // as a built-in one from the user's side even if the plugin author
    // didn't bother adding their own feedback.
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
// plugins/<id>/.store (line-oriented "key\ttype\tvalue"). Kept intentionally
// simple (string/number/bool only) rather than pulling in a JSON dependency
// for what's usually small plugin config values.
namespace PluginStore {
    void Set(const std::string &plugin_dir, const std::string &key, const std::string &value);
    std::string Get(const std::string &plugin_dir, const std::string &key, const std::string &default_value);
}
